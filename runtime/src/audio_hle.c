// Quest 64's audio microcode, libultra's aspMain ("ABI 1"), as C.
//
// The game's synthesizer (alAudioFrame) builds a list of 64-bit commands each frame; the RSP runs
// them on 4 KB of DMEM: ADPCM decoding, resampling, envelope mixing into the main and aux (reverb)
// buses, reverb delay lines through RDRAM, and finally interleaving into the AI buffer. This file
// runs the same list on the CPU. The command semantics follow the public descriptions of the ABI,
// cross-checked with mupen64plus-rsp-hle (GPL-2.0-or-later), and are compared bit for bit against
// Quest64-Recomp's recompilation of the game's own microcode by tools/audio_test.
//
// DMEM is kept as host-order halfwords (the RSP's big-endian byte n is at n ^ 1), so the sample
// loops work on plain int16_t arrays; DMA to and from rdram's 32-bit words swaps their halves.

#include <stdbool.h>
#include <string.h>

#include "audio_hle.h"

#ifdef AUDIO_PROFILE
#include <arch/timer.h>
#endif

#define DMEM_SIZE 0x1000

// Flags in the command word.
#define A_INIT 0x01
#define A_LOOP 0x02
#define A_LEFT 0x02
#define A_VOL  0x04
#define A_AUX  0x08

enum {
    OP_SPNOOP,
    OP_ADPCM,
    OP_CLEARBUFF,
    OP_ENVMIXER,
    OP_LOADBUFF,
    OP_RESAMPLE,
    OP_SAVEBUFF,
    OP_SEGMENT,
    OP_SETBUFF,
    OP_SETVOL,
    OP_DMEMMOVE,
    OP_LOADADPCM,
    OP_MIXER,
    OP_INTERLEAVE,
    OP_POLEF,
    OP_SETLOOP,
};

// Room on both sides of DMEM, so the resampler's history before its input and its read-ahead
// can't leave the array however the game sets its buffers.
static int16_t dmem_store[64 + DMEM_SIZE / 2 + 2048] __attribute__((aligned(32)));
static int16_t* const dmem = dmem_store + 64;

static uint8_t* ram;
static uint32_t ram_mask;

static uint32_t segments[16];
static uint16_t buf_in, buf_out, buf_count;
static uint16_t aux_dry_right, aux_wet_left, aux_wet_right;
static int16_t env_vol[2], env_target[2], env_dry, env_wet;
static int32_t env_rate[2];
static uint32_t loop_addr;
static int16_t adpcm_book[128];

static uint32_t resample_lut_addr = 0xFFFFFFFF;
static int16_t resample_lut[64 * 4];
static bool resample_lut_has_min;

static uint32_t op_counts[16];
static uint32_t op_ns[16];
#ifdef AUDIO_PROFILE
uint32_t env_blocks_stepped, env_blocks_settled;
#endif

static inline uint32_t ram_w(uint32_t p) {
    return *(uint32_t*)(ram + (p & ram_mask & ~3u));
}

static inline int16_t ram_h(uint32_t p) {
    return *(int16_t*)(ram + ((p & ram_mask & ~1u) ^ 2));
}

static inline void ram_h_store(uint32_t p, int16_t v) {
    *(int16_t*)(ram + ((p & ram_mask & ~1u) ^ 2)) = v;
}

static inline int16_t clamp16(int32_t v) {
    if (__builtin_expect(v == (int16_t)v, 1)) {
        return (int16_t)v;
    }
    return v > 0 ? 32767 : -32768;
}

static inline int16_t* dmem_at(uint32_t a) {
    return dmem + ((a & (DMEM_SIZE - 1)) >> 1);
}

static inline uint8_t dmem_byte(uint32_t a) {
    return ((const uint8_t*)dmem)[(a & (DMEM_SIZE - 1)) ^ 1];
}

// Byte counts, rounded up the way the microcode's 8- or 16-byte loops round them, and kept
// inside DMEM.
static inline uint32_t dmem_len(uint32_t a, uint32_t len, uint32_t unit) {
    a &= DMEM_SIZE - 1;
    len = (len + unit - 1) & ~(unit - 1);
    return a + len > DMEM_SIZE ? DMEM_SIZE - a : len;
}

