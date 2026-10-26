// The port's AICA firmware: runs the SH-4's timed channel events (shared.h) when they fall due,
// and walks volume ramps in TL steps every Q64_AICA_TICK samples, so the SH-4 sends one event
// per ramp. The clock is the play position of a channel looping silence at 44.1 kHz.
//
// ARM7DI (ARMv3): no halfword loads or stores, so the shared structures are words and the channel
// registers are written 32 bits at a time.

#include <stddef.h>
#include <stdint.h>

#include "shared.h"

#define REG(off)     (*(volatile uint32_t*)(0x00800000u + (off)))
#define CHN(ch, off) REG((uint32_t)(ch) * 0x80u + (off))

#define KYONEX 0x8000u
#define KYONB  0x4000u

static volatile q64_shared* const sh = (volatile q64_shared*)Q64_AICA_SHARED;
static volatile q64_event* const ring = (volatile q64_event*)Q64_AICA_RING;

// Each channel's TL ramp, 16.16.
static int32_t tl_cur[64], tl_step[64], tl_end[64];
static uint32_t tl_ticks[64];
static uint32_t tl_sent[64];

static uint32_t clock_base, clock_last;

static uint32_t clock_now(void) {
    uint32_t ca = REG(0x2814) & 0xFFFFu;
    if (ca < clock_last) {
        clock_base += Q64_AICA_CLOCK_LOOP;
    }
    clock_last = ca;
    return clock_base + ca;
}

static void write_tl(int ch, int32_t tl1616) {
    uint32_t tl = tl1616 < 0 ? 0 : tl1616 > (255 << 16) ? 255 : (uint32_t)tl1616 >> 16;
    if (tl != tl_sent[ch]) {
        tl_sent[ch] = tl;
        // TL, then the filter off and resonance 4 as KallistiOS sets them.
        CHN(ch, 0x28) = (tl << 8) | 0x24;
    }
}

static void key_off(int ch) {
    CHN(ch, 0x00) = CHN(ch, 0x00) & ~(KYONB | KYONEX);
    CHN(ch + 1, 0x00) = (CHN(ch + 1, 0x00) & ~KYONB) | KYONEX;
    tl_ticks[ch] = tl_ticks[ch + 1] = 0;
}

static void setup(int ch, uint32_t pitch, uint32_t a, uint32_t b) {
    uint32_t sa = a & 0x7FFFFFu;
    uint32_t loop = a >> 31;
    key_off(ch);
    for (int s = 0; s < 2; s++) {
        int c = ch + s;
        CHN(c, 0x04) = sa & 0xFFFFu;
        CHN(c, 0x08) = b & 0xFFFFu;
        CHN(c, 0x0C) = b >> 16;
        CHN(c, 0x10) = 0x1F;                    // attack at once, no decay
        CHN(c, 0x14) = (15u << 10) | 0x1F;      // no key scaling, fastest release
        CHN(c, 0x18) = pitch;
        CHN(c, 0x1C) = 0;                       // no LFO
        CHN(c, 0x20) = 0;                       // no DSP send
        CHN(c, 0x24) = (15u << 8) | (s == 0 ? 0x1Fu : 0x0Fu);  // full level, hard left or right
        tl_cur[c] = tl_end[c] = 255 << 16;
        tl_step[c] = 0;
        tl_ticks[c] = 0;
        tl_sent[c] = 0x100;
        write_tl(c, tl_cur[c]);
        // 16-bit PCM, loop flag, address; key on comes with Q64_EV_KEYON.
        CHN(c, 0x00) = (loop << 9) | (sa >> 16);
    }
}

static void ramp(int ch, uint32_t ticks, uint32_t a, uint32_t b) {
    for (int s = 0; s < 2; s++) {
        int c = ch + s;
        int32_t from = (int32_t)(s == 0 ? a & 0xFFFFu : a >> 16) << 8;
        int32_t end = (int32_t)(s == 0 ? b & 0xFFFFu : b >> 16) << 8;
        tl_end[c] = end;
        tl_ticks[c] = ticks;
        if (ticks == 0) {
            tl_cur[c] = end;
        } else {
            tl_cur[c] = from;
            tl_step[c] = (end - from) / (int32_t)ticks;
        }
        write_tl(c, tl_cur[c]);
    }
}

