#!/usr/bin/env python3
"""Frames per second of each scene of tests/perf.txt, from Flycast logs.

  scripts/perf_fps.py <log> [<log> ...]

A scene runs from its "PERF <name>" marker's profile_start to its profile_dump, which prints the
retraces and swaps in between; its fps is swaps per 1/59.94 s retrace. (Logs from before the dump
printed them: frames swapped per second between the first and last progress line inside it.)
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
        m = re.search(r"\] t=(\d+)s VI \d+ swaps (\d+)", line)
        if m and inside:
            rows.append((int(m.group(1)), int(m.group(2))))
    return result


def main():
    runs = [scenes(path) for path in sys.argv[1:]]
    names = []
    for run in runs:
        names += [n for n in run if n not in names]
    print("scene      " + "".join(f"{p[-22:]:>24s}" for p in sys.argv[1:]))
    for name in names:
        cells = [f"{run[name]:.1f} fps" if name in run else "-" for run in runs]
        print(f"{name:10s} " + "".join(f"{c:>24s}" for c in cells))


if __name__ == "__main__":
    main()
