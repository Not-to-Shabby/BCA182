#!/usr/bin/env python3
"""
extract_features.py - P3 cry/non-cry audio front-end (pure NumPy, no
librosa / scipy / ffmpeg at feature time).

This module is the documented PC mirror of the embedded CMSIS-DSP front-end
planned in docs/project-plan.md P3 (arm_rfft_fast_f32 -> 40 mel -> log ->
13 MFCCs -> per-window aggregation). It is hand-rolled from NumPy primitives
so that every constant and formula can later be replicated exactly in C and
verified with a PC<->C parity unit test.

Pipeline (all stages deterministic, fixed constants, no RNG):
  1. load WAV        stdlib `wave` + numpy frombuffer -> float32 in [-1, 1],
                     multi-channel mixed down by averaging. Non-WAV containers
                     (.3gp/.caf/.m4a/...) raise UnsupportedAudio - run
                     dataset_prep.py (ffmpeg) first.
  2. resample        windowed-sinc (Blackman) bandlimited resampler to
                     16 kHz mono, hand-written, documented in
                     resample_to_16k(). No np.interp.
  3. frame           512-sample frames, hop 256, periodic Hann window
                     w[k] = 0.5 - 0.5*cos(2*pi*k/512)
                     (generate the CMSIS arm_mfcc window table with the same
                     formula for parity).
  4. spectrum        rfft -> power = (re^2 + im^2) / 512
  5. mel filterbank  40 triangular filters, 0-8000 Hz, HTK mel scale
                     mel(f) = 2595*log10(1 + f/700), (N_MELS+2) edge points
                     linearly spaced in mel; hand-built matrix MEL_FB
                     (40 x 257), documented in build_mel_filterbank().
  6. log             natural log with 1e-10 floor: log(mel + 1e-10)
  7. DCT-II          orthonormal DCT-II (equal to scipy dct norm='ortho'),
                     hand-built matrix DCT_MAT (13 x 40) -> 13 MFCCs
  8. aggregate       per 1 s (16000-sample, non-overlapping) window over its
                     61 frames.  Feature-set v2 (63 features) = the original
                     29 per-window summary statistics (indices 0-28, meaning
                     unchanged) PLUS temporal-dynamics features appended
                     after them:

    0-28   v1 summary (unchanged)
           0-12  mean of each MFCC over the 61 frames
           13-25 population std (ddof=0) of each MFCC
           26    mean frame log-energy
           27    std of frame log-energy
           28    mean frame ZCR (sign changes / 511 pairs)
    29-55  MFCC DELTAS (temporal dynamics of the spectral envelope):
           per-frame first difference of the MFCC frame sequence with EDGE
           REPLICATION, d[t] = (m'[t+1] - m'[t-1]) / 2 where m' pads the
           61-frame MFCC matrix by replicating the first/last frame
           (interior t: (m[t+1]-m[t-1])/2; d[0] = (m[1]-m[0])/2;
           d[60] = (m[60]-m[59])/2).
           29-41 mean of each MFCC delta over the 61 frames
           42-54 population std of each MFCC delta
    55-56  LOG-ENERGY DELTA: same central-difference-with-edge-replication
           operator applied to the per-frame log-energy sequence
           (55 = mean, 56 = population std) - captures loudness dynamics
           (cry rhythm) that the static mean/std summaries average away.
    57-62  SPECTRAL SHAPE dynamics, per frame on the raw 257-bin rfft power
           spectrum (bin k = k * BIN_HZ Hz, BIN_HZ = 16000/512 = 31.25):
           - centroid (Hz): sum_k f_k * P[k] / sum_k P[k]; defined 0 when
             the frame power sums to 0 (digital silence).
           - rolloff (Hz): ROLLOFF_FRAC = 0.85 cumulative-power point, i.e.
             the frequency f_k* of the first bin whose running sum reaches
             0.85 * total power (0 for a zero-power frame).
           - flatness: geometric/arithmetic mean of the power spectrum,
             exp(mean_k log P[k]) / mean_k P[k] (0 for a zero-power frame;
             a single 0-power bin makes the geometric mean 0, documented).
           57/58 centroid mean/std, 59/60 rolloff mean/std,
           61/62 flatness mean/std.  std is the population std (ddof=0)
           everywhere.

CLI:
  python extract_features.py --selftest    shape/determinism/sanity asserts

Library API used by train_cry_model.py:
  FEATURE_NAMES                  list of the 63 feature names, in order
  extract_window(samples_16k)    16000-sample window -> (63,) float32 vector
  load_wav(path)                 WAV file -> (samples, fs)
  resample_to_16k(x, fs)         arbitrary-rate samples -> 16 kHz samples
  extract_file_windows(path)     file -> list of 1 s window feature vectors
"""

