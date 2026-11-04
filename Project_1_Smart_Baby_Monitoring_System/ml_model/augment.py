#!/usr/bin/env python3
"""
augment.py - v2 wave-level data augmentation for the cry/non-cry trainer.

Purpose: donateacry (phone-app, close mic) cries look nothing like the frozen
test's ESC-50 crying_baby clips (ambient room recordings, distant mic, FULL of
broadband room noise: test-cry ZCR 0.185 vs train-cry 0.138, mfcc00_mean
-56.9 vs -76.7). The v1 forest therefore recalled only 0.29 of test-cry
windows. This module closes that gap by synthesizing, from every TRAINING cry
window only, variants that emulate the deployment channel:

  * playback-bandwidth limiting: first-order lowpass, cutoff 3-6 kHz
    (distant/phone speaker)
  * additive room noise: white or pink (1/f) at SNR 5-20 dB (raises ZCR and
    fills the mel spectrogram floor exactly like the ESC-50 shift)
  * volume +/-8 dB
  * pitch/speed shift +/-10% (linear-interp resample trick, per FILE so a
    variant is a coherent re-recording)
  * 50 Hz mains hum mix at low level (-32..-22 dB rel RMS) with 100/150 Hz
    harmonics

A FRACTION of TRAINING non-cry windows also gets noise + volume variants so
the model stays calibrated under noise (per the improvement plan; no lowpass
on non-cry - muffling ambient noise would make it look MORE cry-like).

Hard rules encoded here:
  * only manifest rows with split == "train" are ever augmented;
  * val / test / confuser rows are passed through untouched;
  * everything is deterministic: every (file, variant) carries its own RNG
    seed derived from a CRC32 of its cache key, so results are independent
    of processing order and stable across cache-partial runs.

Augmented features are precomputed ONCE into ml_model/features_aug_cache.npz
(same layout as features_cache.npz; keys append the variant id) so retraining
is fast and reproducible.

Library API used by train_cry_model.py:
  load_or_build_augmented(manifest_rows, dataset_dir, n_cry_variants,
                          noncry_frac) -> (X_aug, y_aug, src_aug, rel_aug)

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
AUG_CACHE_VERSION = "aug-v1"     # bump to invalidate stale augmented features
SEED = 42                        # master seed, matches the trainer contract
LOWPASS_FC_MIN_HZ = 3000.0
LOWPASS_FC_MAX_HZ = 6000.0
LOWPASS_PROB = 0.75              # per cry window
NOISE_PROB = 0.85                # per cry window (white 0.45 / pink 0.40)
SNR_MIN_DB, SNR_MAX_DB = 5.0, 20.0
GAIN_MIN_DB, GAIN_MAX_DB = -8.0, 8.0
SPEED_MIN, SPEED_MAX = 0.90, 1.10    # +/-10 % pitch/speed, per cry FILE variant
SPEED_PROB = 0.5
HUM_PROB = 0.25                  # per cry window
HUM_LEVEL_MIN_DB, HUM_LEVEL_MAX_DB = -32.0, -22.0   # rel signal RMS
NONCRY_NOISE_PROB = 0.8          # white/pink for non-cry variants


# ------------------------------------------------------------ primitives ----
def _rms(x):
    return float(np.sqrt(np.mean(np.square(x, dtype=np.float64)) + 1e-12))


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


def add_noise(x, rng, snr_db, kind="white"):
    """Additive white/pink noise scaled to the requested SNR vs signal RMS."""
    n = len(x)
    noise = (rng.standard_normal(n) if kind == "white"
             else _pink_noise(n, rng))
    noise_rms = _rms(noise)
    if noise_rms <= 0.0:
        return x
    target_rms = _rms(x) * float(10.0 ** (-snr_db / 20.0))
    return x + noise * (target_rms / noise_rms)


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


# ------------------------------------------------------------- recipes ------
def augment_cry_window(w, rng):
    """One 1 s cry window -> one augmented variant (deterministic given rng)."""
    y = np.asarray(w, dtype=np.float64)
    if rng.random() < LOWPASS_PROB:                       # distant speaker
        y = lowpass_first_order(y, rng.uniform(LOWPASS_FC_MIN_HZ,
                                               LOWPASS_FC_MAX_HZ))
    if rng.random() < HUM_PROB:                           # mains hum
        y = add_hum(y, rng)
    r = rng.random()                                      # room noise
    if r < 0.45:
        y = add_noise(y, rng, rng.uniform(SNR_MIN_DB, SNR_MAX_DB), "white")
    elif r < NOISE_PROB:
        y = add_noise(y, rng, rng.uniform(SNR_MIN_DB, SNR_MAX_DB), "pink")
    y = gain_db(y, rng.uniform(GAIN_MIN_DB, GAIN_MAX_DB))  # volume
    return _clip_guard(y)


def augment_noncry_window(w, rng):
    """Calibration variant for non-cry windows: noise + volume only (keeps
    them recognizably non-cry under room noise; no muffling)."""
    y = np.asarray(w, dtype=np.float64)
    if rng.random() < NONCRY_NOISE_PROB:
        kind = "white" if rng.random() < 0.5 else "pink"
        y = add_noise(y, rng, rng.uniform(SNR_MIN_DB, SNR_MAX_DB), kind)
    y = gain_db(y, rng.uniform(GAIN_MIN_DB, GAIN_MAX_DB))
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


def _aug_file_block(rel_path, abs_path, label, variant_id, noncry_frac=0.5):
    """One augmented variant of one training clip -> (n, 29) float32 features.
    Fully deterministic: the RNG is seeded from the variant's cache key.
    For non-cry clips, `noncry_frac` of the windows get a calibration variant
    (Bernoulli per window, seeded with the file), the rest are left out."""
    seed_key = (f"{AUG_CACHE_VERSION}|{rel_path}|{variant_id}"
                f"|f{noncry_frac:.2f}")
    rng = np.random.default_rng(_file_seed(seed_key))
    try:
        x, fs = load_wav(abs_path)
        x = resample_to_16k(x, fs)
    except (UnsupportedAudio, Exception) as exc:      # noqa: BLE001
        print(f"  aug skip: {abs_path}: {exc}")
        return np.zeros((0, N_FEATURES), np.float32)

    if label == "cry" and rng.random() < SPEED_PROB:
        # per-FILE pitch/speed shift so a variant is a coherent re-recording
        x = speed_pitch_shift(x, rng.uniform(SPEED_MIN, SPEED_MAX))
    windows = _windows_of(x)
    feats = []
    for w in windows:
        if len(w) != WINDOW_SAMPLES:
            continue
        if label != "cry" and rng.random() >= noncry_frac:
            continue                                  # not selected this run
        aw = (augment_cry_window(w, rng) if label == "cry"
              else augment_noncry_window(w, rng))
        feats.append(extract_window(aw))
    return (np.stack(feats).astype(np.float32) if feats
            else np.zeros((0, N_FEATURES), np.float32))


def load_or_build_augmented(manifest_rows, dataset_dir,
                            n_cry_variants=2, noncry_frac=0.5,
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
                   f"{st.st_size}|{vid}")
            if key not in cache:
                cache[key] = _aug_file_block(rel_path, abs_path, label, vid,
                                             noncry_frac)
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

    y1 = augment_cry_window(w, rng)
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

    print(f"{'ALL PASS' if not failures else f'{len(failures)} FAILURE(S): ' + ', '.join(failures)}")
    return 0 if not failures else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="augment.py - wave-level "
                                 "augmentation + augmented feature cache")
    ap.add_argument("--dataset-dir", default=HERE)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--variants", type=int, default=2,
                    help="augmented variants per training cry clip")
    ap.add_argument("--noncry-frac", type=float, default=0.5,
                    help="fraction of training non-cry windows augmented")
    args = ap.parse_args()
    if args.selftest:
        return _selftest()

    import csv
    manifest = os.path.join(args.dataset_dir, "manifest.csv")
    with open(manifest, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    X, y, src, rel = load_or_build_augmented(
        rows, os.path.abspath(args.dataset_dir),
        n_cry_variants=args.variants, noncry_frac=args.noncry_frac)
    print(f"augmented windows: {X.shape[0]} (cry={int(y.sum())}, "
          f"noncry={int((1 - y).sum())})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
