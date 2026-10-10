#!/usr/bin/env python3
"""
gen_opl4_drums.py
Generates 16-bit 22,050 Hz acoustic/metallic PCM drum samples for the OPL4 hybrid
WaveTable drum engine and writes them as const int16_t Flash tables to
include/opl4_drum_samples.h.
"""

import math
import random
import os

FS = 22050.0

def clamp16(x):
    v = int(round(x))
    return max(-32767, min(32767, v))

def gen_kick():
    """Acoustic Bass Drum: beater click + punchy nonlinear pitch sweep + warm shell body."""
    n = int(0.18 * FS)
    out = []
    phase = 0.0
    phase2 = 0.0
    rng = random.Random(101)
    lp = 0.0
    for i in range(n):
        t = i / FS
        f = 47.0 + 135.0 * math.exp(-t / 0.022) + 45.0 * math.exp(-t / 0.005)
        phase += 2.0 * math.pi * f / FS
        phase2 += 2.0 * math.pi * (f * 2.0) / FS
        body = math.sin(phase) * math.exp(-t / 0.095)
        punch = 0.35 * math.sin(phase2) * math.exp(-t / 0.025)
        click_nz = (rng.uniform(-1.0, 1.0)) * math.exp(-t / 0.0035)
        lp += 0.35 * (click_nz - lp)
        val = (body * 0.82 + punch + lp * 0.45) * 31000.0
        out.append(clamp16(val))
    return out

def gen_snare():
    """Acoustic Snare: dual drumhead modes (185 Hz + 330 Hz) + stick snap + snare wire rattle."""
    n = int(0.18 * FS)
    out = []
    p1 = 0.0
    p2 = 0.0
    rng = random.Random(202)
    prev = 0.0
    bp = 0.0
    for i in range(n):
        t = i / FS
        f1 = 172.0 + 65.0 * math.exp(-t / 0.015)
        f2 = 325.0 + 90.0 * math.exp(-t / 0.010)
        p1 += 2.0 * math.pi * f1 / FS
        p2 += 2.0 * math.pi * f2 / FS
        head = (0.65 * math.sin(p1) + 0.35 * math.sin(p2)) * math.exp(-t / 0.065)
        raw = rng.uniform(-1.0, 1.0)
        hp = raw - prev
        prev = raw
        bp += 0.45 * (hp - bp)
        wires = bp * (0.85 * math.exp(-t / 0.115) + 0.25 * math.exp(-t / 0.020))
        val = (head * 0.58 + wires * 0.62) * 30000.0
        out.append(clamp16(val))
    return out

def gen_stick():
    """Side Stick / Rimshot: woody 1.7 kHz + 3.1 kHz resonant click."""
    n = int(0.065 * FS)
    out = []
    rng = random.Random(303)
    for i in range(n):
        t = i / FS
        wood = (0.6 * math.sin(2.0 * math.pi * 1680.0 * t) +
                0.4 * math.sin(2.0 * math.pi * 3120.0 * t) +
                0.25 * math.sin(2.0 * math.pi * 820.0 * t)) * math.exp(-t / 0.016)
        click = rng.uniform(-1.0, 1.0) * math.exp(-t / 0.004)
        out.append(clamp16((wood * 0.75 + click * 0.45) * 30000.0))
    return out

def gen_clap():
    """TR-909 / RX5 style multi-burst Hand Clap (4 micro-slaps at 0, 10, 21, 32 ms + room tail)."""
    n = int(0.14 * FS)
    out = []
    rng = random.Random(404)
    bp1 = 0.0
    bp2 = 0.0
    for i in range(n):
        t = i / FS
        env = 0.0
        for d in (0.0, 0.010, 0.021, 0.032):
            if t >= d:
                env += math.exp(-(t - d) / 0.0045)
        if t >= 0.032:
            env += 0.42 * math.exp(-(t - 0.032) / 0.075)
        raw = rng.uniform(-1.0, 1.0)
        bp1 += 0.32 * (raw - bp1)
        bp2 += 0.08 * (bp1 - bp2)
        band = bp1 - bp2
        out.append(clamp16(band * env * 28000.0))
    return out

def bronze_matrix(t, freqs):
    """6-oscillator inharmonic metallic cymbal matrix."""
    s = 0.0
    for idx, f in enumerate(freqs):
        s += math.sin(2.0 * math.pi * f * t + 0.7 * math.sin(2.0 * math.pi * (f * 1.414) * t)) * (1.0 / (1.0 + 0.12 * idx))
    return s / len(freqs)

def gen_hat_closed():
    """Closed Hi-Hat: crisp metallic 6-oscillator bronze matrix + high-passed stick sizzle."""
    n = int(0.055 * FS)
    out = []
    rng = random.Random(505)
    prev = 0.0
    freqs = (3140.0, 4380.0, 5690.0, 6920.0, 8150.0, 9480.0)
    for i in range(n):
        t = i / FS
        metal = bronze_matrix(t, freqs)
        raw = rng.uniform(-1.0, 1.0)
        hp = raw - prev
        prev = raw
        env = math.exp(-t / 0.022)
        out.append(clamp16((metal * 0.45 + hp * 0.42) * env * 29000.0))
    return out

