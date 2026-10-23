// RDP color combiner approximation. The PowerVR computes texture * vertex color + offset color,
// so each cycle's (A - B) * C + D is evaluated per vertex as a linear function of the texel:
// mul * texel + add. A texel times a texel is taken as the texel (exact for 0 and 1), and the
// texel alpha in the color equation is taken as the texel color.

#include "gfx_internal.h"

static const uint8_t color_a[16] = {
    CC_COMBINED, CC_TEXEL, CC_TEXEL, CC_PRIM, CC_SHADE, CC_ENV, CC_1, CC_HALF,
    CC_0, CC_0, CC_0, CC_0, CC_0, CC_0, CC_0, CC_0,
};
static const uint8_t color_b[16] = {
    CC_COMBINED, CC_TEXEL, CC_TEXEL, CC_PRIM, CC_SHADE, CC_ENV, CC_HALF, CC_0,
    CC_0, CC_0, CC_0, CC_0, CC_0, CC_0, CC_0, CC_0,
};
static const uint8_t color_c[32] = {
    CC_COMBINED, CC_TEXEL, CC_TEXEL, CC_PRIM, CC_SHADE, CC_ENV, CC_1, CC_COMBINED_A,
    CC_TEXEL_A, CC_TEXEL_A, CC_PRIM_A, CC_SHADE_A, CC_ENV_A, CC_LOD, CC_PRIM_LOD, CC_0,
    CC_0, CC_0, CC_0, CC_0, CC_0, CC_0, CC_0, CC_0,
    CC_0, CC_0, CC_0, CC_0, CC_0, CC_0, CC_0, CC_0,
};
static const uint8_t color_d[8] = {
    CC_COMBINED, CC_TEXEL, CC_TEXEL, CC_PRIM, CC_SHADE, CC_ENV, CC_1, CC_0,
};
static const uint8_t alpha_abd[8] = {
    CC_COMBINED, CC_TEXEL, CC_TEXEL, CC_PRIM, CC_SHADE, CC_ENV, CC_1, CC_0,
};
static const uint8_t alpha_c[8] = {
    CC_LOD, CC_TEXEL, CC_TEXEL, CC_PRIM, CC_SHADE, CC_ENV, CC_PRIM_LOD, CC_0,
};

typedef struct {
    float m, o;
} Lin;

static bool uses_combined(const uint8_t in[2][4]) {
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            if (in[i][j] == CC_COMBINED || in[i][j] == CC_COMBINED_A) {
                return true;
            }
        }
    }
    return false;
}

void cc_compile(GfxCombiner* cc, uint32_t w0, uint32_t w1, bool two_cycle) {
    cc->w0 = w0;
    cc->w1 = w1;
    cc->two_cycle = two_cycle;

    uint8_t raw[2][2][4] = {
        { { (w0 >> 20) & 0xF, (w1 >> 28) & 0xF, (w0 >> 15) & 0x1F, (w1 >> 15) & 0x7 },
          { (w0 >> 12) & 0x7, (w1 >> 12) & 0x7, (w0 >> 9) & 0x7, (w1 >> 9) & 0x7 } },
        { { (w0 >> 5) & 0xF, (w1 >> 24) & 0xF, w0 & 0x1F, (w1 >> 6) & 0x7 },
          { (w1 >> 21) & 0x7, (w1 >> 3) & 0x7, (w1 >> 18) & 0x7, w1 & 0x7 } },
    };
    for (int c = 0; c < 2; c++) {
        cc->in[c][0][0] = color_a[raw[c][0][0]];
        cc->in[c][0][1] = color_b[raw[c][0][1]];
        cc->in[c][0][2] = color_c[raw[c][0][2]];
        cc->in[c][0][3] = color_d[raw[c][0][3]];
        cc->in[c][1][0] = alpha_abd[raw[c][1][0]];
        cc->in[c][1][1] = alpha_abd[raw[c][1][1]];
        cc->in[c][1][2] = alpha_c[raw[c][1][2]];
        cc->in[c][1][3] = alpha_abd[raw[c][1][3]];
    }

    // One-cycle mode runs the second cycle's settings. Games set both to the same mode, but
    // a second cycle that reads the combined output can't be what they meant, so fall back
    // to the first.
    if (!two_cycle && uses_combined(cc->in[1])) {
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 4; j++) {
                cc->in[1][i][j] = cc->in[0][i][j];
            }
        }
    }

    // Find out whether the texel reaches the output with typical inputs.
    static const GfxCCConst probe_k = {
        { 0.6f, 0.6f, 0.6f, 0.6f }, { 0.3f, 0.3f, 0.3f, 0.3f }, 0.5f,
    };
    static const float probe_shade[4] = { 0.8f, 0.8f, 0.8f, 0.8f };
    float mul[4], add[4];
    cc_eval(cc, &probe_k, probe_shade, mul, add);
    cc->tex_rgb = mul[0] > 0.001f || mul[0] < -0.001f || mul[1] > 0.001f || mul[1] < -0.001f ||
                  mul[2] > 0.001f || mul[2] < -0.001f;
    cc->tex_alpha = mul[3] > 0.001f || mul[3] < -0.001f;
}

