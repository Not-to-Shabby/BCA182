#!/usr/bin/env python3
"""
gen_c_tables.py - emit include/audio_features_tables.h, the flash-resident
coefficient tables for the on-device 63-feature MFCC front end
(src/audio_features.c).

The tables are the EXACT matrices from ml_model/extract_features.py (the
PC-side trainer front end), imported from that module so there is a single
source of truth -- no formulas are re-typed here:

  audio_feat_hann512[512]      periodic Hann window (extract_features.HANN),
                               w[k] = 0.5 - 0.5*cos(2*pi*k/512)
  audio_feat_mel_fb[40][257]   HTK triangular mel filterbank
                               (extract_features.MEL_FB),
                               mel(f) = 2595*log10(1 + f/700), 42 edges
                               linearly spaced in mel over 0..8000 Hz,
                               column k = rfft bin k (f = k*FS/512)
  audio_feat_dct_mat[13][40]   orthonormal DCT-II (extract_features.DCT_MAT),
                               D[k,n] = sqrt(2/40)*cos(pi*k*(2n+1)/80),
                               row 0 scaled by 1/sqrt(2)
                               (== scipy dct(..., norm='ortho') as a matrix)

Feature-set v2 note: the temporal-dynamics features appended after the
original 29 (MFCC deltas, log-energy delta, spectral centroid/rolloff/
flatness) need NO coefficient tables -- deltas are pure differences of the
already-computed per-frame MFCC/log-energy sequences and the spectral-shape
features are computed directly from the rfft power spectrum with the single
constant AUDIO_TABLES_BIN_HZ (rfft bin width, emitted below from
extract_features.FS / extract_features.N_FFT).

Numerical contract: every value is the round-to-nearest float32 of the
trainer's float64 matrix ("no rounding beyond float32 storage").  Each
literal is printed with %.9g, which round-trips float32 exactly; the
round-trip of every single value is verified in-process before the header
is written.

All arrays are `static const` so they land in flash on the device and are
shared by the one translation unit that includes the header
(src/audio_features.c, which is also #included by test/test_features).

Usage (from the project root):
  C:/Users/Perch/.platformio/penv/Scripts/python.exe ml_model/gen_c_tables.py
Then COMMIT the generated include/audio_features_tables.h.
"""

import os
import sys

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import extract_features as ef  # noqa: E402  (single source of truth)

ROOT = os.path.dirname(_HERE)
OUT_PATH = os.path.join(ROOT, "include", "audio_features_tables.h")

PER_LINE_1D = 8
PER_LINE_2D = 6


def _float_lit(text):
    """Ensure a %.9g decimal is a valid float literal: '0' -> '0.0'."""
    if ("." not in text) and ("e" not in text) and ("E" not in text):
        text += ".0"
    return text + "f"


def f32_literals(arr):
    """float64 matrix -> flat list of exact-round-trip float32 C literals.

    Rounds to float32 exactly once (round-to-nearest, the device storage
    format) and verifies that each %.9g literal parses back to the same
    float32 bit pattern.
    """
    src = np.asarray(arr, dtype=np.float64)
    f32 = src.astype(np.float32)                    # the one rounding step
    if not np.isfinite(f32).all():
        raise AssertionError("non-finite value in table")
    lits = [_float_lit("%.9g" % float(v)) for v in f32.reshape(-1)]
    back = np.array([float(s[:-1]) for s in lits], dtype=np.float64)
    if not np.array_equal(back.astype(np.float32), f32.reshape(-1)):
        raise AssertionError("float32 literal round-trip check failed")
    return lits, f32.shape


def _wrap(lits, per_line, indent, first_line, last_line):
    """Emit an initializer list wrapped at `per_line` values per line."""
    out = [indent + first_line]
    n = len(lits)
    for i in range(0, n, per_line):
        chunk = ", ".join(lits[i:i + per_line])
        if i + per_line < n:
            chunk += ","
        out.append(indent + "  " + chunk)
    out.append(indent + last_line)
    return out


def emit_1d(lines, comment, name, length_macro, arr, per_line=PER_LINE_1D):
    lits, shape = f32_literals(arr)
    assert shape == (len(arr),)
    lines.append("")
    lines.append(comment)
    lines.append("static const float %s[%s] = {" % (name, length_macro))
    for i in range(0, len(lits), per_line):
        chunk = ", ".join(lits[i:i + per_line])
        if i + per_line < len(lits):
            chunk += ","
        lines.append("    " + chunk)
    lines.append("};")
    return lits


def emit_2d(lines, comment, name, rows_macro, cols_macro, mat,
            row_label, per_line=PER_LINE_2D):
    lits, shape = f32_literals(mat)
    rows, cols = shape
    lines.append("")
    lines.append(comment)
    lines.append("static const float %s[%s][%s] = {"
                 % (name, rows_macro, cols_macro))
    pos = 0
    for r in range(rows):
        row_lits = lits[pos:pos + cols]
        pos += cols
        lines.append("    /* %s %02d */" % (row_label, r))
        lines.extend(_wrap(row_lits, per_line, "    ", "{", "},"))
    lines.append("};")
    return lits


