#!/usr/bin/env python3
"""
prep_v2.py - wave-2 dataset merger for the Smart Baby Monitoring System.

Merges three newly acquired corpora into the P3 training pipeline without
touching the frozen evaluation protocol:

  ml_model/manifest_siccd.csv      3,200 cry clips  (SICCD, CC BY 4.0)
  ml_model/manifest_csibe.csv      1,200 cry (960 distant/reverb AIBO room
                                   recordings) + 28,570 non-cry, 13 domestic
                                   classes (CSIBE, CC BY 4.0)
  ml_model/manifest_hoyyu_icd.csv  13,330 cry (incl. 4,552 CryCeleb distant
                                   room recordings) + 15,148 non-cry
                                   (RESEARCH-USE-ONLY license: training OK,
                                   raw audio NEVER redistributed)

Outputs:
  manifest_v2.csv       merged manifest (existing rows unchanged + new
                        candidate rows assigned to train/val)
  prep_v2_report.txt    dedup stats, split counts, run log
  dropped_clips_v2.csv  audit trail: every dropped candidate + reason
  features_cache.npz    pre-warmed with the v2 63-feature front end for every
                        manifest_v2 row (same key format as
                        train_cry_model.file_cache_key, so the trainer run is
                        a pure cache read)

DEDUP POLICY (mission contract):
  Every clip is fingerprinted by md5 of its 16 kHz mono signed-16-bit PCM
  bytes. 16 kHz/mono/s16 files (all three new corpora + donateacry_wav) are
  hashed straight from the WAV data chunk; any other format (ESC-50 is
  44.1 kHz) is converted through the pipeline's own loader/resampler
  (extract_features.load_wav + resample_to_16k) and quantized to s16 first,
  so the fingerprint is comparable across corpora.  This is an EXACT-content
  hash: re-encoded, gain-shifted or pitch-shifted duplicates are not caught
  (documented limitation; SICCD's ~330 internally-augmented clips carry no
  metadata marker and are therefore not identifiable).

  Hash sets of the EXISTING manifest:
    train_hash    existing split=train rows
    val_hash      existing split=val rows
    protect_hash  existing split=test (FROZEN) + split=confuser rows

  Every candidate clip is then routed:
    duplicate of another candidate (same hash)   -> keep 1 deterministic
                                                    representative, drop rest
    hash in protect_hash                         -> DROP ENTIRELY (protects
                                                    the frozen test AND the
                                                    confuser FP gate from
                                                    training leakage)
    hash in val_hash                             -> train (never val: val is
                                                    tuning-only; a duplicate
                                                    of a val clip in val would
                                                    contaminate tuning)
    hash in train_hash                           -> train (candidate-train)
    otherwise                                    -> eligible for split

SPLIT POLICY:
  Eligible candidates need >= 1 full 1 s window (16,000 samples at 16 kHz) -
  shorter clips yield zero windows in the front end and only cost I/O.
  val = 10% of eligible candidates, stratified by (corpus, label); csibe
  clips are grouped by their underlying source file (the '__<n>__' chunk
  ordinal in the filename is stripped) so chunks of one recording never
  straddle train/val.  siccd/hoyyu_icd clips are numbered segments without
  session metadata - split per clip, with exact-duplicate segments already
  removed by the hash pass (near-duplicate adjacent segments of one source
  session may straddle; documented limitation).
  Non-cry candidates are included in FULL (measured extraction cost
  ~11 ms/clip on this machine; by clips the merged train split stays inside
  the mission's 1:2..1:4 cry:noncry target band, by windows it is ~1:1, and
  the trainer uses class_weight=balanced).

Deterministic: fixed seeds (182), sorted inputs, no wall-clock input to
data decisions.
"""

import csv
import hashlib
import os
import random
import re
import sys
import time
import wave
import datetime
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from extract_features import (                       # noqa: E402
    UnsupportedAudio, extract_file_windows, load_wav, resample_to_16k)
import train_cry_model                               # noqa: E402  (cache format)

BASE_MANIFEST = HERE / "manifest.csv"
CAND_MANIFESTS = {
    "siccd": HERE / "manifest_siccd.csv",
    "csibe": HERE / "manifest_csibe.csv",
    "hoyyu_icd": HERE / "manifest_hoyyu_icd.csv",
}
OUT_MANIFEST = HERE / "manifest_v2.csv"
OUT_REPORT = HERE / "prep_v2_report.txt"
OUT_DROPPED = HERE / "dropped_clips_v2.csv"
CACHE_NAME = "features_cache.npz"

SEED = 182
VAL_FRAC = 0.10
N_WORKERS = max(4, min(12, (os.cpu_count() or 8) - 2))
SAVE_EVERY = 2000          # cache entries between incremental saves

LINES = []


def log(msg=""):
    print(msg)
    LINES.append(str(msg))


# ------------------------------------------------------------- hashing -----
def content_hash(abs_path):
    """md5 of the clip's 16 kHz mono s16 PCM bytes + (duration_s, fmt).

    Fast path: fs==16000, mono, s16 -> hash the raw data chunk verbatim.
    Anything else: pipeline loader + resampler + s16 quantization (exact
    round-trip for 16 kHz s16 sources, deterministic for the rest).
    """
    with wave.open(str(abs_path), "rb") as w:
        ch, sw, fs, nf = (w.getnchannels(), w.getsampwidth(),
                          w.getframerate(), w.getnframes())
        raw = w.readframes(nf)
    dur = (nf / fs) if fs else 0.0
    if fs == 16000 and ch == 1 and sw == 2:
        digest = hashlib.md5(raw).hexdigest()
    else:
        x, fs_in = load_wav(abs_path)
        x = resample_to_16k(x, fs_in)
        q = np.clip(np.round(x.astype(np.float64) * 32768.0),
                    -32768, 32767).astype("<i2")
        digest = hashlib.md5(q.tobytes()).hexdigest()
    return digest, dur, (fs, ch, sw)


# ----------------------------------------------------------- worker ---------
def scan_clip(job):
    """One clip -> (rel, label, ok, hash, dur, n_windows, feats).

    On failure the last element carries the error message string instead of
    a feature array; the parent routes the clip to dropped-unreadable."""
    rel, label = job
    abs_path = os.path.join(str(HERE), rel)
    try:
        digest, dur, _fmt = content_hash(abs_path)
    except Exception as exc:                              # noqa: BLE001
        return (rel, label, False, "", 0.0, 0, f"unreadable: {exc}")
    try:
        wins = extract_file_windows(abs_path)
        feats = (np.stack(wins).astype(np.float32) if wins
                 else np.zeros((0, train_cry_model.N_FEATURES), np.float32))
    except Exception as exc:                              # noqa: BLE001
        return (rel, label, False, digest, dur, 0, f"extract error: {exc}")
    return (rel, label, True, digest, dur, feats.shape[0], feats)


def corpus_of(path):
    for name in CAND_MANIFESTS:
        if f"data/{name}/" in path:
            return name
    return "unknown"


CSIBE_CHUNK_RE = re.compile(r"__\d+__")


def csibe_group_key(path):
    """csibe chunked exports of one original source file share a group key
    (strip the '__<chunk-ordinal>__' segment from the filename)."""
    return CSIBE_CHUNK_RE.sub("__", path)


def group_key(corpus, path):
    return csibe_group_key(path) if corpus == "csibe" else path


