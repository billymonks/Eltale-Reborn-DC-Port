// F3DEX 1.23 and RDP command interpreter, drawing with the PowerVR.
//
// The RSP part (matrices, vertices, lighting, fog, clipping) runs on the SH-4, using its
// matrix unit for the vertex transform. Every primitive goes to the translucent list with
// presorting disabled, so the PowerVR draws in submission order like the RDP, with depth
// tests and writes chosen per primitive from the RDP's render mode.

#include <kos.h>
#include <dc/matrix.h>
#include <dc/pvr.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "gfx_internal.h"
#include "test.h"

// Geometry mode.
#define G_ZBUFFER            0x00000001
#define G_SHADE              0x00000004
#define G_SHADING_SMOOTH     0x00000200
#define G_CULL_FRONT         0x00001000
#define G_CULL_BACK          0x00002000
#define G_FOG                0x00010000
#define G_LIGHTING           0x00020000
#define G_TEXTURE_GEN        0x00040000
#define G_TEXTURE_GEN_LINEAR 0x00080000

// Other mode L.
#define G_AC_THRESHOLD 1
#define Z_CMP          0x0010
#define Z_UPD          0x0020
#define ZMODE_MASK     0x0C00
#define ZMODE_XLU      0x0800
#define CVG_X_ALPHA    0x1000
#define ALPHA_CVG_SEL  0x2000
#define FORCE_BL       0x4000

#define CYCLE_1    0
#define CYCLE_2    1
#define CYCLE_COPY 2
#define CYCLE_FILL 3

#define CLIP_NEAR 0x10

#define MV_STACK_SIZE 16
#define DL_STACK_SIZE 32
#define MAX_LIGHTS    8

// Stop adding to the frame before the PowerVR's vertex buffer overflows.
#define VERTEX_BUF_SIZE (1024 * 1024)
#define VERTEX_BUF_LIMIT (VERTEX_BUF_SIZE - 64 * 1024)

typedef struct {
    float x, y, z, w;  // clip space
    float s, t;        // texels, before the tile's shift and origin
    float shade[4];
    float mul[4], add[4];  // combiner output, valid when cc_epoch matches
    uint32_t cc_epoch;
    uint8_t clip;
    bool has_ofs;
    float sx, sy, sz;            // projected, valid unless clipped by the near plane
    float u, v;                  // PowerVR output, valid when out_gen matches
    uint32_t argb, oargb;
    uint32_t out_gen;
} Vertex;

// A vertex on its way to the PowerVR, after the combiner and before projection.
typedef struct {
    float x, y, z, w;
    float s, t;
    float col[4], ofs[4];
} ClipVert;

typedef struct {
    pvr_ptr_t tex;
    uint32_t tex_fmt;
    uint16_t pw, ph;
    uint8_t filter, clamp, flip, txralpha_dis;
    uint8_t blend, fog, offset, depth_cmp, depth_write;
} HeaderKey;

// How the current material maps onto the PowerVR, worked out when the RDP state changes.
typedef struct {
    bool textured;
    int variant;
    GfxTexture tex;
    float shift_s, shift_t, uls, ult;
    float inv_pw, inv_ph;
    bool blend, fog, depth_cmp, depth_write;
    bool filter;
} Material;

static struct {
    uint32_t segments[16];
    matrix_t mv[MV_STACK_SIZE];
    int mv_top;
    matrix_t proj;
    bool ortho;
    bool mvp_dirty;
    bool lights_dirty;

    Vertex vtx[32];
    uint32_t geom;

    int num_lights;
    float light_col[MAX_LIGHTS + 1][3];
    float light_dir[MAX_LIGHTS + 1][3];
    float light_coeff[MAX_LIGHTS][3];
    float lookat_dir[2][3];
    float lookat_coeff[2][3];

    float fog_mul, fog_off;
    float vp_sx, vp_sy, vp_tx, vp_ty;

    bool tex_on;
    int tex_tile;
    float tex_ss, tex_st;
    uint32_t tex_scale_raw_s, tex_scale_raw_t;

    uint32_t oh, ol;
    uint32_t cc_w0, cc_w1;
    GfxCombiner cc;
    GfxCCConst k;
    uint32_t cc_epoch;
    uint32_t fog_color, fill_color;

    uint32_t cimg, zimg;
    uint8_t cimg_siz;
    float sc_x0, sc_y0, sc_x1, sc_y1;
    uint32_t rdphalf_1;

    uint32_t tmem_gen;  // bumped by every TMEM load
    bool mat_dirty;
    Material mat;
    bool mat_for_rect;
    int mat_rect_tile;
    HeaderKey mat_key;     // header for the material, without the offset flag
    bool mat_key_changed;
    uint32_t out_gen;      // bumped when cached vertex outputs go stale
} g;

static matrix_t mvp __attribute__((aligned(32)));

static pvr_dr_state_t dr;
static HeaderKey cur_key;
static bool have_key;
static uint32_t vbuf_used;

static uint32_t stat_tris, stat_headers, stat_frame_tris, stat_tex_lookups, stat_mat_updates;
static bool warned_op[256];

static inline float u8f(uint32_t v) {
    return (float)(v & 0xFF) * (1.0f / 255.0f);
}

static inline uint32_t seg_addr(uint32_t a) {
    uint32_t phys = (g.segments[(a >> 24) & 0xF] + (a & 0x00FFFFFF)) & 0x1FFFFFFF;
    return 0x80000000u | phys;
}

static inline bool addr_ok(uint32_t vaddr, uint32_t len) {
    uint32_t off = vaddr - 0x80000000u;
    return off < RDRAM_SIZE && len <= RDRAM_SIZE - off;
}

static inline uint32_t rd32(uint32_t vaddr) {
    return *(uint32_t*)RDRAM_PTR(vaddr);
}

static inline uint32_t rd16(uint32_t vaddr) {
    return *(uint16_t*)RDRAM_PTR(vaddr ^ 2);
}

static inline uint8_t rd8(uint32_t vaddr) {
    return *(uint8_t*)RDRAM_PTR(vaddr ^ 3);
}

static void unpack_rgba(uint32_t c, float out[4]) {
    out[0] = u8f(c >> 24);
    out[1] = u8f(c >> 16);
    out[2] = u8f(c >> 8);
    out[3] = u8f(c);
}

// --- Matrices and lights ---

static void read_matrix(uint32_t addr, matrix_t out) {
    for (int i = 0; i < 16; i++) {
        int32_t hi = (int16_t)rd16(addr + i * 2);
        uint32_t lo = rd16(addr + 0x20 + i * 2);
        out[i / 4][i % 4] = (float)(int32_t)(((uint32_t)hi << 16) | lo) * (1.0f / 65536.0f);
    }
}

