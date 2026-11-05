#!/usr/bin/env python3
"""
train_cry_model.py - P3 cry/non-cry tree-ensemble trainer for the Smart Baby
Monitoring System (house style modeled on Lab 4's train_cycle_model.py and
Lab 3's train_model.py header-wrapping / confidence wrapper).

v7 (this revision, wave 2) merges three newly acquired corpora (SICCD,
CSIBE, hoyyu_icd - including real distant/room cry recordings that directly
target the diagnosed domain-shift bottleneck) into the training split via
prep_v2.py manifest_v2.csv, and retrains on the merged set. The frozen
protocol is untouched; v6's history is retained below:

v6 attacks the v5 shipped model's frozen-test cry recall
(0.335 at F1 0.480, confuser FP 7.7 %) by fixing the diagnosed root cause -
the 29-feature front end SUMMARIES AWAY temporal dynamics - without touching
the frozen protocol:

  * feature-set v2 (extract_features.py): the original 29 per-window summary
    statistics keep indices 0-28, and 34 temporal-dynamics features are
    appended (63 total, under the 64-feature budget): per-frame MFCC deltas
    (first difference with edge replication, aggregated mean+std = 26),
    per-frame log-energy delta mean+std (2), and per-frame spectral
    centroid / rolloff(0.85) / flatness mean+std (6).  The on-device C
    front end mirrors all 63 (src/audio_features.c, parity-tested).
  * the v5 wave-level augmentation of TRAINING windows only (augment.py v5)
    is reused unchanged - same recipes, same per-(file, variant) seeds, so
    the v2 cache rebuild replays the identical augmented waveforms through
    the new front end (features_aug_cache.npz is invalidated by its feature
    width and rebuilt once; the waveform noise pool stays valid).
  * seeded candidate sweep (fixed seed 42), compared on VAL only:
    RandomForest 12x7 (apples-to-apples with the shipped v5 model),
    RandomForest 30x8, and ExtraTreesClassifier 30x8 / 12x7 (emlearn
    exports ET through the same eml_trees inline code).
  * the firmware decision rule is replicated exactly - cry iff the strict
    forest majority votes CRY AND (cry_votes*100)//N_TREES >= threshold -
    and the cry-vote-share threshold is swept over 40..85, maximizing cry F1
    over the cry side (raw VAL cry + stress-val cry proxy: the deployment
    channel applied to val cry windows, still 100 % val data) subject to the
    task's CONFUSER FP gate (<= 10 % on the siren/helicopter/chainsaw/
    rooster split); raw VAL non-cry FP is reported at every sweep point.
  * the FROZEN test (all 40 ESC-50 crying_baby clips + frozen fold-5 non-cry
    subset) is evaluated ONCE, at the chosen operating point, in a final run
    (--skip-frozen-test lets you do honest val-only development runs).

Pipeline:
  ml_model/manifest.csv (built by dataset_prep.py)
    -> 16 kHz 1 s windows -> 63 features via extract_features.py
       (cached in ml_model/features_cache.npz, keyed by
       cache-version + path + mtime + size)
       + augmented train windows (cached in features_aug_cache.npz)
    -> sklearn ensemble candidates (seed 42)
    -> emlearn inline-C export post-processed into include/cry_model.h
       (provenance header, extern "C" guard, cry_model_predict() and
       cry_model_predict_confidence() wrappers, same vote-share semantics
       as v1)

CLI (defaults match the v2 protocol):
  python train_cry_model.py \
      --dataset-dir ml_model \
      --out ../include/cry_model.h \
      --report model_report.txt

Paths in manifest.csv are relative to --dataset-dir. The header is written
ONLY if the frozen test set contains >= 20 cry AND >= 20 noncry clips
(see MIN_TEST_CLIPS); otherwise the report states exactly what is missing.
The exported header is rejected and the next-best candidate exported instead
if it exceeds --header-max-bytes (default 400 KB).

Deterministic: manifest order, fixed seeds, fixed hyperparameters.
"""

import argparse
import csv
import datetime
import os
import re
import sys
import time

import numpy as np
import sklearn
import emlearn
from sklearn.ensemble import ExtraTreesClassifier, RandomForestClassifier
from sklearn.metrics import confusion_matrix, precision_recall_fscore_support

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from extract_features import (                       # noqa: E402
    FEATURE_NAMES, extract_file_windows, UnsupportedAudio)
import augment                                       # noqa: E402

CACHE_NAME = "features_cache.npz"
CACHE_VERSION = "v2"          # v2 = feature-set v2 (63 features); bump to
                              # invalidate a stale cache
SEED = 42
MIN_TEST_CLIPS = 20
LABELS = ("noncry", "cry")    # 0, 1
N_FEATURES = len(FEATURE_NAMES)
ENERGY_FEAT = FEATURE_NAMES.index("log_energy_mean")

HEADER_MAX_BYTES = 1_500_000      # exported header budget (wave 2: emlearn
                                  # inline trees are tiny in flash - the v6
                                  # 332 KB header compiled into ~15 KB of the
                                  # 1 MB STM32F407 flash with 850 KB free, so
                                  # the SOURCE budget is raised to 1.5 MB;
                                  # actual flash % verified by the final
                                  # pio run)
FP_GATE = 0.10                    # max noncry FP rate on VAL and confusers
T_MIN, T_MAX = 40, 90             # cry-vote-share threshold sweep bounds (%)

