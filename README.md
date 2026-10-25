# Eltale Reborn: Holy Magic Century - Dreamcast version

Dreamcast builds of Quest 64 and of [Eltale Reborn: Holy Magic Century](https://github.com/billymonks/Eltale-Reborn).

Eltale Reborn: Holy Magic Century is a full reimagining of Quest 64, combining the N64 and GBC versions of the game as inspiration for an all new version that aims to deliver a satisfying and fresh RPG experience.

Put your own Quest 64 (USA) 1.0 ROM at `quest64.us.z64` (big-endian `.z64`). The CDI includes that ROM. Don't distribute it.

Buttons and VMU saves: [PLAYING.md](PLAYING.md). How the port is put together, and the Flycast numbers: [NOTES.md](NOTES.md).

## Setup

Docker. The toolchain image builds GCC the first time, which takes 30 to 90 minutes.

```
git clone --recurse-submodules <this repo>
docker build -t q64dc-toolchain docker
```

`--recurse-submodules` is required. `tools/N64Recomp` and `Quest64Syms` are submodules.

A 512x512 PNG at `assets/cover.png` is packed as `0GDTEX.PVR`. `assets/cover-eltale.png` is used instead for the Eltale Reborn disc, when that file is there.

## Eltale Reborn: Holy Magic Century

Holy Magic Century, from the PC story mod, compiled into this port. The disc also includes that project's gameplay mod: hold X to sprint, the triggers switch item filter, Start opens the map.

PC version, source, and its own build instructions: https://github.com/billymonks/Eltale-Reborn

Clone that repo and set it up the same way as a PC build (Python requirements and zig). Then, from this repo:

```
scripts\mod.ps1 -ModDir ..\Eltale-Reborn make -j16 cdi
```

Output is `build/eltale-reborn.cdi`.

With no `-ModDir`, `scripts\mod.ps1` looks for `..\Quest-64-Good-Edition`. `-NoBuild` skips the mod's build and uses the `.nrm` already in that checkout.

Linux and macOS:

```
python3 scripts/stage_mods.py eltale --mod-dir ../Eltale-Reborn
scripts/dc.sh make -j16 cdi MOD=eltale,gameplay
```

## Quest 64

The US game, statically recompiled with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and run under KallistiOS. Same recompilation as [Quest 64: Recompiled](https://github.com/Rainchus/Quest64-Recomp).

```
scripts\dc.ps1 make -j16 cdi
```

Output is `build/quest64-dc.cdi`. `scripts/dc.sh make -j16 cdi` is the Linux and macOS equivalent.

## License

GPL-3.0, same as Quest 64: Recompiled. See [COPYING](COPYING).