static inline Lin input(uint8_t in, int ch, const Lin* comb, const GfxCCConst* k, const float* shade) {
    switch (in) {
        case CC_1: return (Lin){ 0.0f, 1.0f };
        case CC_HALF: return (Lin){ 0.0f, 0.5f };
        case CC_COMBINED: return comb[ch];
        case CC_COMBINED_A: return comb[3];
        case CC_TEXEL:
        case CC_TEXEL_A: return (Lin){ 1.0f, 0.0f };
        case CC_PRIM: return (Lin){ 0.0f, k->prim[ch] };
        case CC_PRIM_A: return (Lin){ 0.0f, k->prim[3] };
        case CC_SHADE: return (Lin){ 0.0f, shade[ch] };
        case CC_SHADE_A: return (Lin){ 0.0f, shade[3] };
        case CC_ENV: return (Lin){ 0.0f, k->env[ch] };
        case CC_ENV_A: return (Lin){ 0.0f, k->env[3] };
        case CC_LOD: return (Lin){ 0.0f, 1.0f };
        case CC_PRIM_LOD: return (Lin){ 0.0f, k->prim_lod };
        default: return (Lin){ 0.0f, 0.0f };
    }
}

void cc_eval(const GfxCombiner* cc, const GfxCCConst* k, const float shade[4], float mul[4], float add[4]) {
    Lin comb[4] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    int first = cc->two_cycle ? 0 : 1;
    for (int c = first; c < 2; c++) {
        Lin res[4];
        for (int ch = 0; ch < 4; ch++) {
            const uint8_t* in = cc->in[c][ch == 3];
            Lin a = input(in[0], ch, comb, k, shade);
            Lin b = input(in[1], ch, comb, k, shade);
            Lin m = input(in[2], ch, comb, k, shade);
            Lin d = input(in[3], ch, comb, k, shade);
            float sm = a.m - b.m;
            float so = a.o - b.o;
            res[ch].m = sm * m.m + sm * m.o + so * m.m + d.m;
            res[ch].o = so * m.o + d.o;
        }
        for (int ch = 0; ch < 4; ch++) {
            comb[ch] = res[ch];
        }
    }
    for (int ch = 0; ch < 4; ch++) {
        mul[ch] = comb[ch].m;
        add[ch] = comb[ch].o;
    }
}

// The same evaluation with the shade left as variables: each channel's mul and add become
// polynomials c0 + c1 x + c2 a + c3 x a in its shade x and the shade alpha a (x is a for the
// alpha channel), evaluated per vertex by cc_run. Higher powers aren't representable, and
// those combiners keep using cc_eval.

typedef struct {
    float c[4];
} Poly;

typedef struct {
    Poly m, o;
} PLin;

static Poly poly_k(float k) {
    return (Poly){ { k, 0.0f, 0.0f, 0.0f } };
}

