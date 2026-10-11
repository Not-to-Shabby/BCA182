#!/usr/bin/env python3
"""
gen_opl4_melodic.py
Extracts genuine 16-bit 22,050 Hz acoustic attack transient waveforms (Acoustic Grand
Piano Wave 304, Acoustic Steel Guitar Wave 13, and Electric Bass Wave 163) directly
from the official Yamaha YRW801 (OPL4 / YMF278B) WaveTable ROM and writes them as
const int16_t Flash ROM tables to include/opl4_melodic_samples.h.
"""

import math
import os
import urllib.request
import zlib

FS = 22050.0

ROM_PATHS = [
    "/tmp/yrw801.rom",
    "C:/Users/Perch/AppData/Local/Temp/yrw801.rom",
    "D:/BCA182_Lab/Side_Quest_for_LAB_2/tools/yrw801.rom",
    "tools/yrw801.rom",
]

def get_yrw801_rom():
    for p in ROM_PATHS:
        if os.path.exists(p) and os.path.getsize(p) == 2097152:
            return open(p, "rb").read()
    url = "https://raw.githubusercontent.com/BatoceraPLUS/Batocera.PLUS-UPDATE-bios/main/bios/openmsx/yrw801.rom"
    print(f"Downloading genuine Yamaha YRW801 ROM from {url}...")
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
    if len(data) == 2097152:
        out_p = "D:/BCA182_Lab/Side_Quest_for_LAB_2/tools/yrw801.rom"
        try:
            open(out_p, "wb").write(data)
        except Exception:
            pass
        return data
    raise RuntimeError(f"YRW801 ROM download failed (size {len(data)})")

def extract_yrw801_melodic(rom, wave_id, midi_note, pitch_offset, out_len):
    """
    Decodes a 12-bit/8-bit/16-bit waveform from YRW801 ROM and resamples it using
    the exact OPL4 hardware pitch equation:
      pitch = (midi_note << 7) + pitch_offset
      step_44k = 2 ** ((pitch - 12288) / 1536.0)
      step_22k = 2.0 * step_44k
    """
    hdr = rom[wave_id * 12 : (wave_id + 1) * 12]
    b0, b1, b2 = hdr[0], hdr[1], hdr[2]
    fmt = (b0 >> 6) & 3
    start_addr = ((b0 & 0x3F) << 16) | (b1 << 8) | b2
    loop_pos = (hdr[3] << 8) | hdr[4]
    total_len = (65536 - ((hdr[5] << 8) | hdr[6])) & 0xFFFF

    raw = []
    for i in range(total_len):
        if fmt == 1: # 12-bit packed
            addr = start_addr + (i // 2) * 3
            if i & 1:
                u16 = (rom[addr + 2] << 8) | (rom[addr + 1] & 0xF0)
            else:
                u16 = (rom[addr + 0] << 8) | ((rom[addr + 1] & 0x0F) << 4)
        elif fmt == 0: # 8-bit
            u16 = rom[start_addr + i] << 8
        else: # 16-bit
            addr = start_addr + i * 2
            u16 = (rom[addr] << 8) | rom[addr + 1]
        s = u16 - 65536 if u16 >= 32768 else u16
        raw.append(max(-32767, min(32767, s)))

    pitch = (midi_note << 7) + pitch_offset
    step_22k = 2.0 * (2.0 ** ((pitch - 12288.0) / 1536.0))

    out = []
    pos = 0.0
    loop_len = max(1, total_len - loop_pos)
    for k in range(out_len):
        idx = int(pos)
        if idx + 1 >= total_len:
            if loop_pos < total_len - 2:
                idx = loop_pos + ((idx - loop_pos) % loop_len)
            else:
                break
        frac = pos - int(pos)
        s0 = raw[idx]
        s1 = raw[min(idx + 1, total_len - 1)]
        val = s0 * (1.0 - frac) + s1 * frac
        # Smooth attack-transient envelope fade over the second half of the window
        if k > out_len // 2:
            rem = (out_len - 1 - k) / float(out_len - (out_len // 2))
            val *= rem
        out.append(max(-32767, min(32767, int(round(val)))))
        pos += step_22k

    while len(out) < out_len:
        out.append(0)
    return out

def main():
    rom = get_yrw801_rom()
    print(f"Loaded Yamaha YRW801 ROM ({len(rom)} bytes, CRC32=0x{zlib.crc32(rom)&0xFFFFFFFF:08X})")

    # 1. Acoustic Grand Piano (regions_00: Wave 0x130 = 304, pitch_offset = 4260, root C4 = note 60)
    piano = extract_yrw801_melodic(rom, 304, 60, 4260, int(0.085 * FS))
    # 2. Acoustic Steel Guitar (regions_19: Wave 0x00d = 13, pitch_offset = 5410, root E3 = note 52)
    guitar = extract_yrw801_melodic(rom, 13, 52, 5410, int(0.065 * FS))
    # 3. Slap / Fingered Bass (regions_24: Wave 0x0a3 = 163, pitch_offset = 7606, root E2 = note 40)
    bass = extract_yrw801_melodic(rom, 163, 40, 7606, int(0.055 * FS))

    total_bytes = (len(piano) + len(guitar) + len(bass)) * 2

    lines = [
        "/**",
        " * @file opl4_melodic_samples.h",
        f" * @brief Genuine Yamaha OPL4 (YRW801) factory acoustic attack WaveTables ({total_bytes} bytes in Flash).",
        " *        Extracted directly from the official Yamaha YRW801 ROM by tools/gen_opl4_melodic.py.",
        " */",
        "",
        "#ifndef OPL4_MELODIC_SAMPLES_H_",
        "#define OPL4_MELODIC_SAMPLES_H_",
        "",
        "#include <stdint.h>",
        "",
        "#define OPL4_MELODIC_SAMPLE_RATE 22050U",
        "",
        f"#define OPL4_PCM_PIANO_LEN {len(piano)}U",
        f"#define OPL4_PCM_PIANO_ROOT_HZ 261.6256f /* C4 (YRW801 Wave 304) */",
        f"static const int16_t OPL4_PCM_PIANO[{len(piano)}] = {{",
    ]
    for i in range(0, len(piano), 16):
        lines.append("    " + ", ".join(str(x) for x in piano[i:i+16]) + ",")
    lines.append("};")
    lines.append("")

    lines.extend([
        f"#define OPL4_PCM_GUITAR_LEN {len(guitar)}U",
        f"#define OPL4_PCM_GUITAR_ROOT_HZ 164.8138f /* E3 (YRW801 Wave 13) */",
        f"static const int16_t OPL4_PCM_GUITAR[{len(guitar)}] = {{",
    ])
    for i in range(0, len(guitar), 16):
        lines.append("    " + ", ".join(str(x) for x in guitar[i:i+16]) + ",")
    lines.append("};")
    lines.append("")

    lines.extend([
        f"#define OPL4_PCM_BASS_LEN {len(bass)}U",
        f"#define OPL4_PCM_BASS_ROOT_HZ 82.4069f /* E2 (YRW801 Wave 163) */",
        f"static const int16_t OPL4_PCM_BASS[{len(bass)}] = {{",
    ])
    for i in range(0, len(bass), 16):
        lines.append("    " + ", ".join(str(x) for x in bass[i:i+16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif /* OPL4_MELODIC_SAMPLES_H_ */")
    lines.append("")

    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.normpath(os.path.join(here, "..", "include", "opl4_melodic_samples.h"))

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"SUCCESS: Generated {out_path} ({total_bytes} bytes from genuine Yamaha YRW801 ROM)!")

if __name__ == "__main__":
    main()