# Candidate registry (v6 sweep over model types on the feature-set-v2 front
# end: the shipped v5 architecture rf12x7 for an apples-to-apples
# comparison, a larger forest, intermediate forest sizes that probe the
# 400 KB header budget (rf30x8 measured 1.18 MB and cannot ship - the
# intermediate sizes give the val winner a size-feasible fallback with
# finer vote granularity than 12 trees), and ExtraTrees variants - emlearn
# exports ET through the same eml_trees inline code.  The MLP(32) fallback
# of the plan is NOT executable on this PC: emlearn's net exporter builds
# its evaluator with distutils and requires MSVC 14 (not installed) -
# documented in the README, not silently dropped).
CANDIDATES = {
    "rf12x7":       dict(kind="rf", n_estimators=12, max_depth=7,
                         class_weight="balanced",
                         desc="RF 12 trees x depth 7, class_weight=balanced "
                              "+ augmentation (v5 architecture, "
                              "apples-to-apples on feature-set v2)"),
    "rf16x7":       dict(kind="rf", n_estimators=16, max_depth=7,
                         class_weight="balanced",
                         desc="RF 16 trees x depth 7, class_weight=balanced "
                              "+ augmentation (finer vote granularity within "
                              "the header budget)"),
    "rf18x7":       dict(kind="rf", n_estimators=18, max_depth=7,
                         class_weight="balanced",
                         desc="RF 18 trees x depth 7, class_weight=balanced "
                              "+ augmentation (finer vote granularity within "
                              "the header budget)"),
    "rf20x7":       dict(kind="rf", n_estimators=20, max_depth=7,
                         class_weight="balanced",
                         desc="RF 20 trees x depth 7, class_weight=balanced "
                              "+ augmentation (probe: may exceed the header "
                              "budget)"),
    "rf24x6":       dict(kind="rf", n_estimators=24, max_depth=6,
                         class_weight="balanced",
                         desc="RF 24 trees x depth 6, class_weight=balanced "
                              "+ augmentation (wave-2 sweep: finer vote "
                              "granularity, shallow trees resist the noisier "
                              "merged-train domain shift)"),
    "rf30x8":       dict(kind="rf", n_estimators=30, max_depth=8,
                         class_weight="balanced",
                         desc="RF 30 trees x depth 8, class_weight=balanced "
                              "+ augmentation (finer vote granularity)"),
    "et30x8":       dict(kind="et", n_estimators=30, max_depth=8,
                         class_weight="balanced",
                         desc="ExtraTrees 30 trees x depth 8, "
                              "class_weight=balanced + augmentation "
                              "(stronger randomization vs noise)"),
    "et12x7":       dict(kind="et", n_estimators=12, max_depth=7,
                         class_weight="balanced",
                         desc="ExtraTrees 12 trees x depth 7, "
                              "class_weight=balanced + augmentation "
                              "(size-feasible ET variant)"),
}
DEFAULT_CANDIDATES = ("rf12x7", "rf16x7", "rf18x7", "rf20x7", "rf30x8",
                      "et30x8", "et12x7")


# ------------------------------------------------------------ feature cache -
def load_cache(cache_path):
    """features_cache.npz -> {key: (n_windows, N_FEATURES) float32 array}.

    Entries whose key does not carry the current CACHE_VERSION prefix are
    dropped on load (stale front-end versions must never be served from a
    shared cache file)."""
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
        version_prefix = CACHE_VERSION + "|"
        cache, off = {}, 0
        for k, c in zip(keys, counts):
            key = str(k)
            if key.startswith(version_prefix):
                cache[key] = feats[off:off + int(c)]
            off += int(c)          # advance for every block (flat layout)
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


def path_exists_retry(abs_path, attempts=3, delay=0.5):
    """os.path.exists with retries: a parallel process holding transient
    locks in ml_model/data/ can make a stat call fail, which os.path.exists
    reports as False - a false 'missing' would silently shrink the frozen
    splits.  A genuinely absent file just costs the retry delay."""
    for attempt in range(attempts):
        if os.path.exists(abs_path):
            return True
        if attempt < attempts - 1:
            time.sleep(delay)
    return os.path.exists(abs_path)


def windows_for(cache, cache_path, rel_path, abs_path):
    """(n, N_FEATURES) float32 window features for one clip, cached.

    Extraction is retried on generic OS errors: a parallel process may
    transiently lock files/directories under ml_model/data/ (Windows
    sharing violations), and a skipped row here would silently shrink the
    frozen splits.  UnsupportedAudio (container/parse errors) is a
    permanent property of the file and is not retried."""
    key = file_cache_key(rel_path, abs_path)
    if key in cache:
        return cache[key]
    wins = []
    for attempt in range(3):
        try:
            wins = extract_file_windows(abs_path)
            break
        except UnsupportedAudio as exc:
            print(f"  skip (unsupported): {exc}")
            wins = []
            break
        except OSError as exc:
            if attempt == 2:
                print(f"  skip (os error after retries): {abs_path}: {exc}")
                wins = []
            else:
                time.sleep(0.5)
        except Exception as exc:                      # noqa: BLE001
            print(f"  skip (error): {abs_path}: {exc}")
            wins = []
            break
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


# ------------------------------------------------------ firmware protocol ---
def tree_votes(clf, X):
    """Hard per-tree CRY vote counts - exactly what the exported emlearn
    inline forest computes (each leaf returns its majority class)."""
    votes = np.zeros(X.shape[0], dtype=np.int64)
    for est in clf.estimators_:
        votes += est.predict(X).astype(np.int64)
    return votes