import os
import sys
import time
import wave

import numpy as np

# ----------------------------------------------------------- constants -----
FS = 16000                 # target sample rate [Hz]
N_FFT = 512                # frame length [samples] (32 ms at 16 kHz)
HOP = 256                  # frame hop [samples] (16 ms)
N_MELS = 40                # mel filterbank filters
N_MFCC = 13                # MFCC coefficients kept
FMIN = 0.0                 # mel filterbank lower edge [Hz]
FMAX = 8000.0              # mel filterbank upper edge [Hz] = Nyquist at 16 kHz
WINDOW_SAMPLES = 16000     # aggregation window: 1 s
LOG_FLOOR_MEL = 1e-10      # mel log floor (matches magnitude floor 1e-5)
LOG_FLOOR_ENERGY = 1e-12   # frame-energy log floor
SINC_HALF_WIDTH = 16       # resampler half-width in input samples (33-tap kernel)
RESAMPLE_BLOCK = 20000     # output samples computed per block (memory bound)

HANN = (0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(N_FFT) / N_FFT)).astype(np.float64)

BIN_HZ = FS / N_FFT        # rfft bin width [Hz] (31.25 at 16 kHz / 512)
BIN_FREQS = np.arange(N_FFT // 2 + 1) * BIN_HZ   # bin k center frequency [Hz]
ROLLOFF_FRAC = 0.85        # spectral-rolloff cumulative-power fraction

FEATURE_NAMES = (
    [f"mfcc{i:02d}_mean" for i in range(N_MFCC)]
    + [f"mfcc{i:02d}_std" for i in range(N_MFCC)]
    + ["log_energy_mean", "log_energy_std", "zcr_mean"]
    + [f"mfcc{i:02d}_delta_mean" for i in range(N_MFCC)]
    + [f"mfcc{i:02d}_delta_std" for i in range(N_MFCC)]
    + ["log_energy_delta_mean", "log_energy_delta_std"]
    + ["spectral_centroid_mean", "spectral_centroid_std",
       "spectral_rolloff85_mean", "spectral_rolloff85_std",
       "spectral_flatness_mean", "spectral_flatness_std"]
)
assert len(FEATURE_NAMES) == 63

# Feature index offsets (v1 block must keep indices 0-28).
IDX_MFCC_MEAN = 0
IDX_MFCC_STD = 13
IDX_LOG_ENERGY_MEAN = 26
IDX_LOG_ENERGY_STD = 27
IDX_ZCR_MEAN = 28
IDX_MFCC_DELTA_MEAN = 29
IDX_MFCC_DELTA_STD = 42
IDX_LOG_ENERGY_DELTA_MEAN = 55
IDX_LOG_ENERGY_DELTA_STD = 56
IDX_CENTROID_MEAN = 57
IDX_ROLLOFF_MEAN = 59
IDX_FLATNESS_MEAN = 61


class UnsupportedAudio(Exception):
    """Raised for containers the stdlib `wave` module cannot parse."""


# ------------------------------------------------------- mel filterbank ----
def build_mel_filterbank():
    """Hand-built 40 x 257 triangular mel filterbank matrix.

    HTK mel scale: mel(f) = 2595 * log10(1 + f/700).
    N_MELS+2 = 42 edge frequencies are linearly spaced in mel between FMIN
    and FMAX and mapped back to Hz. Filter m is a triangle rising from edge m
    to edge m+1 and falling from edge m+1 to edge m+2. Column k corresponds
    to rfft bin k, i.e. frequency f_k = k * FS / N_FFT (257 bins for 16 kHz).
    At FMAX = Nyquist the first/last triangles taper off the grid edge, so
    filters 0 and N_MELS-1 may have no mass - documented and expected.
    """
    def hz_to_mel(f):
        return 2595.0 * np.log10(1.0 + np.asarray(f, dtype=np.float64) / 700.0)

    def mel_to_hz(m):
        return 700.0 * (10.0 ** (np.asarray(m, dtype=np.float64) / 2595.0) - 1.0)

    n_bins = N_FFT // 2 + 1
    edges_mel = np.linspace(hz_to_mel(FMIN), hz_to_mel(FMAX), N_MELS + 2)
    edges_hz = mel_to_hz(edges_mel)
    bin_hz = np.arange(n_bins) * (FS / N_FFT)

    fb = np.zeros((N_MELS, n_bins), dtype=np.float64)
    for m in range(N_MELS):
        left, center, right = edges_hz[m], edges_hz[m + 1], edges_hz[m + 2]
        up = (bin_hz - left) / max(center - left, 1e-12)
        down = (right - bin_hz) / max(right - center, 1e-12)
        fb[m] = np.maximum(0.0, np.minimum(up, down))
    return fb


MEL_FB = build_mel_filterbank()

# ------------------------------------------------------------- DCT-II ------
def build_dct_matrix():
    """Orthonormal DCT-II matrix (13 x 40), equal to
    scipy.fftpack.dct(x, type=2, norm='ortho') as a matrix multiply.

    D[k, n] = sqrt(2/N) * cos(pi * k * (2n + 1) / (2N)), with the k = 0 row
    scaled by 1/sqrt(2). Kept as an explicit matrix so the C port can use the
    same table (this is also the shape CMSIS-DSP's arm_mfcc_f32 expects).
    """
    k = np.arange(N_MFCC)[:, None]
    n = np.arange(N_MELS)[None, :]
    d = np.sqrt(2.0 / N_MELS) * np.cos(np.pi * k * (2 * n + 1) / (2.0 * N_MELS))
    d[0, :] *= 1.0 / np.sqrt(2.0)
    return d


DCT_MAT = build_dct_matrix()

# --------------------------------------------------------------- loaders ---
def load_wav(path):
    """Read a RIFF WAVE file with the stdlib `wave` module.

    Returns (samples, fs): float32 mono samples in [-1, 1] (multi-channel is
    mixed down by averaging), plus the native sample rate. Supports PCM
    8/16/24/32-bit; non-PCM or non-WAV containers (.3gp/.caf/.m4a/...) raise
    UnsupportedAudio - convert them with dataset_prep.py / ffmpeg first.
    """
    path = str(path)
    ext = os.path.splitext(path)[1].lower()
    if ext in (".3gp", ".caf", ".m4a", ".mp4", ".aac", ".ogg", ".flac",
               ".opus", ".wma"):
        raise UnsupportedAudio(
            f"non-WAV container '{ext}' cannot be read by stdlib wave "
            f"(run dataset_prep.py ffmpeg conversion first): {path}")
    try:
        raw = None
        # Retry transient OS errors: a parallel process may briefly lock
        # files/directories under ml_model/data/ (Windows sharing
        # violations surface as OSError from wave.open).  A genuinely
        # missing file also raises OSError (FileNotFoundError) - the retry
        # costs ~0.5 s and the caller's skip logic is unchanged.
        for attempt in range(3):
            try:
                with wave.open(path, "rb") as w:
                    nch = w.getnchannels()
                    width = w.getsampwidth()
                    fs = w.getframerate()
                    nframes = w.getnframes()
                    if w.getcomptype() != "NONE":
                        raise UnsupportedAudio(f"compressed WAV (comptype "
                                               f"{w.getcomptype()}) unsupported: {path}")
                    raw = w.readframes(nframes)
                break
            except OSError as exc:
                if attempt == 2:
                    raise
                time.sleep(0.25)
    except wave.Error as exc:
        raise UnsupportedAudio(f"wave module cannot parse {path}: {exc}") from exc

    if width == 2:                                             # s16 (standard)
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif width == 1:                                           # u8
        x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128.0) / 128.0
    elif width == 4:                                           # s32
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    elif width == 3:                                           # s24 (manual)
        b = np.frombuffer(raw, dtype=np.uint8)
        usable = (len(b) // 3) * 3
        b = b[:usable].reshape(-1, 3).astype(np.int64)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - (1 << 24), v)           # sign-extend
        x = v.astype(np.float64) / 8388608.0
    else:
        raise UnsupportedAudio(f"unsupported sample width {width} bytes: {path}")

    if nch > 1:
        x = x.reshape(-1, nch).mean(axis=1)
    return x.astype(np.float32), fs


