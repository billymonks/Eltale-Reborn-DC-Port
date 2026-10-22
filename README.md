# Quest 64 for Dreamcast

A Sega Dreamcast port of Quest 64 (USA), built from [N64Recomp](https://github.com/N64Recomp/N64Recomp)'s static recompilation of the game, the same base as [Quest 64: Recompiled](https://github.com/Rainchus/Quest64-Recomp). The long-term goal is to run the Eltale Reborn: Holy Magic Century mod on the Dreamcast.

**This repository contains no game data.** You need your own Quest 64 (USA) ROM to build it, and builds must not be shared.

## Status

Milestone 3 (the renderer) is done: the game draws on the Dreamcast, without sound yet. In Flycast the intro, story panels, title screen and file menu render at 59 fps, and a new game shows the first room in 3D (about 19 fps; the game's own CPU time is the limit there, see milestone 7). Audio tasks are still acknowledged without being executed.

| # | Milestone | State |
| --- | --- | --- |
| 1 | Docker toolchain (KallistiOS v2.2.1, GCC 14.3.0), N64Recomp with 32-bit registers, linkable ELF | done |
| 2 | Runtime: threads, message queues, timers, VI, PI DMA from disc, controllers; game boots headless | done |
| 3 | F3DEX renderer on the PowerVR; title screen | done |
| 4 | Rooms load from disc, Dreamcast controller, VMU saves | |
| 5 | Audio: audio microcode commands in C, streamed to the AICA | |
| 6 | Eltale Reborn compiled in ahead of time | |
| 7 | Performance work on real hardware | |

## How it works

N64Recomp turns every MIPS function of the ROM into a C function that operates on a register context and a byte array standing in for the N64's RDRAM. The desktop recomp compiles that C for x86/ARM and runs it on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) and RT64. Here the same C is compiled for the Dreamcast's SH-4 with KallistiOS, and the runtime underneath is written for the Dreamcast.

- **32-bit registers.** N64Recomp normally models every GPR as 64 bits, which the 32-bit SH-4 would emulate with register pairs. Quest 64 never uses the upper halves: its 64-bit libultra helpers (`__ll_div`, `__ull_rem`, ...) are already runtime functions, and none of the other ~87,000 instructions is a 64-bit integer operation. The `gpr_32bit` config option (`tools/patches/n64recomp-gpr32.patch`) makes the recompiler reject 64-bit GPR instructions, emit 32-bit `hi`/`lo`, and call `DIV32`/`DIVU32` instead of 64-bit division (a libgcc call on SH-4). `runtime/include/recomp.h` then defines `gpr` as `uint32_t`.
- **Memory.** The game runs in the N64's base 4 MB. RDRAM is a 4 MB block in main RAM, stored as 32-bit words in host order like the desktop runtime does (SH-4 runs little-endian, as x86 does). The 16 MB ROM doesn't fit in the Dreamcast's 16 MB of RAM, so it stays on the disc and is read on demand by the PI DMA functions.
- **Threads.** Each N64 thread is a KallistiOS thread, but only one runs game code at a time, as on the N64's single CPU: a thread gives up the CPU when it blocks on a message queue, and the highest-priority ready thread runs next (`runtime/src/sched.cpp`). An events thread raises the VI retrace on the Dreamcast's vblank and fires `osSetTimer` timers (`events.c`). `osSpTaskStartGo` runs graphics tasks through the renderer and completes audio tasks immediately, then sends the SP and DP done messages.
- **Controllers and saves.** Dreamcast controllers are read through Maple (`cont.c`): A and B are A and B, X and the left trigger are Z, Y and the right trigger are R, the D-pad is the C buttons, and the stick is the stick. Controller Pak saves are kept in RAM (`pak.c`) and lost at power-off until VMU support (milestone 4).
- **Graphics.** The game uses the F3DEX 1.23 microcode. `gfx.c` interprets its display lists on the CPU and draws them with KallistiOS's PowerVR API directly (no GL), in the spirit of jnmartin84's Fast3D backends for [sm64-dc](https://github.com/jnmartin84/sm64-dc) and [sf64-dc](https://github.com/jnmartin84/sf64-dc). The graphics task is reported done once its frame is submitted.
  - Vertices are transformed with the SH-4's `ftrv` (KallistiOS `mat_trans_nodiv`), lit with the game's directional lights, clipped against the near plane and culled in screen space. Each vertex's projected position, UVs and colors are cached until the material changes.
  - Everything goes into the translucent list with presorting off, so polygons draw in submission order like the RDP, with depth compare and write set per material. The N64's 320x240 is scaled to 640x480.
  - Texture loads go into an emulated 4 KB TMEM (`gfx_tex.c`). Tiles are decoded from it (CI4/CI8 with RGBA16 or IA16 palettes, I4/I8, IA4/IA8/IA16, RGBA16/RGBA32) into twiddled ARGB1555/4444 or RGB565 textures, cached by a hash of their TMEM contents and parameters and evicted when unused.
  - The color combiner (`gfx_cc.c`) is reduced per vertex to texture × color + offset, which the PowerVR does natively. The blender maps to alpha blending, and fog to the PowerVR's per-vertex fog.
  - Fill and texture rectangles (fades, text boxes, the HUD) become screen-space quads.
- **Audio** (milestone 5). The desktop recomp runs a recompiled copy of the RSP audio microcode, which is far too heavy for the SH-4. Instead, the audio command lists will be processed in C, and the mixed output streamed to the AICA.
- **The mod** (milestone 6). The Dreamcast can't recompile `.nrm` files at runtime. Instead, the mod's MIPS ELF will be recompiled ahead of time (N64Recomp's offline mod tooling) and its hooks wired in when the game is generated. It only needs `recomp_printf` and `recomp_get_config` from the recomp runtime.

## Building

Requirements: Docker. Everything else (KallistiOS, the SH-4 and ARM compilers, CMake, mkdcdisc) lives in the toolchain image.

```
git clone --recurse-submodules <this repo>
docker build -t q64dc-toolchain docker        # once; builds GCC, takes 30-90 minutes
copy <your ROM> quest64.us.z64                 # big-endian .z64, US 1.0
scripts\dc.ps1 make -j16                       # Windows (scripts/dc.sh on Linux/macOS)
```

`make` builds the patched N64Recomp, recompiles the ROM into `build/recomp/`, generates stubs for any runtime function that isn't implemented (`build/runtime_stubs.c`; there are none now), and links `build/quest64-dc.elf`. `make cdi` also builds `build/quest64-dc.cdi`, a self-booting disc with the ELF and the ROM. `make size` prints the section sizes. `scripts\dc.ps1 bash` opens a shell in the toolchain.

The runtime looks for the ROM at `/cd/quest64.z64` (the disc), `/pc/quest64.us.z64` (dcload's host filesystem) and `/sd/quest64.z64`.

## Running

The runtime prints its progress once a second to the debug console (dcload, or the serial port): video retraces, buffer swaps, graphics and audio tasks, ROM DMA, and the game's mode, state and map.

- **Emulator, headless.** `docker build -t q64dc-flycast docker/flycast` once, then `scripts\dc.ps1 make cdi` and `scripts\flycast.ps1 [seconds] [screenshot interval]` (`scripts/flycast.sh` on Linux/macOS). It runs Flycast under Xvfb and prints the serial console; with an interval, it also saves a screenshot every that many seconds to `build/screens/`. Flycast has no input in this mode; build with `make clean` then `make cdi RT_DEFINES=-DQ64DC_AUTOPRESS` to have the runtime press Start and A every few seconds.
- **Emulator, with a window.** Open `build/quest64-dc.cdi` in Flycast. To see the log, set `Debug.SerialConsoleEnabled = yes` in Flycast's `emu.cfg` and start Flycast from a terminal.
- **Dreamcast with dcload-ip or dcload-serial.** From the repo directory, `dc-tool-ip -t <dreamcast ip> -c . -x build/quest64-dc.elf` (or `dc-tool-ser -c . -x ...`). The `-c .` makes the repo the `/pc/` filesystem, so the ROM is read from `quest64.us.z64` over the link. That's slow, especially over serial, so the disc is better for longer runs.
- **Dreamcast from disc.** Burn `build/quest64-dc.cdi` to a CD-R, or put it on an optical drive emulator.

## Layout

| Path | Purpose |
| --- | --- |
| `config/quest64.us.toml` | N64Recomp config (from Quest64-Recomp's `us.rev0.toml`, with `gpr_32bit`) |
| `runtime/include/recomp.h` | SH-4 version of N64Recomp's `recomp.h`: 32-bit GPRs, RDRAM access, FPU rounding |
| `runtime/src/` | Dreamcast runtime: startup, function lookup and overlays, threads and message queues, VI and timers, ROM DMA, controllers, Controller Pak |
| `runtime/src/gfx*.c` | F3DEX renderer: display lists (`gfx.c`), TMEM and textures (`gfx_tex.c`), color combiner (`gfx_cc.c`) |
| `scripts/gen_stubs.py` | Stubs every runtime function the game calls that isn't implemented |
| `scripts/flycast.ps1`, `scripts/flycast.sh` | Headless Flycast run with optional screenshots |
| `tools/N64Recomp` | Upstream N64Recomp (submodule, pinned to the commit Quest64-Recomp uses) |
| `tools/patches/` | Patches applied to it by `scripts/build_n64recomp.sh` |
| `Quest64Syms` | Function and data symbols of the ROM (submodule) |
| `docker/Dockerfile` | Toolchain image |
| `docker/flycast/Dockerfile` | Headless Flycast image (downloads the official Flycast release; ImageMagick for screenshots) |

## License

GPL-3.0, like Quest64-Recomp. See [COPYING](COPYING).
