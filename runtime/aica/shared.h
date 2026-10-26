// What the port's AICA firmware (runtime/aica/main.c, on the ARM) and runtime/src/aica.c (on the
// SH-4) agree on. Addresses are in sound RAM, as the ARM sees it from 0.
#ifndef Q64_AICA_SHARED_H
#define Q64_AICA_SHARED_H

#include <stdint.h>

#define Q64_AICA_MAGIC   0x41343651u  // "Q64A", written by the firmware once it runs
#define Q64_AICA_VERSION 1

// Code, data and the stack live below 0x10000.
#define Q64_AICA_SHARED  0x010000u
#define Q64_AICA_RING    0x010040u
#define Q64_AICA_EVENTS  1024u
// A loop of 8-bit silence the clock channel plays: its play position counts samples.
#define Q64_AICA_CLOCK_BUF  0x01C000u
#define Q64_AICA_CLOCK_LOOP 16384u
#define Q64_AICA_CLOCK_CH   63
// Sample memory starts here.
#define Q64_AICA_SAMPLES 0x020000u

// Ramps step every this many samples.
#define Q64_AICA_TICK 32

// One timed change to a pair of channels, the left one even. Words, so the ARM never needs a
// halfword load: op | ch << 8 | h << 16, then a and b.
enum {
    Q64_EV_SETUP = 1,  // h pitch (OCT << 11 | FNS); a sample address | loop << 31; b LSA | LEA << 16
    Q64_EV_RAMP,       // h ticks (0: at once); a TL 8.8 now, left | right << 16; b TL 8.8 at the end
    Q64_EV_KEYON,
    Q64_EV_KEYOFF,
    Q64_EV_PITCH,      // h pitch
};

typedef struct {
    uint32_t time;  // the clock below
    uint32_t op;
    uint32_t a, b;
} q64_event;

typedef struct {
    uint32_t magic, version;
    uint32_t now;         // ARM: the sample clock, 44.1 kHz
    uint32_t head;        // SH-4: the next event slot it fills
    uint32_t tail;        // ARM: the next event it runs
    uint32_t passes;      // ARM: main loop passes
    uint32_t applied;     // ARM: events run
    uint32_t late;        // ARM: events run more than a tick after their time
    uint32_t max_late;    // ARM: the latest, in samples
    uint32_t steps;       // ARM: ramp steps written
    uint32_t pad[6];
} q64_shared;

#endif
