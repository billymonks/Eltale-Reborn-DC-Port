# Performance

How to measure the port on a Dreamcast, what the measurements showed at the heaviest spot found so far, and what could still be tried. Numbers are from a real Dreamcast unless a line says Flycast. Flycast does not model the SH-4's caches, and its timings were consistently far too optimistic, about 6x on the renderer. Only the hardware numbers count. `NOTES.md` has the Flycast figures.

## Perf and diag discs

A perf disc is a normal disc built with `RT_DEFINES=-DPERF_HUD`: the HUD on the left of the screen, the profiler, and frame skip. `RT_DEFINES="-DPERF_HUD -DGFX_DIAG"` makes a diag disc, which adds four renderer lines to the HUD, the renderer's run-time configurations and the diagnostic modes. The runtime objects rebuild when `RT_DEFINES` changes.

Both pack the profiler's tables, `profaddr.bin` and `profname.bin`, onto the disc. `scripts/prof_syms.py` makes them from the link map. `make test-cdi` leaves them off, so the profiler on a test disc shows `NO SYM`.

`make cdi` writes `build/quest64-dc.cdi`, and `MOD=eltale` writes `build/eltale-reborn.cdi`. The recent perf discs were built in directories of their own (`BUILD=`) by local scripts such as `build/gfxdiag/build.sh` and `build/night2/elt.sh`, so the normal discs stayed as they were. The current ones are `build/quest64-dc-night2.cdi` and `build/eltale-reborn-night2-perf.cdi`. Both are diag discs that start in C3, with their ELF next to them.

## The HUD

```
F<fps> T<tasks/s>
G<ms> IC<ms> R<ms> A<ms>
AIC <us> V<voices> D<ms> S<slips>
SK0 I<%> O<%>
RND <ms> REG <ms>
OP<tris> PT<tris>
TR<tris> V<KB>
X PROF
C<set> <letters> RO<ms> RI<ms>
DL<ms> TX<ms> VX<ms> TRI<ms> Q<ms>
TL<n> CS<KB> DE<n> UP<KB> LK<n>
TA<KB> DM<KB> ND<n> NC<n> CM<n>
```

The first eight lines are on every perf disc, the last four only on diag discs. Per-frame values are averages over the half second between refreshes. F and T are averaged over two seconds.

F is frames drawn per second and T graphics tasks per second. They differ when frame skip leaves frames out.

G, R and A are milliseconds per frame spent on the game's threads, on the renderer (the `n64 rsp` thread, which runs the graphics tasks) and on audio. They are whole milliseconds, so a change in G of less than 1 ms doesn't show. IC is the game threads' instruction-cache stall time, from the SH-4's performance counters.

With AICA audio the third line is `AIC`: microseconds per audio frame in the hook that hands the voices to the AICA, voices sounding (V), the lead in milliseconds (D) and time-line slips since boot (S). `NOTES.md` explains the lead and the slips. An `AUDIO_HLE` build shows its audio mode there instead (EX, FST, LIN or NRV), with its microseconds per task and its fast-path, linear and dry counts.

The fourth line is frame skip: SK0 is off, SK1 skips every other frame and SKA skips a frame when the previous one took longer than two vblanks. I is the idle thread's share of the CPU and O the runtime's other threads', in percent.

RND and REG are the PowerVR's render time and the time it took to take in the last frame's lists, in milliseconds, from KallistiOS's `pvr_get_stats`. OP, PT and TR count the triangles sent to the opaque, punch-through and translucent lists. V is the PowerVR vertex buffer the frame used, in KB. The eighth line is the profiler's state.

On a diag disc, the ninth line is the configuration (`C<set>`) and one letter per renderer optimization, a dot where it is off. RO and RI are the renderer's operand-cache and instruction-cache stall times per frame, from the performance counters with the game threads' share taken out. With a diagnostic mode on, `M<mode>` takes RO's place.

