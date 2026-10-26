#ifndef __AUDIO_HLE_H__
#define __AUDIO_HLE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runs an audio task's command list (libultra's aspMain, ABI 1) on the CPU. `task` is the OSTask's
// physical address; addresses in the command list are physical too. Plain C, so the host-side
// comparison against the recompiled microcode (tools/audio_test) builds it unchanged.
void audio_hle_run(uint8_t* rdram, uint32_t rdram_size, uint32_t task);

// Command counts by opcode since the last call, for the progress log, and the time spent in each
// in microseconds (zero unless built with AUDIO_PROFILE).
void audio_hle_take_counts(uint32_t counts[16], uint32_t us[16]);

// 0 exact microcode, 1 same math with wider loads and a branchless clamp, 2 that plus linear
// resampling, 3 that plus the reverb pole filter bypassed. The perf HUD cycles these.
void audio_hle_cycle_mode(void);
int audio_hle_mode(void);
// Fast-path hits, linear resamples, and reverb sends dropped, since the previous take.
uint32_t audio_hle_take_paths(uint32_t* linear, uint32_t* dry);
// Voices given a nonzero reverb send since the previous take, and the largest send level.
uint32_t audio_hle_take_wet(int16_t* max);

#ifdef __cplusplus
}
#endif

#endif
