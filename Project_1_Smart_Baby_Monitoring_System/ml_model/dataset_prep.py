#!/usr/bin/env python3
"""
dataset_prep.py - P3 dataset builder for the Smart Baby Monitoring System.

Builds ml_model/manifest.csv (columns: path,label,split,fold,source_id) from the
FROZEN evaluation protocol in docs/project-plan.md P3:

  cry-train   donateacry-corpus (github.com/gveres/donateacry-corpus, ODbL 1.0),
              every clip label=cry. Split into train/val by SOURCE RECORDING
              (chunked exports of one recording share a base name and are
              grouped by the source_id heuristic below - no source straddles
              splits).
  non-cry-train ESC-50 (github.com/karolpiczak/ESC-50, CC BY-NC 3.0) folds 1-3,
              ALL classes except crying_baby, label=noncry.
  val         ESC-50 fold 4 non-cry + the held-out donateacry source recordings.
  test        FROZEN: ALL 40 ESC-50 crying_baby clips (any fold - zero appear in
              training, so cry-class leakage is structurally impossible via the
              ESC-50 fold design) + a deterministic >=20-clip fold-5 non-cry
              subset (seeded shuffle, TEST_NONCRY_N clips).
  confuser    ESC-50 siren / helicopter / chainsaw / rooster (all folds) and,
              IF the user has manually placed UrbanSound8K at
              data/UrbanSound8K, its children_playing and street_music clips.
              Kept in a separate split - never used for training or the main
              test metrics, only for the false-positive table.

Paths in the manifest are relative to the ml_model directory (the directory
containing this script / manifest.csv).

Download behaviour: both datasets are fetched with `git clone --depth 1`. A
clone that fails or exceeds CLONE_TIMEOUT_S prints a clear WARNING and the
script continues with whatever is present, so the pipeline stays runnable.
3gp/caf/... source clips are converted to 16 kHz mono 16-bit WAV with ffmpeg
when ffmpeg is on PATH; without ffmpeg they are skipped with a warning (the
feature extractor cannot read non-WAV containers).

Deterministic: fixed SEED everywhere, no wall-clock input to data decisions.
"""

import csv
import os
import random
import re
import shutil
import subprocess
import sys
import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
DONATE_DIR = DATA / "donateacry-corpus"
DONATE_WAV = DATA / "donateacry_wav"          # ffmpeg output (16 kHz mono s16)
ESC50_DIR = DATA / "ESC-50"
USB8K_DIR = DATA / "UrbanSound8K"             # optional, never auto-downloaded
MANIFEST = HERE / "manifest.csv"
PROVENANCE = HERE / "dataset_provenance.txt"

SEED = 182
CLONE_TIMEOUT_S = 360          # ~6 min per dataset, per the frozen protocol
TEST_NONCRY_N = 40             # >= 20 required; 40 balances the 40 crying_baby
DONATE_VAL_FRAC = 0.15         # share of donateacry SOURCE RECORDINGS held out
ESC50_CONFUSERS = ("siren", "helicopter", "chainsaw", "rooster")
USB8K_CONFUSER_CLASSES = {"children_playing", "street_music"}
CONVERT_EXTS = {".3gp", ".caf", ".wav", ".m4a", ".mp4", ".aac", ".ogg",
                ".flac", ".opus", ".wma"}

DONATEACRY_URL = "https://github.com/gveres/donateacry-corpus.git"
ESC50_URL = "https://github.com/karolpiczak/ESC-50.git"

WARNINGS = []


def warn(msg: str) -> None:
    WARNINGS.append(msg)
    print(f"WARNING: {msg}")


def ffmpeg_available() -> bool:
    return shutil.which("ffmpeg") is not None


def git_sha(repo: Path):
    """Short HEAD sha of a cloned repo, or None."""
    if not (repo / ".git").exists():
        return None
    try:
        out = subprocess.run(["git", "-C", str(repo), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=30)
        return out.stdout.strip() or None
    except Exception:
        return None


def clone_repo(url: str, dest: Path, name: str) -> bool:
    """`git clone --depth 1` with the protocol's ~6-minute budget."""
    if (dest / ".git").exists():
        print(f"[{name}] already cloned at {dest} - skipping download")
        return True
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"[{name}] cloning {url} -> {dest} (timeout {CLONE_TIMEOUT_S}s) ...")
    try:
        r = subprocess.run(["git", "clone", "--depth", "1", url, str(dest)],
                           capture_output=True, text=True, timeout=CLONE_TIMEOUT_S)
        if r.returncode == 0:
            print(f"[{name}] clone OK")
            return True
        warn(f"{name} clone failed (exit {r.returncode}): {r.stderr.strip()[:300]}")
    except subprocess.TimeoutExpired:
        warn(f"{name} clone exceeded {CLONE_TIMEOUT_S}s and was aborted; "
             f"dataset {name} is NOT available this run")
    except Exception as exc:                       # noqa: BLE001
        warn(f"{name} clone failed: {exc}")
    # A timed-out clone can leave a partial directory behind; clear it.
    if dest.exists() and not (dest / ".git").exists():
        shutil.rmtree(dest, ignore_errors=True)
    return False


# ------------------------------------------------------------- donateacry ---
def donate_source_id(stem: str) -> str:
    """Group chunked exports of one source recording.

    donateacry filenames can carry trailing numeric chunk suffixes
    ('<recording>-3', '<recording>_2', '<recording>-3.2'); everything before
    that suffix belongs to the same source recording. Stems whose tail is not
    numeric are their own source.
    """
    m = re.match(r"^(.*?)[-_]\d+(\.\d+)?$", stem)
    return m.group(1) if m and m.group(1) else stem


def convert_donateacry() -> int:
    """ffmpeg-convert every audio file in the donateacry clone to
    16 kHz mono signed-16-bit WAV under data/donateacry_wav/ (mirroring the
    corpus subdirectory layout). Returns number of converted files."""
    if not DONATE_DIR.exists():
        warn("donateacry-corpus not cloned - no cry training data this run")
        return 0
    if not ffmpeg_available():
        warn("ffmpeg not on PATH - .3gp/.caf/.m4a sources cannot be converted; "
             "they will be skipped by later pipeline steps")
        return 0

    out_dir = DONATE_WAV
    n_new, n_ok, n_fail = 0, 0, 0
    for src in sorted(DONATE_DIR.rglob("*")):
        if not src.is_file() or src.suffix.lower() not in CONVERT_EXTS:
            continue
        rel = src.relative_to(DONATE_DIR).with_suffix(".wav")
        dst = out_dir / rel
        if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
            n_ok += 1
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", str(src),
               "-ac", "1", "-ar", "16000", "-sample_fmt", "s16", str(dst)]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            if r.returncode == 0:
                n_new += 1
                n_ok += 1
            else:
                n_fail += 1
                if n_fail <= 5:
                    warn(f"ffmpeg failed on {src.name}: {r.stderr.strip()[:160]}")
        except Exception as exc:                    # noqa: BLE001
            n_fail += 1
            if n_fail <= 5:
                warn(f"ffmpeg error on {src.name}: {exc}")
    print(f"[donateacry] converted {n_new} new files ({n_ok} total available, "
          f"{n_fail} failures)")
    if n_fail:
        warn(f"{n_fail} donateacry files failed ffmpeg conversion and are excluded")
    return n_ok


def donateacry_rows():
    """All converted donateacry WAVs, split into train/val by SOURCE RECORDING."""
    if not DONATE_WAV.exists():
        return []
    files = sorted(DONATE_WAV.rglob("*.wav"))
    sources = {}
    for f in files:
        sources.setdefault(donate_source_id(f.stem), []).append(f)

    src_names = sorted(sources)
    rng = random.Random(SEED)
    rng.shuffle(src_names)
    n_val = int(round(len(src_names) * DONATE_VAL_FRAC)) if len(src_names) > 1 else 0
    val_sources = set(src_names[:n_val])

    rows = []
    for f in files:
        split = "val" if donate_source_id(f.stem) in val_sources else "train"
        rel = os.path.relpath(f, HERE).replace("\\", "/")
        rows.append({
            "path": rel,
            "label": "cry",
            "split": split,
            "fold": "",
            "source_id": f"donateacry:{donate_source_id(f.stem)}",
        })
    print(f"[donateacry] {len(rows)} clips, {len(src_names)} source recordings "
          f"({n_val} held out to val)")
    return rows


