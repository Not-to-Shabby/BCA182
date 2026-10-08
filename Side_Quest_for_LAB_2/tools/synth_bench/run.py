#!/usr/bin/env python3
"""
run.py [out_dir]
Builds bench.c against the current src/ and renders a fixed set of notes and songs, so the
FM voice can be compared before and after a change. WAV files go to out_dir (default
synth_out/, not committed) and can be played in any audio player.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
MIDI = "D:/idx/shub_extracted/midi"

NOTES = [  # label, program, midi note, channel
    ("grand piano C4", 0, 60, 0),
    ("electric piano C4", 4, 60, 0),
    ("organ C4", 16, 60, 0),
    ("nylon guitar E3", 24, 52, 0),
    ("bass E1", 33, 28, 0),
    ("strings C4", 48, 60, 0),
    ("trumpet C4", 56, 60, 0),
    ("sax C4", 65, 60, 0),
    ("flute C5", 73, 72, 0),
    ("synth lead C4", 80, 60, 0),
]
SONGS = [
    ("Beer (Itchyworms)", "027720 - Itchyworms - Beer.mid"),
    ("Everything I Do (Bryan Adams)", "002106 - Bryan Adams - (Everything I Do) I Do It for You.mid"),
    ("#9 Dream (John Lennon)", "036527 - John Lennon - #9 Dream.mid"),
]


def midi_hz(n):
    return 440.0 * 2 ** ((n - 69) / 12.0)


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "synth_out"))
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, "bench.exe")
    subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", f"-I{ROOT}/test/stubs", "-DQUIET_PRINTK", f"-I{ROOT}/include",
                    "-o", exe, os.path.join(HERE, "bench.c"), "-lm"], check=True)

    for label, prog, note, ch in NOTES:
        wav = os.path.join(out, f"note_{prog:03d}.wav")
        print(f"[{label}]")
        subprocess.run([exe, "note", str(prog), str(note), "100", "1.8", wav, str(ch)], check=True)
        subprocess.run([sys.executable, os.path.join(HERE, "analyze.py"), wav, f"{midi_hz(note):.3f}"], check=True)

    for label, name in SONGS:
        path = os.path.join(MIDI, name)
        if not os.path.exists(path):
            print(f"[{label}] missing {path}")
            continue
        print(f"[{label}] first 40 s")
        wav = os.path.join(out, "song_" + name.split(" - ")[0] + ".wav")
        subprocess.run([exe, "song", path, "40", wav, "80"], check=True)


if __name__ == "__main__":
    main()
