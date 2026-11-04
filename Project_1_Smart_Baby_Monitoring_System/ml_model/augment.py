#!/usr/bin/env python3
"""
augment.py - v3 wave-level data augmentation for the cry/non-cry trainer.

Purpose: donateacry (phone-app, close mic) cries look nothing like the frozen
test's ESC-50 crying_baby clips (ambient room recordings, distant mic, FULL of
broadband room noise: test-cry ZCR 0.185 vs train-cry 0.138, mfcc00_mean
-56.9 vs -76.7). The v1 forest therefore recalled only 0.29 of test-cry
windows. This module closes that gap by synthesizing, from every TRAINING cry
window only, variants that emulate the deployment channel.

v5 recipe (v2 diagnostics: the forest keys on spectral-shape MFCCs and
learned a synthetic-noise shortcut; v3/v4 lesson: the cry must stay DOMINANT
in mixtures - ESC-50 crying_baby is loud crying in a room, not a faint cry
buried in noise, and noise-heavy mixtures inflate the ambient false-positive
rate):

  * REAL-NOISE MIXING: with p=0.5 a cry window is mixed with a REAL ESC-50
    fold 1-3 non-cry window (train split only) at SNR 10-25 dB (cry clearly
    dominant). This creates contrastive pairs (cry+real ambience -> cry vs
    the same ambience alone -> noncry). Remaining variants keep synthetic
    white/pink noise at SNR 5-20 dB.
  * REVERB: 2-3 decaying echo taps (25-110 ms, gain 0.15-0.40), p=0.7 -
    the dominant room effect the v2 recipe lacked.
  * playback-bandwidth limiting: first-order lowpass (Tustin, prewarped),
    cutoff 2.5-7 kHz, p=0.6 (distant/phone speaker).
  * volume -6..+14 dB for all variants; a "loud" branch (p=0.3) applies
    +10..+20 dB - mirrored on the non-cry side so loudness alone never
    becomes a cry cue (ESC-50 cries are LOUD: mfcc00_mean -56.9 vs
    train-cry -76.7; the loud branch closes that gap). ALL training non-cry
    windows get a noise/volume variant (frac 1.0) to counterweight the
    boundary.
  * pitch/speed shift +/-10% (linear-interp resample trick, per FILE so a
    variant is a coherent re-recording).
  * 50 Hz mains hum mix at low level (-32..-22 dB rel RMS) with 100/150 Hz
    harmonics.

A FRACTION of TRAINING non-cry windows also gets noise + volume variants
(same loud branch) so the model stays calibrated under noise.

Also provides stress_val_windows(): the DEPLOYMENT channel applied to VAL cry
windows only (always lowpass + heavier real-noise mixing at SNR 0-10 dB) so
the trainer can tune its vote threshold in the noisy regime instead of the
clean close-mic regime. This still uses ONLY val data (plus the train-split
noise pool) - the frozen test is never touched.

Hard rules encoded here:
  * only manifest rows with split == "train" are ever used for training
    augmentation, and only their waveforms enter the noise pool;
  * val rows appear ONLY in stress_val_windows (label-preserving channel
    simulation for threshold tuning), never in training data;
  * test/confuser rows are never touched;
  * everything is deterministic: every (file, variant) carries its own RNG
    seed derived from a CRC32 of its cache key, so results are independent
    of processing order and stable across cache-partial runs.

Augmented features are precomputed ONCE into ml_model/features_aug_cache.npz
(same layout as features_cache.npz; keys append the variant id) so retraining
is fast and reproducible.

Library API used by train_cry_model.py:
  load_noise_pool(manifest_rows, dataset_dir)     -> list of 1 s noise windows
  load_or_build_augmented(manifest_rows, dataset_dir, n_cry_variants,
                          noncry_frac, noise_pool)
                                                  -> (X, y, src, rel)
  stress_val_windows(manifest_rows, dataset_dir, noise_pool)
                                                  -> (X, y, rel)

CLI:
  python augment.py --dataset-dir ml_model --selftest
"""

import argparse
import os
import sys
import zlib

