#!/usr/bin/env python3
"""
generate_songbook.py
Generates a human-readable, printable karaoke songbook text file (songbook.txt)
formatted for easy searching in Windows Notepad (Ctrl + F).

Contains:
  1. Numerical Index: Sorted by Song Code (look up by number)
  2. Alphabetical Index: Sorted by Song Title (look up by title)
"""

import json
import os
import sys

def generate_songbook(json_path, out_txt_path):
    print(f"Reading songs from {json_path}...")
    with open(json_path, 'r', encoding='utf-8') as f:
        songs = json.load(f)

    print(f"Total songs to index: {len(songs):,}")

    # Clean and parse records
    records = []
    for s in songs:
        code = int(s.get('code', 0))
        title = s.get('title', '').strip()
        singer = s.get('singer', '').strip() or 'Unknown'
        lang = s.get('language', 'OPM').strip()
        records.append({
            'code': code,
            'title': title,
            'singer': singer,
            'lang': lang
        })

    # Sort 1: By Song Code
    by_code = sorted(records, key=lambda r: r['code'])

    # Sort 2: By Title (case-insensitive)
    by_title = sorted(records, key=lambda r: (r['title'].upper(), r['singer'].upper()))

    print(f"Writing songbook to {out_txt_path}...")
    with open(out_txt_path, 'w', encoding='utf-8') as f:
        f.write("================================================================================\n")
        f.write("             RT-SPARK EMBEDDED MIDI KARAOKE PLAYER - SONGBOOK                   \n")
        f.write("================================================================================\n")
        f.write("  INSTRUCTIONS:\n")
        f.write("  1. Press Ctrl + F in Notepad to quickly search for any Title or Artist.\n")
        f.write("  2. Note the 5-digit or 6-digit Song Code.\n")
        f.write("  3. On the RT-Spark board, press BOTH UP + DOWN buttons to open NUMBER SELECT.\n")
        f.write("  4. Enter the digits using LEFT/RIGHT (move) and UP/DOWN (change).\n")
        f.write("  5. Hold UP/DOWN or click CENTER (PA0) to immediately play the song!\n")
        f.write("================================================================================\n\n")

        # PART 1: NUMERICAL DIRECTORY
        f.write("################################################################################\n")
        f.write("  PART 1: NUMERICAL DIRECTORY (SORTED BY SONG CODE)\n")
        f.write("################################################################################\n")
        f.write(f"{'CODE':<8} | {'TITLE':<38} | {'ARTIST':<24} | {'LANG'}\n")
        f.write("-" * 80 + "\n")

        for r in by_code:
            code_str = f"{r['code']:05d}" if r['code'] < 100000 else f"{r['code']:06d}"
            title_str = (r['title'][:36] + '..') if len(r['title']) > 38 else r['title']
            singer_str = (r['singer'][:22] + '..') if len(r['singer']) > 24 else r['singer']
            f.write(f"{code_str:<8} | {title_str:<38} | {singer_str:<24} | {r['lang']}\n")

        f.write("\n\n")
        # PART 2: ALPHABETICAL DIRECTORY
        f.write("################################################################################\n")
        f.write("  PART 2: ALPHABETICAL DIRECTORY (SORTED BY SONG TITLE)\n")
        f.write("################################################################################\n")
        f.write(f"{'TITLE':<38} | {'CODE':<8} | {'ARTIST':<24} | {'LANG'}\n")
        f.write("-" * 80 + "\n")

        for r in by_title:
            code_str = f"{r['code']:05d}" if r['code'] < 100000 else f"{r['code']:06d}"
            title_str = (r['title'][:36] + '..') if len(r['title']) > 38 else r['title']
            singer_str = (r['singer'][:22] + '..') if len(r['singer']) > 24 else r['singer']
            f.write(f"{title_str:<38} | {code_str:<8} | {singer_str:<24} | {r['lang']}\n")

    size_mb = os.path.getsize(out_txt_path) / (1024 * 1024)
    print(f"[Success] Generated {out_txt_path} ({size_mb:.2f} MB)")

if __name__ == '__main__':
    default_json = "D:/idx/shub_extracted/shub-songs.json"
    default_out = "D:/BCA182_Lab/Side_Quest_for_LAB_2/songbook.txt"
    json_p = sys.argv[1] if len(sys.argv) > 1 else default_json
    out_p = sys.argv[2] if len(sys.argv) > 2 else default_out
    generate_songbook(json_p, out_p)
