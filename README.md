# Eltale Reborn: Holy Magic Century - Dreamcast version

Dreamcast builds of Quest 64 and of [Eltale Reborn: Holy Magic Century](https://github.com/billymonks/Eltale-Reborn).

Eltale Reborn: Holy Magic Century is a full reimagining of Quest 64, combining the N64 and GBC versions of the game as inspiration for an all new version that aims to deliver a satisfying and fresh RPG experience.

Eltale Reborn is an independent, noncommercial fan modification. Not affiliated with or endorsed by Imagineer, SEGA, Nintendo, THQ, or Konami.

Original game © 1998 Imagineer Co., Ltd.

The PC version is https://github.com/billymonks/Eltale-Reborn

## Build

Put your own Quest 64 (USA) 1.0 ROM at `quest64.us.z64` (big-endian `.z64`). The disc contains that ROM. Don't distribute it.

Docker. The toolchain image builds GCC the first time (30 to 90 minutes). Clone with `--recurse-submodules` so `tools/N64Recomp` and `Quest64Syms` come along.

```
git clone --recurse-submodules https://github.com/billymonks/Eltale-Reborn-DC-Port
cd Eltale-Reborn-DC-Port
docker build -t q64dc-toolchain docker
scripts\mod.ps1
```

That writes `build/eltale-reborn.cdi` with the story mod and the gameplay mod (hold X to sprint, the triggers switch item filter, Start opens the map).

Linux and macOS:

```
scripts/dc.sh python3 scripts/stage_mods.py eltale
scripts/dc.sh make -j16 cdi MOD=eltale,gameplay
```

A 512x512 PNG at `assets/cover-eltale.png` is packed onto the disc as `0GDTEX.PVR`. `assets/cover.png` is the fallback. Neither file is committed.

Buttons and VMU saves: [PLAYING.md](PLAYING.md).

## Quest 64

The US game, no mods. Same recompilation as [Quest 64: Recompiled](https://github.com/Rainchus/Quest64-Recomp), run under KallistiOS.

```
scripts\dc.ps1 make -j16 cdi
```

`build/quest64-dc.cdi`. On Linux and macOS, `scripts/dc.sh make -j16 cdi`.

## License

GPL-3.0, same as Quest 64: Recompiled. See [COPYING](COPYING).
