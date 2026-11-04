#!/usr/bin/env python3
"""
train_cry_model.py - P3 cry/non-cry RandomForest trainer for the Smart Baby
Monitoring System (house style modeled on Lab 4's train_cycle_model.py and
Lab 3's train_model.py header-wrapping / confidence wrapper).

Pipeline:
  ml_model/manifest.csv (built by dataset_prep.py)
    -> 16 kHz 1 s windows -> 29 features via extract_features.py
       (cached in ml_model/features_cache.npz, keyed by path+mtime+size)
    -> sklearn RandomForestClassifier(n_estimators=10, max_depth=6,
       class_weight="balanced", random_state=42, n_jobs=-1)
    -> evaluation on the FROZEN test split (ESC-50 crying_baby x40 + fold-5
       non-cry subset; zero overlap with training by fold design) plus the
       val split and a confuser false-positive table (siren, helicopter,
       chainsaw, rooster, [UrbanSound8K children_playing/street_music])
    -> honest rule-based baseline row (single log-energy threshold tuned on
       the TRAIN split only)
    -> emlearn inline-C export post-processed into include/cry_model.h
       (provenance header, extern "C" guard, cry_model_predict() and
       cry_model_predict_confidence() wrappers)

CLI (defaults match the P3 protocol):
  python train_cry_model.py \
      --dataset-dir ml_model \
      --out ../include/cry_model.h \
      --report model_report.txt

Paths in manifest.csv are relative to --dataset-dir. The header is written
ONLY if the frozen test set contains >= 20 cry AND >= 20 noncry clips
(see MIN_TEST_CLIPS); otherwise the report states exactly what is missing.

Deterministic: manifest order, fixed seeds, fixed hyperparameters.
"""

import argparse
import csv
import datetime
import os
import re
import sys

import numpy as np
import sklearn
import emlearn
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import confusion_matrix, precision_recall_fscore_support

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from extract_features import (                       # noqa: E402
    FEATURE_NAMES, extract_file_windows, UnsupportedAudio)

CACHE_NAME = "features_cache.npz"
CACHE_VERSION = "v1"          # bump to invalidate a stale cache
N_TREES = 10
MAX_DEPTH = 6
SEED = 42
MIN_TEST_CLIPS = 20
LABELS = ("noncry", "cry")    # 0, 1
N_FEATURES = len(FEATURE_NAMES)
ENERGY_FEAT = FEATURE_NAMES.index("log_energy_mean")


# ------------------------------------------------------------ feature cache -
def load_cache(cache_path):
    """features_cache.npz -> {key: (n_windows, 29) float32 array}."""
    if not os.path.exists(cache_path):
        return {}
    try:
        with np.load(cache_path, allow_pickle=False) as z:
            keys = z["keys"]
            feats = z["feats_flat"]
            counts = z["counts"]
        if feats.shape[1] != N_FEATURES:
            print(f"cache feature count mismatch ({feats.shape[1]} != "
                  f"{N_FEATURES}) - rebuilding cache")
            return {}
        cache, off = {}, 0
        for k, c in zip(keys, counts):
            cache[str(k)] = feats[off:off + int(c)]
            off += int(c)
        return cache
    except Exception as exc:                          # noqa: BLE001
        print(f"cache unreadable ({exc}) - rebuilding")
        return {}


def save_cache(cache_path, cache):
    keys = list(cache.keys())
    counts = np.array([cache[k].shape[0] for k in keys], dtype=np.int64)
    flat = (np.concatenate([cache[k] for k in keys], axis=0)
            if keys else np.zeros((0, N_FEATURES), np.float32))
    tmp = cache_path + ".tmp.npz"   # np.savez* appends .npz if missing
    np.savez_compressed(tmp, keys=np.array(keys, dtype="U"),
                        counts=counts, feats_flat=flat)
    os.replace(tmp, cache_path)


def file_cache_key(rel_path, abs_path):
    st = os.stat(abs_path)
    return f"{CACHE_VERSION}|{rel_path}|{st.st_mtime_ns}|{st.st_size}"


def windows_for(cache, cache_path, rel_path, abs_path):
    """(n, 29) float32 window features for one clip, cached."""
    key = file_cache_key(rel_path, abs_path)
    if key in cache:
        return cache[key]
    try:
        wins = extract_file_windows(abs_path)
    except UnsupportedAudio as exc:
        print(f"  skip (unsupported): {exc}")
        wins = []
    except Exception as exc:                          # noqa: BLE001
        print(f"  skip (error): {abs_path}: {exc}")
        wins = []
    arr = (np.stack(wins).astype(np.float32) if wins
           else np.zeros((0, N_FEATURES), np.float32))
    cache[key] = arr
    return arr


