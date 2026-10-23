# Quest 64 for Dreamcast

A Sega Dreamcast port of Quest 64 (USA), built from [N64Recomp](https://github.com/N64Recomp/N64Recomp)'s static recompilation of the game, the same base as [Quest 64: Recompiled](https://github.com/Rainchus/Quest64-Recomp). The long-term goal is to run the Eltale Reborn: Holy Magic Century mod on the Dreamcast.

**This repository contains no game data.** You need your own Quest 64 (USA) ROM to build it, and builds must not be shared.

## Status

The game is playable on the Dreamcast with its music and sound effects. In Flycast, towns, fields, caves, castles and interiors load in under a second, random battles play out, the spell and Start menus work, and saves go to the VMU and load back after a reboot. Milestone 7 (performance) is under way: with audio on, the opening, the title, the monastery and the Blue Cave run at the game's full 30 fps (60 for the opening), Melrode at 27 and battles at 26.7 (see [Performance](#performance)).

| # | Milestone | State |
| --- | --- | --- |
| 1 | Docker toolchain (KallistiOS v2.2.1, GCC 14.3.0), N64Recomp with 32-bit registers, linkable ELF | done |
| 2 | Runtime: threads, message queues, timers, VI, PI DMA from disc, controllers; game boots headless | done |
| 3 | F3DEX renderer on the PowerVR; title screen | done |
| 4 | Rooms load from disc, Dreamcast controller, VMU saves | done |
| 5 | Audio: audio microcode commands in C, streamed to the AICA | done |
| 6 | Eltale Reborn compiled in ahead of time | |
| 7 | Performance: 30 fps everywhere with audio on | in progress: 30 fps except Melrode and battles |

## How it works

N64Recomp turns every MIPS function of the ROM into a C function that operates on a register context and a byte array standing in for the N64's RDRAM. The desktop recomp compiles that C for x86/ARM and runs it on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) and RT64. Here the same C is compiled for the Dreamcast's SH-4 with KallistiOS, and the runtime underneath is written for the Dreamcast.

- **32-bit registers.** N64Recomp normally models every GPR as 64 bits, which the 32-bit SH-4 would emulate with register pairs. Quest 64 never uses the upper halves: its 64-bit libultra helpers (`__ll_div`, `__ull_rem`, ...) are already runtime functions, and none of the other ~87,000 instructions is a 64-bit integer operation. The `gpr_32bit` config option (`tools/patches/n64recomp-gpr32.patch`) makes the recompiler reject 64-bit GPR instructions, emit 32-bit `hi`/`lo`, and call `DIV32`/`DIVU32` instead of 64-bit division (a libgcc call on SH-4). `runtime/include/recomp.h` then defines `gpr` as `uint32_t`.
- **Memory.** The game runs in the N64's base 4 MB. RDRAM is a 4 MB block in main RAM, stored as 32-bit words in host order like the desktop runtime does (SH-4 runs little-endian, as x86 does). The 16 MB ROM doesn't fit in the Dreamcast's 16 MB of RAM, so it stays on the disc and is read on demand by the PI DMA functions.
- **Threads.** Each N64 thread is a KallistiOS thread, but only one runs game code at a time, as on the N64's single CPU: a thread gives up the CPU when it blocks on a message queue, and the highest-priority ready thread runs next (`runtime/src/sched.cpp`). An events thread raises the VI retrace on the Dreamcast's vblank and fires `osSetTimer` timers (`events.c`). `osSpTaskStartGo` queues graphics tasks for an "n64 rsp" thread below the game threads, which runs them through the renderer while the game goes on, as the RSP runs beside the N64's CPU; audio tasks run through the audio interpreter at once. Each sends the SP and DP done messages when finished.
- **Controllers.** Dreamcast controllers are read through Maple (`cont.c`); see [Controls](#controls).
- **ROM reads.** PI DMA reads the ROM from the disc through a 1.5 MB LRU cache of 32 KB blocks (`rom.c`), read with KallistiOS's `fs_read` into 32-byte-aligned buffers so each block is one multi-sector GD-ROM transfer. A room change misses 10-40 blocks at about 30 ms each in Flycast; play in a room reads only from the cache.
- **Saves.** The game's Controller Pak is emulated on the first VMU (`pak.c`); see [Saves](#saves).
- **Graphics.** The game uses the F3DEX 1.23 microcode. `gfx.c` interprets its display lists on the CPU and draws them with KallistiOS's PowerVR API directly (no GL), in the spirit of jnmartin84's Fast3D backends for [sm64-dc](https://github.com/jnmartin84/sm64-dc) and [sf64-dc](https://github.com/jnmartin84/sf64-dc). The graphics task is reported done once its frame is submitted.
  - Vertices are transformed with the SH-4's `ftrv` (KallistiOS `mat_trans_nodiv`), lit with the game's directional lights, clipped against the near plane and culled in screen space. Triangles that cross the near plane are culled on the sign of their clip-space determinant before clipping, so walls the camera backs into stay hidden. Each vertex's projected position, UVs and colors are cached until the material changes.
  - Everything goes into the translucent list with presorting off, so polygons draw in submission order like the RDP, with depth compare and write set per material. Depth is 1/w. Decals (`ZMODE_DEC`: carpets, windows, shadows) are moved toward the camera by the depth slope of two pixels plus 1/4096 of their depth, which stands in for the RDP's decal tolerance. The N64's 320x240 is scaled to 640x480.
  - Texture loads go into an emulated 4 KB TMEM (`gfx_tex.c`). Tiles are decoded from it (CI4/CI8 with RGBA16 or IA16 palettes, I4/I8, IA4/IA8/IA16, RGBA16/RGBA32) into twiddled ARGB1555/4444 or RGB565 textures, cached by a hash of their TMEM contents and parameters and evicted when unused.
  - The color combiner (`gfx_cc.c`) is reduced per vertex to texture × color + offset, which the PowerVR does natively. The blender maps to alpha blending, and fog to the PowerVR's per-vertex fog.
  - Fill and texture rectangles (fades, text boxes, the HUD) become screen-space quads.
- **Audio.** The desktop recomp runs a recompiled copy of the RSP audio microcode, which is far too heavy for the SH-4. Here the command lists are interpreted in C and the mixed output is streamed to the AICA; see [Audio](#audio).
- **The mod** (milestone 6). The Dreamcast can't recompile `.nrm` files at runtime. Instead, the mod's MIPS ELF will be recompiled ahead of time (N64Recomp's offline mod tooling) and its hooks wired in when the game is generated. It only needs `recomp_printf` and `recomp_get_config` from the recomp runtime.

## Audio

Quest 64 uses libultra's standard audio microcode, `aspMain` (the "ABI 1" command set), through the libultra synthesizer. Every retrace the game's audio thread builds a command list, starts it as an RSP task and hands the previous task's output to the AI.

- **Commands** (`runtime/src/audio_hle.c`). The interpreter implements the 16 commands of the game's microcode, all of which the game uses: VADPCM decoding with loops, the 4-tap resampler, the volume/envelope mixer into the dry and reverb buses, the mixer, the reverb's pole filter, DMEM moves, buffer loads and saves through RDRAM, and the final interleave. It is written from public descriptions of the ABI and cross-checked with mupen64plus-rsp-hle (GPL-2.0-or-later); the resampler's filter table is read from the game's own microcode data in RDRAM. Its output is bit-identical to Quest64-Recomp's recompilation of the game's microcode (see below), with one known difference: the pole filter's first call after the reverb is set up starts from zero history, where the microcode reads two leftover scratch words.
- **Output** (`runtime/src/audio.c`). The game asks for 22,050 Hz; `osAiSetFrequency` rounds that as libultra does, to 22,047 Hz, and starts a KallistiOS sound stream at that rate. Every buffer the game hands to `osAiSetNextBuffer` goes into an 8192-frame ring that a high-priority thread feeds to the stream (2 KB per channel of AICA memory, refilled every 4 ms as needed). The game sizes each buffer from `osAiGetLength`. Game threads only switch at runtime calls here, so the audio thread runs late and in bursts, and a timed model of the AI would starve it. Instead `osAiGetLength` reports whatever makes the game's own formula ask for what the AICA will need: a retrace's worth of samples, plus a quarter of the ring's distance from its 2048-frame target. That keeps the latency to about 140 ms.
- **Cost.** Measured in Flycast at milestone 5 with `tests/audio.txt`, against the same run with `RT_DEFINES=-DAUDIO_OFF`, which skips the commands. Flycast's SH-4 timing isn't cycle-accurate, so real hardware will differ. Since milestone 7's faster loops, audio takes 18-21% of the CPU in the field and in battles (see [Performance](#performance)).

  | Scene | Audio CPU | Per audio frame | fps with audio | fps without |
  | --- | --- | --- | --- | --- |
  | Title screen (sound effects only) | 5% | 0.9 ms | 54.8 | 55.8 |
  | Opening cutscene | 22% | 3.7 ms | 40.6 | 49.4 |
  | Monastery hall | 24% | 4.0 ms | 18.1 | 19.2 |
  | Melrode village | 27% | 4.4 ms | 12.6 | 18.6 |
  | Battle on the Holy Plain | 23% | 3.9 ms | 14.9 | 18.8 |

  The reverb alone costs about 0.9 ms per frame; the rest is per voice, mostly the envelope mixer (about a third), then the resampler and ADPCM decoding. `RT_DEFINES=-DAUDIO_PROFILE` adds the time per command to the progress log. Moving the voices to the AICA's own 64 channels, as jnmartin84's ports do, would remove most of this cost (see milestone 7).

**Checking it.** `tools/audio_test/run.sh <Quest64-Recomp checkout> [seeds]` builds a host program that runs random command lists, shaped like the game's, through both `audio_hle.c` and Quest64-Recomp's recompiled microcode (`rsp/aspMain.cpp`, generated from the ROM by that project's build), and compares every buffer, the AI output and the ADPCM state: `docker run --rm -v "${PWD}:/src" -v "<Quest64-Recomp>:/qr:ro" -w /src q64dc-toolchain bash tools/audio_test/run.sh /qr 50`. To listen, `scripts\flycast.ps1 175 -Test -AudioDump build/audio/audio.raw` records Flycast's output, and `scripts/audio_dump.py build/audio/audio.raw --clip <name> <start> <seconds>` prints its loudness, peak and clipping over time and cuts WAV clips.

## Performance

**The game's frame rate is its speed.** Quest 64 advances its world by a fixed step every frame, not by elapsed time. Its field loop (`func_80001800`) waits until its scheduler has counted two retraces since the last frame, then builds the next one, so it runs at 30 fps at most and moves at its designed speed only then. Below 30 fps the game runs in slow motion rather than skipping ahead: `tests/speed.txt` walks the Holy Plain normally and then with `burn 50` slowing every frame down, and Brian covers the same distance per frame, but fewer frames per second. Running at the N64's speed therefore means 30 fps (the opening's cutscene runs at 60). There's nothing to interpolate.

**Measuring.** `tests/perf.txt` visits six scenes and profiles 10 s of each. The test command `profile_start` starts a 1000 Hz sampler of the program counter on TMU1 (`runtime/src/profile.c`), and `profile_dump` prints its buckets with the retraces and frames in between. In the container, `scripts/perf_fps.py build/perf.log` prints each scene's fps, and `scripts/profile.py build/perf.log` the time per object file and function, with idle time as "(idle)". The periodic log pauses during a profile window, since release discs don't print it.

fps in Flycast 2.7 with audio on. Each column adds a change to the previous one:

| Scene | Milestone 5 | RSP thread | Log thread | `restrict` | fsqrt, log paused | Builtins, audio loops |
| --- | --- | --- | --- | --- | --- | --- |
| Opening | 53.1 | 50.9 | 55.3 | 56.7 | 58.6 | 58.7 |
| Title | 54.7 | 53.0 | 58.3 | 58.3 | 59.9 | 59.9 |
| Monastery hall | 18.1 | 26.0 | 28.3 | 28.4 | 30.0 | 30.0 |
| Melrode | 15.4 | 21.7 | 25.1 | 25.8 | 26.4 | 27.0 |
| Blue Cave | 18.9 | 26.3 | 28.8 | 28.8 | 30.0 | 30.0 |
| Battle (Holy Plain) | 15.2 | 16.8 | 22.2 | 23.4 | 26.3 | 26.7 |

- **RSP thread.** Graphics tasks used to run inside the game's scheduler thread, before the main thread reset its retrace count, so every frame then waited two more retraces and the CPU idled for a quarter of the time. They now run on a thread of their own below the game threads.
- **Log thread.** The once-a-second statistics went to the serial port (about 90 us a character, waited for) from the VI thread. They're printed by their own thread now, and only on test discs.
- **`restrict`.** The generated functions take `rdram` and `ctx` as restrict pointers, so GCC keeps the N64 registers in SH-4 registers across RDRAM stores (8% less code).
- **fsqrt, builtins.** KOS compiles with `-fno-builtin`, which turned every `sqrtf` into a software square root; `-fbuiltin -fno-math-errno` makes it the FPU's `fsqrt`, as exact as the N64's `sqrt.s`. The "log paused" column also starts the Melrode, cave and battle windows 5 s after their warp instead of 1.5 s, past the map load.
- **Audio loops.** One bus per pass, four samples at a time, with the rounding constant in a register; still bit-identical to the microcode.
- **Tried and dropped.** `-Os` for the generated code is 2.5% smaller and slightly slower in Flycast, which doesn't model the SH-4's caches, so it is worth retrying on hardware.

**Where the time goes** in a battle, which never idles: the renderer 48% (`gfx_run_task` 15%, texture loads into TMEM and texture lookups 20%, the color combiner 5%), the game's own code 25%, audio 20%, KOS and libc 3%. Melrode idles 26% of the time but still averages 27 fps. Its frames take either two or three retraces, and a frame that needs slightly more than two waits for the third. 30 fps in Melrode and in battles needs about 13% less work per frame, most of which can only come from the renderer (texture loads re-copy and re-hash TMEM for every texture use, even for textures already cached) or from mixing voices on the AICA.

## Controls

| Dreamcast | N64 | In Quest 64 |
| --- | --- | --- |
| Analog stick | Stick | Walk; move cursors |
| A | A | Talk, confirm, attack |
| B | B | Cancel, close menus |
| D-pad | C buttons | Spirits; C-up opens the spell menu |
| X, left trigger | Z | |
| Y, right trigger | R | |
| Start | Start | Start menu (status, items) |

The stick is scaled to the N64's range: a radial deadzone of 14 (out of the Dreamcast's ±128), full deflection at 110, mapped to the N64 stick's ±80, with the direction kept. Menus such as the title screen's NEW / LOAD / DELETE move with the stick, not the D-pad.

## Saves

The game sees a Controller Pak; its files are kept on the first VMU found (normally the one in controller A's first slot), as a single file `QUEST64.PAK` (3 blocks for one save) with a Quest 64 icon and the description "Quest 64 saves". Every Controller Pak file is stored whole, so all 0x200 bytes of the game's save, including bytes 0x160-0x1FF that Eltale Reborn uses, round-trip. The VMU is read at boot and whenever a different VMU is plugged in, and written only after the game writes, 300 ms after its last write (about 0.7 s in Flycast). Don't remove the VMU while it's being written.

Without a VMU the game reports no Controller Pak, as on an N64 without one, and plays on without saving. If the VMU can't hold the save, creating a save file fails with the game's "full" error.

## Building

Requirements: Docker. Everything else (KallistiOS, the SH-4 and ARM compilers, CMake, mkdcdisc) lives in the toolchain image.

```
git clone --recurse-submodules <this repo>
docker build -t q64dc-toolchain docker        # once; builds GCC, takes 30-90 minutes
copy <your ROM> quest64.us.z64                 # big-endian .z64, US 1.0
scripts\dc.ps1 make -j16                       # Windows (scripts/dc.sh on Linux/macOS)
```

`make` builds the patched N64Recomp, recompiles the ROM into `build/recomp/`, generates stubs for any runtime function that isn't implemented (`build/runtime_stubs.c`; there are none now), and links `build/quest64-dc.elf`. `make cdi` also builds `build/quest64-dc.cdi`, a self-booting disc with the ELF and the ROM; it never contains a test script. `make size` prints the section sizes. `scripts\dc.ps1 bash` opens a shell in the toolchain.

The runtime looks for the ROM at `/cd/quest64.z64` (the disc), `/pc/quest64.us.z64` (dcload's host filesystem) and `/sd/quest64.z64`.

## Running

Test discs print their progress once a second to the debug console (dcload, or the serial port): video retraces, buffer swaps, graphics and audio tasks, ROM DMA, and the game's mode, state and map, then the renderer's and the audio's statistics. Every 10 seconds they add the ROM cache, the time no game thread could run, and how many frames took one, two, three or more retraces. Release discs don't print them, as the serial port is slow enough to cost frames.

- **Emulator, headless.** `docker build -t q64dc-flycast docker/flycast` once, then `scripts\dc.ps1 make cdi` and `scripts\flycast.ps1 [seconds] [screenshot interval]` (`scripts/flycast.sh` on Linux/macOS). It runs Flycast under Xvfb and prints the serial console; with an interval, it also saves a screenshot every that many seconds to `build/screens/shot_<seconds>.png`. Flycast's VMU images live in `build/flycast-home/`, so saves survive between runs; `-ResetVmu` (`RESET_VMU=1`) starts with blank ones. `-Renderer n` (`FLYCAST_REND=n`) picks Flycast's renderer (`pvr.rend`). The image uses Flycast 2.7; `docker build --build-arg FLYCAST_VERSION=2.6 --build-arg FLYCAST_APPIMAGE=flycast-x86_64.AppImage -t q64dc-flycast26 docker/flycast` builds a 2.6 one.
- **Test scripts.** Flycast has no input in headless mode, so `make test-cdi TEST_SCRIPT=tests/<script>.txt` builds `build/quest64-dc-test.cdi`, a separate disc with a script that the runtime plays as controller 1 input (`runtime/src/test.c` lists the commands: button taps, the stick, waiting for Brian to be free, warps, placing Brian, forcing a random battle, memory peeks, pokes and watches, Brian's position, named screenshots, slowing the game down, profiling). Run it with `scripts\flycast.ps1 -Test` (`TEST=1`). A test disc prints a banner on the console and draws a red bar across the top of the screen. The scripts in `tests/`:
  - `tour.txt`: a new game, then Melrode, the Holy Plain, the Blue Cave, the monastery, Dondoran Castle, Dindom Dries and the Melrode Inn, with the spell and Start menus.
  - `battle.txt`: a random battle on the Holy Plain.
  - `gfx.txt`: bursts of screenshots while walking in the monastery, Melrode, the Blue Cave and a monastery corridor, then a battle, for depth and culling errors.
  - `save.txt` (run with `-ResetVmu`), then `load.txt`: saves day 7 at the Melrode Inn, then loads it from a fresh boot.
  - `autopress.txt`: presses Start and A every few seconds.
  - `audio.txt`: lets the opening play, waits on the title, then stays in the monastery, Melrode and a battle, for recording the music (see [Audio](#audio)).
  - `perf.txt`: profiles the opening, the title, the monastery, Melrode, the Blue Cave and a battle (see [Performance](#performance)).
  - `speed.txt`: walks the Holy Plain at the normal frame rate and slowed down, printing Brian's position.
- **Emulator, with a window.** Open `build/quest64-dc.cdi` in Flycast (tested with 2.6 and 2.7, OpenGL and Vulkan, HLE and real BIOS). To see the log, set `Debug.SerialConsoleEnabled = yes` in Flycast's `emu.cfg` and start Flycast from a terminal.
- **Dreamcast with dcload-ip or dcload-serial.** From the repo directory, `dc-tool-ip -t <dreamcast ip> -c . -x build/quest64-dc.elf` (or `dc-tool-ser -c . -x ...`). The `-c .` makes the repo the `/pc/` filesystem, so the ROM is read from `quest64.us.z64` over the link. That's slow, especially over serial, so the disc is better for longer runs.
- **Dreamcast from disc.** Burn `build/quest64-dc.cdi` to a CD-R, or put it on an optical drive emulator.

## Layout

| Path | Purpose |
| --- | --- |
| `config/quest64.us.toml` | N64Recomp config (from Quest64-Recomp's `us.rev0.toml`, with `gpr_32bit`) |
| `runtime/include/recomp.h` | SH-4 version of N64Recomp's `recomp.h`: 32-bit GPRs, RDRAM access, FPU rounding, `neg.s`/`abs.s` |
| `runtime/src/` | Dreamcast runtime: startup, function lookup and overlays, threads and message queues, VI and timers, ROM DMA, controllers, Controller Pak |
| `runtime/src/gfx*.c` | F3DEX renderer: display lists (`gfx.c`), TMEM and textures (`gfx_tex.c`), color combiner (`gfx_cc.c`) |
| `runtime/src/audio*.c` | Audio microcode commands (`audio_hle.c`), the AI and the sound stream (`audio.c`) |
| `tools/audio_test/` | Bit-for-bit comparison of `audio_hle.c` with Quest64-Recomp's recompiled audio microcode |
| `scripts/gen_stubs.py` | Stubs every runtime function the game calls that isn't implemented |
| `runtime/src/test.c` | Scripted input for test discs |
| `runtime/src/profile.c` | Program counter sampler for test discs |
| `scripts/profile.py`, `scripts/perf_fps.py` | Profile by function and object file; fps per scene of `tests/perf.txt` |
| `tests/` | Test scripts |
| `scripts/flycast.ps1`, `scripts/flycast.sh` | Headless Flycast run with screenshots and, optionally, an audio recording |
| `scripts/audio_dump.py` | Loudness and clipping of an audio recording; cuts WAV clips |
| `tools/N64Recomp` | Upstream N64Recomp (submodule, pinned to the commit Quest64-Recomp uses) |
| `tools/patches/` | Patches applied to it by `scripts/build_n64recomp.sh` |
| `Quest64Syms` | Function and data symbols of the ROM (submodule) |
| `docker/Dockerfile` | Toolchain image |
| `docker/flycast/Dockerfile` | Headless Flycast image (downloads the official Flycast release; ImageMagick for screenshots) |

## License

GPL-3.0, like Quest64-Recomp. See [COPYING](COPYING).
