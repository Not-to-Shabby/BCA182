#!/usr/bin/env python3
"""
analyze_sensme.py
Analyzes the SongHub MIDI library to compute Sony SensMe acoustic mood coordinates
(Valence, Energy, BPM, and Mood Channel classification) for every song in the catalog.

Outputs:
  sd_card/sensme.bin: Compact binary database for the RT-Spark's 8MB W25Q64 SPI NOR Flash.

Binary Layout:
  Header (64 bytes):
    uint32_t magic;         // 0x53454E53 ("SENS")
    uint16_t version;       // 1
    uint16_t entry_size;    // 4 bytes
    uint32_t song_count;    // Total songs (e.g. 47,998)
    uint16_t ch_counts[16]; // Song count per SensMe channel (1..12)
    uint32_t data_crc32;    // CRC32 of entry table
    uint8_t  reserved[16];  // Zero padding to 64 bytes

  Entries (song_count * 4 bytes):
    uint8_t valence;        // 0 (Melancholic/Minor) to 255 (Euphoric/Major), 128 = Neutral
    uint8_t energy;         // 0 (Calm/Ambient) to 255 (Intense/Extreme)
    uint8_t channel;        // SensMe Channel ID (1..12)
    uint8_t bpm;            // Tempo in BPM (40..240)
"""

import os
import sys
import json
import struct
import zlib
import time
from concurrent.futures import ProcessPoolExecutor
import numpy as np

# Standard Krumhansl-Schmuckler Key Profiles (12 chromatic pitch classes)
KS_MAJOR = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
KS_MINOR = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])
KS_MAJOR = (KS_MAJOR - KS_MAJOR.mean()) / KS_MAJOR.std()
KS_MINOR = (KS_MINOR - KS_MINOR.mean()) / KS_MINOR.std()

CH_NAMES = [
    "None",
    "Morning",    # 1: Major key, gentle tempo, waking up
    "Daytime",    # 2: Bright, lively pop/accompaniment
    "Evening",    # 3: Warm acoustic, relaxing sunset
    "Night",      # 4: Slow, minor, moody, after-dark
    "Relax",      # 5: Lowest energy, ambient, soft
    "Mellow",     # 6: Smooth ballad, moderate energy
    "Upbeat",     # 7: High positive valence, cheerful, happy
    "Emotional",  # 8: Deep feeling, minor key, strings/flute
    "Lounge",     # 9: Sophisticated jazz harmony, chill
    "Dance",      # 10: Driving rhythm, 4-on-the-floor beat, high BPM
    "Extreme",    # 11: Maximum velocity, distorted guitars/heavy drums
    "Acoustic"    # 12: Natural piano/guitar/woodwinds
]

