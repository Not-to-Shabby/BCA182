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

def lowpass_taps(n=31, cutoff=0.22):
    """Windowed-sinc low-pass (cutoff in cycles/sample of the source rate) used before 2:1 decimation."""
    import math
    mid = (n - 1) / 2.0
    taps = []
    for i in range(n):
        x = i - mid
        h = 2 * cutoff if x == 0 else math.sin(2 * math.pi * cutoff * x) / (math.pi * x)
        w = 0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1))
        taps.append(h * w)
    g = sum(taps)
    return [t / g for t in taps]

def fetch_sf2_resampled(start_sample, src_rate, out_len, apply_tail_fade=True, src_len=None, peak=None):
    """Resample to 22.05 kHz with an anti-alias filter. Never reads past the sample's own end
    (src_len); anything after the natural end is faded and zero padded."""
    step = float(src_rate) / 22050.0
    needed_src = int(out_len * step) + 8
    if src_len is not None:
        needed_src = min(needed_src, src_len)
    pad = 16
    b_start = SMPL_OFFSET + max(0, start_sample - pad) * 2
    lead = start_sample - max(0, start_sample - pad)
    b_end = SMPL_OFFSET + (start_sample + needed_src + pad) * 2 - 1

    req = urllib.request.Request(
        URL,
        headers={"User-Agent": "Mozilla/5.0", "Range": f"bytes={b_start}-{b_end}"}
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        raw_bytes = r.read()
    cnt = len(raw_bytes) // 2
    full = struct.unpack(f"<{cnt}h", raw_bytes[: cnt * 2])
    raw = list(full[lead: lead + needed_src])

    if step > 1.0:
        taps = lowpass_taps()
        half = len(taps) // 2
        filt = []
        for i in range(len(raw)):
            acc = 0.0
            for k, t in enumerate(taps):
                j = i + k - half
                if 0 <= j < len(raw):
                    acc += raw[j] * t
            filt.append(acc)
        raw = filt

    out = []
    pos = 0.0
    for k in range(out_len):
        idx = int(pos)
        if idx + 1 >= len(raw):
            break
        frac = pos - idx
        val = raw[idx] * (1.0 - frac) + raw[idx + 1] * frac
        if not apply_tail_fade and k > out_len // 2:
            rem = (out_len - 1 - k) / float(out_len - (out_len // 2))
            val *= rem
        out.append(val)
        pos += step

    valid = len(out)
    fade_len = min(128 if valid < out_len else 64, valid)
    if apply_tail_fade:
        for k in range(fade_len):
            idx = valid - fade_len + k
            out[idx] *= (fade_len - 1 - k) / float(fade_len)
    while len(out) < out_len:
        out.append(0.0)

    mx = max(1.0, max(abs(v) for v in out))
    scale = (peak / mx) if peak else 1.0
    if mx * scale > 32767:
        scale = 32767.0 / mx
    return [max(-32767, min(32767, int(round(v * scale)))) for v in out]

def main():
    print("Fetching genuine Roland Sound Canvas SC-55 waveforms...")
    # Drum Kit
    kick    = fetch_sf2_resampled(21860616, 44100, 3969, True, src_len=3994)  # Kick1 [657]
    snare   = fetch_sf2_resampled(21844809, 44100, 3969, True, src_len=11118)  # Tight Snare [655]
    stick   = fetch_sf2_resampled(594148,   44100, 1433, True, src_len=3024)  # RimShot [47]
    clap    = fetch_sf2_resampled(6875540,  44100, 3087, True, src_len=21696)  # Clap [243]
    hat_c   = fetch_sf2_resampled(22026842, 44100, 1212, True, src_len=5048)  # Close Hi Hat [660]
    hat_o   = fetch_sf2_resampled(22035260, 44100, 6174, True, src_len=111370)  # Open Hi Hat [662]
    crash   = fetch_sf2_resampled(21864633, 44100, 9922, True, src_len=78150)  # Crash Cymbal [658]
    ride    = fetch_sf2_resampled(22146653, 44100, 7717, True, src_len=80796)  # Ride Cymbal1 [663]
    tom     = fetch_sf2_resampled(21628954, 44100, 3969, True, src_len=25340)  # Tom Mid 1 [Stan1] [649]
    cowbell = fetch_sf2_resampled(8315813,  44100, 2205, True, src_len=5184)  # CowbellG#3 [307]

    # Per-key samples from the SC-55 Standard drum preset (each key plays its own recording)
    kick2    = fetch_sf2_resampled(21855950, 44100, 3969, True, src_len=4643)    # Kick2 [656] key 35
    snare_f  = fetch_sf2_resampled(21829791, 44100, 5000, True, src_len=14995)   # Fat Snare [654] key 38
    hat_p    = fetch_sf2_resampled(22031913, 44100, 1500, True, src_len=3324)    # Pedal Hi Hat [661] key 44
    splash   = fetch_sf2_resampled(21942806, 44100, 9000, True, src_len=84013)   # Splash Cymbal [659] key 55
    ride_b   = fetch_sf2_resampled(22227472, 44100, 7000, True, src_len=78848)   # Ride Bell [664] key 53
    ride2    = fetch_sf2_resampled(22306343, 44100, 8000, True, src_len=101893)  # Ride Cymbal2 [665] key 59
    china    = fetch_sf2_resampled(8528837,  44100, 9000, True, src_len=84480)   # ChinaCym [322] key 52
    tamb     = fetch_sf2_resampled(16870314, 44100, 3500, True, src_len=8704)    # Tambourine [533] key 54
    tom41    = fetch_sf2_resampled(8915154,  44100, 6000, True, src_len=41700)   # F2 [348] key 41
    tom43    = fetch_sf2_resampled(8861463,  44100, 6000, True, src_len=36493)   # G2 [346] key 43
    tom45    = fetch_sf2_resampled(8830224,  44100, 6000, True, src_len=31216)   # A2 [345] key 45
    tom47    = fetch_sf2_resampled(8795212,  44100, 5500, True, src_len=25112)   # B2 [337] key 47
    tom48    = fetch_sf2_resampled(8774349,  44100, 5000, True, src_len=20840)   # C3 [336] key 48
    tom50    = fetch_sf2_resampled(8756774,  44100, 4500, True, src_len=17552)   # D3 [335] key 50

    # Melodic Attack Transients
    piano    = fetch_sf2_resampled(652425,   44100, 1874, False, src_len=25604) # SC55 Piano C4m [50]
    guitar   = fetch_sf2_resampled(10376166, 44100, 1433, False, src_len=83456) # SC88SteelstrGt C4 [375]
    bass     = fetch_sf2_resampled(17570961, 44100, 1212, False, src_len=12144) # Pick Bass B [551]
    dist_gtr = fetch_sf2_resampled(8038884,  44100, 1433, False, src_len=7558) # SC55 Overdrive C4 [300]

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
        ("KICK2", kick2),
        ("SNARE_FAT", snare_f),
        ("HAT_P", hat_p),
        ("SPLASH", splash),
        ("RIDE_BELL", ride_b),
        ("RIDE2", ride2),
        ("CHINA", china),
        ("TAMB", tamb),
        ("TOM41", tom41),
        ("TOM43", tom43),
        ("TOM45", tom45),
        ("TOM47", tom47),
        ("TOM48", tom48),
        ("TOM50", tom50),
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