static inline uint32_t seg_addr(uint32_t w) {
    return segments[(w >> 24) & 0xF] + (w & 0xFFFFFF);
}

// RSP DMA moves 8-byte units between 8-byte aligned addresses.
static void dma_read(uint32_t dmem_addr, uint32_t dram, uint32_t len) {
    dmem_addr &= (DMEM_SIZE - 1) & ~7u;
    dram &= ~7u;
    len = dmem_len(dmem_addr, len, 8);
    uint32_t* d = (uint32_t*)((uint8_t*)dmem + dmem_addr);
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t v = ram_w(dram + i);
        *d++ = (v >> 16) | (v << 16);
    }
}

static void dma_write(uint32_t dmem_addr, uint32_t dram, uint32_t len) {
    dmem_addr &= (DMEM_SIZE - 1) & ~7u;
    dram &= ~7u;
    len = dmem_len(dmem_addr, len, 8);
    const uint32_t* s = (const uint32_t*)((const uint8_t*)dmem + dmem_addr);
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t v = *s++;
        *(uint32_t*)(ram + ((dram + i) & ram_mask)) = (v >> 16) | (v << 16);
    }
}

// The resampler's 64-phase, 4-tap filter is part of the microcode's data, which the game keeps
// in RDRAM.
static void load_resample_lut(uint32_t ucode_data) {
    uint32_t addr = ucode_data + 0xC0;
    if (addr == resample_lut_addr) {
        return;
    }
    resample_lut_addr = addr;
    resample_lut_has_min = false;
    for (int i = 0; i < 64 * 4; i++) {
        resample_lut[i] = ram_h(addr + i * 2);
        resample_lut_has_min |= resample_lut[i] == -32768;
    }
}

// --- Commands ---

// Order-2 VADPCM: each 9-byte frame is a header (scale, predictor) and 16 4-bit residuals. The
// output starts with the previous frame, which the resampler uses as history.
static void cmd_adpcm(uint32_t w0, uint32_t w1) {
    uint32_t flags = (w0 >> 16) & 0xFF;
    uint32_t state = seg_addr(w1);
    int16_t last[16];

    if (flags & A_INIT) {
        memset(last, 0, sizeof(last));
    } else {
        uint32_t from = (flags & A_LOOP) ? loop_addr : state;
        for (int i = 0; i < 16; i++) {
            last[i] = ram_h(from + i * 2);
        }
    }

    uint32_t out = buf_out & (DMEM_SIZE - 1);
    uint32_t in = buf_in;
    uint32_t frames = dmem_len(out + 32, buf_count, 32) / 32;
    memcpy(dmem_at(out), last, sizeof(last));
    int16_t* o = dmem_at(out) + 16;

    while (frames-- > 0) {
        uint8_t code = dmem_byte(in++);
        int shift = code >> 4;
        if (shift > 12) {
            shift = 12;
        }
        const int16_t* book1 = adpcm_book + (code & 0xF) * 16;
        const int16_t* book2 = book1 + 8;

        int32_t res[16];
        for (int i = 0; i < 16; i += 2) {
            uint8_t b = dmem_byte(in++);
            res[i] = ((int32_t)(int8_t)b >> 4) << shift;
            res[i + 1] = ((int32_t)(int8_t)(b << 4) >> 4) << shift;
        }

        int32_t l1 = last[14], l2 = last[15];
        for (int half = 0; half < 16; half += 8) {
            const int32_t* r = res + half;
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++) {
                int32_t acc = (r[i] << 11) + book1[i] * l1 + book2[i] * l2;
#pragma GCC unroll 8
                for (int j = 0; j < i; j++) {
                    acc += book2[i - 1 - j] * r[j];
                }
                o[half + i] = clamp16(acc >> 11);
            }
            l1 = o[half + 6];
            l2 = o[half + 7];
        }
        memcpy(last, o, sizeof(last));
        o += 16;
    }

    for (int i = 0; i < 16; i++) {
        ram_h_store(state + i * 2, last[i]);
    }
}

static void cmd_clearbuff(uint32_t w0, uint32_t w1) {
    uint32_t a = w0 & 0xFFFF;
    uint32_t len = w1 & 0xFFFF;
    if (len != 0) {
        memset(dmem_at(a), 0, dmem_len(a, len, 16));
    }
}