// Row-vector convention, as on the N64: res = a * b.
static void mat_mul(matrix_t res, matrix_t a, matrix_t b) {
    matrix_t tmp;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

static void normalize3(float v[3]) {
    float len = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (len > 0.0f) {
        float inv = 1.0f / sqrtf(len);
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    }
}

// Light directions in model space, so vertex normals can be used untransformed.
static void model_space_dir(const float dir[3], float out[3]) {
    matrix_t* m = &g.mv[g.mv_top];
    for (int i = 0; i < 3; i++) {
        out[i] = dir[0] * (*m)[i][0] + dir[1] * (*m)[i][1] + dir[2] * (*m)[i][2];
    }
    normalize3(out);
}

static void update_mvp(void) {
    mat_mul(mvp, g.mv[g.mv_top], g.proj);
    g.ortho = g.proj[0][3] == 0.0f && g.proj[1][3] == 0.0f && g.proj[2][3] == 0.0f && g.proj[3][3] != 0.0f;
    g.mvp_dirty = false;
}

static void update_lights(void) {
    for (int i = 0; i < g.num_lights; i++) {
        model_space_dir(g.light_dir[i], g.light_coeff[i]);
    }
    model_space_dir(g.lookat_dir[0], g.lookat_coeff[0]);
    model_space_dir(g.lookat_dir[1], g.lookat_coeff[1]);
    g.lights_dirty = false;
}

// --- Vertices ---

static inline void project(Vertex* v, float winv) {
    v->sx = v->x * winv * g.vp_sx + g.vp_tx;
    v->sy = -v->y * winv * g.vp_sy + g.vp_ty;
    v->sz = g.ortho ? 1.0f - v->z * winv * 0.01f : winv;
}

static void load_vertices(uint32_t addr, uint32_t first, uint32_t count) {
    if (first + count > 32 || !addr_ok(addr, count * 16)) {
        return;
    }
    if (g.mvp_dirty) {
        update_mvp();
    }
    bool lighting = (g.geom & G_LIGHTING) != 0;
    if (lighting && g.lights_dirty) {
        update_lights();
    }
    mat_load(&mvp);

    for (uint32_t i = 0; i < count; i++) {
        Vertex* v = &g.vtx[first + i];
        uint32_t a = addr + i * 16;
        uint32_t w0 = rd32(a), w1 = rd32(a + 4), w2 = rd32(a + 8), w3 = rd32(a + 12);

        float x = (float)(int16_t)(w0 >> 16);
        float y = (float)(int16_t)w0;
        float z = (float)(int16_t)(w1 >> 16);
        float w = 1.0f;
        mat_trans_nodiv(x, y, z, w);
        v->x = x;
        v->y = y;
        v->z = z;
        v->w = w;

        uint8_t clip = 0;
        if (x < -w) clip |= 0x01;
        if (x > w) clip |= 0x02;
        if (y < -w) clip |= 0x04;
        if (y > w) clip |= 0x08;
        if (z < -w || w <= 0.0f) clip |= CLIP_NEAR;
        v->clip = clip;

        float s = (float)(int16_t)(w2 >> 16) * g.tex_ss * (1.0f / 32.0f);
        float t = (float)(int16_t)w2 * g.tex_st * (1.0f / 32.0f);

        if (lighting) {
            float n[3] = { (float)(int8_t)(w3 >> 24), (float)(int8_t)(w3 >> 16), (float)(int8_t)(w3 >> 8) };
            float r = g.light_col[g.num_lights][0];
            float gg = g.light_col[g.num_lights][1];
            float b = g.light_col[g.num_lights][2];
            for (int l = 0; l < g.num_lights; l++) {
                float d = (n[0] * g.light_coeff[l][0] + n[1] * g.light_coeff[l][1] + n[2] * g.light_coeff[l][2]) *
                          (1.0f / 127.0f);
                if (d > 0.0f) {
                    r += d * g.light_col[l][0];
                    gg += d * g.light_col[l][1];
                    b += d * g.light_col[l][2];
                }
            }
            v->shade[0] = r > 1.0f ? 1.0f : r;
            v->shade[1] = gg > 1.0f ? 1.0f : gg;
            v->shade[2] = b > 1.0f ? 1.0f : b;
            if (g.geom & G_TEXTURE_GEN) {
                float dx = n[0] * g.lookat_coeff[0][0] + n[1] * g.lookat_coeff[0][1] + n[2] * g.lookat_coeff[0][2];
                float dy = n[0] * g.lookat_coeff[1][0] + n[1] * g.lookat_coeff[1][1] + n[2] * g.lookat_coeff[1][2];
                s = (dx * (1.0f / 127.0f) + 1.0f) * 0.25f * (float)g.tex_scale_raw_s * (1.0f / 32.0f);
                t = (dy * (1.0f / 127.0f) + 1.0f) * 0.25f * (float)g.tex_scale_raw_t * (1.0f / 32.0f);
            }
        } else {
            v->shade[0] = u8f(w3 >> 24);
            v->shade[1] = u8f(w3 >> 16);
            v->shade[2] = u8f(w3 >> 8);
        }
        v->shade[3] = u8f(w3);
        v->s = s;
        v->t = t;

        float winv = w > 0.0f ? 1.0f / w : 32767.0f;
        if (g.geom & G_FOG) {
            float f = z * winv * g.fog_mul + g.fog_off;
            f = f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f);
            v->shade[3] = f * (1.0f / 255.0f);
        }
        project(v, winv);
        v->cc_epoch = 0;
        v->out_gen = 0;
    }
}

// --- Materials and PowerVR headers ---

static void blend_mode(int cycle, bool* blend, bool* fog) {
    uint32_t l = g.ol;
    *fog = false;
    *blend = false;
    if (cycle == CYCLE_FILL) {
        return;
    }
    if (cycle == CYCLE_COPY) {
        *blend = (l & 3) != 0;
        return;
    }
    uint32_t p1 = (l >> 30) & 3, a1 = (l >> 26) & 3, m1 = (l >> 22) & 3, b1 = (l >> 18) & 3;
    uint32_t p2 = (l >> 28) & 3, a2 = (l >> 24) & 3, m2 = (l >> 20) & 3, b2 = (l >> 16) & 3;
    // Cycle 1: fog color blended in by the shade alpha the RSP computed.
    *fog = p1 == 3 && a1 == 2;
    bool xlu1 = p1 == 0 && a1 == 0 && m1 == 1 && b1 == 0;
    bool xlu2 = p2 == 0 && a2 == 0 && m2 == 1 && b2 == 0;
    bool xlu = cycle == CYCLE_2 ? xlu2 : (xlu1 || xlu2);
    *blend = (xlu && (l & FORCE_BL)) || (l & 3) == G_AC_THRESHOLD || ((l & CVG_X_ALPHA) && (l & ALPHA_CVG_SEL)) ||
             (l & ZMODE_MASK) == ZMODE_XLU;
}

