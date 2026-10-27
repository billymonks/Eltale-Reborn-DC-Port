// libultra 2.0's sinf and cosf, the game's __sinf (0x80035680) and __cosf (0x800361F0), in native code
// (config/quest64.us.toml hooks them). Each does the recompiled function's double operations,
// conversions and comparisons in the same order, so every result is bit-identical to it under the
// same FPSCR (the runtime's threads run with DN set and round-to-nearest, fpu.c). It also stores the
// argument to the caller's stack word like the original and leaves the registers the recompiled
// function writes back for its callers (v0, f0-f2, f12, f14: what the register analysis says a
// caller may read after the call), with the same values.

#include "recomp.h"

// Each product and sum rounds on its own, as in the generated code (-ffp-contract=off).
#pragma GCC optimize("fp-contract=off")

typedef union {
    uint64_t u;
    double d;
} dbits;

// The constants both functions load from the ROM's rodata (sinf's copy at 0x80071B40, cosf's at
// 0x80071BB0; they are the same, and match libultra's source).
static const dbits P1 = { 0xBFC55554BC83656Dull };
static const dbits P2 = { 0x3F8110ED3804C2A0ull };
static const dbits P3 = { 0xBF29F6FFEEA56814ull };
static const dbits P4 = { 0x3EC5DBDF0E314BFEull };
static const dbits RPI = { 0x3FD45F306DC9C883ull };
static const dbits PIHI = { 0x400921FB50000000ull };
static const dbits PILO = { 0x3E6110B4611A6263ull };

// The words the giving-up paths return: __libm_qnan_f, sinf's zero and cosf's (also 0.0) result
// for |x| >= 2^28.
#define QNAN_ADDR 0x80071F10u
#define SIN_HUGE_ADDR 0x80071B80u
#define COS_HUGE_ADDR 0x80071BF0u

static inline double poly_of(double xsq) {
    double p = MUL_D(P4.d, xsq) + P3.d;
    p = MUL_D(p, xsq) + P2.d;
    p = MUL_D(p, xsq);
    return P1.d + p;
}

// libultra's ROUND: add or subtract one half, then truncate.
static inline int32_t round_half_away(double dn) {
    return 0.0 <= dn ? TRUNC_W_D(dn + 0.5) : TRUNC_W_D(dn - 0.5);
}

static inline uint32_t high_word(double d) {
    dbits b;
    b.d = d;
    return (uint32_t)(b.u >> 32);
}

void ultra_sinf(uint8_t* rdram, recomp_context* ctx) {
    gpr sp = ctx->r29;
    uint32_t ix = ctx->f12.u32l;
    float x = ctx->f12.fl;
    MEM_W(0, sp) = (int32_t)ix;
    int32_t xpt = ((int32_t)ix >> 22) & 0x1FF;
    if (xpt < 0xFF) {
        // |x| < 1.5
        double dx = CVT_D_S(x);
        ctx->r2 = ix;
        ctx->f2.d = dx;
        if (xpt < 0xE6) {
            // |x| < 2^-12: x itself
            ctx->f0.u32l = ix;
            return;
        }
        double xsq = MUL_D(dx, dx);
        double poly = poly_of(xsq);
        ctx->f0.fl = CVT_S_D(MUL_D(MUL_D(dx, xsq), poly) + dx);
        ctx->f12.d = xsq;
        ctx->f14.d = poly;
        return;
    }
    // The original loads x into f4 in a delay slot here.
    ctx->f4.u32l = ix;
    if (xpt < 0x136) {
        // |x| < 2^28: dx = x - n*pi
        double dx = CVT_D_S(x);
        int32_t n = round_half_away(MUL_D(dx, RPI.d));
        double dn = CVT_D_W(n);
        double hi = MUL_D(dn, PIHI.d);
        double lo = MUL_D(dn, PILO.d);
        dx = dx - hi;
        dx = dx - lo;
        double xsq = MUL_D(dx, dx);
        double poly = poly_of(xsq);
        float result = CVT_S_D(MUL_D(MUL_D(dx, xsq), poly) + dx);
        ctx->f0.fl = (n & 1) ? FNEG_S(result) : result;
        ctx->f0.u32h = high_word(dn);
        ctx->r2 = (gpr)n;
        ctx->f2.d = dx;
        ctx->f12.d = xsq;
        ctx->f14.d = poly;
        return;
    }
    ctx->r2 = ix;
    ctx->f0.u32l = (uint32_t)MEM_W(0, x == x ? SIN_HUGE_ADDR : QNAN_ADDR);
}

void ultra_cosf(uint8_t* rdram, recomp_context* ctx) {
    gpr sp = ctx->r29;
    uint32_t ix = ctx->f12.u32l;
    float x = ctx->f12.fl;
    MEM_W(0, sp) = (int32_t)ix;
    // The original also loads x into f10 for its NaN test.
    ctx->f10.u32l = ix;
    int32_t xpt = ((int32_t)ix >> 22) & 0x1FF;
    if (xpt < 0x136) {
        // |x| < 2^28: dx = |x| - (n - 0.5)*pi
        float xabs = 0.0f < x ? x : FNEG_S(x);
        double dx = CVT_D_S(xabs);
        int32_t n = round_half_away(MUL_D(dx, RPI.d) + 0.5);
        double dn = CVT_D_W(n) - 0.5;
        double hi = MUL_D(dn, PIHI.d);
        double lo = MUL_D(dn, PILO.d);
        double r = dx - hi;
        r = r - lo;
        double xsq = MUL_D(r, r);
        double poly = poly_of(xsq);
        float result = CVT_S_D(MUL_D(MUL_D(r, xsq), poly) + r);
        ctx->f0.fl = (n & 1) ? FNEG_S(result) : result;
        ctx->f0.u32h = high_word(dn);
        ctx->r2 = (gpr)n;
        ctx->f2.d = r;
        ctx->f12.d = dx;
        ctx->f14.d = xsq;
        return;
    }
    ctx->r2 = ix;
    ctx->f0.u32l = (uint32_t)MEM_W(0, x == x ? COS_HUGE_ADDR : QNAN_ADDR);
}

