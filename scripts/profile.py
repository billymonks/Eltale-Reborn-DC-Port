#!/usr/bin/env python3
"""Map the "[q64dc] prof <address> <count>" lines of a Flycast log to functions.

  scripts/profile.py <log> [elf] [top]

Run inside the toolchain container (it needs sh-elf-nm). A log can hold several profiles; each one
(from "profile: started" to "profile: end") is reported separately.
"""

import bisect
import glob
import os
import subprocess
import sys

BUCKET = 32  # profile.c's bucket size


def read_log(path):
    raw = open(path, "rb").read()
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16")
    return raw.decode("utf-8", "replace")


def symbols(elf):
    out = subprocess.run(["sh-elf-nm", "-n", "-C", elf], capture_output=True, text=True,
                         check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def object_of_symbols():
    """Function name -> the object file that defines it, for our own objects."""
    owner = {}
    objs = glob.glob("build/obj/runtime/*.o") + glob.glob("build/obj/recomp/*.o")
    out = subprocess.run(["sh-elf-nm", "-C", "--defined-only", "-A"] + objs, capture_output=True,
                         text=True).stdout
    for line in out.splitlines():
        path, _, rest = line.partition(":")
        parts = rest.split(None, 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            obj = os.path.basename(path)
            owner[parts[2]] = "recompiled game" if path.startswith("build/obj/recomp/") else obj
    return owner


def main():
    log = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else "build/quest64-dc.elf"
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    addrs, names = symbols(elf)
    owner = object_of_symbols()
    owner["(idle)"] = "(idle)"
    # KOS's idle thread is a two-instruction loop that shares its bucket with thd_block_now.
    idle = addrs[names.index("thd_idle_task")] if "thd_idle_task" in names else None

    profiles, current, label = [], None, ""
    for line in read_log(log).splitlines():
        if "[q64dc] test: " in line and "profile" not in line:
            label = line.split("[q64dc] test: ", 1)[1].strip()
        if "profile: started" in line:
            current = {}
        elif "profile: end" in line and current is not None:
            profiles.append((label, current))
            current = None
        elif "[q64dc] prof " in line and current is not None:
            addr, count = line.split("[q64dc] prof ", 1)[1].split()[:2]
            current[int(addr, 16)] = current.get(int(addr, 16), 0) + int(count)

    for label, buckets in profiles:
        funcs = {}
        total = sum(buckets.values())
        for addr, count in buckets.items():
            if idle is not None and addr <= idle < addr + BUCKET:
                name = "(idle)"
            else:
                i = bisect.bisect_right(addrs, addr) - 1
                name = names[i] if i >= 0 else "?"
            funcs[name] = funcs.get(name, 0) + count
        print(f"== {label or 'profile'}: {total} samples")
        groups = {}
        for name, count in funcs.items():
            group = owner.get(name, "KOS/libc/libgcc")
            groups[group] = groups.get(group, 0) + count
        for group, count in sorted(groups.items(), key=lambda kv: -kv[1]):
            print(f"{100.0 * count / total:6.2f}%  {group}")
        print()
        acc = 0
        for name, count in sorted(funcs.items(), key=lambda kv: -kv[1])[:top]:
            acc += count
            print(f"{100.0 * count / total:6.2f}% {100.0 * acc / total:6.2f}%  {count:6d}  {name}")
        print()


if __name__ == "__main__":
    main()
