// F3DEX 1.23 display lists drawn with the PowerVR.
#ifndef Q64DC_GFX_H
#define Q64DC_GFX_H

#include <stdint.h>

void gfx_init(void);
// Runs a graphics task's display list and submits the frame. Returns once the PowerVR has the
// whole scene, so the game can be told the task is done.
void gfx_run_task(uint32_t task);
void gfx_print_stats(void);

#endif