# ------------------------------------------------------------------ metrics -
def prf(y_true, y_pred):
    """Per-class precision/recall/F1 with explicit label order."""
    p, r, f, s = precision_recall_fscore_support(
        y_true, y_pred, labels=[0, 1], zero_division=0)
    return p, r, f, s


def metrics_block(y_true, y_pred, title, lines):
    cm = confusion_matrix(y_true, y_pred, labels=[0, 1])
    p, r, f, s = prf(y_true, y_pred)
    acc = float((np.asarray(y_true) == np.asarray(y_pred)).mean())
    lines.append(f"--- {title} ---")
    lines.append("Confusion matrix (rows = truth [noncry, cry], "
                 "cols = prediction [noncry, cry]):")
    lines.append(f"                 pred noncry   pred cry")
    lines.append(f"  true noncry | {cm[0, 0]:>11} | {cm[0, 1]:>8}")
    lines.append(f"  true cry    | {cm[1, 0]:>11} | {cm[1, 1]:>8}")
    lines.append(f"{'class':<8} {'precision':>10} {'recall':>10} {'f1':>10} "
                 f"{'support':>8}")
    for i, name in enumerate(LABELS):
        lines.append(f"{name:<8} {p[i]:>10.3f} {r[i]:>10.3f} {f[i]:>10.3f} "
                     f"{s[i]:>8}")
    lines.append(f"accuracy: {acc:.3f}")
    return cm, acc


# ---------------------------------------------------------------- baseline --
def energy_baseline_threshold(X_train, y_train):
    """Single log-energy threshold maximizing balanced accuracy on TRAIN."""
    cand = np.unique(np.quantile(X_train[:, ENERGY_FEAT],
                                 np.linspace(0.0, 1.0, 101)))
    best_t, best_ba = None, -1.0
    for t in cand:
        pred = (X_train[:, ENERGY_FEAT] > t).astype(int)
        rec = [(pred[y_train == c] == c).mean() for c in (0, 1)]
        ba = float(np.mean(rec))
        if ba > best_ba:
            best_t, best_ba = float(t), ba
    return best_t, best_ba


def energy_baseline_apply(X, t):
    return (X[:, ENERGY_FEAT] > t).astype(int)


# ------------------------------------------------------------------ header --
def wrap_header(generated: str, out_path: str, provenance_lines, tree_ids,
                report_summary: str) -> None:
    """Post-process emlearn inline C into include/cry_model.h with provenance,
    an extern \"C\" guard, and the Lab-3-style prediction wrappers."""
    # Header-safe: make the file-scope aggregates static inline (Lab 3 pattern).
    generated = generated.replace("int32_t cry_forest_predict(",
                                  "static inline int32_t cry_forest_predict(")
    generated = generated.replace("int cry_forest_predict_proba(",
                                  "static inline int cry_forest_predict_proba(")
    # Zero-warning builds: emlearn's tree functions never read features_length.
    generated = re.sub(
        r"(cry_forest_tree_\d+\(const float \*features, int32_t features_length\) \{)",
        r"\1 (void)features_length;",
        generated)

    vote_lines = "\n".join(
        f"    votes_cry += cry_forest_tree_{i}(features, CRY_MODEL_NUM_FEATURES);"
        for i in tree_ids)

    content = f"""/**
 * ============================================================================
 * cry_model.h - Baby-cry / non-cry RandomForest (P3 Smart Baby Monitor)
 * ============================================================================
 * AUTO-GENERATED by ml_model/train_cry_model.py (emlearn inline export) -
 * DO NOT EDIT BY HAND.
 *
 * Model    : sklearn RandomForestClassifier, {N_TREES} trees, max_depth {MAX_DEPTH},
 *            class_weight="balanced", random_state={SEED}
 * Input    : {N_FEATURES} float features from 1 s / 16 kHz windows
 *            (MFCC 0-12 mean+std, log-energy mean/std, mean ZCR - see
 *            ml_model/extract_features.py FEATURE_NAMES for exact order)
 * Classes  : 0 = CRY_PRED_NONCRY, 1 = CRY_PRED_CRY
 * Memory   : pure inline decision trees, zero dynamic allocation
 *
 * Provenance:
{chr(10).join(' *   ' + ln for ln in provenance_lines)}
 *
 * Frozen-test summary: {report_summary}
 */

#ifndef CRY_MODEL_H
#define CRY_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {{
#endif

#define CRY_MODEL_NUM_FEATURES {N_FEATURES}
#define CRY_MODEL_NUM_TREES    {len(tree_ids)}

typedef enum {{
    CRY_PRED_NONCRY = 0,
    CRY_PRED_CRY    = 1
}} cry_class_t;

/* ---- begin emlearn-generated forest (method=inline, dtype=float) ---- */

{generated}

/* ---- end emlearn-generated forest ---- */

/**
 * @brief  Classify one 29-feature window.
 * @param  features  float[{N_FEATURES}] in FEATURE_NAMES order
 * @return 0 = noncry, 1 = cry (sklearn forest majority vote; ties -> noncry)
 */
static inline int cry_model_predict(const float *features)
{{
    return (int)cry_forest_predict(features, CRY_MODEL_NUM_FEATURES);
}}

/**
 * @brief  Classify with tree-vote confidence (Lab 3 wrapper pattern).
 * @param  features       float[{N_FEATURES}] in FEATURE_NAMES order
 * @param  out_confidence optional; voting confidence 0-100 percent
 * @return CRY_PRED_CRY on strict tree majority, else CRY_PRED_NONCRY
 *         (a {N_TREES}-vote tie resolves to NONCRY, mirroring sklearn argmax)
 */
static inline cry_class_t cry_model_predict_confidence(const float *features,
                                                       uint8_t *out_confidence)
{{
    int votes_cry = 0;
{vote_lines}

    if (votes_cry * 2 > CRY_MODEL_NUM_TREES) {{
        if (out_confidence) {{
            *out_confidence = (uint8_t)((votes_cry * 100) / CRY_MODEL_NUM_TREES);
        }}
        return CRY_PRED_CRY;
    }}
    if (out_confidence) {{
        *out_confidence =
            (uint8_t)(((CRY_MODEL_NUM_TREES - votes_cry) * 100) / CRY_MODEL_NUM_TREES);
    }}
    return CRY_PRED_NONCRY;
}}

#ifdef __cplusplus
}}
#endif

#endif /* CRY_MODEL_H */
"""
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)