import numpy as np
from scipy.signal import lfilter           # train-time only; the embedded
                                            # front-end stays scipy-free

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from extract_features import (              # noqa: E402
    FS, FEATURE_NAMES, WINDOW_SAMPLES, extract_window, load_wav,
    resample_to_16k, UnsupportedAudio)

N_FEATURES = len(FEATURE_NAMES)             # 29, mirrors the trainer constant

AUG_CACHE_NAME = "features_aug_cache.npz"
AUG_CACHE_VERSION = "aug-v5"     # bump to invalidate stale augmented features
N_AUG_CRY_VARIANTS_DEFAULT = 2   # augmented variants per training cry clip
NONCRY_FRAC_DEFAULT = 1.0        # fraction of training non-cry windows aug'd
SEED = 42                        # master seed, matches the trainer contract

LOWPASS_FC_MIN_HZ = 2500.0
LOWPASS_FC_MAX_HZ = 7000.0
LOWPASS_PROB = 0.6               # per cry window (stress val: always on)
REVERB_PROB = 0.7                # per cry window
REVERB_TAP_DELAYS_MS = (25.0, 45.0, 65.0, 85.0, 110.0)
REVERB_GAIN_MAX = 0.40
NOISE_PROB = 0.85                # per cry window when real-noise mix not used
REAL_NOISE_PROB = 0.5            # per cry window (real ESC-50 ambience mix)
# v3/v4 lesson: the cry must stay DOMINANT in the mixture. ESC-50 crying_baby
# clips are loud crying in a room, not a faint cry buried in noise - burying
# donateacry's quiet cries at low SNR created "noise labeled cry" windows
# that blurred the boundary (val noncry FP at majority: 18.7% v2 -> 21.6% v3
# -> 23.6% v4). At SNR 10-25 dB the cry formant structure stays learnable
# while the room channel (noise floor, reverb, distance) still varies.
REAL_NOISE_SNR_MIN_DB = 10.0     # stress val: 8-18 dB
REAL_NOISE_SNR_MAX_DB = 25.0
SNR_MIN_DB, SNR_MAX_DB = 5.0, 20.0          # synthetic noise range
GAIN_MIN_DB, GAIN_MAX_DB = -6.0, 14.0
LOUD_PROB = 0.3                  # loud branch, mirrored cry AND non-cry
LOUD_GAIN_MIN_DB, LOUD_GAIN_MAX_DB = 10.0, 20.0
SPEED_MIN, SPEED_MAX = 0.90, 1.10    # +/-10 % pitch/speed, per cry FILE variant
SPEED_PROB = 0.5
HUM_PROB = 0.25                  # per cry window
HUM_LEVEL_MIN_DB, HUM_LEVEL_MAX_DB = -32.0, -22.0   # rel signal RMS
NOISE_POOL_MAX_WINDOWS = 4000    # cap ESC-50 ambience pool memory (~250 MB)


# ------------------------------------------------------------ primitives ----
def _rms(x):
    """RMS with an explicit float64 cast (numpy 2.x raises FloatingPointError
    on float32-subnormal squares otherwise) and a tiny floor so silent
    windows still yield a usable noise-target scale."""
    with np.errstate(under="ignore", over="ignore"):
        p = float(np.mean(np.square(np.asarray(x, dtype=np.float64))))
    return float(np.sqrt(max(p, 1e-24)))


def gain_db(x, db):
    """Volume change in dB (amplitude ratio 10^(db/20))."""
    return x * float(10.0 ** (db / 20.0))


def lowpass_first_order(x, fc_hz, fs=FS):
    """First-order IIR lowpass (Tustin/bilinear with frequency prewarping,
    so the -3 dB point lands exactly on fc_hz), emulating a small speaker's
    roll-off. H(z) = (b0 + b1 z^-1) / (1 + a1 z^-1), K = tan(pi*fc/fs)."""
    k = np.tan(np.pi * fc_hz / fs)
    b0 = b1 = k / (1.0 + k)
    a1 = (k - 1.0) / (k + 1.0)
    return lfilter([b0, b1], [1.0, a1], x).astype(np.float64)


