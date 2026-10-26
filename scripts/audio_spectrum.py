#!/usr/bin/env python3
"""Band energies and fine tempo drift between two Flycast audio dumps (needs numpy).

Usage: audio_spectrum.py <reference.raw> <other.raw> <offset_s> [name:start:seconds]...

offset_s lines the other up with the reference (audio_compare.py prints it). For each stretch
(seconds into the reference): the share of energy in four bands, and the offset that lines up
5 s windows at its start and end, to 5 ms: their difference over the stretch is a tempo error.
"""
import sys

import numpy as np

RATE = 44100
BANDS = [(0, 2000), (2000, 5000), (5000, 11000), (11000, 22050)]


def load(path):
    a = np.fromfile(path, dtype="<i2").astype(np.float64)
    return a.reshape(-1, 2).mean(axis=1)


def bands(x):
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    freqs = np.fft.rfftfreq(len(x), 1.0 / RATE)
    total = spec.sum() or 1.0
    return [100.0 * spec[(freqs >= lo) & (freqs < hi)].sum() / total for lo, hi in BANDS]


def env(x, hop=220):
    n = len(x) // hop
    return np.log10((x[: n * hop].reshape(n, hop) ** 2).mean(axis=1) + 1.0)


def align(er, eo, r0, length, guess, span):
    seg = er[r0:r0 + length]
    seg = (seg - seg.mean()) / (seg.std() or 1.0)
    best, best_off = -2.0, guess
    for off in range(guess - span, guess + span + 1):
        a = r0 + off
        if a < 0 or a + length > len(eo):
            continue
        o = eo[a:a + length]
        o = (o - o.mean()) / (o.std() or 1.0)
        c = float((seg * o).mean())
        if c > best:
            best, best_off = c, off
    return best_off, best


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    ref, oth = load(sys.argv[1]), load(sys.argv[2])
    offset = float(sys.argv[3])
    er, eo = env(ref), env(oth)
    step = 220 / RATE
    for s in sys.argv[4:]:
        name, start, secs = s.split(":")
        start, secs = float(start), float(secs)
        a = int(start * RATE)
        b = int((start + offset) * RATE)
        n = int(secs * RATE)
        br = bands(ref[a:a + n])
        bo = bands(oth[b:b + n])
        w = int(5.0 / step)
        r0 = int(start / step)
        r1 = int((start + secs - 5.0) / step)
        g = int(offset / step)
        o0, c0 = align(er, eo, r0, w, g, int(0.4 / step))
        o1, c1 = align(er, eo, r1, w, o0, int(0.2 / step))
        drift_ms = (o1 - o0) * step * 1000
        span = (r1 - r0) * step
        print(f"{name:10s} bands % ref " + " ".join(f"{v:5.1f}" for v in br) +
              " | other " + " ".join(f"{v:5.1f}" for v in bo) +
              f" | offset {o0 * step * 1000:+.0f} ms (r {c0:.2f}) -> {o1 * step * 1000:+.0f} ms (r {c1:.2f}), "
              f"drift {drift_ms:+.0f} ms over {span:.0f} s ({100 * drift_ms / 1000 / span:+.2f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
