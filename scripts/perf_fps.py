#!/usr/bin/env python3
"""Frames per second of each scene of tests/perf.txt, from Flycast logs.

  scripts/perf_fps.py <log> [<log> ...]

A scene runs from its "PERF <name>" marker's profile_start to its profile_dump, which prints the
retraces and swaps in between; its fps is swaps per 1/59.94 s retrace. (Logs from before the dump
printed them: frames swapped per second between the first and last progress line inside it.)
Where the dump also printed the ROM blocks read from the disc during the scene, so does the table.
"""

import re
import sys

RETRACE_HZ = 59.94


def read_log(path):
    raw = open(path, "rb").read()
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16")
    return raw.decode("utf-8", "replace")


def scenes(path):
    result = {}
    name, inside, rows = None, False, []
    for line in read_log(path).splitlines():
        m = re.search(r"test: PERF (\w+)", line)
        if m:
            name = m.group(1)
        if "profile: started" in line:
            inside, rows = True, []
        elif "profile: " in line and "samples" in line and inside:
            inside = False
            m = re.search(r"(\d+) retraces, (\d+) swaps", line)
            if name and m and int(m.group(1)):
                result[name] = int(m.group(2)) * RETRACE_HZ / int(m.group(1))
            elif name and len(rows) >= 2:
                (t0, s0), (t1, s1) = rows[0], rows[-1]
                result[name] = (s1 - s0) / (t1 - t0)
        m = re.search(r"profile: ROM (\d+) blocks in \d+ reads, (\d+) ms", line)
        if m and name in result:
            result[name] = (result[name], int(m.group(1)), int(m.group(2)))
        m = re.search(r"\] t=(\d+)s VI \d+ swaps (\d+)", line)
        if m and inside:
            rows.append((int(m.group(1)), int(m.group(2))))
    return result


def main():
    runs = [scenes(path) for path in sys.argv[1:]]
    names = []
    for run in runs:
        names += [n for n in run if n not in names]
    print("scene      " + "".join(f"{p[-28:]:>30s}" for p in sys.argv[1:]))
    for name in names:
        cells = [cell(run.get(name)) for run in runs]
        print(f"{name:10s} " + "".join(f"{c:>30s}" for c in cells))


def cell(value):
    if value is None:
        return "-"
    if isinstance(value, tuple):
        fps, blocks, ms = value
        return f"{fps:.1f} fps, ROM {blocks} blk {ms} ms"
    return f"{value:.1f} fps"


if __name__ == "__main__":
    main()