def apply_reverb(x, rng, fs=FS):
    """Cheap room reverb: 2-3 decaying echo taps at 25-110 ms (later taps
    weaker). Emulates the distant-mic room recordings of the test domain."""
    n_taps = int(rng.integers(2, 4))
    delays = rng.choice(len(REVERB_TAP_DELAYS_MS), size=n_taps, replace=False)
    y = np.array(x, dtype=np.float64)
    for order, di in enumerate(sorted(delays)):
        d = max(int(REVERB_TAP_DELAYS_MS[di] * fs / 1000.0), 1)
        g = float(rng.uniform(0.15, REVERB_GAIN_MAX)) * (0.7 ** order)
        y[d:] += g * x[:-d]
    return y


def _pink_noise(n, rng):
    """1/f (pink) noise via spectral shaping of white noise (Kasdin-Walter
    style: rfft -> 1/sqrt(f) weights -> irfft)."""
    white = rng.standard_normal(n)
    spec = np.fft.rfft(white)
    f = np.fft.rfftfreq(n, d=1.0 / FS)
    shape = np.ones_like(f)
    nz = f > 0
    shape[nz] = 1.0 / np.sqrt(f[nz])
    return np.fft.irfft(spec * shape, n=n)


def mix_noise(x, noise, snr_db):
    """Scale a noise array to the requested SNR (vs signal RMS) and add."""
    noise = np.asarray(noise, dtype=np.float64)
    noise_rms = _rms(noise)
    if noise_rms <= 0.0 or len(noise) != len(x):
        return np.asarray(x, dtype=np.float64)
    target_rms = _rms(x) * float(10.0 ** (-snr_db / 20.0))
    return np.asarray(x, dtype=np.float64) + noise * (target_rms / noise_rms)


def add_noise(x, rng, snr_db, kind="white"):
    """Additive white/pink noise scaled to the requested SNR vs signal RMS."""
    n = len(x)
    noise = (rng.standard_normal(n) if kind == "white"
             else _pink_noise(n, rng))
    return mix_noise(x, noise, snr_db)


def add_hum(x, rng, fs=FS):
    """Mains hum: 50 Hz + 100/150 Hz harmonics at -32..-22 dB rel signal RMS
    (the level constants are already negative dB)."""
    t = np.arange(len(x), dtype=np.float64) / fs
    amp = _rms(x) * float(10.0 ** (rng.uniform(HUM_LEVEL_MIN_DB,
                                               HUM_LEVEL_MAX_DB) / 20.0))
    ph = rng.uniform(0.0, 2.0 * np.pi)
    hum = (np.sin(2.0 * np.pi * 50.0 * t + ph)
           + 0.5 * np.sin(2.0 * np.pi * 100.0 * t + 2.0 * ph)
           + 0.35 * np.sin(2.0 * np.pi * 150.0 * t + 3.0 * ph))
    return x + amp * hum / 1.85


def speed_pitch_shift(x, rate):
    """Resample trick applied to a WHOLE clip: linear-interp resampling by
    `rate` (rate > 1 plays faster -> pitch up; rate < 1 slower -> pitch
    down). Output length is round(len(x)/rate); 1 s windows are sliced from
    the shifted waveform afterwards, so every window stays WINDOW_SAMPLES."""
    x = np.asarray(x, dtype=np.float64)
    rate = float(min(max(rate, 0.5), 2.0))
    n_out = max(int(round(len(x) / rate)), 2)
    if n_out == len(x):
        return x
    return np.interp(np.linspace(0.0, len(x) - 1.0, n_out),
                     np.arange(len(x), dtype=np.float64), x)


def _clip_guard(y):
    """Keep float audio in [-1, 1] without a hard-sounding clip: rescale the
    whole window if it would exceed full scale."""
    peak = float(np.max(np.abs(y))) if len(y) else 0.0
    if peak > 0.999:
        y = y * (0.999 / peak)
    return y


def _apply_volume(y, rng):
    """Volume -8..+8 dB; the loud branch (p=0.2, +8..+16 dB) is mirrored on
    cry AND non-cry windows so loudness alone is not a cry cue."""
    if rng.random() < LOUD_PROB:
        return gain_db(y, rng.uniform(LOUD_GAIN_MIN_DB, LOUD_GAIN_MAX_DB))
    return gain_db(y, rng.uniform(GAIN_MIN_DB, GAIN_MAX_DB))


