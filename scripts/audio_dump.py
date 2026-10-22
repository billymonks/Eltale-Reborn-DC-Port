#!/usr/bin/env python3
"""Checks an audio recording from scripts/flycast.ps1 -AudioDump and cuts WAV clips from it.

Usage: audio_dump.py <dump.raw> [--every N] [--clip name start seconds]...

The dump is Flycast's output, raw 44.1 kHz stereo s16. Prints loudness, peak and clipped samples
for every N seconds (default 2), with the zero-crossing rate as a rough brightness/pitch measure,
and writes each --clip to a WAV next to the dump.
"""
import array
import math
import os
import sys
import wave

RATE = 44100


def stats(s):
    if not s:
        return 0.0, 0, 0, 0
    sq = sum(v * v for v in s)
    rms = math.sqrt(sq / len(s))
    peak = max(abs(min(s)), max(s))
    clipped = sum(1 for v in s if v >= 32767 or v <= -32768)
    # Zero crossings of the left channel per second.
    left = s[0::2]
    zc = sum(1 for a, b in zip(left, left[1:]) if (a < 0) != (b < 0))
    return rms, peak, clipped, zc * RATE // max(1, len(left))


def db(v):
    return 20 * math.log10(v / 32768) if v > 0 else -999.0


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    path = args.pop(0)
    every = 2.0
    clips = []
    while args:
        a = args.pop(0)
        if a == "--every":
            every = float(args.pop(0))
        elif a == "--clip":
            clips.append((args.pop(0), float(args.pop(0)), float(args.pop(0))))
    data = array.array("h")
    with open(path, "rb") as f:
        data.frombytes(f.read())
    if sys.byteorder != "little":
        data.byteswap()
    secs = len(data) / 2 / RATE
    print(f"{path}: {secs:.1f} s")
    rms, peak, clipped, zc = stats(data)
    print(f"whole: rms {db(rms):6.1f} dBFS peak {db(peak):6.1f} dBFS clipped {clipped}")
    step = int(every * RATE) * 2
    for i in range(0, len(data), step):
        rms, peak, clipped, zc = stats(data[i:i + step])
        bar = "#" * max(0, int((db(rms) + 60) / 2)) if rms > 0 else ""
        print(f"{i / 2 / RATE:6.1f}s rms {db(rms):6.1f} peak {db(peak):6.1f} clip {clipped:5d} zc {zc:5d}/s {bar}")
    for name, start, length in clips:
        a = int(start * RATE) * 2
        b = a + int(length * RATE) * 2
        out = os.path.join(os.path.dirname(path), name + ".wav")
        with wave.open(out, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(data[a:b].tobytes())
        rms, peak, clipped, zc = stats(data[a:b])
        print(f"wrote {out}: rms {db(rms):.1f} dBFS peak {db(peak):.1f} dBFS clipped {clipped}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
