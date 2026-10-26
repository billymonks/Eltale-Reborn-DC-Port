# Notes

Numbers are from Flycast 2.7 unless a line says otherwise. Flycast's SH-4 timing is not cycle-accurate, and it does not model the caches, so a real Dreamcast can come out slower. Hardware has not been timed.

## Speed

Quest 64 advances one fixed step per frame. The field loop waits until two retraces have passed, then builds the next frame, so 30 fps is the speed the game was made for. Below that it is slow motion. The opening cutscene is 60. `tests/speed.txt` walks the Holy Plain normally and then with `burn 50`. Brian covers the same distance per frame either way.

fps with audio on, `tests/perf.txt` (10 s a scene):

| Scene | fps |
| --- | --- |
| Opening | 59.0 |
| Title | 59.9 |
| Monastery hall | 30.0 |
| Melrode | 29.7 |
| Blue Cave | 30.0 |
| Battle, Holy Plain | 29.9 |

Melrode's missing frames are the house door (map 0x0F) about 3 s into that walk: 297 frames of 300, and 300 on the same street without the door. `tests/path.txt` holds 30.0 on the monastery steps and along the garden fence, and 29.3 on the Holy Plain outside Melrode. Those windows do not read the disc. On the plain the renderer is about 47% of the CPU, audio 22%, the game's code 15%, idle 12%.

A battle is about 40% renderer (`gfx_run_task` 16%, textures 11%, combiner 2%), 21% audio, 18% the game (13% recompiled, 5% the floor test in `collision.c`), 17% idle. Audio in the field and in battles is 18-21% of the CPU. Moving voices onto the AICA would drop most of that: ADPCM samples decoded into AICA RAM, envelopes and reverb remapped. The scenes above already hold 30 without it.

On the Holy Plain, `tests/path.txt`, about 1,900 triangles a frame and about 40% of them are off-screen after the transform. Lighting a vertex only when a triangle uses it would save well under a millisecond. About 200 triangles a frame are fully fogged, in the fog color, over a sky of another color. Skipping them would change the picture. Display lists that `G_CULLDL` rejects are skipped, including ones behind the near plane (about 4 a frame out there).

Changes that moved the numbers, in the order they landed: graphics tasks on their own thread below the game threads (they used to run inside the scheduler and burn a frame of idle); the once-a-second log on its own thread, test discs only; `restrict` on `rdram` and `ctx` in the generated functions; `fsqrt` instead of the soft sqrt KOS was emitting under `-fno-builtin`; tighter audio loops; the texture cache and combiner; the ROM reader below. Opaque, punch-through, and translucent lists, and fog, did not change CPU time. `-Os` on the generated code is about 2.5% smaller and a bit slower in Flycast. Worth retrying on hardware.

`scripts/perf_fps.py build/perf.log` prints fps per scene, `scripts/profile.py build/perf.log` the time per object file and function. `RT_DEFINES=-DAUDIO_OFF` skips the audio commands. `RT_DEFINES="-DROM_SYNC -DROM_NO_PIN -DLRU_BLOCKS=48 -DREAD_RUN=1"` is the old ROM reader (game thread, audio not pinned, 1.5 MB cache, one block per read). `make clean` after changing `RT_DEFINES`.

Eltale Reborn 0.11.22 on `tests/mod_perf.txt` (the Melrode walk turned away from the inn door): title 59.9, monastery, Blue Cave, and the battle at 30, the mod's own opening 56.6, that Melrode walk 28.0 against 27.0 without the mod.

## Frame pacing

An fps counter can read 30 while frames sit up for 1, then 3, then 1 vblank. KallistiOS starts the PowerVR as soon as the last list is in and shows the frame on the next vblank after the render finishes, so a frame that finished just after a vblank waited an extra one. The game's tasks were already every 2 vblanks.

`present_wait` in `events.c` holds the finished frame and hands it to the PowerVR just after a vblank, at least one game frame after the previous hand-off. The render is about 8 ms in the heavy scenes, so it fits in that interval and each frame stays up for one period. That adds up to one vblank of latency. The period is 1 vblank during the opening and 2 everywhere else, taken from how the game behaves. Nothing in the game stores it. At 2, a 60 fps stretch already has its next task waiting at every hand-off. At 1, the gaps between tasks show 30 fps.

`tests/pace.txt` and `tests/mod_pace.txt`. `pacing 0` turns the cadence off. Frames that stayed up for exactly the game's period, after the change:

