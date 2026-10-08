#!/usr/bin/env python3
"""
prepare_karaoke_sd.py
Prepares MicroSD card contents for the RT-Spark MIDI Karaoke Player.

Converts shub-songs.json into a compact binary index file (songs.idx)
using fixed-size 96-byte records and copies the corresponding MIDI
files into /midi/ on the MicroSD card.

MIDI files go into one folder per thousand song codes, so no FAT32 folder
holds more than 1000 files:

  midi/036/036527.mid      (song code 36527 -> folder 036)

The firmware builds the same path from the song code (src/song_path.c) and
still accepts a flat midi/036527.mid card.

Binary Record Layout (96 bytes):
  uint32_t song_code;    // Offset 0 (4 bytes, Little Endian)
  char title[40];        // Offset 4 (40 bytes, ASCII null-terminated)
  char singer[28];       // Offset 44 (28 bytes)
  char language[8];      // Offset 72 (8 bytes)
  char filename[16];     // Offset 80 (16 bytes, e.g. "036527.mid", no folder)
"""

import os
import sys
import json
import struct
import shutil
import argparse
import collections
import time
from concurrent.futures import ThreadPoolExecutor

RECORD_FORMAT = '<I40s28s8s16s'
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)
assert RECORD_SIZE == 96, f"Expected 96 bytes, got {RECORD_SIZE}"

def truncate_str(text, max_len):
    encoded = text.encode('latin1', errors='replace')
    if len(encoded) >= max_len:
        encoded = encoded[:max_len - 1]
    return encoded + b'\x00' * (max_len - len(encoded))

def shard_dir(code):
    """Folder holding a song: its code in thousands, zero padded (36527 -> '036').
    Keep in sync with song_midi_path() in src/song_path.c."""
    return f"{code // 1000:03d}"

def copy_file_task(src_path, dst_path):
    """Copy one file. Returns 'copied', 'present', 'missing' or 'error: <reason>'."""
    if os.path.exists(dst_path):
        return 'present'
    if not os.path.exists(src_path):
        return 'missing'
    try:
        shutil.copy2(src_path, dst_path)
    except OSError as exc:
        return f'error: {exc}'
    return 'copied'

