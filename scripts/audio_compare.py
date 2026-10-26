#!/usr/bin/env python3
"""Compares two audio recordings from scripts/flycast.ps1 -AudioDump of the same test script.

Usage: audio_compare.py <reference.raw> <other.raw> [name:start:seconds]...

Both are Flycast's output, raw 44.1 kHz stereo s16. The recordings start at different moments, so
the other is first lined up with the reference on their loudness envelopes. Then, for each named
stretch (seconds into the reference), prints loudness, peak and left/right balance of both, and the
offset that lines up the start and the end of the stretch: a drift between the two is a tempo
difference.
"""
import array
import math
import sys

RATE = 44100
HOP = 441  # 10 ms envelope steps


def load(path):
    a = array.array("h")
    with open(path, "rb") as f:
        a.frombytes(f.read())
    if sys.byteorder != "little":
        a.byteswap()
    return a


def envelope(a):
    """Log loudness of the summed channels in 10 ms steps."""
    out = []
    n = len(a) // 2
    for i in range(0, n - HOP, HOP):
        s = 0
        for k in range(2 * i, 2 * (i + HOP), 8):
            v = a[k] + a[k + 1]
            s += v * v
        out.append(math.log10(s / (HOP / 4) + 1.0))
    return out


def best_offset(ref, oth, r0, length, lo, hi):
    """Envelope offset (oth index = ref index + offset) that matches ref[r0:r0+length] best."""
    best, best_off = None, 0
    seg = ref[r0:r0 + length]
    if not seg:
        return 0, 0.0
    mr = sum(seg) / len(seg)
    sr = math.sqrt(sum((x - mr) ** 2 for x in seg)) or 1.0
    for off in range(lo, hi + 1):
        a = r0 + off
        if a < 0 or a + length > len(oth):
            continue
        o = oth[a:a + length]
        mo = sum(o) / length
        so = math.sqrt(sum((x - mo) ** 2 for x in o)) or 1.0
        c = sum((x - mr) * (y - mo) for x, y in zip(seg, o)) / (sr * so)
        if best is None or c > best:
            best, best_off = c, off
    return best_off, best or 0.0


def stats(a, start, secs):
    i0 = max(0, int(start * RATE)) * 2
    i1 = min(len(a), i0 + int(secs * RATE) * 2)
    seg = a[i0:i1]
    if not seg:
        return -999.0, -999.0, 0.0
    left, right = seg[0::2], seg[1::2]
    sl = sum(v * v for v in left) / max(1, len(left))
    sr = sum(v * v for v in right) / max(1, len(right))
    rms = math.sqrt((sl + sr) / 2)
    peak = max(abs(min(seg)), max(seg))
    db = lambda v: 20 * math.log10(v / 32768) if v > 0 else -999.0
    bal = 10 * math.log10((sl + 1) / (sr + 1))
    return db(rms), db(peak), bal


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 1
    ref, oth = load(args[0]), load(args[1])
    stretches = []
    for s in args[2:]:
        name, start, secs = s.split(":")
        stretches.append((name, float(start), float(secs)))
    er, eo = envelope(ref), envelope(oth)
    print(f"reference {len(ref) / 2 / RATE:.1f} s, other {len(oth) / 2 / RATE:.1f} s")
    for name, start, secs in stretches:
        r0 = int(start * 100)
        n = int(secs * 100)
        # Line up the start and the end of the stretch separately: their difference is drift.
        w = min(n, 600)
        off_a, ca = best_offset(er, eo, r0, w, -800, 800)
        off_b, cb = best_offset(er, eo, r0 + n - w, w, off_a - 100, off_a + 100)
        rr = stats(ref, start, secs)
        ro = stats(oth, start + off_a / 100.0, secs)
        print(f"{name:10s} ref rms {rr[0]:6.1f} peak {rr[1]:6.1f} L-R {rr[2]:+5.1f} dB | "
              f"other rms {ro[0]:6.1f} peak {ro[1]:6.1f} L-R {ro[2]:+5.1f} dB | "
              f"offset {off_a * 10:+5d} ms (r {ca:.2f}) -> {off_b * 10:+5d} ms (r {cb:.2f}), "
              f"drift {(off_b - off_a) * 10:+d} ms over {secs - w / 100.0:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
