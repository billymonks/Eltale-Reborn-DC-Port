// F3DEX 1.23 display lists drawn with the PowerVR.
#ifndef Q64DC_GFX_H
#define Q64DC_GFX_H

#include <stdint.h>

void gfx_init(void);
// Runs a graphics task's display list. Returns once the game's memory is no longer needed, so the
// game can be told the task is done; the frame isn't rendered until gfx_submit.
void gfx_run_task(uint32_t task);
void gfx_submit(void);
void gfx_print_stats(void);
#ifdef GFX_DIAG
// Both triggers and B: the next set of the renderer's optimizations, from the next frame on.
void gfx_diag_cycle(void);
// Both triggers and D-pad down: the next diagnostic mode.
void gfx_diag_mode_cycle(void);
// Test scripts: an optimization set and a diagnostic mode (-1 leaves one as it is).
void gfx_diag_select(int set, int mode);
#endif

#endif
