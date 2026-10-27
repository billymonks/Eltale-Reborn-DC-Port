// Eltale Reborn's ambient sound. On the PC the mod plays it through a native library of its own
// (eltale_ambience.dll, native/ambience.c in the mod's repository): loops of birds, wind, water,
// crickets... under the music, picked by the room and blended by the time of day, and one-shots (owls
// at night, the rooster at dawn). The mod calls the library once a frame (ambience_frame,
// ambience_set) and for the one-shots (ambience_play). tools/modrecomp sends those calls here
// (runtime_natives), and this plays the clips on AICA channels of their own, after the game's 32
// (aica.c), from ambience.bin on the disc (tools/ambpack, from eltale/ambience).
//
// Levels follow the library: a loop's level moves to its target at a third of full scale a second,
// its volume is the clip's gain times the level squared, a loop that falls silent stops and later
// resumes near where it stopped, and with no frames for half a second (the title screen) everything
// fades out. Indoors (the muffle amount) the library mixes in a 700 Hz low-pass of the clips; here a
// clip's volume follows how loud that mix comes out for it (ambience_pack.h), without the filter's tone.
//
// Each playing loop streams through a ring in sound RAM, which a thread below the game's tops up from
// a buffer in main RAM, and the buffer from the disc when the ROM reader has nothing to read (or the
// loop is close to running dry). One-shots are in sound RAM from the start. The channels are
// programmed from here directly: the firmware, KallistiOS's or the port's, only touches channels it
// plays.

#include <kos.h>
#include <dc/g2bus.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ambience_pack.h"
#include "modrt.h"
#include "../src/aica.h"
#include "../src/test.h"
#include "../src/ultra.h"

#define PACK_PATH "/cd/ambience.bin"

#define FADE_S      3.0f   // a loop's level from silence to full
#define MUFFLE_S    1.0f
#define WATCHDOG_US 500000 // no frame for this long: everything fades out
#define TICK_MS     20
#define PRIO        (PRIO_DEFAULT + 2)

// Channels 40-55: up to 6 loops and the one-shots, a pair each.
#define FIRST_CH   40
#define LOOP_SLOTS 6
#define MAX_SHOTS  2

// A loop's ring: 65024 samples (2.95 s) per channel, under the AICA's 16-bit loop end. Its buffer in
// main RAM holds STAGE_CHUNKS chunks per channel, read from the disc READ_CHUNKS at most at a time,
// once READ_FREE chunks are free: each read costs a seek, which on a CD-R is the slow part.
#define RING_SAMPLES 65024
#define RING_BYTES   (RING_SAMPLES / 2)
#define STAGE_CHUNKS 4
#define STAGE_BYTES  (STAGE_CHUNKS * AMB_PACK_CHUNK)
#define READ_CHUNKS  4
#define READ_FREE    3
// Ring samples kept clear of the play position, and the least ahead of it before the loop counts as
// running dry. With less than URGENT_S buffered, a loop reads even while the ROM reader is busy. A
// loop starting (at a room change, while the game loads the room) waits up to PRIME_WAIT_US for it.
#define RING_GUARD    1024
#define RING_LOW      2048
#define URGENT_S      1.0f
#define PRIME_WAIT_US 2000000
// After key-on a loop stays muted for this long while its preamble plays.
#define PREAMBLE_MUTE_US 40000

#define AICA_CHN(ch, reg) (0xA0700000u + (uint32_t)(ch) * 0x80u + (reg))
#define KYONEX 0x8000u
#define KYONB  0x4000u
#define LPCTL  0x0200u
#define PITCH_22050 (0xFu << 11)  // OCT -1, FNS 0: half the AICA's 44100 Hz, exactly

