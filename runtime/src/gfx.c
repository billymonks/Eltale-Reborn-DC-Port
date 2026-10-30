// F3DEX 1.23 and RDP command interpreter, drawing with the PowerVR.
//
// The RSP part (matrices, vertices, lighting, fog, clipping) runs on the SH-4, using its
// matrix unit for the vertex transform. Opaque surfaces go to the opaque list, alpha-tested
// cutouts to the punch-through list and blended surfaces to the translucent list, which has
// presorting disabled so it draws in submission order like the RDP; anything the RDP draws
// after a primitive that has to stay in order (the HUD, text boxes, fades) also goes to the
// translucent list. Depth tests and writes come from the RDP's render mode, and N64 fog uses
// the PowerVR's vertex fog where it can.

#include <kos.h>
#include <dc/matrix.h>
#include <dc/pvr.h>
#include <malloc.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "aica.h"
#include "audio_hle.h"
#include "diag.h"
#include "gfx.h"
#include "gfx_internal.h"
#include "profile.h"
#include "runtime.h"
#include "screen_log.h"
#include "test.h"
#include "ultra.h"

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
#define ZMODE_DEC      0x0C00
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

// Kept out of the functions that call them, so the display list loop and the triangle and vertex
// paths stay small enough to share the 8 KB instruction cache.
#define GFX_OUTLINE __attribute__((noinline))

// Stop adding to the frame before the PowerVR's vertex buffer overflows. KallistiOS takes two
// of them out of texture memory; the busiest frames fill about 140 KB.
#define VERTEX_BUF_SIZE (512 * 1024)
#define VERTEX_BUF_LIMIT (VERTEX_BUF_SIZE - 64 * 1024)

typedef struct {
    float x, y, z, w;  // clip space, projected at load
    float s, t;        // texels, before the tile's shift and origin
    float shade[4];
    float mul[4], add[4];
    uint32_t cc_epoch;
    uint8_t clip;
    uint8_t has_ofs;
    float sx, sy, sz;
    float u, v;
    uint32_t argb, oargb;
    uint32_t out_gen;
    float nx, ny, nz;  // lighting normal, used when unlit is set
    uint8_t unlit;     // shade and texture-gen UVs still to be computed
    uint8_t st_lock;   // MODIFYVTX wrote s,t, so lighting must leave them
} Vertex;
_Static_assert(sizeof(Vertex) == 128, "vertex stride");

// A vertex on its way to the PowerVR, after the combiner and before projection.
typedef struct {
    float x, y, z, w;
    float s, t;
    float col[4], ofs[4];
} ClipVert;

// The PowerVR's lists, in the order it draws them.
enum { LIST_OP, LIST_PT, LIST_TR, LIST_COUNT };
static const pvr_list_t pvr_lists[LIST_COUNT] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };

// Where a frame is, for keeping the RDP's drawing order across the lists: the background comes
// before anything with a depth test, the overlay after the first thing without one.
enum { PHASE_BACKGROUND, PHASE_SCENE, PHASE_OVERLAY };

enum { FOG_NONE, FOG_PVR, FOG_CPU };

typedef struct {
    pvr_ptr_t tex;
    uint32_t tex_fmt;
    uint16_t pw, ph;
    uint8_t filter, clamp, flip, txralpha_dis;
    uint8_t list, blend, fog, offset, depth_cmp, depth_write;
} HeaderKey;

// How the current material maps onto the PowerVR, worked out when the RDP state changes.
typedef struct {
    bool textured;
    int variant;
    GfxTexture tex;
    float shift_s, shift_t, uls, ult;
    float inv_pw, inv_ph;
    bool blend, cutout, depth_cmp, depth_write;
    int list, fog;
    float fog_rgb[3];
    bool decal;
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
    uint32_t light_seq;  // bumped when the light count or a light's or look-at's direction changes
    // The rotation and light_seq light_coeff and lookat_coeff were last worked out for.
    float lights_rot[3][3];
    uint32_t lights_rot_seq;
    bool lights_rot_ok;

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
    uint32_t prim_raw, env_raw, prim_lod_raw;  // g.k as set
    const GfxCCProg* cc_prog;
    uint32_t cc_prog_epoch;  // cc_epoch that cc_prog was found for
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
    // A texture rectangle stepping one texel per N64 pixel from a whole texel: the RDP's bilinear
    // filter returns single texels there, so the PowerVR samples the nearest texel at 640x480.
    bool rect_texel_exact, mat_rect_texel_exact;
    int phase, mat_phase;
    uint32_t pvr_fog_color;  // the PowerVR's vertex fog color this frame, if pvr_fog_set
    bool pvr_fog_set;
    HeaderKey mat_key;     // header for the material, without the offset flag
    bool mat_key_changed;
    uint32_t out_gen;      // bumped when cached vertex outputs go stale
} g;

static matrix_t mvp __attribute__((aligned(32)));

// OPT_DEFER's records of a frame's draws (see defer_flush).
enum { QD_TRI, QD_NEAR, QD_RECT };

typedef struct {
    uint16_t v[3];  // in q.vtx; for QD_RECT, v[0] is in q.rect
    uint8_t kind;
    uint8_t flat;
    uint16_t mat, cc;  // in q.mat and q.cc
    uint16_t view;     // QD_NEAR: in q.view
    uint16_t pad;
} QDraw;
_Static_assert(sizeof(QDraw) == 16, "two draws a cache line");

typedef struct {
    Material mat;
    HeaderKey key;
    uint32_t out_gen;
} QMat;

// A draw's out_gen is the newer of its QMat's and its QCC's: out_gen moves on with a material or,
// with OPT_COLORS, with a combiner epoch.
typedef struct {
    GfxCombiner cc;
    GfxCCConst k;
    uint32_t prim_raw, env_raw, prim_lod_raw, epoch, out_gen;
} QCC;

// What projecting a near-plane triangle's clipped corners reads.
typedef struct {
    float vp_sx, vp_sy, vp_tx, vp_ty;
    bool ortho;
} QView;

#define Q_DRAWS 4096
#define Q_VTX   2048
#define Q_MATS  512
#define Q_CCS   512
#define Q_RECTS 256
#define Q_VIEWS 128

static struct {
    QDraw* draw;
    Vertex* vtx;
    QMat* mat;
    QCC* cc;
    ClipVert (*rect)[4];
    QView* view;
    uint32_t ndraw, nvtx, nmat, ncc, nrect, nview;
    int16_t slot[32];   // q.vtx copy of each slot's vertex as it is now, -1 if none
    bool mat_new;       // the material changed since q.mat[nmat - 1]
    uint32_t cc_epoch;  // the combiner epoch of q.cc[ncc - 1]
    bool ok;            // the arrays are there
} q;
static bool q_on;  // this frame's draws are deferred

#ifdef GFX_DIAG
#define DEFER_POSSIBLE true
#else
#define DEFER_POSSIBLE GFX_OPT_ON(OPT_DEFER)
#endif

static void defer_draw(int kind, int i0, int i1, int i2, bool flat, const ClipVert* rect);

static void defer_reset(void) {
    q.ndraw = q.nvtx = q.nmat = q.ncc = q.nrect = q.nview = 0;
    for (int i = 0; i < 32; i++) {
        q.slot[i] = -1;
    }
    q.mat_new = true;
}

// Slots a load or a modification gives new vertices.
static inline void defer_forget(uint32_t first, uint32_t count) {
    if (DEFER_POSSIBLE) {
        for (uint32_t i = first; i < first + count && i < 32; i++) {
            q.slot[i] = -1;
        }
    }
}

// The opaque list goes straight to the tile accelerator through the store queues as the display
// list runs; the punch-through and translucent lists are kept in RAM and sent by DMA after it.
#define LIST_BUF_SIZE (256 * 1024)

static pvr_dr_state_t dr;
static HeaderKey cur_key[LIST_COUNT];
static bool have_key[LIST_COUNT];
static uint8_t* list_buf[LIST_COUNT];
static uint32_t list_used[LIST_COUNT];
static int out_list;  // the list the next vertices go to
static uint32_t vbuf_used;
static uint32_t stat_list_bytes[LIST_COUNT];

#ifdef GFX_DIAG
#ifndef PERF_HUD
#error "GFX_DIAG builds on PERF_HUD"
#endif
#define DIAG_IS(mode) (gfx_diag == (mode))
int gfx_diag;
static volatile int diag_request;
// DIAG_NO_TA: the opaque list's vertices and headers, instead of the store queues.
static uint8_t diag_scratch[64] __attribute__((aligned(32)));
static uint32_t diag_scratch_at;
// DIAG_NO_DMA: where the HUD starts in the translucent list, the only part that's sent.
static uint32_t diag_hud_from;
// DIAG_NO_TEXTURE: bound for every textured material.
static GfxTexture diag_dummy;

// Since boot, for the HUD's per-frame averages.
static struct {
    uint32_t frames;
    uint32_t wait_us, submit_us;
    uint32_t oc_stall, ic_stall;  // the RSP thread's stall cycles
    uint32_t cmds, tri_cmds, drawn, loads, ta_bytes, dma_bytes;
} dg;

// TMU2 and the RSP thread's KallistiOS CPU time record at one moment. The record changes when the
// thread is switched back in, so between two marks the thread was away for the wall time between
// the two switch-ins less the CPU time it was credited with.
typedef struct {
    uint32_t tcnt;
    uint64_t sched, total;
} CpuMark;

static inline void cpu_mark(CpuMark* m) {
    volatile const kthread_t* self = thd_current;
    uint64_t s;
    do {
        s = self->cpu_time.scheduled;
        m->total = self->cpu_time.total;
        m->tcnt = DIAG_TMU2_TCNT;
    } while (s != self->cpu_time.scheduled);
    m->sched = s;
}

// TMU2 ticks from a to b that the thread had the CPU for.
static uint32_t cpu_ticks(const CpuMark* a, const CpuMark* b) {
    uint32_t d = diag_ticks_between(a->tcnt, b->tcnt);
    if (b->sched != a->sched) {
        int64_t away_ns = (int64_t)(b->sched - a->sched) - (int64_t)(b->total - a->total);
        // KallistiOS's nanoseconds are 80 per TMU2 tick.
        uint32_t away = away_ns > 0 ? (uint32_t)(away_ns / 80) : 0;
        d = away < d ? d - away : 0;
    }
    return d;
}

// The display list's time by kind of command. The clock is read where the kind changes, not per
// command. DR_DEFER is OPT_DEFER's second pass over the frame's recorded draws.
enum { DR_OTHER, DR_VTX, DR_TRI, DR_TEX, DR_DEFER, DR_COUNT };
static struct {
    int cur;
    CpuMark mark;
    uint32_t ticks[DR_COUNT];
} dr_time;

static const uint8_t dl_region[256] = {
    [0x04] = DR_VTX, [0xB1] = DR_TRI, [0xBF] = DR_TRI, [0xF0] = DR_TEX, [0xF3] = DR_TEX, [0xF4] = DR_TEX,
};

static void dr_switch(int region) {
    CpuMark now;
    cpu_mark(&now);
    dr_time.ticks[dr_time.cur] += cpu_ticks(&dr_time.mark, &now);
    dr_time.mark = now;
    dr_time.cur = region;
}

static inline void dr_enter(int region) {
    if (region != dr_time.cur) {
        dr_switch(region);
    }
}

static inline void dr_start(void) {
    cpu_mark(&dr_time.mark);
    dr_time.cur = DR_OTHER;
}

// The RSP thread's operand and instruction cache miss stalls, over gfx_run_task and gfx_submit.
static struct {
    uint32_t pmc0, pmc1, game0, game1;
} pmc_mark;

static void pmc_begin(void) {
    pmc_mark.pmc0 = DIAG_PMC0;
    pmc_mark.pmc1 = DIAG_PMC1;
    pmc_mark.game0 = diag_game_pmc0;
    pmc_mark.game1 = diag_game_pmc1;
}

static void pmc_end(void) {
    int32_t oc = (int32_t)((DIAG_PMC0 - pmc_mark.pmc0) - (diag_game_pmc0 - pmc_mark.game0));
    int32_t ic = (int32_t)((DIAG_PMC1 - pmc_mark.pmc1) - (diag_game_pmc1 - pmc_mark.game1));
    dg.oc_stall += oc > 0 ? (uint32_t)oc : 0;
    dg.ic_stall += ic > 0 ? (uint32_t)ic : 0;
}

// The sets both triggers + B steps through: the default, none, the release's before M, N, E, O, G
// and F joined it, then the default without each of Q, M, U, N, E, O, G and F. Builds start in set 3,
// the default without Q: the fastest on hardware, and the release's.
static const uint32_t opt_sets[] = {
    OPT_DEFAULT,
    0,
    OPT_TEX | OPT_SUM | OPT_LOAD | OPT_VTX | OPT_COLOR | OPT_TRI | OPT_REJECT | OPT_KEEP | OPT_RECT | OPT_ROWS |
        OPT_LIGHTS,
    OPT_DEFAULT & ~OPT_DEFER,
    OPT_DEFAULT & ~OPT_ALLOC,
    OPT_DEFAULT & ~OPT_LIGHTS,
    OPT_DEFAULT & ~OPT_NOOP,
    OPT_DEFAULT & ~OPT_SAME,
    OPT_DEFAULT & ~OPT_COLORS,
    OPT_DEFAULT & ~OPT_PROGS,
    OPT_DEFAULT & ~OPT_PACK,
};
#define OPT_SETS ((int)(sizeof(opt_sets) / sizeof(opt_sets[0])))
#define OPT_START 3
_Static_assert((OPT_DEFAULT & ~OPT_DEFER) == OPT_RELEASE, "set 3 is the release's");
uint32_t gfx_opt = OPT_DEFAULT & ~OPT_DEFER;  // opt_sets[OPT_START]
static int opt_set = OPT_START;
static volatile int opt_request = OPT_START;

void gfx_diag_cycle(void) {
    opt_request = (opt_request + 1) % OPT_SETS;
}

void gfx_diag_mode_cycle(void) {
    diag_request = (diag_request + 1) % DIAG_MODES;
}

void gfx_diag_select(int set, int mode) {
    if (set >= 0 && set < OPT_SETS) {
        opt_request = set;
    }
    if (mode >= 0 && mode < DIAG_MODES) {
        diag_request = mode;
    }
}

static void diag_pvr_init(void);
#else
#define DIAG_IS(mode) false
#endif

#ifdef GFX_XCHECK
// What a triangle sends to the PowerVR while it's being cross-checked: each 32-byte block and
// the list it went to.
static struct {
    bool on;
    uint32_t n;
    uint8_t list[16];
    uint32_t data[16][8];
} cap;
#endif

static inline void* out_target(void) {
#ifdef GFX_XCHECK
    if (cap.on) {
        return cap.data[cap.n];
    }
#endif
    if (out_list == LIST_OP) {
#ifdef GFX_DIAG
        if (gfx_diag == DIAG_NO_TA) {
            diag_scratch_at ^= 32;
            return diag_scratch + diag_scratch_at;
        }
#endif
        return pvr_dr_target(dr);
    }
    uint8_t* p = list_buf[out_list] + list_used[out_list];
    if (GFX_OPT_ON(OPT_ALLOC)) {
        // Every word of the block is written next, so its line needn't be read from RAM first.
        dcache_alloc_block(p, 0);
    }
    return p;
}

#ifdef GFX_HASH
// Test builds: a hash of what each frame's display list sends to the PowerVR, printed with the
// frame so runs of two configurations can be compared frame by frame. It covers what the PowerVR
// reads: a header's first four words (all pvr_poly_compile sets), and a vertex's words but its
// u and v under an untextured header, which hold whatever the vertex had there before.
static uint32_t frame_hash;
static bool hash_on;
static bool hash_textured[LIST_COUNT];
static uint32_t frames;

static inline void hash_block(const uint32_t* d) {
    bool header = (d[0] >> 29) == 4;
    int n = header ? 4 : 8;
    if (header) {
        hash_textured[out_list] = (d[0] & PVR_TA_CMD_TXRENABLE) != 0;
    }
    for (int i = 0; i < n; i++) {
        if (!header && !hash_textured[out_list] && (i == 4 || i == 5)) {
            continue;
        }
        frame_hash = (frame_hash ^ d[i]) * 16777619u;
    }
    frame_hash = (frame_hash ^ (uint32_t)out_list) * 16777619u;
#ifdef GFX_HASH_DUMP
    // One frame's blocks in full, for finding where two runs part.
    if (frames == GFX_HASH_DUMP) {
        printf("[q64dc] fblk %d %08X %08X %08X %08X %08X %08X %08X %08X\n", out_list, (unsigned)d[0], (unsigned)d[1],
               (unsigned)d[2], (unsigned)d[3], n == 8 ? (unsigned)d[4] : 0, n == 8 ? (unsigned)d[5] : 0,
               n == 8 ? (unsigned)d[6] : 0, n == 8 ? (unsigned)d[7] : 0);
    }
#endif
}
#endif

static inline void out_commit(void* p) {
#ifdef GFX_XCHECK
    if (cap.on) {
        cap.list[cap.n++] = (uint8_t)out_list;
        return;
    }
#endif
#ifdef GFX_HASH
    if (hash_on) {
        hash_block((const uint32_t*)p);
    }
#endif
    if (out_list == LIST_OP) {
#ifdef GFX_DIAG
        if (gfx_diag == DIAG_NO_TA) {
            return;
        }
#endif
        pvr_dr_commit(p);
    } else {
        list_used[out_list] += 32;
    }
}

// Whether a header and n vertices fit in the vertex buffer and the list's RAM buffer.
static inline bool list_room(int list, uint32_t n) {
    uint32_t bytes = n * 32 + 32;
    return vbuf_used + bytes <= VERTEX_BUF_LIMIT && (list == LIST_OP || list_used[list] + bytes <= LIST_BUF_SIZE);
}

static uint32_t stat_tris, stat_headers, stat_frame_tris, stat_tex_lookups, stat_mat_updates, stat_culled_dls;
static uint32_t stat_list_tris[LIST_COUNT];

#ifdef GFX_COUNT
GfxCount gfx_count;