// The Q15 rounding constant, in a register: as a known constant, GCC reloads it from the literal
// pool at every use in the sample loops.
static inline int32_t q15_round(void) {
    int32_t r = 0x4000;
    __asm__("" : "+r"(r));
    return r;
}

// The RSP's VMULF: a rounded Q15 product.
static inline int16_t mulf(int32_t a, int32_t b) {
    return clamp16((a * b + 0x4000) >> 15);
}

// dst * 0x7FFF + src * gain in Q15, rounded once: the microcode's VMULF/VMACF mix. The sum
// stays inside 32 bits for any 16-bit inputs.
static inline int16_t mix_q15_r(int32_t dst, int32_t src, int32_t gain, int32_t round) {
    // dst * 0x7FFF is dst * 0x8000 - dst, and the multiple of 0x8000 passes through the shift.
    return clamp16(dst + ((src * gain - dst + round) >> 15));
}

static inline int16_t mix_q15(int32_t dst, int32_t src, int32_t gain) {
    return mix_q15_r(dst, src, gain, 0x4000);
}

// mix_q15 over n samples, four at a time. One bus per pass keeps everything in the SH-4's
// registers.
static inline void mix_run(int16_t* d, const int16_t* s, int32_t gain, uint32_t n) {
    int32_t round = q15_round();
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        int32_t s0 = s[i], s1 = s[i + 1], s2 = s[i + 2], s3 = s[i + 3];
        int32_t d0 = d[i], d1 = d[i + 1], d2 = d[i + 2], d3 = d[i + 3];
        d[i] = mix_q15_r(d0, s0, gain, round);
        d[i + 1] = mix_q15_r(d1, s1, gain, round);
        d[i + 2] = mix_q15_r(d2, s2, gain, round);
        d[i + 3] = mix_q15_r(d3, s3, gain, round);
    }
    for (; i < n; i++) {
        d[i] = mix_q15_r(d[i], s[i], gain, round);
    }
}

// The same with a gain per lane of an 8-sample vector.
static inline void mix_vector(int16_t* d, const int16_t* s, const int16_t* gain) {
    int32_t round = q15_round();
    for (int k = 0; k < 8; k += 4) {
        int32_t s0 = s[k], s1 = s[k + 1], s2 = s[k + 2], s3 = s[k + 3];
        int32_t d0 = d[k], d1 = d[k + 1], d2 = d[k + 2], d3 = d[k + 3];
        d[k] = mix_q15_r(d0, s0, gain[k], round);
        d[k + 1] = mix_q15_r(d1, s1, gain[k + 1], round);
        d[k + 2] = mix_q15_r(d2, s2, gain[k + 2], round);
        d[k + 3] = mix_q15_r(d3, s3, gain[k + 3], round);
    }
}

// One envelope: a 16.16 volume per lane of the RSP's 8-sample vectors, multiplied by a 16.16
// rate once per vector and then clamped to the target, which only stops the integer half.
typedef struct {
    int16_t hi[8];
    uint16_t lo[8];
    int16_t target, rate_hi;
    uint16_t rate_lo;
    bool settled;
} Envelope;

// VMADN's result: the low half of the accumulator, saturated when the accumulator doesn't fit
// in 32 bits.
static inline uint16_t acc_low(int64_t acc) {
    return acc > 0x7FFFFFFF ? 0xFFFF : acc < -0x80000000LL ? 0 : (uint16_t)acc;
}

static inline void env_clamp(Envelope* e) {
    for (int k = 0; k < 8; k++) {
        if (e->rate_hi > 0) {
            if ((uint16_t)e->hi[k] >= (uint16_t)e->target) {
                e->hi[k] = e->target;
            }
        } else if (e->hi[k] < e->target) {
            e->hi[k] = e->target;
        }
    }
}

