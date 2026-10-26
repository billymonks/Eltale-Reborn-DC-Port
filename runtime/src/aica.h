#ifndef __AICA_H__
#define __AICA_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The synthesizer's voices on the AICA's channels (aica.c). aica_init starts the sound processor
// and puts every sample in sound RAM; false if the AICA didn't answer or the samples didn't fit, and
// the game's audio is then mixed on the SH-4 (audio_hle.c) as before.
bool aica_init(void);
bool aica_active(void);
// osAiGetLength, in bytes, for the game's frame size in samples.
uint32_t aica_ai_length(uint32_t frame_size);
// osAiSetNextBuffer: the samples the game says it made.
void aica_ai_buffer(uint32_t frames);
// Microseconds spent in the frame hook since the previous take, and how many frames that was.
uint32_t aica_take_frame_us(uint32_t* frames);
// For the perf HUD: voices sounding now, the lead in milliseconds, and time line slips since boot.
void aica_hud(uint32_t* voices_on, uint32_t* lead_ms, uint32_t* slips);
void aica_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif
