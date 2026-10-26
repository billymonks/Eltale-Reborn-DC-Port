// Audio: the N64's audio interface. The synthesizer's voices play on the AICA's channels (aica.c);
// with -DAUDIO_HLE, or when the AICA doesn't answer at boot, the game's audio tasks are mixed on
// the SH-4 instead (audio_hle.c) and played through a KOS sound stream, as below.
//
// Every buffer the game hands to osAiSetNextBuffer is copied into a ring that a high-priority
// thread feeds to the stream, so the AICA's clock sets the pace. Each retrace the game's audio
// thread (func_80024BEC) asks osAiGetLength how much audio is still queued and sizes its next
// buffer as (frame size - queued + 0x60) & ~0xF. On the N64 that keeps the AI fed because the
// thread runs on time every retrace. Here game threads only switch at runtime calls, so the
// audio thread runs late and in bursts, and a queue drained on the CPU clock would lose time in
// the gaps. Instead osAiGetLength reports whatever makes that formula ask for what the AICA will
// need: a retrace's worth of samples, plus a share of the distance from the ring's target fill.

#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <stdio.h>

#include "aica.h"
#include "audio_hle.h"
#include "ultra.h"

#ifndef AUDIO_HLE
static bool use_aica;
#else
#define use_aica false
#endif

// Per channel, in bytes. The stream asks for half of it at a time.
#define STREAM_BUFFER_BYTES 2048
#define RING_FRAMES         8192
#define RING_TARGET_FRAMES  2048
// The ring's distance from its target is made up over this many retraces.
#define CATCH_UP_RETRACES   4

#define AUDIO_KOS_PRIO (PRIO_DEFAULT - 6)

// The VI clock the AI's DAC divides, and the retrace rate (NTSC, in mHz).
#define VI_CLOCK      48681812
#define RETRACE_MHZ   59940

static uint32_t ring[RING_FRAMES];  // stereo frames, left in the low half
static volatile uint32_t ring_write, ring_read;

static snd_stream_hnd_t stream = SND_STREAM_INVALID;
static uint32_t stream_freq;
static mutex_t stream_lock = MUTEX_INITIALIZER;

static uint32_t ai_freq = 22047;
static uint32_t ai_frame_size = 368;  // the game's: a retrace's worth, rounded up to 16

static uint32_t stat_tasks, stat_task_us, stat_task_max_us;
static uint32_t stat_underrun_frames, stat_dropped_frames;
static uint32_t stat_submitted_frames;
static volatile uint32_t stat_streamed_frames;

static uint32_t out_buf[STREAM_BUFFER_BYTES / 2] __attribute__((aligned(32)));

static void* stream_cb(snd_stream_hnd_t hnd, int bytes, int* got) {
    (void)hnd;
    uint32_t want = (uint32_t)bytes / 4;
    if (want > sizeof(out_buf) / 4) {
        want = sizeof(out_buf) / 4;
    }
    uint32_t r = ring_read;
    uint32_t avail = ring_write - r;
    uint32_t n = avail < want ? avail : want;
    for (uint32_t i = 0; i < n; i++) {
        out_buf[i] = ring[(r + i) & (RING_FRAMES - 1)];
    }
    // A short ring plays as silence rather than stalling the stream, so it keeps its timing.
    for (uint32_t i = n; i < want; i++) {
        out_buf[i] = 0;
    }
    if (n < want && ring_write != 0) {
        stat_underrun_frames += want - n;
    }
    ring_read = r + n;
    stat_streamed_frames += want;
    *got = (int)(want * 4);
    return out_buf;
}

static void* stream_thread(void* param) {
    (void)param;
    for (;;) {
        mutex_lock(&stream_lock);
        if (stream != SND_STREAM_INVALID && stream_freq != 0) {
            snd_stream_poll(stream);
        }
        mutex_unlock(&stream_lock);
        thd_sleep(4);
    }
    return NULL;
}

