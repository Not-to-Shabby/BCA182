#!/usr/bin/env python3
"""
train_cycle_model.py - Laboratory 4 Appliance Cycle Monitor model trainer.

Two operating modes:

1. BOOTSTRAP (default, no arguments):
   Generates *synthetic* appliance vibration windows (documented as synthetic -
   never quoted as product accuracy) so the full firmware chain compiles, runs
   and is testable before real appliance data exists.

2. REAL DATA:
   python train_cycle_model.py --real capture1.csv capture2.csv ...
   Reads labeled CSV streams produced by the firmware's service-mode COLLECT
   (columns: ax,ay,az,gx,gy,gz,label ; 100 Hz), slices 200-sample windows,
   trains the same forest, and rewrites ml_model/cycle_model_eml.h.

Pipeline (both modes):
   windows -> 14 features (exact C implementation semantics) ->
   sklearn RandomForestClassifier(10 trees, depth 8) ->
   emlearn inline-C export (ml_model/cycle_model_eml.h) ->
   metrics report (model vs rule-based baseline, held-out split).
"""

import argparse
import os
import sys

import numpy as np
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import confusion_matrix, classification_report
from sklearn.model_selection import train_test_split
import emlearn

FS = 100.0
WINDOW = 200
FEATURES = 14
CLASSES = ["IDLE", "WASH", "SPIN"]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_HEADER = os.path.join(HERE, "cycle_model_eml.h")
REPORT = os.path.join(HERE, "model_report.txt")

# ---------------------------------------------------------------- features --
def extract_features(win: np.ndarray) -> np.ndarray:
    """Mirror of src/vibration_features.c (200 x 6 window -> 14 features)."""
    ax, ay, az, gx, gy, gz = win.T
    mag = np.sqrt(ax * ax + ay * ay + az * az)
    gyr = np.sqrt(gx * gx + gy * gy + gz * gz)

    mm = mag - mag.mean()
    gm = gyr - gyr.mean()
    n = len(win)

    rms = np.sqrt(np.mean(mm * mm))
    p2p = mag.max() - mag.min()
    crest = np.abs(mm).max() / rms if rms > 1e-6 else 0.0
    std_x = ax.std()
    std_y = ay.std()
    std_z = az.std()
    signs = np.signbit(mm)
    zcr = np.mean(signs[1:] != signs[:-1])

    power = np.zeros(51)
    for f in range(1, 51):
        # bin f analyzes true frequency f Hz: f cycles/window at 100 Hz over 200 samples
        w = 2.0 * np.pi * f * (2.0 / n)
        coeff = 2.0 * np.cos(w)
        s1 = s2 = 0.0
        for x in mm:
            s0 = x + coeff * s1 - s2
            s2, s1 = s1, s0
        power[f] = s1 * s1 + s2 * s2 - coeff * s1 * s2

    total = power[1:].sum()
    dom = int(power[1:].argmax()) + 1
    low = power[1:4].sum() / total if total > 0 else 0.0
    mid = power[4:11].sum() / total if total > 0 else 0.0
    high = power[11:51].sum() / total if total > 0 else 0.0
    lo, hi = 0.8 * dom, 1.2 * dom
    conc = power[int(np.ceil(lo)):int(np.floor(hi)) + 1].sum() / total if total > 0 else 0.0

    return np.array([rms, p2p, crest, std_x, std_y, std_z, zcr,
                     low, mid, high, float(dom), gm.std(),
                     gyr.max() - gyr.min(), conc], dtype=np.float32)

def windowize(sig: np.ndarray, label: int) -> list:
    rows = []
    for start in range(0, len(sig) - WINDOW + 1, WINDOW):
        rows.append((extract_features(sig[start:start + WINDOW]), label))
    return rows

# ------------------------------------------------------- synthetic windows --
def synth_idle(rng):
    n = rng.integers(2, 6) * WINDOW
    t = np.arange(n) / FS
    noise = rng.normal(0, 0.0035, n)
    bump = 0.02 * np.exp(-np.minimum(((t - rng.uniform(0.3, n / FS - 0.3)) ** 2) / 0.02, 300.0))
    a = noise + bump
    g = [rng.normal(0, 0.002, n) for _ in range(3)]
    return np.column_stack([a, 0.5 * a, 0.7 * a, g[0], g[1], g[2]]), 0

def synth_wash(rng):
    n = rng.integers(4, 10) * WINDOW
    t = np.arange(n) / FS
    f = rng.uniform(1.2, 2.8)                      # agitation slosh
    amp = rng.uniform(0.08, 0.25)
    slosh = amp * np.sin(2 * np.pi * f * t) * (0.7 + 0.3 * np.sin(2 * np.pi * 0.3 * t))
    harmonics = 0.3 * amp * np.sin(2 * np.pi * 2 * f * t + 1.0)
    motor = 0.02 * np.sin(2 * np.pi * rng.uniform(4, 9) * t)
    noise = rng.normal(0, 0.012, n)
    a = slosh + harmonics + motor + noise
    g = [rng.normal(0, 0.05, n) for _ in range(3)]
    return np.column_stack([a, 0.8 * a, 0.9 * a, g[0], g[1], g[2]]), 1