static float tile_shift(uint8_t shift) {
    if (shift == 0) {
        return 1.0f;
    }
    if (shift <= 10) {
        return 1.0f / (float)(1 << shift);
    }
    return (float)(1 << (16 - shift));
}

static void update_material(bool rect, int rect_tile) {
    Material* m = &g.mat;
    int cycle = (g.oh >> 20) & 3;
    int tlut = (g.oh >> 14) & 3;
    int tile = rect ? rect_tile : g.tex_tile;

    bool tex_rgb = g.cc.tex_rgb, tex_alpha = g.cc.tex_alpha;
    if (cycle == CYCLE_COPY) {
        tex_rgb = tex_alpha = true;
    }
    m->textured = cycle != CYCLE_FILL && (tex_rgb || tex_alpha) && (rect ? tile >= 0 : g.tex_on);
    m->variant = tex_rgb && tex_alpha ? TEXVAR_NORMAL : (tex_rgb ? TEXVAR_NOALPHA : TEXVAR_ALPHAONLY);
    if (m->textured) {
        // Hashing TMEM is the expensive part, so only look the texture up again when
        // something it depends on changed.
        static struct {
            GfxTile tile;
            uint32_t tmem_gen;
            int index, tlut, variant;
            bool ok;
            GfxTexture tex;
        } last;
        bool same = last.index == tile && last.tlut == tlut && last.variant == m->variant &&
                    last.tmem_gen == g.tmem_gen && memcmp(&last.tile, &gfx_tiles[tile], sizeof(GfxTile)) == 0 &&
                    g.tmem_gen != 0;
        if (!same) {
            last.tile = gfx_tiles[tile];
            last.tmem_gen = g.tmem_gen;
            last.index = tile;
            last.tlut = tlut;
            last.variant = m->variant;
            last.ok = tex_lookup(tile, tlut, m->variant, &last.tex);
            stat_tex_lookups++;
        }
        m->tex = last.tex;
        m->textured = last.ok;
    }
    if (m->textured) {
        const GfxTile* t = &gfx_tiles[tile];
        m->shift_s = tile_shift(t->shifts);
        m->shift_t = tile_shift(t->shiftt);
        m->uls = (float)t->uls * 0.25f;
        m->ult = (float)t->ult * 0.25f;
        m->inv_pw = 1.0f / (float)m->tex.pw;
        m->inv_ph = 1.0f / (float)m->tex.ph;
    }
    m->filter = cycle != CYCLE_COPY && ((g.oh >> 12) & 3) != 0;
    blend_mode(cycle, &m->blend, &m->fog);
    bool zb = !rect && (g.geom & G_ZBUFFER);
    m->depth_cmp = zb && (g.ol & Z_CMP);
    m->depth_write = zb && (g.ol & Z_UPD);

    HeaderKey* k = &g.mat_key;
    memset(k, 0, sizeof(*k));
    if (m->textured) {
        k->tex = m->tex.ptr;
        k->tex_fmt = m->tex.pvr_fmt;
        k->pw = m->tex.pw;
        k->ph = m->tex.ph;
        k->filter = m->filter;
        k->clamp = (m->tex.clamp_s ? PVR_UVCLAMP_U : 0) | (m->tex.clamp_t ? PVR_UVCLAMP_V : 0);
        k->flip = (m->tex.mirror_s ? PVR_UVFLIP_U : 0) | (m->tex.mirror_t ? PVR_UVFLIP_V : 0);
        k->txralpha_dis = m->variant == TEXVAR_NOALPHA;
    }
    k->blend = m->blend;
    k->fog = m->fog;
    k->depth_cmp = m->depth_cmp ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
    k->depth_write = m->depth_write;
    g.mat_key_changed = true;
    g.out_gen++;

    stat_mat_updates++;
    g.mat_dirty = false;
    g.mat_for_rect = rect;
    g.mat_rect_tile = rect_tile;
}

static void ensure_material(bool rect, int rect_tile) {
    if (g.mat_dirty || g.mat_for_rect != rect || (rect && g.mat_rect_tile != rect_tile)) {
        update_material(rect, rect_tile);
    }
}

static void submit_header(const HeaderKey* k) {
    if (have_key && memcmp(k, &cur_key, sizeof(*k)) == 0) {
        return;
    }
    cur_key = *k;
    have_key = true;

    pvr_poly_cxt_t cxt;
    if (k->tex != NULL) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, k->tex_fmt | PVR_TXRFMT_TWIDDLED, k->pw, k->ph, k->tex,
                         k->filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.alpha = k->txralpha_dis ? PVR_TXRALPHA_DISABLE : PVR_TXRALPHA_ENABLE;
        cxt.txr.uv_clamp = k->clamp;
        cxt.txr.uv_flip = k->flip;
    } else {
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.alpha = k->blend ? PVR_ALPHA_ENABLE : PVR_ALPHA_DISABLE;
    cxt.blend.src = k->blend ? PVR_BLEND_SRCALPHA : PVR_BLEND_ONE;
    cxt.blend.dst = k->blend ? PVR_BLEND_INVSRCALPHA : PVR_BLEND_ZERO;
    cxt.gen.fog_type = k->fog ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
    cxt.gen.specular = k->offset ? PVR_SPECULAR_ENABLE : PVR_SPECULAR_DISABLE;
    cxt.depth.comparison = k->depth_cmp;
    cxt.depth.write = k->depth_write ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;

    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);
    uint32_t* d = (uint32_t*)pvr_dr_target(dr);
    const uint32_t* s = (const uint32_t*)&hdr;
    for (int i = 0; i < 8; i++) {
        d[i] = s[i];
    }
    pvr_dr_commit(d);
    vbuf_used += 32;
    stat_headers++;
}

