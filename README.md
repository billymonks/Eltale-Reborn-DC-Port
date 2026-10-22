# Quest 64 for Dreamcast

A Sega Dreamcast port of Quest 64 (USA), built from [N64Recomp](https://github.com/N64Recomp/N64Recomp)'s static recompilation of the game, the same base as [Quest 64: Recompiled](https://github.com/Rainchus/Quest64-Recomp). The long-term goal is to run the Eltale Reborn: Holy Magic Century mod on the Dreamcast.

**This repository contains no game data.** You need your own Quest 64 (USA) ROM to build it, and builds must not be shared.

## Status

Milestone 1 (toolchain and recompilation) is done. Nothing is playable yet: the ELF links and starts the game's entrypoint, then stops at the first libultra function the runtime doesn't implement.

| # | Milestone | State |
| --- | --- | --- |
| 1 | Docker toolchain (KallistiOS v2.2.1, GCC 14.3.0), N64Recomp with 32-bit registers, linkable ELF | done |
| 2 | Runtime: threads, message queues, timers, VI, PI DMA from disc, controllers; game boots headless | |
| 3 | F3DEX renderer on the PowerVR; title screen | |
| 4 | Rooms load from disc, Dreamcast controller, VMU saves | |
| 5 | Audio: audio microcode commands in C, streamed to the AICA | |
| 6 | Eltale Reborn compiled in ahead of time | |
| 7 | Performance work on real hardware | |

## How it works

N64Recomp turns every MIPS function of the ROM into a C function that operates on a register context and a byte array standing in for the N64's RDRAM. The desktop recomp compiles that C for x86/ARM and runs it on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) and RT64. Here the same C is compiled for the Dreamcast's SH-4 with KallistiOS, and the runtime underneath is written for the Dreamcast.

- **32-bit registers.** N64Recomp normally models every GPR as 64 bits, which the 32-bit SH-4 would emulate with register pairs. Quest 64 never uses the upper halves: its 64-bit libultra helpers (`__ll_div`, `__ull_rem`, ...) are already runtime functions, and none of the other ~87,000 instructions is a 64-bit integer operation. The `gpr_32bit` config option (`tools/patches/n64recomp-gpr32.patch`) makes the recompiler reject 64-bit GPR instructions, emit 32-bit `hi`/`lo`, and call `DIV32`/`DIVU32` instead of 64-bit division (a libgcc call on SH-4). `runtime/include/recomp.h` then defines `gpr` as `uint32_t`.
- **Memory.** The game runs in the N64's base 4 MB. RDRAM is a 4 MB block in main RAM, stored as 32-bit words in host order like the desktop runtime does (SH-4 runs little-endian, as x86 does). The 16 MB ROM doesn't fit in the Dreamcast's 16 MB of RAM, so it stays on the disc and is read on demand by the PI DMA functions.
- **Graphics** (milestone 3). The game uses the F3DEX 1.23 microcode. Display lists will be interpreted on the CPU and drawn with the PowerVR, along the lines of jnmartin84's Fast3D backends for [sm64-dc](https://github.com/jnmartin84/sm64-dc), [sf64-dc](https://github.com/jnmartin84/sf64-dc) and [mk64-dc](https://github.com/jnmartin84/mk64-dc).
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

`make` builds the patched N64Recomp, recompiles the ROM into `build/recomp/`, generates stubs for runtime functions that don't exist yet (`build/runtime_stubs.c`), and links `build/quest64-dc.elf`. `make size` prints the section sizes. `scripts\dc.ps1 bash` opens a shell in the toolchain.

The runtime looks for the ROM at `/cd/quest64.z64` (the disc), `/pc/quest64.us.z64` (dcload's host filesystem) and `/sd/quest64.z64`.

## Layout

| Path | Purpose |
| --- | --- |
| `config/quest64.us.toml` | N64Recomp config (from Quest64-Recomp's `us.rev0.toml`, with `gpr_32bit`) |
| `runtime/include/recomp.h` | SH-4 version of N64Recomp's `recomp.h`: 32-bit GPRs, RDRAM access, FPU rounding |
| `runtime/src/` | Dreamcast runtime: startup, ROM access, function lookup and overlays |
| `scripts/gen_stubs.py` | Stubs every runtime function the game calls that isn't implemented yet |
| `tools/N64Recomp` | Upstream N64Recomp (submodule, pinned to the commit Quest64-Recomp uses) |
| `tools/patches/` | Patches applied to it by `scripts/build_n64recomp.sh` |
| `Quest64Syms` | Function and data symbols of the ROM (submodule) |
| `docker/Dockerfile` | Toolchain image |

## License

GPL-3.0, like Quest64-Recomp. See [COPYING](COPYING).