typedef struct {
    const AmbPackClip* info;
    const AmbPackStart* starts;
    float gain;              // linear, at level 1
    // Loops.
    volatile int32_t target; // per mille, from the mod
    float level;
    int slot;                // the loop slot playing it, or -1
    uint32_t resume;         // sample to resume near
    // One-shots.
    volatile int32_t play;   // per mille to start at, 0 for none
    volatile int32_t pan;    // -1000 left .. 1000 right
    int shot;                // its pair among the one-shots, or -1
    uint32_t aram[2];
    float shot_amp;
    uint64_t shot_end_us;    // 0: not playing
    uint8_t tl_sent[2];
} Clip;

typedef enum { SLOT_FREE, SLOT_PRIMING, SLOT_PLAYING } SlotState;

typedef struct {
    SlotState state;
    int clip;
    uint32_t ring[2];        // sound RAM
    uint8_t* stage[2];       // main RAM
    uint32_t stage_rd, stage_len;  // bytes of each channel
    uint32_t src;            // clip byte (per channel) the next disc read starts at
    uint32_t data_byte;      // clip byte of the ring's first data byte (after the preamble)
    uint32_t pre_bytes;      // the preamble's bytes at the start of the ring
    uint32_t written;        // samples written to the ring since key-on
    uint32_t played;         // samples played since key-on
    uint32_t last_pos;
    uint32_t keyon_clock;    // the port firmware's clock at key-on
    uint64_t keyon_us;
    uint64_t start_us;       // when it was given its clip
    uint8_t tl_sent[2];
} Slot;

static Clip clips[AMB_PACK_MAX];
static int clip_count;
static Slot slots[LOOP_SLOTS];
static int slot_count;
static int shot_count;
static file_t pack = FILEHND_INVALID;
static uint8_t* tables;
static uint8_t* bounce;
static bool port_fw;

static volatile int32_t master_pm = 1000, muffle_pm;
static volatile uint64_t last_frame_us;
static float muffle;
static volatile bool started;

static struct {
    uint32_t reads, read_bytes, read_us, read_max_us, deferred, underruns, starts, stops, shots;
} st;

// --- Small helpers --------------------------------------------------------------------------------

static inline void chn_write(int ch, uint32_t reg, uint32_t v) {
    g2_write_32(AICA_CHN(ch, reg), v);
}

static int32_t per_mille(uint32_t reg) {
    int32_t v = (int32_t)reg;
    return v < 0 ? 0 : v > 4000 ? 4000 : v;
}

// TL: 16 steps a factor of two (0.375 dB), 255 silent.
static uint8_t tl_of(float amp) {
    if (!(amp > 0.0f)) {
        return 255;
    }
    float tl = -16.0f * log2f(amp);
    return tl <= 0.0f ? 0 : tl >= 254.5f ? 255 : (uint8_t)(tl + 0.5f);
}

static float muffle_gain(const Clip* c) {
    float m = muffle, d = 1.0f - m;
    float e = d * d + m * m * c->info->wet + 2.0f * d * m * c->info->cross;
    return e > 0.0f ? sqrtf(e) : 0.0f;
}

static void set_tl(int ch, uint8_t tl, uint8_t* sent) {
    if (tl != *sent) {
        *sent = tl;
        chn_write(ch, 0x28, (uint32_t)tl << 8 | 0x24);
    }
}

// Sets a channel up keyed off, looping from LSA to LEA: ADPCM from aram (a ring's is a long stream,
// which carries the decoder on round the loop) at 22050 Hz, full level at once and silent at once
// on key off, straight to the output at pan. A one-shot loops in the silence after it, so it never
// ends keyed on, where the next KYONEX (anyone's) would start it again.
static uint32_t chn_setup(int ch, uint32_t aram, uint32_t lsa, uint32_t lea, bool ring, uint32_t pan) {
    uint32_t r0 = (ring ? 3u << 7 : 2u << 7) | LPCTL | (aram >> 16);
    chn_write(ch, 0x00, r0);
    chn_write(ch, 0x04, aram & 0xFFFF);
    chn_write(ch, 0x08, lsa);
    chn_write(ch, 0x0C, lea);
    chn_write(ch, 0x10, 0x001F);
    chn_write(ch, 0x14, 0x001F);
    chn_write(ch, 0x18, PITCH_22050);
    chn_write(ch, 0x1C, 0);
    chn_write(ch, 0x20, 0);
    chn_write(ch, 0x24, 15u << 8 | pan);
    chn_write(ch, 0x28, 255u << 8 | 0x24);
    return r0;
}

