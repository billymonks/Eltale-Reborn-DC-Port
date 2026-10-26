// Shared state of the F3DEX/RDP renderer (gfx_*.c).
#ifndef Q64DC_GFX_INTERNAL_H
#define Q64DC_GFX_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <dc/pvr.h>

#include "ultra.h"

// N64 320x240 to the Dreamcast's 640x480.
#define GFX_SCALE 2.0f

// RDP image formats and sizes.
#define G_IM_FMT_RGBA 0
#define G_IM_FMT_YUV  1
#define G_IM_FMT_CI   2
#define G_IM_FMT_IA   3
#define G_IM_FMT_I    4

#define G_IM_SIZ_4b  0
#define G_IM_SIZ_8b  1
#define G_IM_SIZ_16b 2
#define G_IM_SIZ_32b 3

// Texture lookup table type (othermode H bits 14-15).
#define G_TT_NONE   0
#define G_TT_RGBA16 2
#define G_TT_IA16   3

typedef struct {
    uint8_t fmt, siz, palette;
    uint8_t cms, cmt, masks, maskt, shifts, shiftt;
    uint16_t line;  // row stride, in 64-bit words
    uint16_t tmem;  // in 64-bit words
    uint16_t uls, ult, lrs, lrt;  // 10.2 fixed point
} GfxTile;

typedef struct {
    uint32_t addr;
    uint8_t fmt, siz;
    uint16_t width;
} GfxImage;

extern GfxTile gfx_tiles[8];
extern GfxImage gfx_timg;

// Texture memory loads (gfx_tex.c).
void tmem_load_block(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt);
void tmem_load_tile(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);
void tmem_load_tlut(int tile, uint32_t count);

// How the combiner uses the texture, which picks the decoded format.
enum {
    TEXVAR_NORMAL,     // color and alpha
    TEXVAR_NOALPHA,    // color only: RGB565
    TEXVAR_ALPHAONLY,  // alpha only: white ARGB4444
};

typedef struct {
    pvr_ptr_t ptr;
    uint32_t pvr_fmt;
    uint16_t pw, ph;  // PVR texture size (powers of two)
    bool clamp_s, clamp_t, mirror_s, mirror_t;
} GfxTexture;

bool tex_lookup(int tile, int tlut, int variant, GfxTexture* out);
#ifdef GFX_OPT
// tex_lookup's usual case, a texture already in the cache, in compact code; the rest goes to it.
bool tex_lookup_fast(int tile, int tlut, int variant, GfxTexture* out);
#endif
void tex_init(void);
void tex_frame_begin(void);
void tex_frame_end(void);
uint32_t tex_vram_free(void);  // bytes of the cache's texture memory not holding a texture
void tex_print_stats(void);
extern uint32_t stat_tex_uploads;
extern uint32_t stat_tex_entries;
extern uint32_t stat_tmem_copies;    // loads copied into TMEM
extern uint32_t stat_tex_fallbacks;  // lookups that hashed TMEM

// Color combiner (gfx_cc.c): evaluates (A - B) * C + D for both cycles as tex * mul + add,
// treating the texel as a variable and everything else as known per vertex.
enum {
    CC_0, CC_1, CC_HALF, CC_COMBINED, CC_COMBINED_A, CC_TEXEL, CC_TEXEL_A,
    CC_PRIM, CC_PRIM_A, CC_SHADE, CC_SHADE_A, CC_ENV, CC_ENV_A, CC_LOD, CC_PRIM_LOD,
};

typedef struct {
    uint32_t w0, w1;
    bool two_cycle;
    uint8_t in[2][2][4];  // [cycle][color, alpha][A, B, C, D]
    bool tex_rgb, tex_alpha;
} GfxCombiner;

typedef struct {
    float prim[4], env[4];
    float prim_lod;
} GfxCCConst;