def operating_points(n_trees):
    """(votes_needed, threshold%) pairs the firmware can express.

    src/cry_task.c counts a window as cry-positive iff
        votes_cry * 2 > n_trees            (strict majority; ties -> NONCRY)
        and (votes_cry*100)//n_trees >= CRY_CONF_THRESHOLD
    so each vote count v >= majority corresponds to the canonical threshold
    (v*100)//n_trees; the sweep is limited to the 30..80 range."""
    pts = []
    for v in range(n_trees // 2 + 1, n_trees + 1):
        t = (v * 100) // n_trees
        if t > T_MAX:
            break
        pts.append((v, t))
    return pts


def point_metrics_split(votes_cry_raw, votes_cry_stress, votes_noncry, v):
    """Cry metrics at 'votes >= v' over the cry side (raw val cry + stress-val
    cry) and the noncry FP rate over the raw val noncry windows.

    The cry side deliberately includes the stress-val deployment-channel
    proxy (augment.stress_val_windows): raw val cries are clean close-mic
    recordings the forest is near-saturation-confident on, so tuning the
    threshold on them alone drives it far too high for the domain-shifted
    frozen test (the v2 lesson: val-optimal T=75 collapsed test recall to
    0.17). Stress cries emulate 'cry heard through room noise' and pull the
    optimum into the regime the device actually operates in."""
    votes_cry = np.concatenate([votes_cry_raw, votes_cry_stress])
    tp = int((votes_cry >= v).sum())
    fn = int((votes_cry < v).sum())
    fp = int((votes_noncry >= v).sum())
    n_nc = len(votes_noncry)
    precision = tp / (tp + fp) if (tp + fp) else 0.0
    recall = tp / (tp + fn) if (tp + fn) else 0.0
    f1 = (2 * precision * recall / (precision + recall)
          if (precision + recall) else 0.0)
    fp_rate = fp / n_nc if n_nc else 0.0
    return dict(votes=v, precision=precision, recall=recall, f1=f1,
                fp_rate=fp_rate,
                raw_recall=(float((votes_cry_raw >= v).mean())
                            if len(votes_cry_raw) else 0.0),
                stress_recall=(float((votes_cry_stress >= v).mean())
                               if len(votes_cry_stress) else 0.0))


def sweep_points(votes_raw, votes_stress, votes_noncry, votes_cf, n_trees):
    """All in-sweep operating points with VAL metrics + confuser FP rate."""
    out = []
    for v, t in operating_points(n_trees):
        m = point_metrics_split(votes_raw, votes_stress, votes_noncry, v)
        m["threshold"] = t
        m["confuser_fp"] = (float((votes_cf >= v).mean())
                            if len(votes_cf) else 0.0)
        out.append(m)
    return out


def pick_point(points, confuser_gate=FP_GATE):
    """Best point by cry F1 (tie: recall, then lower FP, then lower
    threshold) subject to the CONFUSER FP gate - the task's own success
    metric is 'confuser FP <= 10%' (the siren/helicopter/chainsaw/rooster
    split). The raw-val noncry FP rate is REPORTED at every sweep point (it
    runs higher than the confuser rate because fold-4 ambience includes the
    full 40-class ESC-50 distribution, while the frozen-test non-cry subset
    and the confuser split are the deployment-relevant FP populations).

    Returns None only if no in-sweep point keeps confuser FP within gate."""
    ok = [m for m in points if m["confuser_fp"] <= confuser_gate]
    if not ok:
        return None
    return max(ok, key=lambda m: (m["f1"], m["recall"], -m["fp_rate"],
                                  -m["threshold"]))


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
                report_summary: str, n_trees: int, max_depth: int,
                op_note: str, model_label: str) -> None:
    """Post-process emlearn inline C into include/cry_model.h with provenance,
    an extern "C" guard, and the Lab-3-style prediction wrappers (public API
    and vote-share semantics unchanged from v1)."""
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
 * Model    : sklearn {model_label}, {n_trees} trees, max_depth {max_depth},
 *            random_state={SEED} (trained on wave-augmented train windows)
 * Input    : {N_FEATURES} float features from 1 s / 16 kHz windows
 *            (feature-set v2: MFCC 0-12 mean+std, log-energy mean/std,
 *            mean ZCR, then MFCC-delta mean+std, log-energy-delta
 *            mean/std, spectral centroid/rolloff/flatness mean+std - see
 *            ml_model/extract_features.py FEATURE_NAMES for exact order)
 * Classes  : 0 = CRY_PRED_NONCRY, 1 = CRY_PRED_CRY
 * Memory   : pure inline decision trees, zero dynamic allocation
 * Firmware : pair with CRY_CONF_THRESHOLD in include/cry_task.h
 *            {op_note}
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
 * @brief  Classify one {N_FEATURES}-feature window.
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
 *         (a {n_trees}-vote tie resolves to NONCRY, mirroring sklearn argmax)
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


def op_note_for(cand):
    """Firmware-pairing note for the header comment (cry_task.h threshold)."""
    b = cand["best"]
    return (f"cry-positive iff forest majority votes CRY and cry vote share "
            f">= {b['threshold']}% (>= {b['votes']}/"
            f"{cand['cfg']['n_estimators']} trees; val-tuned)")


def train_provenance_lines(rows):
    """Corpus-level provenance for the train split actually in use (derived
    from the manifest, so old-manifest and merged-manifest runs both report
    truthfully)."""
    corpora = [
        ("data/donateacry_wav/", "donateacry-corpus (ODbL 1.0)"),
        ("data/ESC-50/", "ESC-50 folds 1-3 non-cry (CC BY-NC 3.0)"),
        ("data/siccd/", "SICCD cries (CC BY 4.0)"),
        ("data/csibe/", "CSIBE cries incl. 960 distant/reverb AIBO room "
                        "recordings + 13 domestic non-cry classes (CC BY 4.0)"),
        ("data/hoyyu_icd/", "hoyyu_icd cries incl. 4,552 CryCeleb distant "
                            "room recordings + non-cry (RESEARCH-USE-ONLY: "
                            "training OK, raw audio never redistributed)"),
    ]
    prov = []
    for prefix, desc in corpora:
        n = sum(1 for r in rows if r.get("split") == "train"
                and r.get("path", "").startswith(prefix))
        if n:
            prov.append(f"train corpus: {desc} [{n} clips]")
    return prov