def resample_to_16k(x, fs_in):
    """Bandlimited resampling to 16 kHz by windowed-sinc interpolation.

    For each output sample at time t (expressed in input-sample units) the
    value is the sum of input samples weighted by a sinc kernel evaluated at
    the distance to t, cut at min(fs_in, FS)/2 and tapered by a Blackman
    window spanning SINC_HALF_WIDTH input samples on each side (33 taps):

        h(d) = sinc(2 * fc * d) * blackman(|d| <= SINC_HALF_WIDTH)
        fc   = 0.5 * min(1.0, FS / fs_in)          (normalized cutoff)

    Weights are normalized per output sample so the kernel has exactly unity
    DC gain at every fractional phase, which also removes edge artifacts from
    the truncated support at signal boundaries. No scipy, no np.interp.
    Deterministic and exact for a given input. Output length:
    floor((len(x) - 1) * FS / fs_in) + 1 samples.
    """
    x = np.asarray(x, dtype=np.float64)
    if fs_in == FS:
        return x.astype(np.float32, copy=False)
    fs_in = float(fs_in)
    n_out = int(np.floor((len(x) - 1) * FS / fs_in)) + 1
    if len(x) == 0 or n_out <= 0:
        return np.zeros(0, dtype=np.float32)

    offs = np.arange(-SINC_HALF_WIDTH, SINC_HALF_WIDTH + 1)
    win = np.blackman(2 * SINC_HALF_WIDTH + 1)
    fc = 0.5 * min(1.0, FS / fs_in)

    out = np.empty(n_out, dtype=np.float64)
    for start in range(0, n_out, RESAMPLE_BLOCK):
        stop = min(start + RESAMPLE_BLOCK, n_out)
        # output sample i sits at input-sample position i * fs_in / FS
        pos_out = np.arange(start, stop) * (fs_in / FS)
        base = np.floor(pos_out).astype(np.int64)
        pos = base[:, None] + offs[None, :]                # (block, 33)
        d = pos - pos_out[:, None]
        h = np.sinc(2.0 * fc * d) * win[None, :]
        np.clip(pos, 0, len(x) - 1, out=pos)
        out[start:stop] = (x[pos] * h).sum(axis=1) / h.sum(axis=1)
    return out.astype(np.float32)