static void env_step(Envelope* e) {
    if (e->settled) {
        return;
    }
    // The microcode's four partial products, with only lo * rate_lo truncated, add up to the
    // full 16.16 x 16.16 product shifted down by 16.
    int32_t rate = (int32_t)(((uint32_t)(uint16_t)e->rate_hi << 16) | e->rate_lo);
    for (int k = 0; k < 8; k++) {
        int32_t vol = (int32_t)(((uint32_t)(uint16_t)e->hi[k] << 16) | e->lo[k]);
        int64_t acc = ((int64_t)vol * rate) >> 16;
        e->hi[k] = clamp16((int32_t)(acc >> 16));
        e->lo[k] = acc_low(acc);
    }
    env_clamp(e);
    // At its target with the rate pushing past it, the integer half stays put for good. Only the
    // fraction, which never reaches the output, would keep changing.
    bool all = e->target >= 0 && e->rate_hi >= 0;
    for (int k = 0; all && k < 8; k++) {
        all = e->hi[k] == e->target;
    }
    e->settled = all;
}

// A_INIT: the first vector ramps linearly from the start volume towards start * rate.
static void env_start(Envelope* e, int16_t vol) {
    int64_t acc = (int64_t)vol * e->rate_lo + ((int64_t)vol * e->rate_hi) * 65536;
    int32_t d_lo = acc_low(acc);
    int32_t d_hi = clamp16(clamp16((int32_t)(acc >> 16)) - vol);
    static const uint16_t frac[8] = {0x2000, 0x4000, 0x6000, 0x8000, 0xA000, 0xC000, 0xE000, 0xFFFF};
    for (int k = 0; k < 8; k++) {
        int64_t a = ((int64_t)frac[k] * (uint16_t)d_lo) >> 16;
        a += (int64_t)frac[k] * d_hi;
        a += (int64_t)vol * 65536;
        e->hi[k] = clamp16((int32_t)(a >> 16));
        e->lo[k] = acc_low(a);
    }
    e->settled = false;
    env_clamp(e);
}

static void env_load(Envelope* e, uint32_t addr) {
    for (int k = 0; k < 8; k++) {
        e->hi[k] = ram_h(addr + k * 2);
        e->lo[k] = (uint16_t)ram_h(addr + 16 + k * 2);
    }
    e->settled = false;
}

static void env_save(const Envelope* e, uint32_t addr) {
    for (int k = 0; k < 8; k++) {
        ram_h_store(addr + k * 2, e->hi[k]);
        ram_h_store(addr + 16 + k * 2, (int16_t)e->lo[k]);
    }
}

// One 8-sample vector of a voice into the left and right dry buses and, with aux, the wet buses.
static inline void env_mix(const Envelope* env, int16_t dry, int16_t wet, bool aux,
                           const int16_t* in, int16_t* dl, int16_t* dr, int16_t* wl, int16_t* wr) {
    int16_t gl[8], gr[8];
    for (int k = 0; k < 8; k++) {
        gl[k] = mulf(env[0].hi[k], dry);
        gr[k] = mulf(env[1].hi[k], dry);
    }
    mix_vector(dl, in, gl);
    mix_vector(dr, in, gr);
    if (aux) {
        int16_t hl[8], hr[8];
        for (int k = 0; k < 8; k++) {
            hl[k] = mulf(env[0].hi[k], wet);
            hr[k] = mulf(env[1].hi[k], wet);
        }
        mix_vector(wl, in, hl);
        mix_vector(wr, in, hr);
    }
}