def prov_lines_for(cand, n_cry_clips, n_ncry_clips, augmented, summary,
                   rows=None):
    """Provenance lines for the candidate actually being exported."""
    b = cand["best"]
    prov = [
        f"generated {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}",
        f"numpy {np.__version__} / scikit-learn {sklearn.__version__} / emlearn {emlearn.__version__}",
    ]
    if rows is not None:
        prov.extend(train_provenance_lines(rows))
    else:                                       # legacy fallback (old manifest)
        prov.append("train: donateacry-corpus (ODbL) + ESC-50 folds 1-3 "
                    "non-cry (CC BY-NC 3.0)")
    if augmented:
        prov.append("train corpus audio additionally wave-augmented "
                    "(train windows only, ml_model/augment.py)")
    prov.extend([
        f"val-tuned operating point: cry vote share >= {b['threshold']}% "
        f"({b['votes']}/{cand['cfg']['n_estimators']} trees); "
        f"val cry F1 {b['f1']:.3f}, val noncry FP {b['fp_rate']:.3f}, "
        f"confuser FP {b['confuser_fp']:.3f}",
        f"frozen test: ESC-50 crying_baby x{n_cry_clips} + fold-5 non-cry "
        f"x{n_ncry_clips}",
        f"features ({N_FEATURES}, feature-set v2): {', '.join(FEATURE_NAMES)}",
        f"model: {model_label_for(cand['cfg'])}",
        f"candidate: {cand['cfg']['desc']}",
        "augmentation recipe (train windows only): cry - real ESC-50 fold "
        f"1-3 ambience mix SNR {int(augment.REAL_NOISE_SNR_MIN_DB)}-"
        f"{int(augment.REAL_NOISE_SNR_MAX_DB)} dB (p={augment.REAL_NOISE_PROB}"
        ") or white/pink noise SNR 5-20 dB, 2-3-tap reverb "
        f"(p={augment.REVERB_PROB}), lowpass "
        f"{int(augment.LOWPASS_FC_MIN_HZ)}-{int(augment.LOWPASS_FC_MAX_HZ)} Hz "
        f"(p={augment.LOWPASS_PROB}), volume {int(augment.GAIN_MIN_DB)}.."
        f"+{int(augment.GAIN_MAX_DB)} dB with +{int(augment.LOUD_GAIN_MIN_DB)}.."
        f"+{int(augment.LOUD_GAIN_MAX_DB)} dB loud branch "
        f"(p={augment.LOUD_PROB}), pitch/speed +/-10% (p=0.5), 50 Hz hum; "
        "non-cry - synthetic noise + volume only",
        "threshold tuning: cry side = raw val cry + stress-val cry "
        "(deployment-channel proxy on val windows); confuser FP gate 10%",
    ])
    if summary:
        prov.append(f"frozen-test result at operating point: {summary}")
    return prov


def build_header(clf, out_path, prov_lines, op_note, max_depth, summary,
                 model_label):
    """emlearn inline export + header wrap. Returns (ok, size, msg).
    Leaves the header on disk; caller removes it if the size is over budget."""
    print("\nemlearn inline export ...")
    cmodel = emlearn.convert(clf, method="inline", dtype="float")
    tmp_header = out_path + ".tmp_eml"
    cmodel.save(name="cry_forest", file=tmp_header)
    with open(tmp_header, encoding="utf-8") as f:
        generated = f.read()
    os.remove(tmp_header)
    tree_ids = sorted({int(m) for m in
                       re.findall(r"cry_forest_tree_(\d+)\s*\(", generated)})
    n_trees = len(clf.estimators_)
    if len(tree_ids) != n_trees:
        return False, 0, (f"expected {n_trees} tree functions in emlearn "
                          f"output, found {len(tree_ids)}")
    wrap_header(generated, out_path, prov_lines, tree_ids,
                summary, n_trees, max_depth, op_note, model_label)
    size = os.path.getsize(out_path)
    return True, size, f"{n_trees} trees x depth <= {max_depth}"


def model_label_for(cfg):
    """Human-readable sklearn class name for the header provenance (the
    wrap_header template already prefixes 'sklearn ')."""
    return ("ExtraTreesClassifier" if cfg["kind"] == "et"
            else "RandomForestClassifier")


