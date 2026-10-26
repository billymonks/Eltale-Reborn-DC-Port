// The game's synthesizer voices on the AICA's own channels.
//
// libultra's alAudioFrame (func_80036A68) runs the sequence and sound players' handlers, which post
// parameter updates (ALParam) on each physical voice's envelope mixer, and then turns those
// updates into an RSP command list. A hook after the handlers (config/quest64.us.toml, before
// 0x80036C20) takes the updates off the lists instead and plays each voice on a pair of AICA
// channels, one panned hard left and one hard right, and leaves the command list empty. Voice
// allocation and stealing, the parameter pool and both players stay the game's own code.
//
// The 75 samples are decoded once at boot to 16-bit PCM in sound RAM, with aspMain's ADPCM math.
// The loop states stored in the banks equal what that decode reaches on its first pass, so the
// AICA's own loop plays what the N64 did.
//
// Levels follow libultra's envelope mixer (env.c): the voice volume squared, an equal-power pan
// and dry level from eqpower[], and ramps that multiply the gain by a constant, which are straight
// lines in log2 of the gain. Pitch is the resampler's ratio times the output rate.
//
// Timing: the game asks osAiGetLength how much audio is still queued and synthesizes a retrace's
// worth less that. Here a virtual AI on the SH-4's clock answers, so the synthesizer's sample clock
// keeps pace with real time, and every update reaches the AICA stamped a lead ahead of real time.
// The AICA's ARM applies it then, however late and bunched up the game's audio thread runs.
//
// The ARM runs KallistiOS's stock firmware: channel commands in its queue, stamped with its 4410 Hz
// clock. It has no volume ramps, so a ramp is sent as a step every STEP synthesizer samples.
// With -DAICA_FW it runs the port's own firmware instead (runtime/aica/): events stamped with
// the AICA's sample clock, and the ramps walked on the ARM, so a ramp is one event.

#include <kos.h>
#include <dc/g2bus.h>
#include <dc/sound/aica_comm.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "aica.h"
#include "ultra.h"

#ifdef AICA_FW
#include "../aica/shared.h"
// runtime/aica/, as bin2c writes it.
extern const int aica_fw_size;
extern const unsigned char aica_fw_data[];
#define FW_SHARED (SPU_RAM_UNCACHED_BASE + Q64_AICA_SHARED)
#define FW_RING   (SPU_RAM_UNCACHED_BASE + Q64_AICA_RING)
#endif

// How far ahead of real time the synthesizer's output is played. The game's audio thread runs
// only when another game thread blocks, so it can be this late without moving a note. A slip
// raises it by LEAD_STEP_US, up to MAX_LEAD_US; ten seconds without one lowers it again. A
// thread that was held up longer than LONG_STALL_US (a map load) slips without raising it.
#ifndef AICA_LEAD_US
#define AICA_LEAD_US 60000
#endif
#define MAX_LEAD_US   160000
#define LEAD_STEP_US  20000
#define LONG_STALL_US 120000
// Below this, a frame's notes would play late: the time line slips forward instead, which is what
// an AI underrun does on the N64.
#define MIN_LEAD_US  4000
#define RETRACE_US   16683

// Synthesizer samples between the volume steps of a ramp.
#define STEP 256

// Gain of the whole mix, in dB. 0 puts a voice at the level the N64's mix gave it.
#ifndef AICA_GAIN_DB
#define AICA_GAIN_DB 0.0f
#endif

#define VOICES      16      // the game's physical voices (alSynNew, maxPVoices)
#define MAX_WAVES   96
#define MAX_PACKETS 512

// --- The game's synthesizer, as libultra 2.0 lays it out ---------------------------------------

#define AL_GLOBALS 0x80070990u  // ALGlobals* alGlobals; the ALSynth is its first member

#define SYN_PLAMELIST  0x14
#define SYN_CURSAMPLES 0x20
#define SYN_PARAMLIST  0x2C
#define SYN_AUXBUS     0x34
#define SYN_NUMPVOICES 0x3C
#define SYN_OUTPUTRATE 0x44

#define AUX_SOURCECOUNT 0x14
#define AUX_SOURCES     0x1C

#define PV_ENVMIXER 0x8C  // PVoice: node, vvoice, channelKnob, decoder, resampler, envmixer, offset
#define PV_OFFSET   0xD8

#define ENV_CTRLLIST 0x3C
#define ENV_CTRLTAIL 0x40

// ALParam, and the start updates that share its first fields.
#define P_NEXT     0x00
#define P_DELTA    0x04
#define P_TYPE     0x08
#define P_UNITY    0x0A
#define P_DATA     0x0C
#define P_MOREDATA 0x10
#define PA_PITCH   0x0C
#define PA_VOLUME  0x10
#define PA_PAN     0x12
#define PA_FXMIX   0x13
#define PA_SAMPLES 0x14
#define PA_WAVE    0x18
#define PS_WAVE    0x0C
#define PF_PVOICE  0x0C

enum {
    AL_FILTER_FREE_VOICE = 0,
    AL_FILTER_SET_PITCH = 7,
    AL_FILTER_SET_UNITY_PITCH = 8,
    AL_FILTER_SET_VOLUME = 11,
    AL_FILTER_SET_PAN = 12,
    AL_FILTER_START_VOICE_ALT = 13,
    AL_FILTER_START_VOICE = 14,
    AL_FILTER_STOP_VOICE = 15,
    AL_FILTER_SET_FXAMT = 16,
};

#define MAX_RATIO 1.99996f
#define UNITY_PITCH 32768.0f

// env.c's equal-power curve, for pan and the dry/wet split.
static const int16_t eqpower[128] = {
    32767, 32764, 32757, 32744, 32727, 32704, 32677, 32644, 32607, 32564, 32517, 32464, 32407,
    32344, 32277, 32205, 32127, 32045, 31958, 31866, 31770, 31668, 31561, 31450, 31334, 31213,
    31087, 30957, 30822, 30682, 30537, 30388, 30234, 30075, 29912, 29744, 29572, 29395, 29214,
    29028, 28838, 28643, 28444, 28241, 28033, 27821, 27605, 27385, 27160, 26931, 26698, 26461,
    26220, 25975, 25726, 25473, 25216, 24956, 24691, 24423, 24151, 23875, 23596, 23313, 23026,
    22736, 22442, 22145, 21845, 21541, 21234, 20924, 20610, 20294, 19974, 19651, 19325, 18997,
    18665, 18331, 17993, 17653, 17310, 16965, 16617, 16266, 15913, 15558, 15200, 14840, 14477,
    14113, 13746, 13377, 13006, 12633, 12258, 11881, 11503, 11122, 10740, 10357, 9971,  9584,
    9196,  8806,  8415,  8023,  7630,  7235,  6839,  6442,  6044,  5646,  5246,  4845,  4444,
    4042,  3640,  3237,  2833,  2429,  2025,  1620,  1216,  810,   405,   0,
};

// --- Quest 64's banks -----------------------------------------------------------------------------

// ctl (the bank file) and tbl (the samples) in the ROM: the instruments and the sound effects.
static const struct {
    uint32_t ctl, ctl_end, tbl;
} banks[] = {
    { 0x00E7E800u, 0x00E81610u, 0x00E81610u },
    { 0x00EEE020u, 0x00EEFCE0u, 0x00EEFCE0u },
};

typedef struct {
    uint32_t rom;          // ALWaveTable.base once alBnkfNew has added the tbl's ROM address
    uint32_t aram;         // the PCM copy in sound RAM
    uint32_t samples;
    uint32_t loop_start, loop_end;  // loop_end 0: no loop
} Wave;

static Wave waves[MAX_WAVES];
static int wave_count;

// --- Voices ---------------------------------------------------------------------------------------

#define LV_SILENT (-30.0f)

typedef struct {
    // libultra's envelope mixer.
    int32_t volume;         // the player's volume squared, Q15
    int32_t pan;            // 0 left .. 127 right
    int32_t dry;            // eqpower[fxmix]
    int32_t seg_end;        // the segment length a pan or fx change ramps over
    float ratio;            // the resampler's pitch
    bool unity;
    bool playing;
    const Wave* wave;
    // Each side's level, log2 of the linear gain, ramping from lv0 at t0 to lv1 at t1.
    float lv0[2], lv1[2];
    int32_t t0, t1;
    // The AICA pair.
    bool keyed;
    int32_t next_step;      // synthesizer time of the ramp's next step
    uint8_t vol_sent[2];
    uint32_t freq_sent;
    // Updates seen at the current time stamp.
    bool g_start, g_stop, g_pitch, g_level;
} Voice;

static Voice voices[VOICES];
static uint32_t pvoices[VOICES];  // each Voice's PVoice in rdram
static int pvoice_count;

// --- State ----------------------------------------------------------------------------------------

static bool active;
static uint32_t out_rate = 22047;
static float gain_lin = 1.0f;

// Synthesizer sample anchor_s plays at SH-4 time anchor_us.
static bool anchored;
static int32_t anchor_s;
static uint64_t anchor_us;
static int32_t synth_end;
static int32_t lead_us = AICA_LEAD_US;
static uint32_t frames_since_slip;

// The firmware's clock and its rate: KallistiOS's 4410 Hz AICA_MEM_CLOCK, measured at boot, or the
// port firmware's sample clock.
#ifdef AICA_FW
static float jiffies_per_us = 0.0441f;
static uint32_t fw_head;
static uint32_t aram_next = Q64_AICA_SAMPLES;
#else
static float jiffies_per_us = 0.00441f;
#endif
static uint32_t last_ts;

// This frame: synthesizer time f0 is j_lead jiffies after j_now, and each sample jps_sample more.
static int32_t f0, f1;
static float j_lead, jps_sample;
static uint32_t j_now;

enum { PK_START, PK_SYNC, PK_STOP, PK_UPDATE };

typedef struct {
    uint32_t ts;
    uint16_t order;
    uint8_t kind;
    uint8_t ch;
    uint32_t a, b, c;
} Packet;

static Packet packets[MAX_PACKETS];
static uint16_t packet_order[MAX_PACKETS];
static int packet_count;

// KallistiOS's command queue and clock in sound RAM (kernel/arch/dreamcast/sound/arm/aica_cmd_iface.h).
#define Q_BASE     (SPU_RAM_UNCACHED_BASE + 0x010000u)
#define Q_HEAD     0
#define Q_TAIL     4
#define Q_SIZE     8
#define Q_VALID    12
#define Q_PROC_OK  16
#define Q_DATA     20
#define CLOCK_ADDR (SPU_RAM_UNCACHED_BASE + 0x021000u)
#define PKT_WORDS  18  // aica_cmd_t and the aica_channel_t fields up to pan
static uint32_t q_data, q_size;

static struct {
    uint32_t frames, frame_us, frame_max_us;
    uint32_t packets, dropped, late, slips;
    uint32_t keyons, keyoffs, unknown_waves, wet_sends;
    uint32_t q_peak;
    int32_t lead_min_us, lead_sum_ms;
    uint32_t ai_frames;
} st;
static uint32_t hud_frames, hud_us, total_slips;

// --- Small helpers --------------------------------------------------------------------------------

static inline float bits_float(uint32_t u) {
    union { uint32_t u; float f; } v = { u };
    return v.f;
}

static inline int16_t clamp16(int32_t v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

// log2 and 2^x, accurate to about 1e-4, enough for a level in 1/16 dB.
static float log2_fast(float x) {
    if (!(x > 0.0f)) {
        return LV_SILENT;
    }
    union { float f; uint32_t u; } v = { x };
    int e = (int)((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
    float m = v.f;
    float p = -1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m;
    return p + (float)e;
}

static float exp2_fast(float x) {
    if (x < -30.0f) {
        return 0.0f;
    }
    int i = (int)x;
    if ((float)i > x) {
        i--;
    }
    float f = x - (float)i;
    float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550410f + f * (0.00961813f + f * 0.00133336f))));
    union { float f; uint32_t u; } v = { p };
    v.u += (uint32_t)i << 23;
    return v.f;
}

// --- Samples --------------------------------------------------------------------------------------

static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline int16_t be16(const uint8_t* p) {
    return (int16_t)(((uint32_t)p[0] << 8) | p[1]);
}

// Order-2 VADPCM, as aspMain decodes it (audio_hle.c's cmd_adpcm): 9-byte frames of a header and
// 16 residuals, predicted from the last two outputs.
static void vadpcm_decode(const uint8_t* in, uint32_t frames, const int16_t* book, int16_t* out) {
    int32_t l1 = 0, l2 = 0;
    for (uint32_t f = 0; f < frames; f++, in += 9, out += 16) {
        int shift = in[0] >> 4;
        if (shift > 12) {
            shift = 12;
        }
        const int16_t* b1 = book + (in[0] & 0xF) * 16;
        const int16_t* b2 = b1 + 8;
        int32_t res[16];
        for (int i = 0; i < 8; i++) {
            uint8_t b = in[1 + i];
            res[2 * i] = ((int32_t)(int8_t)b >> 4) << shift;
            res[2 * i + 1] = ((int32_t)(int8_t)(uint8_t)(b << 4) >> 4) << shift;
        }
        for (int half = 0; half < 16; half += 8) {
            const int32_t* r = res + half;
            for (int i = 0; i < 8; i++) {
                int32_t acc = (r[i] << 11) + b1[i] * l1 + b2[i] * l2;
                for (int j = 0; j < i; j++) {
                    acc += b2[i - 1 - j] * r[j];
                }
                out[half + i] = clamp16(acc >> 11);
            }
            l1 = out[half + 6];
            l2 = out[half + 7];
        }
    }
}