static Poly poly_add(Poly p, Poly q) {
    for (int i = 0; i < 4; i++) {
        p.c[i] += q.c[i];
    }
    return p;
}

static Poly poly_sub(Poly p, Poly q) {
    for (int i = 0; i < 4; i++) {
        p.c[i] -= q.c[i];
    }
    return p;
}

// Monomial i is x^(i & 1) a^(i >> 1); a product that needs x^2 or a^2 fails.
static Poly poly_mul(Poly p, Poly q, bool* ok) {
    Poly r = poly_k(0.0f);
    for (int i = 0; i < 4; i++) {
        if (p.c[i] == 0.0f) {
            continue;
        }
        for (int j = 0; j < 4; j++) {
            if (q.c[j] == 0.0f) {
                continue;
            }
            if (i & j) {
                *ok = false;
                return r;
            }
            r.c[i | j] += p.c[i] * q.c[j];
        }
    }
    return r;
}

static PLin pinput(uint8_t in, int ch, const PLin* comb, const GfxCCConst* k) {
    static const Poly shade_x = { { 0.0f, 1.0f, 0.0f, 0.0f } };
    static const Poly shade_a = { { 0.0f, 0.0f, 1.0f, 0.0f } };
    Poly zero = poly_k(0.0f);
    switch (in) {
        case CC_1: return (PLin){ zero, poly_k(1.0f) };
        case CC_HALF: return (PLin){ zero, poly_k(0.5f) };
        case CC_COMBINED: return comb[ch];
        case CC_COMBINED_A: return comb[3];
        case CC_TEXEL:
        case CC_TEXEL_A: return (PLin){ poly_k(1.0f), zero };
        case CC_PRIM: return (PLin){ zero, poly_k(k->prim[ch]) };
        case CC_PRIM_A: return (PLin){ zero, poly_k(k->prim[3]) };
        case CC_SHADE: return (PLin){ zero, ch == 3 ? shade_a : shade_x };
        case CC_SHADE_A: return (PLin){ zero, shade_a };
        case CC_ENV: return (PLin){ zero, poly_k(k->env[ch]) };
        case CC_ENV_A: return (PLin){ zero, poly_k(k->env[3]) };
        case CC_LOD: return (PLin){ zero, poly_k(1.0f) };
        case CC_PRIM_LOD: return (PLin){ zero, poly_k(k->prim_lod) };
        default: return (PLin){ zero, zero };
    }
}

void cc_prepare(const GfxCombiner* cc, const GfxCCConst* k, GfxCCProg* prog) {
    PLin comb[4];
    for (int ch = 0; ch < 4; ch++) {
        comb[ch].m = comb[ch].o = poly_k(0.0f);
    }
    bool ok = true;
    int first = cc->two_cycle ? 0 : 1;
    for (int c = first; c < 2; c++) {
        PLin res[4];
        for (int ch = 0; ch < 4; ch++) {
            const uint8_t* in = cc->in[c][ch == 3];
            PLin a = pinput(in[0], ch, comb, k);
            PLin b = pinput(in[1], ch, comb, k);
            PLin m = pinput(in[2], ch, comb, k);
            PLin d = pinput(in[3], ch, comb, k);
            Poly sm = poly_sub(a.m, b.m);
            Poly so = poly_sub(a.o, b.o);
            res[ch].m = poly_add(poly_add(poly_mul(sm, m.m, &ok), poly_mul(sm, m.o, &ok)),
                                 poly_add(poly_mul(so, m.m, &ok), d.m));
            res[ch].o = poly_add(poly_mul(so, m.o, &ok), d.o);
        }
        for (int ch = 0; ch < 4; ch++) {
            comb[ch] = res[ch];
        }
    }
    prog->ok = ok;
    for (int ch = 0; ch < 4; ch++) {
        for (int i = 0; i < 4; i++) {
            prog->m[ch][i] = comb[ch].m.c[i];
            prog->o[ch][i] = comb[ch].o.c[i];
        }
    }
}
