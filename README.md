# Eltale Reborn: Holy Magic Century - Dreamcast version

Dreamcast builds of Quest 64 and of [Eltale Reborn: Holy Magic Century](https://github.com/billymonks/Eltale-Reborn).

Eltale Reborn: Holy Magic Century is a full reimagining of Quest 64, combining the N64 and GBC versions of the game as inspiration for an all new version that aims to deliver a satisfying and fresh RPG experience.

Eltale Reborn is an independent, noncommercial fan modification. Not affiliated with or endorsed by Imagineer, SEGA, Nintendo, THQ, or Konami.

Original game © 1998 Imagineer Co., Ltd.

The PC version is https://github.com/billymonks/Eltale-Reborn

## Build

Put your own Quest 64 (USA) 1.0 ROM at `quest64.us.z64` (big-endian `.z64`). The disc contains that ROM. Don't distribute it.

1. Once, on Windows, Linux, or macOS. Clone with the submodules, then build the toolchain image. That compiles GCC the first time and takes 30 to 90 minutes. Every disc below uses this image.

```
git clone --recurse-submodules https://github.com/billymonks/Eltale-Reborn-DC-Port
cd Eltale-Reborn-DC-Port
docker build -t q64dc-toolchain docker
```

2. Eltale Reborn, both mods (story and gameplay). Hold X to sprint, the triggers switch item filter, Start opens the map. Windows: `scripts\mod.ps1`. Linux and macOS: `scripts/mod.sh`. Writes `build/eltale-reborn.cdi`.
3. Quest 64, no mods. Windows: `scripts\dc.ps1 make -j16 cdi`. Linux and macOS: `scripts/dc.sh make -j16 cdi`. Writes `build/quest64-dc.cdi`.

A 512x512 PNG at `assets/cover-eltale.png` is packed onto the disc as `0GDTEX.PVR`. `assets/cover.png` is the fallback. Neither file is committed.

## Button Mapping

| Button        | Action                                      |
| ------------- | ------------------------------------------- |
| Stick         | Walk, and move cursors                      |
| A             | Talk, confirm, attack                       |
| B             | Cancel, close menus                         |
| X             | Sprint                                      |
| Y             | Item menu                                   |
| D-pad         | Magic                                       |
| Left trigger  | Scroll the item list left                   |
| Right trigger | Scroll the item list right                  |
| Start         | Open menu                                   |
| A+B+X+Y+Start | Soft reset: back to the title screen; on the title screen, to the Dreamcast menu |

## Saves

The game sees a Controller Pak. Files live on the first VMU found, usually controller A's first slot.

| Disc | VMU file | Description |
| --- | --- | --- |
| Quest 64 | `QUEST64.PAK` | Quest 64 saves |
| Eltale Reborn | `Q64ELT.PAK` | Eltale Reborn saves |

One save is 3 blocks. The whole 0x200-byte note is stored, including 0x160-0x1FF, which Eltale uses. The two discs do not share a file.

The VMU is read at boot and when a different one is plugged in. It is written about 300 ms after the game's last write (around 0.7 s in Flycast). Leave it inserted through that. With no VMU the game reports no Controller Pak and plays on, as it would on an N64. A VMU with no room fails with the game's own "full" error.

## Quest 64

The US game, no mods. Same recompilation as [Quest 64: Recompiled](https://github.com/Rainchus/Quest64-Recomp), run under KallistiOS.

## License

GPL-3.0, same as Quest 64: Recompiled. See [COPYING](COPYING).
