#!/usr/bin/env python3
"""Fills in a mod's symbols in a test script for a modded test disc.

A test script can write {gModSave} or {gModSave+8} (or {eltale:gModSave} if two mods have the
name) for the Dreamcast address of a mod symbol: its address in the mod's link map (staged by
scripts/stage_mods.py as build/mods/<mod>/mod.map), moved to where tools/modrecomp loaded its
section (mods_layout.txt). The result, in hex as the test commands take it, goes to stdout.

usage: test_syms.py <script> <mods_layout.txt> <staged mods dir>
"""

import re
import sys
from pathlib import Path

MAP_LINE = re.compile(r"^\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+\d+\s+(\S+)\s*$")
TOKEN = re.compile(r"\{(?:(\w+):)?(\w+)(?:\+(0x[0-9a-fA-F]+|\d+))?\}")


def main():
    script, layout_path, stage = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    sections = {}  # mod -> [(link, load, size)]
    for line in layout_path.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        mod, _, link, load, size = line.split()
        sections.setdefault(mod, []).append((int(link, 16), int(load, 16), int(size, 16)))

    maps = {}
    def symbols(mod):
        if mod not in maps:
            maps[mod] = {}
            path = stage / mod / "mod.map"
            if path.exists():
                for line in path.read_text(errors="replace").splitlines():
                    m = MAP_LINE.match(line)
                    if m:
                        maps[mod].setdefault(m.group(2), int(m.group(1), 16))
        return maps[mod]

    def resolve(m):
        mods = [m.group(1)] if m.group(1) else list(sections)
        hits = [(mod, symbols(mod)[m.group(2)]) for mod in mods if m.group(2) in symbols(mod)]
        if len(hits) != 1:
            sys.exit(f"test_syms: {m.group(0)}: " + ("not in any staged mod.map" if not hits
                     else "in more than one mod; write {mod:name}"))
        mod, addr = hits[0]
        for link, load, size in sections.get(mod, []):
            if link <= addr < link + size:
                return f"{load + addr - link + int(m.group(3) or '0', 0):08X}"
        sys.exit(f"test_syms: {m.group(0)} (0x{addr:08X}) isn't in any of {mod}'s sections")

    sys.stdout.write(TOKEN.sub(resolve, script.read_text()))


if __name__ == "__main__":
    main()