# ------------------------------------------------------------- frames ------
def _first_delta(series):
    """First difference along axis 0 with edge replication (documented in the
    module docstring): d[t] = (m'[t+1] - m'[t-1]) / 2 where m' is the input
    sequence padded by replicating its first and last frame.  A single-frame
    input yields zeros (nothing to difference)."""
    m = np.asarray(series, dtype=np.float64)
    if m.ndim == 1:                       # 1-D series: difference per element
        return _first_delta(m[:, None])[:, 0]
    if m.shape[0] < 2:
        return np.zeros_like(m)
    padded = np.vstack([m[:1], m, m[-1:]])
    return (padded[2:] - padded[:-2]) * 0.5


def _spectral_shape(power):
    """Per-frame spectral shape on the raw rfft power spectrum.

    power: (n_frames, 257) power spectrum (bin k = k * BIN_HZ Hz).
    Returns (centroid, rolloff, flatness) in Hz/Hz/linear, all finite:
    a frame whose power sums to 0 (digital silence) defines all three as 0
    (guards mirror the C implementation in src/audio_features.c 1:1); a
    frame with any exactly-zero power bin has geometric mean 0 -> flatness
    0 (log(0) = -inf -> exp -> 0, same in C)."""
    n_bins = power.shape[1]
    with np.errstate(divide="ignore", invalid="ignore"):
        p_sum = power.sum(axis=1)
        safe_sum = np.where(p_sum > 0.0, p_sum, 1.0)
        centroid = np.where(p_sum > 0.0,
                            (power * BIN_FREQS).sum(axis=1) / safe_sum, 0.0)
        cum = np.cumsum(power, axis=1)
        idx = (cum < ROLLOFF_FRAC * p_sum[:, None]).sum(axis=1)
        rolloff = idx * BIN_HZ                              # 0-power -> idx 0
        geo = np.exp(np.log(power).sum(axis=1) / n_bins)
        flatness = np.where(p_sum > 0.0, geo / (p_sum / n_bins), 0.0)
    return centroid, rolloff, flatness