// Applies the left and right volume envelopes to a voice and mixes it into the dry bus and,
// with A_AUX, the wet (reverb) bus. The 80-byte state is the microcode's: left and right
// envelope lanes, then the envelope parameters, which A_INIT takes from SETVOL.
static void cmd_envmixer(uint32_t w0, uint32_t w1) {
    uint32_t flags = (w0 >> 16) & 0xFF;
    uint32_t state = seg_addr(w1);
    bool aux = (flags & A_AUX) != 0;
    int16_t params[8];
    Envelope env[2];

    if (flags & A_INIT) {
        params[0] = env_target[0];
        params[1] = (int16_t)(env_rate[0] >> 16);
        params[2] = (int16_t)env_rate[0];
        params[3] = env_target[1];
        params[4] = (int16_t)(env_rate[1] >> 16);
        params[5] = (int16_t)env_rate[1];
        params[6] = env_dry;
        params[7] = env_wet;
    } else {
        for (int k = 0; k < 8; k++) {
            params[k] = ram_h(state + 0x40 + k * 2);
        }
    }
    for (int c = 0; c < 2; c++) {
        env[c].target = params[c * 3];
        env[c].rate_hi = params[c * 3 + 1];
        env[c].rate_lo = (uint16_t)params[c * 3 + 2];
    }
    int16_t dry = params[6], wet = params[7];

    const int16_t* in = dmem_at(buf_in);
    int16_t* dl = dmem_at(buf_out);
    int16_t* dr = dmem_at(aux_dry_right);
    int16_t* wl = dmem_at(aux_wet_left);
    int16_t* wr = dmem_at(aux_wet_right);
    int32_t blocks = ((int32_t)buf_count + 15) / 16;

    if (flags & A_INIT) {
        env_start(&env[0], env_vol[0]);
        env_start(&env[1], env_vol[1]);
        env_mix(env, dry, wet, aux, in, dl, dr, wl, wr);
        in += 8, dl += 8, dr += 8;
        if (aux) {
            wl += 8, wr += 8;
        }
        blocks--;
    } else {
        env_load(&env[0], state);
        env_load(&env[1], state + 0x20);
    }
    if (blocks < 1) {
        blocks = 1;
    }
    if (dmem_len(buf_in, (uint32_t)blocks * 16, 1) < (uint32_t)blocks * 16) {
        blocks = (int32_t)(dmem_len(buf_in, (uint32_t)blocks * 16, 1) / 16);
    }

    while (blocks > 0 && !(env[0].settled && env[1].settled)) {
        env_step(&env[0]);
        env_step(&env[1]);
        env_mix(env, dry, wet, aux, in, dl, dr, wl, wr);
        in += 8, dl += 8, dr += 8;
        if (aux) {
            wl += 8, wr += 8;
        }
        blocks--;
#ifdef AUDIO_PROFILE
        env_blocks_stepped++;
#endif
    }
#ifdef AUDIO_PROFILE
    env_blocks_settled += blocks > 0 ? blocks : 0;
#endif
    // Settled envelopes have every lane at the target, so the gains stay put.
    if (blocks > 0) {
        int32_t n = blocks * 8;
        mix_run(dl, in, mulf(env[0].target, dry), n);
        mix_run(dr, in, mulf(env[1].target, dry), n);
        if (aux) {
            mix_run(wl, in, mulf(env[0].target, wet), n);
            mix_run(wr, in, mulf(env[1].target, wet), n);
        }
    }

    env_save(&env[0], state);
    env_save(&env[1], state + 0x20);
    for (int k = 0; k < 8; k++) {
        ram_h_store(state + 0x40 + k * 2, params[k]);
    }
}

// Resamples by a Q1.15 pitch with the microcode's 4-tap filter. The state holds the last four
// input samples and the phase.
static void cmd_resample(uint32_t w0, uint32_t w1) {
    uint32_t flags = (w0 >> 16) & 0xFF;
    uint32_t pitch = (w0 & 0xFFFF) << 1;
    uint32_t state = seg_addr(w1);
    int16_t* s = dmem_at(buf_in) - 4;
    uint32_t phase;

    if (flags & A_INIT) {
        s[0] = s[1] = s[2] = s[3] = 0;
        phase = 0;
    } else {
        for (int k = 0; k < 4; k++) {
            s[k] = ram_h(state + k * 2);
        }
        phase = (uint16_t)ram_h(state + 8);
    }

    uint32_t n = dmem_len(buf_out, buf_count, 16) / 2;
    int16_t* out = dmem_at(buf_out);
    // Each tap is rounded on its own, then the taps are added in pairs with saturation. A rounded
    // product only needs saturating for -32768 * -32768, which a filter without -32768 can't make.
    if (resample_lut_has_min) {
        for (uint32_t i = 0; i < n; i++) {
            const int16_t* c = resample_lut + (phase >> 10) * 4;
            int32_t a = clamp16(mulf(s[0], c[0]) + mulf(s[1], c[1]));
            int32_t b = clamp16(mulf(s[2], c[2]) + mulf(s[3], c[3]));
            out[i] = clamp16(a + b);
            phase += pitch;
            s += phase >> 16;
            phase &= 0xFFFF;
        }
    } else {
        int32_t round = q15_round();
        for (uint32_t i = 0; i < n; i++) {
            const int16_t* c = resample_lut + (phase >> 10) * 4;
            int32_t a = ((s[0] * c[0] + round) >> 15) + ((s[1] * c[1] + round) >> 15);
            int32_t b = ((s[2] * c[2] + round) >> 15) + ((s[3] * c[3] + round) >> 15);
            out[i] = clamp16(clamp16(a) + clamp16(b));
            phase += pitch;
            s += phase >> 16;
            phase &= 0xFFFF;
        }
    }

    for (int k = 0; k < 4; k++) {
        ram_h_store(state + k * 2, s[k]);
    }
    ram_h_store(state + 8, (int16_t)phase);
}