static inline uint32_t pack_color(const float c[4]) {
    uint32_t r = c[0] <= 0.0f ? 0 : (c[0] >= 1.0f ? 255 : (uint32_t)(c[0] * 255.0f));
    uint32_t gg = c[1] <= 0.0f ? 0 : (c[1] >= 1.0f ? 255 : (uint32_t)(c[1] * 255.0f));
    uint32_t b = c[2] <= 0.0f ? 0 : (c[2] >= 1.0f ? 255 : (uint32_t)(c[2] * 255.0f));
    uint32_t a = c[3] <= 0.0f ? 0 : (c[3] >= 1.0f ? 255 : (uint32_t)(c[3] * 255.0f));
    return (a << 24) | (r << 16) | (gg << 8) | b;
}

// Turns the combiner's tex * mul + add into the PowerVR's texture * color + offset.
static void finish_colors(const float mul[4], const float add[4], float fog, ClipVert* out) {
    const Material* m = &g.mat;
    if (!m->textured) {
        for (int i = 0; i < 4; i++) {
            out->col[i] = add[i];
            out->ofs[i] = 0.0f;
        }
    } else if (m->variant == TEXVAR_ALPHAONLY) {
        out->col[0] = add[0];
        out->col[1] = add[1];
        out->col[2] = add[2];
        out->col[3] = mul[3] + add[3];
        out->ofs[0] = out->ofs[1] = out->ofs[2] = 0.0f;
    } else {
        out->col[0] = mul[0];
        out->col[1] = mul[1];
        out->col[2] = mul[2];
        out->col[3] = m->variant == TEXVAR_NOALPHA ? add[3] : mul[3] + add[3];
        out->ofs[0] = add[0];
        out->ofs[1] = add[1];
        out->ofs[2] = add[2];
    }
    out->ofs[3] = fog;
}

static inline void emit_vertex(const ClipVert* c, bool eol, bool screen) {
    pvr_vertex_t* d = pvr_dr_target(dr);
    const Material* m = &g.mat;
    float x, y, z;
    if (screen) {
        x = c->x;
        y = c->y;
        z = c->z;
    } else {
        float inv = 1.0f / c->w;
        x = c->x * inv * g.vp_sx + g.vp_tx;
        y = -c->y * inv * g.vp_sy + g.vp_ty;
        z = g.ortho ? 1.0f - c->z * inv * 0.01f : inv;
    }
    d->flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    d->x = x;
    d->y = y;
    d->z = z;
    if (m->textured) {
        d->u = (c->s * m->shift_s - m->uls) * m->inv_pw;
        d->v = (c->t * m->shift_t - m->ult) * m->inv_ph;
    } else {
        d->u = 0.0f;
        d->v = 0.0f;
    }
    d->argb = pack_color(c->col);
    d->oargb = pack_color(c->ofs);
    pvr_dr_commit(d);
}

// The header for the current material; only compared against the last one sent when the
// material or the offset color changed.
static void submit_material_header(bool offset) {
    if (!g.mat_key_changed && have_key && cur_key.offset == offset) {
        return;
    }
    HeaderKey k = g.mat_key;
    k.offset = offset;
    g.mat_key_changed = false;
    submit_header(&k);
}

static void submit_polygon(const ClipVert* v, int n, bool screen) {
    if (vbuf_used + (uint32_t)n * 32 + 32 > VERTEX_BUF_LIMIT) {
        return;
    }
    bool offset = false;
    for (int i = 0; i < n && !offset && g.mat.textured; i++) {
        offset = v[i].ofs[0] > 0.004f || v[i].ofs[1] > 0.004f || v[i].ofs[2] > 0.004f;
    }
    submit_material_header(offset);

    // Fan to strip: 0 1 2 for a triangle, 0 1 3 2 for a quad.
    if (n == 3) {
        emit_vertex(&v[0], false, screen);
        emit_vertex(&v[1], false, screen);
        emit_vertex(&v[2], true, screen);
    } else {
        emit_vertex(&v[0], false, screen);
        emit_vertex(&v[1], false, screen);
        emit_vertex(&v[3], false, screen);
        emit_vertex(&v[2], true, screen);
    }
    vbuf_used += (uint32_t)n * 32;
}

static void lerp_vert(const ClipVert* a, const ClipVert* b, float t, ClipVert* out) {
    out->x = a->x + (b->x - a->x) * t;
    out->y = a->y + (b->y - a->y) * t;
    out->z = a->z + (b->z - a->z) * t;
    out->w = a->w + (b->w - a->w) * t;
    out->s = a->s + (b->s - a->s) * t;
    out->t = a->t + (b->t - a->t) * t;
    for (int i = 0; i < 4; i++) {
        out->col[i] = a->col[i] + (b->col[i] - a->col[i]) * t;
        out->ofs[i] = a->ofs[i] + (b->ofs[i] - a->ofs[i]) * t;
    }
}

// Clips against the near plane (z = -w), giving up to 4 vertices.
static int clip_near(const ClipVert* in, ClipVert* out) {
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const ClipVert* a = &in[i];
        const ClipVert* b = &in[(i + 1) % 3];
        float da = a->z + a->w, db = b->z + b->w;
        if (da >= 0.0f) {
            out[n++] = *a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            lerp_vert(a, b, da / (da - db), &out[n++]);
        }
    }
    return n;
}

static void ensure_combined(Vertex* v) {
    if (v->cc_epoch != g.cc_epoch) {
        cc_eval(&g.cc, &g.k, v->shade, v->mul, v->add);
        v->cc_epoch = g.cc_epoch;
    }
}

// The PowerVR vertex for an unclipped corner, kept until the material or combiner changes.
static void ensure_output(Vertex* v) {
    if (v->out_gen == g.out_gen) {
        return;
    }
    ensure_combined(v);
    ClipVert c;
    finish_colors(v->mul, v->add, g.mat.fog ? v->shade[3] : 0.0f, &c);
    v->argb = pack_color(c.col);
    v->oargb = pack_color(c.ofs);
    v->has_ofs = g.mat.textured && (c.ofs[0] > 0.004f || c.ofs[1] > 0.004f || c.ofs[2] > 0.004f);
    if (g.mat.textured) {
        v->u = (v->s * g.mat.shift_s - g.mat.uls) * g.mat.inv_pw;
        v->v = (v->t * g.mat.shift_t - g.mat.ult) * g.mat.inv_ph;
    }
    v->out_gen = g.out_gen;
}

static inline void emit_cached(const Vertex* v, uint32_t argb, uint32_t oargb, bool eol) {
    pvr_vertex_t* d = pvr_dr_target(dr);
    d->flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    d->x = v->sx;
    d->y = v->sy;
    d->z = v->sz;
    d->u = v->u;
    d->v = v->v;
    d->argb = argb;
    d->oargb = oargb;
    pvr_dr_commit(d);
}

