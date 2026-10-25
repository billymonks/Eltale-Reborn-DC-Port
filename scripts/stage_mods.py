#!/usr/bin/env python3
"""Stages the Eltale Reborn packages in eltale/ for a Dreamcast build.

Each .nrm is a zip. mod_syms.bin and mod_binary.bin come out of it. The matching .toml is
written as mod.toml, and the .map (test scripts only) as mod.map. Output is build/mods/<name>/.
Files are rewritten only when their contents change.

usage: stage_mods.py <mods, e.g. eltale (which also stages gameplay)>
"""

import argparse
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ELTALE = ROOT / "eltale"

# name -> (nrm, toml, link map) under eltale/
MODS = {
    "eltale": ("eltale_reborn.nrm", "eltale_reborn.toml", "eltale_reborn.map"),
    "gameplay": ("quest64_modern_gameplay.nrm", "quest64_modern_gameplay.toml", "quest64_modern_gameplay.map"),
}


def write_if_changed(path, data):
    if path.exists() and path.read_bytes() == data:
        return False
    path.write_bytes(data)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mods")
    args = parser.parse_args()

    mods = args.mods.split(",")
    # Eltale Reborn is always built with its gameplay companion (the Makefile adds it too).
    if "eltale" in mods and "gameplay" not in mods:
        mods.append("gameplay")
    for name in mods:
        if name not in MODS:
            sys.exit(f"stage_mods: unknown mod {name!r}; known: {', '.join(MODS)}")
        nrm_name, toml_name, map_name = MODS[name]
        nrm = ELTALE / nrm_name
        toml = ELTALE / toml_name
        if not nrm.is_file():
            sys.exit(f"stage_mods: missing {nrm.relative_to(ROOT)}")
        if not toml.is_file():
            sys.exit(f"stage_mods: missing {toml.relative_to(ROOT)}")

        out = ROOT / "build" / "mods" / name
        out.mkdir(parents=True, exist_ok=True)
        changed = []
        with zipfile.ZipFile(nrm) as z:
            for member in ("mod_syms.bin", "mod_binary.bin"):
                try:
                    data = z.read(member)
                except KeyError:
                    sys.exit(f"stage_mods: {nrm.name} has no {member}")
                if write_if_changed(out / member, data):
                    changed.append(member)
        if write_if_changed(out / "mod.toml", toml.read_bytes()):
            changed.append("mod.toml")
        mod_map = ELTALE / map_name
        if mod_map.is_file():
            (out / "mod.map").write_bytes(mod_map.read_bytes())
        print(f"stage_mods: {name} from {nrm.name} -> {out.relative_to(ROOT)}"
              + (f" (updated {', '.join(changed)})" if changed else " (unchanged)"))


if __name__ == "__main__":
    main()
