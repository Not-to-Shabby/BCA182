#!/usr/bin/env python3
"""
find_heavy_songs.py [--top 40] [--out heavy_songs.txt]

Ranks the SongHub library by how long a song keeps the synthesizer busy, not by its single
loudest chord: the time-weighted average number of sounding voices, the share of the song above
a voice count, and the drum-hit rate. Notes are held to their note-off (or the sustain pedal)
plus a short release tail. The first pass is a cheap estimate; run bench.c's "cost" mode on the
top of this list to measure the real synth.
"""
import argparse
import os
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor

MIDI_DIR = "D:/idx/shub_extracted/midi"
TAIL_S = 0.35          # release time added to every note
MAX_SECONDS = 600.0    # a runaway file must not dominate the ranking


def vlq(d, p):
    v = 0
    while p < len(d):
        b = d[p]
        p += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            break
    return v, p


def analyze(path):
    try:
        d = open(path, "rb").read()
        if len(d) < 14 or d[:4] != b"MThd":
            return None
        ppqn = struct.unpack(">H", d[12:14])[0]
        if ppqn == 0 or ppqn & 0x8000:
            return None
        pos = 8 + struct.unpack(">I", d[4:8])[0]
        events = []          # (tick, kind, ch, note)  kind 1 on, 0 off, 2 pedal down, 3 pedal up
        tempos = []
        drums = []
        while pos + 8 <= len(d):
            cid = d[pos:pos + 4]
            ln = struct.unpack(">I", d[pos + 4:pos + 8])[0]
            pos += 8
            end = min(pos + ln, len(d))
            if cid != b"MTrk":
                pos = end
                continue
            p, tick, rs = pos, 0, 0
            while p < end:
                dt, p = vlq(d, p)
                tick += dt
                if p >= end:
                    break
                b = d[p]
                if b & 0x80:
                    st = b
                    p += 1
                else:
                    st = rs
                if st == 0xFF:
                    mt = d[p]
                    ml, p = vlq(d, p + 1)
                    if mt == 0x51 and ml == 3:
                        tempos.append((tick, (d[p] << 16) | (d[p + 1] << 8) | d[p + 2]))
                    p += ml
                    continue
                if st in (0xF0, 0xF7):
                    ml, p = vlq(d, p)
                    p += ml
                    continue
                rs = st
                c, ch = st & 0xF0, st & 0x0F
                if c in (0xC0, 0xD0):
                    p += 1
                    continue
                n, v = d[p], d[p + 1]
                p += 2
                if c == 0x90 and v > 0:
                    (drums if ch == 9 else events).append((tick, 1, ch, n))
                elif c == 0x80 or (c == 0x90 and v == 0):
                    if ch != 9:
                        events.append((tick, 0, ch, n))
                elif c == 0xB0 and n == 64:
                    events.append((tick, 2 if v >= 64 else 3, ch, 0))
            pos = end
        if not events:
            return None

        tempos.sort()
        if not tempos or tempos[0][0] != 0:
            tempos.insert(0, (0, 500000))

        def to_s(tk):                     # tick -> seconds through the tempo map
            t, last_tk, us = 0.0, 0, tempos[0][1]
            for ttk, tus in tempos:
                if ttk >= tk:
                    break
                t += (ttk - last_tk) * us / ppqn / 1e6
                last_tk, us = ttk, tus
            return t + (tk - last_tk) * us / ppqn / 1e6

        events.sort(key=lambda e: (e[0], -e[1]))
        held, pedal, pending = {}, [False] * 16, {}
        spans = []                         # (start_s, end_s)
        for tk, kind, ch, n in events:
            t = to_s(tk)
            if kind == 1:
                prev = held.pop((ch, n), None)
                if prev is not None:
                    # the synth releases a key that is struck again while still down
                    spans.append((prev, t + TAIL_S))
                held[(ch, n)] = t
            elif kind == 0:
                s = held.pop((ch, n), None)
                if s is not None:
                    if pedal[ch]:
                        pending.setdefault(ch, []).append(s)
                    else:
                        spans.append((s, t + TAIL_S))
            elif kind == 2:
                pedal[ch] = True
            else:
                pedal[ch] = False
                for s in pending.pop(ch, []):
                    spans.append((s, t + TAIL_S))
        for s in held.values():
            spans.append((s, s + 1.0))
        for lst in pending.values():
            for s in lst:
                spans.append((s, s + 1.0))
        if not spans:
            return None

        total = min(max(e for _, e in spans), MAX_SECONDS)
        step = 0.05
        n = int(total / step) + 1
        diff = [0] * (n + 2)
        for s, e in spans:
            a, b = int(s / step), int(min(e, total) / step)
            if a <= n:
                diff[a] += 1
                diff[min(b, n) + 1] -= 1
        counts, run = [], 0
        for i in range(n):
            run += diff[i]
            counts.append(min(run, 70))        # the synth has 70 voices; more is demand it cannot serve
        active = [c for c in counts if c > 0]
        if len(active) < 20:
            return None
        srt = sorted(active)
        mean = sum(active) / len(active)
        return {
            "path": path,
            "seconds": total,
            "mean": mean,
            "p95": srt[int(len(srt) * 0.95)],
            "peak": srt[-1],
            "over40": sum(1 for c in active if c > 40) / len(active),
            "over54": sum(1 for c in active if c > 54) / len(active),
            "drums_per_s": len(drums) / total,
        }
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--out", default="heavy_songs.txt")
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    files = sorted(os.path.join(MIDI_DIR, f) for f in os.listdir(MIDI_DIR) if f.lower().endswith(".mid"))
    if a.limit:
        files = files[:a.limit]
    t0 = time.time()
    rows = []
    with ProcessPoolExecutor() as ex:
        for i, r in enumerate(ex.map(analyze, files, chunksize=64)):
            if r:
                rows.append(r)
            if i % 8000 == 0:
                print(f"  {i}/{len(files)} scanned, {time.time() - t0:.0f} s", file=sys.stderr)
    print(f"scanned {len(files)} files, {len(rows)} usable, {time.time() - t0:.0f} s")

    rows = [r for r in rows if 60 <= r["seconds"] <= 480]       # real songs, not stubs or runaways
    rows.sort(key=lambda r: (r["mean"] + 20 * r["over54"] + 0.5 * r["drums_per_s"]), reverse=True)
    lines = [f"{'RANK':<5}{'CODE':<8}{'MEAN':>6}{'P95':>5}{'PEAK':>6}{'>40':>6}{'>54':>6}{'DRUM/s':>8}{'SEC':>6}  NAME"]
    for i, r in enumerate(rows[:a.top], 1):
        name = os.path.basename(r["path"])
        lines.append(f"{i:<5}{name[:6]:<8}{r['mean']:>6.1f}{r['p95']:>5}{r['peak']:>6}"
                     f"{r['over40'] * 100:>5.0f}%{r['over54'] * 100:>5.0f}%{r['drums_per_s']:>8.1f}{r['seconds']:>6.0f}  {name[9:-4]}")
    text = "\n".join(lines)
    print(text)
    open(a.out, "w").write(text + "\n")


if __name__ == "__main__":
    main()