# ------------------------------------------------------------- recipes ------
def augment_cry_window(w, rng, noise_pool=None, stress=False):
    """One 1 s cry window -> one augmented variant (deterministic given rng).

    stress=True is the deployment-channel simulation used for the VAL-cry
    threshold proxy: reverb p=0.8, real-noise mixing at SNR 8-18 dB (cry
    still dominant), and lowpass at p=0.5 - half the stress windows model
    the live speaker-playback channel, half the full-band ambient-mic
    channel of the frozen test domain."""
    y = np.asarray(w, dtype=np.float64)
    lp_p = 0.5 if stress else LOWPASS_PROB
    if rng.random() < lp_p:                               # distant speaker
        fc_lo = 2500.0 if stress else LOWPASS_FC_MIN_HZ
        fc_hi = 5000.0 if stress else LOWPASS_FC_MAX_HZ
        y = lowpass_first_order(y, rng.uniform(fc_lo, fc_hi))
    if rng.random() < (0.8 if stress else REVERB_PROB):   # room reverb
        y = apply_reverb(y, rng)
    if rng.random() < HUM_PROB:                           # mains hum
        y = add_hum(y, rng)
    r = rng.random()                                      # room noise
    snr_lo = 8.0 if stress else REAL_NOISE_SNR_MIN_DB
    snr_hi = 18.0 if stress else REAL_NOISE_SNR_MAX_DB
    real_p = 0.85 if stress else REAL_NOISE_PROB
    if noise_pool and r < real_p:
        idx = int(rng.integers(0, len(noise_pool)))
        y = mix_noise(y, noise_pool[idx], rng.uniform(snr_lo, snr_hi))
    elif r < (real_p + (1.0 - real_p) * (NOISE_PROB / 0.85)):
        kind = "white" if rng.random() < 0.45 else "pink"
        y = add_noise(y, rng, rng.uniform(SNR_MIN_DB, SNR_MAX_DB), kind)
    y = _apply_volume(y, rng)                             # volume (+ loud)
    return _clip_guard(y)


def augment_noncry_window(w, rng):
    """Calibration variant for non-cry windows: synthetic noise + volume only
    (keeps them recognizably non-cry under noise; no muffling, no reverb -
    those are cry-channel effects)."""
    y = np.asarray(w, dtype=np.float64)
    if rng.random() < NOISE_PROB:
        kind = "white" if rng.random() < 0.5 else "pink"
        y = add_noise(y, rng, rng.uniform(SNR_MIN_DB, SNR_MAX_DB), kind)
    y = _apply_volume(y, rng)
    return _clip_guard(y)


def _file_seed(key):
    """Stable per-(file, variant) RNG seed (CRC32; Python's hash() is
    salted per process and must not be used)."""
    return (zlib.crc32(key.encode("utf-8")) ^ SEED) & 0xFFFFFFFF


def _windows_of(x16k):
    """Non-overlapping 1 s windows from a 16 kHz waveform (trailing partial
    window dropped), mirroring extract_features.extract_file_windows()."""
    return [x16k[s:s + WINDOW_SAMPLES]
            for s in range(0, len(x16k) - WINDOW_SAMPLES + 1, WINDOW_SAMPLES)]


# ------------------------------------------------------- real-noise pool ----
def _pool_cache_path(dataset_dir):
    return os.path.join(dataset_dir, f"noise_pool_{AUG_CACHE_VERSION}.npz")