static const Wave* find_wave(uint32_t rom) {
    int lo = 0, hi = wave_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (waves[mid].rom == rom) {
            return &waves[mid];
        }
        if (waves[mid].rom < rom) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static bool add_wave(const uint8_t* ctl, uint32_t ctl_size, uint32_t tbl, uint32_t wt, int16_t* pcm, uint8_t* adpcm,
                     uint32_t* bytes_total) {
    if (wt + 0x14 > ctl_size) {
        return false;
    }
    uint32_t rom = tbl + be32(ctl + wt);
    for (int i = 0; i < wave_count; i++) {
        if (waves[i].rom == rom) {
            return true;
        }
    }
    if (wave_count >= MAX_WAVES) {
        printf("[q64dc] aica: more than %d samples\n", MAX_WAVES);
        return false;
    }
    uint32_t len = be32(ctl + wt + 4);
    uint32_t loop = be32(ctl + wt + 0xC);
    uint32_t bookp = be32(ctl + wt + 0x10);
    if (ctl[wt + 8] != 0 || bookp + 8 > ctl_size || len == 0 || len > 0x8000) {
        printf("[q64dc] aica: sample at ROM 0x%06X isn't ADPCM (type %u, %u bytes)\n", (unsigned)rom, ctl[wt + 8],
               (unsigned)len);
        return false;
    }
    int32_t order = (int32_t)be32(ctl + bookp);
    int32_t npred = (int32_t)be32(ctl + bookp + 4);
    if (order != 2 || npred < 1 || npred > 16 || bookp + 8 + npred * 32 > ctl_size) {
        printf("[q64dc] aica: sample at ROM 0x%06X has an order %d book of %d\n", (unsigned)rom, (int)order, (int)npred);
        return false;
    }
    // Predictors past the book's are zero: none of the game's samples uses them.
    static int16_t book[16 * 16];
    memset(book, 0, sizeof(book));
    for (int i = 0; i < npred * 16; i++) {
        book[i] = be16(ctl + bookp + 8 + 2 * i);
    }

    uint32_t frames = len / 9;
    uint32_t n = frames * 16;
    if (n >= 0xFFFF) {
        printf("[q64dc] aica: sample at ROM 0x%06X is past the AICA's 64K samples\n", (unsigned)rom);
        return false;
    }
    rom_read_bytes(adpcm, rom, frames * 9);
    vadpcm_decode(adpcm, frames, book, pcm);

    Wave w = { rom, 0, n, 0, 0 };
    if (loop != 0 && loop + 12 <= ctl_size) {
        uint32_t start = be32(ctl + loop), end = be32(ctl + loop + 4);
        int32_t count = (int32_t)be32(ctl + loop + 8);
        if (count != 0 && end > start && end < n) {
            // Every loop in the game repeats forever (count -1).
            w.loop_start = start;
            w.loop_end = end;
        }
    }
    // The AICA interpolates towards the sample after the one it plays: past a loop's end that is
    // the loop's start, past the end of a one-shot silence.
    if (w.loop_end != 0) {
        pcm[w.loop_end] = pcm[w.loop_start];
    } else {
        pcm[n] = 0;
    }
    uint32_t bytes = ((n + 1) * 2 + 31) & ~31u;
#ifdef AICA_FW
    w.aram = aram_next + bytes <= 0x200000u ? aram_next : 0;
    aram_next += bytes;
#else
    w.aram = snd_mem_malloc(bytes);
#endif
    if (w.aram == 0) {
        printf("[q64dc] aica: no sound RAM for %u bytes\n", (unsigned)bytes);
        return false;
    }
    spu_memload_sq(w.aram, pcm, bytes);
    *bytes_total += bytes;

    int at = wave_count++;
    while (at > 0 && waves[at - 1].rom > rom) {
        waves[at] = waves[at - 1];
        at--;
    }
    waves[at] = w;
    return true;
}

// Every wavetable each bank's instruments use, decoded into sound RAM.
static bool load_waves(void) {
    uint64_t t0 = timer_us_gettime64();
    uint32_t bytes_total = 0;
    // The longest sample is 14,364 bytes of ADPCM, 25,536 samples.
    int16_t* pcm = memalign(32, (0x8000 / 9 * 16 + 32) * 2);
    uint8_t* adpcm = malloc(0x8000);
    bool ok = pcm != NULL && adpcm != NULL;
    for (size_t b = 0; ok && b < sizeof(banks) / sizeof(banks[0]); b++) {
        uint32_t size = banks[b].ctl_end - banks[b].ctl;
        uint8_t* ctl = malloc(size);
        if (ctl == NULL) {
            ok = false;
            break;
        }
        rom_read_bytes(ctl, banks[b].ctl, size);
        uint32_t nbanks = (uint32_t)be16(ctl + 2);
        if (be16(ctl) != 0x4231 || nbanks == 0 || nbanks > 8) {
            printf("[q64dc] aica: no bank file at ROM 0x%06X\n", (unsigned)banks[b].ctl);
            ok = false;
        }
        for (uint32_t bi = 0; ok && bi < nbanks; bi++) {
            uint32_t bank = be32(ctl + 4 + 4 * bi);
            if (bank == 0 || bank + 0x10 > size) {
                continue;
            }
            uint32_t ninst = (uint16_t)be16(ctl + bank);
            uint32_t perc = be32(ctl + bank + 8);
            for (uint32_t ii = 0; ok && ii <= ninst; ii++) {
                uint32_t inst = ii < ninst ? be32(ctl + bank + 0xC + 4 * ii) : perc;
                if (inst == 0 || inst + 0x10 > size) {
                    continue;
                }
                uint32_t nsounds = (uint16_t)be16(ctl + inst + 0xE);
                for (uint32_t si = 0; ok && si < nsounds; si++) {
                    uint32_t sound = be32(ctl + inst + 0x10 + 4 * si);
                    if (sound == 0 || sound + 0x10 > size) {
                        continue;
                    }
                    ok = add_wave(ctl, size, banks[b].tbl, be32(ctl + sound + 8), pcm, adpcm, &bytes_total);
                }
            }
        }
        free(ctl);
    }
    free(pcm);
    free(adpcm);
    printf("[q64dc] aica: %d samples, %u KB of PCM in sound RAM, %u ms\n", wave_count, (unsigned)(bytes_total / 1024),
           (unsigned)((timer_us_gettime64() - t0) / 1000));
    return ok && wave_count > 0;
}

// --- The firmware ---------------------------------------------------------------------------------

// The boot ROM can leave its reverb running on the DSP, writing its delay line into sound RAM:
// mute the DSP's outputs and turn every step of its program into a no-op.
static void dsp_off(void) {
    for (int i = 0; i < 18; i++) {
        g2_write_32(0xA0700000u + 0x2000 + 4 * i, 0);
    }
    for (int i = 0; i < 128 * 4; i++) {
        g2_write_32(0xA0700000u + 0x3400 + 4 * i, 0);
    }
}

#ifdef AICA_FW
static inline uint32_t clock_now(void) {
    // The ARM writes the word whole, but the two halves can still be read across a write.
    uint32_t a = g2_read_32(FW_SHARED + offsetof(q64_shared, now));
    uint32_t b = g2_read_32(FW_SHARED + offsetof(q64_shared, now));
    return (int32_t)(b - a) >= 0 && b - a < 0x10000 ? b : g2_read_32(FW_SHARED + offsetof(q64_shared, now));
}

// Stops the ARM, clears the DSP's program, loads the firmware and waits for it to answer.
static bool firmware_start(void) {
    spu_disable();
    dsp_off();
    spu_memset_sq(0, 0, Q64_AICA_SAMPLES);
    uint32_t size = ((uint32_t)aica_fw_size + 31) & ~31u;
    uint8_t* fw = memalign(32, size);
    if (fw == NULL) {
        return false;
    }
    memset(fw, 0, size);
    memcpy(fw, aica_fw_data, (size_t)aica_fw_size);
    spu_memload(0, fw, size);
    free(fw);
    spu_enable();
    uint64_t start = timer_us_gettime64();
    uint32_t c0 = 0;
    bool seen = false;
    for (;;) {
        if (g2_read_32(FW_SHARED + offsetof(q64_shared, magic)) == Q64_AICA_MAGIC) {
            uint32_t c = clock_now();
            if (seen && c != c0) {
                break;
            }
            seen = true;
            c0 = c;
        }
        if (timer_us_gettime64() - start > 200000) {
            printf("[q64dc] aica: the port firmware didn't answer within 200 ms\n");
            spu_disable();
            return false;
        }
        thd_sleep(1);
    }
    fw_head = 0;
    printf("[q64dc] aica: port firmware running (%d bytes)\n", aica_fw_size);
    return true;
}
#else
static inline uint32_t clock_now(void) {
    return g2_read_32(CLOCK_ADDR);
}

static bool firmware_alive(void) {
    uint64_t start = timer_us_gettime64();
    uint32_t c0 = clock_now();
    for (;;) {
        uint64_t t = timer_us_gettime64();
        if (g2_read_32(Q_BASE + Q_VALID) == 1 && clock_now() != c0) {
            return true;
        }
        if (t - start > 200000) {
            return false;
        }
        thd_sleep(1);
    }
}

// KallistiOS's aica_freq turns Hz into octave and mantissa with a truncated base, and a rate just
// under twice that base plays an octave low (11024 Hz, 5512 Hz...). Stay under it.
static uint32_t kos_freq(float hz) {
    uint32_t freq = hz < 1.0f ? 1 : (uint32_t)(hz + 0.5f);
    uint32_t base = 5644800;
    int hi = 7;
    while (freq < base && hi > -8) {
        base >>= 1;
        hi--;
    }
    if (freq >= 2 * base) {
        freq = 2 * base - 1;
    }
    return freq;
}
#endif

static void add_packet(uint8_t kind, uint8_t ch, int32_t t, uint32_t a, uint32_t b, uint32_t c) {
    if (packet_count >= MAX_PACKETS) {
        st.dropped++;
        return;
    }
    float j = j_lead + (float)(t - f0) * jps_sample;
    uint32_t ts = j_now + (uint32_t)(int32_t)(j < 0.0f ? j - 0.5f : j + 0.5f);
    if (ts == 0) {
        ts = 1;
    }
    Packet* p = &packets[packet_count];
    p->ts = ts;
    p->order = (uint16_t)packet_count;
    p->kind = kind;
    p->ch = ch;
    p->a = a;
    p->b = b;
    p->c = c;
    packet_count++;
}

// This frame's packets in time order: the firmware stops at the first one that isn't due yet.
static void sort_packets(void) {
    int n = packet_count;
    for (int i = 0; i < n; i++) {
        packet_order[i] = (uint16_t)i;
    }
    for (int i = 1; i < n; i++) {
        uint16_t k = packet_order[i];
        int j = i - 1;
        while (j >= 0 && (int32_t)(packets[packet_order[j]].ts - packets[k].ts) > 0) {
            packet_order[j + 1] = packet_order[j];
            j--;
        }
        packet_order[j + 1] = k;
    }
}

#ifdef AICA_FW
// Writes this frame's events to the firmware's ring.
static void flush_packets(void) {
    int n = packet_count;
    if (n == 0) {
        return;
    }
    sort_packets();
    uint32_t tail = g2_read_32(FW_SHARED + offsetof(q64_shared, tail)) & (Q64_AICA_EVENTS - 1);
    uint32_t head = fw_head;
    for (int i = 0; i < n; i++) {
        const Packet* p = &packets[packet_order[i]];
        uint32_t next = (head + 1) & (Q64_AICA_EVENTS - 1);
        if (next == tail) {
            st.dropped += n - i;
            break;
        }
        uint32_t ts = (int32_t)(p->ts - last_ts) < 0 ? last_ts : p->ts;
        last_ts = ts;
        if ((int32_t)(ts - j_now) <= 0) {
            st.late++;
        }
        uint32_t w[4] = { ts, (uint32_t)p->kind | (uint32_t)p->ch << 8 | p->c << 16, p->a, p->b };
        uint32_t at = FW_RING + head * sizeof(q64_event);
        g2_ctx_t ctx = g2_lock();
        for (int k = 0; k < 4; k++) {
            g2_write_32_raw(at + 4 * k, w[k]);
        }
        g2_unlock(ctx);
        head = next;
        st.packets++;
    }
    uint32_t used = ((head - tail) & (Q64_AICA_EVENTS - 1)) * sizeof(q64_event);
    if (used > st.q_peak) {
        st.q_peak = used;
    }
    fw_head = head;
    g2_write_32(FW_SHARED + offsetof(q64_shared, head), head);
    packet_count = 0;
}
#else
// Writes this frame's packets to KallistiOS's queue.
static void flush_packets(void) {
    int n = packet_count;
    if (n == 0) {
        return;
    }
    sort_packets();

    uint32_t head = g2_read_32(Q_BASE + Q_HEAD);
    uint32_t tail = g2_read_32(Q_BASE + Q_TAIL);
    uint32_t used = (head + q_size - tail) % q_size;
    for (int i = 0; i < n; i++) {
        const Packet* p = &packets[packet_order[i]];
        if (used + PKT_WORDS * 4 + 4 >= q_size) {
            st.dropped += n - i;
            break;
        }
        uint32_t ts = (int32_t)(p->ts - last_ts) < 0 ? last_ts : p->ts;
        last_ts = ts;
        if ((int32_t)(ts - j_now) <= 0) {
            st.late++;
        }
        uint32_t w[PKT_WORDS];
        memset(w, 0, sizeof(w));
        w[0] = PKT_WORDS;
        w[1] = AICA_CMD_CHAN;
        w[2] = ts;
        w[3] = p->ch;
        switch (p->kind) {
            case PK_START: {
                const Wave* wv = (const Wave*)(uintptr_t)p->a;
                w[8] = AICA_CH_CMD_START | AICA_CH_START_DELAY;
                w[9] = wv->aram;
                w[10] = AICA_SM_16BIT;
                w[11] = wv->samples;
                w[12] = wv->loop_end != 0;
                w[13] = wv->loop_start;
                w[14] = wv->loop_end != 0 ? wv->loop_end : wv->samples;
                w[15] = p->b;
                w[16] = p->c & 0xFF;
                w[17] = p->c >> 8;
                break;
            }
            case PK_SYNC:
                w[3] = p->a;  // the channels, as a bit map
                w[8] = AICA_CH_CMD_START | AICA_CH_START_SYNC;
                break;
            case PK_STOP:
                w[8] = AICA_CH_CMD_STOP;
                break;
            default:
                w[8] = AICA_CH_CMD_UPDATE | p->a;
                w[15] = p->b;
                w[16] = p->c & 0xFF;
                break;
        }
        g2_ctx_t ctx = g2_lock();
        for (int k = 0; k < PKT_WORDS; k++) {
            if ((k & 7) == 0) {
                g2_fifo_wait();
            }
            g2_write_32_raw(q_data + head, w[k]);
            head += 4;
            if (head >= q_size) {
                head = 0;
            }
        }
        g2_unlock(ctx);
        used += PKT_WORDS * 4;
        st.packets++;
    }
    if (used > st.q_peak) {
        st.q_peak = used;
    }
    g2_write_32(Q_BASE + Q_HEAD, head);
    packet_count = 0;
}
#endif

// --- Voices ---------------------------------------------------------------------------------------

// The side's gain target as libultra's envelope mixer computes it, as log2 of the linear gain.
static float target_lv(const Voice* v, int side) {
    int32_t tgt = (v->volume * eqpower[side == 0 ? v->pan : 127 - v->pan]) >> 15;
    // _getRate never ramps below 1.
    if (tgt < 1) {
        tgt = 1;
    }
    return log2_fast((float)tgt * (float)v->dry * (1.0f / (32768.0f * 32768.0f)));
}

static float level_at(const Voice* v, int side, int32_t t) {
    if (t - v->t1 >= 0 || v->t1 == v->t0) {
        return v->lv1[side];
    }
    if (t - v->t0 <= 0) {
        return v->lv0[side];
    }
    return v->lv0[side] + (v->lv1[side] - v->lv0[side]) * (float)(t - v->t0) / (float)(v->t1 - v->t0);
}

#ifndef AICA_FW
static uint8_t kos_vol(float lv) {
    float g = exp2_fast(lv) * gain_lin * 255.0f;
    if (g >= 254.5f) {
        return 255;
    }
    return g < 0.5f ? 0 : (uint8_t)(g + 0.5f);
}
#endif

static void ramp(Voice* v, int32_t t, int32_t len) {
    for (int s = 0; s < 2; s++) {
        v->lv0[s] = level_at(v, s, t);
    }
    for (int s = 0; s < 2; s++) {
        v->lv1[s] = target_lv(v, s);
    }
    v->t0 = t;
    v->t1 = t + (len > 0 ? len : 0);
    if (len <= 0) {
        v->lv0[0] = v->lv1[0];
        v->lv0[1] = v->lv1[1];
    }
    v->next_step = t + STEP;
}

static float pitch_hz(const Voice* v) {
    float r = v->unity ? 1.0f : v->ratio;
    if (r > MAX_RATIO) {
        r = MAX_RATIO;
    }
    if (!(r > 0.0f)) {
        r = 0.0f;
    }
    // The resampler quantizes the ratio to its 1.15 increment.
    r = (float)(int32_t)(r * UNITY_PITCH) * (1.0f / UNITY_PITCH);
    return r * (float)out_rate;
}

#ifdef AICA_FW
// A level as the channel's TL in 8.8: 16 TL steps make a factor of 2 (6 dB).
static uint32_t tl88(float lv) {
    float tl = -(lv + AICA_GAIN_DB / 6.0206f) * 4096.0f;
    if (!(tl > 0.0f)) {
        return 0;
    }
    return tl >= 65280.0f ? 65280 : (uint32_t)(tl + 0.5f);
}

// OCT and FNS for a rate in Hz: the AICA plays 44100 * 2^OCT * (1 + FNS / 1024) samples a second.
static uint32_t pitch_reg(float hz) {
    float m = hz / 44100.0f;
    int oct = 0;
    if (!(m > 0.0f)) {
        return 8u << 11;
    }
    while (m < 1.0f && oct > -8) {
        m *= 2.0f;
        oct--;
    }
    while (m >= 2.0f && oct < 7) {
        m *= 0.5f;
        oct++;
    }
    uint32_t fns = m < 1.0f ? 0 : (uint32_t)((m - 1.0f) * 1024.0f + 0.5f);
    if (fns >= 1024) {
        if (oct < 7) {
            oct++;
            fns = 0;
        } else {
            fns = 1023;
        }
    }
    return ((uint32_t)oct & 0xF) << 11 | fns;
}

// The voice's current segment from time t: where it is now, where it ends, and over how many of
// the firmware's ticks.
static void send_ramp(int vi, int32_t t) {
    Voice* v = &voices[vi];
    uint32_t to0 = tl88(v->lv1[0]), to1 = tl88(v->lv1[1]);
    uint32_t from0 = to0, from1 = to1, ticks = 0;
    if (v->t1 - t > 0) {
        float samples = (float)(v->t1 - t) * 44100.0f / (float)out_rate;
        ticks = (uint32_t)(samples / Q64_AICA_TICK + 0.5f);
        ticks = ticks == 0 ? 1 : ticks > 0xFFFF ? 0xFFFF : ticks;
        from0 = tl88(level_at(v, 0, t));
        from1 = tl88(level_at(v, 1, t));
    }
    add_packet(Q64_EV_RAMP, (uint8_t)(vi * 2), t, from0 | from1 << 16, to0 | to1 << 16, ticks);
}

static void send_level(int vi, int32_t t) {
    send_ramp(vi, t);
}

static void key_on(int vi, int32_t t) {
    Voice* v = &voices[vi];
    const Wave* w = v->wave;
    uint32_t pitch = pitch_reg(pitch_hz(v));
    uint32_t loop = w->loop_end != 0;
    add_packet(Q64_EV_SETUP, (uint8_t)(vi * 2), t, w->aram | loop << 31,
               w->loop_start | (loop ? w->loop_end : w->samples) << 16, pitch);
    send_ramp(vi, t);
    add_packet(Q64_EV_KEYON, (uint8_t)(vi * 2), t, 0, 0, 0);
    v->freq_sent = pitch;
    v->keyed = true;
    st.keyons++;
}

static void key_off(int vi, int32_t t) {
    add_packet(Q64_EV_KEYOFF, (uint8_t)(vi * 2), t, 0, 0, 0);
    voices[vi].keyed = false;
    st.keyoffs++;
}

static void send_pitch(int vi, int32_t t) {
    Voice* v = &voices[vi];
    uint32_t pitch = pitch_reg(pitch_hz(v));
    if (pitch != v->freq_sent) {
        v->freq_sent = pitch;
        add_packet(Q64_EV_PITCH, (uint8_t)(vi * 2), t, 0, 0, pitch);
    }
}

// The firmware walks the ramps.
static void step_ramp(int vi, int32_t lim) {
    (void)vi;
    (void)lim;
}
#else
static void send_level(int vi, int32_t t) {
    Voice* v = &voices[vi];
    for (int s = 0; s < 2; s++) {
        uint8_t vol = kos_vol(level_at(v, s, t));
        if (vol != v->vol_sent[s]) {
            v->vol_sent[s] = vol;
            add_packet(PK_UPDATE, (uint8_t)(vi * 2 + s), t, AICA_CH_UPDATE_SET_VOL, 0, vol);
        }
    }
}

static void send_pitch(int vi, int32_t t) {
    Voice* v = &voices[vi];
    uint32_t freq = kos_freq(pitch_hz(v));
    if (freq != v->freq_sent) {
        v->freq_sent = freq;
        add_packet(PK_UPDATE, (uint8_t)(vi * 2), t, AICA_CH_UPDATE_SET_FREQ, freq, 0);
        add_packet(PK_UPDATE, (uint8_t)(vi * 2 + 1), t, AICA_CH_UPDATE_SET_FREQ, freq, 0);
    }
}

static void key_on(int vi, int32_t t) {
    Voice* v = &voices[vi];
    uint32_t freq = kos_freq(pitch_hz(v));
    for (int s = 0; s < 2; s++) {
        uint8_t vol = kos_vol(level_at(v, s, t));
        v->vol_sent[s] = vol;
        // KallistiOS's pan: 0 is hard left, 255 hard right.
        add_packet(PK_START, (uint8_t)(vi * 2 + s), t, (uint32_t)(uintptr_t)v->wave, freq,
                   vol | (s == 0 ? 0u : 255u << 8));
    }
    add_packet(PK_SYNC, 0, t, 3u << (vi * 2), 0, 0);
    v->freq_sent = freq;
    v->keyed = true;
    v->next_step = t + STEP;
    st.keyons++;
}

static void key_off(int vi, int32_t t) {
    Voice* v = &voices[vi];
    add_packet(PK_STOP, (uint8_t)(vi * 2), t, 0, 0, 0);
    add_packet(PK_STOP, (uint8_t)(vi * 2 + 1), t, 0, 0, 0);
    v->keyed = false;
    st.keyoffs++;
}

// The steps of a ramp up to (not including) synthesizer time lim.
static void step_ramp(int vi, int32_t lim) {
    Voice* v = &voices[vi];
    if (!v->keyed || v->t1 == v->t0) {
        return;
    }
    while (v->next_step - lim < 0) {
        int32_t t = v->next_step;
        if (t - v->t1 >= 0) {
            if (v->t1 - lim < 0) {
                send_level(vi, v->t1);
                v->t0 = v->t1;
            }
            return;
        }
        send_level(vi, t);
        v->next_step = t + STEP;
    }
}
#endif

// The updates that came in at one time stamp, sent as one change.
static void flush_group(int vi, int32_t t) {
    Voice* v = &voices[vi];
    if (v->g_start) {
        if (v->playing && v->wave != NULL) {
            key_on(vi, t);
        } else if (v->keyed) {
            key_off(vi, t);
        }
    } else if (v->g_stop) {
        if (v->keyed) {
            key_off(vi, t);
        }
    } else if (v->keyed) {
        if (v->g_pitch) {
            send_pitch(vi, t);
        }
        if (v->g_level) {
            send_level(vi, t);
        }
    }
    v->g_start = v->g_stop = v->g_pitch = v->g_level = false;
}

static void al_unlink(uint8_t* rdram, uint32_t ln) {
    uint32_t next = (uint32_t)MEM_W(0, ln), prev = (uint32_t)MEM_W(4, ln);
    if (next != 0) {
        MEM_W(4, next) = (int32_t)prev;
    }
    if (prev != 0) {
        MEM_W(0, prev) = (int32_t)next;
    }
}

static void al_link(uint8_t* rdram, uint32_t ln, uint32_t to) {
    uint32_t to_next = (uint32_t)MEM_W(0, to);
    MEM_W(0, ln) = (int32_t)to_next;
    MEM_W(4, ln) = (int32_t)to;
    if (to_next != 0) {
        MEM_W(4, to_next) = (int32_t)ln;
    }
    MEM_W(0, to) = (int32_t)ln;
}

static void set_wave(uint8_t* rdram, Voice* v, uint32_t table) {
    v->wave = NULL;
    if (table >= 0x80000000u && table < 0x80400000u) {
        v->wave = find_wave((uint32_t)MEM_W(0, table));
    }
    if (v->wave == NULL) {
        st.unknown_waves++;
    }
}

// One update off a voice's list, as alEnvmixerPull (env.c) applies it.
static void apply(uint8_t* rdram, uint32_t drvr, int vi, uint32_t p, int32_t t) {
    Voice* v = &voices[vi];
    switch (MEM_H(P_TYPE, p)) {
        case AL_FILTER_START_VOICE_ALT: {
            int32_t vol = MEM_H(PA_VOLUME, p);
            int32_t fx = MEM_BU(PA_FXMIX, p) & 0x7F;
            int32_t samples = MEM_W(PA_SAMPLES, p);
            v->unity = MEM_H(P_UNITY, p) != 0;
            set_wave(rdram, v, (uint32_t)MEM_W(PA_WAVE, p));
            v->playing = true;
            v->volume = (vol * vol) >> 15;
            v->pan = MEM_BU(PA_PAN, p) & 0x7F;
            v->dry = eqpower[fx];
            if (fx != 0) {
                st.wet_sends++;
            }
            v->ratio = bits_float((uint32_t)MEM_W(PA_PITCH, p));
            v->seg_end = samples;
            // With an attack the mixer starts from a volume of 1.
            for (int s = 0; s < 2; s++) {
                v->lv1[s] = target_lv(v, s);
                v->lv0[s] = samples > 0 ? log2_fast((float)v->dry * (1.0f / (32768.0f * 32768.0f))) : v->lv1[s];
            }
            v->t0 = t;
            v->t1 = t + (samples > 0 ? samples : 0);
            v->g_start = true;
            v->g_stop = false;
            break;
        }
        case AL_FILTER_START_VOICE:
            v->unity = MEM_H(P_UNITY, p) != 0;
            set_wave(rdram, v, (uint32_t)MEM_W(PS_WAVE, p));
            v->playing = true;
            // The mixer was reset by the last stop: its volume is 1 until a SET_VOLUME.
            for (int s = 0; s < 2; s++) {
                v->lv0[s] = v->lv1[s] = log2_fast((float)v->dry * (1.0f / (32768.0f * 32768.0f)));
            }
            v->t0 = v->t1 = t;
            v->g_start = true;
            v->g_stop = false;
            break;
        case AL_FILTER_STOP_VOICE:
            v->playing = false;
            v->volume = 1;
            v->g_stop = true;
            break;
        case AL_FILTER_SET_VOLUME: {
            int32_t vol = MEM_W(P_DATA, p);
            v->volume = (int16_t)((vol * vol) >> 15);
            v->seg_end = MEM_W(P_MOREDATA, p);
            ramp(v, t, v->seg_end);
            v->g_level = true;
            break;
        }
        case AL_FILTER_SET_PAN:
            v->pan = MEM_W(P_DATA, p) & 0x7F;
            ramp(v, t, v->seg_end);
            v->g_level = true;
            break;
        case AL_FILTER_SET_FXAMT: {
            int32_t fx = MEM_W(P_DATA, p) & 0x7F;
            v->dry = eqpower[fx];
            if (fx != 0) {
                st.wet_sends++;
            }
            ramp(v, t, v->seg_end);
            v->g_level = true;
            break;
        }
        case AL_FILTER_SET_PITCH:
            v->ratio = bits_float((uint32_t)MEM_W(P_DATA, p));
            v->g_pitch = true;
            break;
        case AL_FILTER_SET_UNITY_PITCH:
            v->unity = true;
            v->g_pitch = true;
            break;
        case AL_FILTER_FREE_VOICE: {
            // _freePVoice: the voice goes on the lame list, and alAudioFrame's _collectPVoices
            // frees it after this.
            uint32_t pv = (uint32_t)MEM_W(PF_PVOICE, p);
            MEM_W(PV_OFFSET, pv) = 0;
            al_unlink(rdram, pv);
            al_link(rdram, pv, drvr + SYN_PLAMELIST);
            break;
        }
        default:
            break;
    }
}

static void run_voice(uint8_t* rdram, uint32_t drvr, int vi) {
    uint32_t env = pvoices[vi] + PV_ENVMIXER;
    uint32_t p = (uint32_t)MEM_W(ENV_CTRLLIST, env);
    int32_t group_t = 0;
    bool grouped = false;
    while (p != 0) {
        int32_t delta = MEM_W(P_DELTA, p);
        // alEnvmixerPull takes updates up to the end of what it builds.
        if (delta - f1 > 0) {
            break;
        }
        int32_t t = delta - f0 < 0 ? f0 : delta;
        if (!grouped || t != group_t) {
            if (grouped) {
                flush_group(vi, group_t);
            }
            step_ramp(vi, t);
            group_t = t;
            grouped = true;
        }
        apply(rdram, drvr, vi, p, t);
        uint32_t next = (uint32_t)MEM_W(P_NEXT, p);
        // __freeParam
        MEM_W(P_NEXT, p) = MEM_W(SYN_PARAMLIST, drvr);
        MEM_W(SYN_PARAMLIST, drvr) = (int32_t)p;
        p = next;
    }
    MEM_W(ENV_CTRLLIST, env) = (int32_t)p;
    if (p == 0) {
        MEM_W(ENV_CTRLTAIL, env) = 0;
    }
    if (grouped) {
        flush_group(vi, group_t);
    }
    step_ramp(vi, f1);
}

static bool find_pvoices(uint8_t* rdram, uint32_t drvr) {
    uint32_t aux = (uint32_t)MEM_W(SYN_AUXBUS, drvr);
    if (aux < 0x80000000u || aux >= 0x80400000u) {
        return false;
    }
    int n = MEM_W(AUX_SOURCECOUNT, aux);
    uint32_t sources = (uint32_t)MEM_W(AUX_SOURCES, aux);
    if (n <= 0 || n > VOICES || n != MEM_W(SYN_NUMPVOICES, drvr)) {
        printf("[q64dc] aica: %d voices on the aux bus, %d in the synthesizer\n", n, (int)MEM_W(SYN_NUMPVOICES, drvr));
        if (n > VOICES) {
            n = VOICES;
        }
    }
    for (int i = 0; i < n; i++) {
        pvoices[i] = (uint32_t)MEM_W(4 * i, sources) - PV_ENVMIXER;
    }
    pvoice_count = n;
    return n > 0;
}

static inline uint64_t synth_us(int32_t s) {
    return anchor_us + (uint64_t)(((int64_t)(s - anchor_s) * 1000000) / (int64_t)out_rate);
}

void aica_audio_frame(uint8_t* rdram, recomp_context* ctx) {
    if (!active) {
        return;
    }
    uint64_t t_start = timer_us_gettime64();
    uint32_t drvr = ctx->r17;
    int32_t len = (int32_t)ctx->r19;
    if (len <= 0) {
        return;
    }
    // The command list is left empty: the game skips its build loop when the length is 0.
    ctx->r19 = 0;
    if (pvoice_count == 0 && !find_pvoices(rdram, drvr)) {
        MEM_W(SYN_CURSAMPLES, drvr) += len;
        return;
    }
#ifdef PERF_HUD
    // Out of the HUD's I-cache count, as the SH-4 mixer's audio tasks are (events.c).
    bool icache_counting = sched_icache_pause();
#endif
    uint32_t rate = (uint32_t)MEM_W(SYN_OUTPUTRATE, drvr);
    if (rate >= 8000 && rate <= 48000) {
        out_rate = rate;
    }
    f0 = MEM_W(SYN_CURSAMPLES, drvr);
    f1 = f0 + len;

    uint64_t now = timer_us_gettime64();
    j_now = clock_now();
    if (!anchored) {
        anchored = true;
        anchor_s = f0;
        anchor_us = now + lead_us - RETRACE_US;
    }
    int64_t lead = (int64_t)(synth_us(f0) - now);
    if (lead < MIN_LEAD_US) {
        if (lead > -LONG_STALL_US) {
            lead_us = lead_us + LEAD_STEP_US > MAX_LEAD_US ? MAX_LEAD_US : lead_us + LEAD_STEP_US;
        }
        anchor_s = f0;
        anchor_us = now + lead_us - RETRACE_US;
        lead = lead_us - RETRACE_US;
        frames_since_slip = 0;
        st.slips++;
        total_slips++;
    } else if (++frames_since_slip >= 600 && lead_us > AICA_LEAD_US) {
        lead_us = lead_us - LEAD_STEP_US / 2 < AICA_LEAD_US ? AICA_LEAD_US : lead_us - LEAD_STEP_US / 2;
        frames_since_slip = 0;
    }
    if (st.frames == 0 || lead < st.lead_min_us) {
        st.lead_min_us = (int32_t)lead;
    }
    st.lead_sum_ms += (int32_t)(lead / 1000);
    j_lead = (float)lead * jiffies_per_us;
    jps_sample = jiffies_per_us * 1000000.0f / (float)out_rate;

    for (int i = 0; i < pvoice_count; i++) {
        run_voice(rdram, drvr, i);
    }
    flush_packets();

    MEM_W(SYN_CURSAMPLES, drvr) = f1;
    synth_end = f1;

    uint32_t us = (uint32_t)(timer_us_gettime64() - t_start);
    st.frames++;
    st.frame_us += us;
    if (us > st.frame_max_us) {
        st.frame_max_us = us;
    }
    hud_frames++;
    hud_us += us;
#ifdef PERF_HUD
    sched_icache_resume(icache_counting);
#endif
}

// --- Audio interface ------------------------------------------------------------------------------

uint32_t aica_ai_length(uint32_t frame_size) {
    if (!anchored) {
        return 0;
    }
    uint64_t now = timer_us_gettime64();
    int64_t ahead = (int64_t)(synth_us(synth_end) - now);
    // The game's next frame is (frame size - queued + 0x60) & ~0xF samples: queue what puts the
    // synthesizer's end a retrace and the lead ahead of now.
    int64_t want_us = RETRACE_US + lead_us - ahead;
    int32_t want = (int32_t)(want_us * (int64_t)out_rate / 1000000);
    int32_t left = (int32_t)frame_size + 0x60 + 8 - want;
    return left > 0 ? (uint32_t)left * 4 : 0;
}

void aica_ai_buffer(uint32_t frames) {
    st.ai_frames += frames;
}

bool aica_active(void) {
    return active;
}

uint32_t aica_take_frame_us(uint32_t* frames) {
    uint32_t us = hud_us;
    if (frames) {
        *frames = hud_frames;
    }
    hud_us = 0;
    hud_frames = 0;
    return us;
}

void aica_hud(uint32_t* voices_on, uint32_t* lead_ms, uint32_t* slips) {
    uint32_t on = 0;
    for (int i = 0; i < pvoice_count; i++) {
        on += voices[i].keyed;
    }
    *voices_on = on;
    *lead_ms = (uint32_t)lead_us / 1000;
    *slips = total_slips;
}

void aica_print_stats(void) {
    uint32_t n = st.frames ? st.frames : 1;
    uint32_t on = 0;
    for (int i = 0; i < pvoice_count; i++) {
        on += voices[i].keyed;
    }
    printf("[q64dc] aica: %u frames, %u us each (max %u), %u AI samples; %u packets (late %u, dropped %u, "
           "queue peak %u B); key on %u off %u, %u voices on; lead %d ms (frames %d ms avg, %d min); slips %u; "
           "unknown samples %u; wet sends %u\n",
           (unsigned)st.frames, (unsigned)(st.frame_us / n), (unsigned)st.frame_max_us, (unsigned)st.ai_frames,
           (unsigned)st.packets, (unsigned)st.late, (unsigned)st.dropped, (unsigned)st.q_peak, (unsigned)st.keyons,
           (unsigned)st.keyoffs, (unsigned)on, (int)(lead_us / 1000), (int)(st.lead_sum_ms / (int32_t)n),
           (int)(st.lead_min_us / 1000), (unsigned)st.slips, (unsigned)st.unknown_waves, (unsigned)st.wet_sends);
#ifdef AICA_FW
    printf("[q64dc] aica firmware: clock %u, %u passes, %u events run, %u late (latest %u samples), %u ramp steps\n",
           (unsigned)clock_now(), (unsigned)g2_read_32(FW_SHARED + offsetof(q64_shared, passes)),
           (unsigned)g2_read_32(FW_SHARED + offsetof(q64_shared, applied)),
           (unsigned)g2_read_32(FW_SHARED + offsetof(q64_shared, late)),
           (unsigned)g2_read_32(FW_SHARED + offsetof(q64_shared, max_late)),
           (unsigned)g2_read_32(FW_SHARED + offsetof(q64_shared, steps)));
#endif
    uint32_t wet = st.wet_sends;
    memset(&st, 0, sizeof(st));
    // Kept for the run's total.
    st.wet_sends = wet;
}

bool aica_init(void) {
#ifdef AICA_FW
    if (!firmware_start()) {
        return false;
    }
#else
    dsp_off();
    snd_init();
    if (!firmware_alive()) {
        printf("[q64dc] aica: the sound processor didn't answer within 200 ms\n");
        return false;
    }
    q_data = SPU_RAM_UNCACHED_BASE + g2_read_32(Q_BASE + Q_DATA);
    q_size = g2_read_32(Q_BASE + Q_SIZE);
    if (q_size < 4096 || q_size > 0x10000) {
        printf("[q64dc] aica: odd command queue (%u bytes)\n", (unsigned)q_size);
        return false;
    }
#endif

    uint64_t us0 = timer_us_gettime64();
    uint32_t c0 = clock_now();
    if (!load_waves()) {
        return false;
    }
    // Measure the firmware's clock over the decode: KallistiOS's is meant to tick at 4410 Hz but
    // loses the interrupt's latency at every tick.
    uint64_t us1 = timer_us_gettime64();
    uint32_t c1 = clock_now();
    if (us1 - us0 > 50000) {
        float rate = (float)(c1 - c0) * 1000000.0f / (float)(us1 - us0);
        float nominal = jiffies_per_us * 1000000.0f;
        printf("[q64dc] aica: firmware clock %.1f Hz\n", (double)rate);
        if (rate > nominal * 0.9f && rate < nominal * 1.1f) {
            jiffies_per_us = rate / 1000000.0f;
        }
    }
    gain_lin = exp2_fast(AICA_GAIN_DB / 6.0206f);
    for (int i = 0; i < VOICES; i++) {
        voices[i].volume = 1;
        voices[i].pan = 64;
        voices[i].dry = eqpower[0];
        voices[i].ratio = 1.0f;
        voices[i].lv0[0] = voices[i].lv0[1] = voices[i].lv1[0] = voices[i].lv1[1] = LV_SILENT;
    }
    last_ts = clock_now();
    active = true;
    printf("[q64dc] aica: voices on the AICA, %d ms ahead\n", AICA_LEAD_US / 1000);
    return true;
}
