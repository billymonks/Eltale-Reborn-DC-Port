#!/usr/bin/env python3
"""Checks that two Flycast audio dumps play at the same pitch (needs numpy).

Usage: audio_pitch.py <reference.raw> <other.raw> <offset_s> [name:start:seconds]...

For each stretch, compares the log spectra (50 Hz - 4 kHz) of the two, stretched by factors
around 1, and prints the factor that matches best: 1.000 is the same pitch, 0.5 or 2 an octave.
"""
import sys

import numpy as np

RATE = 44100


def load(path):
    a = np.fromfile(path, dtype="<i2").astype(np.float64)
    return a.reshape(-1, 2).mean(axis=1)


def logspec(x):
    n = 16384
    acc = None
    for i in range(0, len(x) - n, n // 2):
        s = np.abs(np.fft.rfft(x[i:i + n] * np.hanning(n))) ** 2
        acc = s if acc is None else acc + s
    return np.log10(acc + 1e-9)


def main():
    ref, oth = load(sys.argv[1]), load(sys.argv[2])
    offset = float(sys.argv[3])
    freqs = np.fft.rfftfreq(16384, 1.0 / RATE)
    grid = np.exp(np.linspace(np.log(50), np.log(4000), 2000))
    for s in sys.argv[4:]:
        name, start, secs = s.split(":")
        start, secs = float(start), float(secs)
        a, b, n = int(start * RATE), int((start + offset) * RATE), int(secs * RATE)
        sr, so = logspec(ref[a:a + n]), logspec(oth[b:b + n])
        r = np.interp(grid, freqs, sr)
        r = (r - r.mean()) / r.std()
        best = (-2.0, 1.0)
        results = []
        for f in np.concatenate([np.linspace(0.97, 1.03, 61), [0.5, 2.0]]):
            o = np.interp(grid * f, freqs, so)
            o = (o - o.mean()) / o.std()
            c = float((r * o).mean())
            results.append((f, c))
            if c > best[0]:
                best = (c, f)
        c1 = dict(results)[1.0] if 1.0 in dict(results) else None
        half = [c for f, c in results if f == 0.5][0]
        dbl = [c for f, c in results if f == 2.0][0]
        print(f"{name:10s} best factor {best[1]:.3f} (r {best[0]:.3f}); at 1.000 r {c1:.3f}; "
              f"octave down r {half:.3f}, octave up r {dbl:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
