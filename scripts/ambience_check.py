#!/usr/bin/env python3
"""Checks an Eltale ambience pack (tools/ambpack) with the AICA's ADPCM decoder.

For a loop: decodes start 0's preamble from the key-on state and then the whole clip, checks that the
decoder comes back to the state it began the clip with (a seamless repeat), and that every other
start's preamble reaches the state the whole decode has there. Prints each clip's level as stored and
as played at volume 1 (with its gain): the PC mod's loops are -42 dBFS RMS.

usage: ambience_check.py <ambience.bin> [--wav <dir>]   (--wav also writes each clip as a .wav)
"""
import struct
import sys
import wave

import numpy as np

HDR = struct.Struct("<8I")
CLIP = struct.Struct("<IIIiffBB2x36s")
START = struct.Struct("<IHH")
STARTS, PRE, MAX = 8, 512, 32
SCALE = (230, 230, 230, 230, 307, 409, 512, 614)


def codes_of(data, n):
    nib = np.frombuffer(bytes(data), dtype=np.uint8)
    codes = np.empty(n, dtype=np.int32)
    codes[0::2] = nib[: (n + 1) // 2] & 15
    codes[1::2] = nib[: n // 2] >> 4
    return codes.tolist()


def decode(codes, state, marks=None, every=0):
    """Decodes codes from state (hist, step); with marks, notes the state before every `every`th."""
    hist, step = state
    out = np.empty(len(codes), dtype=np.int32)
    for i, c in enumerate(codes):
        if marks is not None and i % every == 0:
            marks.append((hist, step))
        mag = c & 7
        diff = min(((1 + (mag << 1)) * step) >> 3, 32767)
        hist = hist - diff if c & 8 else hist + diff
        hist = max(-32768, min(32767, hist))
        step = max(127, min(24576, (SCALE[mag] * step) >> 8))
        out[i] = hist
    return out, (hist, step)


def db(x):
    return 20 * np.log10(max(x, 1e-12))


def main():
    path = sys.argv[1]
    wav_dir = sys.argv[sys.argv.index("--wav") + 1] if "--wav" in sys.argv else None
    blob = open(path, "rb").read()
    magic, count, rate, chunk, data_at = HDR.unpack_from(blob, 0)[:5]
    assert magic == 0x32424D41, hex(magic)
    every = chunk * 2
    bad = 0
    for i in range(count):
        offset, samples, audible, gain_mdb, wet, cross, channels, loops, name = CLIP.unpack_from(blob, HDR.size + i * CLIP.size)
        name = name.split(b"\0")[0].decode()
        nbytes = samples // 2
        chunks = (nbytes + chunk - 1) // chunk
        starts = []
        for j in range(STARTS):
            at = HDR.size + MAX * CLIP.size + (i * STARTS + j) * (START.size + PRE)
            sample, ncodes, _ = START.unpack_from(blob, at)
            pres = [codes_of(blob[at + START.size + c * PRE // 2: at + START.size + (c + 1) * PRE // 2], ncodes) for c in range(2)]
            starts.append((sample, ncodes, pres))
        chans, notes = [], []
        for c in range(channels):
            parts = [blob[offset + (k * channels + c) * chunk: offset + (k * channels + c + 1) * chunk] for k in range(chunks)]
            codes = codes_of(b"".join(parts)[:nbytes], samples)
            state = (0, 127)
            if loops:
                _, state = decode(starts[0][2][c], (0, 127))
            marks = []
            pcm, end = decode(codes, state, marks, every)
            chans.append(pcm)
            if loops:
                if end != state:
                    notes.append(f"ch {c} ends in {end}, began in {state}")
                for sample, ncodes, pres in starts:
                    _, got = decode(pres[c], (0, 127))
                    want = marks[sample // every]
                    if got != want:
                        notes.append(f"ch {c} start {sample}: preamble reaches {got}, data needs {want}")
        x = np.stack(chans).astype(np.float64) / 32768.0
        rms = np.sqrt(np.mean(x[:, :audible] ** 2))
        line = f"{name:26s} {channels} ch {samples / rate:6.2f} s  peak {db(np.abs(x).max()):6.1f}  rms {db(rms):6.1f} dBFS  played {db(rms) + gain_mdb / 1000:6.1f}"
        if loops:
            steps = np.abs(np.diff(x, axis=1))
            wrap = np.abs(x[:, 0] - x[:, -1])
            line += f"  wrap step {wrap.max():.4f} (99.9% of steps < {np.quantile(steps, 0.999):.4f}); preambles {max(s[1] for s in starts)} codes at most"
        print(line)
        for n in notes:
            print("   PROBLEM:", n)
            bad += 1
        if wav_dir:
            with wave.open(f"{wav_dir}/{name}.wav", "wb") as w:
                w.setnchannels(channels)
                w.setsampwidth(2)
                w.setframerate(rate)
                w.writeframes((x.T * 32767).clip(-32768, 32767).astype("<i2").tobytes())
    print("all clips check out" if bad == 0 else f"{bad} problems")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