#ifdef SINCOS_CHECK
#include <stdio.h>
#include <math.h>

// Validation builds (RT_DEFINES=-DSINCOS_CHECK, with the hooks swapped for sincos_enter at entry and
// sincos_leave at every return; build/agent/sccheck.sh): the recompiled function still runs, and each
// call's results are compared with the native function's on a copy of the context.
void __sinf_recomp(uint8_t* rdram, recomp_context* ctx);
void __cosf_recomp(uint8_t* rdram, recomp_context* ctx);

static recomp_context shadow[2];
static uint32_t shadow_stack[2];
static unsigned long long checked[2], mismatched[2];

void sincos_enter(uint8_t* rdram, recomp_context* ctx, int k) {
    shadow[k] = *ctx;
    int32_t saved = MEM_W(0, ctx->r29);
    (k ? ultra_cosf : ultra_sinf)(rdram, &shadow[k]);
    shadow_stack[k] = (uint32_t)MEM_W(0, ctx->r29);
    MEM_W(0, ctx->r29) = saved;
}

void sincos_leave(uint8_t* rdram, recomp_context* ctx, int k) {
    const recomp_context* s = &shadow[k];
    checked[k]++;
    if (ctx->r2 != s->r2 || ctx->f0.u64 != s->f0.u64 || ctx->f2.u64 != s->f2.u64 || ctx->f12.u64 != s->f12.u64 ||
        ctx->f14.u64 != s->f14.u64 || (uint32_t)MEM_W(0, ctx->r29) != shadow_stack[k]) {
        if (mismatched[k]++ < 16) {
            printf("[q64dc] SINCOS MISMATCH %s x=%08X: f0 %08X%08X/%08X%08X v0 %08X/%08X\n", k ? "cosf" : "sinf",
                   (unsigned)s->f12.u32l == (unsigned)ctx->f12.u32l ? (unsigned)ctx->f12.u32l : (unsigned)shadow_stack[k],
                   (unsigned)ctx->f0.u32h, (unsigned)ctx->f0.u32l, (unsigned)s->f0.u32h, (unsigned)s->f0.u32l,
                   (unsigned)ctx->r2, (unsigned)s->r2);
        }
    }
}

static void call_both(uint8_t* rdram, recomp_context* c, uint32_t sp, uint32_t u) {
    for (int k = 0; k < 2; k++) {
        c->r29 = sp;
        c->f12.u32l = u;
        (k ? __cosf_recomp : __sinf_recomp)(rdram, c);
    }
}

// Every stride-th bit pattern, the boundaries of each path, every non-finite and denormal class, and
// the floats around k*pi/2 for |k| <= 100000, through both functions.
void sincos_sweep(uint8_t* rdram, uint32_t stride) {
    static recomp_context c;
    const uint32_t sp = 0x80000100u;
    int32_t saved = MEM_W(0, sp);
    unsigned long long before = checked[0] + checked[1];
    for (uint64_t u = 0; u < 0x100000000ull; u += stride) {
        call_both(rdram, &c, sp, (uint32_t)u);
    }
    static const uint32_t specials[] = { 0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
                                         0x00800000u, 0x80800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u,
                                         0x7F800001u, 0x7FBFFFFFu, 0x7FC00000u, 0xFFC00000u, 0x7FFFFFFFu, 0xFFFFFFFFu };
    for (unsigned i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
        call_both(rdram, &c, sp, specials[i]);
    }
    static const uint32_t xpts[] = { 229, 230, 254, 255, 309, 310 };
    for (unsigned i = 0; i < sizeof(xpts) / sizeof(xpts[0]); i++) {
        for (uint32_t m = 0; m < 0x400000u; m += 0x1FFFu) {
            call_both(rdram, &c, sp, xpts[i] << 22 | m);
            call_both(rdram, &c, sp, 0x80000000u | xpts[i] << 22 | m);
        }
        call_both(rdram, &c, sp, xpts[i] << 22 | 0x3FFFFFu);
        call_both(rdram, &c, sp, 0x80000000u | xpts[i] << 22 | 0x3FFFFFu);
    }
    for (int k = -100000; k <= 100000; k++) {
        union {
            float f;
            uint32_t u;
        } v = { (float)(k * 1.5707963267948966) };
        for (int d = -2; d <= 2; d++) {
            call_both(rdram, &c, sp, v.u + (uint32_t)d);
        }
    }
    MEM_W(0, sp) = saved;
    printf("[q64dc] SINCOS sweep: %llu calls compared\n", checked[0] + checked[1] - before);
}

void sincos_report(void) {
    printf("[q64dc] SINCOS sinf %llu checked %llu mismatched, cosf %llu checked %llu mismatched\n",
           checked[0], mismatched[0], checked[1], mismatched[1]);
}
#endif
