#ifndef Q64DC_PROFILE_H
#define Q64DC_PROFILE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void profile_start(void);
void profile_dump(void);
// Between the two: the periodic log is held back meanwhile, as release discs don't print it.
bool profile_running(void);

#ifdef PERF_HUD
// Both triggers and X. First press samples for 10 seconds. Later presses flip the page.
void profile_chord(void);
void profile_poll(void);
// 0 idle, 1 sampling, 2 function page, 3 group page, 4 (GFX_DIAG) the busiest 32-byte buckets.
int profile_mode(void);
int profile_secs(void);
const char* profile_row(int i);
uint32_t profile_table_bytes(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