# ------------------------------------------------------------------ ESC-50 --
def esc50_rows():
    """ESC-50 rows for every split except donateacry-val. Requires meta/esc50.csv."""
    meta = ESC50_DIR / "meta" / "esc50.csv"
    if not meta.exists():
        warn("ESC-50 not cloned (or meta/esc50.csv missing) - ESC-50-based "
             "train/val/test/confuser rows are all absent this run")
        return [], []

    with open(meta, newline="", encoding="utf-8") as f:
        entries = list(csv.DictReader(f))
    print(f"[ESC-50] metadata loaded: {len(entries)} clips")

    rows = []
    fold5_noncry_pool = []
    for e in entries:
        cat = e["category"]
        fold = int(e["fold"])
        src = e["src_file"]
        fn = e["filename"]
        rel = f"data/ESC-50/audio/{fn}"

        if cat == "crying_baby":
            # FROZEN test positives: every crying_baby clip, any fold.
            rows.append({"path": rel, "label": "cry", "split": "test",
                         "fold": fold, "source_id": f"esc50:crying_baby:{src}"})
        elif cat in ESC50_CONFUSERS:
            rows.append({"path": rel, "label": "noncry", "split": "confuser",
                         "fold": fold, "source_id": f"esc50:{cat}:{src}"})
        elif fold in (1, 2, 3):
            rows.append({"path": rel, "label": "noncry", "split": "train",
                         "fold": fold, "source_id": f"esc50:{cat}:{src}"})
        elif fold == 4:
            rows.append({"path": rel, "label": "noncry", "split": "val",
                         "fold": fold, "source_id": f"esc50:{cat}:{src}"})
        elif fold == 5:
            fold5_noncry_pool.append({"path": rel, "label": "noncry",
                                      "split": "test", "fold": fold,
                                      "source_id": f"esc50:{cat}:{src}"})

    # Deterministic fold-5 non-cry test subset (>= 20 required by protocol).
    fold5_noncry_pool.sort(key=lambda r: r["path"])
    rng = random.Random(SEED)
    pick = rng.sample(range(len(fold5_noncry_pool)),
                      min(TEST_NONCRY_N, len(fold5_noncry_pool)))
    pick_idx = set(pick)
    test_noncry = [r for i, r in enumerate(fold5_noncry_pool) if i in pick_idx]
    rows.extend(test_noncry)
    n = len(test_noncry)
    print(f"[ESC-50] test non-cry subset: {n} of {len(fold5_noncry_pool)} "
          f"fold-5 clips (seed {SEED})")
    if n < 20:
        warn(f"fold-5 non-cry test subset has only {n} clips (< 20 protocol minimum)")
    return rows, entries


# ------------------------------------------------------- UrbanSound8K (opt) --
def usb8k_rows():
    """UrbanSound8K confuser rows, ONLY if the user placed the dataset at
    data/UrbanSound8K (it is ~6 GB and is never auto-downloaded here)."""
    meta = USB8K_DIR / "metadata" / "UrbanSound8K.csv"
    if not USB8K_DIR.exists():
        print("[UrbanSound8K] not present at data/UrbanSound8K - skipped "
              "(optional confuser source; download manually to enable)")
        return []
    if not meta.exists():
        warn("data/UrbanSound8K exists but metadata/UrbanSound8K.csv is missing - "
             "UrbanSound8K confusers skipped")
        return []
    rows = []
    with open(meta, newline="", encoding="utf-8") as f:
        for e in csv.DictReader(f):
            if e["class_name"] in USB8K_CONFUSER_CLASSES:
                rel = (f"data/UrbanSound8K/audio/fold{e['fold']}/"
                       f"{e['slice_file_name']}")
                if (HERE / rel).exists():
                    rows.append({"path": rel, "label": "noncry", "split": "confuser",
                                 "fold": int(e["fold"]),
                                 "source_id": f"usb8k:{e['class_name']}:"
                                              f"{Path(e['slice_file_name']).stem}"})
    print(f"[UrbanSound8K] {len(rows)} confuser clips "
          f"({', '.join(sorted(USB8K_CONFUSER_CLASSES))})")
    return rows


# --------------------------------------------------------------------- main --
def main() -> int:
    started = datetime.datetime.now(datetime.timezone.utc)
    print(f"dataset_prep.py start {started.isoformat(timespec='seconds')}")

    # 1. Downloads -----------------------------------------------------------
    esc50_ok = clone_repo(ESC50_URL, ESC50_DIR, "ESC-50")
    donate_ok = clone_repo(DONATEACRY_URL, DONATE_DIR, "donateacry-corpus")
    esc_sha = git_sha(ESC50_DIR) if esc50_ok else None
    donate_sha = git_sha(DONATE_DIR) if donate_ok else None

    # Rough on-disk sizes for the run log.
    def dir_size_gb(p: Path) -> float:
        if not p.exists():
            return 0.0
        total = sum(f.stat().st_size for f in p.rglob("*") if f.is_file())
        return total / 1e9
    print(f"[sizes] ESC-50 {dir_size_gb(ESC50_DIR):.2f} GB, "
          f"donateacry {dir_size_gb(DONATE_DIR):.2f} GB")

    # 2. Convert + enumerate -------------------------------------------------
    convert_donateacry()
    rows = donateacry_rows()
    esc_rows, _entries = esc50_rows()
    rows += esc_rows
    rows += usb8k_rows()

    if not rows:
        warn("NO dataset rows found at all - manifest will be empty. "
             "Re-run once clones succeed (see warnings above).")

    # 3. Write manifest ------------------------------------------------------
    rows.sort(key=lambda r: (r["split"], r["path"]))
    with open(MANIFEST, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["path", "label", "split", "fold",
                                          "source_id"])
        w.writeheader()
        w.writerows(rows)
    print(f"[manifest] wrote {len(rows)} rows -> {MANIFEST}")

    # 4. Summary -------------------------------------------------------------
    summary = {}
    for r in rows:
        key = (r["split"], r["label"])
        summary.setdefault(key, [0, set()])
        summary[key][0] += 1
        summary[key][1].add(r["source_id"])
    print("\nSplit summary (clips / unique sources):")
    for (split, label), (n, srcs) in sorted(summary.items()):
        print(f"  {split:<9} {label:<7} {n:>5} clips  {len(srcs):>4} sources")
    if WARNINGS:
        print("\nWarnings this run:")
        for wtxt in WARNINGS:
            print(f"  - {wtxt}")

    # 5. Provenance sidecar (consumed by train_cry_model.py) -----------------
    with open(PROVENANCE, "w", encoding="utf-8") as f:
        f.write(f"dataset_prep.py run {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}\n")
        f.write(f"donateacry-corpus: {'cloned sha=' + str(donate_sha) if donate_sha else 'NOT CLONED'}"
                f"  license=ODbL 1.0  url={DONATEACRY_URL}\n")
        f.write(f"ESC-50: {'cloned sha=' + str(esc_sha) if esc_sha else 'NOT CLONED'}"
                f"  license=CC BY-NC 3.0  url={ESC50_URL}\n")
        f.write(f"UrbanSound8K: {'present' if USB8K_DIR.exists() else 'absent (optional)'}"
                f"  license=CC BY-NC 3.0\n")
        f.write(f"ffmpeg available: {ffmpeg_available()}\n")
        f.write(f"seed={SEED} donate_val_frac={DONATE_VAL_FRAC} "
                f"test_noncry_n={TEST_NONCRY_N}\n")
        f.write(f"manifest rows: {len(rows)}\n")
        for wtxt in WARNINGS:
            f.write(f"WARNING: {wtxt}\n")

    print("\ndataset_prep.py finished OK (warnings above are non-fatal)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
