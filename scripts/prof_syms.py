#!/usr/bin/env python3
# Builds the on-device profiler tables from a linked ELF and its linker map.
# profaddr.bin: uint32 count, uint32 starts[count+1], uint8 group[count]
# profname.bin: 12 bytes per function, uppercase, space padded.
#
# Groups come from the linker map's input-section address ranges, not from symbol
# name prefixes. A function nm knows about but the map does not cover is UNK.
import re
import struct
import subprocess
import sys

GROUPS = ("REND", "AUD", "GAME", "COL", "ROM", "RT", "MOD", "KOS", "LIBC", "LIBM", "GCC", "UNK")


def group_of(path):
    p = path.replace("\\", "/").lower()
    base = p.rsplit("/", 1)[-1]
    if "libgcc" in p:
        return 10
    if "libc.a" in p or "/libc/" in p:
        return 8
    if "libm.a" in p or "/libm/" in p:
        return 9
    if "/kos/" in p or "libkallisti" in p or "libkos" in p:
        return 7
    if "/mods/" in p or "/modrt/" in p or base.startswith("mod"):
        return 6
    if "/recomp/" in p or base.startswith("funcs_") or base.startswith("lookup"):
        return 2
    if base.startswith("gfx"):
        return 0
    if base.startswith("audio"):
        return 1
    if base.startswith("collision"):
        return 3
    if base.startswith("rom.") or base.startswith("pi."):
        return 4
    if "/runtime/" in p or base.startswith("runtime"):
        return 5
    if p == "":
        return 11
    return 11


def short_name(name):
    name = name.split(".")[-1]
    name = name.replace("_recomp", "").replace("_", "")
    name = "".join(c for c in name.upper() if c.isalnum())
    if not name:
        name = "NONAME"
    return name[:12].ljust(12)


def parse_ranges(path):
    """Input sections of the linked image: (start, end, object path)."""
    ranges = []
    pending = None
    # ".text.foo  0xADDR  0xSIZE  path" or a name-only line followed by the numbers.
    one = re.compile(
        r"^ (\.\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+)\s*$"
    )
    name_only = re.compile(r"^ (\.\S+)\s*$")
    cont = re.compile(
        r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+)\s*$"
    )
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = one.match(line)
            if m:
                start = int(m.group(2), 16)
                size = int(m.group(3), 16)
                if size and start >= 0x80000000:
                    ranges.append((start, start + size, m.group(4)))
                pending = None
                continue
            m = name_only.match(line)
            if m:
                pending = m.group(1)
                continue
            m = cont.match(line)
            if m and pending:
                start = int(m.group(1), 16)
                size = int(m.group(2), 16)
                if size and start >= 0x80000000:
                    ranges.append((start, start + size, m.group(3)))
                pending = None
                continue
            if line.startswith(" .") or (line and not line[0].isspace() and not line.startswith("Archive")):
                pending = None
    ranges.sort()
    return ranges


def owner_of(ranges, addr):
    # Last range that contains addr. Nested sections are rare; the later, tighter one wins
    # if we scan in order and keep going.
    path = ""
    for start, end, p in ranges:
        if start > addr:
            break
        if addr < end:
            path = p
    return path


def functions(elf):
    out = subprocess.check_output(
        ["sh-elf-nm", "-n", "--defined-only", elf], text=True, errors="replace"
    )
    rows = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        addr_s, kind, name = parts[0], parts[1], parts[-1]
        if kind not in ("T", "t", "W", "w"):
            continue
        rows.append((int(addr_s, 16), name))
    return rows


def main():
    elf, map_path, addr_path, name_path = sys.argv[1:5]
    ranges = parse_ranges(map_path)
    funcs = functions(elf)
    if not funcs:
        sys.exit("prof_syms: no functions in " + elf)
    uniq = []
    for i, (addr, name) in enumerate(funcs):
        if i + 1 < len(funcs) and funcs[i + 1][0] == addr:
            continue
        uniq.append((addr, name))
    end = uniq[-1][0] + 4
    starts = [a for a, _ in uniq] + [end]
    groups = []
    names = []
    missing = 0
    for addr, name in uniq:
        path = owner_of(ranges, addr)
        if not path:
            missing += 1
        groups.append(group_of(path))
        names.append(short_name(name).encode("ascii"))
    n = len(uniq)
    with open(addr_path, "wb") as f:
        f.write(struct.pack("<I", n))
        f.write(struct.pack("<%dI" % (n + 1), *starts))
        f.write(bytes(groups))
    with open(name_path, "wb") as f:
        for name in names:
            f.write(name)
    table = 4 + (n + 1) * 4 + n
    print(
        "prof_syms: %d functions, address table %d bytes, names %d bytes, %d outside every map range"
        % (n, table, 12 * n, missing)
    )
    counts = [0] * len(GROUPS)
    for g in groups:
        counts[g if g < len(GROUPS) else len(GROUPS) - 1] += 1
    print("prof_syms: " + " ".join("%s %d" % (GROUPS[i], counts[i]) for i in range(len(GROUPS))))


if __name__ == "__main__":
    main()