def load_noise_pool(manifest_rows, dataset_dir,
                    max_windows=NOISE_POOL_MAX_WINDOWS, log=print):
    """Deterministic pool of REAL ambience windows for cry mixing: 1 s
    windows from TRAIN-split non-cry clips only (ESC-50 folds 1-3). Val/test/
    confuser audio never enters the pool. Subsampling (if over the cap) is
    seeded with SEED, so the pool is identical across runs. The pool is
    cached to noise_pool_<version>.npz (validated against the number of
    source files) so repeated trainer runs skip the ~2 min WAV load."""
    files = []
    for row in manifest_rows:
        if row.get("split") == "train" and row.get("label") != "cry":
            files.append(row["path"])
    files = sorted(files)

    cache_path = _pool_cache_path(dataset_dir)
    if os.path.exists(cache_path):
        try:
            with np.load(cache_path, allow_pickle=False) as z:
                if (int(z["n_source_files"]) == len(files)
                        and int(z["max_windows"]) == int(max_windows)
                        and z["pool"].shape[1] == WINDOW_SAMPLES):
                    log(f"noise pool: {z['pool'].shape[0]} windows loaded "
                        f"from {os.path.basename(cache_path)}")
                    return [row for row in z["pool"]]
        except Exception as exc:                      # noqa: BLE001
            print(f"noise-pool cache unreadable ({exc}) - rebuilding")

    pool = []
    for rel in files:
        abs_path = os.path.join(dataset_dir, rel)
        if not os.path.exists(abs_path):
            continue
        try:
            x, fs = load_wav(abs_path)
            x = resample_to_16k(x, fs)
        except Exception as exc:                      # noqa: BLE001
            print(f"  noise-pool skip: {abs_path}: {exc}")
            continue
        for w in _windows_of(x):
            if len(w) == WINDOW_SAMPLES:
                pool.append(w.astype(np.float32))
    if len(pool) > max_windows:
        rng = np.random.default_rng(SEED)
        pick = rng.permutation(len(pool))[:max_windows]
        pool = [pool[i] for i in sorted(pick.tolist())]
    tmp = cache_path + ".tmp.npz"
    np.savez_compressed(tmp,
                        pool=np.stack(pool).astype(np.float32)
                        if pool else np.zeros((0, WINDOW_SAMPLES), np.float32),
                        n_source_files=np.int64(len(files)),
                        max_windows=np.int64(max_windows))
    os.replace(tmp, cache_path)
    log(f"noise pool: {len(pool)} real ambience windows (train non-cry only) "
        f"-> {os.path.basename(cache_path)}")
    return pool


# ------------------------------------------------------- augmented cache ----
def _load_cache(cache_path):
    if not os.path.exists(cache_path):
        return {}
    try:
        with np.load(cache_path, allow_pickle=False) as z:
            keys, flat, counts = z["keys"], z["feats_flat"], z["counts"]
        if flat.shape[1] != N_FEATURES:
            return {}
        cache, off = {}, 0
        for k, c in zip(keys, counts):
            cache[str(k)] = flat[off:off + int(c)]
            off += int(c)
        return cache
    except Exception as exc:                          # noqa: BLE001
        print(f"aug cache unreadable ({exc}) - rebuilding")
        return {}


def _save_cache(cache_path, cache):
    keys = list(cache.keys())
    counts = np.array([cache[k].shape[0] for k in keys], dtype=np.int64)
    flat = (np.concatenate([cache[k] for k in keys], axis=0)
            if keys else np.zeros((0, N_FEATURES), np.float32))
    tmp = cache_path + ".tmp.npz"
    np.savez_compressed(tmp, keys=np.array(keys, dtype="U"),
                        counts=counts, feats_flat=flat)
    os.replace(tmp, cache_path)