// What became of each vertex slot's vertex: 1 loaded, 2 used by a rejected triangle, 3 by a drawn
// one; and whether its G_VTX batch was entirely outside one plane (2 once a drawn triangle used it).
static uint8_t slot_fate[32], slot_out[32];

static void count_retire(uint32_t i) {
    if (slot_fate[i] == 1) {
        GCOUNT(vtx_unused, 1);
    } else if (slot_fate[i] == 2) {
        GCOUNT(vtx_rejected, 1);
    } else if (slot_fate[i] == 3) {
        GCOUNT(vtx_drawn, 1);
    }
    slot_fate[i] = 0;
}

static void count_batch(const Vertex* vtx, uint32_t first, uint32_t count) {
    if (first + count > 32) {
        return;
    }
    GCOUNT(vtx_cmds, 1);
    GCOUNT(vtx_verts, count);
    uint8_t all = 0xFF;
    for (uint32_t i = first; i < first + count; i++) {
        count_retire(i);
        slot_fate[i] = 1;
        all &= vtx[i].clip;
        GCOUNT(vtx_outside, vtx[i].clip != 0);
    }
    bool out = count != 0 && (all & (0x0F | CLIP_NEAR)) != 0;
    for (uint32_t i = first; i < first + count; i++) {
        slot_out[i] = out;
    }
    GCOUNT(batch_out, out);
    GCOUNT(batch_out_verts, out ? count : 0);
}

static void count_tri(int i0, int i1, int i2, int fate) {
    const int s[3] = { i0, i1, i2 };
    for (int k = 0; k < 3; k++) {
        if (slot_fate[s[k]] < fate) {
            slot_fate[s[k]] = (uint8_t)fate;
        }
        if (fate == 3 && slot_out[s[k]] == 1) {
            slot_out[s[k]] = 2;
            GCOUNT(batch_out_used, 1);
        }
    }
}

// Display list calls, for the ones that draw nothing.
static struct {
    uint32_t verts, tris, drawn;
} dl_mark[DL_STACK_SIZE];

// The last 30 frames' counts, taken by the RSP thread for the log thread to print.
static GfxCount count_report;
static volatile uint32_t count_report_seq;
#endif
static uint32_t stat_vbuf_peak, vbuf_peak_boot;
static bool warned_op[256];

static void touch_project(void) {
    g.mvp_dirty = true;
}

static void touch_mvp(void) {
    g.mvp_dirty = true;
}

static inline float u8f(uint32_t v) {
    return (float)(v & 0xFF) * (1.0f / 255.0f);
}

static inline uint32_t seg_addr(uint32_t a) {
    uint32_t phys = (g.segments[(a >> 24) & 0xF] + (a & 0x00FFFFFF)) & 0x1FFFFFFF;
    return 0x80000000u | phys;
}

static inline bool addr_ok(uint32_t vaddr, uint32_t len) {
    uint32_t off = vaddr - 0x80000000u;
    return off < RDRAM_MAPPED_SIZE && len <= RDRAM_MAPPED_SIZE - off;
}

static inline uint32_t rd32(uint32_t vaddr) {
    uint8_t* p = RDRAM_PTR(vaddr);
    // N64 display data is usually word aligned. A 2-aligned address is a legal
    // halfword boundary; a 32-bit load there is an address error on the SH-4.
    if (((uintptr_t)p & 3) == 0) {
        return *(uint32_t*)p;
    }
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

// Integer halves of 16 elements, then fraction halves; a word holds two elements' halves.
static void GFX_OUTLINE read_matrix(uint32_t addr, matrix_t out) {
    float* o = &out[0][0];
    for (int i = 0; i < 8; i++) {
        uint32_t hi = rd32(addr + i * 4), lo = rd32(addr + 0x20 + i * 4);
        o[i * 2] = (float)(int32_t)((hi & 0xFFFF0000u) | (lo >> 16)) * (1.0f / 65536.0f);
        o[i * 2 + 1] = (float)(int32_t)((hi << 16) | (lo & 0xFFFFu)) * (1.0f / 65536.0f);
    }
}

// Row-vector convention, as on the N64: res = a * b.
static void GFX_OUTLINE mat_mul(matrix_t res, matrix_t a, matrix_t b) {
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

static void GFX_OUTLINE update_mvp(void) {
    mat_mul(mvp, g.mv[g.mv_top], g.proj);
    g.ortho = g.proj[0][3] == 0.0f && g.proj[1][3] == 0.0f && g.proj[2][3] == 0.0f && g.proj[3][3] != 0.0f;
    g.mvp_dirty = false;
}

static void GFX_OUTLINE update_lights(void) {
    // The directions depend only on the modelview's rotation and on the lights (light_seq).
    const matrix_t* m = &g.mv[g.mv_top];
    float rot[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            rot[i][j] = (*m)[i][j];
        }
    }
    bool same = g.lights_rot_ok && g.lights_rot_seq == g.light_seq && memcmp(rot, g.lights_rot, sizeof(rot)) == 0;
#ifdef GFX_XCHECK
    float kept_light[MAX_LIGHTS][3], kept_lookat[2][3];
    memcpy(kept_light, g.light_coeff, sizeof(kept_light));
    memcpy(kept_lookat, g.lookat_coeff, sizeof(kept_lookat));
    bool check = GFX_OPT_ON(OPT_LIGHTS) && same;
    same = false;
#endif
    if (GFX_OPT_ON(OPT_LIGHTS) && same) {
        g.lights_dirty = false;
        return;
    }
    for (int i = 0; i < g.num_lights; i++) {
        model_space_dir(g.light_dir[i], g.light_coeff[i]);
        // Normals are s8 with 127 for 1.
        for (int j = 0; j < 3; j++) {
            g.light_coeff[i][j] *= 1.0f / 127.0f;
        }
    }
    model_space_dir(g.lookat_dir[0], g.lookat_coeff[0]);
    model_space_dir(g.lookat_dir[1], g.lookat_coeff[1]);
    g.lights_dirty = false;
    memcpy(g.lights_rot, rot, sizeof(rot));
    g.lights_rot_seq = g.light_seq;
    g.lights_rot_ok = true;
#ifdef GFX_XCHECK
    if (check) {
        gfx_xcheck_runs[XC_LIGHTS]++;
        if ((memcmp(kept_light, g.light_coeff, (size_t)g.num_lights * sizeof(kept_light[0])) != 0 ||
             memcmp(kept_lookat, g.lookat_coeff, sizeof(kept_lookat)) != 0) &&
            gfx_xcheck_fails[XC_LIGHTS]++ < 8) {
            printf("[q64dc] xcheck lights: kept %f %f %f, recomputed %f %f %f\n", (double)kept_light[0][0],
                   (double)kept_light[0][1], (double)kept_light[0][2], (double)g.light_coeff[0][0],
                   (double)g.light_coeff[0][1], (double)g.light_coeff[0][2]);
        }
    }
#endif
}

// --- Vertices ---

// Positive reciprocal. A divide, not fsrra: the SH-4's approximation would move vertices.
static inline float recip_pos(float w) {
    if (!(w > 0.0f)) {
        return 32767.0f;
    }
    return 1.0f / w;
}

// OPT_BATCH: when every vertex of a load is outside the same plane, the load stops after the
// transform and the clip codes, since triangles of those vertices alone are rejected on them. The
// rest of each vertex waits here, with the state it depended on, until a triangle with other
// vertices needs it (vtx_complete).
typedef struct {
    float tex_ss, tex_st, fog_mul, fog_off, vp_sx, vp_sy, vp_tx, vp_ty;
    bool lighting, fog, ortho;
} VtxState;

typedef struct {
    uint32_t w2, w3;  // texture coordinates; color or normal and alpha
    VtxState st;
} VtxLater;

static VtxLater vtx_later[32];
static uint32_t vtx_pending;  // a bit per slot still waiting

// OPT_REJECT: each slot's clip code, packed, for rejecting triangles without their vertices.
static uint8_t vclip[32];

#ifdef GFX_DIAG
#define BATCH_POSSIBLE  true
#define REJECT_POSSIBLE true
#else
#define BATCH_POSSIBLE  GFX_OPT_ON(OPT_BATCH)
#define REJECT_POSSIBLE GFX_OPT_ON(OPT_REJECT)
#endif

static inline uint32_t slot_bits(uint32_t first, uint32_t count) {
    return (count >= 32 ? 0xFFFFFFFFu : ((1u << count) - 1)) << first;
}

static inline void project(Vertex* v, float winv) {
    v->sx = v->x * winv * g.vp_sx + g.vp_tx;
    v->sy = -v->y * winv * g.vp_sy + g.vp_ty;
    v->sz = g.ortho ? 1.0f - v->z * winv * 0.01f : winv;
}

static void GFX_OUTLINE load_vertices(uint32_t addr, uint32_t first, uint32_t count) {
    if (first + count > 32 || !addr_ok(addr, count * 16)) {
        return;
    }
    if (g.mvp_dirty) {
        update_mvp();
    }
    if (BATCH_POSSIBLE) {
        vtx_pending &= ~slot_bits(first, count);
    }
    defer_forget(first, count);
    bool lighting = (g.geom & G_LIGHTING) != 0;
    mat_load(&mvp);

    for (uint32_t i = 0; i < count; i++) {
        Vertex* v = &g.vtx[first + i];
        uint32_t a = addr + i * 16;
        if (i + 1 < count) {
            __builtin_prefetch(RDRAM_PTR(a + 16));
        }
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
        v->clip = (uint8_t)((x < -w) | ((x > w) << 1) | ((y < -w) << 2) | ((y > w) << 3) |
                            (((z < -w) | (w <= 0.0f)) * CLIP_NEAR));
        if (REJECT_POSSIBLE) {
            vclip[first + i] = v->clip;
        }

        float s = (float)(int16_t)(w2 >> 16) * g.tex_ss * (1.0f / 32.0f);
        float t = (float)(int16_t)w2 * g.tex_st * (1.0f / 32.0f);
        v->st_lock = 0;
        if (lighting) {
            v->nx = (float)(int8_t)(w3 >> 24);
            v->ny = (float)(int8_t)(w3 >> 16);
            v->nz = (float)(int8_t)(w3 >> 8);
            v->unlit = 1;
        } else {
            v->shade[0] = u8f(w3 >> 24);
            v->shade[1] = u8f(w3 >> 16);
            v->shade[2] = u8f(w3 >> 8);
            v->unlit = 0;
        }
        v->shade[3] = u8f(w3);
        v->s = s;
        v->t = t;

        float winv = recip_pos(w);
        if ((g.geom & G_FOG) && !DIAG_IS(DIAG_NO_LIGHT)) {
            float f = z * winv * g.fog_mul + g.fog_off;
            f = f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f);
            v->shade[3] = f * (1.0f / 255.0f);
        }
        project(v, winv);
        v->cc_epoch = 0;
        v->out_gen = 0;
    }
}

// load_vertices as its own small loop: aligned word loads with the RDRAM a few vertices ahead
// and the Vertex lines about to be filled already asked for. Same arithmetic, expression for
// expression.
static void __attribute__((noinline)) load_vertices_fast(uint32_t addr, uint32_t first, uint32_t count) {
    if (first + count > 32 || !addr_ok(addr, count * 16)) {
        return;
    }
    const uint32_t* src = (const uint32_t*)RDRAM_PTR(addr);
    if (((uintptr_t)src & 3) != 0) {
        load_vertices(addr, first, count);
        return;
    }
    if (g.mvp_dirty) {
        update_mvp();
    }
    if (BATCH_POSSIBLE) {
        vtx_pending &= ~slot_bits(first, count);
    }
    defer_forget(first, count);
    bool lighting = (g.geom & G_LIGHTING) != 0;
    bool fog = (g.geom & G_FOG) && !DIAG_IS(DIAG_NO_LIGHT);
    mat_load(&mvp);
#ifdef GFX_XCHECK
    static Vertex before[32], after[32];
    memcpy(before, &g.vtx[first], count * sizeof(Vertex));
#endif
    __builtin_prefetch(src + 8);
    __builtin_prefetch(src + 16);
    Vertex* v = &g.vtx[first];
    for (uint32_t i = 0; i < count; i++, v++, src += 4) {
        __builtin_prefetch(src + 24);
        const char* next = (const char*)(v + 1);
        __builtin_prefetch(next);
        __builtin_prefetch(next + 32);
        __builtin_prefetch(next + 64);
        __builtin_prefetch(next + 96);
        uint32_t w0 = src[0], w1 = src[1], w2 = src[2], w3 = src[3];

        float x = (float)(int16_t)(w0 >> 16);
        float y = (float)(int16_t)w0;
        float z = (float)(int16_t)(w1 >> 16);
        float w = 1.0f;
        mat_trans_nodiv(x, y, z, w);
        v->x = x;
        v->y = y;
        v->z = z;
        v->w = w;
        v->clip = (uint8_t)((x < -w) | ((x > w) << 1) | ((y < -w) << 2) | ((y > w) << 3) |
                            (((z < -w) | (w <= 0.0f)) * CLIP_NEAR));
        if (REJECT_POSSIBLE) {
            vclip[first + i] = v->clip;
        }

        float s = (float)(int16_t)(w2 >> 16) * g.tex_ss * (1.0f / 32.0f);
        float t = (float)(int16_t)w2 * g.tex_st * (1.0f / 32.0f);
        v->st_lock = 0;
        if (lighting) {
            v->nx = (float)(int8_t)(w3 >> 24);
            v->ny = (float)(int8_t)(w3 >> 16);
            v->nz = (float)(int8_t)(w3 >> 8);
            v->unlit = 1;
        } else {
            v->shade[0] = u8f(w3 >> 24);
            v->shade[1] = u8f(w3 >> 16);
            v->shade[2] = u8f(w3 >> 8);
            v->unlit = 0;
        }
        v->shade[3] = u8f(w3);
        v->s = s;
        v->t = t;

        float winv = recip_pos(w);
        if (fog) {
            float f = z * winv * g.fog_mul + g.fog_off;
            f = f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f);
            v->shade[3] = f * (1.0f / 255.0f);
        }
        project(v, winv);
        v->cc_epoch = 0;
        v->out_gen = 0;
    }
#ifdef GFX_XCHECK
    memcpy(after, &g.vtx[first], count * sizeof(Vertex));
    memcpy(&g.vtx[first], before, count * sizeof(Vertex));
    load_vertices(addr, first, count);
    gfx_xcheck_runs[XC_VTX] += count;
    for (uint32_t i = 0; i < count; i++) {
        const Vertex* a = &after[i];
        const Vertex* b = &g.vtx[first + i];
        if (memcmp(a, b, sizeof(Vertex)) != 0 && gfx_xcheck_fails[XC_VTX]++ < 8) {
            printf("[q64dc] xcheck vtx %u: fast %f %f %f %f c%u, original %f %f %f %f c%u\n", (unsigned)(first + i),
                   (double)a->sx, (double)a->sy, (double)a->sz, (double)a->shade[3], a->clip, (double)b->sx,
                   (double)b->sy, (double)b->sz, (double)b->shade[3], b->clip);
        }
    }
#endif
}

// What load_vertices does after the clip code, from the state it had at the load: the same
// expressions, with st's copies for g's fields.
static inline void vtx_finish(Vertex* v, uint32_t w2, uint32_t w3, const VtxState* st) {
    float s = (float)(int16_t)(w2 >> 16) * st->tex_ss * (1.0f / 32.0f);
    float t = (float)(int16_t)w2 * st->tex_st * (1.0f / 32.0f);
    v->st_lock = 0;
    if (st->lighting) {
        v->nx = (float)(int8_t)(w3 >> 24);
        v->ny = (float)(int8_t)(w3 >> 16);
        v->nz = (float)(int8_t)(w3 >> 8);
        v->unlit = 1;
    } else {
        v->shade[0] = u8f(w3 >> 24);
        v->shade[1] = u8f(w3 >> 16);
        v->shade[2] = u8f(w3 >> 8);
        v->unlit = 0;
    }
    v->shade[3] = u8f(w3);
    v->s = s;
    v->t = t;

    float z = v->z;
    float winv = recip_pos(v->w);
    if (st->fog) {
        float f = z * winv * st->fog_mul + st->fog_off;
        f = f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f);
        v->shade[3] = f * (1.0f / 255.0f);
    }
    v->sx = v->x * winv * st->vp_sx + st->vp_tx;
    v->sy = -v->y * winv * st->vp_sy + st->vp_ty;
    v->sz = st->ortho ? 1.0f - v->z * winv * 0.01f : winv;
    v->cc_epoch = 0;
    v->out_gen = 0;
}

#ifdef GFX_XCHECK
// The original load's vertex for each slot still waiting, for comparing once it's completed.
static Vertex vtx_reference[32];
#endif

// The rest of the waiting vertices among mask.
static void __attribute__((noinline)) vtx_complete(uint32_t mask) {
    mask &= vtx_pending;
    vtx_pending &= ~mask;
    while (mask != 0) {
        uint32_t i = (uint32_t)__builtin_ctz(mask);
        mask &= mask - 1;
        const VtxLater* l = &vtx_later[i];
        vtx_finish(&g.vtx[i], l->w2, l->w3, &l->st);
#ifdef GFX_XCHECK
        gfx_xcheck_runs[XC_BATCH]++;
        if (memcmp(&g.vtx[i], &vtx_reference[i], sizeof(Vertex)) != 0 && gfx_xcheck_fails[XC_BATCH]++ < 8) {
            printf("[q64dc] xcheck batch: slot %u completed %f %f %f, original %f %f %f\n", (unsigned)i,
                   (double)g.vtx[i].sx, (double)g.vtx[i].sy, (double)g.vtx[i].shade[3], (double)vtx_reference[i].sx,
                   (double)vtx_reference[i].sy, (double)vtx_reference[i].shade[3]);
        }
#endif
    }
}