static void draw_triangle(int i0, int i1, int i2) {
    Vertex* v[3] = { &g.vtx[i0], &g.vtx[i1], &g.vtx[i2] };
    if (g.cimg == g.zimg) {
        return;
    }
    if (v[0]->clip & v[1]->clip & v[2]->clip) {
        return;
    }
    ensure_material(false, 0);

    // Back/front face culling in screen space (y down), when no vertex is behind the camera.
    uint32_t cull = g.geom & (G_CULL_FRONT | G_CULL_BACK);
    bool near = (v[0]->clip | v[1]->clip | v[2]->clip) & CLIP_NEAR;
    if (cull && !near) {
        float cross = (v[1]->sx - v[0]->sx) * (v[2]->sy - v[0]->sy) - (v[1]->sy - v[0]->sy) * (v[2]->sx - v[0]->sx);
        if ((cull & G_CULL_BACK) && cross >= 0.0f) {
            return;
        }
        if ((cull & G_CULL_FRONT) && cross <= 0.0f) {
            return;
        }
    }

    bool flat = !(g.geom & G_SHADING_SMOOTH);
    stat_frame_tris++;
    if (!near) {
        if (vbuf_used + 3 * 32 + 32 > VERTEX_BUF_LIMIT) {
            return;
        }
        ensure_output(v[0]);
        ensure_output(v[1]);
        ensure_output(v[2]);
        bool offset = flat ? v[0]->has_ofs : (v[0]->has_ofs || v[1]->has_ofs || v[2]->has_ofs);
        submit_material_header(offset);
        if (flat) {
            emit_cached(v[0], v[0]->argb, v[0]->oargb, false);
            emit_cached(v[1], v[0]->argb, (v[0]->oargb & 0x00FFFFFF) | (v[1]->oargb & 0xFF000000), false);
            emit_cached(v[2], v[0]->argb, (v[0]->oargb & 0x00FFFFFF) | (v[2]->oargb & 0xFF000000), true);
        } else {
            emit_cached(v[0], v[0]->argb, v[0]->oargb, false);
            emit_cached(v[1], v[1]->argb, v[1]->oargb, false);
            emit_cached(v[2], v[2]->argb, v[2]->oargb, true);
        }
        vbuf_used += 3 * 32;
        return;
    }

    ClipVert cv[3];
    for (int i = 0; i < 3; i++) {
        Vertex* src = flat ? v[0] : v[i];
        ensure_combined(src);
        cv[i].x = v[i]->x;
        cv[i].y = v[i]->y;
        cv[i].z = v[i]->z;
        cv[i].w = v[i]->w;
        cv[i].s = v[i]->s;
        cv[i].t = v[i]->t;
        finish_colors(src->mul, src->add, g.mat.fog ? v[i]->shade[3] : 0.0f, &cv[i]);
    }
    ClipVert clipped[4];
    int n = clip_near(cv, clipped);
    if (n >= 3) {
        submit_polygon(clipped, n, false);
    }
}