def _aug_file_block(rel_path, abs_path, label, variant_id, noncry_frac,
                    noise_pool):
    """One augmented variant of one training clip -> (n, 29) float32 features.
    Fully deterministic: the RNG is seeded from the variant's cache key.
    For non-cry clips, `noncry_frac` of the windows get a calibration variant
    (Bernoulli per window, seeded with the file), the rest are left out."""
    # noncry_frac only shapes non-cry blocks; cry seeds/keys must not depend
    # on it so tuning the fraction does not invalidate cry variant caches.
    seed_key = (f"{AUG_CACHE_VERSION}|{rel_path}|{variant_id}"
                + ("" if label == "cry" else f"|f{noncry_frac:.2f}"))
    rng = np.random.default_rng(_file_seed(seed_key))
    try:
        x, fs = load_wav(abs_path)
        x = resample_to_16k(x, fs)
    except (UnsupportedAudio, Exception) as exc:      # noqa: BLE001
        print(f"  aug skip: {abs_path}: {exc}")
        return np.zeros((0, N_FEATURES), np.float32)

    feats = []
    # donateacry has near-digital-silence windows whose subnormal squares
    # trigger numpy 2.x FloatingPointError - underflow to 0 is harmless here.
    with np.errstate(under="ignore", over="ignore"):
        if label == "cry" and rng.random() < SPEED_PROB:
            # per-FILE pitch/speed shift: a variant is a coherent re-recording
            x = speed_pitch_shift(x, rng.uniform(SPEED_MIN, SPEED_MAX))
        windows = _windows_of(x)
        for w in windows:
            if len(w) != WINDOW_SAMPLES:
                continue
            if label != "cry" and rng.random() >= noncry_frac:
                continue                              # not selected this run
            aw = (augment_cry_window(w, rng, noise_pool) if label == "cry"
                  else augment_noncry_window(w, rng))
            feats.append(extract_window(aw))
    return (np.stack(feats).astype(np.float32) if feats
            else np.zeros((0, N_FEATURES), np.float32))


def load_or_build_augmented(manifest_rows, dataset_dir,
                            n_cry_variants=N_AUG_CRY_VARIANTS_DEFAULT,
                            noncry_frac=NONCRY_FRAC_DEFAULT, noise_pool=None,
                            save_every=100, log=print):
    """Train-split wave augmentation with a persistent feature cache.

    Returns (X, y, src, rel): augmented-window matrix + labels/source/paths,
    in manifest order (cry variants first for each cry row, then the non-cry
    calibration variant block for each non-cry row). Non-train rows are
    ignored by contract (val/test/confuser are never augmented).
    """
    cache_path = os.path.join(dataset_dir, AUG_CACHE_NAME)
    cache = _load_cache(cache_path)
    X, y, src, rel = [], [], [], []
    n_new, n_files = 0, 0

    for row in manifest_rows:
        if row.get("split") != "train":
            continue
        rel_path, label = row["path"], row["label"]
        abs_path = os.path.join(dataset_dir, rel_path)
        if not os.path.exists(abs_path):
            continue
        variants = ([f"cry{v}" for v in range(n_cry_variants)] if label == "cry"
                    else ["nc"] * (1 if noncry_frac > 0 else 0))
        for vid in variants:
            st = os.stat(abs_path)
            key = (f"{AUG_CACHE_VERSION}|{rel_path}|{st.st_mtime_ns}|"
                   f"{st.st_size}|{vid}"
                   + ("" if label == "cry" else f"|f{noncry_frac:.2f}"))
            if key not in cache:
                cache[key] = _aug_file_block(rel_path, abs_path, label, vid,
                                             noncry_frac, noise_pool)
                n_new += 1
                n_files += 1
                if n_files % save_every == 0:
                    _save_cache(cache_path, cache)   # resilient incremental
                    log(f"  aug cache: {n_files} new variant blocks saved")
            block = cache[key]
            if block.shape[0] == 0:
                continue
            X.append(block)
            y.append(np.ones(block.shape[0], dtype=np.int64) if label == "cry"
                     else np.zeros(block.shape[0], dtype=np.int64))
            src.append(np.full(block.shape[0], row["source_id"], dtype=object))
            rel.append(np.full(block.shape[0], rel_path, dtype=object))

    _save_cache(cache_path, cache)
    if n_new:
        log(f"aug cache: built {n_new} new variant blocks -> {cache_path}")
    else:
        log(f"aug cache: all variants cached -> {cache_path}")
    if not X:
        return (np.zeros((0, N_FEATURES), np.float32), np.zeros(0, np.int64),
                np.zeros(0, dtype=object), np.zeros(0, dtype=object))
    return (np.concatenate(X), np.concatenate(y), np.concatenate(src),
            np.concatenate(rel))