# --------------------------------------------------------------- main -------
def main() -> int:
    t_start = time.perf_counter()
    log(f"=== prep_v2.py wave-2 dataset merge "
        f"{datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')} ===")
    log(f"workers={N_WORKERS}, val_frac={VAL_FRAC}, seed={SEED}")

    # 1. existing (frozen) manifest ------------------------------------------
    with open(BASE_MANIFEST, newline="", encoding="utf-8") as f:
        base_rows = list(csv.DictReader(f))
    base_split = defaultdict(list)
    for r in base_rows:
        base_split[r["split"]].append(r)
    log(f"base manifest: {len(base_rows)} rows "
        + ", ".join(f"{s}={len(v)}" for s, v in sorted(base_split.items())))

    # 2. candidates ------------------------------------------------------------
    cand_rows = []
    for name, mf in CAND_MANIFESTS.items():
        with open(mf, newline="", encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
        for r in rows:
            r.setdefault("fold", "")
            r["_corpus"] = name
        cand_rows.extend(rows)
        log(f"candidates {name}: {len(rows)} rows")
    log(f"candidates total: {len(cand_rows)}")

    # 3. hash pass over everything (parallel) ----------------------------------
    def rel_of(r):
        return r["path"]

    hash_targets = [(rel_of(r), r["label"]) for r in base_rows] + \
                   [(rel_of(r), r["label"]) for r in cand_rows]
    log(f"hashing + extracting {len(hash_targets)} clips "
        f"({len(base_rows)} base + {len(cand_rows)} candidates) ...")

    hashes = {}            # rel -> md5 hex ("" if unreadable)
    durations = {}         # rel -> seconds
    feats_new = {}         # rel -> (n, 63) float32 (ALL scanned clips)
    errors = {}
    n_done = 0
    t_hash = time.perf_counter()
    cache = train_cry_model.load_cache(str(HERE / CACHE_NAME))
    log(f"feature cache loaded: {len(cache)} entries")

    with ProcessPoolExecutor(max_workers=N_WORKERS) as ex:
        for rel, label, ok, digest, dur, n_win, feats in \
                ex.map(scan_clip, hash_targets, chunksize=64):
            hashes[rel] = digest
            durations[rel] = dur
            if ok:
                feats_new[rel] = feats
            else:
                errors[rel] = feats   # error message string in feats slot
            n_done += 1
            if n_done % 5000 == 0:
                log(f"  scanned {n_done}/{len(hash_targets)} "
                    f"({time.perf_counter() - t_hash:.0f} s)")

    n_err = len(errors)
    log(f"hash+extract pass done: {n_done} clips, {n_err} errors "
        f"({time.perf_counter() - t_hash:.0f} s)")

    # 4. build hash sets of existing splits ------------------------------------
    train_hash = {hashes[r["path"]] for r in base_split["train"]
                  if hashes.get(r["path"])}
    val_hash = {hashes[r["path"]] for r in base_split["val"]
                if hashes.get(r["path"])}
    protect_hash = ({hashes[r["path"]] for r in base_split["test"]
                     if hashes.get(r["path"])}
                    | {hashes[r["path"]] for r in base_split["confuser"]
                       if hashes.get(r["path"])})
    log(f"existing hash sets: train={len(train_hash)} val={len(val_hash)} "
        f"protect(test+confuser)={len(protect_hash)}")

    # 5. route candidates --------------------------------------------------------
    seen = set()           # candidate hashes already assigned (dedup groups)
    routed = []            # (row, status) status in
                           # {train, train-dup-train, train-dup-val, val,
                           #  dropped-dup, dropped-protect, dropped-short,
                           #  dropped-unreadable}
    stats = Counter()
    dropped = []           # (path, reason)
    eligible = []          # candidates for the val/train split

    for r in sorted(cand_rows, key=lambda x: x["path"]):
        rel = r["path"]
        h = hashes.get(rel, "")
        if rel in errors:
            stats["dropped_unreadable"] += 1
            dropped.append((rel, errors[rel]))
            routed.append((r, "dropped-unreadable"))
            continue
        if not h:
            stats["dropped_unreadable"] += 1
            dropped.append((rel, "no content hash"))
            routed.append((r, "dropped-unreadable"))
            continue
        if h in protect_hash:
            stats["dropped_protect"] += 1
            dropped.append((rel, "hash matches FROZEN test/confuser clip"))
            routed.append((r, "dropped-protect"))
            continue
        if h in seen:
            stats["dropped_intra_candidate_dup"] += 1
            dropped.append((rel, "duplicate content of another candidate "
                                 "(kept first in deterministic order)"))
            routed.append((r, "dropped-dup"))
            continue
        seen.add(h)
        if h in val_hash:
            stats["train_dup_of_val"] += 1
            routed.append((r, "train-dup-val"))
            continue
        if h in train_hash:
            stats["train_dup_of_train"] += 1
            routed.append((r, "train-dup-train"))
            continue
        if durations.get(rel, 0.0) < 1.0:
            stats["dropped_short"] += 1
            dropped.append((rel, "shorter than one 1 s window (< 1.0 s)"))
            routed.append((r, "dropped-short"))
            continue
        stats["eligible"] += 1
        eligible.append(r)

    log(f"routing: {dict(stats)}")
    log(f"eligible for split: {len(eligible)}")

    # 6. stratified val split ----------------------------------------------------
    rng = random.Random(SEED)
    strata = defaultdict(lambda: defaultdict(list))   # corpus -> label -> rows
    for r in eligible:
        strata[corpus_of(r["path"])][r["label"]].append(r)
    val_set = set()
    log("")
    log("--- stratified 10% val split (eligible candidates) ---")
    for corpus in sorted(strata):
        for label in sorted(strata[corpus]):
            rows = strata[corpus][label]
            groups = defaultdict(list)
            for r in rows:
                groups[group_key(corpus, r["path"])].append(r)
            gkeys = sorted(groups)
            rng.shuffle(gkeys)
            target = int(round(len(rows) * VAL_FRAC))
            n_val, val_keys = 0, []
            for gk in gkeys:                      # walk shuffled groups until
                if n_val >= target:               # the clip target is met
                    break
                val_keys.append(gk)
                n_val += len(groups[gk])
            for gk in val_keys:
                for r in groups[gk]:
                    val_set.add(r["path"])
            log(f"  {corpus:<9} {label:<6} eligible={len(rows):>6} "
                f"groups={len(gkeys):>5} -> val {n_val:>5} clips "
                f"({n_val / max(len(rows), 1):.1%})")
    for r in eligible:
        routed.append((r, "val" if r["path"] in val_set else "train"))
    stats["val_eligible"] = len(val_set)
    stats["train_eligible"] = len(eligible) - len(val_set)

    # 7. write manifest_v2.csv ----------------------------------------------------
    new_rows = []
    for r, status in routed:
        if status in ("train", "train-dup-train", "train-dup-val", "val"):
            split = "val" if status == "val" else "train"
            new_rows.append({"path": r["path"], "label": r["label"],
                             "split": split, "fold": r.get("fold", ""),
                             "source_id": r["source_id"]})
    all_rows = (list(base_rows)
                + sorted(new_rows, key=lambda x: (x["split"], x["path"])))
    with open(OUT_MANIFEST, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["path", "label", "split", "fold",
                                          "source_id"])
        w.writeheader()
        w.writerows(all_rows)
    log("")
    log(f"wrote {OUT_MANIFEST.name}: {len(all_rows)} rows "
        f"({len(base_rows)} base + {len(new_rows)} new)")

    # 8. split summary -------------------------------------------------------------
    log("")
    log("--- merged manifest split summary (clips) ---")
    summ = Counter()
    for r in all_rows:
        summ[(r["split"], r["label"])] += 1
    for (split, label), n in sorted(summ.items()):
        log(f"  {split:<9} {label:<7} {n:>6}")
    for split in ("train", "val"):
        cry = summ[(split, "cry")]
        nc = summ[(split, "noncry")]
        log(f"  {split}: cry:noncry = 1:{nc / max(cry, 1):.2f} (by clips)")

    # 9. pre-warm feature cache ------------------------------------------------------
    log("")
    log("--- pre-warming features_cache.npz ---")
    n_new_entries, n_hit, n_missing = 0, 0, 0
    t_cache = time.perf_counter()
    for i, r in enumerate(all_rows):
        rel = r["path"]
        abs_path = os.path.join(str(HERE), rel)
        if not os.path.exists(abs_path):
            n_missing += 1
            continue
        st = os.stat(abs_path)
        key = train_cry_model.file_cache_key(rel, abs_path)
        if key in cache:
            n_hit += 1
        else:
            arr = feats_new.get(rel)
            if arr is None:
                if rel in errors:
                    arr = np.zeros((0, train_cry_model.N_FEATURES), np.float32)
                else:
                    n_missing += 1
                    continue
            cache[key] = arr.astype(np.float32)
            n_new_entries += 1
            if n_new_entries % SAVE_EVERY == 0:
                train_cry_model.save_cache(str(HERE / CACHE_NAME), cache)
                log(f"  cache: {n_new_entries} new entries saved "
                    f"({time.perf_counter() - t_cache:.0f} s)")
    train_cry_model.save_cache(str(HERE / CACHE_NAME), cache)
    log(f"cache pre-warm done: {n_new_entries} new, {n_hit} hits, "
        f"{n_missing} missing ({time.perf_counter() - t_cache:.0f} s)")

    # 10. audit trail ------------------------------------------------------------------
    with open(OUT_DROPPED, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["path", "reason"])
        w.writerows(dropped)

    # 11. report -----------------------------------------------------------------------
    dt = time.perf_counter() - t_start
    log("")
    log(f"--- dedup method ---")
    log("content fingerprint = md5 of 16 kHz mono signed-16-bit PCM bytes")
    log("(16 kHz/mono/s16 files hashed verbatim from the WAV data chunk;")
    log("other formats routed through extract_features.load_wav +")
    log("resample_to_16k + s16 quantization). EXACT-content matching only:")
    log("re-encoded / gain- or pitch-shifted duplicates are NOT caught.")
    log("SICCD's ~330 internally-augmented clips carry no metadata marker")
    log("and could not be identified or excluded individually.")
    log("")
    log("--- dedup stats (candidates) ---")
    log(f"  candidates scanned                    {len(cand_rows)}")
    log(f"  unreadable/error                      {stats['dropped_unreadable']}")
    log(f"  dropped: hash == FROZEN test/confuser {stats['dropped_protect']}")
    log(f"  dropped: duplicate of another candidate {stats['dropped_intra_candidate_dup']}")
    log(f"  -> train (duplicate of existing train clip) {stats['train_dup_of_train']}")
    log(f"  -> train (duplicate of existing val clip; kept out of val) {stats['train_dup_of_val']}")
    log(f"  dropped: shorter than 1 s window      {stats['dropped_short']}")
    log(f"  eligible for train/val split          {stats['eligible']}")
    log(f"  assigned val (10% stratified)         {stats['val_eligible']}")
    log(f"  assigned train                        {stats['train_eligible']}")
    log("")
    log(f"total runtime {dt / 60:.1f} min")
    with open(OUT_REPORT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(LINES) + "\n")
    print(f"report: {OUT_REPORT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
