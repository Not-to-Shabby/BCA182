#!/usr/bin/env python3
"""
find_highest_voices.py
Scans the SongHub MIDI library to identify tracks with the highest simultaneous
polyphony (peak active voices), total note events, and densest musical passages.

Usage:
  python tools/find_highest_voices.py [--top 25] [--limit 0] [--out report.txt]
"""

import os
import sys
import json
import time
import struct
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed

def analyze_single_midi(args_tuple):
    file_path, code, title, singer, lang = args_tuple
    if not os.path.exists(file_path):
        return None
        
    try:
        with open(file_path, 'rb') as f:
            d = f.read()
    except Exception:
        return None
        
    if len(d) < 14 or d[:4] != b'MThd':
        return None

    try:
        fmt, ntrk, ppqn = struct.unpack('>HHH', d[8:14])
        pos = 14
        
        def vlq(p):
            v = 0
            while p < len(d):
                b = d[p]; p += 1
                v = (v << 7) | (b & 0x7F)
                if not (b & 0x80): break
            return v, p
            
        all_events = []
        note_on_count = 0
        total_ticks = 0
        
        for _ in range(ntrk):
            if pos + 8 > len(d): break
            if d[pos:pos+4] != b'MTrk':
                pos += 8 + struct.unpack('>I', d[pos+4:pos+8])[0]
                continue
            ln = struct.unpack('>I', d[pos+4:pos+8])[0]; pos += 8
            end = min(pos + ln, len(d))
            p = pos; cur_tick = 0; rs = 0
            
            while p < end:
                dt, p = vlq(p); cur_tick += dt
                if p >= end: break
                b = d[p]
                if b & 0x80: st = b; p += 1
                else: st = rs
                rs = st
                c = st & 0xF0
                ch = st & 0x0F
                
                if c == 0x90:
                    note = d[p]; vel = d[p+1]; p += 2
                    if vel > 0:
                        all_events.append((cur_tick, 1, ch, note))
                        note_on_count += 1
                    else:
                        all_events.append((cur_tick, 0, ch, note))
                elif c == 0x80:
                    note = d[p]; p += 2
                    all_events.append((cur_tick, 0, ch, note))
                elif c == 0xB0:
                    ctrl = d[p]; val = d[p+1]; p += 2
                    if ctrl == 64: # Sustain pedal CC64
                        all_events.append((cur_tick, 2 if val >= 64 else 3, ch, 0))
                elif c in (0xC0, 0xD0):
                    p += 1
                elif c in (0xA0, 0xE0):
                    p += 2
                elif st == 0xFF:
                    p += 1; ml, p = vlq(p); p += ml
                elif st in (0xF0, 0xF7):
                    ml, p = vlq(p); p += ml
            if cur_tick > total_ticks:
                total_ticks = cur_tick
            pos = end

        # Sort events chronologically. If ticks match, process Note Offs (0) before Note Ons (1)
        all_events.sort(key=lambda e: (e[0], 0 if e[1] == 0 else 1))
        
        # Simulate polyphony with sustain pedal
        active_voices = 0
        peak_voices = 0
        active_keys_only = 0
        peak_keys_only = 0
        
        keys_down = [[0]*128 for _ in range(16)]
        pedal_held = [[False]*128 for _ in range(16)]
        pedal_down = [False]*16
        
        for tick, etype, ch, note in all_events:
            if etype == 1: # Note On
                if keys_down[ch][note] == 0 and not pedal_held[ch][note]:
                    active_voices += 1
                if keys_down[ch][note] == 0:
                    active_keys_only += 1
                keys_down[ch][note] += 1
                pedal_held[ch][note] = False
                
                if active_voices > peak_voices:
                    peak_voices = active_voices
                if active_keys_only > peak_keys_only:
                    peak_keys_only = active_keys_only
            elif etype == 0: # Note Off
                if keys_down[ch][note] > 0:
                    keys_down[ch][note] -= 1
                    if keys_down[ch][note] == 0:
                        active_keys_only -= 1
                        if pedal_down[ch]:
                            pedal_held[ch][note] = True
                        else:
                            active_voices -= 1
            elif etype == 2: # Pedal Down
                pedal_down[ch] = True
            elif etype == 3: # Pedal Up
                pedal_down[ch] = False
                for n in range(128):
                    if pedal_held[ch][n]:
                        pedal_held[ch][n] = False
                        if keys_down[ch][n] == 0:
                            active_voices -= 1
                            
        return {
            'code': code,
            'title': title,
            'singer': singer,
            'lang': lang,
            'peak_voices': peak_voices,
            'peak_keys': peak_keys_only,
            'note_ons': note_on_count,
            'tracks': ntrk,
            'file_size': len(d)
        }
    except Exception:
        return None

