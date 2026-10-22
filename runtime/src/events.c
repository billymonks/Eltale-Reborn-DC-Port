// Hardware events: the 60 Hz VI retrace, osSetTimer timers, RSP task completion, and the VI and
// AI functions that only need to keep their state for now.

#include <kos.h>
#include <dc/vblank.h>
#include <stdio.h>

#include "gfx.h"
#include "ultra.h"

// NTSC retrace interval: 1 / 59.94 Hz.
#define VI_INTERVAL_NS 16683350ull

#define MAX_TIMERS 16
#define EVENTS_KOS_PRIO (PRIO_DEFAULT - 5)

typedef struct {
    uint32_t mq;
    uint32_t msg;
} EventTarget;

typedef struct {
    uint32_t vram;
    uint64_t deadline_us;
    uint64_t interval_us;
    uint32_t mq;
    uint32_t msg;
    bool active;
} Timer;

static EventTarget event_targets[OS_NUM_EVENTS];
static EventTarget vi_target;
static uint32_t vi_retrace_count = 1;
static Timer timers[MAX_TIMERS];
static uint64_t boot_us;

static uint32_t vi_current_fb;
static uint32_t vi_next_fb;

volatile uint32_t stat_vi_count;
volatile uint32_t stat_swap_count;
volatile uint32_t stat_gfx_tasks;
volatile uint32_t stat_audio_tasks;
volatile uint32_t stat_dma_count;
volatile uint32_t stat_dma_bytes;

uint64_t events_now_us(void) {
    return timer_us_gettime64() - boot_us;
}

bool events_post(int event) {
    EventTarget target = event_targets[event];
    if (target.mq == 0) {
        return false;
    }
    mq_send_nonblock(target.mq, target.msg);
    return true;
}

static void log_progress(void) {
    // Game globals from the Eltale Reborn mod's include/q64/game.h.
    printf("[q64dc] t=%us VI %u swaps %u gfx %u audio %u dma %u (%u KB) | mode %u state %d map %d next %d/%d\n",
        (unsigned)(events_now_us() / 1000000), (unsigned)stat_vi_count, (unsigned)stat_swap_count,
        (unsigned)stat_gfx_tasks, (unsigned)stat_audio_tasks, (unsigned)stat_dma_count, (unsigned)(stat_dma_bytes / 1024),
        (unsigned)MEM_HU(0, 0x8007B2E0u), (int)MEM_W(0, 0x8007B2E4u), (int)MEM_W(0, 0x80084EECu),
        (int)MEM_W(0, 0x80084EE4u), (int)MEM_W(0, 0x80084EE8u));
    gfx_print_stats();
    rom_print_stats();
}

// The VI retrace follows the Dreamcast's own vertical blank, so the game's frames line up with
// the PowerVR's buffer flips.
static semaphore_t vblank_sem;

static void on_vblank(uint32_t code, void* data) {
    (void)code;
    (void)data;
    sem_signal(&vblank_sem);
}

static void* events_thread(void* param) {
    (void)param;
    uint32_t remaining_retraces = 1;

    for (;;) {
        uint64_t now_us = timer_us_gettime64();
        // A retrace arrives within 17 ms; this bound only matters if vblanks stop.
        uint64_t deadline_us = now_us + 2 * VI_INTERVAL_NS / 1000;

        sched_lock();
        for (int i = 0; i < MAX_TIMERS; i++) {
            if (timers[i].active && timers[i].deadline_us + boot_us < deadline_us) {
                deadline_us = timers[i].deadline_us + boot_us;
            }
        }
        sched_unlock();

        bool retrace = false;
        if (deadline_us > now_us) {
            int wait_ms = (int)((deadline_us - now_us + 999) / 1000);
            retrace = sem_wait_timed(&vblank_sem, wait_ms) == 0;
        } else {
            retrace = sem_trywait(&vblank_sem) == 0;
        }

        // Timers that are due.
        uint64_t now_rel = events_now_us();
        for (int i = 0; i < MAX_TIMERS; i++) {
            sched_lock();
            Timer t = timers[i];
            bool fire = t.active && t.deadline_us <= now_rel;
            if (fire) {
                if (t.interval_us != 0) {
                    timers[i].deadline_us += t.interval_us;
                } else {
                    timers[i].active = false;
                }
            }
            sched_unlock();
            if (fire && t.mq != 0) {
                mq_send_nonblock(t.mq, t.msg);
            }
        }

        if (retrace) {
            stat_vi_count++;
            if (vi_next_fb != 0) {
                vi_current_fb = vi_next_fb;
            }

            if (--remaining_retraces == 0) {
                remaining_retraces = vi_retrace_count ? vi_retrace_count : 1;
                if (vi_target.mq != 0) {
                    mq_send_nonblock(vi_target.mq, vi_target.msg);
                }
            }

            if (stat_vi_count % 60 == 0) {
                log_progress();
            }
        }
    }
    return NULL;
}