| Scene | Frames |
| --- | --- |
| Eltale, village farewell and Mira on the road | 1184 of 1221 |
| Eltale, Odran on the Holy Plain | 810 of 813 |
| Battle camera | 267 of 269 |
| Opening (60 fps) | 1169 of 1174 |
| Melrode, walking, camera turning | 141 of 226 |

Before this, the camera moves were on the right count about half the time, mostly 1 and 3 vblanks (`312312` in a pan). What is left over 2 vblanks is the game taking longer than 2 vblanks to build the frame. Melrode's side streets are about 25 fps either way. The farewell has a few seconds of that among the villagers. The fps table above did not change.

Eltale moves the camera from its player-update hook, once a game frame, a fixed fraction toward the shot (`CAM_PAN`) or along a path of a set length. It does not read a clock. On PC, Quest 64: Recompiled draws at the monitor rate and RT64 interpolates, so a pan there is smoother than 30 fps. At that project's original frame rate it is 30, same as here.

## ROM reads

RDRAM is 4 MB. The ROM stays on the disc. PI DMA goes through 32 KB blocks (`rom.c`): a cached read is copied immediately, anything else is finished by a reader thread below the game threads, and the DMA message goes out when the bytes are in RDRAM. Up to four blocks share one disc command. The 1.1 MB of audio banks, samples, and sequences is read at boot and kept, because the audio thread fires sample DMAs every frame and does not wait. The rest is a 3 MB LRU. In a room, play hits the cache. A map load in Flycast is a handful of reads, 60-150 ms each. The pinned audio is one 1.1 MB read, about 0.6 s in Flycast.

Flycast's GD-ROM is roughly 2 MB/s (20-30 ms for one block, 60-80 ms for four). A CD-R seek that is not next to the last read is on the order of 100 ms or more, so the count of reads matters more than the byte count. With audio pinned, a room does not touch the disc, and a slow seek shows up as a longer map load.

With audio pinned, the game's heap and the cache leave about 5.6 MB free.

## Audio

The synthesizer's voices play on the AICA's own channels (`aica.c`). A hook in libultra's `alAudioFrame` (`func_80036A68`, before `0x80036C20`), after the sequence and sound players' handlers have run, takes the parameter updates they posted on each of the 16 physical voices and sends them to the AICA, then leaves the RSP command list empty. Voice allocation and stealing, the update pool and both players are still the game's code. Each voice is a pair of channels, one panned hard left and one hard right, so libultra's equal-power pan and its separate left and right ramps carry over.

The 75 samples (385 KB of VADPCM) are decoded at boot to 1.3 MB of 16-bit PCM in sound RAM, about 0.3 s. The stored loop states equal what the decode reaches on its first pass, so the AICA's own loop plays what the N64 did. A sample's first frame after the loop end is overwritten with the loop's start for the AICA's interpolation.

Levels follow `env.c`: the voice volume squared, `eqpower` for pan and the dry level, ramps that multiply the gain by a constant (straight lines in dB). Pitch is the resampler's ratio, clamped and quantized as `alResamplePull` does, times 22047 Hz. No voice in the game sends anything to the reverb: the sound player's FX mix stays 0, and the sequences only ever set controller 91 to 0. Logged across `tests/audio.txt` and `tests/tour.txt` with the SH-4 mixer: no nonzero wet send. So there is no reverb.

Timing: `osAiGetLength` answers from a virtual AI on the SH-4's clock, sized so the synthesizer's output ends a lead ahead of real time after the next frame, and every update goes to the AICA stamped with its time on that line. The ARM applies it then, however late and bunched up the game's audio thread runs. The lead starts at 60 ms. A frame that would play late slips the time line forward instead (an N64 AI underrun) and raises the lead by 20 ms, up to 160 ms; ten seconds without a slip lower it again. A thread held up for longer than 120 ms, which is a map or battle load, slips without raising it. After a load the game's audio thread catches up in a burst of frames of at least 352 samples each, so the lead runs high for a couple of seconds afterwards.

The ARM runs KallistiOS's stock firmware: channel commands in its queue, on its 4410 Hz clock (measured at boot; 0.8% slow in Flycast). It has no ramps, so a ramp is sent as a volume step every 256 synthesizer samples. `RT_DEFINES=-DAICA_FW` uses the port's own firmware (`runtime/aica/`, 2 KB): events on the AICA's sample clock (a channel looping silence, read through the monitor), ramps walked on the ARM in TL steps every 32 samples, one event per ramp. Building it needs the ARM compiler: `docker build -t q64dc-toolchain-arm docker/arm`; `scripts\dc.ps1` uses that image when it exists, otherwise the committed `runtime/aica/aica_fw.bin`.

The perf HUD's third line is `AIC <us per audio frame> V<voices> D<lead ms> S<slips>`, and `A` on the second line is the audio milliseconds per game frame.

`RT_DEFINES=-DAUDIO_HLE` builds the old path below. A disc also falls back to it when the AICA doesn't answer within 200 ms at boot.

`aspMain` is interpreted in C (`audio_hle.c`) and the mix is streamed to the AICA (`audio.c`). Recompiling the RSP microcode, as the PC port does, is too heavy for the SH-4.

The 16 commands the game actually sends are implemented. Output was checked bit for bit against Quest 64: Recompiled's recompiled microcode. One known difference: the reverb pole filter's first call starts from zero history, where the microcode reads two leftover scratch words. The resampler table is taken from the game's microcode in RDRAM. Semantics came from the public ABI notes and were cross-checked with mupen64plus-rsp-hle (GPL-2.0-or-later).

The game asks for 22050 Hz. `osAiSetFrequency` rounds that the way libultra does, to 22047, and a KallistiOS stream runs at that rate. Samples sit in an 8192-frame ring. `osAiGetLength` is faked so the game's own size formula asks for a retrace of samples plus a quarter of the ring's distance from a 2048-frame target. Latency stays around 140 ms. A timed model of the AI starves the audio thread, which only runs when some other thread hits a runtime call.

Reverb is about 0.9 ms of a frame. The rest is per voice, mostly the envelope mixer, then the resampler and ADPCM. `RT_DEFINES=-DAUDIO_PROFILE` adds per-command times to the log.

To compare against the PC recomp, with a Quest 64: Recompiled checkout:

```
docker run --rm -v "${PWD}:/src" -v "<Quest64-Recomp>:/qr:ro" -w /src q64dc-toolchain bash tools/audio_test/run.sh /qr 50
```

To listen: `scripts\flycast.ps1 175 -Test -AudioDump build/audio/audio.raw`, then `scripts/audio_dump.py build/audio/audio.raw --clip <name> <start> <seconds>`.

## Graphics

F3DEX 1.23, interpreted on the SH-4, drawn with the PVR API. No GL. `gfx.c`, `gfx_tex.c`, `gfx_cc.c`. The file headers cover the lists, fog, and TMEM.

Vertices use `ftrv` (`mat_trans_nodiv`). Triangles that cross the near plane are culled on the sign of their clip-space determinant before clipping, so a wall the camera backs into stays hidden. 320x240 is scaled to 640x480.

Decals (`ZMODE_DEC`: carpets, windows, shadows) are pushed toward the camera by the depth slope of two pixels plus 1/4096 of their depth.

The combiner is reduced to texture × color + offset, which the PVR does, once per combiner and set of constants rather than once per vertex.

The PVR has one fog color per frame. The first fog color a frame uses gets the register. Other fog colors, and untextured primitives, mix fog on the CPU.

Text, menus, and most of the HUD are 1:1 texels drawn from a whole texel. Those are point sampled even when the game asks for bilinear. The RDP returns a single texel there. The PVR's filter at 640x480 mixes in the font's transparent texels, which are pink.

The floor probe (`func_8000F238` and `func_8000F5A4`) is native C in `collision.c`, same steps in the same order, hooked from `config/quest64.us.toml`. It was 16% of a battle and is about 5%. `-DFLOOR_VERIFY` plus the hooks described in that file runs both and compares them. 131,072 calls in `tests/perf.txt` matched.

There is no framebuffer readback, and no texture over 256 px on a side. RT64's extended GBI commands are ignored.

## Registers and threads

Quest 64 does not use the upper half of a GPR. The 64-bit libultra helpers are already runtime calls, and the recompiler is patched (`tools/patches/n64recomp-gpr32.patch`, `gpr_32bit` in the toml) to reject a 64-bit GPR op, emit 32-bit `hi`/`lo`, and call `DIV32`/`DIVU32`. `runtime/include/recomp.h` types `gpr` as `uint32_t`. RDRAM words are host-endian, same as the PC runtime. SH-4 is little-endian.

