#ifndef __ULTRA_H__
#define __ULTRA_H__

// Internal interface between the runtime's libultra replacements.

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

// libultra constants the runtime needs.
#define OS_MESG_NOBLOCK 0
#define OS_MESG_BLOCK   1

#define OS_EVENT_SW1      0
#define OS_EVENT_SW2      1
#define OS_EVENT_CART     2
#define OS_EVENT_COUNTER  3
#define OS_EVENT_SP       4
#define OS_EVENT_SI       5
#define OS_EVENT_AI       6
#define OS_EVENT_VI       7
#define OS_EVENT_PI       8
#define OS_EVENT_DP       9
#define OS_NUM_EVENTS     15

#define M_GFXTASK 1
#define M_AUDTASK 2

// The VR4300 count register runs at half the CPU clock, 46.875 MHz.
#define COUNT_TICKS_TO_US(t) ((uint64_t)(t) * 8 / 375)
#define US_TO_COUNT_TICKS(u) ((uint64_t)(u) * 375 / 8)

// Runtime-owned RDRAM above the game's 4 MB, for structures libultra returns pointers to.
#define RUNTIME_RAM_BASE 0x80400000u
#define RUNTIME_RAM_SIZE 0x10000u
#define OS_CART_HANDLE   (RUNTIME_RAM_BASE + 0x0)

// Arguments past the fourth are on the N64 stack (o32 ABI).
#define STACK_ARG(ctx, n) ((uint32_t)MEM_W(0x10 + 4 * ((n) - 4), (ctx)->r29))

// Scheduler (sched.cpp).
void sched_init(void);
// Sends a message without blocking, from a game thread or from the runtime's own threads.
// A game thread caller switches to a woken thread that has a higher priority.
int32_t mq_send_nonblock(uint32_t mq, uint32_t msg);
// Called by runtime functions on game threads: switches to a higher-priority thread made ready
// by an event since the last switch.
void sched_check_yield(void);
// Locks out the scheduler while runtime threads touch shared state.
void sched_lock(void);
void sched_unlock(void);

// Events: VI, timers, RSP tasks (events.c).
void events_init(void);
// Sends the message registered with osSetEventMesg; false if nothing is registered.
bool events_post(int event);
uint64_t events_now_us(void);

extern volatile uint32_t stat_vi_count;
extern volatile uint32_t stat_swap_count;
extern volatile uint32_t stat_gfx_tasks;
extern volatile uint32_t stat_audio_tasks;
extern volatile uint32_t stat_dma_count;
extern volatile uint32_t stat_dma_bytes;

// Controllers (cont.c).
void cont_poll(void);

// Controller Pak saves on the VMU (pak.c).
void pak_init(void);
// Writes pending changes to the VMU once the game has stopped writing for a moment.
void pak_flush_idle(void);

// ROM cache statistics (rom.c).
void rom_print_stats(void);

// ROM access with a block cache (rom.c). rdram_addr may be any alignment.
void rom_dma_to_rdram(uint32_t rom_addr, uint32_t rdram_addr, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