def main():
    parser = argparse.ArgumentParser(description="Find MIDI files with the highest simultaneous voice polyphony")
    parser.add_argument("--json", default="D:/idx/shub_extracted/shub-songs.json", help="Path to shub-songs.json")
    parser.add_argument("--midi", default="D:/idx/shub_extracted/midi", help="Path to extracted midi directory")
    parser.add_argument("--top", type=int, default=30, help="Number of top songs to display")
    parser.add_argument("--limit", type=int, default=0, help="Scan only first N songs (0 = all songs)")
    parser.add_argument("--out", default=None, help="Save report to file")
    args = parser.parse_args()

    print("================================================================================")
    print("           SONGHUB MIDI LIBRARY - POLYPHONY & VOICE ANALYZER                    ")
    print("================================================================================")
    print(f"Loading metadata index: {args.json}")
    
    with open(args.json, 'r', encoding='utf-8') as f:
        songs = json.load(f)
        
    if args.limit > 0:
        songs = songs[:args.limit]
        print(f"Analyzing {len(songs):,} songs (limited)...")
    else:
        print(f"Analyzing complete database of {len(songs):,} songs...")

    tasks = []
    for s in songs:
        file_path = os.path.join(args.midi, s['file'])
        tasks.append((file_path, int(s['code']), s['title'], s['singer'], s['language']))

    start_time = time.time()
    results = []
    total_tasks = len(tasks)
    workers = min(16, (os.cpu_count() or 4))
    
    print(f"Running parallel polyphony simulation with {workers} worker processes...\n")
    
    with ProcessPoolExecutor(max_workers=workers) as executor:
        batch_size = 500
        for i in range(0, total_tasks, batch_size):
            batch = tasks[i:i+batch_size]
            batch_results = list(executor.map(analyze_single_midi, batch))
            for res in batch_results:
                if res is not None:
                    results.append(res)
                    
            done_count = min(total_tasks, i + batch_size)
            pct = done_count * 100.0 / total_tasks
            speed = done_count / max(0.1, time.time() - start_time)
            print(f"  Progress: {pct:5.1f}% ({done_count:,}/{total_tasks:,} songs) | Speed: {speed:.0f} songs/sec...", end='\r')

    elapsed = time.time() - start_time
    print(f"\n\nAnalysis complete in {elapsed:.1f} seconds! ({len(results):,} songs analyzed)")

    # Sort by peak voices (descending), then total note ons
    results.sort(key=lambda r: (r['peak_voices'], r['note_ons']), reverse=True)

    header = f"{'RANK':<5} | {'CODE':<7} | {'PEAK VOICES':<11} | {'KEYS':<6} | {'NOTES':<7} | {'TITLE':<32} | {'ARTIST'}"
    divider = "-" * 100
    
    report_lines = []
    report_lines.append("\n================================================================================")
    report_lines.append(f"       TOP {args.top} SONGS WITH THE HIGHEST SIMULTANEOUS VOICES (POLYPHONY)    ")
    report_lines.append("================================================================================")
    report_lines.append(header)
    report_lines.append(divider)
    
    for rank, r in enumerate(results[:args.top], start=1):
        line = f"#{rank:<4} | {r['code']:05d}   | {r['peak_voices']:<11} | {r['peak_keys']:<6} | {r['note_ons']:<7,} | {r['title'][:30]:<32} | {r['singer'][:24]}"
        report_lines.append(line)
        
    report_lines.append(divider)
    
    # Statistical Summary
    voices_list = [r['peak_voices'] for r in results]
    v_max = max(voices_list)
    v_min = min(voices_list)
    v_avg = sum(voices_list) / len(voices_list)
    v_sorted = sorted(voices_list)
    v_p90 = v_sorted[int(len(v_sorted) * 0.90)]
    v_p95 = v_sorted[int(len(v_sorted) * 0.95)]
    v_p99 = v_sorted[int(len(v_sorted) * 0.99)]
    
    report_lines.append("\n================================================================================")
    report_lines.append("                         POLYPHONY DISTRIBUTION SUMMARY                         ")
    report_lines.append("================================================================================")
    report_lines.append(f"  Total Songs Analyzed:       {len(results):,}")
    report_lines.append(f"  Maximum Simultaneous Voice: {v_max} voices")
    report_lines.append(f"  Minimum Simultaneous Voice: {v_min} voices")
    report_lines.append(f"  Average Peak Polyphony:     {v_avg:.1f} voices")
    report_lines.append(f"  90th Percentile Polyphony:  {v_p90} voices  (90% of all songs peak at or below this)")
    report_lines.append(f"  95th Percentile Polyphony:  {v_p95} voices  (95% of all songs peak at or below this)")
    report_lines.append(f"  99th Percentile Polyphony:  {v_p99} voices  (99% of all songs peak at or below this)")
    report_lines.append(f"  Songs with > 40 voices:     {sum(1 for v in voices_list if v > 40):,} ({sum(1 for v in voices_list if v > 40)*100.0/len(voices_list):.2f}%)")
    report_lines.append(f"  Songs with > 70 voices:     {sum(1 for v in voices_list if v > 70):,} ({sum(1 for v in voices_list if v > 70)*100.0/len(voices_list):.2f}%)")
    report_lines.append("================================================================================\n")

    report_text = "\n".join(report_lines)
    print(report_text)
    
    out_file = args.out or "highest_voices_report.txt"
    with open(out_file, 'w', encoding='utf-8') as f:
        f.write(report_text)
    print(f"Report saved to: {os.path.abspath(out_file)}")

if __name__ == '__main__':
    main()