// Both channels of a pair key on (or off) on the same sample: the first only gets KYONB, which the
// second's KYONEX carries out. A mono clip uses the first alone.
static void key(int ch, uint32_t r0[2], int channels, bool on) {
    uint32_t b = on ? KYONB : 0;
    if (channels == 2) {
        chn_write(ch, 0x00, r0[0] | b);
        chn_write(ch + 1, 0x00, r0[1] | b | KYONEX);
    } else {
        chn_write(ch, 0x00, r0[0] | b | KYONEX);
    }
}

// Samples a loop's channel has played since key-on. KallistiOS's firmware keeps every channel's play
// position in sound RAM, a few milliseconds old, so right after key-on it may still be the channel's
// last stream's: until a position fits the time since key-on, none counts. The port firmware has a
// sample clock instead, which the channel follows exactly.
#define POS_UNKNOWN 0xFFFFFFFFu

static uint32_t slot_played(Slot* s, int ch) {
    if (port_fw) {
        return (aica_port_clock() - s->keyon_clock) / 2;
    }
    uint32_t pos = g2_read_32(SPU_RAM_UNCACHED_BASE + 0x20000 + ch * 64 + 40) & 0xFFFF;
    if (s->last_pos == POS_UNKNOWN) {
        uint64_t expect = (timer_us_gettime64() - s->keyon_us) * AMB_PACK_RATE / 1000000u;
        if (pos > expect + RING_LOW) {
            return 0;
        }
        s->last_pos = 0;
    }
    uint32_t delta = (pos + RING_SAMPLES - s->last_pos) % RING_SAMPLES;
    s->last_pos = pos;
    s->played += delta;
    return s->played;
}

// --- Setup ----------------------------------------------------------------------------------------

static bool read_at(uint32_t offset, void* dst, uint32_t bytes) {
    uint64_t t0 = timer_us_gettime64();
    bool ok = fs_seek(pack, (off_t)offset, SEEK_SET) == (off_t)offset && fs_read(pack, dst, bytes) == (ssize_t)bytes;
    uint32_t us = (uint32_t)(timer_us_gettime64() - t0);
    st.reads++;
    st.read_bytes += bytes;
    st.read_us += us;
    st.read_max_us = us > st.read_max_us ? us : st.read_max_us;
    return ok;
}

