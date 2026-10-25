# Quest 64 for Dreamcast

A Sega Dreamcast port of Quest 64 (USA), built from [N64Recomp](https://github.com/N64Recomp/N64Recomp)'s static recompilation of the game, the same base as [Quest 64: Recompiled](https://github.com/Rainchus/Quest64-Recomp). It can also run the Eltale Reborn: Holy Magic Century mod, compiled in ahead of time as a separate disc (see [Mods](#mods)).

**This repository contains no game data.** You need your own Quest 64 (USA) ROM to build it, and builds must not be shared.

## Status

The game is playable on the Dreamcast with its music and sound effects. In Flycast, towns, fields, caves, castles and interiors load in under a second, random battles play out, the spell and Start menus work, and saves go to the VMU and load back after a reboot. Milestone 7 (performance) is under way: with audio on, the opening, the title, the monastery, the Blue Cave and battles run at the game's full 30 fps (60 for the opening), the Melrode scene at 29.7, its only lost frames being a house door's map load, and the Holy Plain outside Melrode at 29.3 (see [Performance](#performance)). Eltale Reborn runs on its own disc: its logo, prologue, scripted scenes, dialogue with choices, signs, scene NPCs, script-started battles and save data work, at the same frame rates as the unmodded game.

| # | Milestone | State |
| --- | --- | --- |
| 1 | Docker toolchain (KallistiOS v2.2.1, GCC 14.3.0), N64Recomp with 32-bit registers, linkable ELF | done |
| 2 | Runtime: threads, message queues, timers, VI, PI DMA from disc, controllers; game boots headless | done |
| 3 | F3DEX renderer on the PowerVR; title screen | done |
| 4 | Rooms load from disc, Dreamcast controller, VMU saves | done |
| 5 | Audio: audio microcode commands in C, streamed to the AICA | done |
| 6 | Eltale Reborn compiled in ahead of time | done (`MOD=eltale`, optionally `MOD=eltale,gameplay`) |
| 7 | Performance: 30 fps everywhere with audio on | in progress: 30 fps in the measured scenes, except during map loads and on the Holy Plain (29.3) |

## How it works

N64Recomp turns every MIPS function of the ROM into a C function that operates on a register context and a byte array standing in for the N64's RDRAM. The desktop recomp compiles that C for x86/ARM and runs it on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) and RT64. Here the same C is compiled for the Dreamcast's SH-4 with KallistiOS, and the runtime underneath is written for the Dreamcast.

- **32-bit registers.** N64Recomp normally models every GPR as 64 bits, which the 32-bit SH-4 would emulate with register pairs. Quest 64 never uses the upper halves: its 64-bit libultra helpers (`__ll_div`, `__ull_rem`, ...) are already runtime functions, and none of the other ~87,000 instructions is a 64-bit integer operation. The `gpr_32bit` config option (`tools/patches/n64recomp-gpr32.patch`) makes the recompiler reject 64-bit GPR instructions, emit 32-bit `hi`/`lo`, and call `DIV32`/`DIVU32` instead of 64-bit division (a libgcc call on SH-4). `runtime/include/recomp.h` then defines `gpr` as `uint32_t`.
- **Memory.** The game runs in the N64's base 4 MB. RDRAM is a 4 MB block in main RAM, stored as 32-bit words in host order like the desktop runtime does (SH-4 runs little-endian, as x86 does). The 16 MB ROM doesn't fit in the Dreamcast's 16 MB of RAM, so it stays on the disc and is read on demand by the PI DMA functions.
- **Threads.** Each N64 thread is a KallistiOS thread, but only one runs game code at a time, as on the N64's single CPU: a thread gives up the CPU when it blocks on a message queue, and the highest-priority ready thread runs next (`runtime/src/sched.cpp`). An events thread raises the VI retrace on the Dreamcast's vblank and fires `osSetTimer` timers (`events.c`). `osSpTaskStartGo` queues graphics tasks for an "n64 rsp" thread below the game threads, which runs them through the renderer while the game goes on, as the RSP runs beside the N64's CPU; audio tasks run through the audio interpreter at once. Each sends the SP and DP done messages when finished; a graphics task's frame then goes to the PowerVR just after a vblank, at the game's frame rate (see [Frame pacing](#performance)).
- **Controllers.** Dreamcast controllers are read through Maple (`cont.c`); see [Controls](#controls).
- **ROM reads.** PI DMA reads the ROM from the disc through a cache of 32 KB blocks (`rom.c`), read with KallistiOS's `fs_read` into 32-byte-aligned buffers so each read is one multi-sector GD-ROM transfer. The 1.1 MB of audio banks, samples and sequences is read at boot and kept, since the audio thread starts its sample DMAs every frame and doesn't wait for them; everything else goes through a 3 MB LRU, and a miss reads up to four blocks in one disc command, as the game reads its files front to back. As on the N64, `osPiStartDma` only starts a transfer: a cached one is copied at once, and the rest wait for a reader thread below the game threads, which sends the DMA's message when the data is in RDRAM, so the other game threads go on meanwhile. Transfers to the same message queue or the same memory stay in order, and an audio task waits for pending reads of the audio data. A map load takes 1-20 disc reads of 60-150 ms each in Flycast; play in a room reads only from the cache. With audio pinned, the game's heap and the cache leave about 5.6 MB of RAM free.
- **Game-code hot spots.** The floor probe's point-in-shape test (`func_8000F238` and its edge test, `func_8000F5A4`), which every actor runs against every nearby shape each frame, is replaced by native C with the same steps in the same order (`collision.c`, through a hook in `config/quest64.us.toml`); building it with `-DFLOOR_VERIFY` and the hooks described in the file runs both and compares every result.
- **Saves.** The game's Controller Pak is emulated on the first VMU (`pak.c`); see [Saves](#saves).
- **Graphics.** The game uses the F3DEX 1.23 microcode. `gfx.c` interprets its display lists on the CPU and draws them with KallistiOS's PowerVR API directly (no GL), in the spirit of jnmartin84's Fast3D backends for [sm64-dc](https://github.com/jnmartin84/sm64-dc) and [sf64-dc](https://github.com/jnmartin84/sf64-dc). The graphics task is reported done once its frame is submitted.
  - Vertices are transformed with the SH-4's `ftrv` (KallistiOS `mat_trans_nodiv`), lit with the game's directional lights, clipped against the near plane and culled in screen space. Triangles that cross the near plane are culled on the sign of their clip-space determinant before clipping, so walls the camera backs into stay hidden. Each vertex's projected position, UVs and colors are cached until the material changes.
  - Each primitive goes to one of the PowerVR's three lists by the RDP's render mode: opaque surfaces to the opaque list, alpha-tested cutouts (fences, trees, foliage) to the punch-through list, which keeps texels with alpha of at least 0x80, and blended surfaces to the translucent list. The PowerVR draws the lists in that order and keeps submission order only within the translucent list (presorting is off), so the RDP's order is kept by phases: opaque untested primitives before the 3D scene (the sky) go first in the opaque list, and from the first untested primitive after it (the HUD, text boxes, fades) everything goes to the translucent list in order. The opaque list is written straight to the PowerVR; the punch-through and translucent lists are built in RAM and sent by DMA at the end of the frame. Depth compare and write are set per material, and depth is 1/w. Decals (`ZMODE_DEC`: carpets, windows, shadows) are moved toward the camera by the depth slope of two pixels plus 1/4096 of their depth, which stands in for the RDP's decal tolerance. The N64's 320x240 is scaled to 640x480.
  - Texture loads go into an emulated 4 KB TMEM (`gfx_tex.c`). Tiles are decoded from it (CI4/CI8 with RGBA16 or IA16 palettes, I4/I8, IA4/IA8/IA16, RGBA16/RGBA32) into twiddled ARGB1555/4444 or RGB565 textures, cached in VRAM and evicted when unused. A cached texture is found by its tile parameters and the loads its bytes came from: each load is keyed by its parameters and a checksum of the RDRAM it reads, taken once per frame per source, so textures the CPU rewrites (text, animated textures) update on the next frame while unchanged ones skip the copy and the hash. Loads are copied into TMEM only when a texture has to be decoded, and tiles that read bytes no load describes fall back to hashing TMEM.
  - The color combiner (`gfx_cc.c`) is reduced per vertex to texture × color + offset, which the PowerVR does natively. For each combiner and set of constant colors it is first turned into small polynomials in the vertex color, so a vertex costs a few multiply-adds. The blender maps to alpha blending.
  - Fog follows the N64's: a factor per vertex from its depth and the game's fog range, applied where the blender mixes in the fog color. Textured primitives use the PowerVR's vertex fog, with the factor in the offset color's alpha and the fog color in its fog color register, which works in all three lists at no cost per pixel. The PowerVR has one fog color, so the first one a frame uses takes it; other fog colors, and untextured primitives (which have no offset color), mix the fog color into their vertex colors on the CPU instead.
  - Fill and texture rectangles (fades, text boxes, the HUD) become screen-space quads. Texture rectangles drawn at one texel per N64 pixel from a whole texel (text, menus, most of the HUD) are point-sampled even when the game asks for bilinear filtering: the RDP's filter returns single texels there, while the PowerVR's at 640x480 would mix in the hidden colors of transparent texels, which in the game's fonts are pink.
- **Audio.** The desktop recomp runs a recompiled copy of the RSP audio microcode, which is far too heavy for the SH-4. Here the command lists are interpreted in C and the mixed output is streamed to the AICA; see [Audio](#audio).
- **Mods.** The Dreamcast can't recompile `.nrm` files at runtime, so a modded build recompiles the mod's code ahead of time, places it at a fixed address in RDRAM and wires its patches and hooks into the game's recompiler config; see [Mods](#mods).

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

| Scene | Milestone 5 | RSP thread | Log thread | `restrict` | fsqrt, log paused | Builtins, audio loops | Texture cache, combiner | Image loads, vertex path | ROM reader | Native floor test | Lists, fog |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Opening | 53.1 | 50.9 | 55.3 | 56.7 | 58.6 | 58.7 | 58.8 | 58.9 | 59.0 | 59.0 | 59.0 |
| Title | 54.7 | 53.0 | 58.3 | 58.3 | 59.9 | 59.9 | 59.9 | 59.9 | 59.9 | 59.9 | 59.9 |
| Monastery hall | 18.1 | 26.0 | 28.3 | 28.4 | 30.0 | 30.0 | 30.0 | 30.0 | 30.0 | 30.0 | 30.0 |
| Melrode | 15.4 | 21.7 | 25.1 | 25.8 | 26.4 | 27.0 | 28.5 | 28.7 | 29.7 | 29.7 | 29.7 |
| Blue Cave | 18.9 | 26.3 | 28.8 | 28.8 | 30.0 | 30.0 | 30.0 | 30.0 | 30.0 | 30.0 | 30.0 |
| Battle (Holy Plain) | 15.2 | 16.8 | 22.2 | 23.4 | 26.3 | 26.7 | 29.2 | 29.4 | 29.7 | 30.0 | 29.9 |

- **RSP thread.** Graphics tasks used to run inside the game's scheduler thread, before the main thread reset its retrace count, so every frame then waited two more retraces and the CPU idled for a quarter of the time. They now run on a thread of their own below the game threads.
- **Log thread.** The once-a-second statistics went to the serial port (about 90 us a character, waited for) from the VI thread. They're printed by their own thread now, and only on test discs.
- **`restrict`.** The generated functions take `rdram` and `ctx` as restrict pointers, so GCC keeps the N64 registers in SH-4 registers across RDRAM stores (8% less code).
- **fsqrt, builtins.** KOS compiles with `-fno-builtin`, which turned every `sqrtf` into a software square root; `-fbuiltin -fno-math-errno` makes it the FPU's `fsqrt`, as exact as the N64's `sqrt.s`. The "log paused" column also starts the Melrode, cave and battle windows 5 s after their warp instead of 1.5 s, past the map load.
- **Audio loops.** One bus per pass, four samples at a time, with the rounding constant in a register; still bit-identical to the microcode.
- **Tried and dropped.** `-Os` for the generated code is 2.5% smaller and slightly slower in Flycast, which doesn't model the SH-4's caches, so it is worth retrying on hardware.
- **Texture cache, combiner.** Texture loads used to be copied into TMEM and every texture use re-hashed TMEM. Now a load is keyed by a per-frame checksum of its RDRAM source and copied only when a texture has to be decoded, and a texture is found by the loads behind it (see [How it works](#how-it-works)). The combiner is reduced once per combiner and constant colors instead of per vertex. The battle in `tests/perf.txt` now fixes the game's random seed (`battle_walk 30000 1234`), so every run fights the same formation; the previous column measures 26.6 with it.
- **Image loads, vertex path.** Loads whose rows aren't a whole number of 8-byte words (the HUD, text boxes and other 2D images) are keyed too, and the vertex loop lost a few multiplies and branches.
- **ROM reader.** The audio data pinned at boot, PI DMA on a reader thread, a 3 MB LRU instead of 1.5 MB and four-block reads (see [How it works](#how-it-works)). The Melrode window read 17 blocks in 17 disc reads (413 ms) and the battle window 3 (63 ms); now Melrode reads 4 blocks in one read and the battle none. The pinned audio data takes 0.6 s to read at boot. The Melrode walk in `tests/perf.txt` goes through a house door (map 0x0F) about 3 s in, and that map load is its remaining loss: 297 frames of 300, against 300 in the same street without the door. `profile_dump` also prints the ROM blocks read in its window, and `perf_fps.py` shows them; `RT_DEFINES="-DROM_SYNC -DROM_NO_PIN -DLRU_BLOCKS=48 -DREAD_RUN=1"` builds the previous behavior for comparison.
- **Native floor test.** The floor probe's point-in-shape test took 16% of a battle's CPU time as recompiled code and takes 5% in C (see [How it works](#how-it-works)), with bit-identical results over 131,072 calls in `tests/perf.txt`.
- **Lists, fog.** Opaque, punch-through and translucent lists, and the game's fog (see [How it works](#how-it-works)). Cutouts used to sit in the translucent list with depth writes, so geometry drawn after them behind a fence or a tree card was hidden in the transparent parts, and there was no fog. The CPU cost is unchanged: the previous renderer measures the same in the same runs (the battle at 29.9 too). Flycast's per-pixel renderers sort translucent polygons per pixel on the host, so moving most polygons out of the translucent list should also lighten the host's work there; the headless runs use the default renderer and don't show it.

**Where the time goes.** A battle is now the renderer 40% (`gfx_run_task` 16%, textures 11%, the combiner 2%), audio 21%, the game's own code 18% (13% recompiled, 5% the floor test) and idles 17%, about 5.6 ms a frame, where it idled 8% before the floor test. The audio mixer is the largest remaining cost that isn't the renderer: moving the voices to the AICA's 64 channels would free most of its 7 ms a frame, but takes days (the game's ADPCM samples would have to be decoded into AICA memory, and its envelopes, pitch changes and reverb remapped), and the measured scenes no longer need it. Flycast doesn't model the SH-4's caches, so hardware may be slower.

**Frame pacing.** An even 30 fps also needs every frame on screen for exactly two vblanks, which an fps counter doesn't show: Flycast's counts frames per second, and frames shown for 3, 1 and 2 vblanks average 30 fps as well. KallistiOS starts the PowerVR's render as soon as a frame's last list is in and shows it at the first vblank after the render ends, so where a frame's build happened to end decided whether it waited for that vblank or the next one. The game's frames came every 2 vblanks, but they were shown for 1, 2 or 3, often in runs of `312312`, which looks like dropped frames in a smooth camera move. Now the RSP thread sends the SP and DP done messages once a frame is built, then hands the frame to the PowerVR just after a vblank, at least the game's frame period after the last one (`present_wait` in `events.c`). The render, about 8 ms in the heaviest scenes, fits in that vblank interval, so every frame shows exactly one vblank after its hand-over. That's up to one vblank more latency. The period is 1 vblank in the 60 fps opening and 2 everywhere else, taken from the game's behaviour, as no game variable holds it: at 2, a 60 fps game has its next task already waiting at every hand-over, and at 1, the gaps between its tasks show 30 fps.

`tests/pace.txt` and `tests/mod_pace.txt` measure it: `pace_start` and `pace_dump` record each vblank, then print how many vblanks each frame stayed on screen, as a histogram and a timeline, and how long after its hand-over each frame showed. The serial log is held back while recording. The test command `pacing 0` switches the cadence off. Frames on screen for exactly the game's period, in Flycast 2.7:

| Scene | Before | After |
| --- | --- | --- |
| Eltale Reborn: the village farewell and Mira on the road (CAM_PAN, NPC_WALK) | 609 of 1203 (51%); 287 for 1 vblank, 299 for 3 | 1184 of 1221 (97%); 3 for 1, 33 for 3 |
| Eltale Reborn: Odran on the Holy Plain | 380 of 813 (47%); 219 for 1, 205 for 3 | 810 of 813 (99.6%); none for 1 or 3 |
| Battle camera swing | 94 of 267 (35%); 87 for 1, 82 for 3 | 267 of 269 (99%) |
| Opening (60 fps) | 1174 of 1177 | 1169 of 1174 |
| Melrode, walking with the camera turning | 115 of 228; 16 for 1, 92 for 3 | 141 of 226; none for 1, 84 for 3 |

The frames left over 2 vblanks are frames the game took longer than 2 vblanks to make, in the same bursts as its task gaps: the walk across Melrode's side streets runs at about 25 fps either way, and the farewell has a few seconds of it among the villagers. The fps in the table above are unchanged (the opening's window, which starts as the logos hand over to the 60 fps cutscene, measures 58.6).

**The path from the monastery.** `tests/path.txt` runs at 30.0 fps on the monastery steps and along the garden fence and 29.3 on the Holy Plain outside Melrode, the same with the previous renderer, and reads nothing from the disc in any of its windows. On the plain the renderer takes 47% (`gfx_run_task` 15%, textures 14%, the combiner 2%), audio 22%, the game's own code 15% and idles 12%. The renderer already skips display lists that F3DEX's `G_CULLDL` rejects, now also behind the near plane (about 4 a frame there). Of about 1,900 triangles a frame there, about 40% are entirely off screen and rejected after their vertices' transform; lighting them only when a triangle uses them would save well under a millisecond a frame. About 200 triangles a frame are fully fogged, but they draw in the fog color over a sky of another color, so skipping them would change the picture.

**Reading from a real disc.** Flycast's GD-ROM reads cost about 20-30 ms for one 32 KB block and 60-80 ms for four, roughly 2 MB/s. A real drive reading a CD-R seeks more slowly (on the order of 100 ms or more per read that isn't next to the last one), so what counts on hardware is the number of separate reads, not the bytes: the reader thread keeps them off the game threads, runs of four blocks quarter their number on map loads, and with the audio data pinned, play within a room doesn't touch the disc at all, so slow seeks show up as longer map loads rather than lost frames. The pinned audio data is one contiguous 1.1 MB read at boot (about 0.6 s in Flycast, perhaps a second on hardware).

**Disc size.** `build/quest64-dc.cdi` is 740 MB but fits a 700 MB (80-minute) CD-R. mkdcdisc pads the data track to the full CD size and writes the files at its end, the outer edge of the disc, where the Dreamcast's drive reads fastest: the track is 316,212 sectors (647.6 MB of 2048-byte data) with the ELF and the ROM in its last 8,700, and the whole disc, with the audio session and the gaps between sessions, is about 328,000 sectors (73 minutes; an 80-minute CD-R holds 360,000). The CDI is bigger than the disc's data because it stores each data sector raw, 2,336 bytes. mkdcdisc's `-N` makes a 22 MB CDI with the files at the slow inner edge instead, which costs read speed on a real drive and gains nothing on an optical drive emulator, so the default stays padded.

## Controls

| Dreamcast | N64 | In Quest 64 | In Eltale Reborn |
| --- | --- | --- | --- |
| Analog stick | Stick | Walk; move cursors | |
| A | A | Talk, confirm, attack | |
| B | B | Cancel, close menus | |
| D-pad | C buttons | Spirits; C-up opens the spell menu | |
| Left trigger | L | | Previous item filter |
| Right trigger | Z | | Next item filter; open chests |
| X | Bit 0x0040 (no N64 button) | | Sprint (hold) |
| Y | R | | Item menu |
| Start | Start | Start menu (status, items) | Start menu (status, map) |

Dreamcast X sends a bit of the N64 button word that no N64 button uses and Quest 64 never reads, so a mod can bind it without taking another button: the field sprint (the gameplay mod's `sprint_speed`, 1.25 by default; earlier versions had it in Eltale Reborn at 1.5) makes Brian's steps longer while X is held (`tests/mod_sprint.txt`). In the unmodded game X does nothing; the right trigger is the only Z.

The stick is scaled to the N64's range: a radial deadzone of 14 (out of the Dreamcast's ±128), full deflection at 110, mapped to the N64 stick's ±80, with the direction kept. Menus such as the title screen's NEW / LOAD / DELETE move with the stick, not the D-pad.

## Saves

The game sees a Controller Pak; its files are kept on the first VMU found (normally the one in controller A's first slot), as a single file `QUEST64.PAK` (3 blocks for one save) with a Quest 64 icon and the description "Quest 64 saves". Every Controller Pak file is stored whole, so all 0x200 bytes of the game's save, including bytes 0x160-0x1FF that Eltale Reborn uses, round-trip. The VMU is read at boot and whenever a different VMU is plugged in, and written only after the game writes, 300 ms after its last write (about 0.7 s in Flycast). Don't remove the VMU while it's being written. Modded discs keep their own file, `Q64<code>.PAK` (`Q64ELT.PAK` for Eltale Reborn, described "Eltale Reborn saves"), so modded and unmodded saves never mix.

Without a VMU the game reports no Controller Pak, as on an N64 without one, and plays on without saving. If the VMU can't hold the save, creating a save file fails with the game's "full" error.

## Building

Requirements: Docker. Everything else (KallistiOS, the SH-4 and ARM compilers, CMake, mkdcdisc) lives in the toolchain image.

```
git clone --recurse-submodules <this repo>
docker build -t q64dc-toolchain docker        # once; builds GCC, takes 30-90 minutes
copy <your ROM> quest64.us.z64                 # big-endian .z64, US 1.0
scripts\dc.ps1 make -j16                       # Windows (scripts/dc.sh on Linux/macOS)
```

`make` builds the patched N64Recomp, recompiles the ROM into `build/recomp/`, generates stubs for any runtime function that isn't implemented (`build/runtime_stubs.c`; there are none now), and links `build/quest64-dc.elf`. `make cdi` also builds `build/quest64-dc.cdi`, a self-booting disc with the ELF and the ROM; it never contains a test script. `make size` prints the section sizes. `scripts\dc.ps1 bash` opens a shell in the toolchain. Modded discs are built with `scripts\mod.ps1` (see [Mods](#mods)).

The runtime looks for the ROM at `/cd/quest64.z64` (the disc), `/pc/quest64.us.z64` (dcload's host filesystem) and `/sd/quest64.z64`.

**Disc title and cover.** The discs are titled `QUEST 64` (test discs `QUEST 64 TEST`) with the serial `Q64-DC` (`Q64-DC-T`); Flycast names VMU saves after the serial, so it stays fixed. For cover art, put a 512x512 PNG of your own at `assets/cover.png` (it is never committed): `make cdi` and `make test-cdi` then convert it with `scripts/make_gdtex.py` to `0GDTEX.PVR` in the disc root, the cover file Flycast, GDEMU menus and their card managers read. Without it the discs have no cover.

**Flycast's game list.** Flycast takes a disc's name and cover from TheGamesDB, searched by the serial, then by the IP.BIN title, and caches them in `data/boxart/flycast-gamedb.json` next to `flycast.exe` (or in its home folder), keyed by file name. Quest 64 isn't in TheGamesDB's Dreamcast list, so the list shows the cover from `0GDTEX.PVR` with the file name. Earlier discs were titled `Quest 64 DC`, which TheGamesDB matched to its homebrew Quake, and Flycast keeps a cached entry until it's removed. To fix the list, close Flycast, back up `flycast-gamedb.json`, and in the object whose `file_name` is `quest64-dc.cdi`, either delete the whole object (Flycast reads the cover from the disc again) or set `"name": "Quest 64"` and `"boxart_path"` to a PNG or JPG in that folder. `parsed` and `scraped` stay `true` so Flycast keeps the entry.

## Running

Test discs print their progress once a second to the debug console (dcload, or the serial port): video retraces, buffer swaps, graphics and audio tasks, ROM DMA, and the game's mode, state and map, then the renderer's and the audio's statistics. Every 10 seconds they add the ROM cache, the time no game thread could run, and how many frames took one, two, three or more retraces. Release discs don't print them, as the serial port is slow enough to cost frames.

- **Emulator, headless.** `docker build -t q64dc-flycast docker/flycast` once, then `scripts\dc.ps1 make cdi` and `scripts\flycast.ps1 [seconds] [screenshot interval]` (`scripts/flycast.sh` on Linux/macOS). It runs Flycast under Xvfb and prints the serial console; with an interval, it also saves a screenshot every that many seconds to `build/screens/shot_<seconds>.png`. Flycast's VMU images live in `build/flycast-home/`, so saves survive between runs; `-ResetVmu` (`RESET_VMU=1`) starts with blank ones. `-Renderer n` (`FLYCAST_REND=n`) picks Flycast's renderer (`pvr.rend`). The image uses Flycast 2.7; `docker build --build-arg FLYCAST_VERSION=2.6 --build-arg FLYCAST_APPIMAGE=flycast-x86_64.AppImage -t q64dc-flycast26 docker/flycast` builds a 2.6 one.
- **Test scripts.** Flycast has no input in headless mode, so `make test-cdi TEST_SCRIPT=tests/<script>.txt` builds `build/quest64-dc-test.cdi`, a separate disc with a script that the runtime plays as controller 1 input (`runtime/src/test.c` lists the commands: button taps, the stick, waiting for Brian to be free, warps, placing Brian, forcing a random battle, memory peeks, pokes and watches, Brian's position, named screenshots, slowing the game down, profiling, tracing the ROM cache's disc reads, free memory). Run it with `scripts\flycast.ps1 -Test` (`TEST=1`), and a modded one with `-Disc build/quest64-eltale-test.cdi` (`DISC=...`). A test disc prints a banner on the console and draws a red bar across the top of the screen. The scripts in `tests/`:
  - `tour.txt`: a new game, then Melrode, the Holy Plain, the Blue Cave, the monastery, Dondoran Castle, Dindom Dries and the Melrode Inn, with the spell and Start menus.
  - `battle.txt`: a random battle on the Holy Plain.
  - `gfx.txt`: bursts of screenshots while walking in the monastery, Melrode, the Blue Cave and a monastery corridor, then a battle, for depth and culling errors.
  - `save.txt` (run with `-ResetVmu`), then `load.txt`: saves day 7 at the Melrode Inn, then loads it from a fresh boot.
  - `autopress.txt`: presses Start and A every few seconds.
  - `audio.txt`: lets the opening play, waits on the title, then stays in the monastery, Melrode and a battle, for recording the music (see [Audio](#audio)).
  - `perf.txt`: profiles the opening, the title, the monastery, Melrode, the Blue Cave and a battle (see [Performance](#performance)).
  - `text.txt`: the Grand Abbott's and the innkeeper's dialogue, the Start menu's pages and a battle with a spell, for text and HUD rendering.
  - `path.txt`: the path out of the monastery, down its front steps and along the garden fence, then the Holy Plain outside Melrode's south gate, with a screenshot and a profile at each, for cutouts, fog and the frame rate there (`scripts/perf_fps.py` reads its log too).
  - `speed.txt`: walks the Holy Plain at the normal frame rate and slowed down, printing Brian's position.
  - `pace.txt`: how long each frame stays on screen in the opening, a walk round Melrode and a battle (see [Frame pacing](#performance)).
- **Emulator, with a window.** Open `build/quest64-dc.cdi` in Flycast (tested with 2.6 and 2.7, OpenGL and Vulkan, HLE and real BIOS). To see the log, set `Debug.SerialConsoleEnabled = yes` in Flycast's `emu.cfg` and start Flycast from a terminal.
- **Dreamcast with dcload-ip or dcload-serial.** From the repo directory, `dc-tool-ip -t <dreamcast ip> -c . -x build/quest64-dc.elf` (or `dc-tool-ser -c . -x ...`). The `-c .` makes the repo the `/pc/` filesystem, so the ROM is read from `quest64.us.z64` over the link. That's slow, especially over serial, so the disc is better for longer runs.
- **Dreamcast from disc.** Burn `build/quest64-dc.cdi` to an 80-minute CD-R (it fits; see [Disc size](#performance)), or put it on an optical drive emulator.

## Mods

Plain `make` and `make cdi` always build the unmodded game; its generated code and runtime are the same with or without the mod support. A modded build is a separate target, selected with `MOD`, with its own build directory (`build/mod-<mods>/`), ELF and disc, disc title, serial and VMU file:

| `MOD=` | Disc | Title | Serial (test disc) | VMU file |
| --- | --- | --- | --- | --- |
| (none) | `build/quest64-dc.cdi` | QUEST 64 | Q64-DC (Q64-DC-T) | `QUEST64.PAK` |
| `eltale` | `build/quest64-eltale.cdi` | Eltale Reborn | Q64-ELT (Q64-TELT) | `Q64ELT.PAK` |
| `eltale,gameplay` | `build/quest64-eltale-gameplay.cdi` | Eltale Reborn | Q64-ELT (Q64-TELT) | `Q64ELT.PAK` |
| `gameplay` | `build/quest64-gameplay.cdi` | QUEST 64 GP | Q64-GP (Q64-TGP) | `Q64GP.PAK` |

The gameplay mod (Quest 64: Modern Gameplay, in the same repo) saves nothing and works with any story, so with Eltale Reborn the disc stays Eltale Reborn's, saves and all, and either disc loads the other's saves. Its options include Start opening the map and the field sprint's speed, so a disc without it has neither.

The cover is `assets/cover-<first mod>.png` (e.g. `assets/cover-eltale.png`), else `assets/cover.png`, else none.

**Building.** The mods are built by their own build script on the host, where their toolchain is, and consumed from their repository; nothing of a mod is copied into this one. For Eltale Reborn (the `Quest-64-Good-Edition` repo, checked out next to this one by default):

```
scripts\mod.ps1                                       # MOD=eltale, make -j16 cdi
scripts\mod.ps1 -Mod eltale,gameplay make -j24 cdi    # with the optional gameplay mod
scripts\mod.ps1 -ModDir D:\src\Quest-64-Good-Edition make -j24 test-cdi TEST_SCRIPT=tests/mod_story.txt
```

`scripts\mod.ps1` takes the mod repo from `-ModDir`, else `MOD_DIR`, else `..\Quest-64-Good-Edition`. It runs `scripts/stage_mods.py`, which runs the mod's `python build.py story` (and `gameplay`; this needs the mod's Python requirements and zig, as for the desktop build) and copies `mod_syms.bin` and `mod_binary.bin` out of the `.nrm`, plus `mod.toml` and the link map, to `build/mods/<mod>/`, rewriting only what changed. Then it runs `make MOD=<mods>` in the toolchain container. With the mod already built, `-NoBuild` skips its build, and `scripts\dc.ps1 make MOD=eltale cdi` builds from what's staged. After a change to the mod, the same command rebuilds the disc; nothing is listed by hand.

**How it's wired in.**
- `tools/modrecomp` (built from N64Recomp's library) reads the staged mods and places each one at a fixed address from 0x80410000, after the game's 4 MB and the runtime's 64 KB. Sections are 16-byte aligned with their bss after them, and there are 0x400 bytes between mods. It applies the data relocations at build time, recompiles the code to C whose section addresses are constants, and checks every import against the runtime's.
- It also turns the mods' `RECOMP_PATCH`, `RECOMP_HOOK` and `RECOMP_HOOK_RETURN` functions into hooks in a copy of the game's recompiler config (`scripts/gen_mod_config.py`). A patch replaces the game function's body. A hook runs at its first instruction, a return hook at each of its returns (`tools/patches/n64recomp-return-hooks.patch` adds return hooks to N64Recomp). Two mods patching the same function, or a mod patching one the port stubs or replaces, fails the build.
- The game is then regenerated into `build/mod-<mods>/recomp/`. At boot, `mods_init` copies the mods' image into place. Calls through pointers into mod code go through a sorted function table, and the rest of the 2 MB mod area (`MOD_RAM_SIZE`) is the `recomp_alloc` heap.
- **Imports.**
  - Implemented (`runtime/mod/modrt.c`): `recomp_printf`, `recomp_alloc`/`recomp_free`, `recomp_get_config_u32`/`_double`/`_string`, `recomp_free_config_string`, `recomp_get_mod_version`, `recomp_is_dependency_met`, and the `recomphook_get_return_*` helpers.
  - Cross-mod imports and exports also work.
  - `recomp_printf` goes to the serial console on test discs only, at most 30 messages a second; build with `RT_DEFINES=-DMOD_PRINTF=1` to keep it on a release disc.
  - Config options take their defaults from `mod.toml`. Override them at build time with `MOD_CONFIG=eltale.time_log=On,gameplay.<option>=<value>`, where enums take a name or an index. The options are resolved when the disc is built, and an option of a type the port doesn't know fails the build. At boot every disc prints each mod's options to the serial console (`config start_on_map = 1`; enums as the index of their choice), and a mod asking for an option its mod.toml doesn't have gets 0 and one line saying so.

**Memory.** Eltale Reborn 0.9.42 takes 947 KB (0x80410000-0x804FCB00: 775 KB of code and data, 171 KB of bss). The gameplay mod takes 11 KB after it. The modded ELF is 1.3 MB larger than the unmodded one, from the recompiled mod code and the mod's image. With the 2 MB mod area, the largest block still free in Melrode drops from 5.2 MB to 2.2 MB. Eltale Reborn allocates nothing, so a smaller `MOD_RAM_SIZE` (e.g. `0x110000`) gives back most of its 1.1 MB heap.

**What a mod can use on the Dreamcast.**
- Game code it patches or hooks, its own code and data, the imports above, and display lists and textures in its own memory all work.
- RT64's extended GBI commands are ignored; the extended-RDRAM switch isn't needed, since mod memory is inside the renderer's address range.
- High-framerate interpolation tags aren't needed at 30 fps.
- Not supported yet, and the build stops with a message:
  - mod events and callbacks (`RECOMP_DECLARE_EVENT`, `RECOMP_CALLBACK`);
  - native libraries;
  - fixed-address sections;
  - imports the runtime lacks, such as `recomp_change_save_file` and the path functions.
- There is no in-game config menu; `MOD_CONFIG` stands in for it.
- The renderer has no framebuffer effects yet (reading back or drawing to the color or depth buffer), and no textures over 256 px a side.
- The SH-4 is several times slower than a PC: code that runs every frame should stay small, as the game itself already uses most of the frame in busy scenes.
- Eltale Reborn's assumptions about Quest64-Recomp hold:
  - Of the base patches it must not patch, only the Controller Pak scan (`func_80031574`) and the limb drawing (`func_8001DC78`, interpolation tags) are active there; the others are commented out. The port runs the game's own versions with its own Controller Pak.
  - Its 160-byte save block at 0x160-0x1FF round-trips (see [Saves](#saves)).

**Tests.** `tests/mod_*.txt` run on modded test discs (`make MOD=eltale test-cdi TEST_SCRIPT=...`, `scripts\flycast.ps1 <s> -Test -Disc build/quest64-eltale-test.cdi`). In them, `{symbol}` or `{symbol+offset}` stands for a mod symbol's Dreamcast address, looked up in the staged link map (`scripts/test_syms.py`). This keeps the scripts working when the mod's layout changes; for example, `{gModSave+8}` is Eltale Reborn's flag array.
- `mod_story.txt` (with `-ResetVmu`), then `mod_load.txt`:
  - a new game through the mod's logo and prologue;
  - the inn sign;
  - the village farewell and Mira on the Melrode road;
  - Odran's offer on the Holy Plain;
  - a save at the inn, then a fresh boot that loads it and checks the mod's flags.
- `mod_battle.txt`: the battle Lavaar's ring in Glencoe Forest starts.
- `mod_hounds.txt` and `mod_fx.txt` use the mod's dev mode, whose `dev_scene` config option jumps to a test scene the first time Brian is free (build with `MOD_CONFIG=eltale.dev_scene=<n>`, the option's index in its mod.toml):
  - `mod_hounds.txt` (`dev_scene=4`): the prologue's night battle with the Hell Hounds at the monastery wall;
  - `mod_fx.txt` (`dev_scene=2`): the scripted spell visuals in Melrode, a scene that loops; run it with a screenshot interval.
- `mod_sprint.txt`: Brian walks 0.6 s in Melrode without and then with X held (the test command `hold X`); pos shows about 27 units, then that times the sprint speed (34 with the gameplay mod's 1.25).
- `mod_menu.txt` (`-Mod eltale,gameplay`): Start opens the world map (the gameplay mod's `start_on_map`), and the stick still turns to the next page.
- `mod_perf.txt`: `perf.txt`'s scenes, with the Melrode walk turned away from the inn door, which the mod's town would take Brian through.
  - With Eltale Reborn 0.11.22, the title runs at 59.9 fps, and the monastery, the Blue Cave and the battle at 30, as without it.
  - The mod's own opening (its notice, logos and attract mode) runs at 56.6 fps (57.4 without the frame cadence), with the CPU two-thirds idle.
  - Melrode on that walk runs at 28.0 fps with the mod against 27.0 without it.
- `mod_pace.txt` (with `-ResetVmu`): frame pacing in the village farewell, Mira on the road and Odran's scene (see [Frame pacing](#performance)).
  - The mod's scenes move the camera from its hook on the player update, once a game frame. It moves a fixed fraction of the way to the shot each frame (`CAM_PAN`), or along an eased path over a set number of frames. Nothing there reads a clock, so the camera takes the same steps as on the N64.
  - On PC, Quest64-Recomp draws at the monitor's refresh rate by default and RT64 interpolates between the game's frames, so a pan there is smoother than any 30 fps build. At its "original" frame rate it is 30 fps like the Dreamcast.
  - The mod's `recomp_printf` logging costs nothing on release discs: the runtime returns before formatting anything.

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
| `scripts/make_gdtex.py` | Converts `assets/cover.png` to the disc's `0GDTEX.PVR` cover |
| `scripts/mod.ps1`, `scripts/stage_mods.py` | Modded builds: build the mods with their own script, stage them, run `make MOD=...` |
| `tools/modrecomp/` | Recompiles staged mods ahead of time at fixed addresses; lists their patches and hooks |
| `scripts/gen_mod_config.py` | The game's recompiler config with the mods' patches and hooks added |
| `runtime/mod/` | Mod runtime: loading, hook calls, the imports mods use, `recomp_alloc`'s heap |
| `scripts/test_syms.py` | Fills in mod symbols in test scripts for modded test discs |
| `runtime/src/test.c` | Scripted input for test discs |
| `runtime/src/profile.c` | Program counter sampler for test discs |
| `scripts/profile.py`, `scripts/perf_fps.py` | Profile by function and object file; fps per scene of `tests/perf.txt` |
| `tests/` | Test scripts |
| `scripts/flycast.ps1`, `scripts/flycast.sh` | Headless Flycast run with screenshots and, optionally, an audio recording |
| `scripts/audio_dump.py` | Loudness and clipping of an audio recording; cuts WAV clips |
| `tools/N64Recomp` | [N64Recomp-DC](https://github.com/billymonks/N64Recomp-DC) (submodule, `main`) |
| `tools/patches/` | `gpr_32bit` and return-hook patches; `scripts/build_n64recomp.sh` applies any that are not already in the submodule |
| `Quest64Syms` | Function and data symbols of the ROM (submodule) |
| `docker/Dockerfile` | Toolchain image |
| `docker/flycast/Dockerfile` | Headless Flycast image (downloads the official Flycast release; ImageMagick for screenshots) |

## License

GPL-3.0, like Quest64-Recomp. See [COPYING](COPYING).
