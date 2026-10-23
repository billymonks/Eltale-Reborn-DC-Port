#!/usr/bin/env python3
"""Builds mods with their own build script and stages them for a modded Dreamcast build.

Runs on the host, where the mod's toolchain is (Quest-64-Good-Edition's build.py uses zig/clang
and downloads RecompModTool). For each mod it runs `python build.py <target>` in the mod repo,
then copies what the Dreamcast build needs out of the .nrm (mod_syms.bin, mod_binary.bin), the
mod's mod.toml (for its config defaults) and its link map (for test scripts) to build/mods/<name>/.
The build's inputs are only rewritten when
their contents change, so an unchanged mod doesn't trigger a rebuild.

usage: stage_mods.py <mods, e.g. eltale or eltale,gameplay> [--mod-dir DIR] [--no-build]
"""

import argparse
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# name -> (build.py target, mod.toml and build dir relative to the mod repo)
MODS = {
    "eltale": ("story", "mod.toml", "build"),
    "gameplay": ("gameplay", "mods/gameplay/mod.toml", "build/gameplay"),
}


def write_if_changed(path, data):
    if path.exists() and path.read_bytes() == data:
        return False
    path.write_bytes(data)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mods")
    parser.add_argument("--mod-dir", default=str(ROOT.parent / "Quest-64-Good-Edition"))
    parser.add_argument("--no-build", action="store_true", help="stage the mod's existing build output")
    args = parser.parse_args()

    mod_dir = Path(args.mod_dir).resolve()
    if not (mod_dir / "build.py").exists():
        sys.exit(f"stage_mods: no build.py in {mod_dir} (set MOD_DIR / --mod-dir to the mod repo)")

    for name in args.mods.split(","):
        if name not in MODS:
            sys.exit(f"stage_mods: unknown mod {name!r}; known: {', '.join(MODS)}")
        target, toml, build_dir = MODS[name]
        if not args.no_build:
            print(f"stage_mods: building {name} (python build.py {target}) in {mod_dir}", flush=True)
            subprocess.run([sys.executable, "build.py", target], cwd=mod_dir, check=True)
        nrms = sorted((mod_dir / build_dir).glob("*.nrm"), key=lambda p: p.stat().st_mtime)
        if not nrms:
            sys.exit(f"stage_mods: no .nrm in {mod_dir / build_dir}")
        nrm = nrms[-1]

        out = ROOT / "build" / "mods" / name
        out.mkdir(parents=True, exist_ok=True)
        changed = []
        with zipfile.ZipFile(nrm) as z:
            for member in ("mod_syms.bin", "mod_binary.bin"):
                if write_if_changed(out / member, z.read(member)):
                    changed.append(member)
        if write_if_changed(out / "mod.toml", (mod_dir / toml).read_bytes()):
            changed.append("mod.toml")
        # Only test scripts use the map (scripts/test_syms.py), so it isn't one of the build's inputs.
        mod_map = mod_dir / build_dir / "mod.map"
        if mod_map.exists():
            (out / "mod.map").write_bytes(mod_map.read_bytes())
        print(f"stage_mods: {name} from {nrm.name} -> {out.relative_to(ROOT)}"
              + (f" (updated {', '.join(changed)})" if changed else " (unchanged)"))


if __name__ == "__main__":
    main()