static bool setup(void) {
    pack = fs_open(PACK_PATH, O_RDONLY);
    if (pack == FILEHND_INVALID) {
        printf("[q64dc] ambience: no %s, ambient sound is off\n", PACK_PATH);
        return false;
    }
    bounce = memalign(32, READ_CHUNKS * 2 * AMB_PACK_CHUNK);
    AmbPackHeader hdr;
    if (bounce == NULL || !read_at(0, bounce, AMB_PACK_ALIGN)) {
        return false;
    }
    memcpy(&hdr, bounce, sizeof(hdr));
    if (hdr.magic != AMB_PACK_MAGIC || hdr.count == 0 || hdr.count > AMB_PACK_MAX || hdr.rate != AMB_PACK_RATE ||
        hdr.chunk != AMB_PACK_CHUNK || hdr.data % AMB_PACK_ALIGN != 0) {
        printf("[q64dc] ambience: %s isn't an ambience pack this runtime reads\n", PACK_PATH);
        return false;
    }
    tables = memalign(32, hdr.data);
    if (tables == NULL || !read_at(0, tables, hdr.data)) {
        return false;
    }
    clip_count = (int)hdr.count;
    const AmbPackClip* infos = (const AmbPackClip*)(tables + sizeof(AmbPackHeader));
    const AmbPackStart* starts = (const AmbPackStart*)(tables + sizeof(AmbPackHeader) + AMB_PACK_MAX * sizeof(AmbPackClip));
    port_fw = aica_port_firmware();

    uint32_t aram_bytes = 0;
    for (int i = 0; i < clip_count; i++) {
        Clip* c = &clips[i];
        c->info = &infos[i];
        c->starts = &starts[i * AMB_PACK_STARTS];
        c->gain = powf(10.0f, (float)c->info->gain_mdb / 20000.0f);
        c->slot = -1;
        c->shot = -1;
        c->tl_sent[0] = c->tl_sent[1] = 255;
        if (c->info->loops || shot_count == MAX_SHOTS) {
            continue;
        }
        // A one-shot goes into sound RAM whole, a channel at a time.
        uint32_t bytes = c->info->samples / 2;
        uint32_t chunks = (bytes + AMB_PACK_CHUNK - 1) / AMB_PACK_CHUNK;
        if (chunks > READ_CHUNKS || !read_at(c->info->offset, bounce, chunks * AMB_PACK_CHUNK * c->info->channels)) {
            printf("[q64dc] ambience: can't load %s\n", c->info->name);
            continue;
        }
        bool ok = true;
        for (int ch = 0; ch < c->info->channels && ok; ch++) {
            c->aram[ch] = aica_sound_alloc((bytes + 31) & ~31u);
            ok = c->aram[ch] != 0;
            for (uint32_t k = 0; ok && k < chunks; k++) {
                uint32_t n = k + 1 == chunks ? bytes - k * AMB_PACK_CHUNK : AMB_PACK_CHUNK;
                spu_memload(c->aram[ch] + k * AMB_PACK_CHUNK, bounce + (k * c->info->channels + ch) * AMB_PACK_CHUNK,
                            (n + 3) & ~3u);
            }
            aram_bytes += (bytes + 31) & ~31u;
        }
        if (ok) {
            c->shot = shot_count++;
        }
    }
    for (int i = 0; i < LOOP_SLOTS; i++) {
        Slot* s = &slots[i];
        s->stage[0] = memalign(32, STAGE_BYTES);
        s->stage[1] = memalign(32, STAGE_BYTES);
        s->ring[0] = aica_sound_alloc(RING_BYTES);
        s->ring[1] = s->ring[0] != 0 ? aica_sound_alloc(RING_BYTES) : 0;
        if (s->stage[0] == NULL || s->stage[1] == NULL || s->ring[1] == 0) {
            free(s->stage[0]);
            free(s->stage[1]);
            break;
        }
        s->state = SLOT_FREE;
        s->tl_sent[0] = s->tl_sent[1] = 255;
        aram_bytes += 2 * RING_BYTES;
        slot_count++;
    }
    struct mallinfo mi = mallinfo();
    printf("[q64dc] ambience: %d clips from %s, %d loops at once, %d one-shots; %u KB of sound RAM (%u KB left), "
           "%u KB of RAM (%u KB left above the heap, %u KB free in it)\n",
           clip_count, PACK_PATH, slot_count, shot_count, (unsigned)(aram_bytes / 1024),
           port_fw ? 0u : (unsigned)(snd_mem_available() / 1024),
           (unsigned)((slot_count * 2 * STAGE_BYTES + READ_CHUNKS * 2 * AMB_PACK_CHUNK + hdr.data) / 1024),
           (unsigned)((0x8D000000u - (uintptr_t)sbrk(0)) / 1024), (unsigned)(mi.fordblks / 1024));
    return slot_count > 0;
}

// --- Loops ----------------------------------------------------------------------------------------

static uint32_t clip_bytes(const Clip* c) {
    return c->info->samples / 2;
}