// Fill and texture rectangles, in N64 pixels.
static void draw_rect(float x0, float y0, float x1, float y1, int tile, float s, float t, float dsdx, float dtdy,
                      bool flip, bool textured) {
    if (g.cimg == g.zimg) {
        return;
    }
    int cycle = (g.oh >> 20) & 3;
    if (cycle == CYCLE_FILL || cycle == CYCLE_COPY) {
        x1 += 1.0f;
        y1 += 1.0f;
    }
    if (cycle == CYCLE_COPY) {
        dsdx *= 0.25f;
    }

    // Scissor, moving the texture origin along with the clipped edges.
    float cx0 = x0 < g.sc_x0 ? g.sc_x0 : x0;
    float cy0 = y0 < g.sc_y0 ? g.sc_y0 : y0;
    float cx1 = x1 > g.sc_x1 ? g.sc_x1 : x1;
    float cy1 = y1 > g.sc_y1 ? g.sc_y1 : y1;
    if (cx1 <= cx0 || cy1 <= cy0) {
        return;
    }
    if (!flip) {
        s += (cx0 - x0) * dsdx;
        t += (cy0 - y0) * dtdy;
    } else {
        s += (cy0 - y0) * dsdx;
        t += (cx0 - x0) * dtdy;
    }

    ensure_material(true, textured ? tile : -1);

    float mul[4], add[4];
    if (cycle == CYCLE_FILL) {
        uint32_t c = g.fill_color;
        if (g.cimg_siz == G_IM_SIZ_16b) {
            uint32_t p = c >> 16;
            uint32_t r = (p >> 11) & 0x1F, gg = (p >> 6) & 0x1F, b = (p >> 1) & 0x1F;
            c = (((r << 3) | (r >> 2)) << 24) | (((gg << 3) | (gg >> 2)) << 16) | (((b << 3) | (b >> 2)) << 8) | 0xFF;
        }
        unpack_rgba(c, add);
        mul[0] = mul[1] = mul[2] = mul[3] = 0.0f;
    } else if (cycle == CYCLE_COPY) {
        mul[0] = mul[1] = mul[2] = mul[3] = 1.0f;
        add[0] = add[1] = add[2] = add[3] = 0.0f;
    } else {
        static const float no_shade[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cc_eval(&g.cc, &g.k, no_shade, mul, add);
    }

    float w = cx1 - cx0, h = cy1 - cy0;
    ClipVert v[4];
    const float xs[4] = { cx0, cx1, cx1, cx0 };
    const float ys[4] = { cy0, cy0, cy1, cy1 };
    for (int i = 0; i < 4; i++) {
        float dx = xs[i] - cx0, dy = ys[i] - cy0;
        v[i].x = xs[i] * GFX_SCALE;
        v[i].y = ys[i] * GFX_SCALE;
        v[i].z = 1.0f;
        v[i].w = 1.0f;
        v[i].s = s + (flip ? dy : dx) * dsdx;
        v[i].t = t + (flip ? dx : dy) * dtdy;
        finish_colors(mul, add, 0.0f, &v[i]);
    }
    (void)w;
    (void)h;
    submit_polygon(v, 4, true);
}

// --- Display list ---

static void cc_changed(void) {
    g.cc_epoch++;
    g.mat_dirty = true;
}

static void set_combine(uint32_t w0, uint32_t w1) {
    bool two_cycle = ((g.oh >> 20) & 3) == CYCLE_2;
    static bool compiled;
    if (compiled && g.cc.w0 == w0 && g.cc.w1 == w1 && g.cc.two_cycle == two_cycle) {
        return;
    }
    compiled = true;
    cc_compile(&g.cc, w0, w1, two_cycle);
    cc_changed();
}

static void set_othermode_h(uint32_t oh) {
    uint32_t old = g.oh;
    g.oh = oh;
    if (((old ^ oh) >> 20) & 3) {
        set_combine(g.cc_w0, g.cc_w1);
        cc_changed();
    }
    g.mat_dirty = true;
}

static void warn_op(uint8_t op, uint32_t w0, uint32_t w1) {
    if (!warned_op[op]) {
        warned_op[op] = true;
        printf("[q64dc] gfx: unhandled command %08X %08X\n", (unsigned)w0, (unsigned)w1);
    }
}

static void run_dl(uint32_t start) {
    uint32_t stack[DL_STACK_SIZE];
    int sp = 0;
    uint32_t pc = start;

    for (uint32_t budget = 0; budget < 2000000; budget++) {
        if (!addr_ok(pc, 8)) {
            printf("[q64dc] gfx: display list jumped to %08X\n", (unsigned)pc);
            return;
        }
        uint32_t w0 = rd32(pc), w1 = rd32(pc + 4);
        pc += 8;
        uint8_t op = w0 >> 24;
        switch (op) {
            case 0x00:  // G_SPNOOP
                break;

            case 0x01: {  // G_MTX
                uint32_t p = (w0 >> 16) & 0xFF;
                uint32_t a = seg_addr(w1);
                if (!addr_ok(a, 64)) {
                    break;
                }
                matrix_t m;
                read_matrix(a, m);
                if (p & 0x01) {
                    if (p & 0x02) {
                        memcpy(g.proj, m, sizeof(m));
                    } else {
                        mat_mul(g.proj, m, g.proj);
                    }
                } else {
                    if ((p & 0x04) && g.mv_top < MV_STACK_SIZE - 1) {
                        memcpy(g.mv[g.mv_top + 1], g.mv[g.mv_top], sizeof(matrix_t));
                        g.mv_top++;
                    }
                    if (p & 0x02) {
                        memcpy(g.mv[g.mv_top], m, sizeof(m));
                    } else {
                        mat_mul(g.mv[g.mv_top], m, g.mv[g.mv_top]);
                    }
                    g.lights_dirty = true;
                }
                g.mvp_dirty = true;
                break;
            }

            case 0x03: {  // G_MOVEMEM
                uint32_t idx = (w0 >> 16) & 0xFF;
                uint32_t a = seg_addr(w1);
                if (!addr_ok(a, 16)) {
                    break;
                }
                if (idx == 0x80) {  // viewport
                    float sx = (float)(int16_t)rd16(a) * 0.25f, sy = (float)(int16_t)rd16(a + 2) * 0.25f;
                    float tx = (float)(int16_t)rd16(a + 8) * 0.25f, ty = (float)(int16_t)rd16(a + 10) * 0.25f;
                    g.vp_sx = sx * GFX_SCALE;
                    g.vp_sy = sy * GFX_SCALE;
                    g.vp_tx = tx * GFX_SCALE;
                    g.vp_ty = ty * GFX_SCALE;
                } else if (idx == 0x82 || idx == 0x84) {  // look-at Y, X
                    int i = idx == 0x84 ? 0 : 1;
                    for (int j = 0; j < 3; j++) {
                        g.lookat_dir[i][j] = (float)(int8_t)rd8(a + 8 + j) * (1.0f / 127.0f);
                    }
                    g.lights_dirty = true;
                } else if (idx >= 0x86 && idx <= 0x94) {  // lights
                    int i = (idx - 0x86) / 2;
                    for (int j = 0; j < 3; j++) {
                        g.light_col[i][j] = u8f(rd8(a + j));
                        g.light_dir[i][j] = (float)(int8_t)rd8(a + 8 + j) * (1.0f / 127.0f);
                    }
                    g.lights_dirty = true;
                }
                break;
            }

            case 0x04: {  // G_VTX
                uint32_t n = (w0 >> 10) & 0x3F;
                uint32_t v0 = ((w0 >> 16) & 0xFF) / 2;
                load_vertices(seg_addr(w1), v0, n);
                break;
            }

            case 0x06: {  // G_DL
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (sp >= DL_STACK_SIZE) {
                        printf("[q64dc] gfx: display list stack overflow\n");
                        return;
                    }
                    stack[sp++] = pc;
                }
                pc = seg_addr(w1);
                break;
            }

            case 0xAF:  // G_LOAD_UCODE
                break;

            case 0xB0:  // G_BRANCH_Z: always take the near (detailed) branch
                pc = seg_addr(g.rdphalf_1);
                break;

            case 0xB1:  // G_TRI2
                draw_triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                draw_triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;

            case 0xB2: {  // G_MODIFYVTX
                uint32_t where = (w0 >> 16) & 0xFF;
                uint32_t i = (w0 & 0xFFFF) / 2;
                if (i >= 32) {
                    break;
                }
                Vertex* v = &g.vtx[i];
                if (where == 0x10) {
                    unpack_rgba(w1, v->shade);
                } else if (where == 0x14) {
                    v->s = (float)(int16_t)(w1 >> 16) * (1.0f / 32.0f);
                    v->t = (float)(int16_t)w1 * (1.0f / 32.0f);
                } else if (where == 0x18 && v->w > 0.0f) {
                    float sx = (float)(int16_t)(w1 >> 16) * 0.25f * GFX_SCALE;
                    float sy = (float)(int16_t)w1 * 0.25f * GFX_SCALE;
                    v->x = (sx - g.vp_tx) / g.vp_sx * v->w;
                    v->y = -(sy - g.vp_ty) / g.vp_sy * v->w;
                    v->sx = sx;
                    v->sy = sy;
                }
                v->cc_epoch = 0;
                v->out_gen = 0;
                break;
            }

            case 0xB3:  // G_RDPHALF_2
                break;

            case 0xB4:  // G_RDPHALF_1
                g.rdphalf_1 = w1;
                break;

            case 0xB5:  // G_LINE3D
                break;

            case 0xB6:  // G_CLEARGEOMETRYMODE
                g.geom &= ~w1;
                g.mat_dirty = true;
                break;

            case 0xB7:  // G_SETGEOMETRYMODE
                g.geom |= w1;
                g.mat_dirty = true;
                break;

            case 0xB8:  // G_ENDDL
                if (sp == 0) {
                    return;
                }
                pc = stack[--sp];
                break;

            case 0xB9:    // G_SETOTHERMODE_L
            case 0xBA: {  // G_SETOTHERMODE_H
                uint32_t shift = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
                uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
                if (op == 0xB9) {
                    g.ol = (g.ol & ~mask) | (w1 & mask);
                    g.mat_dirty = true;
                } else {
                    set_othermode_h((g.oh & ~mask) | (w1 & mask));
                }
                break;
            }

            case 0xBB:  // G_TEXTURE
                g.tex_on = (w0 & 0xFF) != 0;
                g.tex_tile = (w0 >> 8) & 7;
                g.tex_scale_raw_s = w1 >> 16;
                g.tex_scale_raw_t = w1 & 0xFFFF;
                g.tex_ss = (float)g.tex_scale_raw_s * (1.0f / 65536.0f);
                g.tex_st = (float)g.tex_scale_raw_t * (1.0f / 65536.0f);
                g.mat_dirty = true;
                break;

            case 0xBC: {  // G_MOVEWORD
                uint32_t index = w0 & 0xFF, offset = (w0 >> 8) & 0xFFFF;
                if (index == 0x06) {
                    g.segments[(offset >> 2) & 0xF] = w1 & 0x1FFFFFFF;
                } else if (index == 0x02) {
                    int n = (int)((w1 - 0x80000000u) >> 5) - 1;
                    g.num_lights = n < 0 ? 0 : (n > MAX_LIGHTS ? MAX_LIGHTS : n);
                    g.lights_dirty = true;
                } else if (index == 0x08) {
                    g.fog_mul = (float)(int16_t)(w1 >> 16);
                    g.fog_off = (float)(int16_t)w1;
                } else if (index == 0x0A && (offset & 4) == 0) {
                    int i = offset / 0x20;
                    if (i <= MAX_LIGHTS) {
                        g.light_col[i][0] = u8f(w1 >> 24);
                        g.light_col[i][1] = u8f(w1 >> 16);
                        g.light_col[i][2] = u8f(w1 >> 8);
                    }
                }
                break;
            }

            case 0xBD:  // G_POPMTX
                if (g.mv_top > 0) {
                    g.mv_top--;
                }
                g.mvp_dirty = true;
                g.lights_dirty = true;
                break;

            case 0xBE: {  // G_CULLDL
                uint32_t first = (w0 & 0xFFFF) / 2, last = (w1 & 0xFFFF) / 2;
                if (last >= 32 || first > last) {
                    break;
                }
                uint8_t all = 0xFF;
                for (uint32_t i = first; i <= last; i++) {
                    all &= g.vtx[i].clip;
                }
                if (all & 0x0F) {
                    if (sp == 0) {
                        return;
                    }
                    pc = stack[--sp];
                }
                break;
            }

            case 0xBF:  // G_TRI1
                draw_triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;

            case 0xE4:    // G_TEXRECT
            case 0xE5: {  // G_TEXRECTFLIP
                if (!addr_ok(pc, 16)) {
                    return;
                }
                uint32_t h1 = rd32(pc + 4), h2 = rd32(pc + 12);
                pc += 16;
                float xh = (float)((w0 >> 12) & 0xFFF) * 0.25f, yh = (float)(w0 & 0xFFF) * 0.25f;
                float xl = (float)((w1 >> 12) & 0xFFF) * 0.25f, yl = (float)(w1 & 0xFFF) * 0.25f;
                draw_rect(xl, yl, xh, yh, (w1 >> 24) & 7, (float)(int16_t)(h1 >> 16) * (1.0f / 32.0f),
                          (float)(int16_t)h1 * (1.0f / 32.0f), (float)(int16_t)(h2 >> 16) * (1.0f / 1024.0f),
                          (float)(int16_t)h2 * (1.0f / 1024.0f), op == 0xE5, true);
                break;
            }

            case 0xE6:  // G_RDPLOADSYNC
            case 0xE7:  // G_RDPPIPESYNC
            case 0xE8:  // G_RDPTILESYNC
            case 0xE9:  // G_RDPFULLSYNC
            case 0xEA:  // G_SETKEYGB
            case 0xEB:  // G_SETKEYR
            case 0xEC:  // G_SETCONVERT
            case 0xEE:  // G_SETPRIMDEPTH
                break;

            case 0xED:  // G_SETSCISSOR
                g.sc_x0 = (float)((w0 >> 12) & 0xFFF) * 0.25f;
                g.sc_y0 = (float)(w0 & 0xFFF) * 0.25f;
                g.sc_x1 = (float)((w1 >> 12) & 0xFFF) * 0.25f;
                g.sc_y1 = (float)(w1 & 0xFFF) * 0.25f;
                break;

            case 0xEF:  // G_RDPSETOTHERMODE
                set_othermode_h(w0 & 0x00FFFFFF);
                g.ol = w1;
                g.mat_dirty = true;
                break;

            case 0xF0:  // G_LOADTLUT
                tmem_load_tlut((w1 >> 24) & 7, ((w1 >> 14) & 0x3FF) + 1);
                g.tmem_gen++;
                g.mat_dirty = true;
                break;

            case 0xF2: {  // G_SETTILESIZE
                GfxTile* t = &gfx_tiles[(w1 >> 24) & 7];
                t->uls = (w0 >> 12) & 0xFFF;
                t->ult = w0 & 0xFFF;
                t->lrs = (w1 >> 12) & 0xFFF;
                t->lrt = w1 & 0xFFF;
                g.mat_dirty = true;
                break;
            }

            case 0xF3:  // G_LOADBLOCK
                tmem_load_block((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
                g.tmem_gen++;
                g.mat_dirty = true;
                break;

            case 0xF4: {  // G_LOADTILE
                int tile = (w1 >> 24) & 7;
                uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF, lrs = (w1 >> 12) & 0xFFF, lrt = w1 & 0xFFF;
                tmem_load_tile(tile, uls, ult, lrs, lrt);
                g.tmem_gen++;
                gfx_tiles[tile].uls = uls;
                gfx_tiles[tile].ult = ult;
                gfx_tiles[tile].lrs = lrs;
                gfx_tiles[tile].lrt = lrt;
                g.mat_dirty = true;
                break;
            }

            case 0xF5: {  // G_SETTILE
                GfxTile* t = &gfx_tiles[(w1 >> 24) & 7];
                t->fmt = (w0 >> 21) & 7;
                t->siz = (w0 >> 19) & 3;
                t->line = (w0 >> 9) & 0x1FF;
                t->tmem = w0 & 0x1FF;
                t->palette = (w1 >> 20) & 0xF;
                t->cmt = (w1 >> 18) & 3;
                t->maskt = (w1 >> 14) & 0xF;
                t->shiftt = (w1 >> 10) & 0xF;
                t->cms = (w1 >> 8) & 3;
                t->masks = (w1 >> 4) & 0xF;
                t->shifts = w1 & 0xF;
                g.mat_dirty = true;
                break;
            }

            case 0xF6:  // G_FILLRECT
                draw_rect((float)((w1 >> 12) & 0xFFF) * 0.25f, (float)(w1 & 0xFFF) * 0.25f,
                          (float)((w0 >> 12) & 0xFFF) * 0.25f, (float)(w0 & 0xFFF) * 0.25f, 0, 0, 0, 0, 0, false,
                          false);
                break;

            case 0xF7:  // G_SETFILLCOLOR
                g.fill_color = w1;
                break;

            case 0xF8:  // G_SETFOGCOLOR
                if (g.fog_color != w1) {
                    g.fog_color = w1;
                    PVR_SET(PVR_FOG_VERTEX_COLOR, (w1 >> 8) & 0xFFFFFF);
                    PVR_SET(PVR_FOG_TABLE_COLOR, (w1 >> 8) & 0xFFFFFF);
                }
                break;

            case 0xF9:  // G_SETBLENDCOLOR
                break;

            case 0xFA:  // G_SETPRIMCOLOR
                unpack_rgba(w1, g.k.prim);
                g.k.prim_lod = u8f(w0);
                cc_changed();
                break;

            case 0xFB:  // G_SETENVCOLOR
                unpack_rgba(w1, g.k.env);
                cc_changed();
                break;

            case 0xFC:  // G_SETCOMBINE
                g.cc_w0 = w0 & 0x00FFFFFF;
                g.cc_w1 = w1;
                set_combine(g.cc_w0, g.cc_w1);
                break;

            case 0xFD:  // G_SETTIMG
                gfx_timg.fmt = (w0 >> 21) & 7;
                gfx_timg.siz = (w0 >> 19) & 3;
                gfx_timg.width = (w0 & 0xFFF) + 1;
                gfx_timg.addr = seg_addr(w1);
                break;

            case 0xFE:  // G_SETZIMG
                g.zimg = seg_addr(w1);
                break;

            case 0xFF:  // G_SETCIMG
                g.cimg = seg_addr(w1);
                g.cimg_siz = (w0 >> 19) & 3;
                break;

            default:
                warn_op(op, w0, w1);
                break;
        }
    }
    printf("[q64dc] gfx: display list didn't end\n");
}

// --- Frames ---

void gfx_init(void) {
    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_0 },
        .vertex_buf_size = VERTEX_BUF_SIZE,
        .dma_enabled = 0,
        .fsaa_enabled = 0,
        .autosort_disabled = 1,
    };
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_set_zclip(0.000001f);
    tex_init();

    memset(&g, 0, sizeof(g));
    for (int i = 0; i < 4; i++) {
        g.proj[i][i] = 1.0f;
        g.mv[0][i][i] = 1.0f;
    }
    g.mvp_dirty = true;
    g.lights_dirty = true;
    g.cc_epoch = 1;
    g.out_gen = 1;
    g.tmem_gen = 1;
    g.mat_dirty = true;
    g.vp_sx = 160.0f * GFX_SCALE;
    g.vp_sy = 120.0f * GFX_SCALE;
    g.vp_tx = 160.0f * GFX_SCALE;
    g.vp_ty = 120.0f * GFX_SCALE;
    g.sc_x1 = 320.0f;
    g.sc_y1 = 240.0f;
    g.cimg = 1;
    printf("[q64dc] gfx: PowerVR ready, %u KB of VRAM free\n", (unsigned)(pvr_mem_available() / 1024));
}

