#!/usr/bin/env python3
"""Datasets v2 acquisition helper (P3 baby-cry ML).

Converts freshly acquired datasets under ml_model/data/<name>/ to 16 kHz
mono WAV (ffmpeg) and writes a per-dataset manifest snippet
ml_model/manifest_<name>.csv with columns path,label,split,source_id.

ALL rows are written with split=candidate: the orchestrator integrates and
re-splits; the frozen test set of the current protocol stays untouched.

Usage (run from anywhere, paths are absolute):
  python -I acquire_v2.py <dataset>
where <dataset> is one of: csibe, siccd, hoyyu_icd

Pure stdlib; ffmpeg/ffprobe must be on PATH. Never touches manifest.csv.
"""
import csv
import os
import re
import subprocess
import sys

ML_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(ML_DIR, "data")
AUDIO_EXT = {".wav", ".m4a", ".mp3", ".flac", ".ogg", ".opus", ".aac",
             ".caf", ".3gp", ".mp4", ".aif", ".aiff", ".wma", ".webm"}


def classify(dataset, relpath):
    """-> (label, fine_class) or None to skip. relpath uses forward slashes."""
    parts = relpath.split("/")
    if dataset == "csibe":
        # class = parent dir; raw layout: "baby_cry_2";
        # aibo layout: "baby_cry__ccw_30_1m__2" (class__condition__index)
        d = parts[-2].lower() if len(parts) >= 2 else ""
        cls = re.sub(r"_\d+$", "", d.split("__")[0])
        label = "cry" if "cry" in cls else "noncry"
        return label, cls
    if dataset == "siccd":
        cls = parts[-2].lower() if len(parts) >= 2 else ""
        return "cry", cls  # SICCD is a 4-class *cry* corpus (all positive)
    if dataset == "hoyyu_icd":
        # positive.zip -> cry ; negative.zip -> noncry ; subclass from the
        # official metadata CSV all28479.csv (Name column, stem-matched)
        return None, ""  # resolved by caller via metadata
    raise ValueError(dataset)


def hoyyu_subclass_map():
    m = {}
    meta = os.path.join(DATA_DIR, "hoyyu_icd", "all28479.csv")
    if os.path.isfile(meta):
        with open(meta, newline="", encoding="utf-8") as fh:
            for row in csv.DictReader(fh):
                name = row["Name"]
                stem = os.path.splitext(name)[0]
                m[stem] = (row["class_string"], row["subclass"])
    return m


def convert(dataset):
    root = os.path.join(DATA_DIR, dataset)
    wav_root = os.path.join(root, "wav16")
    hoyyu = hoyyu_subclass_map() if dataset == "hoyyu_icd" else {}
    rows = []
    n_skip = 0
    for dirpath, _dirnames, filenames in os.walk(root):
        if os.path.abspath(dirpath).startswith(os.path.abspath(wav_root)) \
                or "_archives" in os.path.basename(dirpath) \
                or "_archives" in os.path.abspath(dirpath).split(os.sep + dataset + os.sep)[-1]:
            continue
        for fn in sorted(filenames):
            ext = os.path.splitext(fn)[1].lower()
            if ext not in AUDIO_EXT:
                continue
            src = os.path.join(dirpath, fn)
            rel = os.path.relpath(src, root).replace("\\", "/")
            if dataset == "hoyyu_icd":
                stem = os.path.splitext(fn)[0]
                cls_str, sub = hoyyu.get(stem, ("", ""))
                if not cls_str:
                    cls_str = "positive" if "positive" in rel else "negative"
                    sub = stem.split("_")[0]
                label = "cry" if cls_str == "positive" else "noncry"
                fine = sub
            else:
                got = classify(dataset, rel)
                if got is None:
                    n_skip += 1
                    continue
                label, fine = got
            out_rel = os.path.splitext(rel)[0].replace("/", "__") + ".wav"
            dst = os.path.join(wav_root, out_rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if not os.path.isfile(dst):
                r = subprocess.run(
                    ["ffmpeg", "-nostdin", "-y", "-loglevel", "error",
                     "-i", src, "-ac", "1", "-ar", "16000", dst],
                    capture_output=True, text=True)
                if r.returncode != 0:
                    print("FFMPEG FAIL:", rel, r.stderr.strip()[:160])
                    continue
            # all outputs are 16 kHz mono s16le WAV: duration from size
            # (32000 bytes/s + 44-byte RIFF header) - no ffprobe needed
            dur = max(0.0, (os.path.getsize(dst) - 44) / 32000.0)
            stem = os.path.splitext(fn)[0]
            sid = f"{dataset}:{fine}:{stem}" if fine else f"{dataset}:{stem}"
            rows.append({"path": f"data/{dataset}/wav16/{out_rel}",
                         "label": label, "split": "candidate",
                         "source_id": sid, "_dur": dur})
    man = os.path.join(ML_DIR, f"manifest_{dataset}.csv")
    with open(man, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["path", "label", "split", "source_id"])
        for r in rows:
            w.writerow([r["path"], r["label"], r["split"], r["source_id"]])
    cry = [r for r in rows if r["label"] == "cry"]
    nc = [r for r in rows if r["label"] == "noncry"]
    print(f"[{dataset}] converted={len(rows)} (cry={len(cry)} noncry={len(nc)}) "
          f"skipped={n_skip} total_dur={sum(r['_dur'] for r in rows)/3600:.2f} h")
    print(f"manifest snippet: {man}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    convert(sys.argv[1])