def _frame_features(x):
    """16000-sample 1 s window -> per-frame MFCCs (n, 13), log-energies,
    ZCRs, spectral centroid/rolloff/flatness (n,)."""
    n_frames = 1 + (len(x) - N_FFT) // HOP
    if n_frames <= 0:
        return None, None, None, None, None, None
    idx = np.arange(N_FFT)[None, :] + HOP * np.arange(n_frames)[:, None]
    frames = x[idx] * HANN[None, :]

    spec = np.fft.rfft(frames, axis=1)
    power = (spec.real ** 2 + spec.imag ** 2) / N_FFT

    mel = np.log(power @ MEL_FB.T + LOG_FLOOR_MEL)          # (n_frames, 40)
    mfcc = mel @ DCT_MAT.T                                  # (n_frames, 13)

    frame_energy = (frames ** 2).sum(axis=1)
    log_energy = np.log(frame_energy + LOG_FLOOR_ENERGY)

    signs = np.signbit(frames)                              # Lab 4 convention
    zcr = (signs[:, 1:] != signs[:, :-1]).mean(axis=1)

    centroid, rolloff, flatness = _spectral_shape(power)
    return mfcc, log_energy, zcr, centroid, rolloff, flatness


def extract_window(samples_16k):
    """Aggregate one exactly-1 s (16000-sample) 16 kHz mono window into the
    63-feature vector in FEATURE_NAMES order (float32).

    Features (see the module docstring for exact definitions): the v1
    29-summary block (MFCC mean/std 2x13, log-energy mean/std, mean ZCR)
    at indices 0-28, then MFCC-delta mean/std (2x13), log-energy-delta
    mean/std (2), and spectral centroid/rolloff(0.85)/flatness mean/std
    (2x3) appended as indices 29-62.  Raises ValueError for any other
    window length - callers slice fixed 1 s windows.
    """
    x = np.asarray(samples_16k, dtype=np.float64)
    if len(x) != WINDOW_SAMPLES:
        raise ValueError(f"extract_window expects exactly {WINDOW_SAMPLES} "
                         f"samples (1 s at 16 kHz), got {len(x)}")
    mfcc, log_energy, zcr, centroid, rolloff, flatness = _frame_features(x)
    d_mfcc = _first_delta(mfcc)
    d_le = _first_delta(log_energy)
    return np.concatenate([
        # ---- v1 block, indices 0-28 (order and meaning unchanged) ----
        mfcc.mean(axis=0),
        mfcc.std(axis=0),                                   # population std
        [log_energy.mean(), log_energy.std(), zcr.mean()],
        # ---- v2 temporal dynamics, indices 29-62 ----
        d_mfcc.mean(axis=0),
        d_mfcc.std(axis=0),
        [d_le.mean(), d_le.std()],
        [centroid.mean(), centroid.std()],
        [rolloff.mean(), rolloff.std()],
        [flatness.mean(), flatness.std()],
    ]).astype(np.float32)


def extract_file_windows(path):
    """Whole file -> list of 1 s window feature vectors.

    Resamples to 16 kHz if needed, slices non-overlapping 1 s windows
    (a trailing partial window is dropped) and returns a list of (29,) float32
    arrays. Files shorter than 1 s yield an empty list.
    """
    x, fs = load_wav(path)
    x = resample_to_16k(x, fs)
    return [extract_window(x[s:s + WINDOW_SAMPLES])
            for s in range(0, len(x) - WINDOW_SAMPLES + 1, WINDOW_SAMPLES)]


