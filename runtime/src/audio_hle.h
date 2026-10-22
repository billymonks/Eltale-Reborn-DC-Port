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

#ifdef __cplusplus
}
#endif

#endif