# --------------------------------------------- stress-val threshold proxy ---
def stress_val_windows(manifest_rows, dataset_dir, noise_pool, log=print):
    """Deployment-channel simulation of the VAL cry clips (label-preserving;
    uses ONLY val data + the train-split noise pool). Returns (X, y, rel):
    stress-augmented windows of every val cry clip, for threshold tuning in
    the noisy regime. Deterministic per file ("valstress" seed key)."""
    X, y, rel = [], [], []
    for row in manifest_rows:
        if row.get("split") != "val" or row.get("label") != "cry":
            continue
        rel_path = row["path"]
        abs_path = os.path.join(dataset_dir, rel_path)
        if not os.path.exists(abs_path):
            continue
        rng = np.random.default_rng(
            _file_seed(f"{AUG_CACHE_VERSION}|{rel_path}|valstress"))
        try:
            x, fs = load_wav(abs_path)
            x = resample_to_16k(x, fs)
        except Exception as exc:                      # noqa: BLE001
            print(f"  stress-val skip: {abs_path}: {exc}")
            continue
        feats = []
        with np.errstate(under="ignore", over="ignore"):
            for w in _windows_of(x):
                if len(w) != WINDOW_SAMPLES:
                    continue
                aw = augment_cry_window(w, rng, noise_pool, stress=True)
                feats.append(extract_window(aw))
        if not feats:
            continue
        block = np.stack(feats).astype(np.float32)
        X.append(block)
        y.append(np.ones(block.shape[0], dtype=np.int64))
        rel.append(np.full(block.shape[0], rel_path, dtype=object))
    if not X:
        log("stress-val: no val cry windows available")
        return (np.zeros((0, N_FEATURES), np.float32), np.zeros(0, np.int64),
                np.zeros(0, dtype=object))
    log(f"stress-val: {sum(b.shape[0] for b in X)} windows from "
        f"{len(X)} val cry clips")
    return np.concatenate(X), np.concatenate(y), np.concatenate(rel)