One KallistiOS thread per N64 thread, and only one of them runs game code at a time. A thread yields when it blocks on a message queue. `events.c` raises the VI retrace on vblank and fires `osSetTimer`. Graphics tasks run on the "n64 rsp" thread. Audio tasks run in the interpreter immediately. SP and DP done are sent when the task finishes.

`tools/patches/` is applied by `scripts/build_n64recomp.sh` when the submodule does not already have the change. The return-hook patch is there for mods.

## Mods

The Dreamcast does not recompile an `.nrm` at runtime. `scripts/stage_mods.py` unpacks the packages in `eltale/` and copies `mod_syms.bin`, `mod_binary.bin`, `mod.toml`, and the link map into `build/mods/<mod>/`. `tools/modrecomp` then places each mod at a fixed address from `0x80410000` (after the game's 4 MB and 64 KB of runtime), 16-byte aligned, bss after the section, 0x400 bytes between mods. Data relocations are applied at build time. `scripts/gen_mod_config.py` turns `RECOMP_PATCH`, `RECOMP_HOOK`, and `RECOMP_HOOK_RETURN` into hooks in a copy of `config/quest64.us.toml`. Two mods patching one function, or a mod patching a function this port stubs, fails the build.

At boot, `mods_init` copies the image into place. Indirect calls go through a sorted table. The rest of the 2 MB mod area (`MOD_RAM_SIZE`) is the `recomp_alloc` heap.

Imports that exist (`runtime/mod/modrt.c`): `recomp_printf`, alloc/free, the config getters, `recomp_get_mod_version`, `recomp_is_dependency_met`, the `recomphook_get_return_*` helpers. Cross-mod imports and exports work. `recomp_printf` is off on a release disc, and capped at 30 lines a second on a test disc. `RT_DEFINES=-DMOD_PRINTF=1` keeps it on a release disc. Config defaults come from `mod.toml`. Override at build time with `MOD_CONFIG=eltale.time_log=On,gameplay.sprint_speed=1.25` (an enum's name or its index). An unknown type fails the build. A missing key reads as 0 and prints one line.

Eltale Reborn 0.9.42 was 947 KB at `0x80410000`-`0x804FCB00` (775 KB code and data, 171 KB bss). The gameplay mod was 11 KB after it. The modded ELF was 1.3 MB larger. With the 2 MB area, the largest free block in Melrode dropped from 5.2 MB to 2.2 MB. Eltale allocates nothing, so a smaller `MOD_RAM_SIZE` (for example `0x110000`) gives most of that heap back.

Not supported, and the build stops if it sees them:

- `RECOMP_DECLARE_EVENT` and `RECOMP_CALLBACK`
- native libraries
- fixed-address sections
- imports the runtime does not have (`recomp_change_save_file`, the path functions)

No in-game config menu. `MOD_CONFIG` is the stand-in. Per-frame mod code has to stay small. Busy scenes already fill the frame.

`MOD=gameplay` alone is `build/quest64-gameplay.cdi`, serial `Q64-GP`, VMU `Q64GP.PAK`. It saves nothing of its own. Next to Eltale it leaves the disc title, serial, and save file as Eltale's.

## Tests

`make cdi` is the disc you play. `make test-cdi TEST_SCRIPT=tests/<name>.txt` writes `build/quest64-dc-test.cdi` (a modded build writes `build/eltale-reborn-test.cdi`). `runtime/src/test.c` lists the commands. In a mod script, `{symbol}` or `{symbol+offset}` is filled from the staged link map by `scripts/test_syms.py`. `{gModSave+8}` is Eltale's flag array.

Headless Flycast, once `docker build -t q64dc-flycast docker/flycast`:

```
scripts\dc.ps1 make cdi
scripts\flycast.ps1 90 5
scripts\flycast.ps1 240 -Test
scripts\flycast.ps1 600 -Test -ResetVmu -Disc build/eltale-reborn-test.cdi
```

Screenshots land in `build/screens`. VMU images stick around in `build/flycast-home`. `-ResetVmu` blanks them. `-Renderer n` is Flycast's `pvr.rend`. A 2.6 image: `docker build --build-arg FLYCAST_VERSION=2.6 --build-arg FLYCAST_APPIMAGE=flycast-x86_64.AppImage -t q64dc-flycast26 docker/flycast`.

Each script's first comment says what it walks through. `tests/mod_hounds.txt` and `tests/mod_fx.txt` need Eltale's dev scene: `MOD_CONFIG=eltale.dev_scene=<n>`, the index from its `mod.toml` (4 for the hounds, 2 for the spell visuals).