static void cmd_setvol(uint32_t w0, uint32_t w1) {
    uint32_t flags = (w0 >> 16) & 0xFF;
    int16_t v = (int16_t)(w0 & 0xFFFF);
    int c = (flags & A_LEFT) ? 0 : 1;
    if (flags & A_AUX) {
        env_dry = v;
        env_wet = (int16_t)(w1 & 0xFFFF);
    } else if (flags & A_VOL) {
        env_vol[c] = v;
    } else {
        env_target[c] = v;
        env_rate[c] = (int32_t)w1;
    }
}

static void cmd_setbuff(uint32_t w0, uint32_t w1) {
    uint32_t flags = (w0 >> 16) & 0xFF;
    if (flags & A_AUX) {
        aux_dry_right = w0 & 0xFFFF;
        aux_wet_left = w1 >> 16;
        aux_wet_right = w1 & 0xFFFF;
    } else {
        buf_in = w0 & 0xFFFF;
        buf_out = w1 >> 16;
        buf_count = w1 & 0xFFFF;
    }
}

static void cmd_dmemmove(uint32_t w0, uint32_t w1) {
    uint32_t from = w0 & 0xFFFF;
    uint32_t to = w1 >> 16;
    uint32_t len = w1 & 0xFFFF;
    if (len == 0) {
        return;
    }
    len = dmem_len(to, dmem_len(from, len, 16), 16);
    // Forwards, 16 bytes at a time, as the microcode copies (it matters when the buffers overlap).
    const int16_t* s = dmem_at(from);
    int16_t* d = dmem_at(to);
    for (uint32_t i = 0; i < len / 2; i += 8) {
        int16_t chunk[8];
        memcpy(chunk, s + i, sizeof(chunk));
        memcpy(d + i, chunk, sizeof(chunk));
    }
}

static void cmd_loadadpcm(uint32_t w0, uint32_t w1) {
    uint32_t addr = seg_addr(w1);
    uint32_t n = (w0 & 0xFFFF) / 2;
    if (n > 128) {
        n = 128;
    }
    for (uint32_t i = 0; i < n; i++) {
        adpcm_book[i] = ram_h(addr + i * 2);
    }
}

static void cmd_mixer(uint32_t w0, uint32_t w1) {
    int32_t gain = (int16_t)(w0 & 0xFFFF);
    uint32_t from = w1 >> 16;
    uint32_t to = w1 & 0xFFFF;
    uint32_t n = dmem_len(to, dmem_len(from, buf_count, 32), 32) / 2;
    mix_run(dmem_at(to), dmem_at(from), gain, n);
}

static void cmd_interleave(uint32_t w0, uint32_t w1) {
    (void)w0;
    if (buf_count == 0) {
        return;
    }
    uint32_t n = dmem_len(buf_out, ((buf_count + 15) & ~15u) * 2, 1) / 4;
    const int16_t* l = dmem_at(w1 >> 16);
    const int16_t* r = dmem_at(w1 & 0xFFFF);
    int16_t* o = dmem_at(buf_out);
    for (uint32_t i = 0; i < n; i++) {
        o[i * 2] = l[i];
        o[i * 2 + 1] = r[i];
    }
}

