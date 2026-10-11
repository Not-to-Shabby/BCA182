#!/usr/bin/env python3
"""
gen_sc55_samples.py
Extracts genuine 16-bit 22,050 Hz Roland Sound Canvas SC-55 drum and melodic
waveforms via HTTP Range requests from SC-55.sf2 and writes them as const int16_t
Flash ROM tables to include/sc55_samples.h.
"""

import os
import struct
import urllib.request

URL = "https://archive.org/download/sc-55/SC-55.sf2"
SMPL_OFFSET = 108  # Byte offset of sample 0 in SC-55.sf2

def fetch_sf2_resampled(start_sample, src_rate, out_len, apply_tail_fade=True):
    step = float(src_rate) / 22050.0
    needed_src = int(out_len * step) + 8
    b_start = SMPL_OFFSET + start_sample * 2
    b_end = b_start + needed_src * 2 - 1

    req = urllib.request.Request(
        URL,
        headers={"User-Agent": "Mozilla/5.0", "Range": f"bytes={b_start}-{b_end}"}
    )
    with urllib.request.urlopen(req, timeout=20) as r:
        raw_bytes = r.read()

    cnt = len(raw_bytes) // 2
    raw = struct.unpack(f"<{cnt}h", raw_bytes[: cnt * 2])

    out = []
    pos = 0.0
    for k in range(out_len):
        idx = int(pos)
        if idx + 1 >= len(raw):
            break
        frac = pos - idx
        val = raw[idx] * (1.0 - frac) + raw[idx + 1] * frac
        if not apply_tail_fade and k > out_len // 2:
            # Melodic transient smooth envelope fade over second half
            rem = (out_len - 1 - k) / float(out_len - (out_len // 2))
            val *= rem
        out.append(max(-32767, min(32767, int(round(val)))))
        pos += step

    while len(out) < out_len:
        out.append(0)

    if apply_tail_fade:
        fade_len = min(64, len(out))
        for k in range(fade_len):
            idx = len(out) - fade_len + k
            fade = (fade_len - 1 - k) / float(fade_len)
            out[idx] = int(round(out[idx] * fade))

    return out

def main():
    print("Fetching genuine Roland Sound Canvas SC-55 waveforms...")
    # Drum Kit
    kick    = fetch_sf2_resampled(21860616, 44100, 3969, True)  # Kick1 [657]
    snare   = fetch_sf2_resampled(21844809, 44100, 3969, True)  # Tight Snare [655]
    stick   = fetch_sf2_resampled(594148,   44100, 1433, True)  # RimShot [47]
    clap    = fetch_sf2_resampled(6875540,  44100, 3087, True)  # Clap [243]
    hat_c   = fetch_sf2_resampled(22026842, 44100, 1212, True)  # Close Hi Hat [660]
    hat_o   = fetch_sf2_resampled(22035260, 44100, 6174, True)  # Open Hi Hat [662]
    crash   = fetch_sf2_resampled(21864633, 44100, 9922, True)  # Crash Cymbal [658]
    ride    = fetch_sf2_resampled(22146653, 44100, 7717, True)  # Ride Cymbal1 [663]
    tom     = fetch_sf2_resampled(21628954, 44100, 3969, True)  # Tom Mid 1 [Stan1] [649]
    cowbell = fetch_sf2_resampled(8315813,  44100, 2205, True)  # CowbellG#3 [307]

    # Melodic Attack Transients
    piano    = fetch_sf2_resampled(652425,   44100, 1874, False) # SC55 Piano C4m [50]
    guitar   = fetch_sf2_resampled(10376166, 44100, 1433, False) # SC88SteelstrGt C4 [375]
    bass     = fetch_sf2_resampled(17570961, 44100, 1212, False) # Pick Bass B [551]
    dist_gtr = fetch_sf2_resampled(8038884,  44100, 1433, False) # SC55 Overdrive C4 [300]

    all_tables = [
        ("KICK", kick),
        ("SNARE", snare),
        ("STICK", stick),
        ("CLAP", clap),
        ("HAT_C", hat_c),
        ("HAT_O", hat_o),
        ("CRASH", crash),
        ("RIDE", ride),
        ("TOM", tom),
        ("COWBELL", cowbell),
        ("PIANO", piano),
        ("GUITAR", guitar),
        ("BASS", bass),
        ("DIST_GTR", dist_gtr),
    ]

    total_samples = sum(len(x) for _, x in all_tables)
    total_bytes = total_samples * 2

    lines = [
        "/**",
        " * @file sc55_samples.h",
        f" * @brief Genuine Roland Sound Canvas SC-55 16-bit 22,050 Hz PCM WaveTables ({total_bytes} bytes in Flash).",
        " *        Extracted by tools/gen_sc55_samples.py.",
        " */",
        "",
        "#ifndef SC55_SAMPLES_H_",
        "#define SC55_SAMPLES_H_",
        "",
        "#include <stdint.h>",
        "",
        "#define SC55_PCM_SAMPLE_RATE 22050U",
        "#define SC55_PCM_PIANO_ROOT_HZ 261.6256f",
        "#define SC55_PCM_GUITAR_ROOT_HZ 261.6256f",
        "#define SC55_PCM_BASS_ROOT_HZ 82.4069f",
        "#define SC55_PCM_DIST_GTR_ROOT_HZ 261.6256f",
        "",
    ]

    for name, data in all_tables:
        lines.append(f"#define SC55_PCM_{name}_LEN {len(data)}U")
        lines.append(f"static const int16_t SC55_PCM_{name}[{len(data)}] = {{")
        for i in range(0, len(data), 16):
            chunk = ", ".join(str(x) for x in data[i : i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")

    lines.append("#endif /* SC55_SAMPLES_H_ */")
    lines.append("")

    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.normpath(os.path.join(here, "..", "include", "sc55_samples.h"))
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"SUCCESS: Generated {out_path} ({total_bytes} bytes, {total_samples} samples)!")

if __name__ == "__main__":
    main()