def gen_hat_open():
    """Open Hi-Hat: sustained metallic bronze shimmer."""
    n = int(0.28 * FS)
    out = []
    rng = random.Random(606)
    prev = 0.0
    freqs = (3080.0, 4290.0, 5580.0, 6810.0, 8040.0, 9320.0)
    for i in range(n):
        t = i / FS
        metal = bronze_matrix(t, freqs)
        raw = rng.uniform(-1.0, 1.0)
        hp = raw - prev
        prev = raw
        env = 0.35 * math.exp(-t / 0.025) + 0.65 * math.exp(-t / 0.18)
        out.append(clamp16((metal * 0.48 + hp * 0.38) * env * 28000.0))
    return out

def gen_crash():
    """Crash Cymbal: rich bronze wash + splash transient."""
    n = int(0.45 * FS)
    out = []
    rng = random.Random(707)
    prev = 0.0
    freqs = (2480.0, 3420.0, 4690.0, 5890.0, 7240.0, 8810.0)
    for i in range(n):
        t = i / FS
        metal = bronze_matrix(t, freqs)
        raw = rng.uniform(-1.0, 1.0)
        hp = raw - prev
        prev = raw
        env = 0.40 * math.exp(-t / 0.035) + 0.60 * math.exp(-t / 0.28)
        out.append(clamp16((metal * 0.50 + hp * 0.36) * env * 28000.0))
    return out

def gen_ride():
    """Ride Cymbal: clear 3.35 kHz bell ping + warm bronze sustain."""
    n = int(0.35 * FS)
    out = []
    rng = random.Random(808)
    prev = 0.0
    freqs = (2750.0, 3350.0, 4820.0, 6190.0, 7650.0, 9100.0)
    for i in range(n):
        t = i / FS
        ping = math.sin(2.0 * math.pi * 3350.0 * t) * math.exp(-t / 0.14)
        metal = bronze_matrix(t, freqs) * math.exp(-t / 0.22)
        raw = rng.uniform(-1.0, 1.0)
        hp = (raw - prev) * math.exp(-t / 0.18)
        prev = raw
        out.append(clamp16((ping * 0.38 + metal * 0.42 + hp * 0.22) * 28000.0))
    return out

def gen_tom():
    """Acoustic Tom (130 Hz reference, pitch-shifted across all toms and congas)."""
    n = int(0.18 * FS)
    out = []
    phase = 0.0
    rng = random.Random(909)
    for i in range(n):
        t = i / FS
        f = 130.0 + 75.0 * math.exp(-t / 0.030)
        phase += 2.0 * math.pi * f / FS
        body = (math.sin(phase) + 0.22 * math.sin(phase * 1.58)) * math.exp(-t / 0.11)
        stick = rng.uniform(-1.0, 1.0) * math.exp(-t / 0.006)
        out.append(clamp16((body * 0.85 + stick * 0.20) * 30000.0))
    return out

def gen_cowbell():
    """808/Acoustic Cowbell: 540 Hz + 800 Hz inharmonic metallic pair."""
    n = int(0.10 * FS)
    out = []
    for i in range(n):
        t = i / FS
        s1 = math.sin(2.0 * math.pi * 540.0 * t + 0.4 * math.sin(2.0 * math.pi * 540.0 * t))
        s2 = math.sin(2.0 * math.pi * 800.0 * t + 0.4 * math.sin(2.0 * math.pi * 800.0 * t))
        env = 0.5 * math.exp(-t / 0.015) + 0.5 * math.exp(-t / 0.055)
        out.append(clamp16((s1 * 0.55 + s2 * 0.45) * env * 28000.0))
    return out

def write_header(path):
    samples = [
        ("OPL4_PCM_KICK",    gen_kick()),
        ("OPL4_PCM_SNARE",   gen_snare()),
        ("OPL4_PCM_STICK",   gen_stick()),
        ("OPL4_PCM_CLAP",    gen_clap()),
        ("OPL4_PCM_HAT_C",   gen_hat_closed()),
        ("OPL4_PCM_HAT_O",   gen_hat_open()),
        ("OPL4_PCM_CRASH",   gen_crash()),
        ("OPL4_PCM_RIDE",    gen_ride()),
        ("OPL4_PCM_TOM",     gen_tom()),
        ("OPL4_PCM_COWBELL", gen_cowbell()),
    ]
    total_bytes = sum(len(arr) * 2 for _, arr in samples)
    lines = [
        "/**",
        " * @file opl4_drum_samples.h",
        f" * @brief Yamaha OPL4 (YMF278B) style 16-bit 22,050 Hz PCM WaveTable drum samples ({total_bytes} bytes in Flash).",
        " *        Generated by tools/gen_opl4_drums.py.",
        " */",
        "",
        "#ifndef OPL4_DRUM_SAMPLES_H_",
        "#define OPL4_DRUM_SAMPLES_H_",
        "",
        "#include <stdint.h>",
        "",
        "#define OPL4_PCM_SAMPLE_RATE 22050U",
        "",
    ]
    for name, arr in samples:
        lines.append(f"#define {name}_LEN {len(arr)}U")
        lines.append(f"static const int16_t {name}[{len(arr)}] = {{")
        for i in range(0, len(arr), 16):
            chunk = ", ".join(str(x) for x in arr[i:i+16])
            lines.append(f"    {chunk},")
        lines.append("};\n")
    lines.append("#endif /* OPL4_DRUM_SAMPLES_H_ */\n")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"Wrote {path}: {len(samples)} waveforms, {total_bytes} bytes ({total_bytes / 1024:.1f} KB)")

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.normpath(os.path.join(here, "..", "include", "opl4_drum_samples.h"))
    write_header(out_path)
