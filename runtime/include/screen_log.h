#ifndef __SCREEN_LOG_H__
#define __SCREEN_LOG_H__

// RT_DEFINES=-DSCREEN_LOG: startup lines and exceptions on the TV, via KallistiOS's
// framebuffer dbgio. Release discs leave these as nothing.

#ifdef SCREEN_LOG
void screen_log_init(void);
void screen_mark(const char* msg);
void screen_log_pvr(int up);
#else
static inline void screen_log_init(void) {}
static inline void screen_mark(const char* msg) { (void)msg; }
static inline void screen_log_pvr(int up) { (void)up; }
#endif

#endif