def build_karaoke_sd(json_path, midi_dir, out_dir, limit=None, lang_filter=None, copy_files=True):
    # Normalize Windows drive letter path if provided (e.g. "E:" -> "E:/")
    if len(out_dir) == 2 and out_dir[1] == ':':
        out_dir = out_dir + '/'

    print("=====================================================")
    print("  RT-Spark Karaoke SD Card Preparation Utility      ")
    print("=====================================================")
    print(f"Loading metadata from: {json_path}")

    start_time = time.time()
    with open(json_path, 'r', encoding='utf-8') as f:
        songs = json.load(f)

    total_loaded = len(songs)
    print(f"Total songs in database: {total_loaded:,}")

    # Optional Language Filtering (e.g. OPM, English)
    if lang_filter:
        lang_filter = lang_filter.strip().upper()
        songs = [s for s in songs if s.get('language', '').upper() == lang_filter]
        print(f"Filtered to language '{lang_filter}': {len(songs):,} songs")

    # Sort songs by numeric song code for fast binary search on embedded player
    songs.sort(key=lambda s: int(s.get('code', 0)))

    if limit is not None and limit > 0:
        songs = songs[:limit]
        print(f"Target count limited to: {len(songs):,} songs")
    else:
        print(f"Target count: All {len(songs):,} songs")

    os.makedirs(out_dir, exist_ok=True)
    out_midi_dir = os.path.join(out_dir, "midi")
    os.makedirs(out_midi_dir, exist_ok=True)

    idx_file = os.path.join(out_dir, "songs.idx")

    print(f"\n1. Writing binary index to: {idx_file}")
    copy_jobs = []
    per_shard = collections.Counter()

    with open(idx_file, 'wb') as f_idx:
        # 16-byte index header
        header = struct.pack('<4sHHII', b'KIDX', 1, RECORD_SIZE, len(songs), 0)
        f_idx.write(header)

        for s in songs:
            code = int(s.get('code', 0))
            title = s.get('title', 'Unknown')
            singer = s.get('singer', 'Unknown')
            lang = s.get('language', 'OPM')
            orig_file = s.get('file', f"{code:06d}.mid")

            short_fname = f"{code:06d}.mid"

            rec = struct.pack(
                RECORD_FORMAT,
                code,
                truncate_str(title, 40),
                truncate_str(singer, 28),
                truncate_str(lang, 8),
                truncate_str(short_fname, 16)
            )
            f_idx.write(rec)

            if copy_files:
                shard = shard_dir(code)
                per_shard[shard] += 1
                src_path = os.path.join(midi_dir, orig_file)
                dst_path = os.path.join(out_midi_dir, shard, short_fname)
                copy_jobs.append((src_path, dst_path))

    idx_size_kb = os.path.getsize(idx_file) / 1024
    print(f"   [OK] Generated {idx_file} ({idx_size_kb:.1f} KB)")

    failures = 0
    if copy_files and copy_jobs:
        for shard in per_shard:
            os.makedirs(os.path.join(out_midi_dir, shard), exist_ok=True)

        print(f"\n2. Copying {len(copy_jobs):,} MIDI files into {len(per_shard)} folders under {out_midi_dir}...")
        outcome = collections.Counter()
        errors = []

        # Fast parallel copy using ThreadPoolExecutor
        workers = min(16, (os.cpu_count() or 4) * 2)
        with ThreadPoolExecutor(max_workers=workers) as executor:
            batch_size = 500
            for i in range(0, len(copy_jobs), batch_size):
                batch = copy_jobs[i:i + batch_size]
                futures = [executor.submit(copy_file_task, src, dst) for src, dst in batch]
                for (src, _dst), future in zip(batch, futures):
                    result = future.result()
                    if result.startswith('error'):
                        outcome['error'] += 1
                        errors.append(f"{src}: {result}")
                    else:
                        outcome[result] += 1
                progress = min(100.0, (i + len(batch)) * 100.0 / len(copy_jobs))
                print(f"   Progress: {progress:5.1f}% ({i + len(batch):,}/{len(copy_jobs):,} files)...", end='\r')

        print(f"\n   Copied {outcome['copied']:,}, already present {outcome['present']:,}, "
              f"missing source {outcome['missing']:,}, errors {outcome['error']:,}.")
        for line in errors[:5]:
            print(f"   [!] {line}")
        failures = outcome['missing'] + outcome['error']

    elapsed = time.time() - start_time
    if failures:
        print(f"\nFinished in {elapsed:.1f} seconds, but {failures:,} songs were NOT copied: the card is incomplete.")
        sys.exit(1)

    print(f"\nDone in {elapsed:.1f} seconds!")
    print(f"MicroSD Card is ready for RT-Spark!")
    print(f"Contents:")
    print(f"  - {os.path.join(out_dir, 'songs.idx')}")
    if per_shard:
        print(f"  - {out_midi_dir}/<NNN>/*.mid ({len(per_shard)} folders, largest {max(per_shard.values()):,} files)")

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Prepare MicroSD Card for RT-Spark MIDI Karaoke")
    parser.add_argument("--out", required=True, help="Destination directory or drive letter (e.g. E: or /d/karaoke_sd)")
    parser.add_argument("--json", default="D:/idx/shub_extracted/shub-songs.json", help="Path to shub-songs.json")
    parser.add_argument("--midi", default="D:/idx/shub_extracted/midi", help="Path to extracted midi directory")
    parser.add_argument("--limit", type=int, default=0, help="Number of songs to copy (0 = all songs)")
    parser.add_argument("--lang", default=None, help="Filter by language (e.g. OPM, English)")
    parser.add_argument("--no-copy", action="store_true", help="Only build index, do not copy midi files")
    args = parser.parse_args()

    limit_val = None if args.limit == 0 else args.limit
    build_karaoke_sd(
        json_path=args.json,
        midi_dir=args.midi,
        out_dir=args.out,
        limit=limit_val,
        lang_filter=args.lang,
        copy_files=not args.no_copy
    )