// Starts a loop on a free slot, from the start nearest where it stopped; it keys on once its first
// data is in.
static void loop_start(int ci) {
    Clip* c = &clips[ci];
    int free_slot = -1;
    for (int i = 0; i < slot_count; i++) {
        if (slots[i].state == SLOT_FREE) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        return;
    }
    Slot* s = &slots[free_slot];
    int best = 0;
    uint32_t best_d = 0xFFFFFFFFu;
    for (int j = 0; j < AMB_PACK_STARTS; j++) {
        uint32_t at = c->starts[j].sample;
        uint32_t d = at > c->resume ? at - c->resume : c->resume - at;
        if (d < best_d) {
            best_d = d;
            best = j;
        }
    }
    const AmbPackStart* start = &c->starts[best];
    s->state = SLOT_PRIMING;
    s->clip = ci;
    s->stage_rd = s->stage_len = 0;
    s->src = start->sample / 2;
    s->data_byte = s->src;
    s->pre_bytes = start->codes / 2;
    s->written = s->played = s->last_pos = 0;
    s->start_us = timer_us_gettime64();
    c->slot = free_slot;
    st.starts++;
}

static void loop_stop(int ci) {
    Clip* c = &clips[ci];
    Slot* s = &slots[c->slot];
    int ch = FIRST_CH + 2 * c->slot;
    if (s->state == SLOT_PLAYING) {
        uint32_t r0[2] = { (3u << 7) | LPCTL | (s->ring[0] >> 16), (3u << 7) | LPCTL | (s->ring[1] >> 16) };
        key(ch, r0, c->info->channels, false);
        // Resume near the sample that was playing.
        uint32_t played = slot_played(s, ch);
        uint32_t done = played > s->pre_bytes * 2 ? played / 2 - s->pre_bytes : 0;
        c->resume = (uint32_t)(((uint64_t)s->data_byte + done) % clip_bytes(c)) * 2;
    }
    s->state = SLOT_FREE;
    s->tl_sent[0] = s->tl_sent[1] = 255;
    c->slot = -1;
    st.stops++;
}

// Moves what the buffer holds into the ring, up to RING_GUARD samples short of the play position.
static void ring_fill(Slot* s, uint32_t played) {
    const Clip* c = &clips[s->clip];
    uint32_t ahead = s->written - played;
    if (ahead >= RING_SAMPLES - RING_GUARD) {
        return;
    }
    uint32_t room = RING_SAMPLES - RING_GUARD - ahead;
    uint32_t bytes = room / 2;
    bytes = bytes < s->stage_len ? bytes : s->stage_len;
    bytes &= ~3u;
    while (bytes > 0) {
        uint32_t ring_at = (s->written / 2) % RING_BYTES;
        uint32_t n = bytes;
        n = n < RING_BYTES - ring_at ? n : RING_BYTES - ring_at;
        n = n < STAGE_BYTES - s->stage_rd ? n : STAGE_BYTES - s->stage_rd;
        for (int ch = 0; ch < c->info->channels; ch++) {
            spu_memload(s->ring[ch] + ring_at, s->stage[ch] + s->stage_rd, n);
        }
        s->stage_rd = (s->stage_rd + n) % STAGE_BYTES;
        s->stage_len -= n;
        s->written += n * 2;
        bytes -= n;
    }
}

// Reads the next chunks of a slot's clip into its buffer: as many as fit, up to READ_CHUNKS and the
// clip's end, where it goes round to the start.
static bool slot_read(Slot* s) {
    const Clip* c = &clips[s->clip];
    uint32_t total = clip_bytes(c);
    uint32_t first = s->src / AMB_PACK_CHUNK;
    uint32_t last_chunk = (total - 1) / AMB_PACK_CHUNK;
    uint32_t n = (STAGE_BYTES - s->stage_len) / AMB_PACK_CHUNK;
    n = n < READ_CHUNKS ? n : READ_CHUNKS;
    n = n < last_chunk - first + 1 ? n : last_chunk - first + 1;
    if (n == 0) {
        return false;
    }
    int chans = c->info->channels;
    if (!read_at(c->info->offset + first * AMB_PACK_CHUNK * chans, bounce, n * AMB_PACK_CHUNK * chans)) {
        return false;
    }
    for (uint32_t k = 0; k < n; k++) {
        uint32_t chunk_at = (first + k) * AMB_PACK_CHUNK;
        uint32_t from = s->src > chunk_at ? s->src - chunk_at : 0;
        uint32_t to = total - chunk_at < AMB_PACK_CHUNK ? total - chunk_at : AMB_PACK_CHUNK;
        uint32_t len = to - from;
        uint32_t wr = (s->stage_rd + s->stage_len) % STAGE_BYTES;
        for (int ch = 0; ch < chans; ch++) {
            const uint8_t* src = bounce + (k * chans + ch) * AMB_PACK_CHUNK + from;
            uint32_t a = len < STAGE_BYTES - wr ? len : STAGE_BYTES - wr;
            memcpy(s->stage[ch] + wr, src, a);
            memcpy(s->stage[ch], src + a, len - a);
        }
        s->stage_len += len;
        s->src = chunk_at + to >= total ? 0 : chunk_at + to;
    }
    return true;
}

