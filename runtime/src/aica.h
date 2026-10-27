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
// Sound RAM for other users of the AICA (runtime/mod/ambience.c), after the samples: from the port
// firmware's area when it runs, otherwise KallistiOS's snd_mem_malloc. 0 when it's full.
uint32_t aica_sound_alloc(uint32_t bytes);
// Whether the port's own firmware (-DAICA_FW) runs, and its sample clock: 44100 a second.
bool aica_port_firmware(void);
uint32_t aica_port_clock(void);
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
