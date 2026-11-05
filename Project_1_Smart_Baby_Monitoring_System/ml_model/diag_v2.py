#!/usr/bin/env python3
"""
diag_v2.py - wave-2 val-only diagnostic: WHY did the confuser FP rate blow up
after the corpus merge, and which train-set composition fixes it?

The dev run (model_report_dev.txt) showed the merged model failing the task's
confuser FP <= 10% gate at every useful threshold (rf16x7 conf FP 0.185 at
T=75 vs v6's 0.096). Hypotheses:
  H1  dilution: ESC-50 fold 1-3 ambience (the only training data that looks
      like the siren/helicopter/chainsaw/rooster confuser population) fell
      from 100% to ~8% of noncry training windows.
  H2  cry-side dominance: 160k cry vs 103k noncry windows (v6: 26k/10.8k)
      pushes the boundary toward cry; sirens/roosters are loud + modulated.
  H3  the new noncry corpora (domestic/speech/silence) do not cover the
      loud-broadband-modulated part of noncry space.

Controlled variants (ALL fitted on TRAIN rows only, swept on VAL only, seed
42, rf16x7 unless noted; frozen test never touched):

  E1    merged as shipped in the dev run (harness sanity - must reproduce
        the dev numbers exactly)
  E2    merged cry + ESC-50-only noncry (old noncry side, aug'd at frac 0.5)
  E3    merged, but ESC-50 train noncry windows sample_weight x3
  E3b   merged, but ESC-50 train noncry windows sample_weight x6
  E4    merged noncry, cry side capped per corpus (siccd 8k, hoyyu 24k,
        csibe all) -> cry:noncry ~ 1:2.3 (mission target band)
  E5    E4 + ESC-50 noncry x3 weight
  E6    merged + ALL noncry windows aug'd (noncry_frac 1.0)

Usage: python diag_v2.py [variant ...]   (default: all)
"""

import csv
import os
import sys
import time

import numpy as np
from sklearn.ensemble import RandomForestClassifier

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import train_cry_model                       # noqa: E402
import augment                               # noqa: E402

N_FEATURES = train_cry_model.N_FEATURES
SEED = 42
MANIFEST = os.path.join(HERE, "manifest_v2.csv")

VARIANTS = ["E1", "E2", "E3", "E3b", "E4", "E5", "E6",
            "E9", "E10", "E11", "E13", "E14",
            "E17", "E19", "E20",
            "E21", "E23", "E20r30", "E20r24"]

CONFUSER_CATS = {"siren", "helicopter", "chainsaw", "rooster"}
# ESC-50 train categories nearest to the (unseen) confuser categories in
# feature space - loud, broadband or strongly modulated machine/animal
# sounds. Weighting these up pushes the noncry boundary outward around the
# confuser population without ever touching confuser-split audio.
NEAR_CATS = {"engine", "train", "fireworks", "car_horn", "church_bells",
             "glass_breaking", "hand_saw", "vacuum_cleaner",
             "washing_machine", "crow", "hen", "sheep", "pig", "cow",
             "thunderstorm", "wind", "sea_waves", "rain"}


def corpus(rel):
    for c in ("siccd", "csibe", "hoyyu_icd", "donateacry_wav", "ESC-50"):
        if f"data/{c}/" in rel:
            return c
    return "other"


def load_all_rows():
    with open(MANIFEST, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def assemble(rows, cache, aug_cache, want, pool=None, newcry_mild_aug=False):
    """Collect (X, y, tag, is_nearcat) windows for train rows matching `want`
    (dict of corpus -> max original windows or None).

    newcry_mild_aug: cry rows from the NEW corpora (already distant/room
    recordings) get their augmented variant built with the mild noise+volume
    recipe instead of the full cry recipe (reverb/lowpass/real-noise), so
    domain-matched audio is not double-degraded into rooster-like broadband
    bursts. donateacry (close-mic) cries keep the full cry recipe."""
    out = {"orig": [], "aug": []}
    used = {}
    for r in rows:
        if r.get("split") != "train":
            continue
        rel, label = r["path"], r["label"]
        c = corpus(rel)
        cap = want.get(c, "skip")
        if cap == "skip":
            continue
        abs_path = os.path.join(HERE, rel)
        if not os.path.exists(abs_path):
            continue
        st = os.stat(abs_path)
        w = cache.get(train_cry_model.file_cache_key(rel, abs_path))
        if w is None or w.shape[0] == 0:
            continue
        near = (c == "ESC-50"
                and r.get("source_id", "").split(":")[1] in NEAR_CATS)
        n_used = used.get(c, 0)
        take = w.shape[0] if cap is None else max(0, min(w.shape[0], cap - n_used))
        used[c] = n_used + take
        if take:
            out["orig"].append((w[:take], 1 if label == "cry" else 0, c, near))
        # augmented variant block
        frac = take / w.shape[0]
        if label == "cry" and newcry_mild_aug and c != "donateacry_wav":
            akey = (f"{augment.AUG_CACHE_VERSION}|{rel}|{st.st_mtime_ns}|"
                    f"{st.st_size}|mild0|f1.00")
            a = aug_cache.get(akey)
            if a is None or a.shape[0] == 0:
                a = augment._aug_file_block(rel, abs_path, "noncry", "mild0",
                                            1.0, pool)
                aug_cache[akey] = a
        elif label == "cry":
            akey = f"{augment.AUG_CACHE_VERSION}|{rel}|{st.st_mtime_ns}|{st.st_size}|cry0"
            a = aug_cache.get(akey)
        else:
            akey = (f"{augment.AUG_CACHE_VERSION}|{rel}|{st.st_mtime_ns}|"
                    f"{st.st_size}|nc|f0.50")
            a = aug_cache.get(akey)
        if a is not None and a.shape[0] and take > 0:
            k = int(round(a.shape[0] * frac))
            if k:
                out["aug"].append((a[:k], 1 if label == "cry" else 0, c, near))
    return out


def main() -> int:
    t0 = time.perf_counter()
    which = [a for a in sys.argv[1:] if a in VARIANTS] or VARIANTS
    rows = load_all_rows()
    print(f"manifest rows: {len(rows)}")

    cache = train_cry_model.load_cache(os.path.join(HERE, "features_cache.npz"))
    aug_cache = augment._load_cache(os.path.join(HERE, "features_aug_cache.npz"))
    print(f"caches: {len(cache)} window entries, {len(aug_cache)} aug entries "
          f"({time.perf_counter() - t0:.0f} s)")

    # fixed eval splits (never re-fitted)
    def split_xy(split):
        X, y, cf = [], [], []
        for r in rows:
            if r.get("split") != split:
                continue
            rel = r["path"]
            abs_path = os.path.join(HERE, rel)
            if not os.path.exists(abs_path):
                continue
            w = cache.get(train_cry_model.file_cache_key(rel, abs_path))
            if w is None or w.shape[0] == 0:
                continue
            X.append(w)
            y.append(np.full(w.shape[0], 1 if r["label"] == "cry" else 0,
                             dtype=np.int64))
            cf.append(np.full(w.shape[0], r["source_id"], dtype=object))
        return (np.concatenate(X), np.concatenate(y), np.concatenate(cf)) \
            if X else (np.zeros((0, N_FEATURES), np.float32),
                       np.zeros(0, np.int64), np.zeros(0, dtype=object))

    Xval, yval, _ = split_xy("val")
    Xcf, ycf, cf_src = split_xy("confuser")
    print(f"val windows {Xval.shape[0]} (cry {int(yval.sum())}), "
          f"confuser {Xcf.shape[0]}")

    # stress-val proxy (train noise pool = ESC-50 fold 1-3 only, as shipped)
    pool_rows = [r for r in rows if r.get("split") == "train"
                 and r.get("label") != "cry"
                 and r.get("path", "").startswith("data/ESC-50/")]
    pool = augment.load_noise_pool(pool_rows, HERE)
    X_vs, _y_vs, _rel_vs = augment.stress_val_windows(rows, HERE, pool)
    print(f"stress-val windows {X_vs.shape[0]}")

    want_all = {"donateacry_wav": None, "ESC-50": None, "siccd": None,
                "csibe": None, "hoyyu_icd": None}
    cry_caps = {"donateacry_wav": None, "ESC-50": None, "siccd": 8000,
                "csibe": None, "hoyyu_icd": 24000}

    variants = {
        "E1":  dict(want=want_all, noncry_aug_frac=0.5),
        "E2":  dict(want={"donateacry_wav": None, "ESC-50": None},
                    noncry_aug_frac=0.5),
        "E3":  dict(want=want_all, w_esc50_nc=3.0, noncry_aug_frac=0.5),
        "E3b": dict(want=want_all, w_esc50_nc=6.0, noncry_aug_frac=0.5),
        "E4":  dict(want=cry_caps, noncry_aug_frac=0.5),
        "E5":  dict(want=cry_caps, w_esc50_nc=3.0, noncry_aug_frac=0.5),
        "E6":  dict(want=want_all, noncry_aug_frac=1.0),
        # drop-one cry-corpus ablations (isolate per-corpus FP pressure)
        "E9":  dict(want=want_all, noncry_aug_frac=0.5,
                    drop=lambda r: "xunfei" in r.get("source_id", "")),
        "E10": dict(want=want_all, noncry_aug_frac=0.5,
                    drop=lambda r: "siccd" in r.get("source_id", "")),
        "E11": dict(want=want_all, noncry_aug_frac=0.5,
                    drop=lambda r: "EnesBabyCries" in r.get("source_id", "")),
        "E13": dict(want=want_all, noncry_aug_frac=0.5,
                    drop=lambda r: "cryceleb" in r.get("source_id", "")),
        "E14": dict(want=want_all, noncry_aug_frac=0.5,
                    drop=lambda r: corpus(r["path"]) == "csibe"
                    and r["label"] == "cry"),
        # upweight the nearest loud-mechanical/animal ESC-50 train categories
        "E17": dict(want=want_all, w_near=3.0, noncry_aug_frac=0.5),
        # don't double-degrade already-distant cries with the full cry recipe
        "E19": dict(want=want_all, noncry_aug_frac=0.5, newcry_mild_aug=True),
        # E17 + E19 combined
        "E20": dict(want=want_all, w_near=3.0, noncry_aug_frac=0.5,
                    newcry_mild_aug=True),
        # refinements around E20: forest geometry, stronger near-cat weight,
        # xunfei ablation on top
        "E21": dict(want=want_all, w_near=5.0, noncry_aug_frac=0.5,
                    newcry_mild_aug=True),
        "E23": dict(want=want_all, w_near=3.0, noncry_aug_frac=0.5,
                    newcry_mild_aug=True,
                    drop=lambda r: "xunfei" in r.get("source_id", "")),
        "E20r30": dict(want=want_all, w_near=3.0, noncry_aug_frac=0.5,
                       newcry_mild_aug=True, rf=(30, 8)),
        "E20r24": dict(want=want_all, w_near=3.0, noncry_aug_frac=0.5,
                       newcry_mild_aug=True, rf=(24, 6)),
    }

    for name in which:
        cfg = variants[name]
        rows_v = [r for r in rows
                  if not (cfg.get("drop") and cfg["drop"](r))]
        print(f"\n=== variant {name}: "
              f"w_esc50_nc={cfg.get('w_esc50_nc', 1.0)}, "
              f"w_near={cfg.get('w_near', 1.0)}, "
              f"noncry_aug_frac={cfg['noncry_aug_frac']}, "
              f"newcry_mild_aug={cfg.get('newcry_mild_aug', False)}, "
              f"drop={'yes' if cfg.get('drop') else 'no'}, "
              f"cry_caps={ {k: v for k, v in cfg['want'].items() if v is not None} } ===")
        blocks = assemble(rows_v, cache, aug_cache, cfg["want"], pool=pool,
                          newcry_mild_aug=cfg.get("newcry_mild_aug", False))
        Xo = np.concatenate([b[0] for b in blocks["orig"]]) if blocks["orig"] \
            else np.zeros((0, N_FEATURES), np.float32)
        yo = np.concatenate([np.full(b[0].shape[0], b[1], np.int64)
                             for b in blocks["orig"]])
        co = np.concatenate([np.full(b[0].shape[0], b[2], dtype=object)
                             for b in blocks["orig"]])
        fo = np.concatenate([np.full(b[0].shape[0], b[3], dtype=bool)
                             for b in blocks["orig"]])
        if cfg["noncry_aug_frac"] == 1.0:
            # include the f1.00 non-cry aug blocks as well (built on demand)
            extra = []
            for r in rows_v:
                if r.get("split") != "train" or r["label"] == "cry":
                    continue
                rel = r["path"]
                abs_path = os.path.join(HERE, rel)
                if not os.path.exists(abs_path):
                    continue
                st = os.stat(abs_path)
                akey = (f"{augment.AUG_CACHE_VERSION}|{rel}|{st.st_mtime_ns}|"
                        f"{st.st_size}|nc|f1.00")
                a = aug_cache.get(akey)
                if a is None or a.shape[0] == 0:
                    blk = augment._aug_file_block(rel, abs_path, "noncry",
                                                  "nc", 1.0, pool)
                    aug_cache[akey] = blk
                    a = blk
                if a.shape[0]:
                    near = (corpus(rel) == "ESC-50"
                            and r.get("source_id", "").split(":")[1]
                            in NEAR_CATS)
                    extra.append((a, 0, corpus(rel), near))
            if extra:
                blocks["aug"] = blocks["aug"] + extra
        Xa = np.concatenate([b[0] for b in blocks["aug"]]) if blocks["aug"] \
            else np.zeros((0, N_FEATURES), np.float32)
        ya = np.concatenate([np.full(b[0].shape[0], b[1], np.int64)
                             for b in blocks["aug"]])
        ca = np.concatenate([np.full(b[0].shape[0], b[2], dtype=object)
                             for b in blocks["aug"]])
        fa = np.concatenate([np.full(b[0].shape[0], b[3], dtype=bool)
                             for b in blocks["aug"]])
        X = np.concatenate([Xo, Xa])
        y = np.concatenate([yo, ya])
        c = np.concatenate([co, ca])
        f = np.concatenate([fo, fa])
        w = np.ones(X.shape[0], dtype=np.float64)
        if cfg.get("w_esc50_nc", 1.0) != 1.0:
            w[(y == 0) & (c == "ESC-50")] *= cfg["w_esc50_nc"]
        if cfg.get("w_near", 1.0) != 1.0:
            w[(y == 0) & f] *= cfg["w_near"]
        n_cry, n_nc = int(y.sum()), int((1 - y).sum())
        esc50_nc = int(((y == 0) & (c == "ESC-50")).sum())
        print(f"train windows {X.shape[0]} (cry {n_cry}, noncry {n_nc}; "
              f"ESC-50 noncry {esc50_nc} = {esc50_nc / max(n_nc, 1):.1%} of noncry)")

        n_est, depth = cfg.get("rf", (16, 7))
        clf = RandomForestClassifier(n_estimators=n_est, max_depth=depth,
                                     class_weight="balanced",
                                     random_state=SEED, n_jobs=-1)
        tf = time.perf_counter()
        clf.fit(X, y, sample_weight=w)
        print(f"fit rf{n_est}x{depth}: {time.perf_counter() - tf:.0f} s")
        votes_all = train_cry_model.tree_votes(clf, Xval)
        votes_raw = votes_all[yval == 1]
        votes_nc = votes_all[yval == 0]
        votes_stress = train_cry_model.tree_votes(clf, X_vs)
        votes_cf = train_cry_model.tree_votes(clf, Xcf)
        pts = train_cry_model.sweep_points(votes_raw, votes_stress, votes_nc,
                                           votes_cf, n_est)
        cf_cat = np.array([s.split(":")[1] if ":" in s else "unknown"
                           for s in cf_src], dtype=object)
        print(f"{'T%':>4} {'votes':>6} {'cry P':>7} {'cry R':>7} "
              f"{'cry F1':>7} {'rawR':>6} {'strR':>6} {'val FP':>7} "
              f"{'conf FP':>8} {'sir':>5} {'hel':>5} {'chn':>5} {'roo':>5} "
              f"{'gate':>6}")
        for m in pts:
            ok = m["confuser_fp"] <= train_cry_model.FP_GATE
            sel = votes_cf >= m["votes"]
            per = {g: (float(((cf_cat == g) & sel).mean()) if (cf_cat == g).any()
                       else 0.0)
                   for g in ("siren", "helicopter", "chainsaw", "rooster")}
            print(f"{m['threshold']:>4} {m['votes']:>6} "
                  f"{m['precision']:>7.3f} {m['recall']:>7.3f} "
                  f"{m['f1']:>7.3f} {m['raw_recall']:>6.3f} "
                  f"{m['stress_recall']:>6.3f} {m['fp_rate']:>7.3f} "
                  f"{m['confuser_fp']:>8.3f} "
                  f"{per['siren']:>5.2f} {per['helicopter']:>5.2f} "
                  f"{per['chainsaw']:>5.2f} {per['rooster']:>5.2f} "
                  f"{'pass' if ok else 'FAIL':>6}")
        best = train_cry_model.pick_point(pts)
        if best:
            print(f"  -> gate-passing best: T={best['threshold']}% "
                  f"F1 {best['f1']:.3f} rawR {best['raw_recall']:.3f} "
                  f"conf FP {best['confuser_fp']:.3f}")
        else:
            print("  -> NO gate-passing point")
        del clf, X, y, Xa, ya, blocks
    print(f"\ntotal {time.perf_counter() - t0:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