// A red bar along the top of the screen on test discs.
static void draw_test_banner(void) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&hdr, &cxt);
    uint32_t* h = (uint32_t*)pvr_dr_target(dr);
    memcpy(h, &hdr, sizeof(hdr));
    pvr_dr_commit(h);
    const float xs[4] = { 0.0f, 640.0f, 0.0f, 640.0f }, ys[4] = { 0.0f, 0.0f, 6.0f, 6.0f };
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t* d = pvr_dr_target(dr);
        d->flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        d->x = xs[i];
        d->y = ys[i];
        d->z = 1.0f;
        d->u = d->v = 0.0f;
        d->argb = 0xFFFF0000;
        d->oargb = 0;
        pvr_dr_commit(d);
    }
}

static uint32_t frames;
static uint32_t period_frames;
static uint64_t period_wait_us, period_build_us;

void gfx_run_task(uint32_t task) {
    uint32_t dl = (uint32_t)MEM_W(0x30, task);
    if (dl < 0x80000000u) {
        dl |= 0x80000000u;
    }

    uint64_t t0 = timer_us_gettime64();
    pvr_wait_ready();
    uint64_t t1 = timer_us_gettime64();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_dr_init(&dr);
    have_key = false;
    vbuf_used = 0;
    stat_frame_tris = 0;
    g.mv_top = 0;
    g.mvp_dirty = true;
    g.mat_dirty = true;
    // Look textures up again at least once a frame, which marks them as in use.
    g.tmem_gen++;
    tex_frame_begin();

    run_dl(dl);
    if (test_disc()) {
        draw_test_banner();
    }

    pvr_list_finish();
    pvr_scene_finish();
    stat_tris = stat_frame_tris;
    frames++;
    period_frames++;
    period_wait_us += t1 - t0;
    period_build_us += timer_us_gettime64() - t1;
}

void gfx_print_stats(void) {
    pvr_stats_t st;
    pvr_get_stats(&st);
    uint32_t n = period_frames ? period_frames : 1;
    printf("[q64dc] gfx: %u frames, %u tris last frame, %u headers/s, %u material updates/s, %u texture lookups/s, "
           "textures %u cached / %u uploaded, PVR %u fps, per frame: build %u us, wait %u us, render %u us; "
           "VRAM free %u KB\n",
           (unsigned)frames, (unsigned)stat_tris, (unsigned)stat_headers, (unsigned)stat_mat_updates,
           (unsigned)stat_tex_lookups, (unsigned)stat_tex_entries,
           (unsigned)stat_tex_uploads, (unsigned)st.frame_rate, (unsigned)(period_build_us / n),
           (unsigned)(period_wait_us / n), (unsigned)(st.rnd_last_time / 1000), (unsigned)(pvr_mem_available() / 1024));
    stat_headers = 0;
    stat_mat_updates = 0;
    stat_tex_lookups = 0;
    period_frames = 0;
    period_wait_us = 0;
    period_build_us = 0;
}