// The first data is in: the preamble and what the buffer holds go into the ring, and the loop keys on,
// muted until the preamble has played.
static void slot_begin(Slot* s, int slot) {
    Clip* c = &clips[s->clip];
    const AmbPackStart* start = NULL;
    for (int j = 0; j < AMB_PACK_STARTS; j++) {
        if (c->starts[j].sample == s->data_byte * 2) {
            start = &c->starts[j];
            break;
        }
    }
    int ch = FIRST_CH + 2 * slot;
    uint32_t r0[2] = { 0, 0 };
    for (int k = 0; k < c->info->channels; k++) {
        if (start != NULL && s->pre_bytes > 0) {
            static uint8_t pre[AMB_PACK_PRE / 2] __attribute__((aligned(32)));
            memcpy(pre, start->pre[k], sizeof(pre));
            spu_memload(s->ring[k], pre, (s->pre_bytes + 3) & ~3u);
        }
        uint32_t pan = c->info->channels == 2 ? (k == 0 ? 0x1F : 0x0F) : 0;
        r0[k] = chn_setup(ch + k, s->ring[k], 0, RING_SAMPLES, true, pan);
    }
    s->written = s->pre_bytes * 2;
    s->played = 0;
    s->last_pos = POS_UNKNOWN;
    s->tl_sent[0] = s->tl_sent[1] = 255;
    ring_fill(s, 0);
    key(ch, r0, c->info->channels, true);
    s->keyon_clock = port_fw ? aica_port_clock() : 0;
    s->keyon_us = timer_us_gettime64();
    s->state = SLOT_PLAYING;
}

static float buffered_s(const Slot* s, uint32_t played) {
    return ((float)(s->written - played) + (float)s->stage_len * 2.0f) / AMB_PACK_RATE;
}

// Tops up every playing loop's ring and reads from the disc for the one that needs it most.
static void service(uint64_t now) {
    int need = -1;
    float need_s = 1e9f;
    for (int i = 0; i < slot_count; i++) {
        Slot* s = &slots[i];
        if (s->state == SLOT_FREE) {
            continue;
        }
        float b = 0.0f;
        if (s->state == SLOT_PLAYING) {
            int ch = FIRST_CH + 2 * i;
            uint32_t played = slot_played(s, ch);
            if ((int32_t)(s->written - played) < RING_LOW) {
                // Dry: start again at the same place once data is in.
                st.underruns++;
                int ci = s->clip;
                loop_stop(ci);
                loop_start(ci);
                continue;
            }
            ring_fill(s, played);
            b = buffered_s(s, played);
        }
        if (STAGE_BYTES - s->stage_len >= AMB_PACK_CHUNK && b < need_s) {
            need = i;
            need_s = b;
        }
    }
    if (need < 0) {
        return;
    }
    Slot* s = &slots[need];
    if (s->state == SLOT_PLAYING && STAGE_BYTES - s->stage_len < READ_FREE * AMB_PACK_CHUNK) {
        return;
    }
    bool wait = s->state == SLOT_PLAYING ? need_s > URGENT_S : now - s->start_us < PRIME_WAIT_US;
    if (wait && rom_reader_busy()) {
        st.deferred++;
        return;
    }
    if (slot_read(s) && s->state == SLOT_PRIMING) {
        slot_begin(s, need);
    }
}