def analyze_single_midi(args):
    idx, code, midi_path = args
    if not os.path.exists(midi_path):
        return (idx, code, 128, 120, 120, 1) # Default neutral

    try:
        with open(midi_path, "rb") as f:
            d = f.read()
    except Exception:
        return (idx, code, 128, 120, 120, 1)

    if len(d) < 14 or d[:4] != b"MThd":
        return (idx, code, 128, 120, 120, 1)

    ntr = struct.unpack(">H", d[10:12])[0]
    ppq = struct.unpack(">H", d[12:14])[0]
    if ppq == 0 or (ppq & 0x8000):
        ppq = 96

    pos = 14
    tempos = []
    chroma = np.zeros(12, dtype=float)
    total_notes = 0
    drum_notes = 0
    max_tick = 0
    total_vel = 0
    dist_gtr_notes = 0
    acoustic_notes = 0
    prog = {}
    drumch = {9}

    for _ in range(ntr):
        if pos + 8 > len(d) or d[pos:pos+4] != b"MTrk":
            break
        ln = struct.unpack(">I", d[pos+4:pos+8])[0]
        p = pos + 8
        end = min(p + ln, len(d))
        tick = 0
        run = 0

        try:
            while p < end:
                v = 0
                while True:
                    b = d[p]; p += 1; v = (v << 7) | (b & 0x7F)
                    if not (b & 0x80): break
                tick += v
                s = d[p]

                if s == 0xFF:
                    ty = d[p+1]; q = p + 2; l = 0
                    while d[q] & 0x80: l = (l << 7) | (d[q] & 0x7F); q += 1
                    l = (l << 7) | d[q]; q += 1
                    if ty == 0x51 and l == 3 and q + 3 <= len(d):
                        us = (d[q] << 16) | (d[q+1] << 8) | d[q+2]
                        if us > 0:
                            tempos.append((tick, us))
                    p = q + l
                    continue

                if s in (0xF0, 0xF7):
                    q = p + 1; l = 0
                    while d[q] & 0x80: l = (l << 7) | (d[q] & 0x7F); q += 1
                    l = (l << 7) | d[q]; q += 1
                    body = d[q:q+l]
                    if s == 0xF0 and len(body) >= 8 and body[0] == 0x41 and body[4] == 0x40 and (body[5] & 0xF0) == 0x10 and body[6] == 0x15:
                        pp = body[5] & 0x0F
                        ch = 9 if pp == 0 else (pp - 1 if pp <= 9 else pp)
                        if body[7] in (1, 2):
                            drumch.add(ch)
                        elif ch != 9:
                            drumch.discard(ch)
                    p = q + l
                    continue

                if s & 0x80:
                    run = s; p += 1

                hi = run & 0xF0
                ch = run & 15

                if hi in (0xC0, 0xD0):
                    if hi == 0xC0 and p < len(d):
                        prog[ch] = d[p]
                    p += 1
                else:
                    if p + 1 >= len(d): break
                    k, vel = d[p], d[p+1]
                    p += 2
                    if hi == 0x90 and vel > 0:
                        total_notes += 1
                        total_vel += vel
                        if tick > max_tick: max_tick = tick
                        if ch in drumch:
                            drum_notes += 1
                        else:
                            chroma[k % 12] += vel
                            pg = prog.get(ch, 0)
                            if pg in (29, 30):
                                dist_gtr_notes += 1
                            elif pg in (0, 1, 24, 25, 40, 42, 68, 71, 73):
                                acoustic_notes += 1
        except Exception:
            pass
        pos = end

    if max_tick == 0 or total_notes < 10:
        return (idx, code, 128, 120, 120, 1)

    # 1. Time-weighted average tempo & duration
    if not tempos:
        tempos = [(0, 500000)] # 120 BPM
    tempos.sort()
    weighted_us = 0
    total_ticks = 0
    for t_idx in range(len(tempos)):
        t_curr, us_curr = tempos[t_idx]
        t_next = tempos[t_idx + 1][0] if t_idx + 1 < len(tempos) else max_tick
        span = max(0, t_next - t_curr)
        weighted_us += span * us_curr
        total_ticks += span

    avg_us = (weighted_us / max(1, total_ticks)) if total_ticks > 0 else 500000
    bpm = int(round(np.clip(60000000.0 / max(1.0, avg_us), 40.0, 240.0)))
    dur_sec = max(5.0, (max_tick / max(1.0, float(ppq))) * (avg_us / 1e6))

    # 2. Energy (0 to 255)
    note_dens = total_notes / dur_sec
    avg_vel = total_vel / total_notes
    drum_ratio = drum_notes / total_notes

    e_dens = min(255.0, note_dens * 18.0)
    e_vel = min(255.0, max(0.0, (avg_vel - 40.0) * (255.0 / 60.0)))
    e_bpm = min(255.0, max(0.0, (bpm - 60.0) * (255.0 / 100.0)))
    energy = int(np.clip(0.40 * e_dens + 0.35 * e_bpm + 0.25 * e_vel, 0, 255))
    if dist_gtr_notes > total_notes * 0.10:
        energy = min(255, energy + 30)

    # 3. Valence (0 to 255, 128 = Neutral)
    if chroma.std() > 1e-4:
        c_norm = (chroma - chroma.mean()) / chroma.std()
        maj_corrs = [np.dot(c_norm, np.roll(KS_MAJOR, shift)) / 12.0 for shift in range(12)]
        min_corrs = [np.dot(c_norm, np.roll(KS_MINOR, shift)) / 12.0 for shift in range(12)]
        best_maj = max(maj_corrs)
        best_min = max(min_corrs)
        delta = best_maj - best_min
        valence = int(np.clip(128 + delta * 240.0, 0, 255))
    else:
        valence = 128

    # 4. SensMe Channel Classification (1..12)
    if energy >= 200 or dist_gtr_notes > total_notes * 0.20:
        ch = 11  # Extreme
    elif energy >= 155 and bpm >= 115 and drum_ratio > 0.15:
        ch = 10  # Dance
    elif valence >= 155 and energy >= 135:
        ch = 7   # Upbeat
    elif energy <= 70 and note_dens <= 4.0:
        ch = 5   # Relax
    elif valence <= 105 and energy >= 85 and acoustic_notes > 0:
        ch = 8   # Emotional
    elif bpm < 85 and valence <= 120 and energy <= 95:
        ch = 4   # Night
    elif acoustic_notes > total_notes * 0.40 and energy <= 130:
        ch = 12  # Acoustic
    elif energy <= 115 and valence >= 115:
        ch = 6   # Mellow
    elif valence >= 140 and 70 <= energy <= 140:
        ch = 1   # Morning
    elif energy >= 120 and bpm >= 100:
        ch = 2   # Daytime
    elif energy <= 120:
        ch = 3   # Evening
    else:
        ch = 9   # Lounge

    return (idx, code, valence, energy, bpm, ch)