DL is the time in the display-list walk. It includes TX (texture loads, checksums, lookups, decodes and uploads), VX (vertex loads: transform and projection), TRI (triangle commands: culling, lighting and colors, headers and the PowerVR's vertices) and Q (the second pass of the Q toggle, 0 when it is off).

TL is TMEM loads per frame, CS the KB of RDRAM checksummed to notice changed textures, DE textures decoded, UP the KB uploaded to VRAM and LK texture lookups. TA is the KB written to the tile accelerator (the opaque list, through the store queues), DM the KB sent by DMA (the punch-through and translucent lists), ND triangles drawn, NC triangle commands that drew nothing (off screen or back facing) and CM display-list commands.

## Chords

Hold both triggers and press:

- X: start the profiler; once it shows its first page, the next page.
- A: frame skip, SK0, SK1 and SKA in turn.
- B (diag discs): the next renderer configuration.
- D-pad down (diag discs): the next diagnostic mode.
- Y (`AUDIO_HLE` builds): the next audio mode.

The game doesn't see the chord's buttons. `runtime/src/cont.c` handles them.

## Renderer configurations

Each letter is one optimization of the renderer, described in a line each as `OPT_*` in `runtime/src/gfx_internal.h`: T S L V C D B R K P W U Q M N E O G F. All of them are exact: every configuration draws the same picture. The B chord steps through these sets (`opt_sets` in `runtime/src/gfx.c`):

- C0: all but B.
- C1: none.
- C2: the release disc's set before M, N, E, O, G and F joined it, T S L V C D R K P W U.
- C3: C0 without Q. Diag discs start here. It is the fastest on hardware, and the release disc's set (`OPT_RELEASE`).
- C4 to C10: C0 without M, U, N, E, O, G and F, one each.

## Diagnostic modes

Each mode leaves part of the renderer's work out, so the difference against M0 at the same spot, with the camera still, is roughly what that part costs. The picture is wrong in every mode but M0. The modes turn Q off.

- M0: normal.
- M1: textures decoded but not uploaded to VRAM.
- M2: no texture work at all; one fixed texture.
- M3: texture checksums kept from the first frame that read them.
- M4: the opaque list written to RAM instead of the tile accelerator.
- M5: the punch-through and translucent lists built but not sent.
- M6: no lighting, fog or combiner math; one vertex color.
- M7: triangle commands skipped.

## Profiler

The X chord samples the program counter about 1,000 times a second for 10 s (the eighth line counts down), then shows the busiest functions with their share of the samples (FUNC). The next X shows the share by group (GROUP: renderer, audio, game, collision, ROM, runtime, mods, KallistiOS, libc, libm, libgcc). On a diag disc a third page (PC) lists the busiest 32-byte stretches of code by address, and the same list goes out on the serial port.

`sh-elf-addr2line -f -i` on the disc's ELF names the PC page's addresses. `build/eltale-night2-perf/sym.sh` does it for the Eltale perf disc, `ELF=build/quest64-dc-night2.elf` for the Quest 64 one:

```
scripts\dc.ps1 bash build/eltale-night2-perf/sym.sh 8C0103A0 8C011200
```

In the HUD's font, B and 8 differ by one pixel, and so do D and 0.

In a test script, `profile_start` and `profile_dump` print the samples to the log, and `scripts/profile.py` maps them to functions. `tests/perf.txt` and `tests/hot.txt` profile fixed scenes that way.

## Comparing builds

Test discs built with `RT_DEFINES="-DLOCKSTEP -DROM_SYNC"` run the game in virtual time. Messages from the runtime's own threads reach the game at the same points whatever its code's speed (`runtime/src/ultra.h`), so two builds get the same input on the same game frames. In a script, `hash <label>` prints a hash of each 64 KB block of RDRAM, `hashmask` hashes a range as zeros and `dump` prints words. `scripts/compare_runs.py` compares two runs block by block.

The code generation checks were `tests/cg_ab.txt` (a new game, the monastery, Melrode, a seeded battle and a save at the inn) and `tests/cg_tour.txt`. `tests/cg_ab_s.txt`, `tests/cg_tour_s.txt` and `tests/cg_diag.txt` mask or dump the N64 thread stacks. `tests/cg_menu.txt` replays the title menu with nothing masked.

In LOCKSTEP builds `shot` holds the game until the frame is drawn and taken, so two builds' screenshots compare pixel for pixel. The renderer's changes were checked against the release renderer that way on `tests/gfx.txt` (49 shots) and `tests/text.txt` (13), and in the later rounds also on `tests/tour.txt` (14) and Eltale's `tests/mod_story.txt` (28). `build/night/imgdiff.py` counts the pixels that differ outside the HUD box.

Test builds with `-DGFX_HASH` print a hash of everything sent to the PowerVR each frame, and `build/night/fhcmp.py` compares two logs. `-DGFX_XCHECK` builds run each fast path next to the original and count the differences. Both scripts live under `build/`, which is not in the repository.

## Hardware results

The heaviest spot found is in Melrode, near the kids playing and the women talking. An earlier spot near the window was measured first.

At the start the heavy spot ran at about 15 fps, with audio about 24% of the CPU, the renderer about 50% and the game about 30%. Moving the voices onto the AICA took audio to about 0 and Melrode to about 20 fps.

Near the window, on the gfxdiag disc in M0 and then on the first optimized disc (gfxopt):

| Disc | F | R | DL | TX | VX | TRI | RO | RI |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| gfxdiag, M0 | 19.9 | 36 | 35.4 | 8.9 | 7.0 | 13.0 | 5.7 | 8.0 |
| gfxopt, C0 | 23.9 | 31 | | | | | | |

The diagnostic modes at that spot, against M0:

| Left out | Saved (ms) |
| --- | --- |
| Tile accelerator writes (M4), DMA (M5) | about 0 |
| Lighting, fog and combiner math (M6) | about 5 |
| Textures (M2) | about 4 |
| Texture checksums (M3) | about 2 |

With no triangles at all (M7), R was 20 and the frame rate 29.9.

Near the kids and the women:

| Disc | F | G | IC | R | DL | TX | VX | TRI | Q | RO | RI |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| gfxopt2, C0 | 19.9 | 8 | 1.8 | 34 | 33.5 | 7.0 | 5.9 | 15.6 | | 5.2 | 9.9 |
| gfxopt2, C1 (all off) | | | | 42 | 41.5 | | | | | | |
| night, C3 | 21.7 | 8 | | 32 | 32.3 | 6.4 | 6.4 | 14.0 | | 4.3 | 9.0 |
| night, C0 (Q on) | | | | | 33.7 | | | 8.6 | 7.1 | 6.5 | 7.3 |

C3 is the best exact configuration. It is now the default on diag discs and the release discs' set. With Q on, the triangle commands drop from 14.0 to 8.6 ms and RI falls, but the second pass takes 7.1 ms and RO rises. Q trades instruction-cache stalls for data-cache stalls and loses.

30 fps leaves about 33 ms a frame. At this spot the renderer and the game need roughly R32 + G8 = 40 ms, so it runs at F21.7, which is still better than the N64 there. Most other areas hold 30 fps. Holy Plain battles are mostly 30 with small dips.

## What each change was worth

The renderer's changes on hardware, from the gfxopt, gfxopt2 and night discs:

| Change | Saved (ms) |
| --- | --- |
| Hot layout | about 1.7 |
| L, TMEM load queue | about 1.9 |
| V, vertex loads | about 1.1 |
| C, lighting and colors | about 1.0 |
| P, combiner results reused | about 0.8 |
| W, tile load bounds | about 0.6 |
| K, sampled texture checks | about 0.5 |
| R, triangle rejects | about 0.2 |
| T, S, D, U, B | about 0 |

The hot layout puts the busiest functions first in `.text`, in an order that keeps them from evicting each other in the 8 KB instruction cache (`runtime/hot.txt`, `scripts/mkhot.py`, the Makefile's `HOT_LD`).

The game's code comes from this port's fork of N64Recomp (N64Recomp-DC, the `tools/N64Recomp` submodule). Each step is a switch under `[codegen]` in `config/quest64.us.toml`:

- M1, the memory macros: each function reads RDRAM through one biased base pointer (`rdram_base`), and sp-relative halfword and byte accesses fold their address swizzle into the offset (`sp_subword_fold`).
- M2 and M2b: GPRs as C locals instead of context fields (`regcache = "gpr"`).
- M3: FPRs as locals too (`regcache = "gpr+fpr"`).
- FPR ABI sync: FPRs written back at calls and returns only where the other side reads them (`fpr_abi_sync`).
- M5: a branch's delay slot emitted once (`delay_once`).
- M6: return registers written back only when a caller reads them (`ret_trim`), and a small cache in front of `get_function` for indirect calls (`lookup_cache`).
- Small leaf functions that callers may inline (`inline_leaves`).
- The game's busiest functions in the hot layout.

Together they made the game's code about 16 to 23% faster in Flycast's PC samples. On hardware, G read 8 before and after, and it has a resolution of 1 ms.

What didn't work or was dropped:

- A per-slot cache of packed vertices plus triangle strips (the big4 disc, TRI2) looked fine in Flycast, broke on hardware, and was no faster.
- The two-phase restructure (Q) is exact but slower on hardware (above).
- Batch culling (B) gained nothing.
- M4, `skip_saves`, was dropped. It stopped storing callee-saved registers to their N64 stack slots. The game reads uninitialized stack locals whose N64 contents are registers that a sibling call saved there. Without the stores one of those reads saw a stale value, and a menu sound kept chirping (the comment at `skip_saves` in the config). A static scan found 14 more such reads in 6 functions.
- Native `sinf` and `cosf` (`runtime/src/sincos.c`, hooked from the config) are exact and still in, but worth next to nothing.
- Approximate lighting and fog were not built. The diagnostic modes put their ceiling at about 3.7 ms: M6 in Flycast at the heaviest spot. On hardware, M6 saved about 5 ms near the window. An approximation keeps part of that work.

## Problems only hardware showed

- Unaligned reads: a 32-bit load from a 2-aligned address is an address error on the SH-4. `rd32` in `runtime/src/gfx.c` reads those a byte at a time, and the texture decode buffer (`decode_buf` in `runtime/src/gfx_tex.c`) is 32-byte aligned.
- The crash handler called `pvr_shutdown` and `printf` inside the exception. Both take a mutex: `mutex_lock: called inside an interrupt`. It writes through dbgio only now (`runtime/src/screen_log.c`).
- Punch-through polygons drew their transparent texels solid. Real PVR2 applies the blend mode in the punch-through list and Flycast does not. That list uses SRCALPHA/INVSRCALPHA.
- Tiles flickered: a town wall with a door and a window overflowed a 32-pointer tile bin, and with no overflow sets the tile accelerator wrote into the next tile. `ensure_pvr` in `runtime/src/gfx.c` sets the bin sizes and `opb_overflow_count`. Flycast does not enforce the bins.
- Textures ran out of VRAM. The texture cache is a pool with least-recently-used eviction (`runtime/src/gfx_tex.c`), and the vertex buffers are 512 KB (`VERTEX_BUF_SIZE`).
- The camera jumped. KallistiOS starts the threads it creates with FPSCR.DN clear, so a denormal raised the FPU error exception, and KallistiOS's handler skips the instruction. `runtime/src/fpu.c` sets DN in every thread the runtime starts.
- The floor test rounded differently: GCC fused its products and sums into FMAC, which rounds once where the N64 rounds each. `runtime/src/collision.c` turns the contraction off.
- The pixel-font HUD, a quad for each run of lit pixels, cost about 55 KB of DMA a frame on perf builds. Diag discs draw the HUD from a font texture, one quad per character.

## What could still be tried

The figures for the heaviest spot under C3 bound most of these: DL 32.3 ms, of it TX 6.4, VX 6.4 and TRI 14.0; RO 4.3 and RI 9.0; G 8.

### Static geometry caching

Keep the decoded state and the model-space geometry of each display list that doesn't change, so a frame only does its matrix, cull and submit. This targets the display-list interpretation and the texture-state work, and has the largest potential of anything here. It is weeks of work. The risk is invalidation: knowing for certain that a list, its vertices, its textures and the state it inherits haven't changed.

### Exact display-list culling

84 display-list calls a frame draw nothing but cost about 38% of the vertex work. Skipping them exactly needs a bound for each call and a record of the state changes it makes, so a skipped call leaves the state as running it would have. This targets VX and the triangle commands those calls send. It is days to two weeks of work. The risks are a bound that is too small, which drops geometry, and a missed state change, which shows wrong colors or textures later in the frame.

### The character path

Characters load a matrix for each limb, send their lights again for each, and change materials and combiners between characters. Caching each limb's lit colors while its matrix and the lights stay the same, and keeping combiner setups across characters, targets the lighting and color part of TRI and the state work. This is the most relevant item for Eltale's extra NPCs. Animated limbs change their matrices every frame, so only characters standing still hit such a cache. It is about a week of work. The risk is a cache key that misses an input, such as a light color or a material, which gives a wrong shade.

### Texture change detection

The checksums exist because the game can change texture data in RDRAM. Tracking where PI DMA writes would let a texture whose source wasn't written skip its sum. M3 put all of the checksum work at about 2 ms near the window, and K already took about 0.5 ms of it. It is days of work. The risk is a write that isn't tracked, which leaves a stale texture.

### A better phase split

Q showed that walking the lists and emitting the triangles in separate passes cuts RI, but writing its records and reading them back raised RO by more. Streaming compact records with prefetch, or splitting per display list so the records stay in the cache, could keep the RI gain without that cost. This targets RI. It is weeks of work, and like Q it can come out slower; only hardware tells.

### Approximations, off by default

Simpler lighting and fog on distant characters, or a cheaper combiner, as options. The ceiling is about 3.7 ms by the diagnostic modes. They are days of work. The picture changes, which is why they would stay off by default.

### Frame skip on the release disc

The perf discs have it. The game advances one step per frame, so below 30 fps it runs slow. SKA leaves out drawing a frame after one that took too long, so a heavy spot keeps the game's speed and shows fewer frames. It is a small change, a build option or a chord on the release disc. The risk is uneven motion where it kicks in.

### Game code

M4 only for functions proven not to read stale stack, native replacements only where clearly worth it (sin and cos weren't), and the hot layout refreshed from hardware PC samples instead of Flycast's. G is 8 ms at the heaviest spot, so the ceiling is small. The risk for M4 is another stale-stack read like the menu sound.

### Audio

The port's own AICA firmware (`RT_DEFINES=-DAICA_FW`, `runtime/aica/`, already built as an opt-in disc) walks volume ramps on the ARM, for smoother fades. About 1 MB of audio data is still pinned in main RAM and could be freed. Neither changes the frame time, since audio is about 0 of the CPU now.

## Eltale Reborn

Eltale Reborn adds NPCs to every area, and the NPC-heavy spot is the worst one found. Near the window about 675 triangles were drawn at R31 (gfxopt, C0). Near the kids and the women about 797 were drawn at R34 (gfxopt2, C0), on the faster renderer. A character costs more than its drawn triangles. It loads a matrix for each limb, its lights are sent again for each, its material and combiner change, and the parts that face away or are off screen are still transformed and culled.

Eltale's build generates the game's code without the register caching and the switches after it: `scripts/gen_mod_config.py` turns them off for modded builds, which stay on the plain output. Its G is the unoptimized game's.

`build/eltale-reborn-night2-perf.cdi` is Eltale with the perf HUD and the renderer's diagnostics, starting in C3. Its ELF, map and profiler tables are next to it (`build/eltale-reborn-night2-perf.elf`), and `build/eltale-night2-perf/sym.sh` names its PC page's addresses. At a crowded spot, with the camera still, read F, G, IC, R, DL, TX, VX, TRI, RO and RI in C3. C3 is the release disc's renderer, C2 the earlier release's and C1 the unoptimized one. M6 and M7 show what lighting and color and the triangles cost there, and the X chord's GROUP page splits the time between the renderer, the game, the mods and the rest.

The character path would help most, then exact display-list culling for characters out of view. Static geometry caching would help too: it takes time off the scenery, which leaves more for the characters. The approximations and frame skip are the fallbacks if those aren't enough.