// load_vertices_fast in two passes: the transform and clip codes, then the rest only if some
// vertex isn't outside a plane all the others are outside too.
static void __attribute__((noinline)) load_vertices_batch(uint32_t addr, uint32_t first, uint32_t count) {
    if (first + count > 32 || !addr_ok(addr, count * 16)) {
        return;
    }
    const uint32_t* src = (const uint32_t*)RDRAM_PTR(addr);
    if (((uintptr_t)src & 3) != 0) {
        load_vertices(addr, first, count);
        return;
    }
    if (g.mvp_dirty) {
        update_mvp();
    }
    defer_forget(first, count);
    const VtxState st = { g.tex_ss,   g.tex_st,
                          g.fog_mul,  g.fog_off,
                          g.vp_sx,    g.vp_sy,
                          g.vp_tx,    g.vp_ty,
                          (g.geom & G_LIGHTING) != 0, (g.geom & G_FOG) && !DIAG_IS(DIAG_NO_LIGHT),
                          g.ortho };
    mat_load(&mvp);
#ifdef GFX_XCHECK
    static Vertex before[32];
    uint32_t pending_before = vtx_pending;
    memcpy(before, &g.vtx[first], count * sizeof(Vertex));
#endif
    __builtin_prefetch(src + 8);
    __builtin_prefetch(src + 16);
    Vertex* v = &g.vtx[first];
    uint8_t all = 0xFF;
    for (uint32_t i = 0; i < count; i++, v++) {
        const uint32_t* s = src + i * 4;
        __builtin_prefetch(s + 24);
        const char* next = (const char*)(v + 1);
        __builtin_prefetch(next);
        __builtin_prefetch(next + 64);
        uint32_t w0 = s[0], w1 = s[1];
        float x = (float)(int16_t)(w0 >> 16);
        float y = (float)(int16_t)w0;
        float z = (float)(int16_t)(w1 >> 16);
        float w = 1.0f;
        mat_trans_nodiv(x, y, z, w);
        v->x = x;
        v->y = y;
        v->z = z;
        v->w = w;
        uint8_t clip = (uint8_t)((x < -w) | ((x > w) << 1) | ((y < -w) << 2) | ((y > w) << 3) |
                                 (((z < -w) | (w <= 0.0f)) * CLIP_NEAR));
        v->clip = clip;
        vclip[first + i] = clip;
        all &= clip;
    }
    uint32_t bits = slot_bits(first, count);
#ifdef GFX_XCHECK_DEFER
    // Cross-checks of vtx_complete: every batch waits.
    all = 0xFF;
#endif
    if (all != 0) {
        for (uint32_t i = 0; i < count; i++) {
            VtxLater* l = &vtx_later[first + i];
            l->w2 = src[i * 4 + 2];
            l->w3 = src[i * 4 + 3];
            l->st = st;
        }
        vtx_pending |= bits;
    } else {
        v = &g.vtx[first];
        for (uint32_t i = 0; i < count; i++, v++) {
            const char* next = (const char*)(v + 1);
            __builtin_prefetch(next + 32);
            __builtin_prefetch(next + 96);
            vtx_finish(v, src[i * 4 + 2], src[i * 4 + 3], &st);
        }
        vtx_pending &= ~bits;
    }
#ifdef GFX_XCHECK
    // The original load from the same state; a waiting slot is compared on what the first pass
    // wrote now, and in full once it's completed.
    static Vertex after[32];
    uint32_t pending_after = vtx_pending;
    memcpy(after, &g.vtx[first], count * sizeof(Vertex));
    memcpy(&g.vtx[first], before, count * sizeof(Vertex));
    vtx_pending = pending_before;
    load_vertices(addr, first, count);
    gfx_xcheck_runs[XC_VTX] += count;
    for (uint32_t i = 0; i < count; i++) {
        const Vertex* a = &after[i];
        const Vertex* b = &g.vtx[first + i];
        bool waiting = (pending_after >> (first + i)) & 1;
        bool same = waiting ? a->x == b->x && a->y == b->y && a->z == b->z && a->w == b->w && a->clip == b->clip
                            : memcmp(a, b, sizeof(Vertex)) == 0;
        if (!same && gfx_xcheck_fails[XC_VTX]++ < 8) {
            printf("[q64dc] xcheck vtx %u (batch, %s): %f %f c%u, original %f %f c%u\n", (unsigned)(first + i),
                   waiting ? "waiting" : "done", (double)a->sx, (double)a->sy, a->clip, (double)b->sx, (double)b->sy,
                   b->clip);
        }
        if (waiting) {
            vtx_reference[first + i] = *b;
        }
    }
    memcpy(&g.vtx[first], after, count * sizeof(Vertex));
    vtx_pending = pending_after;
#endif
}

// --- Materials and PowerVR headers ---