static void apply(const volatile q64_event* e) {
    uint32_t op = e->op;
    int ch = (int)((op >> 8) & 0x3E);
    uint32_t h = op >> 16;
    switch (op & 0xFF) {
        case Q64_EV_SETUP:
            setup(ch, h, e->a, e->b);
            break;
        case Q64_EV_RAMP:
            ramp(ch, h, e->a, e->b);
            break;
        case Q64_EV_KEYON:
            CHN(ch, 0x00) = (CHN(ch, 0x00) & ~KYONEX) | KYONB;
            CHN(ch + 1, 0x00) = CHN(ch + 1, 0x00) | KYONB | KYONEX;
            break;
        case Q64_EV_KEYOFF:
            key_off(ch);
            break;
        case Q64_EV_PITCH:
            CHN(ch, 0x18) = h;
            CHN(ch + 1, 0x18) = h;
            break;
        default:
            break;
    }
}

static void step_ramps(void) {
    for (int c = 0; c < 62; c++) {
        if (tl_ticks[c] != 0) {
            // The last step lands on the end, whatever the division dropped.
            tl_cur[c] = --tl_ticks[c] == 0 ? tl_end[c] : tl_cur[c] + tl_step[c];
            write_tl(c, tl_cur[c]);
            sh->steps++;
        }
    }
}

static void init(void) {
    REG(0x2800) = 0;
    for (int c = 0; c < 64; c++) {
        CHN(c, 0x00) = KYONEX;
        for (uint32_t r = 0x04; r < 0x80; r += 4) {
            CHN(c, r) = 0;
        }
        CHN(c, 0x14) = 0x1F;
        tl_sent[c] = 0x100;
    }
    // The boot ROM leaves its reverb program running, and the DSP's ring would land on this
    // program's memory: clear the program and the DSP's outputs.
    for (uint32_t i = 0; i < 128 * 4; i++) {
        REG(0x3400 + 4 * i) = 0;
    }
    for (uint32_t i = 0; i < 18; i++) {
        REG(0x2000 + 4 * i) = 0;
    }
    // The clock: 8-bit silence, looping at 44.1 kHz, inaudible, watched through the monitor.
    int c = Q64_AICA_CLOCK_CH;
    CHN(c, 0x04) = Q64_AICA_CLOCK_BUF & 0xFFFFu;
    CHN(c, 0x08) = 0;
    CHN(c, 0x0C) = Q64_AICA_CLOCK_LOOP;
    CHN(c, 0x10) = 0x1F;
    CHN(c, 0x14) = (15u << 10) | 0x1F;
    CHN(c, 0x18) = 0;
    CHN(c, 0x24) = 0;
    CHN(c, 0x28) = (255u << 8) | 0x24;
    CHN(c, 0x00) = KYONEX | KYONB | (1u << 9) | (1u << 7) | (Q64_AICA_CLOCK_BUF >> 16);
    REG(0x280C) = (uint32_t)c << 8;
    REG(0x2800) = 0x0F;
}

int arm_main(void) {
    init();
    sh->head = 0;
    sh->tail = 0;
    sh->passes = sh->applied = sh->late = sh->max_late = sh->steps = 0;
    sh->version = Q64_AICA_VERSION;
    uint32_t tail = 0;
    uint32_t next_tick = clock_now() + Q64_AICA_TICK;
    sh->now = next_tick;
    sh->magic = Q64_AICA_MAGIC;
    for (;;) {
        uint32_t now = clock_now();
        sh->now = now;
        sh->passes++;
        uint32_t head = sh->head & (Q64_AICA_EVENTS - 1);
        while (tail != head) {
            volatile q64_event* e = &ring[tail];
            int32_t late = (int32_t)(now - e->time);
            if (late < 0) {
                break;
            }
            if (late > Q64_AICA_TICK) {
                sh->late++;
                if ((uint32_t)late > sh->max_late) {
                    sh->max_late = (uint32_t)late;
                }
            }
            apply(e);
            sh->applied++;
            tail = (tail + 1) & (Q64_AICA_EVENTS - 1);
            sh->tail = tail;
        }
        if ((int32_t)(now - next_tick) >= 0) {
            step_ramps();
            next_tick += Q64_AICA_TICK;
            if ((int32_t)(now - next_tick) > 8 * Q64_AICA_TICK) {
                next_tick = now + Q64_AICA_TICK;
            }
        }
    }
    return 0;
}
