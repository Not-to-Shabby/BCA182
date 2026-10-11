#!/usr/bin/env python3
"""
gen_opl4_drums.py
Extracts genuine 16-bit 22,050 Hz acoustic & metallic PCM drum waveforms from the
official Yamaha YRW801 (OPL4 / YMF278B) WaveTable ROM and writes them as const int16_t
Flash tables to include/opl4_drum_samples.h.
"""

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
    # Download if not cached
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

def decode_yrw801_sample(rom, wave_id, length, pitch_scale=1.0):
    hdr = rom[wave_id * 12 : (wave_id + 1) * 12]
    b0, b1, b2 = hdr[0], hdr[1], hdr[2]
    fmt = (b0 >> 6) & 3
    start_addr = ((b0 & 0x3F) << 16) | (b1 << 8) | b2
    total_len = (65536 - ((hdr[5] << 8) | hdr[6])) & 0xFFFF

    # Decode raw 12-bit/8-bit/16-bit samples from ROM
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

    # Resample with pitch scaling to fit exact requested length
    out = []
    step = pitch_scale
    pos = 0.0
    for _ in range(length):
        idx = int(pos)
        if idx + 1 >= len(raw):
            break
        frac = pos - idx
        s0 = raw[idx]
        s1 = raw[idx + 1]
        val = int(round(s0 * (1.0 - frac) + s1 * frac))
        out.append(max(-32767, min(32767, val)))
        pos += step

    # Pad or apply smooth anti-click fade at tail
    while len(out) < length:
        out.append(0)

    # Smooth tail fade over last 64 samples
    fade_len = min(64, len(out))
    for k in range(fade_len):
        idx = len(out) - fade_len + k
        fade = (fade_len - 1 - k) / float(fade_len)
        out[idx] = int(round(out[idx] * fade))

    return out

def main():
    rom = get_yrw801_rom()
    print(f"Loaded Yamaha YRW801 ROM ({len(rom)} bytes, CRC32=0x{zlib.crc32(rom)&0xFFFFFFFF:08X})")

    # Genuine Yamaha YRW801 ROM wave definitions:
    # Kick: Wave 17 (3969 samples)
    # Snare: Wave 209 (3969 samples)
    # Side Stick: Wave 210 (1433 samples)
    # Clap: Wave 10 (3087 samples)
    # Closed Hat: Wave 121 (1212 samples, pitch_scale = 1.0)
    # Open Hat: Wave 122 (6174 samples)
    # Crash 1: Wave 49 (9922 samples)
    # Ride 1: Wave 46 (7717 samples)
    # Tom: Wave 200 (3969 samples)
    # Cowbell: Wave 29 (2205 samples)

    kick = decode_yrw801_sample(rom, 17, 3969)
    snare = decode_yrw801_sample(rom, 209, 3969)
    stick = decode_yrw801_sample(rom, 210, 1433)
    clap = decode_yrw801_sample(rom, 10, 3087)
    hat_c = decode_yrw801_sample(rom, 121, 1212)
    hat_o = decode_yrw801_sample(rom, 122, 6174)
    crash = decode_yrw801_sample(rom, 49, 9922)
    ride = decode_yrw801_sample(rom, 46, 7717)
    tom = decode_yrw801_sample(rom, 200, 3969)
    cowbell = decode_yrw801_sample(rom, 29, 2205)

    samples = [
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
    ]

    total_samples = sum(len(s) for _, s in samples)
    total_bytes = total_samples * 2

    lines = [
        "/**",
        " * @file opl4_drum_samples.h",
        f" * @brief Genuine Yamaha OPL4 (YRW801) factory 16-bit 22,050 Hz PCM WaveTable drum samples ({total_bytes} bytes in Flash).",
        " *        Extracted directly from the official Yamaha YRW801 ROM by tools/gen_opl4_drums.py.",
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

    for name, data in samples:
        lines.append(f"#define OPL4_PCM_{name}_LEN {len(data)}U")
        lines.append(f"static const int16_t OPL4_PCM_{name}[{len(data)}] = {{")
        for i in range(0, len(data), 16):
            chunk = ", ".join(str(x) for x in data[i : i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")

    lines.append("#endif /* OPL4_DRUM_SAMPLES_H_ */")
    lines.append("")

    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.normpath(os.path.join(here, "..", "include", "opl4_drum_samples.h"))

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"SUCCESS: Generated {out_path} ({total_bytes} bytes, {total_samples} samples from genuine Yamaha YRW801 ROM)!")

if __name__ == "__main__":
    main()
