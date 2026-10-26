#!/usr/bin/env python3
"""Compares the HASH lines of two test runs (tests/cg_ab.txt), 64 KB block by block.

usage: compare_runs.py <reference log> <candidate log> [--mask <mask file>] [--write-mask <mask file>]

Each HASH line is `[q64dc] HASH <label> poll <n> vi <n> <64 block hashes>`. Lines are matched by
label. --write-mask records the blocks that differ between the two logs (run it on two runs of the
same disc to find what changes from run to run); --mask leaves those blocks out of the comparison.
Also compares the `pos` lines in order. Exit status 0 when nothing unmasked differs.
"""

import re
import sys


def read(path):
    hashes, positions = {}, []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"\[q64dc\] HASH (\S+) poll (\d+) vi (\d+)((?: [0-9A-F]{8})+)", line)
        if m:
            hashes[m.group(1)] = (int(m.group(2)), m.group(4).split())
        m = re.search(r"\[q64dc\] test: pos (\S+ \S+ \S+) step (\S+)", line)
        if m:
            positions.append((m.group(1), m.group(2)))
    return hashes, positions


def main():
    args = sys.argv[1:]
    mask_path = write_mask = None
    if "--mask" in args:
        i = args.index("--mask")
        mask_path = args[i + 1]
        del args[i:i + 2]
    if "--write-mask" in args:
        i = args.index("--write-mask")
        write_mask = args[i + 1]
        del args[i:i + 2]
    ref_path, new_path = args
    ref, ref_pos = read(ref_path)
    new, new_pos = read(new_path)
    mask = {}
    if mask_path:
        for line in open(mask_path):
            label, blocks = line.split(":", 1)
            mask[label.strip()] = {int(b, 16) for b in blocks.split()}

    bad = 0
    found = {}
    labels = [label for label in ref if label in new]
    for label in sorted(set(ref) | set(new), key=lambda l: ref.get(l, new.get(l))[0]):
        if label not in ref or label not in new:
            print(f"{label}: only in {'reference' if label in ref else 'candidate'}")
            bad += 1
            continue
        (rpoll, rh), (npoll, nh) = ref[label], new[label]
        diff = [i for i in range(min(len(rh), len(nh))) if rh[i] != nh[i]]
        found[label] = diff
        unmasked = [i for i in diff if i not in mask.get(label, mask.get("*", set()))]
        status = "same" if not unmasked else "DIFFERS"
        poll_note = "" if rpoll == npoll else f" (polls {rpoll} vs {npoll})"
        print(f"{label}: {status}{poll_note}; {len(diff)} blocks differ, {len(unmasked)} unmasked"
              + (f": {' '.join(f'{b:02X}' for b in unmasked)}" if unmasked else ""))
        bad += 1 if unmasked or rpoll != npoll else 0
    for i, (a, b) in enumerate(zip(ref_pos, new_pos)):
        if a != b:
            print(f"pos {i}: {a} vs {b}")
            bad += 1
    if len(ref_pos) != len(new_pos):
        print(f"pos lines: {len(ref_pos)} vs {len(new_pos)}")
        bad += 1
    if write_mask:
        with open(write_mask, "w") as f:
            for label, diff in found.items():
                f.write(f"{label}: {' '.join(f'{b:02X}' for b in diff)}\n")
        print(f"wrote {write_mask}")
    print("MATCH" if bad == 0 else f"{bad} differences")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