void audio_init(void) {
#ifndef AUDIO_HLE
    if (aica_init()) {
        use_aica = true;
        return;
    }
    printf("[q64dc] audio: mixing on the SH-4 instead\n");
#endif
    if (snd_stream_init() < 0) {
        printf("[q64dc] audio: couldn't start the sound stream; the game will be silent\n");
        return;
    }
    stream = snd_stream_alloc(stream_cb, STREAM_BUFFER_BYTES);
    if (stream == SND_STREAM_INVALID) {
        printf("[q64dc] audio: couldn't allocate a sound stream; the game will be silent\n");
        return;
    }

    kthread_attr_t attr = {0};
    attr.stack_size = 8 * 1024;
    attr.prio = AUDIO_KOS_PRIO;
    attr.label = "audio stream";
    attr.create_detached = true;
    if (thd_create_ex(&attr, stream_thread, NULL) == NULL) {
        runtime_fatal("[q64dc] couldn't create the audio stream thread");
    }
}

static void start_stream(uint32_t freq) {
    mutex_lock(&stream_lock);
    if (stream != SND_STREAM_INVALID && freq != stream_freq) {
        if (stream_freq != 0) {
            snd_stream_stop(stream);
        }
        stream_freq = freq;
        snd_stream_start(stream, freq, 1);
        snd_stream_volume(stream, 240);
    }
    mutex_unlock(&stream_lock);
}

bool audio_aica(void) {
    return use_aica;
}

void audio_run_task(uint32_t task) {
    if (use_aica) {
        // The command list is empty: the voices went to the AICA in alAudioFrame.
        return;
    }
    uint64_t t0 = timer_us_gettime64();
#ifndef AUDIO_OFF  // for measuring the frame rate without audio
    audio_hle_run(rdram, RDRAM_SIZE, task);
#else
    (void)task;
#endif
    uint32_t us = (uint32_t)(timer_us_gettime64() - t0);
    stat_tasks++;
    stat_task_us += us;
    if (us > stat_task_max_us) {
        stat_task_max_us = us;
    }
}

void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx) {
    // As libultra rounds it: the DAC divides the VI clock by an integer.
    uint32_t freq = ctx->r4;
#ifdef AUDIO_RATE
    // The game stores this return value as the synthesizer output rate (funcs_6, sw $v0, 0x18).
    // A lower rate shortens every mix. Pitch follows the rate, so tempo stays put.
    freq = AUDIO_RATE;
#endif
    uint32_t dac_rate = freq ? (uint32_t)((float)VI_CLOCK / freq + 0.5f) : 0;
    if (dac_rate < 132) {
        ctx->r2 = (gpr)-1;
        return;
    }
    ai_freq = VI_CLOCK / dac_rate;
    ai_frame_size = (ai_freq / 60 + 15) & ~15u;
    printf("[q64dc] audio: AI at %u Hz (asked for %u)\n", (unsigned)ai_freq, (unsigned)freq);
    if (!use_aica) {
        start_stream(ai_freq);
    }
    ctx->r2 = (gpr)ai_freq;
}

void osAiGetLength_recomp(uint8_t* rdram, recomp_context* ctx) {
#ifdef LOCKSTEP
    // The game's buffer sizes can't follow the AICA's clock, the stream's or aica.c's virtual AI;
    // see ultra.h.
    int32_t fill = RING_TARGET_FRAMES;
#else
    if (use_aica) {
        ctx->r2 = (gpr)aica_ai_length(ai_frame_size);
        return;
    }
    int32_t fill = (int32_t)(ring_write - ring_read);
#endif
    int32_t want = (int32_t)(ai_freq * 1000u / RETRACE_MHZ) + (RING_TARGET_FRAMES - fill) / CATCH_UP_RETRACES;
    // The game rounds down to 16 samples and won't go below a frame less 16.
    int32_t left = (int32_t)ai_frame_size + 0x60 - (want + 8);
    ctx->r2 = (gpr)(left > 0 ? left * 4 : 0);
}

void osAiSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t addr = ctx->r4;
    uint32_t frames = ctx->r5 / 4;
    stat_submitted_frames += frames;
    if (use_aica) {
        aica_ai_buffer(frames);
        ctx->r2 = 0;
        return;
    }

    // rdram's 32-bit words are left:right; the stream wants them the other way round.
    uint32_t w = ring_write;
    uint32_t space = RING_FRAMES - (w - ring_read);
    uint32_t n = frames < space ? frames : space;
    const uint32_t* src = (const uint32_t*)(rdram + ((addr & 0x1FFFFFFF) & ~3u));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = src[i];
        ring[(w + i) & (RING_FRAMES - 1)] = (v >> 16) | (v << 16);
    }
    stat_dropped_frames += frames - n;
    __asm__ volatile("" ::: "memory");
    ring_write = w + n;
    ctx->r2 = 0;
}

uint32_t audio_take_task_us(uint32_t* ntasks) {
    if (use_aica) {
        return aica_take_frame_us(ntasks);
    }
    uint32_t u = stat_task_us;
    if (ntasks) {
        *ntasks = stat_tasks;
    }
    stat_task_us = 0;
    stat_tasks = 0;
    stat_task_max_us = 0;
    return u;
}

void audio_print_stats(void) {
    if (use_aica) {
        aica_print_stats();
        return;
    }
    // The stream thread can preempt this one, so its count is only ever read.
    static uint32_t last_streamed;
    uint32_t streamed = stat_streamed_frames;
    uint32_t n = stat_tasks ? stat_tasks : 1;
    char ops[384];
    ops[0] = 0;
#ifdef AUDIO_PROFILE
    uint32_t counts[16], us[16];
    audio_hle_take_counts(counts, us);
    static const char* const names[16] = {"noop", "adpcm", "clear", "envmix", "load", "resample",
                                          "save", "segment", "setbuf", "setvol", "move", "book",
                                          "mix", "interleave", "polef", "setloop"};
    int len = snprintf(ops, sizeof(ops), " |");
    for (int i = 0; i < 16 && len < (int)sizeof(ops) - 32; i++) {
        if (counts[i] == 0) {
            continue;
        }
        len += snprintf(ops + len, sizeof(ops) - len, " %s %u", names[i], (unsigned)counts[i]);
        if (us[i] != 0) {
            len += snprintf(ops + len, sizeof(ops) - len, " (%u us)", (unsigned)us[i]);
        }
    }
#endif
    int16_t wet_max = 0;
    uint32_t wet = audio_hle_take_wet(&wet_max);
    printf("[q64dc] audio: %u tasks, %u us each (max %u), %u us/s; frames in %u out %u, ring %d; "
           "underrun %u, dropped %u; wet sends %u (max %d)%s\n",
           (unsigned)stat_tasks, (unsigned)(stat_task_us / n), (unsigned)stat_task_max_us,
           (unsigned)stat_task_us, (unsigned)stat_submitted_frames, (unsigned)(streamed - last_streamed),
           (int)(ring_write - ring_read), (unsigned)stat_underrun_frames,
           (unsigned)stat_dropped_frames, (unsigned)wet, (int)wet_max, ops);
#ifdef AUDIO_PROFILE
    extern uint32_t env_blocks_stepped, env_blocks_settled;
    printf("[q64dc] audio: envelope blocks stepped %u settled %u\n", (unsigned)env_blocks_stepped,
           (unsigned)env_blocks_settled);
    env_blocks_stepped = env_blocks_settled = 0;
#endif
    last_streamed = streamed;
    stat_tasks = stat_task_us = stat_task_max_us = 0;
    stat_underrun_frames = stat_dropped_frames = 0;
    stat_submitted_frames = 0;
}