# ------------------------------------------------------------- selftest ----
def _selftest() -> int:
    rng = np.random.default_rng(182)
    failures = []

    def check(name, cond, detail=""):
        status = "PASS" if cond else "FAIL"
        print(f"  [{status}] {name}{(' - ' + detail) if detail else ''}")
        if not cond:
            failures.append(name)

    print("extract_features selftest (seed 182)")

    # 1. matrix sanity -------------------------------------------------------
    check("MEL_FB shape (40, 257)", MEL_FB.shape == (40, 257))
    check("MEL_FB non-negative, rows have mass where in-band",
          bool((MEL_FB >= 0).all()) and bool(MEL_FB[5].sum() > 0)
          and bool(MEL_FB[20].sum() > 0))
    check("DCT_MAT orthonormal", np.allclose(DCT_MAT @ DCT_MAT.T,
                                             np.eye(N_MFCC), atol=1e-12))

    # 2. shapes, finiteness, determinism --------------------------------------
    sig = rng.normal(0.0, 0.05, WINDOW_SAMPLES).astype(np.float32)
    f1 = extract_window(sig)
    f2 = extract_window(sig.copy())
    check("feature vector shape (63,)", f1.shape == (63,))
    check("dtype float32", f1.dtype == np.float32)
    check("all finite", bool(np.isfinite(f1).all()))
    check("bit-exact determinism", np.array_equal(f1, f2))
    check("silence window finite", bool(np.isfinite(
        extract_window(np.zeros(WINDOW_SAMPLES, np.float32))).all()))

    # 2b. v2 feature definitions ----------------------------------------------
    f_sil = extract_window(np.zeros(WINDOW_SAMPLES, np.float32))
    check("silence: v1 block intact (mfcc00 = sqrt(40)*log(1e-10))",
          abs(float(f_sil[0]) - np.sqrt(40.0) * np.log(1e-10)) < 1e-3
          and abs(float(f_sil[26]) - np.log(1e-12)) < 1e-3)
    check("silence: deltas + spectral shape guard to 0",
          bool((np.abs(f_sil[29:]) < 1e-6).all()))

    t = np.arange(WINDOW_SAMPLES) / FS
    steady = extract_window((0.2 * np.sin(2 * np.pi * 500.0 * t))
                            .astype(np.float32))
    sweep_f = 400.0 + 500.0 * t / 1.0                    # 400 -> 900 Hz
    swept = extract_window((0.2 * np.sin(2 * np.pi * np.cumsum(sweep_f) / FS))
                           .astype(np.float32))
    d_std_steady = float(steady[42:55].max())
    d_std_swept = float(swept[42:55].max())
    check("steady tone has near-frozen MFCC deltas", d_std_steady < 0.05,
          f"max delta std {d_std_steady:.4f}")
    check("swept tone excites MFCC deltas (temporal dynamics)",
          d_std_swept > 0.10 and d_std_swept > 5.0 * d_std_steady,
          f"max delta std {d_std_swept:.4f}")
    check("centroid tracks tone frequency",
          abs(float(steady[57]) - 500.0) < 150.0,
          f"centroid {float(steady[57]):.1f} Hz vs 500 Hz tone")
    # A pure tone leaks only into the two adjacent bins (periodic Hann),
    # so cumulative power first reaches 85 % one bin above the tone.
    check("rolloff(0.85) of a pure tone lands one bin above it",
          abs(float(steady[59]) - 531.25) <= BIN_HZ,
          f"rolloff {float(steady[59]):.1f} Hz (tone bin 500.0 Hz)")
    check("flatness of tone << flatness of noise",
          float(steady[61]) < 0.5 * float(f1[61]),
          f"tone {float(steady[61]):.4f} vs noise {float(f1[61]):.4f}")
    check("delta-mean of constant signal ~ 0",
          abs(float(steady[55])) < 1e-6)

    # 3. resampler ------------------------------------------------------------
    t = np.arange(int(1.5 * 44100)) / 44100.0
    sine441 = (0.5 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)
    y = resample_to_16k(sine441, 44100)
    check("resample 44.1k->16k length", abs(len(y) - 1.5 * FS) <= 2,
          f"got {len(y)} vs {int(1.5 * FS)}")
    # dominant frequency of the resampled signal via zero-padded FFT
    spec = np.abs(np.fft.rfft(y * np.hanning(len(y)), 32768))
    dom_hz = int(np.argmax(spec)) * FS / 32768.0
    check("resample preserves 440 Hz tone", abs(dom_hz - 440.0) < 1.0,
          f"dominant {dom_hz:.2f} Hz")
    up = resample_to_16k(np.sin(2 * np.pi * 100.0 * np.arange(8000) / 8000.0)
                         .astype(np.float32), 8000)
    check("upsample 8k->16k length", abs(len(up) - 16000) <= 2, f"got {len(up)}")
    check("upsample preserves amplitude (unity sine RMS)",
          abs(np.sqrt((up ** 2).mean()) - 1.0 / np.sqrt(2)) < 0.02)

    # 4. tone vs noise separability at feature level ---------------------------
    tone = extract_window(resample_to_16k(
        (0.5 * np.sin(2 * np.pi * 700.0 * t)).astype(np.float32), 44100)[:WINDOW_SAMPLES])
    noise = extract_window(rng.normal(0.0, 0.05, WINDOW_SAMPLES))
    d12 = np.abs(tone[:13] - noise[:13]).max()              # MFCC means differ
    check("tone vs noise MFCCs differ (d > 1)", d12 > 1.0, f"max delta {d12:.2f}")

    print(f"{'ALL PASS' if not failures else f'{len(failures)} FAILURE(S): ' + ', '.join(failures)}")
    return 0 if not failures else 1


def main() -> int:
    if "--selftest" in sys.argv:
        return _selftest()
    print(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