def build_header(hann, mel, dct):
    L = []
    ap = L.append
    ap("/**")
    ap(" * @file audio_features_tables.h")
    ap(" * @brief AUTO-GENERATED coefficient tables for the on-device")
    ap(" *        63-feature MFCC front end (src/audio_features.c).")
    ap(" *        DO NOT EDIT BY HAND -- regenerate instead:")
    ap(" *        C:/Users/Perch/.platformio/penv/Scripts/python.exe \\")
    ap(" *            ml_model/gen_c_tables.py")
    ap(" *")
    ap(" * Source of truth : ml_model/extract_features.py (PC trainer front")
    ap(" *                   end); this header mirrors its matrices exactly.")
    ap(" * Generated with  : numpy %s" % np.__version__)
    ap(" *")
    ap(" * Numerical contract: every value is the round-to-nearest float32 of")
    ap(" * the trainer's float64 matrix (no rounding beyond float32 storage);")
    ap(" * literals are %.9g and were verified in the generator to round-trip")
    ap(" * float32 exactly.  Arrays are `static const` (flash on device) and")
    ap(" * are included by exactly one translation unit")
    ap(" * (src/audio_features.c, also compiled into test/test_features).")
    ap(" *")
    ap(" * Mirrored matrices (extract_features.py symbol in parentheses):")
    ap(" *   audio_feat_hann512[512]     periodic Hann window (HANN):")
    ap(" *                               w[k] = 0.5 - 0.5*cos(2*pi*k/512)")
    ap(" *   audio_feat_mel_fb[40][257]  HTK mel filterbank (MEL_FB):")
    ap(" *                               mel(f) = 2595*log10(1 + f/700),")
    ap(" *                               42 edges linear in mel, 0..8000 Hz,")
    ap(" *                               column k = rfft bin k (k*FS/512 Hz);")
    ap(" *                               edge filters may have zero mass")
    ap(" *                               (documented trainer behaviour)")
    ap(" *   audio_feat_dct_mat[13][40]  orthonormal DCT-II (DCT_MAT):")
    ap(" *                               D[k,n] = sqrt(2/40)*cos(pi*k*(2n+1)/80),")
    ap(" *                               row 0 scaled by 1/sqrt(2)")
    ap(" *")
    ap(" * Feature-set v2: the temporal-dynamics features (MFCC deltas,")
    ap(" * log-energy delta, spectral centroid/rolloff/flatness) need no")
    ap(" * tables -- deltas are differences of computed sequences and the")
    ap(" * spectral-shape features use only AUDIO_TABLES_BIN_HZ (the rfft bin")
    ap(" * width, bin k sits at k * BIN_HZ Hz), emitted from the trainer")
    ap(" * constants FS / N_FFT.")
    ap(" */")
    ap("#ifndef AUDIO_FEATURES_TABLES_H_")
    ap("#define AUDIO_FEATURES_TABLES_H_")
    ap("")
    ap("/* Dimensions (constants of ml_model/extract_features.py). */")
    ap("#define AUDIO_TABLES_N_FFT   512  /* N_FFT: frame length / rfft size */")
    ap("#define AUDIO_TABLES_HOP     256  /* HOP: frame hop                  */")
    ap("#define AUDIO_TABLES_N_MELS  40   /* N_MELS: mel filters             */")
    ap("#define AUDIO_TABLES_N_BINS  257  /* N_FFT/2 + 1 rfft bins           */")
    ap("#define AUDIO_TABLES_N_MFCC  13   /* N_MFCC: coefficients kept       */")
    ap("#define AUDIO_TABLES_BIN_HZ  %.10g  /* FS/N_FFT: rfft bin width [Hz] */"
       % (float(ef.FS) / float(ef.N_FFT)))

    emit_1d(
        L,
        "/* Periodic Hann window, w[k] = 0.5 - 0.5*cos(2*pi*k/512) (HANN). */",
        "audio_feat_hann512", "AUDIO_TABLES_N_FFT", hann)

    emit_2d(
        L,
        "/* 40 x 257 HTK triangular mel filterbank, row m = filter m,\n"
        " * column k = rfft bin k (MEL_FB). */",
        "audio_feat_mel_fb", "AUDIO_TABLES_N_MELS", "AUDIO_TABLES_N_BINS",
        mel, "mel filter")

    emit_2d(
        L,
        "/* Orthonormal DCT-II, D[k,n] = sqrt(2/40)*cos(pi*k*(2n+1)/80),\n"
        " * row 0 scaled by 1/sqrt(2) (DCT_MAT). */",
        "audio_feat_dct_mat", "AUDIO_TABLES_N_MFCC", "AUDIO_TABLES_N_MELS",
        dct, "dct row")

    ap("")
    ap("#endif /* AUDIO_FEATURES_TABLES_H_ */")
    ap("")
    return "\n".join(L)


def main():
    # Exact trainer matrices -- imported, never re-typed.
    hann = ef.HANN          # (512,)  float64
    mel = ef.MEL_FB         # (40, 257) float64
    dct = ef.DCT_MAT        # (13, 40) float64

    assert hann.shape == (512,), hann.shape
    assert mel.shape == (40, 257), mel.shape
    assert dct.shape == (13, 40), dct.shape
    assert float(hann[0]) == 0.0 and float(hann[511]) > 0.0
    assert bool((mel >= 0.0).all())
    assert bool(np.allclose(dct @ dct.T, np.eye(ef.N_MFCC), atol=1e-12))

    header = build_header(hann, mel, dct)
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w", newline="\n", encoding="ascii") as f:
        f.write(header)

    print("wrote %s (%d bytes, %d lines)"
          % (OUT_PATH, len(header), header.count("\n") + 1))
    print("  audio_feat_hann512 : 512 values")
    print("  audio_feat_mel_fb  : 40 x 257 "
          "(filter 0 mass bins=%d, filter 39 mass bins=%d)"
          % (int((mel[0] > 0.0).sum()), int((mel[39] > 0.0).sum())))
    print("  audio_feat_dct_mat : 13 x 40 (orthonormal verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
