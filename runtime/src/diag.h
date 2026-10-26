// GFX_DIAG builds (on top of PERF_HUD): cheap clocks for the renderer's per-frame figures.
#ifndef Q64DC_DIAG_H
#define Q64DC_DIAG_H

#ifdef GFX_DIAG
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// KallistiOS's clock, TMU2, counts down at 12.47 MHz (80 ns) and reloads once a second.
#define DIAG_TMU2_TCOR (*(volatile uint32_t*)0xFFD80020)
#define DIAG_TMU2_TCNT (*(volatile uint32_t*)0xFFD80024)
#define DIAG_TICKS_PER_100US 1247u

// TMU2 ticks from then to now, less than a second apart.
static inline uint32_t diag_ticks_between(uint32_t then, uint32_t now) {
    return then >= now ? then - now : then + DIAG_TMU2_TCOR + 1 - now;
}

// Low words of the performance counters: 0 counts operand cache miss stall cycles in these builds
// (instead of KallistiOS's unused nanosecond clock), 1 instruction cache miss stall cycles.
#define DIAG_PMC0 (*(volatile uint32_t*)0xFF100008)
#define DIAG_PMC1 (*(volatile uint32_t*)0xFF100010)

// Both counters summed over the time game threads held the CPU (sched.cpp). The renderer takes
// them out of its own counts, as game threads preempt the RSP thread.
extern volatile uint32_t diag_game_pmc0, diag_game_pmc1;

#ifdef __cplusplus
}
#endif

#endif
#endif