// blend: mixed with the framebuffer by alpha. cutout: pixels kept or dropped by alpha, by the alpha
// compare or by texture edge coverage (alpha times coverage), without blending.
static void blend_mode(int cycle, bool* blend, bool* cutout, bool* fog) {
    uint32_t l = g.ol;
    *fog = false;
    *blend = false;
    *cutout = false;
    if (cycle == CYCLE_FILL) {
        return;
    }
    if (cycle == CYCLE_COPY) {
        *cutout = (l & 3) != 0;
        return;
    }
    uint32_t p1 = (l >> 30) & 3, a1 = (l >> 26) & 3, m1 = (l >> 22) & 3, b1 = (l >> 18) & 3;
    uint32_t p2 = (l >> 28) & 3, a2 = (l >> 24) & 3, m2 = (l >> 20) & 3, b2 = (l >> 16) & 3;
    // Cycle 1: fog color blended in by the shade alpha the RSP computed.
    *fog = p1 == 3 && a1 == 2;
    bool xlu1 = p1 == 0 && a1 == 0 && m1 == 1 && b1 == 0;
    bool xlu2 = p2 == 0 && a2 == 0 && m2 == 1 && b2 == 0;
    bool xlu = cycle == CYCLE_2 ? xlu2 : (xlu1 || xlu2);
    *blend = (xlu && (l & FORCE_BL)) || (l & ZMODE_MASK) == ZMODE_XLU;
    *cutout = (l & 3) == G_AC_THRESHOLD || ((l & CVG_X_ALPHA) && (l & ALPHA_CVG_SEL));
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

static void GFX_OUTLINE update_material(bool rect, int rect_tile) {
    GCOUNT(mat_updates, 1);
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
#ifdef GFX_DIAG
    if (m->textured && gfx_diag == DIAG_NO_TEXTURE) {
        m->tex = diag_dummy;
        m->textured = diag_dummy.ptr != NULL;
    } else
#endif
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
        GCOUNT(lookups_skipped, same);
        if (!same) {
            last.tile = gfx_tiles[tile];
            last.tmem_gen = g.tmem_gen;
            last.index = tile;
            last.tlut = tlut;
            last.variant = m->variant;
#ifdef GFX_DIAG
            int region = dr_time.cur;
            dr_enter(DR_TEX);
#endif
            if (GFX_OPT_ON(OPT_TEX)) {
                last.ok = tex_lookup_fast(tile, tlut, m->variant, &last.tex);
            } else {
                last.ok = tex_lookup(tile, tlut, m->variant, &last.tex);
            }
#ifdef GFX_DIAG
            dr_enter(region);
#endif
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
    m->filter = cycle != CYCLE_COPY && ((g.oh >> 12) & 3) != 0 && !(rect && g.rect_texel_exact);
    bool fog;
    blend_mode(cycle, &m->blend, &m->cutout, &fog);
    bool zb = !rect && (g.geom & G_ZBUFFER);
    m->depth_cmp = zb && (g.ol & Z_CMP);
    m->depth_write = zb && (g.ol & Z_UPD);
    m->decal = m->depth_cmp && (g.ol & ZMODE_MASK) == ZMODE_DEC;

    // The PowerVR draws its opaque list, then punch-through, then translucent, and keeps the
    // submission order only within the translucent list (presort is off). Depth tested surfaces
    // can go to any list, since the depth test orders them. The background (before the first
    // depth tested surface) is opaque and drawn in order. From the first surface without a depth
    // test after that on (HUD, text boxes, fades), everything stays in order in the translucent list.
    if (m->depth_cmp) {
        if (g.phase == PHASE_BACKGROUND) {
            g.phase = PHASE_SCENE;
        }
        m->list = g.phase == PHASE_OVERLAY ? LIST_TR : (m->blend ? LIST_TR : (m->cutout ? LIST_PT : LIST_OP));
    } else if (g.phase == PHASE_BACKGROUND && !m->blend && !m->cutout) {
        m->list = LIST_OP;
    } else {
        m->list = LIST_TR;
        g.phase = PHASE_OVERLAY;
    }
    g.mat_phase = g.phase;

    // Fog: the PowerVR's vertex fog, with the fog factor in the offset color's alpha, takes one fog
    // color for the frame and needs a texture (untextured vertices have no offset color). Anything
    // else has the fog color mixed into its vertex colors.
    m->fog = FOG_NONE;
    if (fog) {
        uint32_t rgb = g.fog_color >> 8;
        if (m->textured && (!g.pvr_fog_set || g.pvr_fog_color == rgb)) {
            g.pvr_fog_set = true;
            g.pvr_fog_color = rgb;
            m->fog = FOG_PVR;
        } else {
            m->fog = FOG_CPU;
            m->fog_rgb[0] = u8f(g.fog_color >> 24);
            m->fog_rgb[1] = u8f(g.fog_color >> 16);
            m->fog_rgb[2] = u8f(g.fog_color >> 8);
        }
    }

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
    k->list = m->list;
    k->blend = m->blend || (m->cutout && m->list == LIST_TR);
    k->fog = m->fog == FOG_PVR;
    k->depth_cmp = m->depth_cmp ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
    k->depth_write = m->depth_write;
    g.mat_key_changed = true;
    g.out_gen++;
    q.mat_new = true;

    stat_mat_updates++;
    g.mat_dirty = false;
    g.mat_for_rect = rect;
    g.mat_rect_tile = rect_tile;
    g.mat_rect_texel_exact = g.rect_texel_exact;
}

static void ensure_material(bool rect, int rect_tile) {
    if (g.mat_dirty || g.mat_for_rect != rect ||
        (rect && (g.mat_rect_tile != rect_tile || g.mat_rect_texel_exact != g.rect_texel_exact)) ||
        g.mat_phase != g.phase) {
        update_material(rect, rect_tile);
    }
}

// Sends a header to the key's list, unless it's the list's current one. Leaves out_list on that list.
static void GFX_OUTLINE submit_header(const HeaderKey* k) {
    GCOUNT(header_calls, 1);
    out_list = k->list;
    if (have_key[k->list] && memcmp(k, &cur_key[k->list], sizeof(*k)) == 0) {
        return;
    }
    GCOUNT(headers, 1);
    cur_key[k->list] = *k;
    have_key[k->list] = true;

    pvr_list_t list = pvr_lists[k->list];
    pvr_poly_cxt_t cxt;
    if (k->tex != NULL) {
        pvr_poly_cxt_txr(&cxt, list, k->tex_fmt | PVR_TXRFMT_TWIDDLED, k->pw, k->ph, k->tex,
                         k->filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST);
        // MODULATE keeps the texel alpha for the punch test. MODULATEALPHA multiplies it
        // by the vertex alpha, which is what blended surfaces want.
        cxt.txr.env = (k->list == LIST_PT && !k->txralpha_dis) ? PVR_TXRENV_MODULATE : PVR_TXRENV_MODULATEALPHA;
        cxt.txr.alpha = k->txralpha_dis ? PVR_TXRALPHA_DISABLE : PVR_TXRALPHA_ENABLE;
        cxt.txr.uv_clamp = k->clamp;
        cxt.txr.uv_flip = k->flip;
    } else {
        pvr_poly_cxt_col(&cxt, list);
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    // Bit 20. Without it the punch-through pass never looks at alpha.
    cxt.gen.alpha = k->blend || k->list == LIST_PT ? PVR_ALPHA_ENABLE : PVR_ALPHA_DISABLE;
    // Real PVR2 applies this blend in the punch-through list, so ONE/ZERO draws the
    // transparent texels as solid. Flycast does not. Src-alpha / inv-src-alpha drops them.
    int punch = k->list == LIST_PT;
    cxt.blend.src = (k->blend || punch) ? PVR_BLEND_SRCALPHA : PVR_BLEND_ONE;
    cxt.blend.dst = (k->blend || punch) ? PVR_BLEND_INVSRCALPHA : PVR_BLEND_ZERO;
    cxt.gen.fog_type = k->fog ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
    cxt.gen.specular = k->offset ? PVR_SPECULAR_ENABLE : PVR_SPECULAR_DISABLE;
    cxt.depth.comparison = k->depth_cmp;
    cxt.depth.write = k->depth_write ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;

    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);
    uint32_t* d = (uint32_t*)out_target();
    const uint32_t* s = (const uint32_t*)&hdr;
    for (int i = 0; i < 8; i++) {
        d[i] = s[i];
    }
    out_commit(d);
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

// OPT_PACK: pack_color's byte for a channel without its float compares. c * 255 is at most 0 for
// c <= 0, under 255 for c < 1 and at least 255 for c >= 1, so the bytes are the same for any color
// but NaN.
static inline uint32_t color_byte(float c) {
    int32_t v = (int32_t)(c * 255.0f);
    if (__builtin_expect((uint32_t)v > 255, 0)) {
        v = v < 0 ? 0 : 255;
    }
    return (uint32_t)v;
}

static inline uint32_t pack_color_int(const float c[4]) {
    return (color_byte(c[3]) << 24) | (color_byte(c[0]) << 16) | (color_byte(c[1]) << 8) | color_byte(c[2]);
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
    if (m->fog == FOG_PVR) {
        out->ofs[3] = fog;
        return;
    }
    out->ofs[3] = 0.0f;
    if (m->fog == FOG_CPU) {
        // The blender's fog * a + color * (1 - a): scale what the texture is multiplied by and mix the
        // fog color into what's added (or into the color, when nothing comes from the texture).
        float k = 1.0f - fog;
        bool tex_rgb = m->textured && m->variant != TEXVAR_ALPHAONLY;
        for (int i = 0; i < 3; i++) {
            if (tex_rgb) {
                out->col[i] *= k;
                out->ofs[i] = out->ofs[i] * k + m->fog_rgb[i] * fog;
            } else {
                out->col[i] = out->col[i] * k + m->fog_rgb[i] * fog;
            }
        }
    }
}

// How far to move a decal toward the camera, in PowerVR depth (1/w, larger is nearer), for a
// triangle given in screen space. The RDP passes a decal within the depth slope of a pixel of the
// surface under it; the PowerVR only has the compare, so the decal is moved forward by that much
// (two Dreamcast pixels per N64 pixel), plus a fraction of its depth for rounding. Slivers have
// steep slopes, so the bias is capped.
#define DECAL_SLOPE_PIXELS 2.0f
#define DECAL_DEPTH_FRAC   (1.0f / 4096.0f)
#define DECAL_MAX_FRAC     (1.0f / 128.0f)

static float GFX_OUTLINE decal_bias(const float x[3], const float y[3], const float z[3]) {
    float x1 = x[1] - x[0], y1 = y[1] - y[0], z1 = z[1] - z[0];
    float x2 = x[2] - x[0], y2 = y[2] - y[0], z2 = z[2] - z[0];
    float area = x1 * y2 - x2 * y1;
    float zmax = fmaxf(z[0], fmaxf(z[1], z[2]));
    float bias = zmax * DECAL_DEPTH_FRAC;
    if (fabsf(area) > 1e-6f) {
        float dzdx = (z1 * y2 - z2 * y1) / area;
        float dzdy = (x1 * z2 - x2 * z1) / area;
        bias += (fabsf(dzdx) + fabsf(dzdy)) * DECAL_SLOPE_PIXELS;
    }
    return fminf(bias, zmax * DECAL_MAX_FRAC);
}

static inline void project_clip(const ClipVert* c, float* x, float* y, float* z) {
    float inv = c->w > 0.0f ? recip_pos(c->w) : 1.0f / c->w;
    *x = c->x * inv * g.vp_sx + g.vp_tx;
    *y = -c->y * inv * g.vp_sy + g.vp_ty;
    *z = g.ortho ? 1.0f - c->z * inv * 0.01f : inv;
}

static inline void emit_vertex(const ClipVert* c, bool eol, bool screen, float zbias) {
    pvr_vertex_t* d = out_target();
    const Material* m = &g.mat;
    float x, y, z;
    if (screen) {
        x = c->x;
        y = c->y;
        z = c->z;
    } else {
        project_clip(c, &x, &y, &z);
        z += zbias;
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
    out_commit(d);
}

// The header for the current material; only compared against the last one sent when the
// material or the offset color changed. Vertex fog reads the offset color, so it needs it on.
static void submit_material_header(bool offset) {
    int list = g.mat_key.list;
    offset |= g.mat.fog == FOG_PVR;
    if (!g.mat_key_changed && have_key[list] && cur_key[list].offset == offset) {
        out_list = list;
        return;
    }
    HeaderKey k = g.mat_key;
    k.offset = offset;
    g.mat_key_changed = false;
    submit_header(&k);
}

static void __attribute__((noinline)) submit_polygon(const ClipVert* v, int n, bool screen, float zbias) {
    if (!list_room(g.mat.list, (uint32_t)n)) {
        return;
    }
    bool offset = false;
    for (int i = 0; i < n && !offset && g.mat.textured; i++) {
        offset = v[i].ofs[0] > 0.004f || v[i].ofs[1] > 0.004f || v[i].ofs[2] > 0.004f;
    }
    submit_material_header(offset);

    // Fan to strip: 0 1 2 for a triangle, 0 1 3 2 for a quad.
    if (n == 3) {
        emit_vertex(&v[0], false, screen, zbias);
        emit_vertex(&v[1], false, screen, zbias);
        emit_vertex(&v[2], true, screen, zbias);
    } else {
        emit_vertex(&v[0], false, screen, zbias);
        emit_vertex(&v[1], false, screen, zbias);
        emit_vertex(&v[3], false, screen, zbias);
        emit_vertex(&v[2], true, screen, zbias);
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

// Prepared combiners for recent combiners and constants: the game switches prim and env colors
// back and forth many times a frame.
// OPT_PROGS uses all of them, otherwise only the first 32.
#define CC_PROGS 256

typedef struct {
    uint32_t w0, w1, prim, env, lod;
    bool two_cycle, valid;
    GfxCCProg prog;
} CCProgEntry;

static CCProgEntry cc_progs[CC_PROGS];

static const GfxCCProg* GFX_OUTLINE find_cc_prog(void) {
    uint32_t h = g.cc.w0 * 0x9E3779B1u ^ g.cc.w1 * 0x85EBCA77u ^ g.prim_raw * 0xC2B2AE3Du ^ g.env_raw * 0x27D4EB2Fu ^
                 g.prim_lod_raw ^ g.cc.two_cycle;
    CCProgEntry* e = &cc_progs[(h ^ (h >> 15)) & (GFX_OPT_ON(OPT_PROGS) ? CC_PROGS - 1 : 31)];
    if (!e->valid || e->w0 != g.cc.w0 || e->w1 != g.cc.w1 || e->two_cycle != g.cc.two_cycle || e->prim != g.prim_raw ||
        e->env != g.env_raw || e->lod != g.prim_lod_raw) {
        e->w0 = g.cc.w0;
        e->w1 = g.cc.w1;
        e->prim = g.prim_raw;
        e->env = g.env_raw;
        e->lod = g.prim_lod_raw;
        e->two_cycle = g.cc.two_cycle;
        e->valid = true;
        GCOUNT(cc_prep, 1);
        cc_prepare(&g.cc, &g.k, &e->prog);
    }
    return &e->prog;
}

static void ensure_combined(Vertex* v) {
    if (v->cc_epoch != g.cc_epoch) {
        GCOUNT(comb, 1);
        if (g.cc_prog_epoch != g.cc_epoch) {
            g.cc_prog = find_cc_prog();
            g.cc_prog_epoch = g.cc_epoch;
        }
        if (g.cc_prog->ok) {
            cc_run(g.cc_prog, v->shade, v->mul, v->add);
        } else {
            GCOUNT(cc_slow, 1);
            cc_eval(&g.cc, &g.k, v->shade, v->mul, v->add);
        }
        v->cc_epoch = g.cc_epoch;
    }
}

#ifdef GFX_DIAG
// DIAG_NO_LIGHT: one color for every vertex, with the texture coordinates as usual.
#define DIAG_GRAY 0xFFC0C0C0u

static void diag_output(Vertex* v) {
    v->argb = DIAG_GRAY;
    v->oargb = 0;
    v->has_ofs = 0;
    if (g.mat.textured) {
        v->u = (v->s * g.mat.shift_s - g.mat.uls) * g.mat.inv_pw;
        v->v = (v->t * g.mat.shift_t - g.mat.ult) * g.mat.inv_ph;
    }
    v->out_gen = g.out_gen;
}

static void diag_clip_vert(const Vertex* v, ClipVert* c) {
    c->x = v->x;
    c->y = v->y;
    c->z = v->z;
    c->w = v->w;
    c->s = v->s;
    c->t = v->t;
    c->col[0] = c->col[1] = c->col[2] = u8f(DIAG_GRAY);
    c->col[3] = 1.0f;
    for (int i = 0; i < 4; i++) {
        c->ofs[i] = 0.0f;
    }
}
#endif

// The PowerVR vertex for an unclipped corner, kept until the material or combiner changes.
static void ensure_output(Vertex* v) {
    if (v->out_gen == g.out_gen) {
        return;
    }
#ifdef GFX_DIAG
    if (gfx_diag == DIAG_NO_LIGHT) {
        diag_output(v);
        return;
    }
#endif
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

static inline void emit_cached(const Vertex* v, uint32_t argb, uint32_t oargb, bool eol, float zbias) {
    pvr_vertex_t* d = out_target();
    d->flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    d->x = v->sx;
    d->y = v->sy;
    d->z = v->sz + zbias;
    d->u = v->u;
    d->v = v->v;
    d->argb = argb;
    d->oargb = oargb;
    out_commit(d);
}

// Lighting runs after the cull, so a vertex used only by rejected triangles never takes the
// light loop. The result matches lighting at load time unless a later command edits the vertex.
static void light_vertex(Vertex* v) {
    if (!v->unlit || DIAG_IS(DIAG_NO_LIGHT)) {
        return;
    }
    v->unlit = 0;
    if (g.lights_dirty) {
        update_lights();
    }
    float n0 = v->nx, n1 = v->ny, n2 = v->nz;
    float r = g.light_col[g.num_lights][0];
    float gg = g.light_col[g.num_lights][1];
    float b = g.light_col[g.num_lights][2];
    for (int l = 0; l < g.num_lights; l++) {
        float d = n0 * g.light_coeff[l][0] + n1 * g.light_coeff[l][1] + n2 * g.light_coeff[l][2];
        if (d > 0.0f) {
            r += d * g.light_col[l][0];
            gg += d * g.light_col[l][1];
            b += d * g.light_col[l][2];
        }
    }
    v->shade[0] = r > 1.0f ? 1.0f : r;
    v->shade[1] = gg > 1.0f ? 1.0f : gg;
    v->shade[2] = b > 1.0f ? 1.0f : b;
    if ((g.geom & G_TEXTURE_GEN) && !v->st_lock) {
        float dx = n0 * g.lookat_coeff[0][0] + n1 * g.lookat_coeff[0][1] + n2 * g.lookat_coeff[0][2];
        float dy = n0 * g.lookat_coeff[1][0] + n1 * g.lookat_coeff[1][1] + n2 * g.lookat_coeff[1][2];
        v->s = (dx * (1.0f / 127.0f) + 1.0f) * 0.25f * (float)g.tex_scale_raw_s * (1.0f / 32.0f);
        v->t = (dy * (1.0f / 127.0f) + 1.0f) * 0.25f * (float)g.tex_scale_raw_t * (1.0f / 32.0f);
    }
}

// light_vertex for a triangle's three corners in one small function, statement for statement.
static void __attribute__((noinline)) light3_fast(Vertex* v0, Vertex* v1, Vertex* v2) {
    Vertex* const corners[3] = { v0, v1, v2 };
#pragma GCC unroll 1
    for (int k = 0; k < 3; k++) {
        Vertex* v = corners[k];
        if (!v->unlit) {
            continue;
        }
        GCOUNT(lit, 1);
#ifdef GFX_XCHECK
        Vertex ref = *v;
        light_vertex(&ref);
#endif
        v->unlit = 0;
        if (g.lights_dirty) {
            update_lights();
        }
        float n0 = v->nx, n1 = v->ny, n2 = v->nz;
        float r = g.light_col[g.num_lights][0];
        float gg = g.light_col[g.num_lights][1];
        float b = g.light_col[g.num_lights][2];
        for (int l = 0; l < g.num_lights; l++) {
            float d = n0 * g.light_coeff[l][0] + n1 * g.light_coeff[l][1] + n2 * g.light_coeff[l][2];
            if (d > 0.0f) {
                r += d * g.light_col[l][0];
                gg += d * g.light_col[l][1];
                b += d * g.light_col[l][2];
            }
        }
        v->shade[0] = r > 1.0f ? 1.0f : r;
        v->shade[1] = gg > 1.0f ? 1.0f : gg;
        v->shade[2] = b > 1.0f ? 1.0f : b;
        if ((g.geom & G_TEXTURE_GEN) && !v->st_lock) {
            float dx = n0 * g.lookat_coeff[0][0] + n1 * g.lookat_coeff[0][1] + n2 * g.lookat_coeff[0][2];
            float dy = n0 * g.lookat_coeff[1][0] + n1 * g.lookat_coeff[1][1] + n2 * g.lookat_coeff[1][2];
            v->s = (dx * (1.0f / 127.0f) + 1.0f) * 0.25f * (float)g.tex_scale_raw_s * (1.0f / 32.0f);
            v->t = (dy * (1.0f / 127.0f) + 1.0f) * 0.25f * (float)g.tex_scale_raw_t * (1.0f / 32.0f);
        }
#ifdef GFX_XCHECK
        gfx_xcheck_runs[XC_LIGHT]++;
        if (memcmp(&ref, v, sizeof(Vertex)) != 0 && gfx_xcheck_fails[XC_LIGHT]++ < 8) {
            printf("[q64dc] xcheck light: fast %f %f %f, original %f %f %f\n", (double)v->shade[0],
                   (double)v->shade[1], (double)v->shade[2], (double)ref.shade[0], (double)ref.shade[1],
                   (double)ref.shade[2]);
        }
#endif
    }
}

// ensure_output for a triangle's three corners in one small function: the combiner,
// finish_colors' mapping and pack_color, statement for statement.
static void __attribute__((noinline)) output3_fast(Vertex* v0, Vertex* v1, Vertex* v2) {
    const Material* m = &g.mat;
    const bool pack_int = GFX_OPT_ON(OPT_PACK);
    Vertex* const corners[3] = { v0, v1, v2 };
#pragma GCC unroll 1
    for (int k = 0; k < 3; k++) {
        Vertex* v = corners[k];
        if (v->out_gen == g.out_gen) {
            continue;
        }
        GCOUNT(out, 1);
#ifdef GFX_XCHECK
        Vertex ref = *v;
        ensure_output(&ref);
#endif
        ensure_combined(v);
        const float* mul = v->mul;
        const float* add = v->add;
        float fog = m->fog ? v->shade[3] : 0.0f;
        float col[4], ofs[4];
        if (!m->textured) {
            for (int i = 0; i < 4; i++) {
                col[i] = add[i];
                ofs[i] = 0.0f;
            }
        } else if (m->variant == TEXVAR_ALPHAONLY) {
            col[0] = add[0];
            col[1] = add[1];
            col[2] = add[2];
            col[3] = mul[3] + add[3];
            ofs[0] = ofs[1] = ofs[2] = 0.0f;
        } else {
            col[0] = mul[0];
            col[1] = mul[1];
            col[2] = mul[2];
            col[3] = m->variant == TEXVAR_NOALPHA ? add[3] : mul[3] + add[3];
            ofs[0] = add[0];
            ofs[1] = add[1];
            ofs[2] = add[2];
        }
        if (m->fog == FOG_PVR) {
            ofs[3] = fog;
        } else {
            ofs[3] = 0.0f;
            if (m->fog == FOG_CPU) {
                float kf = 1.0f - fog;
                bool tex_rgb = m->textured && m->variant != TEXVAR_ALPHAONLY;
                for (int i = 0; i < 3; i++) {
                    if (tex_rgb) {
                        col[i] *= kf;
                        ofs[i] = ofs[i] * kf + m->fog_rgb[i] * fog;
                    } else {
                        col[i] = col[i] * kf + m->fog_rgb[i] * fog;
                    }
                }
            }
        }
        if (pack_int) {
            v->argb = pack_color_int(col);
            v->oargb = pack_color_int(ofs);
        } else {
            v->argb = pack_color(col);
            v->oargb = pack_color(ofs);
        }
        v->has_ofs = m->textured && (ofs[0] > 0.004f || ofs[1] > 0.004f || ofs[2] > 0.004f);
        if (m->textured) {
            v->u = (v->s * m->shift_s - m->uls) * m->inv_pw;
            v->v = (v->t * m->shift_t - m->ult) * m->inv_ph;
        }
        v->out_gen = g.out_gen;
#ifdef GFX_XCHECK
        gfx_xcheck_runs[XC_OUT]++;
        if (memcmp(&ref, v, sizeof(Vertex)) != 0 && gfx_xcheck_fails[XC_OUT]++ < 8) {
            printf("[q64dc] xcheck out: fast %08X %08X %d, original %08X %08X %d\n", (unsigned)v->argb,
                   (unsigned)v->oargb, v->has_ofs, (unsigned)ref.argb, (unsigned)ref.oargb, ref.has_ofs);
        }
#endif
    }
}

// A triangle across the near plane, once its material is current: its corners' colors, then the
// part in front of the plane to the PowerVR.
static void __attribute__((noinline)) tri_emit_near(Vertex* const v[3], bool flat) {
    ClipVert cv[3];
    for (int i = 0; i < 3; i++) {
        Vertex* src = flat ? v[0] : v[i];
#ifdef GFX_DIAG
        if (gfx_diag == DIAG_NO_LIGHT) {
            diag_clip_vert(v[i], &cv[i]);
            continue;
        }
#endif
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
        float zbias = 0.0f;
        if (g.mat.decal) {
            float xs[3], ys[3], zs[3];
            for (int i = 0; i < 3; i++) {
                project_clip(&clipped[i], &xs[i], &ys[i], &zs[i]);
            }
            zbias = decal_bias(xs, ys, zs);
        }
        submit_polygon(clipped, n, false, zbias);
    }
}

static void draw_triangle(int i0, int i1, int i2) {
    Vertex* v[3] = { &g.vtx[i0], &g.vtx[i1], &g.vtx[i2] };
    if (g.cimg == g.zimg) {
        return;
    }
    if (v[0]->clip & v[1]->clip & v[2]->clip) {
        return;
    }
    if (BATCH_POSSIBLE && (vtx_pending & ((1u << i0) | (1u << i1) | (1u << i2)))) {
        vtx_complete((1u << i0) | (1u << i1) | (1u << i2));
    }

    // Back/front face culling, with the winding on screen (y down). A triangle that crosses the
    // near plane has no screen winding yet, so it is culled on the sign of det(x, y, w).
    uint32_t cull = g.geom & (G_CULL_FRONT | G_CULL_BACK);
    bool near = (v[0]->clip | v[1]->clip | v[2]->clip) & CLIP_NEAR;
    if (cull) {
        float cross;
        if (!near) {
            cross = (v[1]->sx - v[0]->sx) * (v[2]->sy - v[0]->sy) - (v[1]->sy - v[0]->sy) * (v[2]->sx - v[0]->sx);
        } else {
            float det = v[0]->x * (v[1]->y * v[2]->w - v[2]->y * v[1]->w) -
                        v[0]->y * (v[1]->x * v[2]->w - v[2]->x * v[1]->w) +
                        v[0]->w * (v[1]->x * v[2]->y - v[2]->x * v[1]->y);
            cross = -det * g.vp_sx * g.vp_sy;
        }
        if ((cull & G_CULL_BACK) && cross >= 0.0f) {
            return;
        }
        if ((cull & G_CULL_FRONT) && cross <= 0.0f) {
            return;
        }
    }

    bool fast_colors = GFX_OPT_ON(OPT_COLOR) && !DIAG_IS(DIAG_NO_LIGHT);
    if (fast_colors) {
        light3_fast(v[0], v[1], v[2]);
    } else {
        light_vertex(v[0]);
        light_vertex(v[1]);
        light_vertex(v[2]);
    }
    if (g.mat_dirty || g.mat_for_rect || g.mat_phase != g.phase) {
        ensure_material(false, 0);
    }

    bool flat = !(g.geom & G_SHADING_SMOOTH);
    stat_frame_tris++;
    stat_list_tris[g.mat.list]++;
    if (DEFER_POSSIBLE && q_on) {
        defer_draw(near ? QD_NEAR : QD_TRI, i0, i1, i2, flat, NULL);
        return;
    }
    if (!near) {
        if (!list_room(g.mat.list, 3)) {
            return;
        }
        if (fast_colors) {
            output3_fast(v[0], v[1], v[2]);
        } else {
            ensure_output(v[0]);
            ensure_output(v[1]);
            ensure_output(v[2]);
        }
        bool offset = flat ? v[0]->has_ofs : (v[0]->has_ofs || v[1]->has_ofs || v[2]->has_ofs);
        submit_material_header(offset);
        float zbias = 0.0f;
        if (g.mat.decal) {
            const float xs[3] = { v[0]->sx, v[1]->sx, v[2]->sx };
            const float ys[3] = { v[0]->sy, v[1]->sy, v[2]->sy };
            const float zs[3] = { v[0]->sz, v[1]->sz, v[2]->sz };
            zbias = decal_bias(xs, ys, zs);
        }
        if (flat) {
            emit_cached(v[0], v[0]->argb, v[0]->oargb, false, zbias);
            emit_cached(v[1], v[0]->argb, (v[0]->oargb & 0x00FFFFFF) | (v[1]->oargb & 0xFF000000), false, zbias);
            emit_cached(v[2], v[0]->argb, (v[0]->oargb & 0x00FFFFFF) | (v[2]->oargb & 0xFF000000), true, zbias);
        } else {
            emit_cached(v[0], v[0]->argb, v[0]->oargb, false, zbias);
            emit_cached(v[1], v[1]->argb, v[1]->oargb, false, zbias);
            emit_cached(v[2], v[2]->argb, v[2]->oargb, true, zbias);
        }
        vbuf_used += 3 * 32;
        return;
    }
    tri_emit_near(v, flat);
}

// The corners of a triangle to the PowerVR, in one copy of emit_cached for flat and smooth shading.
static void __attribute__((noinline)) emit3(const Vertex* v0, const Vertex* v1, const Vertex* v2,
                                            const uint32_t argb[3], const uint32_t oargb[3], float zbias) {
    emit_cached(v0, argb[0], oargb[0], false, zbias);
    emit_cached(v1, argb[1], oargb[1], false, zbias);
    emit_cached(v2, argb[2], oargb[2], true, zbias);
}

// What tri_fast does for a triangle once its material is current: its corners' colors, the header
// and the corners to the PowerVR.
static void __attribute__((noinline)) tri_emit(Vertex* v0, Vertex* v1, Vertex* v2, bool flat, bool fast_colors) {
    if (!list_room(g.mat.list, 3)) {
        return;
    }
    if (fast_colors) {
        output3_fast(v0, v1, v2);
    } else {
        ensure_output(v0);
        ensure_output(v1);
        ensure_output(v2);
    }
    bool offset = flat ? v0->has_ofs : (v0->has_ofs || v1->has_ofs || v2->has_ofs);
    submit_material_header(offset);
    float zbias = 0.0f;
    if (g.mat.decal) {
        const float xs[3] = { v0->sx, v1->sx, v2->sx };
        const float ys[3] = { v0->sy, v1->sy, v2->sy };
        const float zs[3] = { v0->sz, v1->sz, v2->sz };
        zbias = decal_bias(xs, ys, zs);
    }
    // Flat shading takes the first corner's color, and its offset color with each corner's fog.
    uint32_t argb[3], oargb[3];
    if (flat) {
        argb[0] = argb[1] = argb[2] = v0->argb;
        oargb[0] = v0->oargb;
        oargb[1] = (v0->oargb & 0x00FFFFFF) | (v1->oargb & 0xFF000000);
        oargb[2] = (v0->oargb & 0x00FFFFFF) | (v2->oargb & 0xFF000000);
    } else {
        argb[0] = v0->argb;
        argb[1] = v1->argb;
        argb[2] = v2->argb;
        oargb[0] = v0->oargb;
        oargb[1] = v1->oargb;
        oargb[2] = v2->oargb;
    }
    emit3(v0, v1, v2, argb, oargb, zbias);
    vbuf_used += 3 * 32;
}

// --- Deferred output (OPT_DEFER) ---
//
// The display list walk keeps everything that depends on where in the list it happens: rejecting
// triangles, lighting a vertex at its first drawn triangle, materials with their texture lookups,
// combiner changes. Each triangle, near-plane triangle and rectangle that gets that far is
// recorded with its vertices (copied once until their slot changes), its material and its
// combiner (copied when they change). defer_flush then gives them their colors, headers and
// PowerVR vertices in one loop whose code fits the instruction cache: the same functions on the
// same values in the same order.

static void __attribute__((noinline)) defer_flush(void) {
    if (q.ndraw == 0) {
        defer_reset();
        return;
    }
#ifdef GFX_DIAG
    int region = dr_time.cur;
    dr_enter(DR_DEFER);
#endif
    // The walk's own state, which the draws' copies stand in for until the end.
    Material mat = g.mat;
    HeaderKey key = g.mat_key;
    bool key_changed = g.mat_key_changed;
    uint32_t out_gen = g.out_gen;
    GfxCombiner cc = g.cc;
    GfxCCConst k = g.k;
    uint32_t prim_raw = g.prim_raw, env_raw = g.env_raw, prim_lod_raw = g.prim_lod_raw;
    uint32_t cc_epoch = g.cc_epoch, cc_prog_epoch = g.cc_prog_epoch;
    const GfxCCProg* cc_prog = g.cc_prog;
    float vp_sx = g.vp_sx, vp_sy = g.vp_sy, vp_tx = g.vp_tx, vp_ty = g.vp_ty;
    bool ortho = g.ortho;

    bool fast_colors = GFX_OPT_ON(OPT_COLOR);
    uint32_t cur_mat = 0xFFFF, cur_cc = 0xFFFF;
    g.cc_prog_epoch = 0;
    for (uint32_t n = 0; n < q.ndraw; n++) {
        const QDraw* d = &q.draw[n];
        __builtin_prefetch(d + 4);
        if (n + 1 < q.ndraw && d[1].kind != QD_RECT) {
            __builtin_prefetch(&q.vtx[d[1].v[0]]);
            __builtin_prefetch(&q.vtx[d[1].v[1]]);
            __builtin_prefetch(&q.vtx[d[1].v[2]]);
        }
        if (d->mat != cur_mat || d->cc != cur_cc) {
            if (d->mat != cur_mat) {
                cur_mat = d->mat;
                const QMat* m = &q.mat[cur_mat];
                g.mat = m->mat;
                g.mat_key = m->key;
                g.mat_key_changed = true;
            }
            if (d->cc != cur_cc) {
                cur_cc = d->cc;
                const QCC* c = &q.cc[cur_cc];
                g.cc = c->cc;
                g.k = c->k;
                g.prim_raw = c->prim_raw;
                g.env_raw = c->env_raw;
                g.prim_lod_raw = c->prim_lod_raw;
                g.cc_epoch = c->epoch;
            }
            uint32_t mg = q.mat[cur_mat].out_gen, cg = q.cc[cur_cc].out_gen;
            g.out_gen = mg > cg ? mg : cg;
        }
        if (d->kind == QD_TRI) {
            tri_emit(&q.vtx[d->v[0]], &q.vtx[d->v[1]], &q.vtx[d->v[2]], d->flat, fast_colors);
        } else if (d->kind == QD_NEAR) {
            const QView* w = &q.view[d->view];
            g.vp_sx = w->vp_sx;
            g.vp_sy = w->vp_sy;
            g.vp_tx = w->vp_tx;
            g.vp_ty = w->vp_ty;
            g.ortho = w->ortho;
            Vertex* v[3] = { &q.vtx[d->v[0]], &q.vtx[d->v[1]], &q.vtx[d->v[2]] };
            tri_emit_near(v, d->flat);
        } else {
            submit_polygon(q.rect[d->v[0]], 4, true, 0.0f);
        }
    }
    g.mat = mat;
    g.mat_key = key;
    g.mat_key_changed = key_changed;
    g.out_gen = out_gen;
    g.cc = cc;
    g.k = k;
    g.prim_raw = prim_raw;
    g.env_raw = env_raw;
    g.prim_lod_raw = prim_lod_raw;
    g.cc_epoch = cc_epoch;
    g.cc_prog_epoch = cc_prog_epoch;
    g.cc_prog = cc_prog;
    g.vp_sx = vp_sx;
    g.vp_sy = vp_sy;
    g.vp_tx = vp_tx;
    g.vp_ty = vp_ty;
    g.ortho = ortho;
    defer_reset();
#ifdef GFX_DIAG
    dr_enter(region);
#endif
}

// The vertex in a slot as recorded draws see it: copied the first time one needs it after the
// slot was loaded or modified. Only what the second pass reads is copied (x, y, z and w only for
// near-plane triangles, by defer_draw); with cc_epoch and out_gen at 0, no combiner or material
// has, so it works the colors out before reading them.
static inline uint16_t defer_vtx(int slot) {
    int16_t i = q.slot[slot];
    if (i < 0) {
        i = (int16_t)q.nvtx++;
        Vertex* d = &q.vtx[i];
        const Vertex* s = &g.vtx[slot];
        if (GFX_OPT_ON(OPT_ALLOC)) {
            uint8_t* p = (uint8_t*)d;
            dcache_alloc_block(p, 0);
            dcache_alloc_block(p + 32, 0);
            dcache_alloc_block(p + 64, 0);
            dcache_alloc_block(p + 96, 0);
        }
        d->s = s->s;
        d->t = s->t;
        d->shade[0] = s->shade[0];
        d->shade[1] = s->shade[1];
        d->shade[2] = s->shade[2];
        d->shade[3] = s->shade[3];
        d->sx = s->sx;
        d->sy = s->sy;
        d->sz = s->sz;
        d->cc_epoch = 0;
        d->out_gen = 0;
        q.slot[slot] = i;
    }
    return (uint16_t)i;
}

static void __attribute__((noinline)) defer_draw(int kind, int i0, int i1, int i2, bool flat, const ClipVert* rect) {
    if (q.ndraw >= Q_DRAWS || q.nvtx + 3 > Q_VTX || q.nmat >= Q_MATS || q.ncc >= Q_CCS || q.nrect >= Q_RECTS ||
        q.nview >= Q_VIEWS) {
        defer_flush();
    }
    QDraw* d = &q.draw[q.ndraw];
    if (GFX_OPT_ON(OPT_ALLOC) && (q.ndraw & 1) == 0) {
        dcache_alloc_block(d, 0);
    }
    q.ndraw++;
    if (q.mat_new) {
        QMat* m = &q.mat[q.nmat++];
        m->mat = g.mat;
        m->key = g.mat_key;
        m->out_gen = g.out_gen;
        q.mat_new = false;
    }
    if (q.ncc == 0 || q.cc_epoch != g.cc_epoch) {
        QCC* c = &q.cc[q.ncc++];
        c->cc = g.cc;
        c->k = g.k;
        c->prim_raw = g.prim_raw;
        c->env_raw = g.env_raw;
        c->prim_lod_raw = g.prim_lod_raw;
        c->epoch = g.cc_epoch;
        c->out_gen = g.out_gen;
        q.cc_epoch = g.cc_epoch;
    }
    d->kind = (uint8_t)kind;
    d->flat = flat;
    d->mat = (uint16_t)(q.nmat - 1);
    d->cc = (uint16_t)(q.ncc - 1);
    d->view = 0;
    if (kind == QD_RECT) {
        memcpy(q.rect[q.nrect], rect, sizeof(q.rect[0]));
        d->v[0] = (uint16_t)q.nrect++;
        d->v[1] = d->v[2] = 0;
        return;
    }
    d->v[0] = defer_vtx(i0);
    d->v[1] = defer_vtx(i1);
    d->v[2] = defer_vtx(i2);
    if (kind == QD_NEAR) {
        // Clipping reads the corners' clip-space positions.
        const int slots[3] = { i0, i1, i2 };
        for (int k = 0; k < 3; k++) {
            Vertex* c = &q.vtx[d->v[k]];
            const Vertex* s = &g.vtx[slots[k]];
            c->x = s->x;
            c->y = s->y;
            c->z = s->z;
            c->w = s->w;
        }
        QView* w = &q.view[q.nview];
        w->vp_sx = g.vp_sx;
        w->vp_sy = g.vp_sy;
        w->vp_tx = g.vp_tx;
        w->vp_ty = g.vp_ty;
        w->ortho = g.ortho;
        d->view = (uint16_t)q.nview++;
    }
}

// draw_triangle for a triangle in front of the near plane, its usual case, with the lighting,
// colors, material and header work behind calls; triangles that cross the near plane go to
// draw_triangle. Same results, in the same order.
static void tri_fast(int i0, int i1, int i2) {
    Vertex* v0 = &g.vtx[i0];
    Vertex* v1 = &g.vtx[i1];
    Vertex* v2 = &g.vtx[i2];
    if (g.cimg == g.zimg) {
        return;
    }
    GCOUNT(tris, 1);
    uint8_t c0 = v0->clip, c1 = v1->clip, c2 = v2->clip;
    if (c0 & c1 & c2) {
#ifdef GFX_COUNT
        GCOUNT(tri_offscreen, 1);
        count_tri(i0, i1, i2, 2);
#endif
        return;
    }
    if (BATCH_POSSIBLE && (vtx_pending & ((1u << i0) | (1u << i1) | (1u << i2)))) {
        vtx_complete((1u << i0) | (1u << i1) | (1u << i2));
    }
    if ((c0 | c1 | c2) & CLIP_NEAR) {
#ifdef GFX_COUNT
        GCOUNT(tri_near, 1);
        count_tri(i0, i1, i2, 3);
#endif
        draw_triangle(i0, i1, i2);
        return;
    }
    uint32_t cull = g.geom & (G_CULL_FRONT | G_CULL_BACK);
    if (cull) {
        float cross = (v1->sx - v0->sx) * (v2->sy - v0->sy) - (v1->sy - v0->sy) * (v2->sx - v0->sx);
        if (((cull & G_CULL_BACK) && cross >= 0.0f) || ((cull & G_CULL_FRONT) && cross <= 0.0f)) {
#ifdef GFX_COUNT
            GCOUNT(tri_backface, 1);
            count_tri(i0, i1, i2, 2);
#endif
            return;
        }
    }
#ifdef GFX_COUNT
    GCOUNT(tri_drawn, 1);
    count_tri(i0, i1, i2, 3);
#endif

    bool fast_colors = GFX_OPT_ON(OPT_COLOR) && !DIAG_IS(DIAG_NO_LIGHT);
    if (fast_colors) {
        light3_fast(v0, v1, v2);
    } else {
        light_vertex(v0);
        light_vertex(v1);
        light_vertex(v2);
    }
    if (g.mat_dirty || g.mat_for_rect || g.mat_phase != g.phase) {
        ensure_material(false, 0);
    }
    bool flat = !(g.geom & G_SHADING_SMOOTH);
    stat_frame_tris++;
    stat_list_tris[g.mat.list]++;
    if (DEFER_POSSIBLE && q_on) {
        defer_draw(QD_TRI, i0, i1, i2, flat, NULL);
        return;
    }
    tri_emit(v0, v1, v2, flat, fast_colors);
}

#ifdef GFX_XCHECK
// The renderer state a triangle can change, besides g and the once-a-second statistics the
// log thread resets.
typedef struct {
    HeaderKey cur_key[LIST_COUNT];
    bool have_key[LIST_COUNT];
    int out_list;
    uint32_t vbuf_used, frame_tris, list_tris[LIST_COUNT], pending;
} TriState;

static void tri_state_save(TriState* s) {
    memset(s, 0, sizeof(*s));
    memcpy(s->cur_key, cur_key, sizeof(cur_key));
    memcpy(s->have_key, have_key, sizeof(have_key));
    s->out_list = out_list;
    s->vbuf_used = vbuf_used;
    s->frame_tris = stat_frame_tris;
    memcpy(s->list_tris, stat_list_tris, sizeof(stat_list_tris));
    s->pending = vtx_pending;
}

static void tri_state_load(const TriState* s) {
    memcpy(cur_key, s->cur_key, sizeof(cur_key));
    memcpy(have_key, s->have_key, sizeof(have_key));
    out_list = s->out_list;
    vbuf_used = s->vbuf_used;
    stat_frame_tris = s->frame_tris;
    memcpy(stat_list_tris, s->list_tris, sizeof(stat_list_tris));
    vtx_pending = s->pending;
}

// draw_triangle and tri_fast from the same state, compared on what they send to the PowerVR and
// on the state they leave; then tri_fast's output goes to the PowerVR.
static void tri_checked(int i0, int i1, int i2) {
    static __typeof__(g) g_before, g_after;
    static TriState s_before, s_after;
    static uint32_t data[16][8];
    static uint8_t list[16];
    // Deferred triangles send nothing here; GFX_HASH runs compare what their frames send.
    if (DEFER_POSSIBLE && q_on) {
        tri_fast(i0, i1, i2);
        return;
    }
    g_before = g;
    tri_state_save(&s_before);
    cap.on = true;
    cap.n = 0;
    draw_triangle(i0, i1, i2);
    uint32_t n = cap.n;
    memcpy(data, cap.data, n * 32);
    memcpy(list, cap.list, n);
    g_after = g;
    tri_state_save(&s_after);
    g = g_before;
    tri_state_load(&s_before);
    cap.n = 0;
    tri_fast(i0, i1, i2);
    cap.on = false;
    TriState s_fast;
    tri_state_save(&s_fast);
    // pvr_poly_compile leaves a header's last four words as whatever the cache line held; the
    // PowerVR ignores them.
    bool same_out = cap.n == n && memcmp(cap.list, list, n) == 0;
    for (uint32_t i = 0; same_out && i < n; i++) {
        size_t words = (data[i][0] & 0xE0000000u) == 0x80000000u ? 4 : 8;
        same_out = memcmp(cap.data[i], data[i], words * 4) == 0;
    }
    bool same_state = memcmp(&g, &g_after, sizeof(g)) == 0 && memcmp(&s_fast, &s_after, sizeof(s_fast)) == 0;
    gfx_xcheck_runs[XC_TRI]++;
    if ((!same_out || !same_state) && gfx_xcheck_fails[XC_TRI]++ < 8) {
        const uint8_t* a = (const uint8_t*)&g;
        const uint8_t* b = (const uint8_t*)&g_after;
        size_t at = 0;
        while (at < sizeof(g) && a[at] == b[at]) {
            at++;
        }
        printf("[q64dc] xcheck tri %d %d %d: %u blocks (%s), original %u; g differs at %u of %u (vtx at %u..%u), "
               "other state %s\n",
               i0, i1, i2, (unsigned)cap.n, same_out ? "same" : "differ", (unsigned)n, (unsigned)at,
               (unsigned)sizeof(g), (unsigned)offsetof(__typeof__(g), vtx),
               (unsigned)(offsetof(__typeof__(g), vtx) + sizeof(g.vtx)),
               memcmp(&s_fast, &s_after, sizeof(s_fast)) == 0 ? "same" : "differs");
        if (at < sizeof(g) && at >= offsetof(__typeof__(g), vtx) && at < offsetof(__typeof__(g), vtx) + sizeof(g.vtx)) {
            size_t off = at - offsetof(__typeof__(g), vtx);
            printf("[q64dc] xcheck tri: vertex %u byte %u\n", (unsigned)(off / sizeof(Vertex)),
                   (unsigned)(off % sizeof(Vertex)));
        }
    }
    for (uint32_t i = 0; i < cap.n; i++) {
        out_list = cap.list[i];
        uint32_t* d = out_target();
        for (int k = 0; k < 8; k++) {
            d[k] = cap.data[i][k];
        }
        out_commit(d);
    }
    out_list = s_fast.out_list;
}
#define tri_fast tri_checked
#endif

// A triangle of a run. With OPT_REJECT one whose corners share an outside plane stops here, on
// the packed clip codes, where tri_fast would have returned on its vertices' own.
static inline void tri_one(int i0, int i1, int i2) {
    if (GFX_OPT_ON(OPT_REJECT) && (vclip[i0] & vclip[i1] & vclip[i2]) != 0) {
#ifdef GFX_XCHECK
        gfx_xcheck_runs[XC_REJECT]++;
        if ((g.vtx[i0].clip & g.vtx[i1].clip & g.vtx[i2].clip) == 0 && gfx_xcheck_fails[XC_REJECT]++ < 8) {
            printf("[q64dc] xcheck reject: %d %d %d packed %X %X %X, vertices %X %X %X\n", i0, i1, i2, vclip[i0],
                   vclip[i1], vclip[i2], g.vtx[i0].clip, g.vtx[i1].clip, g.vtx[i2].clip);
        }
#endif
#ifdef GFX_COUNT
        if (g.cimg != g.zimg) {
            GCOUNT(tris, 1);
            GCOUNT(tri_offscreen, 1);
            count_tri(i0, i1, i2, 2);
        }
#endif
        return;
    }
    tri_fast(i0, i1, i2);
}

// A triangle command (w0, w1) and the triangle commands after it at pc, in one loop that keeps
// the triangle path in the instruction cache. Returns the address of the next other command.
static uint32_t __attribute__((noinline)) tri_run(uint32_t w0, uint32_t w1, uint32_t pc) {
    for (;;) {
        if (!DIAG_IS(DIAG_NO_TRI)) {
            if ((w0 >> 24) == 0xB1) {  // G_TRI2
                tri_one(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                tri_one(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
            } else {  // G_TRI1
                tri_one(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
            }
        }
        if (!addr_ok(pc, 8)) {
            return pc;
        }
        __builtin_prefetch(RDRAM_PTR(pc + 64));
        w0 = rd32(pc);
        uint8_t op = w0 >> 24;
        if (op != 0xB1 && op != 0xBF) {
            return pc;
        }
        w1 = rd32(pc + 4);
        pc += 8;
#ifdef GFX_DIAG
        dg.cmds++;
        dg.tri_cmds += op == 0xB1 ? 2 : 1;
#endif
        GCOUNT(cmds, 1);
        GCOUNT(op[op], 1);
    }
}
#ifdef GFX_XCHECK
#undef tri_fast
#endif

// Fill and texture rectangles, in N64 pixels.
static void __attribute__((noinline)) draw_rect(float x0, float y0, float x1, float y1, int tile, float s, float t, float dsdx, float dtdy,
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

    // Text, the HUD and menus are drawn this way. Filtering them at 640x480 would mix in the
    // colors of transparent texels (pink in the game's fonts), which the N64 never shows.
    const GfxTile* tt = &gfx_tiles[tile & 7];
    g.rect_texel_exact = textured && fabsf(dsdx) == 1.0f && fabsf(dtdy) == 1.0f && s == floorf(s) && t == floorf(t) &&
                         tt->shifts == 0 && tt->shiftt == 0;
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
        // cc_eval only reads the combiner and its colors, which bump cc_epoch when they change.
        static uint32_t rect_epoch;
        static float rect_mul[4], rect_add[4];
        if (GFX_OPT_ON(OPT_RECT)) {
            if (rect_epoch != g.cc_epoch) {
                cc_eval(&g.cc, &g.k, no_shade, rect_mul, rect_add);
                rect_epoch = g.cc_epoch;
            }
            memcpy(mul, rect_mul, sizeof(rect_mul));
            memcpy(add, rect_add, sizeof(rect_add));
#ifdef GFX_XCHECK
            float rm[4], ra[4];
            cc_eval(&g.cc, &g.k, no_shade, rm, ra);
            gfx_xcheck_runs[XC_RECT]++;
            if ((memcmp(rm, mul, sizeof(rm)) != 0 || memcmp(ra, add, sizeof(ra)) != 0) &&
                gfx_xcheck_fails[XC_RECT]++ < 8) {
                printf("[q64dc] xcheck rect: kept %f %f, cc_eval %f %f\n", (double)mul[0], (double)add[0],
                       (double)rm[0], (double)ra[0]);
            }
#endif
        } else {
            rect_epoch = 0;
            cc_eval(&g.cc, &g.k, no_shade, mul, add);
        }
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
    if (DEFER_POSSIBLE && q_on) {
        defer_draw(QD_RECT, 0, 0, 0, false, v);
        return;
    }
    submit_polygon(v, 4, true, 0.0f);
}

// --- Display list ---

static void cc_changed(void) {
    g.cc_epoch++;
    g.mat_dirty = true;
}

// Prim and env colors. The material doesn't depend on them (update_material reads only which
// inputs the combiner uses), so with OPT_COLORS they only make the vertices' colors stale, which a
// new material would otherwise do.
static void cc_colors_changed(void) {
    if (GFX_OPT_ON(OPT_COLORS)) {
        g.cc_epoch++;
        g.out_gen++;
    } else {
        cc_changed();
    }
}

static void set_combine(uint32_t w0, uint32_t w1) {
    bool two_cycle = ((g.oh >> 20) & 3) == CYCLE_2;
    static bool compiled;
    if (compiled && g.cc.w0 == w0 && g.cc.w1 == w1 && g.cc.two_cycle == two_cycle) {
        return;
    }
    compiled = true;
    if (GFX_OPT_ON(OPT_RECT)) {
        // cc_compile depends on nothing else, and the game goes back and forth between a few.
        static GfxCombiner seen[8];
        static bool seen_ok[8];
        uint32_t h = ((w0 * 0x9E3779B1u) ^ (w1 * 0x85EBCA77u) ^ (uint32_t)two_cycle) >> 29;
        GfxCombiner* e = &seen[h];
        if (seen_ok[h] && e->w0 == w0 && e->w1 == w1 && e->two_cycle == two_cycle) {
            g.cc = *e;
        } else {
            cc_compile(&g.cc, w0, w1, two_cycle);
            *e = g.cc;
            seen_ok[h] = true;
        }
#ifdef GFX_XCHECK
        GfxCombiner ref;
        memset(&ref, 0, sizeof(ref));
        cc_compile(&ref, w0, w1, two_cycle);
        gfx_xcheck_runs[XC_COMBINE]++;
        if ((ref.w0 != g.cc.w0 || ref.w1 != g.cc.w1 || ref.two_cycle != g.cc.two_cycle ||
             memcmp(ref.in, g.cc.in, sizeof(ref.in)) != 0 || ref.tex_rgb != g.cc.tex_rgb ||
             ref.tex_alpha != g.cc.tex_alpha) &&
            gfx_xcheck_fails[XC_COMBINE]++ < 8) {
            printf("[q64dc] xcheck combine: %08X %08X\n", (unsigned)w0, (unsigned)w1);
        }
#endif
    } else {
        cc_compile(&g.cc, w0, w1, two_cycle);
    }
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

static void GFX_OUTLINE run_dl(uint32_t start) {
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
#ifdef GFX_DIAG
        dg.cmds++;
        dg.tri_cmds += dl_region[op] == DR_TRI ? (op == 0xB1 ? 2 : 1) : 0;
        dg.loads += dl_region[op] == DR_TEX;
        dr_enter(dl_region[op]);
#endif
        GCOUNT(cmds, 1);
        GCOUNT(op[op], 1);
        if (GFX_OPT_ON(OPT_TRI)) {
            __builtin_prefetch(RDRAM_PTR(pc + 64));
            if (op == 0xB1 || op == 0xBF) {
                pc = tri_run(w0, w1, pc);
                continue;
            }
        }
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
                GCOUNT(mtx, 1);
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
                touch_mvp();
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
                    touch_project();
                } else if (idx == 0x82 || idx == 0x84) {  // look-at Y, X
                    int i = idx == 0x84 ? 0 : 1;
                    for (int j = 0; j < 3; j++) {
                        g.lookat_dir[i][j] = (float)(int8_t)rd8(a + 8 + j) * (1.0f / 127.0f);
                    }
                    g.lights_dirty = true;
                    g.light_seq++;
                } else if (idx >= 0x86 && idx <= 0x94) {  // lights
                    int i = (idx - 0x86) / 2;
                    for (int j = 0; j < 3; j++) {
                        g.light_col[i][j] = u8f(rd8(a + j));
                        g.light_dir[i][j] = (float)(int8_t)rd8(a + 8 + j) * (1.0f / 127.0f);
                    }
                    g.lights_dirty = true;
                    g.light_seq++;
                }
                break;
            }

            case 0x04: {  // G_VTX
                uint32_t n = (w0 >> 10) & 0x3F;
                uint32_t v0 = ((w0 >> 16) & 0xFF) / 2;
                if (GFX_OPT_ON(OPT_VTX) && GFX_OPT_ON(OPT_BATCH)) {
                    load_vertices_batch(seg_addr(w1), v0, n);
                } else if (GFX_OPT_ON(OPT_VTX)) {
                    load_vertices_fast(seg_addr(w1), v0, n);
                } else {
                    load_vertices(seg_addr(w1), v0, n);
                }
#ifdef GFX_COUNT
                count_batch(g.vtx, v0, n);
#endif
                break;
            }

            case 0x06: {  // G_DL
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (sp >= DL_STACK_SIZE) {
                        printf("[q64dc] gfx: display list stack overflow\n");
                        return;
                    }
#ifdef GFX_COUNT
                    GCOUNT(dl_calls, 1);
                    dl_mark[sp].verts = gfx_count.vtx_verts;
                    dl_mark[sp].tris = gfx_count.tris;
                    dl_mark[sp].drawn = gfx_count.tri_drawn + gfx_count.tri_near;
#endif
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
#ifdef GFX_DIAG
                if (gfx_diag == DIAG_NO_TRI) {
                    break;
                }
#endif
                draw_triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                draw_triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;

            case 0xB2: {  // G_MODIFYVTX
                uint32_t where = (w0 >> 16) & 0xFF;
                uint32_t i = (w0 & 0xFFFF) / 2;
                if (i >= 32) {
                    break;
                }
                if (BATCH_POSSIBLE) {
                    vtx_complete(1u << i);
                }
                defer_forget(i, 1);
                Vertex* v = &g.vtx[i];
                if (where == 0x10) {
                    unpack_rgba(w1, v->shade);
                    v->unlit = 0;
                } else if (where == 0x14) {
                    v->s = (float)(int16_t)(w1 >> 16) * (1.0f / 32.0f);
                    v->t = (float)(int16_t)w1 * (1.0f / 32.0f);
                    v->st_lock = 1;
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
                if (!GFX_OPT_ON(OPT_NOOP) || (g.geom & w1) != 0) {
                    g.geom &= ~w1;
                    g.mat_dirty = true;
                }
                break;

            case 0xB7:  // G_SETGEOMETRYMODE
                if (!GFX_OPT_ON(OPT_NOOP) || (g.geom | w1) != g.geom) {
                    g.geom |= w1;
                    g.mat_dirty = true;
                }
                break;

            case 0xB8:  // G_ENDDL
                if (sp == 0) {
                    return;
                }
                pc = stack[--sp];
#ifdef GFX_COUNT
                if (gfx_count.tri_drawn + gfx_count.tri_near == dl_mark[sp].drawn &&
                    gfx_count.vtx_verts != dl_mark[sp].verts) {
                    int d = sp < 3 ? sp : 3;
                    GCOUNT(dead_calls[d], 1);
                    GCOUNT(dead_verts[d], gfx_count.vtx_verts - dl_mark[sp].verts);
                    GCOUNT(dead_tris[d], gfx_count.tris - dl_mark[sp].tris);
                }
#endif
                break;

            case 0xB9:    // G_SETOTHERMODE_L
            case 0xBA: {  // G_SETOTHERMODE_H
                uint32_t shift = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
                uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
                if (op == 0xB9) {
                    uint32_t ol = (g.ol & ~mask) | (w1 & mask);
                    if (!GFX_OPT_ON(OPT_NOOP) || ol != g.ol) {
                        g.ol = ol;
                        g.mat_dirty = true;
                    }
                } else {
                    uint32_t oh = (g.oh & ~mask) | (w1 & mask);
                    if (!GFX_OPT_ON(OPT_NOOP) || oh != g.oh) {
                        set_othermode_h(oh);
                    }
                }
                break;
            }

            case 0xBB:  // G_TEXTURE
                if (GFX_OPT_ON(OPT_NOOP) && g.tex_on == ((w0 & 0xFF) != 0) && g.tex_tile == (int)((w0 >> 8) & 7) &&
                    g.tex_scale_raw_s == w1 >> 16 && g.tex_scale_raw_t == (w1 & 0xFFFF)) {
                    break;
                }
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
                    g.light_seq++;
                } else if (index == 0x08) {
                    g.fog_mul = (float)(int16_t)(w1 >> 16);
                    g.fog_off = (float)(int16_t)w1;
                    touch_project();
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
                touch_mvp();
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
                GCOUNT(culldl, 1);
                if (all & (0x0F | CLIP_NEAR)) {
                    GCOUNT(culldl_hits, 1);
                    stat_culled_dls++;
                    if (sp == 0) {
                        return;
                    }
                    pc = stack[--sp];
                }
                break;
            }

            case 0xBF:  // G_TRI1
#ifdef GFX_DIAG
                if (gfx_diag == DIAG_NO_TRI) {
                    break;
                }
#endif
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

            case 0xED: {  // G_SETSCISSOR
                // The world scissor is 8 pixels inside 320x240, the part a TV crops. Rectangles
                // are clipped to it, so a screen-filling background and anything in that margin
                // (the gold counter's right edge) stop short of the frame, and the gap is black.
                // A scissor that only trims that margin is the whole frame. A tighter one still clips.
                float x0 = (float)((w0 >> 12) & 0xFFF) * 0.25f;
                float y0 = (float)(w0 & 0xFFF) * 0.25f;
                float x1 = (float)((w1 >> 12) & 0xFFF) * 0.25f;
                float y1 = (float)(w1 & 0xFFF) * 0.25f;
                if (x0 <= 16.0f && y0 <= 16.0f && x1 >= 320.0f - 16.0f && y1 >= 240.0f - 16.0f) {
                    x0 = 0.0f;
                    y0 = 0.0f;
                    x1 = 320.0f;
                    y1 = 240.0f;
                }
                g.sc_x0 = x0;
                g.sc_y0 = y0;
                g.sc_x1 = x1;
                g.sc_y1 = y1;
                break;
            }

            case 0xEF:  // G_RDPSETOTHERMODE
                if (GFX_OPT_ON(OPT_NOOP) && (w0 & 0x00FFFFFF) == g.oh && w1 == g.ol) {
                    break;
                }
                set_othermode_h(w0 & 0x00FFFFFF);
                g.ol = w1;
                g.mat_dirty = true;
                break;

            case 0xF0:  // G_LOADTLUT
                if (tmem_load_tlut((w1 >> 24) & 7, ((w1 >> 14) & 0x3FF) + 1)) {
                    g.tmem_gen++;
                    g.mat_dirty = true;
                }
                break;

            case 0xF2: {  // G_SETTILESIZE
                GfxTile* t = &gfx_tiles[(w1 >> 24) & 7];
                if (GFX_OPT_ON(OPT_NOOP) && t->uls == ((w0 >> 12) & 0xFFF) && t->ult == (w0 & 0xFFF) &&
                    t->lrs == ((w1 >> 12) & 0xFFF) && t->lrt == (w1 & 0xFFF)) {
                    break;
                }
                t->uls = (w0 >> 12) & 0xFFF;
                t->ult = w0 & 0xFFF;
                t->lrs = (w1 >> 12) & 0xFFF;
                t->lrt = w1 & 0xFFF;
                g.mat_dirty = true;
                break;
            }

            case 0xF3:  // G_LOADBLOCK
                if (tmem_load_block((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF)) {
                    g.tmem_gen++;
                    g.mat_dirty = true;
                }
                break;

            case 0xF4: {  // G_LOADTILE
                int tile = (w1 >> 24) & 7;
                uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF, lrs = (w1 >> 12) & 0xFFF, lrt = w1 & 0xFFF;
                GfxTile* t = &gfx_tiles[tile];
                bool loaded = tmem_load_tile(tile, uls, ult, lrs, lrt);
                if (loaded || t->uls != uls || t->ult != ult || t->lrs != lrs || t->lrt != lrt) {
                    if (loaded) {
                        g.tmem_gen++;
                    }
                    t->uls = uls;
                    t->ult = ult;
                    t->lrs = lrs;
                    t->lrt = lrt;
                    g.mat_dirty = true;
                }
                break;
            }

            case 0xF5: {  // G_SETTILE
                GfxTile* t = &gfx_tiles[(w1 >> 24) & 7];
                if (GFX_OPT_ON(OPT_NOOP) && t->fmt == ((w0 >> 21) & 7) && t->siz == ((w0 >> 19) & 3) &&
                    t->line == ((w0 >> 9) & 0x1FF) && t->tmem == (w0 & 0x1FF) && t->palette == ((w1 >> 20) & 0xF) &&
                    t->cmt == ((w1 >> 18) & 3) && t->maskt == ((w1 >> 14) & 0xF) && t->shiftt == ((w1 >> 10) & 0xF) &&
                    t->cms == ((w1 >> 8) & 3) && t->masks == ((w1 >> 4) & 0xF) && t->shifts == (w1 & 0xF)) {
                    break;
                }
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
                    g.mat_dirty = true;
                }
                break;

            case 0xF9:  // G_SETBLENDCOLOR
                break;

            case 0xFA:  // G_SETPRIMCOLOR
                if (GFX_OPT_ON(OPT_NOOP) && w1 == g.prim_raw && (w0 & 0xFF) == g.prim_lod_raw) {
                    break;
                }
                unpack_rgba(w1, g.k.prim);
                g.k.prim_lod = u8f(w0);
                g.prim_raw = w1;
                g.prim_lod_raw = w0 & 0xFF;
                cc_colors_changed();
                break;

            case 0xFB:  // G_SETENVCOLOR
                if (GFX_OPT_ON(OPT_NOOP) && w1 == g.env_raw) {
                    break;
                }
                unpack_rgba(w1, g.k.env);
                g.env_raw = w1;
                cc_colors_changed();
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

// 0xA05F8000 + 0x011C. Only punch-through pixels with alpha >= this are drawn.
#define PVR_PT_ALPHA_REF 0x011C
#define PT_ALPHA_REF     0x80

static int pvr_up;

static void ensure_pvr(void) {
    if (pvr_up) {
        return;
    }
    screen_mark("pvr init");
    pvr_init_params_t params = {
        // 32 words is 32 polygon pointers per 32x32 tile, the largest single bin. A town
        // wall with a door and a window overflows that. The extra sets are where the TA
        // continues a full bin; with none, it writes into the next tile and that part of
        // the screen flickers the wrong polygon. Flycast does not enforce the bins.
        .opb_sizes = { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32 },
        .vertex_buf_size = VERTEX_BUF_SIZE,
        .dma_enabled = 0,
        .fsaa_enabled = 0,
        .autosort_disabled = 1,
        .opb_overflow_count = 3,
    };
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_set_zclip(0.000001f);
    // KOS 2.2.1 never writes this. The compare is >=. 0x80 keeps a 1555 alpha of 1
    // (expanded to 255) and a 4444 nibble >= 8, and drops the rest. A reset value of
    // 0 keeps every texel. Set before the first scene; the TA can start at list begin.
    PVR_SET(PVR_PT_ALPHA_REF, PT_ALPHA_REF);
    pvr_up = 1;
#ifdef GFX_DIAG
    diag_pvr_init();
#endif
    screen_log_pvr(1);
    printf("[q64dc] gfx: PowerVR ready, %u KB of VRAM free\n", (unsigned)(pvr_mem_available() / 1024));
}

void gfx_init(void) {
    tex_init();
    list_buf[LIST_PT] = memalign(32, LIST_BUF_SIZE);
    list_buf[LIST_TR] = memalign(32, LIST_BUF_SIZE);
    if (list_buf[LIST_PT] == NULL || list_buf[LIST_TR] == NULL) {
        runtime_fatal("[q64dc] gfx: out of RAM for list buffers");
    }
    if (DEFER_POSSIBLE) {
        q.draw = memalign(32, Q_DRAWS * sizeof(QDraw));
        q.vtx = memalign(32, Q_VTX * sizeof(Vertex));
        q.mat = memalign(32, Q_MATS * sizeof(QMat));
        q.cc = memalign(32, Q_CCS * sizeof(QCC));
        q.rect = memalign(32, Q_RECTS * sizeof(q.rect[0]));
        q.view = memalign(32, Q_VIEWS * sizeof(QView));
        q.ok = q.draw && q.vtx && q.mat && q.cc && q.rect && q.view;
        if (!q.ok) {
            printf("[q64dc] gfx: no RAM for deferred draws, drawing as the list runs\n");
        }
        defer_reset();
    }

    memset(&g, 0, sizeof(g));
    for (int i = 0; i < 4; i++) {
        g.proj[i][i] = 1.0f;
        g.mv[0][i][i] = 1.0f;
    }
    touch_mvp();
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
#ifndef SCREEN_LOG
    ensure_pvr();
#endif
}

// A red bar along the top of the screen on test discs.
static void draw_test_banner(void) {
    if (list_used[LIST_TR] + 5 * 32 > LIST_BUF_SIZE) {
        return;
    }
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&hdr, &cxt);
    out_list = LIST_TR;
    have_key[LIST_TR] = false;
    uint32_t* h = (uint32_t*)out_target();
    memcpy(h, &hdr, sizeof(hdr));
    out_commit(h);
    const float xs[4] = { 0.0f, 640.0f, 0.0f, 640.0f }, ys[4] = { 0.0f, 0.0f, 6.0f, 6.0f };
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t* d = out_target();
        d->flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        d->x = xs[i];
        d->y = ys[i];
        d->z = 1.0f;
        d->u = d->v = 0.0f;
        d->argb = 0xFFFF0000;
        d->oargb = 0;
        out_commit(d);
    }
}

#ifdef PERF_HUD
// 3x5 glyphs, bit 2 is the left pixel. Indexed by hud_glyph().
static const char hud_chars[] = " 0123456789ACDEFGIKLMNOPRSTUVX.BH"
#ifdef GFX_DIAG
                                "WQ"
#endif
    ;
static const uint8_t hud_bits[][5] = {
    { 0, 0, 0, 0, 0 }, { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
    { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 },
    { 7, 5, 7, 1, 7 }, { 7, 5, 7, 5, 5 }, { 7, 4, 4, 4, 7 }, { 6, 5, 5, 5, 6 }, { 7, 4, 6, 4, 7 },
    { 7, 4, 6, 4, 4 }, { 7, 4, 5, 5, 7 }, { 7, 2, 2, 2, 7 }, { 5, 5, 6, 5, 5 }, { 4, 4, 4, 4, 7 },
    { 5, 7, 5, 5, 5 }, { 7, 5, 5, 5, 5 }, { 7, 5, 5, 5, 7 }, { 7, 5, 7, 4, 4 }, { 7, 5, 7, 6, 5 },
    { 7, 4, 7, 1, 7 },     { 7, 2, 2, 2, 2 }, { 5, 5, 5, 5, 7 }, { 5, 5, 5, 5, 2 }, { 5, 5, 2, 5, 5 },
    { 0, 0, 0, 0, 2 }, { 7, 5, 6, 5, 7 }, { 5, 5, 7, 5, 5 },
#ifdef GFX_DIAG
    { 5, 5, 5, 7, 5 }, { 7, 5, 5, 7, 1 },
#endif
};
_Static_assert(sizeof(hud_bits) / sizeof(hud_bits[0]) == sizeof(hud_chars) - 1, "hud font");

#ifdef GFX_DIAG
// The glyphs as one texture, 4x8 texels each, so a character is one quad rather than a quad
// for each run of pixels: the HUD's own share of the translucent list, and so of its DMA and the
// tile accelerator's work, stays well below the pixel font's while it measures them.
#define HUD_FONT_W 256
#define HUD_FONT_H 8
static pvr_ptr_t hud_font;
_Static_assert(sizeof(hud_bits) / sizeof(hud_bits[0]) * 4 <= HUD_FONT_W, "hud font texture");

static void diag_pvr_init(void) {
    static uint16_t texels[HUD_FONT_W * HUD_FONT_H] __attribute__((aligned(32)));
    for (uint32_t i = 0; i < sizeof(hud_bits) / sizeof(hud_bits[0]); i++) {
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                texels[row * HUD_FONT_W + i * 4 + col] = (hud_bits[i][row] & (4 >> col)) ? 0xFFFF : 0x0000;
            }
        }
    }
    // Plain stores, as for the texture cache.
    hud_font = pvr_mem_malloc(sizeof(texels));
    if (hud_font != NULL) {
        volatile uint32_t* dst = (volatile uint32_t*)hud_font;
        const uint32_t* src = (const uint32_t*)texels;
        for (uint32_t i = 0; i < sizeof(texels) / 4; i++) {
            dst[i] = src[i];
        }
    }
    // Light gray, 8x8.
    diag_dummy.ptr = pvr_mem_malloc(8 * 8 * 2);
    if (diag_dummy.ptr != NULL) {
        volatile uint32_t* dst = (volatile uint32_t*)diag_dummy.ptr;
        for (int i = 0; i < 8 * 8 / 2; i++) {
            dst[i] = 0xFCCCFCCCu;
        }
        diag_dummy.pvr_fmt = PVR_TXRFMT_ARGB4444;
        diag_dummy.pw = 8;
        diag_dummy.ph = 8;
    }
    printf("[q64dc] gfx diag: font texture %s, dummy texture %s\n", hud_font ? "ok" : "missing",
           diag_dummy.ptr ? "ok" : "missing");
}

// A character cell of the font texture on 6x10 pixels.
static void hud_glyph_quad(float x, float y, int glyph) {
    float u0 = (float)(glyph * 4) * (1.0f / HUD_FONT_W), u1 = (float)(glyph * 4 + 3) * (1.0f / HUD_FONT_W);
    const float xs[4] = { x, x + 6.0f, x, x + 6.0f };
    const float ys[4] = { y, y, y + 10.0f, y + 10.0f };
    const float us[4] = { u0, u1, u0, u1 };
    const float vs[4] = { 0.0f, 0.0f, 5.0f / HUD_FONT_H, 5.0f / HUD_FONT_H };
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t* d = out_target();
        d->flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        d->x = xs[i];
        d->y = ys[i];
        d->z = 1.0f;
        d->u = us[i];
        d->v = vs[i];
        d->argb = 0xFFFFFFFF;
        d->oargb = 0;
        out_commit(d);
    }
}
#endif

static int hud_glyph(char c) {
    for (int i = 0; hud_chars[i] != 0; i++) {
        if (hud_chars[i] == c) {
            return i;
        }
    }
    return 0;
}

static void hud_quad(float x, float y, float w, float h, uint32_t argb) {
    const float xs[4] = { x, x + w, x, x + w };
    const float ys[4] = { y, y, y + h, y + h };
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t* d = out_target();
        d->flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        d->x = xs[i];
        d->y = ys[i];
        d->z = 1.0f;
        d->u = d->v = 0.0f;
        d->argb = argb;
        d->oargb = 0;
        out_commit(d);
    }
}

static void hud_text(float x, float y, const char* s) {
#ifdef GFX_DIAG
    if (hud_font != NULL) {
        for (int n = 0; s[n] != 0; n++) {
            int glyph = hud_glyph(s[n]);
            if (glyph != 0) {
                hud_glyph_quad(x + n * 8.0f, y, glyph);
            }
        }
        return;
    }
#endif
    for (int n = 0; s[n] != 0; n++) {
        const uint8_t* rows = hud_bits[hud_glyph(s[n])];
        float cx = x + n * 8.0f;
        for (int row = 0; row < 5; row++) {
            int bit = 4;
            int col = 0;
            while (col < 3) {
                if ((rows[row] & bit) == 0) {
                    bit >>= 1;
                    col++;
                    continue;
                }
                int start = col;
                while (col < 3 && (rows[row] & bit) != 0) {
                    bit >>= 1;
                    col++;
                }
                hud_quad(cx + start * 2.0f, y + row * 2.0f, (col - start) * 2.0f, 2.0f, 0xFFFFFFFF);
            }
        }
    }
}

#ifdef GFX_DIAG
#define HUD_LINES 12
#define HUD_COLS  38
#else
#define HUD_LINES 8
#define HUD_COLS  22
#endif
static char hud_lines[HUD_LINES][HUD_COLS];
static int hud_ready;

typedef struct {
    uint64_t game, rsp, aud, rom, idle, other;
} HudCpu;

static int hud_cpu_cb(kthread_t* thd, void* data) {
    HudCpu* s = data;
    uint64_t t = thd_get_cpu_time(thd);
    const char* label = thd_get_label(thd);
    if (label == NULL) {
        s->other += t;
    } else if (strcmp(label, "n64 thread") == 0) {
        s->game += t;
    } else if (strcmp(label, "n64 rsp") == 0) {
        s->rsp += t;
    } else if (strcmp(label, "audio stream") == 0) {
        s->aud += t;
    } else if (strcmp(label, "n64 pi") == 0) {
        s->rom += t;
    } else if (strcmp(label, "[idle]") == 0) {
        s->idle += t;
    } else {
        s->other += t;
    }
    return 0;
}

static uint32_t hud_pct(uint64_t ns, uint32_t dt_us) {
    if (dt_us == 0) {
        return 0;
    }
    uint32_t pct = (uint32_t)(ns / (uint64_t)dt_us / 10);
    return pct > 100 ? 100 : pct;
}

#ifdef GFX_DIAG
// Tenths as "12.3", whole numbers from 100 up.
static const char* hud_tenths(char* buf, uint32_t v10) {
    if (v10 >= 1000) {
        snprintf(buf, 12, "%u", (unsigned)(v10 / 10));
    } else {
        snprintf(buf, 12, "%u.%u", (unsigned)(v10 / 10), (unsigned)(v10 % 10));
    }
    return buf;
}

// Lines 8-11, per frame drawn since the last refresh: the diagnostic mode, then milliseconds
// (the display list by kind of command, the end-of-frame submission, the wait for the tile
// accelerator, the RSP thread's cache miss stalls), then counts and KB.
static void diag_hud_lines(void) {
    typedef struct {
        uint32_t frames, ticks[DR_COUNT], wait_us, submit_us, oc_stall, ic_stall;
        uint32_t loads, sum_bytes, decodes, upload_bytes, lookups, ta_bytes, dma_bytes, drawn, tri_cmds, cmds;
    } Sums;
    static Sums prev;
    Sums cur = { dg.frames,
                 { dr_time.ticks[DR_OTHER], dr_time.ticks[DR_VTX], dr_time.ticks[DR_TRI], dr_time.ticks[DR_TEX],
                   dr_time.ticks[DR_DEFER] },
                 dg.wait_us, dg.submit_us, dg.oc_stall, dg.ic_stall, dg.loads, gfx_diag_tex.sum_bytes,
                 gfx_diag_tex.decodes, gfx_diag_tex.upload_bytes, gfx_diag_tex.lookups, dg.ta_bytes, dg.dma_bytes,
                 dg.drawn, dg.tri_cmds, dg.cmds };
    uint32_t n = cur.frames - prev.frames;
    if (n == 0) {
        n = 1;
    }
#define PER(f) ((cur.f - prev.f) / n)
#define TICKS10(d) ((d) / n / DIAG_TICKS_PER_100US)
#define KB(f) ((PER(f) + 512) / 1024)
    uint32_t dl = 0;
    for (int i = 0; i < DR_COUNT; i++) {
        dl += cur.ticks[i] - prev.ticks[i];
    }
    char a[12], b[12], c[12], d[12], e[12];
    // 5 ns per cycle: 20000 cycles to the tenth of a millisecond.
    // The set's number and its optimizations, a dot for each one that is off.
    static const char letters[] = "TSLVCDBRKPWUQMNEOGF";
    char on[sizeof(letters)];
    for (int i = 0; i < (int)sizeof(letters) - 1; i++) {
        on[i] = (gfx_opt >> i) & 1 ? letters[i] : '.';
    }
    on[sizeof(letters) - 1] = 0;
    // The diagnostic mode only when there is one, for room.
    if (gfx_diag == DIAG_NORMAL) {
        snprintf(hud_lines[8], HUD_COLS, "C%d %s RO%s RI%s", opt_set, on, hud_tenths(b, PER(oc_stall) / 20000),
                 hud_tenths(c, PER(ic_stall) / 20000));
    } else {
        snprintf(hud_lines[8], HUD_COLS, "C%d %s M%d RI%s", opt_set, on, gfx_diag, hud_tenths(c, PER(ic_stall) / 20000));
    }
    // DL is the whole list; Q is the part of it OPT_DEFER's second pass took.
    snprintf(hud_lines[9], HUD_COLS, "DL%s TX%s VX%s TRI%s Q%s", hud_tenths(a, TICKS10(dl)),
             hud_tenths(b, TICKS10(cur.ticks[DR_TEX] - prev.ticks[DR_TEX])),
             hud_tenths(c, TICKS10(cur.ticks[DR_VTX] - prev.ticks[DR_VTX])),
             hud_tenths(d, TICKS10(cur.ticks[DR_TRI] - prev.ticks[DR_TRI])),
             hud_tenths(e, TICKS10(cur.ticks[DR_DEFER] - prev.ticks[DR_DEFER])));
    snprintf(hud_lines[10], HUD_COLS, "TL%u CS%u DE%u UP%u LK%u", (unsigned)PER(loads), (unsigned)KB(sum_bytes),
             (unsigned)PER(decodes), (unsigned)KB(upload_bytes), (unsigned)PER(lookups));
    snprintf(hud_lines[11], HUD_COLS, "TA%u DM%u ND%u NC%u CM%u", (unsigned)KB(ta_bytes), (unsigned)KB(dma_bytes),
             (unsigned)PER(drawn), (unsigned)(PER(tri_cmds) - PER(drawn)), (unsigned)PER(cmds));
#undef PER
#undef TICKS10
#undef KB
    prev = cur;
}
#endif

static void hud_refresh(uint32_t tris_op, uint32_t tris_pt, uint32_t tris_tr, uint32_t vtx_bytes) {
    static uint64_t last_us;
    static uint32_t built;
    static HudCpu prev;
    static int have_prev;
    static uint32_t rate_drawn, rate_tasks;
    static uint64_t rate_t0;
    static uint32_t fps10, task10;
    uint64_t now = timer_us_gettime64();
    built++;
    if (last_us != 0 && now - last_us < 500000) {
        return;
    }
    uint32_t dt = last_us == 0 ? 500000 : (uint32_t)(now - last_us);
    last_us = now;
    uint32_t tasks = gfx_take_frames();
    if (tasks == 0) {
        tasks = built;
    }
    rate_drawn += built;
    rate_tasks += tasks;
    if (rate_t0 == 0) {
        rate_t0 = now;
    }
    if (now - rate_t0 >= 2000000) {
        uint32_t rdt = (uint32_t)(now - rate_t0);
        fps10 = (uint32_t)((uint64_t)rate_drawn * 10000000ull / rdt);
        task10 = (uint32_t)((uint64_t)rate_tasks * 10000000ull / rdt);
        rate_drawn = 0;
        rate_tasks = 0;
        rate_t0 = now;
    }
    built = 0;
    pvr_stats_t st;
    memset(&st, 0, sizeof(st));
    pvr_get_stats(&st);
    HudCpu cur;
    memset(&cur, 0, sizeof(cur));
    thd_each(hud_cpu_cb, &cur);
    uint32_t audio_n = 0;
    uint32_t audio_us = audio_take_task_us(&audio_n);
    uint32_t idle = 0, oth = 0;
    uint32_t g_ms = 0, r_ms = 0, a_ms = 0;
    // 5 ns per cycle at 200 MHz; tenths of a millisecond per frame.
    uint32_t ic10 = (uint32_t)(sched_take_icache_stall() / 20000ull / tasks);
    if (have_prev) {
        uint64_t gns = cur.game - prev.game;
        uint64_t rns = cur.rsp - prev.rsp;
        uint64_t ans = cur.aud - prev.aud;
        uint64_t audio_ns = (uint64_t)audio_us * 1000ull;
        if (audio_ns > gns) {
            audio_ns = gns;
        }
        gns -= audio_ns;
        ans += audio_ns;
        idle = hud_pct(cur.idle - prev.idle, dt);
        oth = hud_pct(cur.other - prev.other, dt);
        g_ms = (uint32_t)(gns / 1000000ull / tasks);
        r_ms = (uint32_t)(rns / 1000000ull / tasks);
        a_ms = (uint32_t)(ans / 1000000ull / tasks);
    }
    prev = cur;
    have_prev = 1;
    uint32_t aus = audio_n ? audio_us / audio_n : 0;
    uint32_t path_lin = 0, path_dry = 0;
    uint32_t path_fast = audio_hle_take_paths(&path_lin, &path_dry);
    if (path_fast > 999) {
        path_fast = 999;
    }
    if (path_dry > 999) {
        path_dry = 999;
    }
    if (path_lin > 999) {
        path_lin = 999;
    }
    static const char* const audio_tag[] = { "EX", "FST", "LIN", "NRV" };
    static const char* const skip_tag[] = { "SK0", "SK1", "SKA" };
    snprintf(hud_lines[0], 22, "F%u.%u T%u.%u", (unsigned)(fps10 / 10), (unsigned)(fps10 % 10),
             (unsigned)(task10 / 10), (unsigned)(task10 % 10));
    snprintf(hud_lines[1], 22, "G%u IC%u.%u R%u A%u", (unsigned)g_ms, (unsigned)(ic10 / 10), (unsigned)(ic10 % 10),
             (unsigned)r_ms, (unsigned)a_ms);
    if (audio_aica()) {
        // Microseconds per audio frame in the AICA hook, voices sounding, the lead in ms, and
        // time line slips since boot.
        uint32_t von = 0, lead = 0, slips = 0;
        aica_hud(&von, &lead, &slips);
        snprintf(hud_lines[2], 22, "AIC %u V%u D%u S%u", (unsigned)(aus > 9999 ? 9999 : aus),
                 (unsigned)(von > 99 ? 99 : von), (unsigned)(lead > 999 ? 999 : lead),
                 (unsigned)(slips > 99 ? 99 : slips));
    } else {
        snprintf(hud_lines[2], 22, "%s %u F%u L%u D%u", audio_tag[audio_hle_mode() & 3], (unsigned)aus,
                 (unsigned)path_fast, (unsigned)path_lin, (unsigned)path_dry);
    }
    snprintf(hud_lines[3], 22, "%s I%u O%u", skip_tag[gfx_skip_mode() & 3], (unsigned)idle, (unsigned)oth);
    snprintf(hud_lines[4], 22, "RND %u REG %u", (unsigned)(st.rnd_last_time / 1000000),
             (unsigned)(st.reg_last_time / 1000000));
    snprintf(hud_lines[5], 22, "OP%u PT%u", (unsigned)tris_op, (unsigned)tris_pt);
    snprintf(hud_lines[6], 22, "TR%u V%u", (unsigned)tris_tr, (unsigned)(vtx_bytes / 1024));
    if (profile_mode() == 1) {
        snprintf(hud_lines[7], 22, "PROF %d", profile_secs());
    } else if (profile_mode() >= 2) {
        snprintf(hud_lines[7], 22, profile_mode() == 2 ? "FUNC" : profile_mode() == 3 ? "GROUP" : "PC");
    } else if (profile_table_bytes() != 0) {
        snprintf(hud_lines[7], 22, "SYM %uK", (unsigned)(profile_table_bytes() / 1024));
    } else {
        snprintf(hud_lines[7], 22, "X PROF");
    }
#ifdef GFX_DIAG
    diag_hud_lines();
#endif
    hud_ready = 1;
}

static void __attribute__((noinline)) draw_perf_hud(void) {
    profile_poll();
    hud_refresh(stat_list_tris[LIST_OP], stat_list_tris[LIST_PT], stat_list_tris[LIST_TR], vbuf_used);
    if (!hud_ready) {
        return;
    }
    int extra = profile_mode() >= 2 ? 15 : 0;
    if (list_used[LIST_TR] + 96 * 1024 > LIST_BUF_SIZE) {
        return;
    }
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.gen.alpha = PVR_ALPHA_ENABLE;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    pvr_poly_compile(&hdr, &cxt);
    out_list = LIST_TR;
    have_key[LIST_TR] = false;
    uint32_t* h = (uint32_t*)out_target();
    memcpy(h, &hdr, sizeof(hdr));
    out_commit(h);
#ifdef GFX_DIAG
    float box_h = (HUD_LINES + extra) * 12.0f + 8.0f;
    float y0 = 444.0f - box_h;
    hud_quad(8.0f, y0, (HUD_COLS - 1) * 8.0f + 12.0f, box_h, 0xC0000000);
    if (hud_font != NULL) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED, HUD_FONT_W, HUD_FONT_H,
                         hud_font, PVR_FILTER_NEAREST);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&hdr, &cxt);
        h = (uint32_t*)out_target();
        memcpy(h, &hdr, sizeof(hdr));
        out_commit(h);
    }
#else
    float y0 = extra ? 160.0f : 340.0f;
    hud_quad(8.0f, y0, 220.0f, (8 + extra) * 12.0f + 8.0f, 0xC0000000);
#endif
    for (int i = 0; i < HUD_LINES; i++) {
        hud_text(12.0f, y0 + 4.0f + i * 12.0f, hud_lines[i]);
    }
    for (int i = 0; i < extra; i++) {
        hud_text(12.0f, y0 + 4.0f + (HUD_LINES + i) * 12.0f, profile_row(i));
    }
}
#endif

#ifdef GFX_DIAG
// After the wait for the tile accelerator: takes a new mode and starts the frame's figures.
static void diag_frame_begin(uint32_t wait_us) {
    int want = diag_request;
    if (want != gfx_diag) {
        // Textures decoded without their upload, or from TMEM the loads never reached, are
        // decoded again.
        if (gfx_diag == DIAG_NO_UPLOAD || gfx_diag == DIAG_NO_TEXTURE) {
            gfx_diag_salt++;
        }
        gfx_diag = want;
        printf("[q64dc] gfx diag mode %d\n", want);
    }
    int set = opt_request;
    if (set != opt_set) {
        opt_set = set;
        gfx_opt = opt_sets[set];
        printf("[q64dc] gfx opt set %d (0x%03X)\n", set, (unsigned)gfx_opt);
    }
    dg.wait_us += wait_us;
    pmc_begin();
}

static void diag_dl_end(void) {
    dr_enter(DR_OTHER);
    dg.frames++;
    dg.drawn += stat_frame_tris;
    if (gfx_diag != DIAG_NO_TA) {
        dg.ta_bytes += vbuf_used - list_used[LIST_PT] - list_used[LIST_TR];
    }
}
#endif

#ifndef GFX_HASH
static uint32_t frames;
#endif
static uint32_t period_frames;
static uint64_t period_wait_us, period_build_us;

void gfx_run_task(uint32_t task) {
#ifdef SCREEN_LOG
    static int announced;
    if (!announced) {
        announced = 1;
        screen_mark("first gfx task");
    }
#endif
    ensure_pvr();
    PVR_SET(PVR_PT_ALPHA_REF, PT_ALPHA_REF);
    uint32_t dl = (uint32_t)MEM_W(0x30, task);
    if (dl < 0x80000000u) {
        dl |= 0x80000000u;
    }

    uint64_t t0 = timer_us_gettime64();
    pvr_wait_ready();
    uint64_t t1 = timer_us_gettime64();
#ifdef GFX_DIAG
    diag_frame_begin((uint32_t)(t1 - t0));
#endif
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_dr_init(&dr);
    for (int i = 0; i < LIST_COUNT; i++) {
        have_key[i] = false;
        list_used[i] = 0;
    }
    out_list = LIST_OP;
    vbuf_used = 0;
    stat_frame_tris = 0;
    stat_list_tris[LIST_OP] = stat_list_tris[LIST_PT] = stat_list_tris[LIST_TR] = 0;
    g.mv_top = 0;
    touch_mvp();
    g.mat_dirty = true;
    g.phase = PHASE_BACKGROUND;
    g.pvr_fog_set = false;
    // Look textures up again at least once a frame, which marks them as in use.
    g.tmem_gen++;
    tex_frame_begin();
    q_on = q.ok && GFX_OPT_ON(OPT_DEFER) && GFX_OPT_ON(OPT_TRI) && !DIAG_IS(DIAG_NO_UPLOAD) &&
           !DIAG_IS(DIAG_NO_TEXTURE) && !DIAG_IS(DIAG_NO_CHECKSUM) && !DIAG_IS(DIAG_NO_TA) && !DIAG_IS(DIAG_NO_DMA) &&
           !DIAG_IS(DIAG_NO_LIGHT) && !DIAG_IS(DIAG_NO_TRI);
    if (DEFER_POSSIBLE) {
        defer_reset();
    }
#ifdef GFX_HASH
    frame_hash = 2166136261u;
    hash_on = true;
#endif

#ifdef GFX_DIAG
    dr_start();
#endif
    run_dl(dl);
    if (DEFER_POSSIBLE && q_on) {
        defer_flush();
    }
#ifdef GFX_HASH
    hash_on = false;
    if (test_disc()) {
        printf("[q64dc] fhash %u %08X %u %u\n", (unsigned)frames, (unsigned)frame_hash, (unsigned)vbuf_used,
               (unsigned)stat_frame_tris);
    }
#endif
#ifdef GFX_DIAG
    dr_enter(DR_TEX);
#endif
    tex_frame_end();
#ifdef GFX_DIAG
    diag_dl_end();
#endif
    if (test_disc()) {
        draw_test_banner();
    }
#ifdef PERF_HUD
#ifdef GFX_DIAG
    diag_hud_from = list_used[LIST_TR];
#endif
    draw_perf_hud();
#endif

#ifdef GFX_DIAG
    uint64_t finish_t0 = timer_us_gettime64();
#endif
    pvr_list_finish();
#ifdef GFX_DIAG
    dg.submit_us += (uint32_t)(timer_us_gettime64() - finish_t0);
#endif
    stat_list_bytes[LIST_OP] = vbuf_used - list_used[LIST_PT] - list_used[LIST_TR];
    if (vbuf_used > stat_vbuf_peak) {
        stat_vbuf_peak = vbuf_used;
        if (vbuf_used > vbuf_peak_boot) {
            vbuf_peak_boot = vbuf_used;
        }
    }
    stat_tris = stat_frame_tris;
    frames++;
    period_frames++;
    period_wait_us += t1 - t0;
    period_build_us += timer_us_gettime64() - t1;
#ifdef GFX_DIAG
    pmc_end();
#endif
#ifdef GFX_COUNT
    for (uint32_t i = 0; i < 32; i++) {
        count_retire(i);
    }
    gfx_count.frames++;
    if (gfx_count.frames >= 30) {
        count_report = gfx_count;
        count_report_seq++;
        memset(&gfx_count, 0, sizeof(gfx_count));
    }
#endif
}

// The PowerVR starts rendering once its last list is in, and shows the frame at the first vblank
// after that render finishes.
//
// The TA takes lists in one order: opaque, then translucent, then punch-through (a modifier
// list with bin size 0 is skipped). KOS's vertex DMA walks them that way. Sending punch-through
// before translucent makes the TA bin those polygons with the opaque list, so a cutout is drawn
// as a solid card. Flycast bins by the header's list type and does not care about the order.
static void submit_ram_list(int list, int direct) {
    stat_list_bytes[list] = list_used[list];
#ifdef GFX_DIAG
    // DIAG_NO_DMA sends only the HUD, which ends the translucent list.
    uint32_t from = gfx_diag != DIAG_NO_DMA ? 0 : list == LIST_TR ? diag_hud_from : list_used[list];
    dg.dma_bytes += list_used[list] - from;
#else
    uint32_t from = 0;
#endif
    if (list_used[list] == from) {
        return;
    }
    pvr_list_begin(pvr_lists[list]);
    if (direct) {
        const uint8_t* src = list_buf[list];
        for (uint32_t off = from; off < list_used[list]; off += 32) {
            pvr_prim(src + off, 32);
        }
    } else {
        pvr_dma_load_ta(list_buf[list] + from, list_used[list] - from, true, NULL, NULL);
    }
    pvr_list_finish();
}

void gfx_submit(void) {
#ifdef GFX_DIAG
    uint64_t submit_t0 = timer_us_gettime64();
    pmc_begin();
#endif
    submit_ram_list(LIST_TR, 0);
    submit_ram_list(LIST_PT, 0);
    // Read when rendering; the previous frame may still be rendering, so a new fog color can reach
    // its last tiles.
    if (g.pvr_fog_set) {
        PVR_SET(PVR_FOG_VERTEX_COLOR, g.pvr_fog_color);
    }
    PVR_SET(PVR_PT_ALPHA_REF, PT_ALPHA_REF);
    pvr_scene_finish();
    pace_note_finish();
#ifdef GFX_DIAG
    pmc_end();
    dg.submit_us += (uint32_t)(timer_us_gettime64() - submit_t0);
#endif
}

void gfx_print_stats(void) {
    pvr_stats_t st;
    pvr_get_stats(&st);
    uint32_t n = period_frames ? period_frames : 1;
    printf("[q64dc] gfx: %u frames, %u tris last frame, %u headers/s, %u material updates/s, %u texture lookups/s, "
           "%u TMEM copies/s, %u TMEM hashes/s, textures %u cached / %u uploaded, PVR %u fps, per frame: build %u us, "
           "wait %u us, render %u us; VRAM free %u KB; last frame OP/PT/TR %u/%u/%u KB, vertex buffer peak %u KB "
           "(%u since boot, limit %u), %u display lists culled/s\n",
           (unsigned)frames, (unsigned)stat_tris, (unsigned)stat_headers, (unsigned)stat_mat_updates,
           (unsigned)stat_tex_lookups, (unsigned)stat_tmem_copies, (unsigned)stat_tex_fallbacks,
           (unsigned)stat_tex_entries, (unsigned)stat_tex_uploads, (unsigned)st.frame_rate,
           (unsigned)(period_build_us / n), (unsigned)(period_wait_us / n), (unsigned)(st.rnd_last_time / 1000),
           (unsigned)(tex_vram_free() / 1024), (unsigned)(stat_list_bytes[LIST_OP] / 1024),
           (unsigned)(stat_list_bytes[LIST_PT] / 1024), (unsigned)(stat_list_bytes[LIST_TR] / 1024),
           (unsigned)(stat_vbuf_peak / 1024), (unsigned)(vbuf_peak_boot / 1024), (unsigned)(VERTEX_BUF_LIMIT / 1024),
           (unsigned)stat_culled_dls);
    tex_print_stats();
    printf("[q64dc] gfx: %u repeated texture loads left out/s\n", (unsigned)stat_same_loads);
    stat_same_loads = 0;
#ifdef GFX_DIAG
    // The HUD's renderer lines, for test runs.
    printf("[q64dc] hud: %s | %s | %s | %s | %s | %s\n", hud_lines[1], hud_lines[8], hud_lines[9], hud_lines[10],
           hud_lines[11], hud_lines[0]);
#endif
#ifdef GFX_XCHECK
    {
        static const char* const names[XC_COUNT] = { "tex",  "sum",  "load", "vtx",  "light",   "out",   "tri",
                                                     "batch", "reject", "keep", "rect", "rows", "combine", "lights" };
        char line[512];
        int len = 0;
        for (int i = 0; i < XC_COUNT && len < (int)sizeof(line) - 40; i++) {
            len += snprintf(line + len, sizeof(line) - len, " %s %u/%u", names[i], (unsigned)gfx_xcheck_fails[i],
                            (unsigned)gfx_xcheck_runs[i]);
        }
        printf("[q64dc] xcheck (differences/checks since boot):%s; opt 0x%03X\n", line, (unsigned)gfx_opt);
    }
#endif
#ifdef GFX_COUNT
    static uint32_t count_printed;
    if (count_report_seq != count_printed) {
        count_printed = count_report_seq;
        const GfxCount* c = &count_report;
        uint32_t f = c->frames ? c->frames : 1;
#define PF(x) ((unsigned)((c->x + f / 2) / f))
        printf("[q64dc] count/frame: tris %u (offscreen %u backface %u near %u drawn %u); verts %u in %u loads (outside "
               "%u; unused %u, rejected only %u, drawn %u); batches outside a plane %u with %u verts, %u used drawn; "
               "cmds %u, dl calls %u, mtx %u, culldl %u/%u hit; dead calls/verts/tris %u/%u/%u %u/%u/%u %u/%u/%u "
               "%u/%u/%u\n",
               PF(tris), PF(tri_offscreen), PF(tri_backface), PF(tri_near), PF(tri_drawn), PF(vtx_verts), PF(vtx_cmds),
               PF(vtx_outside), PF(vtx_unused), PF(vtx_rejected), PF(vtx_drawn), PF(batch_out), PF(batch_out_verts),
               PF(batch_out_used), PF(cmds), PF(dl_calls), PF(mtx), PF(culldl), PF(culldl_hits), PF(dead_calls[0]),
               PF(dead_verts[0]), PF(dead_tris[0]), PF(dead_calls[1]), PF(dead_verts[1]), PF(dead_tris[1]),
               PF(dead_calls[2]), PF(dead_verts[2]), PF(dead_tris[2]), PF(dead_calls[3]), PF(dead_verts[3]),
               PF(dead_tris[3]));
        printf("[q64dc] count/frame: lit %u, out %u, comb %u (slow %u), cc prep %u, mat %u (lookup skipped %u), headers "
               "%u of %u; loads %u, sums %u + %u hits, %u bytes (%u from ROM pages); in %u frames: sums changed %u after "
               "DMA, %u in ROM pages, %u other, %u new; pending scan %u, copies %u (%u bytes), lookups %u (%u slow), "
               "decodes %u\n",
               PF(lit), PF(out), PF(comb), PF(cc_slow), PF(cc_prep), PF(mat_updates), PF(lookups_skipped), PF(headers),
               PF(header_calls), PF(loads), PF(sum_misses), PF(sum_hits), PF(sum_bytes), PF(sum_bytes_rom),
               (unsigned)c->frames, (unsigned)c->sum_changed_dma, (unsigned)c->sum_changed_rom,
               (unsigned)c->sum_changed_other, (unsigned)c->sum_new, PF(pend_scan), PF(copies), PF(copy_bytes),
               PF(lookups), PF(lookup_slow), PF(decodes));
        // The busiest commands.
        char line[256];
        int len = 0;
        uint8_t done[256] = { 0 };
        for (int k = 0; k < 16; k++) {
            int best = -1;
            for (int op = 0; op < 256; op++) {
                if (!done[op] && (best < 0 || c->op[op] > c->op[best])) {
                    best = op;
                }
            }
            done[best] = 1;
            len += snprintf(line + len, sizeof(line) - len, " %02X:%u", best, PF(op[best]));
        }
        printf("[q64dc] count/frame ops:%s\n", line);
#undef PF
    }
#endif
    stat_vbuf_peak = 0;
    stat_culled_dls = 0;
    stat_headers = 0;
    stat_mat_updates = 0;
    stat_tex_lookups = 0;
    stat_tmem_copies = 0;
    stat_tex_fallbacks = 0;
    period_frames = 0;
    period_wait_us = 0;
    period_build_us = 0;
}