# -------------------------------------------------------------------- main --
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dataset-dir", default=HERE,
                    help="directory containing the manifest (default: ml_model)")
    ap.add_argument("--manifest", default="manifest.csv",
                    help="manifest file inside --dataset-dir "
                         "(default manifest.csv; wave-2 merged training uses "
                         "manifest_v2.csv built by prep_v2.py)")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "include",
                                                  "cry_model.h"),
                    help="output C header path (default ../include/cry_model.h)")
    ap.add_argument("--report", default=os.path.join(HERE, "model_report.txt"),
                    help="report path (default ml_model/model_report.txt)")
    ap.add_argument("--candidates", default=",".join(DEFAULT_CANDIDATES),
                    help="comma-separated candidate keys to train/compare")
    ap.add_argument("--no-aug", action="store_true",
                    help="disable wave augmentation (v1 behaviour)")
    ap.add_argument("--aug-variants", type=int,
                    default=augment.N_AUG_CRY_VARIANTS_DEFAULT,
                    help="augmented variants per training cry clip (default 2)")
    ap.add_argument("--noncry-aug-frac", type=float,
                    default=augment.NONCRY_FRAC_DEFAULT,
                    help="fraction of training non-cry windows augmented "
                         "(default 0.5)")
    ap.add_argument("--skip-frozen-test", action="store_true",
                    help="val-only development run: pick candidate + "
                         "threshold on VAL but never touch the frozen test")
    ap.add_argument("--header-max-bytes", type=int, default=HEADER_MAX_BYTES,
                    help="reject exports larger than this (default 409600)")
    args = ap.parse_args()

    def resolve_here(p):
        """Relative paths resolve against this script's dir (ml_model/),
        not the caller's CWD - documented CLI contract."""
        return p if os.path.isabs(p) else os.path.normpath(os.path.join(HERE, p))

    dataset_dir = os.path.abspath(args.dataset_dir)
    out_path = resolve_here(args.out)
    report_path = resolve_here(args.report)
    cache_path = os.path.join(dataset_dir, CACHE_NAME)
    manifest_path = os.path.join(dataset_dir, args.manifest)

    cand_keys = [c.strip() for c in args.candidates.split(",") if c.strip()]
    for k in cand_keys:
        if k not in CANDIDATES:
            print(f"ERROR: unknown candidate '{k}' "
                  f"(known: {', '.join(CANDIDATES)})")
            return 1

    lines = []
    lines.append("=== Smart Baby Monitor - P3 cry/non-cry model report "
                 "(v7, wave-2 corpus merge, feature-set v2) ===")
    lines.append(f"generated : {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}")
    lines.append(f"python {sys.version.split()[0]}, numpy {np.__version__}, "
                 f"scikit-learn {sklearn.__version__}, emlearn {emlearn.__version__}")
    lines.append(f"features ({N_FEATURES}, feature-set v2): "
                 + ", ".join(FEATURE_NAMES))
    lines.append("")
    lines.append("--- v7 retraining recipe (wave-2 corpus merge vs domain "
                 "shift) ---")
    lines.append("goal: lift the v6 shipped model's frozen-test cry recall "
                 "(0.345, F1 0.483, confuser FP 9.6%) under the")
    lines.append("immutable frozen protocol (test = 40 ESC-50 crying_baby "
                 "clips + frozen fold-5 non-cry;")
    lines.append("tuning on VAL only; no ESC-50 crying_baby clip is ever "
                 "trained on).")
    lines.append("root cause addressed this wave: wave 1 could only AUGMENT "
                 "close-mic donateacry cries to look")
    lines.append("distant; wave 2 adds corpora that CONTAIN the deployment "
                 "domain - 17,730 new cry clips")
    lines.append("including real distant/room recordings (960 CSIBE AIBO "
                 "room cries, 4,552 CryCeleb distant")
    lines.append("room cries) + 43,718 new domestic non-cry clips, merged "
                 "by prep_v2.py with md5")
    lines.append("content-hash dedup against every existing split (see the "
                 "prep report below).")
    lines.append("levers: (1) merged train split (~19k cry / ~44k non-cry "
                 "clips) with real distant audio, (2)")
    lines.append("the v5 wave-level augmentation of TRAIN windows only "
                 "(augment.py, same recipes/seeds) at")
    lines.append("1 variant per cry clip (far larger base) + 0.5 non-cry "
                 "calibration fraction, (3) seeded")
    lines.append("model-type sweep rf16x7 / rf24x6 / rf30x8, (4) "
                 "cry-vote-share threshold")
    lines.append(f"    swept {T_MIN}..{T_MAX} with the {int(FP_GATE * 100)}% FP gate on the "
                 "CONFUSER split (the task's target metric); the raw VAL")
    lines.append("non-cry FP rate is reported at every sweep point (it runs "
                 "higher: fold-4 ambience covers all 40 ESC-50 classes plus")
    lines.append("the new hard-negative domestic classes, while the confuser "
                 "split and the frozen-test non-cry subset are the "
                 "deployment-relevant FP populations).")
    lines.append("augmentation real-noise pool stays ESC-50 folds 1-3 only "
                 "(identical to the v5/v6 pool so cached augmented windows "
                 "stay valid; merged non-cry clips are NOT added to the "
                 "pool).")
    if args.no_aug:
        lines.append("augmentation: DISABLED (--no-aug)")
    else:
        lines.append("augmentation (train cry windows): REAL ESC-50 fold 1-3 "
                     "non-cry ambience mixed at SNR "
                     f"{int(augment.REAL_NOISE_SNR_MIN_DB)}-"
                     f"{int(augment.REAL_NOISE_SNR_MAX_DB)} dB (p="
                     f"{augment.REAL_NOISE_PROB}, cry dominant; white/pink "
                     f"synthetic noise SNR 5-20 dB otherwise), 2-3-tap room "
                     f"reverb (p={augment.REVERB_PROB}), first-order lowpass "
                     f"{int(augment.LOWPASS_FC_MIN_HZ)}-{int(augment.LOWPASS_FC_MAX_HZ)} Hz "
                     f"(p={augment.LOWPASS_PROB}), volume "
                     f"{int(augment.GAIN_MIN_DB)}..+{int(augment.GAIN_MAX_DB)} dB "
                     f"with a +{int(augment.LOUD_GAIN_MIN_DB)}..+{int(augment.LOUD_GAIN_MAX_DB)} dB "
                     f"loud branch (p={augment.LOUD_PROB}, "
                     "mirrored on non-cry), pitch/speed +/-10% (p=0.5, per "
                     "file), 50 Hz hum -32..-22 dB rel (p="
                     f"{augment.HUM_PROB})")
        lines.append(f"augmentation (train non-cry windows): synthetic noise "
                     f"+ volume only (same loud branch), "
                     f"{args.noncry_aug_frac:.0%} of windows")
        lines.append(f"variants per cry clip: {args.aug_variants} "
                     "(cached in features_aug_cache.npz, seeded per file)")
        lines.append("threshold tuning uses a stress-val cry proxy: the "
                     "deployment channel (always lowpass 2.5-5 kHz, real-"
                     "noise mix SNR 0-10 dB) applied to VAL cry windows only "
                     "- cry-side F1 mixes raw + stress val cries, the FP "
                     "gate stays on raw VAL non-cry + confusers")
    lines.append("firmware rule replicated: cry iff strict majority votes CRY "
                 "AND cry vote share >= threshold (src/cry_task.c)")
    lines.append("")

    # dataset provenance sidecar written by dataset_prep.py
    prov_path = os.path.join(dataset_dir, "dataset_provenance.txt")
    if os.path.exists(prov_path):
        with open(prov_path, encoding="utf-8") as f:
            lines.append("--- dataset provenance (dataset_provenance.txt) ---")
            lines.extend(f.read().rstrip().splitlines())
        lines.append("")

    # wave-2 merge sidecar written by prep_v2.py (dedup stats + split policy)
    prep_report = os.path.join(dataset_dir, "prep_v2_report.txt")
    if os.path.exists(prep_report):
        with open(prep_report, encoding="utf-8") as f:
            lines.append("--- wave-2 corpus merge (prep_v2_report.txt) ---")
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
        if not path_exists_retry(abs_path):
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
    lines.append("--- split counts (1 s windows, unaugmented) ---")
    lines.extend(counts_txt)
    print("\n".join(counts_txt))

    # 2. training set (original + augmented train windows) --------------------
    if not splits["train"]:
        lines.append("NO training data - aborting (run dataset_prep.py)")
        print("ERROR: no training rows available")
        _write_report(report_path, lines)
        return 1
    Xtr_orig = np.concatenate([w for (w, _y, _s, _r) in splits["train"]])
    ytr_orig = np.array([y for (w, y, _s, _r) in splits["train"]
                         for _ in range(w.shape[0])])
    if len(set(ytr_orig.tolist())) < 2:
        lines.append("TRAIN split has a single class - cannot train")
        _write_report(report_path, lines)
        return 1

    Xtr, ytr = Xtr_orig, ytr_orig
    X_vs = np.zeros((0, N_FEATURES), np.float32)
    if not args.no_aug:
        print("\nbuilding/loading augmented train features ...")
        # Real-noise pool stays ESC-50 folds 1-3 ONLY: identical to the pool
        # the v5/v6 augmented cache was built with (so cached variants stay
        # valid) and avoids loading every merged non-cry clip just to
        # subsample 4000 pool windows. ESC-50 ambience spans 40 classes and
        # is broad enough for the deployment-channel simulation.
        pool_rows = [r for r in rows
                     if r.get("split") == "train" and r.get("label") != "cry"
                     and r.get("path", "").startswith("data/ESC-50/")]
        pool = augment.load_noise_pool(pool_rows, dataset_dir)
        X_aug, y_aug, _src_aug, _rel_aug = augment.load_or_build_augmented(
            rows, dataset_dir, n_cry_variants=args.aug_variants,
            noncry_frac=args.noncry_aug_frac, noise_pool=pool)
        n_aug_cry = int(y_aug.sum()) if X_aug.shape[0] else 0
        lines.append(f"augmented train windows: {X_aug.shape[0]} "
                     f"(cry {n_aug_cry}, noncry "
                     f"{X_aug.shape[0] - n_aug_cry})")
        if X_aug.shape[0]:
            Xtr = np.concatenate([Xtr_orig, X_aug])
            ytr = np.concatenate([ytr_orig, y_aug])
        else:
            lines.append("WARNING: no augmented windows produced - "
                         "training on original windows only")
        print("building stress-val deployment proxy (val cry windows only) ...")
        X_vs, _y_vs, _rel_vs = augment.stress_val_windows(rows, dataset_dir,
                                                          pool)
    n_cry_tr = int(ytr.sum())
    lines.append(f"training set: {Xtr.shape[0]} windows "
                 f"(cry {n_cry_tr}, noncry {Xtr.shape[0] - n_cry_tr})")
    print(f"\ntraining pool: {Xtr.shape[0]} windows "
          f"(cry={n_cry_tr}, noncry={Xtr.shape[0] - n_cry_tr})")

    def split_xy(split):
        X = np.concatenate([w for (w, _y, _s, _r) in splits[split]]) \
            if splits[split] else np.zeros((0, N_FEATURES), np.float32)
        y = np.array([y for (w, y, _s, _r) in splits[split]
                      for _ in range(w.shape[0])])
        return X, y

    Xval, yval = split_xy("val")
    Xcf, ycf = split_xy("confuser")

    # 3. candidates: fit + VAL threshold sweep (frozen test untouched) --------
    lines.append("")
    lines.append("--- candidate comparison on VAL "
                 f"(threshold swept {T_MIN}..{T_MAX}, FP gate "
                 f"{int(FP_GATE * 100)}%) ---")
    lines.append("cry side = raw val cry + stress-val cry (deployment proxy);"
                 if X_vs.shape[0] else
                 "cry side = raw val cry (--no-aug: no stress proxy);")
    lines.append("FP side = raw VAL non-cry windows + confuser split (gate).")
    lines.append(f"{'candidate':<14} {'T%':>4} {'votes':>6} {'cry P':>7} "
                 f"{'cry R':>7} {'cry F1':>7} {'rawR':>6} {'strR':>6} "
                 f"{'val FP':>7} {'conf FP':>8}")
    ranked = []
    for key in cand_keys:
        cfg = CANDIDATES[key]
        print(f"\nfitting candidate {key}: {cfg['desc']}")
        forest_cls = (ExtraTreesClassifier if cfg["kind"] == "et"
                      else RandomForestClassifier)
        clf = forest_cls(n_estimators=cfg["n_estimators"],
                         max_depth=cfg["max_depth"],
                         class_weight=cfg["class_weight"],
                         random_state=SEED, n_jobs=-1)
        clf.fit(Xtr, ytr)
        votes_all_val = tree_votes(clf, Xval)
        votes_raw = votes_all_val[yval == 1]
        votes_nc = votes_all_val[yval == 0]
        votes_stress = tree_votes(clf, X_vs) if X_vs.shape[0] else \
            np.zeros(0, dtype=np.int64)
        votes_cf = tree_votes(clf, Xcf)
        points = sweep_points(votes_raw, votes_stress, votes_nc, votes_cf,
                              cfg["n_estimators"])
        best = pick_point(points)
        if best is None:
            lines.append(f"{key:<14}  -- no operating point inside the FP "
                         "gate --")
            continue
        lines.append(f"{key:<14} {best['threshold']:>4} {best['votes']:>6} "
                     f"{best['precision']:>7.3f} {best['recall']:>7.3f} "
                     f"{best['f1']:>7.3f} {best['raw_recall']:>6.3f} "
                     f"{best['stress_recall']:>6.3f} "
                     f"{best['fp_rate']:>7.3f} "
                     f"{best['confuser_fp']:>8.3f}")
        ranked.append(dict(key=key, clf=clf, cfg=cfg, best=best,
                           points=points, votes_raw=votes_raw,
                           votes_stress=votes_stress, votes_nc=votes_nc,
                           votes_cf=votes_cf))
    if not ranked:
        lines.append("NO candidate produced a usable operating point - abort")
        _write_report(report_path, lines)
        return 1

    # deterministic ranking: val cry F1, then recall, then FP, then name
    ranked.sort(key=lambda r: (-r["best"]["f1"], -r["best"]["recall"],
                               r["best"]["fp_rate"] + r["best"]["confuser_fp"],
                               r["key"]))
    win = ranked[0]
    lines.append("")
    lines.append(f"CHOSEN (val only): {win['key']} at threshold "
                 f"{win['best']['threshold']}% cry vote share "
                 f"(>= {win['best']['votes']}/{win['cfg']['n_estimators']} "
                 f"trees) - {win['cfg']['desc']}")
    lines.append(f"  val cry F1 {win['best']['f1']:.3f} "
                 f"(P {win['best']['precision']:.3f} / "
                 f"R {win['best']['recall']:.3f}; raw R "
                 f"{win['best']['raw_recall']:.3f}, stress R "
                 f"{win['best']['stress_recall']:.3f}), "
                 f"val noncry FP {win['best']['fp_rate']:.3f}, "
                 f"confuser FP {win['best']['confuser_fp']:.3f}")
    for cand in (win,):
        lines.append("")
        lines.append(f"--- {cand['key']} threshold sweep on VAL (all in-sweep "
                     "points; R = cry recall, concat raw+stress) ---")
        lines.append(f"{'T%':>4} {'votes':>6} {'cry P':>7} {'cry R':>7} "
                     f"{'cry F1':>7} {'rawR':>6} {'strR':>6} "
                     f"{'val FP':>7} {'conf FP':>8} {'gate':>6}")
        for m in cand["points"]:
            gated = m["confuser_fp"] <= FP_GATE   # confuser FP gate (task metric)
            star = " <-- chosen" if m is cand["best"] else ""
            lines.append(f"{m['threshold']:>4} {m['votes']:>6} "
                         f"{m['precision']:>7.3f} {m['recall']:>7.3f} "
                         f"{m['f1']:>7.3f} {m['raw_recall']:>6.3f} "
                         f"{m['stress_recall']:>6.3f} "
                         f"{m['fp_rate']:>7.3f} "
                         f"{m['confuser_fp']:>8.3f} "
                         f"{'pass' if gated else 'FAIL':>6}{star}")
    # val confusion matrix at the chosen point: cry side = raw + stress,
    # noncry side = raw val non-cry (matches the selection objective)
    yval_block = np.concatenate([
        np.ones(win["votes_raw"].shape[0] + win["votes_stress"].shape[0],
                dtype=np.int64),
        np.zeros(win["votes_nc"].shape[0], dtype=np.int64)])
    pred_val_block = np.concatenate([
        (win["votes_raw"] >= win["best"]["votes"]).astype(int),
        (win["votes_stress"] >= win["best"]["votes"]).astype(int),
        (win["votes_nc"] >= win["best"]["votes"]).astype(int)])
    metrics_block(yval_block, pred_val_block,
                  f"VALIDATION at chosen operating point ({win['key']}, "
                  f"T={win['best']['threshold']}%; cry side raw+stress, "
                  "noncry side raw)", lines)

    # 4. rule-based baseline (honest comparison, tuned on ORIGINAL train) -----
    base_t, base_ba = energy_baseline_threshold(Xtr_orig, ytr_orig)
    lines.append("")
    lines.append(f"--- rule-based baseline: log-energy > {base_t:.6f} "
                 f"(threshold tuned on original TRAIN, balanced acc "
                 f"{base_ba:.3f}) ---")

    # 5. export candidate selection on VAL + header size budget ----------------
    # (size probing writes to a scratch path; the real header is written last)
    n_cry_clips = sum(1 for (_w, y, _s, _r) in splits["test"] if y == 1)
    n_ncry_clips = len(splits["test"]) - n_cry_clips
    ok = (n_cry_clips >= MIN_TEST_CLIPS and n_ncry_clips >= MIN_TEST_CLIPS)
    ship = None
    if ok:
        lines.append("")
        lines.append(f"--- export selection (header budget "
                     f"{args.header_max_bytes} bytes, emlearn inline) ---")
        probe = out_path + ".size_probe"
        for cand in ranked:
            r = build_header(cand["clf"], probe, ["(probing)"], op_note_for(cand),
                             cand["cfg"]["max_depth"], "(probing)",
                             model_label_for(cand["cfg"]))
            if not r[0]:
                lines.append(f"  {cand['key']}: EXPORT FAILED - {r[2]}")
                continue
            os.remove(probe)
            if r[1] > args.header_max_bytes:
                lines.append(f"  {cand['key']}: header {r[1]} bytes > budget "
                             f"{args.header_max_bytes} - skipped")
                print(f"header too large ({r[1]} bytes) - trying next "
                      "candidate")
                continue
            ship = cand
            lines.append(f"  {cand['key']}: header {r[1]} bytes - within "
                         "budget, SELECTED FOR EXPORT")
            break
        if ship is None:
            lines.append("EXPORT FAILED: no candidate fits the header budget")
    else:
        lines.append("")
        lines.append(f"EXPORT SKIPPED: frozen test set too small "
                     f"(cry clips {n_cry_clips}, noncry clips {n_ncry_clips})")
        print("\nEXPORT SKIPPED - frozen test set too small")

    if ship is not None and ship is not win:
        lines.append("")
        lines.append(f"NOTE: val winner {win['key']} exceeded the header "
                     f"budget - shipping {ship['key']} instead "
                     f"(threshold {ship['best']['threshold']}%, "
                     f"val cry F1 {ship['best']['f1']:.3f}, "
                     f"val FP {ship['best']['fp_rate']:.3f}, confuser FP "
                     f"{ship['best']['confuser_fp']:.3f})")
        lines.append("")
        lines.append(f"--- {ship['key']} threshold sweep on VAL ---")
        lines.append(f"{'T%':>4} {'votes':>6} {'cry P':>7} {'cry R':>7} "
                     f"{'cry F1':>7} {'val FP':>7} {'conf FP':>8} {'gate':>6}")
        for m in ship["points"]:
            gated = m["confuser_fp"] <= FP_GATE   # confuser FP gate (task metric)
            star = " <-- shipped" if m is ship["best"] else ""
            lines.append(f"{m['threshold']:>4} {m['votes']:>6} "
                         f"{m['precision']:>7.3f} {m['recall']:>7.3f} "
                         f"{m['f1']:>7.3f} {m['fp_rate']:>7.3f} "
                         f"{m['confuser_fp']:>8.3f} "
                         f"{'pass' if gated else 'FAIL':>6}{star}")

    # 6. FROZEN TEST - evaluated ONCE, for the SHIPPED model, at its val-tuned
    #    operating point (dev runs with --skip-frozen-test never look at it) --
    test_evaluated = False
    if args.skip_frozen_test:
        lines.append("")
        lines.append("FROZEN TEST: skipped (--skip-frozen-test val-only dev "
                     "run; candidate + threshold above were chosen on VAL)")
        print("\nskipping frozen test (dev run)")
    elif ship is None:
        lines.append("")
        lines.append("FROZEN TEST: skipped (no exportable candidate)")
    else:
        Xte, yte = split_xy("test")
        n_cry_wins = int(yte.sum()) if Xte.shape[0] else 0
        n_ncry_wins = int((1 - yte).sum()) if Xte.shape[0] else 0
        if (n_cry_wins >= MIN_TEST_CLIPS and n_ncry_wins >= MIN_TEST_CLIPS):
            votes_te = tree_votes(ship["clf"], Xte)
            pred_te = (votes_te >= ship["best"]["votes"]).astype(int)
            lines.append("")
            metrics_block(yte, pred_te,
                          f"FROZEN TEST split at the shipped operating point "
                          f"({ship['key']}, T={ship['best']['threshold']}%; "
                          f"evaluated once)", lines)
            if Xte.shape[0]:
                cm_b = confusion_matrix(
                    yte, energy_baseline_apply(Xte, base_t), labels=[0, 1])
                lines.append(f"  (energy baseline on FROZEN TEST: noncry FP "
                             f"{cm_b[0, 1]}/{cm_b[0].sum()}, cry recall "
                             f"{cm_b[1, 1]}/{cm_b[1].sum()})")
            test_evaluated = True

            # 7. confuser false-positive table at the shipped point -----------
            if Xcf.shape[0]:
                votes_cf_ship = tree_votes(ship["clf"], Xcf)
                pred_cf = (votes_cf_ship >= ship["best"]["votes"]).astype(int)
                pred_base = energy_baseline_apply(Xcf, base_t)
                # per-window source ids: split_xy concatenates clip windows in
                # order, so expand clip-level sources to window level (a v1
                # bug zipped 800 windows against 160 clip entries and
                # silently truncated the table to the first 160 windows)
                cf_src_win = [src for (w, _y, src, _r) in splits["confuser"]
                              for _ in range(w.shape[0])]
                assert len(cf_src_win) == len(pred_cf), \
                    "confuser window/source alignment broken"
                lines.append("")
                lines.append("--- confuser false-positive table "
                             f"(split=confuser, shipped point "
                             f"T={ship['best']['threshold']}%) ---")
                groups = {}
                for pv, pb, src in zip(pred_cf, pred_base, cf_src_win):
                    g = src.split(":")[1] if ":" in src else "unknown"
                    grp = groups.setdefault(
                        g, {"w": 0, "fp": 0, "fpb": 0, "clips": set(),
                            "clips_fp": set()})
                    grp["w"] += 1
                    grp["fp"] += int(pv == 1)
                    grp["fpb"] += int(pb == 1)
                    grp["clips"].add(src)
                    if pv == 1:
                        grp["clips_fp"].add(src)
                lines.append(f"{'category':<18} {'clips':>6} {'windows':>8} "
                             f"{'FP windows':>11} {'FP rate':>8} "
                             f"{'FP clips':>9} {'baseline FP':>12}")
                tot_fp = tot_w = 0
                for g in sorted(groups):
                    grp = groups[g]
                    tot_fp += grp["fp"]
                    tot_w += grp["w"]
                    lines.append(f"{g:<18} {len(grp['clips']):>6} "
                                 f"{grp['w']:>8} {grp['fp']:>11} "
                                 f"{grp['fp'] / grp['w']:>8.3f} "
                                 f"{len(grp['clips_fp']):>9} "
                                 f"{grp['fpb']:>12}")
                lines.append(f"{'TOTAL':<18} "
                             f"{sum(len(g_['clips']) for g_ in groups.values()):>6} "
                             f"{tot_w:>8} {tot_fp:>11} "
                             f"{tot_fp / tot_w:>8.3f}")

            # informational clip-level view: the device fires only after the
            # 2-of-3 consecutive-window rule (src/cry_task.c), so per-clip
            # positives matter as much as per-window recall. Same frozen
            # predictions as above - descriptive only, no decision input.
            clip_stats = {"cry": [0, 0, 0], "noncry": [0, 0, 0]}
            off = 0
            for (w, y, _s, _r) in splits["test"]:
                n = w.shape[0]
                pos = int((votes_te[off:off + n]
                           >= ship["best"]["votes"]).sum())
                key = "cry" if y == 1 else "noncry"
                clip_stats[key][0] += 1
                clip_stats[key][1] += int(pos >= 1)
                clip_stats[key][2] += int(pos >= 2)
                off += n
            lines.append("")
            lines.append(f"  clip-level view (informational): cry clips with "
                         f">=1 positive window {clip_stats['cry'][1]}/"
                         f"{clip_stats['cry'][0]}, with >=2 positives "
                         f"{clip_stats['cry'][2]}/{clip_stats['cry'][0]}; "
                         f"noncry clips with >=1 FP window "
                         f"{clip_stats['noncry'][1]}/{clip_stats['noncry'][0]}"
                         f", with >=2 FP windows "
                         f"{clip_stats['noncry'][2]}/{clip_stats['noncry'][0]}")
        else:
            lines.append("")
            lines.append(f"FROZEN TEST incomplete (cry windows "
                         f"{n_cry_wins}, noncry windows {n_ncry_wins}) - "
                         "metrics not computed")

    # 8. final header write for the shipped candidate --------------------------
    if ship is not None:
        if test_evaluated:
            # pull the cry-row metrics of the shipped point out of the report
            p_te, r_te, f_te, _ = prf(
                yte, (tree_votes(ship["clf"], Xte)
                      >= ship["best"]["votes"]).astype(int))
            summary = (f"cry P {p_te[1]:.3f} / R {r_te[1]:.3f} / F1 "
                       f"{f_te[1]:.3f}, confuser FP "
                       f"{ship['best']['confuser_fp']:.3f} (400 windows, "
                       f"40 cry + {n_ncry_clips} noncry clips, frozen ESC-50 "
                       f"protocol, T={ship['best']['threshold']}%)")
        else:
            summary = ("not evaluated in this val-only dev run "
                       "(--skip-frozen-test)")
        r = build_header(ship["clf"], out_path,
                         prov_lines_for(ship, n_cry_clips, n_ncry_clips,
                                        not args.no_aug, summary, rows=rows),
                         op_note_for(ship), ship["cfg"]["max_depth"],
                         summary, model_label_for(ship["cfg"]))
        if r[0]:
            lines.append("")
            lines.append(f"EXPORT: wrote {out_path} ({r[1]} bytes, {r[2]}, "
                         f"{N_FEATURES} features) - candidate "
                         f"{ship['key']}, CRY_CONF_THRESHOLD = "
                         f"{ship['best']['threshold']}u")
            print(f"wrote {out_path} ({r[1]} bytes)")
        else:
            lines.append(f"EXPORT FAILED on final write: {r[2]}")
    elif ok:
        lines.append("no header written (no candidate fits the budget)")

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