// --- The frame ------------------------------------------------------------------------------------

static void tick(uint64_t now, float dt) {
    bool alive = now - last_frame_us < WATCHDOG_US;
    float master = alive ? (float)master_pm / 1000.0f : 0.0f;
    float step = dt / FADE_S;
    float m = (float)muffle_pm / 1000.0f, mstep = dt / MUFFLE_S;
    muffle = muffle < m ? fminf(muffle + mstep, m) : fmaxf(muffle - mstep, m);

    for (int i = 0; i < clip_count; i++) {
        Clip* c = &clips[i];
        if (!c->info->loops) {
            if (c->shot < 0) {
                continue;
            }
            int ch = FIRST_CH + 2 * (slot_count + c->shot);
            int old = irq_disable();
            int32_t vol = c->play;
            c->play = 0;
            irq_restore(old);
            uint32_t r0[2] = { 2u << 7 | LPCTL | (c->aram[0] >> 16), 2u << 7 | LPCTL | (c->aram[1] >> 16) };
            if (vol > 0 && alive) {
                if (c->shot_end_us != 0) {
                    key(ch, r0, c->info->channels, false);
                }
                c->shot_amp = c->gain * master * (float)vol / 1000.0f;
                uint32_t silence = (c->info->audible + 63) & ~63u;
                silence = silence + 64 <= c->info->samples ? silence : c->info->samples - 64;
                for (int k = 0; k < c->info->channels; k++) {
                    uint32_t pan = c->info->channels == 2 ? (k == 0 ? 0x1F : 0x0F) : 0;
                    r0[k] = chn_setup(ch + k, c->aram[k], silence, c->info->samples, false, pan);
                    c->tl_sent[k] = 255;
                }
                key(ch, r0, c->info->channels, true);
                c->shot_end_us = now + (uint64_t)c->info->audible * 1000000u / AMB_PACK_RATE + 20000;
                st.shots++;
            }
            if (c->shot_end_us != 0) {
                if (now >= c->shot_end_us) {
                    key(ch, r0, c->info->channels, false);
                    c->shot_end_us = 0;
                    c->tl_sent[0] = c->tl_sent[1] = 255;
                    continue;
                }
                // The library's pan for a stereo clip is a balance: the far side gets quieter. A mono
                // clip plays in the middle.
                float p = (float)c->pan / 1000.0f;
                float amp = c->shot_amp * muffle_gain(c);
                for (int k = 0; k < c->info->channels; k++) {
                    float side = 1.0f;
                    if (c->info->channels == 2) {
                        side = k == 0 ? (p > 0.0f ? 1.0f - p : 1.0f) : (p < 0.0f ? 1.0f + p : 1.0f);
                    }
                    set_tl(ch + k, tl_of(amp * side), &c->tl_sent[k]);
                }
            }
            continue;
        }

        float target = master * (float)c->target / 1000.0f;
        c->level = c->level < target ? fminf(c->level + step, target) : fmaxf(c->level - step, target);
        if (c->level > 0.0f && c->slot < 0) {
            loop_start(i);
        } else if (c->level <= 0.0f && c->slot >= 0) {
            loop_stop(i);
        }
        if (c->slot >= 0 && slots[c->slot].state == SLOT_PLAYING) {
            Slot* s = &slots[c->slot];
            uint8_t tl = now - s->keyon_us < PREAMBLE_MUTE_US ? 255 : tl_of(c->gain * c->level * c->level * muffle_gain(c));
            int ch = FIRST_CH + 2 * c->slot;
            for (int k = 0; k < c->info->channels; k++) {
                set_tl(ch + k, tl, &s->tl_sent[k]);
            }
        }
    }
}