void cc_compile(GfxCombiner* cc, uint32_t w0, uint32_t w1, bool two_cycle);
void cc_eval(const GfxCombiner* cc, const GfxCCConst* k, const float shade[4], float mul[4], float add[4]);

// cc_eval with the constants folded in, for one combiner and one set of constants: per channel,
// mul and add as c0 + c1 x + c2 a + c3 x a in the channel's shade x and the shade alpha a.
// Only valid when ok; otherwise use cc_eval.
typedef struct {
    float m[4][4], o[4][4];  // [channel][coefficient]
    bool ok;
} GfxCCProg;

void cc_prepare(const GfxCombiner* cc, const GfxCCConst* k, GfxCCProg* prog);

static inline void cc_run(const GfxCCProg* p, const float shade[4], float mul[4], float add[4]) {
    float a = shade[3];
    for (int ch = 0; ch < 4; ch++) {
        float x = shade[ch];
        float xa = x * a;
        mul[ch] = p->m[ch][0] + p->m[ch][1] * x + p->m[ch][2] * a + p->m[ch][3] * xa;
        add[ch] = p->o[ch][0] + p->o[ch][1] * x + p->o[ch][2] * a + p->o[ch][3] * xa;
    }
}

#ifdef GFX_OPT
// Faster versions of the renderer's busiest paths, with the same results. GFX_DIAG builds switch
// each one at run time (gfx_opt); other GFX_OPT builds always take them.
enum {
    OPT_TEX = 1,     // T: texture lookups that hit the cache, in compact code
    OPT_SUM = 2,     // S: RDRAM checksums a cache line at a time, prefetching further ahead
    OPT_LOAD = 4,    // L: the TMEM load queue drops hidden loads at once instead of keeping them
    OPT_VTX = 8,     // V: vertex loads in their own tight loop
    OPT_COLOR = 16,  // C: lighting and vertex colors in one compact path
    OPT_TRI = 32,    // D: runs of triangle commands in a compact loop, display lists prefetched
    OPT_ALL = 63,
};
#ifdef GFX_DIAG
extern uint32_t gfx_opt;
#define GFX_OPT_ON(bit) ((gfx_opt & (bit)) != 0)
#else
#define GFX_OPT_ON(bit) true
#endif
#endif

#ifdef GFX_XCHECK
// Investigation builds: each GFX_OPT path also runs the original on the same input and counts
// any difference, printed once a second.
enum { XC_TEX, XC_SUM, XC_LOAD, XC_VTX, XC_LIGHT, XC_OUT, XC_TRI, XC_COUNT };
extern uint32_t gfx_xcheck_runs[XC_COUNT], gfx_xcheck_fails[XC_COUNT];
#endif

#ifdef GFX_DIAG
// Diagnostic modes, each taking one kind of work out while the game runs (gfx.c cycles them).
enum {
    DIAG_NORMAL,
    DIAG_NO_UPLOAD,    // textures decoded into RAM but not copied to VRAM
    DIAG_NO_TEXTURE,   // no texture lookups, decodes, uploads or TMEM copies: one fixed texture
    DIAG_NO_CHECKSUM,  // the RDRAM sums of texture loads kept from the first frame that read them
    DIAG_NO_TA,        // the opaque list written to RAM instead of the tile accelerator
    DIAG_NO_DMA,       // punch-through and translucent lists built but not sent (the HUD still is)
    DIAG_NO_LIGHT,     // no lighting, fog or combiner math: one vertex color
    DIAG_NO_TRI,       // triangle commands skipped
    DIAG_MODES,
};
extern int gfx_diag;  // the mode of the frame being built

// Texture work since boot, for the HUD.
typedef struct {
    uint32_t lookups, decodes, sum_bytes, upload_bytes;
} GfxDiagTex;
extern GfxDiagTex gfx_diag_tex;
// Mixed into every texture's key; changing it makes every cached texture miss.
extern uint32_t gfx_diag_salt;
#endif

#endif
