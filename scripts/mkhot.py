#!/usr/bin/env python3
"""Writes the hot-layout linker script: KallistiOS's shlelf.xc with listed functions first in .text.

usage: mkhot.py <KallistiOS shlelf.xc> <hot list> <out .xc>
The hot list has one function name per line (# comments allowed); each is the .text.<name>
section -ffunction-sections gives it, so a name with no such section is skipped by the linker.
The startup objects stay first: the Dreamcast starts the program at the start of .text.
"""
import re
import sys

CRT = ("crt1.o", "crtbegin.o", "crti.o", "crtend.o")


def main():
    base, names_path, out = sys.argv[1:4]
    names = []
    for line in open(names_path):
        line = line.split("#", 1)[0].strip()
        if line and line not in names:
            names.append(line)
    text = open(base).read()
    m = re.search(r"\.text\s*:\s*\{\n(\s*)\*\(\.text \.stub ", text)
    if not m:
        sys.exit("mkhot: no .text catch-all in " + base)
    indent = m.group(1)
    lines = "".join("%sKEEP(*%s(.text .text.*))\n" % (indent, o) for o in CRT)
    lines += "".join("%s*(.text.%s)\n" % (indent, n) for n in names)
    text = text[: m.start(1)] + lines + text[m.start(1):]
    with open(out, "w", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    main()