// A two-pole filter whose coefficients come from the ADPCM table.
static void cmd_polef(uint32_t w0, uint32_t w1) {
    uint32_t flags = (w0 >> 16) & 0xFF;
    int32_t gain = (int16_t)(w0 & 0xFFFF);
    uint32_t state = seg_addr(w1);
    if (buf_count == 0) {
        return;
    }
    // The microcode scales the second row by gain << 2 as an unsigned 16-bit number, and leaves
    // the scaled row in the table.
    const int16_t* h1 = adpcm_book;
    int16_t h2[8];
    int16_t* h2g = adpcm_book + 8;
    memcpy(h2, h2g, sizeof(h2));
    int32_t scale = (uint16_t)(gain << 2);
    for (int i = 0; i < 8; i++) {
        h2g[i] = (int16_t)((h2[i] * scale) >> 16);
    }

    int32_t l1 = 0, l2 = 0;
    if (!(flags & A_INIT)) {
        l1 = ram_h(state + 4);
        l2 = ram_h(state + 6);
    }

    uint32_t n = dmem_len(buf_out, dmem_len(buf_in, buf_count, 16), 16) / 16;
    const int16_t* in = dmem_at(buf_in);
    int16_t* out = dmem_at(buf_out);
    for (uint32_t f = 0; f < n; f++, in += 8, out += 8) {
        int16_t frame[8];
        memcpy(frame, in, sizeof(frame));
        for (int i = 0; i < 8; i++) {
            int32_t acc = frame[i] * gain + h1[i] * l1 + h2[i] * l2;
            for (int j = 0; j < i; j++) {
                acc += h2g[i - 1 - j] * frame[j];
            }
            out[i] = clamp16(acc >> 14);
        }
        l1 = out[6];
        l2 = out[7];
    }
    for (int k = 0; k < 4; k++) {
        ram_h_store(state + k * 2, out[k - 4]);
    }
}

void audio_hle_run(uint8_t* rdram, uint32_t rdram_size, uint32_t task) {
    ram = rdram;
    ram_mask = rdram_size - 1;

    load_resample_lut(ram_w(task + 0x18));
    uint32_t list = ram_w(task + 0x30);
    uint32_t size = ram_w(task + 0x34);

    for (uint32_t off = 0; off < size; off += 8) {
        uint32_t w0 = ram_w(list + off);
        uint32_t w1 = ram_w(list + off + 4);
        uint32_t op = (w0 >> 24) & 0xF;
        op_counts[op]++;
#ifdef AUDIO_PROFILE
        uint64_t t0 = timer_ns_gettime64();
#endif
        switch (op) {
            case OP_SPNOOP:
                break;
            case OP_ADPCM:
                cmd_adpcm(w0, w1);
                break;
            case OP_CLEARBUFF:
                cmd_clearbuff(w0, w1);
                break;
            case OP_ENVMIXER:
                cmd_envmixer(w0, w1);
                break;
            case OP_LOADBUFF:
                dma_read(buf_in, seg_addr(w1), buf_count);
                break;
            case OP_RESAMPLE:
                cmd_resample(w0, w1);
                break;
            case OP_SAVEBUFF:
                dma_write(buf_out, seg_addr(w1), buf_count);
                break;
            case OP_SEGMENT:
                segments[(w1 >> 24) & 0xF] = w1 & 0xFFFFFF;
                break;
            case OP_SETBUFF:
                cmd_setbuff(w0, w1);
                break;
            case OP_SETVOL:
                cmd_setvol(w0, w1);
                break;
            case OP_DMEMMOVE:
                cmd_dmemmove(w0, w1);
                break;
            case OP_LOADADPCM:
                cmd_loadadpcm(w0, w1);
                break;
            case OP_MIXER:
                cmd_mixer(w0, w1);
                break;
            case OP_INTERLEAVE:
                cmd_interleave(w0, w1);
                break;
            case OP_POLEF:
                cmd_polef(w0, w1);
                break;
            case OP_SETLOOP:
                loop_addr = seg_addr(w1);
                break;
        }
#ifdef AUDIO_PROFILE
        op_ns[op] += (uint32_t)(timer_ns_gettime64() - t0);
#endif
    }
}

void audio_hle_take_counts(uint32_t counts[16], uint32_t us[16]) {
    memcpy(counts, op_counts, sizeof(op_counts));
    memset(op_counts, 0, sizeof(op_counts));
    for (int i = 0; i < 16; i++) {
        us[i] = op_ns[i] / 1000;
        op_ns[i] = 0;
    }
}