# -------------------------------------------------------------------- main --
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dataset-dir", default=HERE,
                    help="directory containing manifest.csv (default: ml_model)")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "include",
                                                  "cry_model.h"),
                    help="output C header path (default ../include/cry_model.h)")
    ap.add_argument("--report", default=os.path.join(HERE, "model_report.txt"),
                    help="report path (default ml_model/model_report.txt)")
    args = ap.parse_args()

    def resolve_here(p):
        """Relative paths resolve against this script's dir (ml_model/),
        not the caller's CWD - documented CLI contract."""
        return p if os.path.isabs(p) else os.path.normpath(os.path.join(HERE, p))

    dataset_dir = os.path.abspath(args.dataset_dir)
    out_path = resolve_here(args.out)
    report_path = resolve_here(args.report)
    cache_path = os.path.join(dataset_dir, CACHE_NAME)
    manifest_path = os.path.join(dataset_dir, "manifest.csv")

    lines = []
    lines.append("=== Smart Baby Monitor - P3 cry/non-cry model report ===")
    lines.append(f"generated : {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}")
    lines.append(f"python {sys.version.split()[0]}, numpy {np.__version__}, "
                 f"scikit-learn {sklearn.__version__}, emlearn {emlearn.__version__}")
    lines.append(f"model     : RandomForestClassifier(n_estimators={N_TREES}, "
                 f"max_depth={MAX_DEPTH}, class_weight='balanced', "
                 f"random_state={SEED}, n_jobs=-1)")
    lines.append("features  : " + ", ".join(FEATURE_NAMES))
    lines.append("")

    # dataset provenance sidecar written by dataset_prep.py
    prov_path = os.path.join(dataset_dir, "dataset_provenance.txt")
    if os.path.exists(prov_path):
        with open(prov_path, encoding="utf-8") as f:
            lines.append("--- dataset provenance (dataset_provenance.txt) ---")
            lines.extend(f.read().rstrip().splitlines())
        lines.append("")

    # 1. manifest ------------------------------------------------------------
    if not os.path.exists(manifest_path):
        print(f"ERROR: {manifest_path} not found - run dataset_prep.py first")
        return 1
    with open(manifest_path, newline="", encoding="utf-8") as f:
        rows = [r for r in csv.DictReader(f)]
    print(f"manifest: {len(rows)} rows")

    cache = load_cache(cache_path)
    splits = {s: [] for s in ("train", "val", "test", "confuser")}
    n_missing, n_zero_win = 0, 0
    for i, r in enumerate(rows):
        rel, label, split, src = (r["path"], r["label"], r["split"],
                                  r["source_id"])
        if split not in splits:
            continue
        abs_path = os.path.join(dataset_dir, rel)
        if not os.path.exists(abs_path):
            n_missing += 1
            continue
        w = windows_for(cache, cache_path, rel, abs_path)
        if i % 250 == 0:
            save_cache(cache_path, cache)      # resilient incremental save
            print(f"  ... {i + 1}/{len(rows)} rows (cache saved)")
        if w.shape[0] == 0:
            n_zero_win += 1
            continue
        y = 1 if label == "cry" else 0
        splits[split].append((w, y, src, rel))
    save_cache(cache_path, cache)
    if n_missing:
        print(f"WARNING: {n_missing} manifest files missing on disk (skipped)")
    if n_zero_win:
        print(f"WARNING: {n_zero_win} clips produced no 1 s window (too short)")

    counts_txt = []
    for s in ("train", "val", "test", "confuser"):
        wins = [x for (w, _y, _s, _r) in splits[s] for x in range(w.shape[0])]
        n_cry_clip = sum(1 for (_w, y, _s, _r) in splits[s] if y == 1)
        n_nc_clip = len(splits[s]) - n_cry_clip
        n_cry_win = sum(1 for (w, y, _s, _r) in splits[s] if y == 1
                        for _ in range(w.shape[0]))
        n_nc_win = len(wins) - n_cry_win
        counts_txt.append(f"  {s:<9} clips cry/noncry {n_cry_clip}/{n_nc_clip}"
                          f"   windows cry/noncry {n_cry_win}/{n_nc_win}")
    lines.append("--- split counts (1 s windows) ---")
    lines.extend(counts_txt)
    print("\n".join(counts_txt))

    # 2. training ------------------------------------------------------------
    if not splits["train"]:
        lines.append("NO training data - aborting (run dataset_prep.py)")
        print("ERROR: no training rows available")
        _write_report(report_path, lines)
        return 1
    Xtr = np.concatenate([w for (w, _y, _s, _r) in splits["train"]])
    ytr = np.array([y for (w, y, _s, _r) in splits["train"]
                    for _ in range(w.shape[0])])
    if len(set(ytr.tolist())) < 2:
        lines.append("TRAIN split has a single class - cannot train")
        _write_report(report_path, lines)
        return 1
    print(f"\ntraining on {Xtr.shape[0]} windows "
          f"(cry={int(ytr.sum())}, noncry={int((1 - ytr).sum())}) ...")
    clf = RandomForestClassifier(n_estimators=N_TREES, max_depth=MAX_DEPTH,
                                 class_weight="balanced", random_state=SEED,
                                 n_jobs=-1)
    clf.fit(Xtr, ytr)

    def split_xy(split):
        X = np.concatenate([w for (w, _y, _s, _r) in splits[split]]) \
            if splits[split] else np.zeros((0, N_FEATURES), np.float32)
        y = np.array([y for (w, y, _s, _r) in splits[split]
                      for _ in range(w.shape[0])])
        return X, y

    # 3. val + frozen test ----------------------------------------------------
    for split, title in (("val", "VALIDATION split"),
                         ("test", "FROZEN TEST split (never seen in training)")):
        X, y = split_xy(split)
        if X.shape[0] == 0:
            lines.append(f"--- {title}: EMPTY ---")
            continue
        pred = clf.predict(X)
        lines.append("")
        metrics_block(y, pred, title, lines)

    # 4. rule-based baseline (honest comparison) ------------------------------
    base_t, base_ba = energy_baseline_threshold(Xtr, ytr)
    lines.append("")
    lines.append(f"--- rule-based baseline: log-energy > {base_t:.6f} "
                 f"(threshold tuned on TRAIN, balanced acc {base_ba:.3f}) ---")
    Xte, yte = split_xy("test")
    if Xte.shape[0]:
        metrics_block(yte, energy_baseline_apply(Xte, base_t),
                      "baseline on FROZEN TEST split", lines)

    # 5. confuser false-positive table ----------------------------------------
    Xcf, ycf = split_xy("confuser")
    lines.append("")
    lines.append("--- confuser false-positive table (split=confuser) ---")
    if Xcf.shape[0]:
        pred_cf = clf.predict(Xcf)
        pred_base = energy_baseline_apply(Xcf, base_t)
        groups = {}
        for row, pv, pb, (_w, _y, src, _r) in zip(Xcf, pred_cf, pred_base,
                                                  splits["confuser"]):
            g = src.split(":")[1] if ":" in src else "unknown"
            grp = groups.setdefault(g, {"w": 0, "fp": 0, "fpb": 0,
                                        "clips": set(), "clips_fp": set()})
            grp["w"] += 1
            grp["fp"] += int(pv == 1)
            grp["fpb"] += int(pb == 1)
            grp["clips"].add(src)
            if pv == 1:
                grp["clips_fp"].add(src)
        lines.append(f"{'category':<18} {'clips':>6} {'windows':>8} "
                     f"{'FP windows':>11} {'FP rate':>8} {'FP clips':>9} "
                     f"{'baseline FP':>12}")
        for g in sorted(groups):
            grp = groups[g]
            lines.append(f"{g:<18} {len(grp['clips']):>6} {grp['w']:>8} "
                         f"{grp['fp']:>11} {grp['fp'] / grp['w']:>8.3f} "
                         f"{len(grp['clips_fp']):>9} {grp['fpb']:>12}")
    else:
        lines.append("(confuser split empty - ESC-50 not cloned?)")

    # 6. export gate ------------------------------------------------------------
    Xte, yte = split_xy("test")
    n_cry_clips = sum(1 for (_w, y, _s, _r) in splits["test"] if y == 1)
    n_ncry_clips = len(splits["test"]) - n_cry_clips
    n_cry_wins = int(yte.sum()) if Xte.shape[0] else 0
    n_ncry_wins = int((1 - yte).sum()) if Xte.shape[0] else 0
    lines.append("")
    ok = (n_cry_clips >= MIN_TEST_CLIPS and n_ncry_clips >= MIN_TEST_CLIPS
          and n_cry_wins >= MIN_TEST_CLIPS and n_ncry_wins >= MIN_TEST_CLIPS)
    header_written = False
    if ok:
        print("\nemlearn inline export ...")
        cmodel = emlearn.convert(clf, method="inline", dtype="float")
        tmp_header = out_path + ".tmp_eml"
        cmodel.save(name="cry_forest", file=tmp_header)
        with open(tmp_header, encoding="utf-8") as f:
            generated = f.read()
        os.remove(tmp_header)
        tree_ids = sorted({int(m) for m in
                           re.findall(r"cry_forest_tree_(\d+)\s*\(", generated)})
        if len(tree_ids) != N_TREES:
            print(f"ERROR: expected {N_TREES} tree functions in emlearn output, "
                  f"found {len(tree_ids)} - header NOT written")
            lines.append(f"EXPORT FAILED: {len(tree_ids)} tree functions found")
        else:
            test_acc = float((clf.predict(Xte) == yte).mean())
            summary = (f"test acc {test_acc:.3f} over {Xte.shape[0]} windows "
                       f"({n_cry_clips} cry + {n_ncry_clips} noncry clips, "
                       f"frozen ESC-50 protocol)")
            prov_lines = [
                f"generated {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}",
                f"numpy {np.__version__} / scikit-learn {sklearn.__version__} / emlearn {emlearn.__version__}",
                f"train: donateacry-corpus (ODbL) + ESC-50 folds 1-3 non-cry (CC BY-NC 3.0)",
                f"frozen test: ESC-50 crying_baby x{n_cry_clips} + fold-5 non-cry x{n_ncry_clips}",
                f"features ({N_FEATURES}): {', '.join(FEATURE_NAMES)}",
            ]
            wrap_header(generated, out_path, prov_lines, tree_ids, summary)
            header_written = True
            size = os.path.getsize(out_path)
            lines.append(f"EXPORT: wrote {out_path} ({size} bytes, "
                         f"{N_TREES} trees x depth <= {MAX_DEPTH}, "
                         f"{N_FEATURES} features)")
            print(f"wrote {out_path} ({size} bytes)")
    else:
        missing = (f"test cry clips {n_cry_clips}/{MIN_TEST_CLIPS}, "
                   f"test noncry clips {n_ncry_clips}/{MIN_TEST_CLIPS}, "
                   f"test cry windows {n_cry_wins}/{MIN_TEST_CLIPS}, "
                   f"test noncry windows {n_ncry_wins}/{MIN_TEST_CLIPS}")
        lines.append(f"EXPORT SKIPPED: frozen test set too small - missing: "
                     f"{missing}")
        print(f"\nEXPORT SKIPPED - missing: {missing}")

    _write_report(report_path, lines)
    print(f"\nreport: {report_path}")
    return 0


def _write_report(path, lines):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    sys.exit(main())
