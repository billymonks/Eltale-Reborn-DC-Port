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

#ifdef __cplusplus
}
#endif

#endif
