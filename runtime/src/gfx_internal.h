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
void tex_init(void);
void tex_frame_begin(void);
extern uint32_t stat_tex_uploads;
extern uint32_t stat_tex_entries;

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

#endif