static void print_state(void) {
    char line[256];
    int len = snprintf(line, sizeof(line), "[q64dc] ambience: master %d, muffle %.2f |", (int)master_pm, (double)muffle);
    for (int i = 0; i < clip_count && len < (int)sizeof(line) - 40; i++) {
        const Clip* c = &clips[i];
        if (c->info->loops && (c->level > 0.0f || c->target > 0)) {
            len += snprintf(line + len, sizeof(line) - len, " %d:%.2f%s", i, (double)c->level,
                            c->slot < 0 ? "" : slots[c->slot].state == SLOT_PLAYING ? "" : "(starting)");
        } else if (!c->info->loops && c->shot_end_us != 0) {
            len += snprintf(line + len, sizeof(line) - len, " %d:shot", i);
        }
    }
    printf("%s | %u reads, %u KB, %u ms (longest %u), %u deferred; %u starts, %u stops, %u one-shots, %u dry\n", line,
           (unsigned)st.reads, (unsigned)(st.read_bytes / 1024), (unsigned)(st.read_us / 1000),
           (unsigned)(st.read_max_us / 1000), (unsigned)st.deferred, (unsigned)st.starts, (unsigned)st.stops,
           (unsigned)st.shots, (unsigned)st.underruns);
    memset(&st, 0, sizeof(st));
}

static void* ambience_thread(void* arg) {
    (void)arg;
    fpu_thread_init();
    if (!setup()) {
        return NULL;
    }
    uint64_t last = timer_us_gettime64(), last_print = last;
    bool was_quiet = true;
    for (;;) {
        thd_sleep(TICK_MS);
        uint64_t now = timer_us_gettime64();
        tick(now, (float)(now - last) / 1000000.0f);
        service(now);
        last = now;
        if (test_disc() && now - last_print >= 1000000) {
            bool quiet = st.reads == 0 && st.starts == 0 && st.stops == 0 && st.shots == 0;
            for (int i = 0; i < clip_count && quiet; i++) {
                quiet = clips[i].level == 0.0f && clips[i].shot_end_us == 0;
            }
            if (!quiet || !was_quiet) {
                print_state();
            }
            was_quiet = quiet;
            last_print = now;
        }
    }
    return NULL;
}

// --- The library's functions ----------------------------------------------------------------------

static void start_once(void) {
    if (started) {
        return;
    }
    started = true;
    kthread_attr_t attr = { 0 };
    attr.stack_size = 16 * 1024;
    attr.prio = PRIO;
    attr.label = "ambience";
    attr.create_detached = true;
    if (thd_create_ex(&attr, ambience_thread, NULL) == NULL) {
        printf("[q64dc] ambience: couldn't create its thread\n");
    }
}

// ambience_frame(u32 master, u32 muffle): once a frame, both per mille.
MODRT_IMPORT(native_ambience_frame) {
    (void)rdram;
    (void)mod;
    master_pm = per_mille(ctx->r4);
    int32_t m = per_mille(ctx->r5);
    muffle_pm = m > 1000 ? 1000 : m;
    last_frame_us = timer_us_gettime64();
    start_once();
}

// ambience_set(u32 clip, u32 volume): the level a loop fades to, per mille.
MODRT_IMPORT(native_ambience_set) {
    (void)rdram;
    (void)mod;
    uint32_t i = ctx->r4;
    if (i < AMB_PACK_MAX) {
        clips[i].target = per_mille(ctx->r5);
    }
}

// ambience_play(u32 clip, u32 volume, s32 pan): starts a one-shot from the top, pan -1000 to 1000.
MODRT_IMPORT(native_ambience_play) {
    (void)rdram;
    (void)mod;
    uint32_t i = ctx->r4;
    int32_t pan = (int32_t)ctx->r6;
    if (i < AMB_PACK_MAX) {
        clips[i].pan = pan < -1000 ? -1000 : pan > 1000 ? 1000 : pan;
        clips[i].play = per_mille(ctx->r5);
    }
}