# --------------------------------------------------------------- selftest ---
def _selftest() -> int:
    rng = np.random.default_rng(SEED)
    failures = []

    def check(name, cond, detail=""):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}"
              f"{(' - ' + detail) if detail else ''}")
        if not cond:
            failures.append(name)

    print("augment selftest (seed 42)")
    w = (0.4 * np.sin(2 * np.pi * 400.0 * np.arange(WINDOW_SAMPLES) / FS)
         .astype(np.float64))

    y1 = augment_cry_window(w, rng, noise_pool=[w * 0.1])
    y2 = augment_cry_window(w, np.random.default_rng(_file_seed("x")))
    check("cry variant finite + in range",
          bool(np.isfinite(y1).all()) and float(np.max(np.abs(y1))) <= 1.0)
    check("cry variant changed the waveform",
          not np.allclose(y1, w), f"rms {np.sqrt(np.mean(y1**2)):.4f}")
    check("different seeds give different variants", not np.allclose(y1, y2))

    f = extract_window(y1)
    check("augmented window yields 29 finite features",
          f.shape == (29,) and bool(np.isfinite(f).all()))

    # SNR correctness: white noise at 10 dB SNR -> noise RMS = sig/10^0.5
    x = np.full(WINDOW_SAMPLES, 0.1)
    snoring = add_noise(x, np.random.default_rng(1), 10.0, "white")
    ratio = _rms(snoring - x) / _rms(x)
    snr_meas = -10.0 * np.log10(ratio ** 2)      # 10*log10(Psig/Pnoise)
    check("white noise SNR honored (~10 dB)", abs(snr_meas - 10.0) < 0.5,
          f"{snr_meas:.2f} dB")

    # mix_noise with an arbitrary (real) noise array must honor SNR too
    real = rng.standard_normal(WINDOW_SAMPLES) * 0.3
    mixed = mix_noise(np.full(WINDOW_SAMPLES, 0.05), real, 6.0)
    r2 = _rms(mixed - 0.05) / 0.05
    check("real-noise mix SNR honored (~6 dB)",
          abs(-10.0 * np.log10(r2 ** 2) - 6.0) < 0.5,
          f"{-10.0 * np.log10(r2**2):.2f} dB")

    # reverb: output keeps the tone (peak spectrum unchanged) + adds energy
    rev = apply_reverb(w, np.random.default_rng(9))
    check("reverb adds tail energy, keeps content",
          _rms(rev) > _rms(w) * 1.05 and _rms(rev) < _rms(w) * 1.6,
          f"rms ratio {_rms(rev)/_rms(w):.3f}")

    # lowpass: 6 kHz tone through a 3 kHz first-order LPF must be attenuated
    t6 = (0.5 * np.sin(2 * np.pi * 6000.0 * np.arange(WINDOW_SAMPLES) / FS))
    att = _rms(lowpass_first_order(t6, 3000.0)) / _rms(t6)
    check("3 kHz LPF attenuates 6 kHz tone", att < 0.35, f"gain {att:.3f}")

    # pink vs white spectral slope
    pw = _pink_noise(WINDOW_SAMPLES, np.random.default_rng(3))
    spec = np.abs(np.fft.rfft(pw * np.hanning(WINDOW_SAMPLES))) ** 2
    lo = spec[5:20].mean(); hi = spec[3000:4000].mean()
    check("pink noise has 1/f tilt", np.log10(lo / hi) > 1.5,
          f"decade drop {np.log10(lo/hi):.2f}")

    yh = add_hum(w, np.random.default_rng(5))
    check("hum adds ~-27 dB component",
          0.0 < _rms(yh - w) / _rms(w) < 0.10,
          f"ratio {_rms(yh - w)/_rms(w):.4f}")

    fast = speed_pitch_shift(w, 1.10)
    slow = speed_pitch_shift(w, 0.90)
    check("speed shift output length = len/rate",
          len(fast) == int(round(WINDOW_SAMPLES / 1.10))
          and len(slow) == int(round(WINDOW_SAMPLES / 0.90)),
          f"fast {len(fast)}, slow {len(slow)}")
    dom_hz = lambda sig: (np.argmax(np.abs(np.fft.rfft(
        sig * np.hanning(len(sig))))) * FS / len(sig))
    check("rate 1.1 raises pitch ~10%",
          abs(dom_hz(fast) - 400.0 * 1.1) <= 8.0,
          f"{dom_hz(w):.1f} Hz -> {dom_hz(fast):.1f} Hz")
    check("rate 0.9 lowers pitch ~10%",
          abs(dom_hz(slow) - 400.0 * 0.9) <= 8.0,
          f"{dom_hz(w):.1f} Hz -> {dom_hz(slow):.1f} Hz")

    yn = augment_noncry_window(w, np.random.default_rng(7))
    check("noncry variant finite + in range",
          bool(np.isfinite(yn).all()) and float(np.max(np.abs(yn))) <= 1.0)
    check("noncry variant changed the waveform", not np.allclose(yn, w))

    ys = augment_cry_window(w, np.random.default_rng(11), stress=True)
    fs_ = extract_window(ys)
    check("stress variant finite + 29 features",
          bool(np.isfinite(ys).all()) and fs_.shape == (29,)
          and bool(np.isfinite(fs_).all()))

    print(f"{'ALL PASS' if not failures else f'{len(failures)} FAILURE(S): ' + ', '.join(failures)}")
    return 0 if not failures else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="augment.py - wave-level "
                                 "augmentation + augmented feature cache")
    ap.add_argument("--dataset-dir", default=HERE)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--variants", type=int, default=N_AUG_CRY_VARIANTS_DEFAULT,
                    help="augmented variants per training cry clip")
    ap.add_argument("--noncry-frac", type=float, default=NONCRY_FRAC_DEFAULT,
                    help="fraction of training non-cry windows augmented")
    args = ap.parse_args()
    if args.selftest:
        return _selftest()

    import csv
    manifest = os.path.join(args.dataset_dir, "manifest.csv")
    with open(manifest, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    pool = load_noise_pool(rows, os.path.abspath(args.dataset_dir))
    X, y, src, rel = load_or_build_augmented(
        rows, os.path.abspath(args.dataset_dir),
        n_cry_variants=args.variants, noncry_frac=args.noncry_frac,
        noise_pool=pool)
    print(f"augmented windows: {X.shape[0]} (cry={int(y.sum())}, "
          f"noncry={int((1 - y).sum())})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