def main():
    root = os.path.dirname(os.path.abspath(__file__))
    json_path = "D:/idx/shub_extracted/shub-songs.json"
    midi_dir = "D:/idx/shub_extracted/midi"
    out_bin = os.path.normpath(os.path.join(root, "..", "sd_card", "sensme.bin"))

    print("=== RT-Spark SensMe Mood Analyzer ===")
    print(f"Loading metadata: {json_path}")
    with open(json_path, "r", encoding="utf-8") as f:
        songs = json.load(f)

    total_songs = len(songs)
    print(f"Total songs to analyze: {total_songs:,}")

    # Prepare jobs
    jobs = []
    for idx, s in enumerate(songs):
        code = int(s.get("song_code", 0) or s.get("code", 0))
        fname = s.get("file", "")
        p = os.path.join(midi_dir, fname)
        jobs.append((idx, code, p))

    t0 = time.time()
    workers = min(16, (os.cpu_count() or 4))
    print(f"Analyzing {total_songs:,} MIDI files across {workers} CPU workers...")

    results = [None] * total_songs
    completed = 0
    with ProcessPoolExecutor(max_workers=workers) as executor:
        for res in executor.map(analyze_single_midi, jobs, chunksize=250):
            idx = res[0]
            results[idx] = res
            completed += 1
            if completed % 2500 == 0 or completed == total_songs:
                pct = (completed * 100.0) / total_songs
                print(f"  [{completed:,}/{total_songs:,}] {pct:5.1f}% completed...", end="\r")

    elapsed = time.time() - t0
    print(f"\nAnalysis completed in {elapsed:.1f} seconds! ({total_songs / max(0.1, elapsed):.0f} songs/sec)")

    # Build binary table
    table_bytes = bytearray()
    ch_counts = [0] * 16

    for r in results:
        idx, code, val, ene, bpm, ch = r
        ch_counts[ch & 15] += 1
        table_bytes.extend(struct.pack("<BBBB", val, ene, ch, bpm))

    data_crc = zlib.crc32(table_bytes) & 0xFFFFFFFF

    # Build 64-byte Header
    header = bytearray(64)
    struct.pack_into("<4sHHII", header, 0,
                     b"SENS",       # magic
                     1,             # version
                     4,             # entry_size
                     total_songs,   # song_count
                     data_crc)      # data_crc32

    # channel counts
    for c in range(16):
        struct.pack_into("<H", header, 16 + c * 2, ch_counts[c])

    os.makedirs(os.path.dirname(out_bin), exist_ok=True)
    with open(out_bin, "wb") as f:
        f.write(header)
        f.write(table_bytes)

    total_bytes = len(header) + len(table_bytes)
    print(f"\nSUCCESS: Generated {out_bin} ({total_bytes / 1024:.1f} KB)")
    print("Channel distribution:")
    for c in range(1, 13):
        cnt = ch_counts[c]
        print(f"  {c:2d}: {CH_NAMES[c]:12s} {cnt:5d} ({cnt * 100.0 / total_songs:5.1f}%)")

if __name__ == "__main__":
    main()
