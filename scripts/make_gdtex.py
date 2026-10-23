#!/usr/bin/env python3
"""Converts a cover image (PNG) to 0GDTEX.PVR, the disc-root cover art read by Flycast, GDEMU
menus (GDMenu, openMenu) and their card managers.

The output is a 512x512 RGB565 square-twiddled PVR texture. Transparent pixels are composited
over white. Other sizes are resampled to 512x512. Only the standard library is used, so it runs
in the toolchain container as is.

usage: make_gdtex.py <cover.png> <0GDTEX.PVR>
"""

import struct
import sys
import zlib

SIZE = 512


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: not a PNG file")
    pos, idat, palette, trns = 8, b"", None, None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if depth != 8 or interlace or color not in (0, 2, 3, 4, 6):
        sys.exit(f"{path}: only 8-bit, non-interlaced PNGs are supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    stride = width * channels
    raw = zlib.decompress(idat)
    rows, prev = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    pixels = []
    for line in rows:
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if color == 0:
                pixels.append((px[0], px[0], px[0], 255))
            elif color == 2:
                pixels.append((px[0], px[1], px[2], 255))
            elif color == 3:
                alpha = trns[px[0]] if trns and px[0] < len(trns) else 255
                pixels.append(palette[px[0]] + (alpha,))
            elif color == 4:
                pixels.append((px[0], px[0], px[0], px[1]))
            else:
                pixels.append(tuple(px))
    return width, height, pixels


def resample(width, height, pixels):
    if (width, height) == (SIZE, SIZE):
        return pixels
    out = []
    for y in range(SIZE):
        y0, y1 = y * height // SIZE, max(y * height // SIZE + 1, (y + 1) * height // SIZE)
        for x in range(SIZE):
            x0, x1 = x * width // SIZE, max(x * width // SIZE + 1, (x + 1) * width // SIZE)
            box = [pixels[yy * width + xx] for yy in range(y0, y1) for xx in range(x0, x1)]
            out.append(tuple(sum(p[c] for p in box) // len(box) for c in range(4)))
    return out


def twiddle(n):
    out = 0
    for bit in range(10):
        out |= ((n >> bit) & 1) << (2 * bit)
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[-1])
    pixels = resample(*read_png(sys.argv[1]))
    texels = bytearray(SIZE * SIZE * 2)
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = pixels[y * SIZE + x]
            r, g, b = ((c * a + 255 * (255 - a)) // 255 for c in (r, g, b))
            value = ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | (b * 31 + 127) // 255
            struct.pack_into("<H", texels, ((twiddle(x) << 1) | twiddle(y)) * 2, value)
    header = b"GBIX" + struct.pack("<II", 8, 0) + b"\0" * 4
    header += b"PVRT" + struct.pack("<IBBHHH", len(texels) + 8, 1, 1, 0, SIZE, SIZE)
    with open(sys.argv[2], "wb") as f:
        f.write(header + texels)


if __name__ == "__main__":
    main()