void events_init(void) {
    boot_us = timer_us_gettime64();
    sem_init(&vblank_sem, 0);
    vblank_handler_add(on_vblank, NULL);

    kthread_attr_t attr = {0};
    attr.stack_size = 16 * 1024;
    attr.prio = EVENTS_KOS_PRIO;
    attr.label = "n64 events";
    attr.create_detached = true;
    if (thd_create_ex(&attr, events_thread, NULL) == NULL) {
        runtime_fatal("[q64dc] couldn't create the events thread");
    }
}

void osSetEventMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t event = ctx->r4;
    if (event < OS_NUM_EVENTS) {
        sched_lock();
        event_targets[event].mq = ctx->r5;
        event_targets[event].msg = ctx->r6;
        sched_unlock();
    }
    printf("[q64dc] osSetEventMesg event %u queue 0x%08X\n", (unsigned)event, (unsigned)ctx->r5);
}

void osViSetEvent_recomp(uint8_t* rdram, recomp_context* ctx) {
    sched_lock();
    vi_target.mq = ctx->r4;
    vi_target.msg = ctx->r5;
    vi_retrace_count = ctx->r6;
    sched_unlock();
    printf("[q64dc] osViSetEvent queue 0x%08X every %u retraces\n", (unsigned)ctx->r4, (unsigned)ctx->r6);
}

void osGetCount_recomp(uint8_t* rdram, recomp_context* ctx) {
    sched_check_yield();
    ctx->r2 = (gpr)(uint32_t)US_TO_COUNT_TICKS(events_now_us());
}

void osGetTime_recomp(uint8_t* rdram, recomp_context* ctx) {
    sched_check_yield();
    uint64_t count = US_TO_COUNT_TICKS(events_now_us());
    ctx->r2 = (gpr)(uint32_t)(count >> 32);
    ctx->r3 = (gpr)(uint32_t)count;
}

void osSetTimer_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t vram = ctx->r4;
    uint64_t countdown = ((uint64_t)ctx->r6 << 32) | ctx->r7;
    uint64_t interval = ((uint64_t)STACK_ARG(ctx, 4) << 32) | STACK_ARG(ctx, 5);
    uint32_t mq = STACK_ARG(ctx, 6);
    uint32_t msg = STACK_ARG(ctx, 7);

    if (countdown == 0) {
        countdown = interval;
    }

    sched_lock();
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active && timers[i].vram == vram) {
            slot = i;
            break;
        }
        if (slot < 0 && !timers[i].active) {
            slot = i;
        }
    }
    if (slot < 0) {
        runtime_fatal("[q64dc] out of timers");
    }
    timers[slot].vram = vram;
    timers[slot].deadline_us = events_now_us() + COUNT_TICKS_TO_US(countdown);
    timers[slot].interval_us = COUNT_TICKS_TO_US(interval);
    timers[slot].mq = mq;
    timers[slot].msg = msg;
    timers[slot].active = true;
    sched_unlock();

    ctx->r2 = 0;
}

// RSP tasks finish as soon as they start: graphics and audio aren't processed yet, but the game
// has to see its SP (and for graphics, DP) completion messages to keep going.
void osSpTaskLoad_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osSpTaskStartGo_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t task = ctx->r4;
    uint32_t type = (uint32_t)MEM_W(0x00, task);

    if (type == M_GFXTASK) {
        stat_gfx_tasks++;
        gfx_run_task(task);
        events_post(OS_EVENT_SP);
        events_post(OS_EVENT_DP);
    } else {
        if (type == M_AUDTASK) {
            stat_audio_tasks++;
        }
        events_post(OS_EVENT_SP);
    }
}

void osSpTaskYield_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osSpTaskYielded_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}

void __osSpSetPc_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}

void osCreateViManager_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osViSetMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osViBlack_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osViSetSpecialFeatures_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    vi_next_fb = ctx->r4;
    stat_swap_count++;
}

void osViGetCurrentFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = vi_current_fb;
}

static uint32_t ai_frequency = 32000;

void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx) {
    ai_frequency = ctx->r4;
    ctx->r2 = ai_frequency;
}

void osAiGetLength_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}

void osAiSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}
