#!/usr/bin/env python3
"""
gen_feature_golden.py - build the deterministic golden vector used by
test/test_features to prove PC <-> C feature parity.

It synthesizes one exactly-1 s (16000-sample) int16 window, runs the REAL
PC-side trainer front end (ml_model/extract_features.py, imported -- the
same extract_window() path train_cry_model.py uses) on it, and writes
test/test_features/feature_golden.h containing:

  golden_window[16000]     the int16 window (exactly what the device ring
                           delivers to audio_features_compute()),
  golden_features[63]      the expected 63-feature vector (float32),
  GOLDEN_FEATURE_NAMES[63] feature names in FEATURE_NAMES order,
  GOLDEN_LOG_ENERGY_MEAN / GOLDEN_ZCR_MEAN convenience defines.

Deterministic synthetic window formula (fixed constants, fixed seed):

  x[n] = 0.10*sin(2*pi*440 Hz*n/16000)      low tone at 0.10 full-scale
       + 0.05*sin(2*pi*2000 Hz*n/16000)     mid tone at 0.05 full-scale
       + noise,  noise ~ N(0, 0.005), numpy default_rng(20261105)
       quantized: int16(clip(round(x * 32768), -32768, 32767)), n = 0..15999

The two tones exercise low/mid mel bands; the broadband noise floor keeps
every filter (and every std) nonzero and finite.  Feature parity path: the
int16 window divided by 32768 (exactly what load_wav() does and what
src/audio_features.c mirrors) is fed to extract_window(); since every
int16/32768 is exactly representable in float32 AND float64, the int16
window pins the PC and device pipelines to identical inputs.

Usage (from the project root):
  C:/Users/Perch/.platformio/penv/Scripts/python.exe ml_model/gen_feature_golden.py
Then COMMIT the generated test/test_features/feature_golden.h.
"""

import os
import sys

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import extract_features as ef  # noqa: E402  (PC trainer front end)

ROOT = os.path.dirname(_HERE)
OUT_PATH = os.path.join(ROOT, "test", "test_features", "feature_golden.h")

SEED = 20261105
F1_HZ, F1_AMP = 440.0, 0.10
F2_HZ, F2_AMP = 2000.0, 0.05
NOISE_SIGMA = 0.005
QUANT_SCALE = 32768.0

PER_LINE_WINDOW = 12
PER_LINE_FEATURES = 1          # one feature per line, with its name


def make_window():
    """Deterministic int16 window (see module docstring for the formula)."""
    rng = np.random.default_rng(SEED)
    n = np.arange(ef.WINDOW_SAMPLES, dtype=np.float64)
    x = (F1_AMP * np.sin(2.0 * np.pi * F1_HZ * n / ef.FS)
         + F2_AMP * np.sin(2.0 * np.pi * F2_HZ * n / ef.FS)
         + rng.normal(0.0, NOISE_SIGMA, ef.WINDOW_SAMPLES))
    q = np.clip(np.rint(x * QUANT_SCALE), -32768.0, 32767.0)
    return q.astype(np.int16)


def f32_literal(v):
    """float32 round-trip literal (%.9g parses back to the same float32)."""
    text = "%.9g" % float(np.float32(v))
    if ("." not in text) and ("e" not in text) and ("E" not in text):
        text += ".0"                      # '0' would make an invalid '0f'
    lit = text + "f"
    if np.float32(float(lit[:-1])) != np.float32(v):
        raise AssertionError("float32 literal round-trip failed: %s" % lit)
    return lit


def build_header(window, feats):
    L = []
    ap = L.append
    ap("/**")
    ap(" * @file feature_golden.h")
    ap(" * @brief AUTO-GENERATED PC<->C feature-parity golden vector.")
    ap(" *        DO NOT EDIT BY HAND -- regenerate instead:")
    ap(" *        C:/Users/Perch/.platformio/penv/Scripts/python.exe \\")
    ap(" *            ml_model/gen_feature_golden.py")
    ap(" *")
    ap(" * Deterministic synthetic 1 s window (16 kHz, int16, n = 0..15999):")
    ap(" *   x[n] = 0.10*sin(2*pi*440*n/16000)      low tone, 0.10 full-scale")
    ap(" *        + 0.05*sin(2*pi*2000*n/16000)     mid tone, 0.05 full-scale")
    ap(" *        + N(0, 0.005) noise, numpy default_rng seed %d" % SEED)
    ap(" *   quantized: int16(clip(round(x*32768), -32768, 32767))")
    ap(" *")
    ap(" * golden_features was produced by the REAL PC trainer front end:")
    ap(" *   extract_window(golden_window.astype(float64) / 32768.0)")
    ap(" * (ml_model/extract_features.py -- the exact int16/32768 conversion")
    ap(" * the device pipeline mirrors; int16/32768 is exact in float32, so")
    ap(" * both sides start from bit-identical samples).  Feature order is")
    ap(" * FEATURE_NAMES (feature-set v2, 63 features): indices 0-28 the v1")
    ap(" * block (mfcc00_mean..mfcc12_mean, mfcc00_std..mfcc12_std,")
    ap(" * log_energy_mean, log_energy_std, zcr_mean), then the v2 temporal")
    ap(" * dynamics 29-62 (mfcc00_delta_mean..mfcc12_delta_mean,")
    ap(" * mfcc00_delta_std..mfcc12_delta_std, log_energy_delta_mean/std,")
    ap(" * spectral_centroid_mean/std, spectral_rolloff85_mean/std,")
    ap(" * spectral_flatness_mean/std).  The window is stationary plus noise,")
    ap(" * so the delta features exercise the edge-replication rule on")
    ap(" * near-constant frame sequences and the spectral features carry")
    ap(" * genuine broadband values.  test/test_features")
    ap(" * requires |C - golden| <= max(1e-3, 1e-3*|golden|) per feature.")
    ap(" */")
    ap("#ifndef FEATURE_GOLDEN_H_")
    ap("#define FEATURE_GOLDEN_H_")
    ap("")
    ap("#include <stdint.h>")
    ap("")
    ap("#define GOLDEN_WINDOW_LEN    %du" % ef.WINDOW_SAMPLES)
    ap("#define GOLDEN_FEATURE_COUNT %du" % len(ef.FEATURE_NAMES))
    ap("")

    ap("/* The synthetic window (device-int16 samples). */")
    ap("static const int16_t golden_window[GOLDEN_WINDOW_LEN] = {")
    vals = ["%d" % int(v) for v in window]
    for i in range(0, len(vals), PER_LINE_WINDOW):
        chunk = ", ".join(vals[i:i + PER_LINE_WINDOW])
        if i + PER_LINE_WINDOW < len(vals):
            chunk += ","
        ap("    " + chunk)
    ap("};")
    ap("")

    ap("/* Expected 63-feature vector (float32 literals, FEATURE_NAMES order). */")
    ap("static const float golden_features[GOLDEN_FEATURE_COUNT] = {")
    for i, name in enumerate(ef.FEATURE_NAMES):
        comma = "," if i + 1 < len(ef.FEATURE_NAMES) else ""
        ap("    /* %02d %-16s */ %s%s" % (i, name, f32_literal(feats[i]), comma))
    ap("};")
    ap("")

    ap("/* Feature names in order (for assertion messages). */")
    ap("static const char *const GOLDEN_FEATURE_NAMES[GOLDEN_FEATURE_COUNT] = {")
    for i, name in enumerate(ef.FEATURE_NAMES):
        comma = "," if i + 1 < len(ef.FEATURE_NAMES) else ""
        ap('    "%s"%s' % (name, comma))
    ap("};")
    ap("")
    ap("/* Convenience accessors for single-feature checks. */")
    ap("#define GOLDEN_LOG_ENERGY_MEAN (%s)" % f32_literal(feats[26]))
    ap("#define GOLDEN_ZCR_MEAN        (%s)" % f32_literal(feats[28]))
    ap("")
    ap("#endif /* FEATURE_GOLDEN_H_ */")
    ap("")
    return "\n".join(L)


def main():
    window = make_window()
    assert window.dtype == np.int16 and window.shape == (ef.WINDOW_SAMPLES,)

    # PC parity path: identical conversion to load_wav() (int16/32768,
    # exact) then the real trainer extract_window().
    feats = ef.extract_window(window.astype(np.float64) / 32768.0)
    assert feats.shape == (len(ef.FEATURE_NAMES),) and feats.dtype == np.float32
    assert len(ef.FEATURE_NAMES) == 63
    assert bool(np.isfinite(feats).all())

    header = build_header(window, feats)
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w", newline="\n", encoding="ascii") as f:
        f.write(header)

    print("wrote %s (%d bytes, %d lines)"
          % (OUT_PATH, len(header), header.count("\n") + 1))
    print("  window : %d int16 samples (peak %d)"
          % (len(window), int(np.abs(window).max())))
    print("  features (%d):" % len(ef.FEATURE_NAMES))
    for i, name in enumerate(ef.FEATURE_NAMES):
        print("    %02d %-16s %.7g" % (i, name, float(feats[i])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
