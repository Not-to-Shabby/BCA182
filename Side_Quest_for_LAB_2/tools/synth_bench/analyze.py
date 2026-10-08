#!/usr/bin/env python3
"""
analyze.py <note.wav> <fundamental_hz>
Prints how bright a rendered single note is: the level of harmonics 2..8 relative to the
fundamental (a pure sine reads about -60 dB or lower), the spectral centroid in multiples of
the fundamental, and how the loudness moves over the note.
"""
import sys
import wave
import numpy as np


def load(path):
    with wave.open(path, 'rb') as w:
        raw = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(np.float64)
    return raw[0::2]


def band_db(spec, freqs, hz, ref):
    k = int(np.argmin(np.abs(freqs - hz)))
    lo, hi = max(k - 3, 0), min(k + 4, len(spec))
    return 20 * np.log10(max(spec[lo:hi].max(), 1e-9) / ref)


def main():
    x = load(sys.argv[1])
    f0 = float(sys.argv[2])
    rate = 44100
    start, size = int(0.12 * rate), 16384
    seg = x[start:start + size]
    if len(seg) < size or np.max(np.abs(seg)) < 1:
        print("  (silent or too short)")
        return
    spec = np.abs(np.fft.rfft(seg * np.hanning(size)))
    freqs = np.fft.rfftfreq(size, 1 / rate)
    ref = spec[int(np.argmin(np.abs(freqs - f0))) - 3:int(np.argmin(np.abs(freqs - f0))) + 4].max()
    harm = []
    for h in range(2, 9):
        hz = f0 * h
        harm.append(f"{h}:{band_db(spec, freqs, hz, ref):6.1f}" if hz < rate / 2 else f"{h}:  n/a")
    centroid = float((spec * freqs).sum() / spec.sum())
    env = [float(np.sqrt(np.mean(x[int(t * rate):int(t * rate) + 2205] ** 2))) for t in (0.02, 0.1, 0.3, 0.6, 1.0, 1.4)
           if int(t * rate) + 2205 <= len(x)]
    print("  harmonics dB re fundamental: " + "  ".join(harm))
    print(f"  spectral centroid = {centroid / f0:.2f} x fundamental")
    print("  rms at 0.02/0.1/0.3/0.6/1.0/1.4 s: " + " ".join(f"{v:7.0f}" for v in env))


if __name__ == '__main__':
    main()