def synth_spin(rng):
    n = rng.integers(4, 10) * WINDOW
    t = np.arange(n) / FS
    f = rng.uniform(10.0, 25.0)                    # extract resonance
    amp = rng.uniform(0.15, 0.50)
    sig = amp * np.sin(2 * np.pi * f * t)
    sig += 0.4 * amp * np.sin(2 * np.pi * 2 * f * t + 0.7)
    sig += rng.normal(0, 0.03, n)
    wobble = rng.normal(0, 0.4, n)                 # unbalanced-load gyro noise
    return np.column_stack([sig, 0.9 * sig, 1.1 * sig, wobble, wobble, wobble]), 2

def make_synthetic():
    rng = np.random.default_rng(182)
    rows = []
    for gen in (synth_idle, synth_wash, synth_spin):
        for _ in range(60):                        # ~600 windows/class
            sig, label = gen(rng)
            rows.extend(windowize(sig, label))
    X = np.stack([r[0] for r in rows])
    y = np.array([r[1] for r in rows])
    return X, y

# ------------------------------------------------------------ real CSV data --
def load_real(paths):
    """Parse firmware COLLECT-mode console captures.

    The board streams raw ICM-20608 counts (integers): accel in LSB (4096/g),
    gyro in LSB (32.8 per deg/s). Comment lines start with '#'. Convert to the
    engineering units the feature extractor expects (g, rad/s).
    """
    rows = []
    for path in paths:
        data = []
        for line in open(path):
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("["):
                continue
            parts = line.split(",")
            if len(parts) < 7:
                continue
            try:
                vals = [float(p) for p in parts[:7]]
            except ValueError:
                continue
            data.append(vals)
        if not data:
            continue
        raw = np.array(data)
        raw[:, 0:3] /= 4096.0                                  # accel -> g
        raw[:, 3:6] = raw[:, 3:6] / 32.8 * np.pi / 180.0       # gyro -> rad/s

        for seg_start in range(0, len(raw) - WINDOW + 1, WINDOW):
            seg = raw[seg_start:seg_start + WINDOW, :6]
            labels = raw[seg_start:seg_start + WINDOW, 6].astype(int)
            if labels.min() != labels.max():
                continue                            # label changed inside window
            rows.extend(windowize(seg, int(labels[0])))
    X = np.stack([r[0] for r in rows])
    y = np.array([r[1] for r in rows])
    return X, y

# ----------------------------------------------------------------- baseline --
def baseline_classify(X):
    out = np.zeros(len(X), dtype=int)
    idle, wash, spin = X[:, 0] < 0.020, None, (X[:, 10] >= 8.0) & (X[:, 0] >= 0.080)
    out[~spin] = 1                                   # WASH by elimination
    out[idle] = 0
    out[spin] = 2
    return out

# --------------------------------------------------------------------- main --
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--real", nargs="*", default=None,
                    help="firmware COLLECT-mode CSV files (replaces synthetic data)")
    args = ap.parse_args()

    if args.real:
        X, y = load_real(args.real)
        source = f"REAL appliance captures: {', '.join(os.path.basename(p) for p in args.real)}"
    else:
        X, y = make_synthetic()
        source = "SYNTHETIC bootstrap data (not valid as product accuracy)"

    Xtr, Xte, ytr, yte = train_test_split(X, y, test_size=0.2, stratify=y, random_state=42)

    forest = RandomForestClassifier(n_estimators=10, max_depth=8, random_state=182)
    forest.fit(Xtr, ytr)

    pred = forest.predict(Xte)
    base = baseline_classify(Xte)

    lines = []
    lines.append(f"Training set : {source}")
    lines.append(f"Windows      : {len(X)} total, {len(Xte)} held out")
    lines.append(f"Class counts : {dict(zip(*np.unique(y, return_counts=True)))}")
    lines.append("")
    lines.append("=== Random Forest (10 trees, depth 8) - held-out confusion matrix ===")
    lines.append(str(confusion_matrix(yte, pred)))
    lines.append(classification_report(yte, pred, target_names=CLASSES, zero_division=0))
    lines.append("=== Rule-based baseline - held-out confusion matrix ===")
    lines.append(str(confusion_matrix(yte, base)))
    lines.append(classification_report(yte, base, target_names=CLASSES, zero_division=0))
    acc_f = float((pred == yte).mean())
    acc_b = float((base == yte).mean())
    lines.append(f"Forest accuracy {acc_f:.3f} vs baseline {acc_b:.3f}")

    # emlearn inline-C export (save() needs a path; prepend provenance afterwards)
    wrapper = emlearn.convert(forest, method="inline", dtype="float")
    tmp_path = OUT_HEADER + ".tmp"
    wrapper.save(name="cycle_forest", file=tmp_path)
    with open(tmp_path) as f:
        generated = f.read()
    os.remove(tmp_path)
    with open(OUT_HEADER, "w") as f:
        f.write("/* Auto-generated by ml_model/train_cycle_model.py - do not edit */\n")
        f.write(f"/* Trained on: {source} */\n")
        f.write(generated)

    text = "\n".join(lines)
    with open(REPORT, "w") as f:
        f.write(text + "\n")
    print(text)
    print(f"\nWrote {OUT_HEADER} and {REPORT}")


if __name__ == "__main__":
    main()
