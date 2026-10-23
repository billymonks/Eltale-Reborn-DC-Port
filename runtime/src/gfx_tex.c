// RDP texture memory and the PowerVR texture cache.
//
// Loads are emulated into a 4 KB TMEM, including the swapped odd rows, so tiles read texels
// the way the RDP does. A texture is decoded from TMEM when a tile is drawn and cached in VRAM.
//
// Copying every load and hashing TMEM at every use cost a fifth of the CPU in battles, so:
// - A load is identified by its parameters and a hash of the RDRAM bytes it reads. The hash is
//   taken in full once per graphics task for each source, so data the CPU rewrites is seen the
//   next time it's drawn.
// - TMEM is described as segments, each holding part of one load's output, and a texture is
//   cached under its tile parameters and the segments of the bytes it reads.
// - Loads are queued and copied into TMEM only when a texture has to be decoded, and at the end
//   of the task. A queued load that a later one overwrites completely is dropped.
// Tiles that read TMEM bytes no load describes (past the rows of a load, palette banks read as
// texels, data left from before) fall back to hashing the TMEM bytes.

#include <stdio.h>
#include <string.h>

#include "gfx_internal.h"

GfxTile gfx_tiles[8];
GfxImage gfx_timg;

uint32_t stat_tex_uploads;
uint32_t stat_tex_entries;
uint32_t stat_tmem_copies;
uint32_t stat_tex_fallbacks;

// Stored as 32-bit words in host order, like RDRAM, so byte b of TMEM is at b ^ 3.
static uint32_t tmem32[1024];
#define TMEM8(a) (((uint8_t*)tmem32)[((a) & 0xFFF) ^ 3])

static inline uint32_t tmem16(uint32_t a) {
    return (TMEM8(a) << 8) | TMEM8(a + 1);
}

static inline bool rdram_range_ok(uint32_t addr, uint32_t len) {
    uint32_t off = addr - 0x80000000u;
    return off < RDRAM_MAPPED_SIZE && len <= RDRAM_MAPPED_SIZE - off;
}

static inline uint8_t rd8(uint32_t addr) {
    return *(uint8_t*)RDRAM_PTR(addr ^ 3);
}

static inline uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// --- Loads ---

enum { LOAD_BLOCK, LOAD_TILE, LOAD_TLUT };

// A load as the RDP was asked to do it, kept until it's copied into TMEM.
typedef struct {
    uint8_t kind, siz;
    bool dead;
    uint16_t tmem, line, width;
    uint16_t q0, nq;  // TMEM qwords it writes (to q0 + nq, wrapping at 512)
    uint32_t src;
    uint32_t p0, p1, p2, p3;  // block: qwords, dxt; tile: s0, t0, row bytes, rows; tlut: count
} TmemLoad;

static void copy_block(const TmemLoad* ld) {
    uint32_t src = ld->src, dxt = ld->p1;
    for (uint32_t j = 0; j < ld->p0; j++) {
        uint32_t q = (ld->tmem + j) & 511;
        bool odd = dxt != 0 && (((j * dxt) >> 11) & 1);
        uint32_t a = src + j * 8;
        uint32_t hi, lo;
        if ((a & 3) == 0) {
            hi = *(uint32_t*)RDRAM_PTR(a);
            lo = *(uint32_t*)RDRAM_PTR(a + 4);
        } else {
            hi = (rd8(a) << 24) | (rd8(a + 1) << 16) | (rd8(a + 2) << 8) | rd8(a + 3);
            lo = (rd8(a + 4) << 24) | (rd8(a + 5) << 16) | (rd8(a + 6) << 8) | rd8(a + 7);
        }
        tmem32[q * 2] = odd ? lo : hi;
        tmem32[q * 2 + 1] = odd ? hi : lo;
    }
}

static inline uint32_t tile_row_src(const TmemLoad* ld, uint32_t row) {
    return ld->src + (((((ld->p1 + row) * ld->width) + ld->p0) << ld->siz) >> 1);
}

static void copy_tile(const TmemLoad* ld) {
    uint32_t row_bytes = ld->p2;
    uint32_t stride = ld->line * 8;
    for (uint32_t row = 0; row < ld->p3; row++) {
        uint32_t src = tile_row_src(ld, row);
        uint32_t dst = ld->tmem * 8 + row * stride;
        uint32_t swap = (row & 1) ? 4 : 0;
        uint32_t i = 0;
        if (((src | dst) & 3) == 0) {
            const uint32_t* s = (const uint32_t*)RDRAM_PTR(src);
            for (; i + 4 <= row_bytes; i += 4) {
                tmem32[(((dst + i) ^ swap) >> 2) & 1023] = s[i >> 2];
            }
        }
        for (; i < row_bytes; i++) {
            TMEM8((dst + i) ^ swap) = rd8(src + i);
        }
    }
}

// The RDP stores each TLUT entry four times across the high TMEM banks; one copy per 8-byte
// slot is all the decoder reads.
static void copy_tlut(const TmemLoad* ld) {
    uint32_t dst = ld->tmem * 8;
    for (uint32_t i = 0; i < ld->p0; i++) {
        TMEM8(dst + i * 8) = rd8(ld->src + i * 2);
        TMEM8(dst + i * 8 + 1) = rd8(ld->src + i * 2 + 1);
    }
}

#define PENDING_MAX 64

static TmemLoad pending[PENDING_MAX];
static uint32_t pending_first, pending_count;

static void copy_load(const TmemLoad* ld) {
    if (ld->dead) {
        return;
    }
    if (ld->kind == LOAD_BLOCK) {
        copy_block(ld);
    } else if (ld->kind == LOAD_TILE) {
        copy_tile(ld);
    } else {
        copy_tlut(ld);
    }
    stat_tmem_copies++;
}

// Brings TMEM up to date with the queued loads.
static void pending_flush(void) {
    for (uint32_t i = 0; i < pending_count; i++) {
        copy_load(&pending[(pending_first + i) % PENDING_MAX]);
    }
    pending_first = 0;
    pending_count = 0;
}

// Queues a load. `overwrites` says it writes all of its qwords; then any queued load inside
// its range is dropped. A palette load drops earlier palette loads inside its range, which
// wrote the same bytes of each qword.
static void pending_add(const TmemLoad* ld, bool overwrites) {
    if (overwrites || ld->kind == LOAD_TLUT) {
        for (uint32_t i = 0; i < pending_count; i++) {
            TmemLoad* p = &pending[(pending_first + i) % PENDING_MAX];
            bool inside = ld->nq >= 512 || (((p->q0 - ld->q0) & 511) + p->nq <= ld->nq);
            if (inside && (overwrites || p->kind == LOAD_TLUT)) {
                p->dead = true;
            }
        }
    }
    if (pending_count == PENDING_MAX) {
        copy_load(&pending[pending_first]);
        pending_first = (pending_first + 1) % PENDING_MAX;
        pending_count--;
    }
    pending[(pending_first + pending_count) % PENDING_MAX] = *ld;
    pending_count++;
}

// --- Source hashes ---

// Fletcher-style sums of the RDRAM words a load reads, kept for the rest of the graphics task
// so a texture loaded many times in a frame is read once.
typedef struct {
    uint32_t k0, k1, k2, k3;
    uint32_t gen;
    uint32_t a, b;
} SrcHash;

#define SRC_HASHES 512

static SrcHash src_hashes[SRC_HASHES];
static uint32_t src_gen = 1;

// Sums the aligned words around [addr, addr + len): a few bytes on either side may count too,
// which can only make a texture look changed when it isn't.
static void sum_bytes(uint32_t addr, uint32_t len, uint32_t* pa, uint32_t* pb) {
    uint32_t a = *pa, b = *pb;
    uint32_t start = addr & ~3u;
    const uint32_t* p = (const uint32_t*)RDRAM_PTR(start);
    uint32_t n = (addr + len + 3 - start) >> 2;
    for (; n >= 4; n -= 4, p += 4) {
        a += p[0];
        b += a;
        a += p[1];
        b += a;
        a += p[2];
        b += a;
        a += p[3];
        b += a;
    }
    for (; n != 0; n--) {
        a += *p++;
        b += a;
    }
    *pa = a;
    *pb = b;
}

static void load_sums(const TmemLoad* ld, uint32_t* pa, uint32_t* pb) {
    uint32_t k0 = ld->src, k1 = ld->kind | (ld->siz << 4) | ((uint32_t)ld->width << 8);
    uint32_t k2 = ld->p0 | (ld->p1 << 16), k3 = ld->p2 | (ld->p3 << 16);
    uint32_t slot = mix32(k0 ^ mix32(k1 ^ k2 * 31u ^ k3 * 977u)) & (SRC_HASHES - 1);
    uint32_t victim = slot;
    for (uint32_t i = 0; i < 4; i++) {
        SrcHash* s = &src_hashes[(slot + i) & (SRC_HASHES - 1)];
        if (s->gen == src_gen) {
            if (s->k0 == k0 && s->k1 == k1 && s->k2 == k2 && s->k3 == k3) {
                *pa = s->a;
                *pb = s->b;
                return;
            }
        } else {
            victim = (slot + i) & (SRC_HASHES - 1);
        }
    }

    uint32_t a = 1, b = 0;
    if (ld->kind == LOAD_BLOCK) {
        sum_bytes(ld->src, ld->p0 * 8, &a, &b);
    } else if (ld->kind == LOAD_TILE) {
        for (uint32_t row = 0; row < ld->p3; row++) {
            sum_bytes(tile_row_src(ld, row), ld->p2, &a, &b);
        }
    } else {
        sum_bytes(ld->src, ld->p0 * 2, &a, &b);
    }
    SrcHash* s = &src_hashes[victim];
    *s = (SrcHash){ k0, k1, k2, k3, src_gen, a, b };
    *pa = a;
    *pb = b;
}

// --- TMEM segments ---

enum { SEG_DATA, SEG_TLUT };

// Qwords [start, end) of TMEM hold qwords rel.. of a load's output (TLUT loads write only the
// first two bytes of each qword). A partial segment is a tile load that leaves bytes between its
// rows alone: it only describes the first row_bytes of each of its rows qword lines.
typedef struct {
    uint16_t start, end, rel;
    uint8_t kind;
    bool partial;
    uint16_t line, row_bytes, rows, span;
    uint32_t k1, k2;
} Segment;

// The TMEM bytes a texture reads: row_bytes at the start of each of rows lines.
typedef struct {
    uint32_t line, row_bytes, rows;
} ReadShape;

#define MAX_SEGMENTS 48

static Segment segments[MAX_SEGMENTS];
static int segment_count;

// Forgets what qwords [s, e) hold.
static void segments_cut(uint32_t s, uint32_t e) {
    for (int i = 0; i < segment_count;) {
        Segment* g = &segments[i];
        if (g->end <= s || g->start >= e) {
            i++;
        } else if (g->start >= s && g->end <= e) {
            segments[i] = segments[--segment_count];
        } else {
            if (g->start < s && g->end > e && segment_count < MAX_SEGMENTS) {
                Segment tail = *g;
                tail.rel += e - g->start;
                tail.start = e;
                segments[segment_count++] = tail;
            }
            if (g->start < s) {
                g->end = s;
            } else {
                g->rel += e - g->start;
                g->start = e;
            }
            i++;
        }
    }
}

// Records that qwords q0.. (wrapping) hold a load's output, or, without a key, that they're
// unknown.
static void segments_write(uint32_t q0, uint32_t nq, bool known, uint8_t kind, uint32_t k1, uint32_t k2,
                           const TmemLoad* partial) {
    if (partial != NULL) {
        segments_cut(q0, q0 + nq);
        if (known && segment_count < MAX_SEGMENTS) {
            segments[segment_count++] = (Segment){ q0, q0 + nq, 0, kind, true, partial->line, partial->p2,
                                                   partial->p3, nq, k1, k2 };
        }
        return;
    }
    if (nq >= 512) {
        segment_count = 0;
        q0 = 0;
        nq = 512;
    }
    uint32_t parts[2][3] = { { q0, q0 + nq < 512 ? q0 + nq : 512, 0 }, { 0, q0 + nq > 512 ? q0 + nq - 512 : 0, 512 - q0 } };
    for (int p = 0; p < 2; p++) {
        uint32_t s = parts[p][0], e = parts[p][1];
        if (e <= s) {
            continue;
        }
        segments_cut(s, e);
        if (known && segment_count < MAX_SEGMENTS) {
            segments[segment_count++] = (Segment){ s, e, parts[p][2], kind, false, 0, 0, 0, 0, k1, k2 };
        }
    }
}

// Adds to h1, h2 a key for qwords q0.. (wrapping) built from the segments over them. Fails if
// any qword isn't covered, texel data would come from a palette load, or the reads (texels with
// shape rs, or palette entries without) could reach bytes a partial segment doesn't describe.
static bool segments_key(uint32_t q0, uint32_t nq, const ReadShape* rs, uint32_t* h1, uint32_t* h2) {
    bool texels = rs != NULL;
    uint32_t covered = 0;
    uint32_t parts[2][3] = { { q0, q0 + nq < 512 ? q0 + nq : 512, 0 }, { 0, q0 + nq > 512 ? q0 + nq - 512 : 0, 512 - q0 } };
    for (int p = 0; p < 2; p++) {
        uint32_t s = parts[p][0], e = parts[p][1];
        if (e <= s) {
            continue;
        }
        for (int i = 0; i < segment_count; i++) {
            const Segment* g = &segments[i];
            if (g->end <= s || g->start >= e) {
                continue;
            }
            if (texels && g->kind == SEG_TLUT) {
                return false;
            }
            if (g->partial && (!texels || g->rel != 0 || g->start != q0 || g->end - g->start != g->span ||
                               rs->line != g->line || rs->row_bytes > g->row_bytes || rs->rows > g->rows)) {
                return false;
            }
            uint32_t os = g->start > s ? g->start : s;
            uint32_t oe = g->end < e ? g->end : e;
            uint32_t x = mix32(g->k1 ^ mix32((g->rel + os - g->start) | ((parts[p][2] + os - s) << 10) | ((oe - os) << 20)));
            *h1 += x;
            *h2 += mix32(g->k2 + x);
            covered += oe - os;
        }
    }
    return covered == nq;
}

static void load_key(const TmemLoad* ld, uint32_t* k1, uint32_t* k2) {
    uint32_t a, b;
    load_sums(ld, &a, &b);
    uint32_t p = mix32(ld->kind | (ld->line << 4) | (ld->p0 << 20)) ^ mix32(ld->p1 + ld->p2 * 7919u + ld->p3 * 104729u);
    *k1 = mix32(a ^ p);
    *k2 = mix32(b + mix32(p));
}

void tmem_load_block(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    const GfxTile* t = &gfx_tiles[tile];
    uint32_t siz = gfx_timg.siz;
    uint32_t texels = lrs - uls + 1;
    uint32_t bytes = ((texels << siz) + 1) >> 1;
    uint32_t src = gfx_timg.addr + ((((ult * gfx_timg.width) + uls) << siz) >> 1);
    uint32_t qwords = (bytes + 7) >> 3;
    if (qwords > 512) {
        qwords = 512;
    }
    if (!rdram_range_ok(src, qwords * 8)) {
        return;
    }
    TmemLoad ld = { .kind = LOAD_BLOCK, .tmem = t->tmem, .q0 = t->tmem, .nq = qwords, .src = src, .p0 = qwords, .p1 = dxt };
    uint32_t k1, k2;
    load_key(&ld, &k1, &k2);
    pending_add(&ld, true);
    segments_write(ld.q0, ld.nq, true, SEG_DATA, k1, k2, NULL);
}

void tmem_load_tile(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    const GfxTile* t = &gfx_tiles[tile];
    uint32_t s0 = uls >> 2, t0 = ult >> 2, s1 = lrs >> 2, t1 = lrt >> 2;
    if (s1 < s0 || t1 < t0) {
        return;
    }
    TmemLoad ld = { .kind = LOAD_TILE, .siz = gfx_timg.siz, .tmem = t->tmem, .line = t->line, .width = gfx_timg.width,
                    .q0 = t->tmem, .src = gfx_timg.addr, .p0 = s0, .p1 = t0 };
    ld.p2 = (((s1 - s0 + 1) << ld.siz) + 1) >> 1;
    // Rows stop at the first one outside RDRAM, as the copy would.
    uint32_t rows = 0;
    while (rows <= t1 - t0 && rdram_range_ok(tile_row_src(&ld, rows), ld.p2)) {
        rows++;
    }
    if (rows == 0) {
        return;
    }
    ld.p3 = rows;
    uint32_t span = (rows - 1) * ld.line + ((ld.p2 + 7) >> 3);
    ld.nq = span > 512 ? 512 : span;
    // Whole rows of whole qwords, without overlap or wrapping onto itself, leave TMEM fully
    // described by the load. Other loads that don't wrap describe the start of each row (images
    // whose rows aren't a whole number of qwords, or narrower than the line).
    bool whole = (ld.p2 & 7) == 0 && ld.p2 == ld.line * 8u && rows == t1 - t0 + 1 && span <= 512;
    bool partial = !whole && ld.q0 + span <= 512;
    uint32_t k1 = 0, k2 = 0;
    if (whole || partial) {
        load_key(&ld, &k1, &k2);
    }
    pending_add(&ld, whole);
    segments_write(ld.q0, ld.nq, whole || partial, SEG_DATA, k1, k2, partial ? &ld : NULL);
}

void tmem_load_tlut(int tile, uint32_t count) {
    const GfxTile* t = &gfx_tiles[tile];
    uint32_t src = gfx_timg.addr;
    if (!rdram_range_ok(src, count * 2)) {
        return;
    }
    TmemLoad ld = { .kind = LOAD_TLUT, .tmem = t->tmem, .q0 = t->tmem, .nq = count > 512 ? 512 : count, .src = src, .p0 = count };
    uint32_t k1, k2;
    load_key(&ld, &k1, &k2);
    pending_add(&ld, false);
    segments_write(ld.q0, ld.nq, true, SEG_TLUT, k1, k2, NULL);
}

// Texel as RGBA8888.
static uint32_t tlut_entry(uint32_t index, int tlut) {
    uint32_t c = tmem16(0x800 + index * 8);
    if (tlut == G_TT_IA16) {
        uint32_t i = c >> 8, a = c & 0xFF;
        return (i << 24) | (i << 16) | (i << 8) | a;
    }
    uint32_t r = (c >> 11) & 0x1F, g = (c >> 6) & 0x1F, b = (c >> 1) & 0x1F;
    return (((r << 3) | (r >> 2)) << 24) | (((g << 3) | (g >> 2)) << 16) | (((b << 3) | (b >> 2)) << 8) |
           ((c & 1) ? 0xFF : 0);
}

static inline uint32_t gray(uint32_t i, uint32_t a) {
    return (i << 24) | (i << 16) | (i << 8) | a;
}

static uint32_t fetch(const GfxTile* t, uint32_t s, uint32_t tt, int tlut) {
    uint32_t row = t->tmem * 8 + tt * t->line * 8;
    uint32_t swap = (tt & 1) ? 4 : 0;
    switch (t->siz) {
        case G_IM_SIZ_4b: {
            uint32_t b = TMEM8((row + (s >> 1)) ^ swap);
            uint32_t v = (s & 1) ? (b & 0xF) : (b >> 4);
            if (tlut != G_TT_NONE || t->fmt == G_IM_FMT_CI) {
                return tlut_entry(t->palette * 16 + v, tlut);
            }
            if (t->fmt == G_IM_FMT_IA) {
                uint32_t i = (v >> 1) * 255 / 7;
                return gray(i, (v & 1) ? 0xFF : 0);
            }
            return gray(v * 17, v * 17);
        }
        case G_IM_SIZ_8b: {
            uint32_t b = TMEM8((row + s) ^ swap);
            if (tlut != G_TT_NONE || t->fmt == G_IM_FMT_CI) {
                return tlut_entry(b, tlut);
            }
            if (t->fmt == G_IM_FMT_IA) {
                return gray((b >> 4) * 17, (b & 0xF) * 17);
            }
            return gray(b, b);
        }
        case G_IM_SIZ_16b: {
            uint32_t c = tmem16((row + s * 2) ^ swap);
            if (t->fmt == G_IM_FMT_IA) {
                return gray(c >> 8, c & 0xFF);
            }
            uint32_t r = (c >> 11) & 0x1F, g = (c >> 6) & 0x1F, b = (c >> 1) & 0x1F;
            return (((r << 3) | (r >> 2)) << 24) | (((g << 3) | (g >> 2)) << 16) |
                   (((b << 3) | (b >> 2)) << 8) | ((c & 1) ? 0xFF : 0);
        }
        default: {
            uint32_t a = (row + s * 4) ^ swap;
            return (TMEM8(a) << 24) | (TMEM8(a + 1) << 16) | (TMEM8(a + 2) << 8) | TMEM8(a + 3);
        }
    }
}

static inline uint16_t to_1555(uint32_t c) {
    return ((c & 0x80) ? 0x8000 : 0) | ((c >> 17) & 0x7C00) | ((c >> 14) & 0x03E0) | ((c >> 11) & 0x001F);
}

static inline uint16_t to_4444(uint32_t c) {
    return ((c & 0xF0) << 8) | ((c >> 20) & 0x0F00) | ((c >> 16) & 0x00F0) | ((c >> 12) & 0x000F);
}

static inline uint16_t to_565(uint32_t c) {
    return ((c >> 16) & 0xF800) | ((c >> 13) & 0x07E0) | ((c >> 11) & 0x001F);
}

// Twiddled (Morton) order of the PowerVR; a rectangle is a row of squares of the short side.
static uint32_t twiddle_bits[1024];

static inline uint32_t twiddle(uint32_t x, uint32_t y, uint32_t min) {
    uint32_t mask = min - 1;
    return (twiddle_bits[y & mask] | (twiddle_bits[x & mask] << 1)) + (x / min + y / min) * min * min;
}

#define TEX_BUCKETS     1024
#define TEX_MAX_ENTRIES 2048
#define MAX_TEXELS      (256 * 256)

typedef struct {
    uint32_t h1, h2;
    pvr_ptr_t ptr;
    uint32_t pvr_fmt;
    uint16_t pw, ph;
    uint32_t last_frame;
    int32_t next;
} TexEntry;

static TexEntry entries[TEX_MAX_ENTRIES];
static int32_t buckets[TEX_BUCKETS];
static int32_t free_list;
static uint32_t frame;
static uint16_t decode_buf[MAX_TEXELS];

void tex_init(void) {
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t v = 0;
        for (int b = 0; b < 10; b++) {
            v |= ((i >> b) & 1) << (2 * b);
        }
        twiddle_bits[i] = v;
    }
    for (int i = 0; i < TEX_BUCKETS; i++) {
        buckets[i] = -1;
    }
    for (int i = 0; i < TEX_MAX_ENTRIES; i++) {
        entries[i].next = i + 1 < TEX_MAX_ENTRIES ? i + 1 : -1;
    }
    free_list = 0;
}

void tex_frame_begin(void) {
    frame++;
    src_gen++;
}

void tex_frame_end(void) {
    pending_flush();
}
static void remove_entry(int32_t index) {
    TexEntry* e = &entries[index];
    int32_t* link = &buckets[e->h1 & (TEX_BUCKETS - 1)];
    while (*link != index) {
        link = &entries[*link].next;
    }
    *link = e->next;
    pvr_mem_free(e->ptr);
    e->next = free_list;
    free_list = index;
    stat_tex_entries--;
}

// Frees textures not used for `age` frames. The PowerVR may still be drawing the previous
// frame, so anything used in the last two frames stays.
static bool evict_older_than(uint32_t age) {
    bool freed = false;
    for (int b = 0; b < TEX_BUCKETS; b++) {
        int32_t i = buckets[b];
        while (i >= 0) {
            int32_t next = entries[i].next;
            if (frame - entries[i].last_frame > age) {
                remove_entry(i);
                freed = true;
            }
            i = next;
        }
    }
    return freed;
}

static pvr_ptr_t vram_alloc(uint32_t size) {
    static const uint32_t ages[] = { 600, 120, 30, 2 };
    pvr_ptr_t p = pvr_mem_malloc(size);
    for (size_t i = 0; p == NULL && i < sizeof(ages) / sizeof(ages[0]); i++) {
        if (evict_older_than(ages[i])) {
            p = pvr_mem_malloc(size);
        }
    }
    return p;
}

static inline uint32_t pow2_at_least(uint32_t v) {
    uint32_t p = 8;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

static uint32_t tile_extent(uint32_t lo, uint32_t hi, uint8_t mask, bool clamp) {
    uint32_t size = hi >= lo ? ((hi - lo) >> 2) + 1 : 1;
    if (mask != 0) {
        uint32_t m = 1u << (mask > 10 ? 10 : mask);
        if (!clamp || m < size) {
            size = m;
        }
    }
    return size > 1024 ? 1024 : size;
}

bool tex_lookup(int tile, int tlut, int variant, GfxTexture* out) {
    const GfxTile* t = &gfx_tiles[tile];
    bool clamp_s = (t->cms & 2) || t->masks == 0;
    bool clamp_t = (t->cmt & 2) || t->maskt == 0;
    uint32_t w = tile_extent(t->uls, t->lrs, t->masks, clamp_s);
    uint32_t h = tile_extent(t->ult, t->lrt, t->maskt, clamp_t);
    uint32_t pw = pow2_at_least(w), ph = pow2_at_least(h);
    if (pw * ph > MAX_TEXELS) {
        return false;
    }
    bool paletted = t->siz <= G_IM_SIZ_8b && (tlut != G_TT_NONE || t->fmt == G_IM_FMT_CI);
    if (paletted && tlut == G_TT_NONE) {
        tlut = G_TT_RGBA16;
    }

    out->clamp_s = clamp_s;
    out->clamp_t = clamp_t;
    out->mirror_s = (t->cms & 1) && !clamp_s;
    out->mirror_t = (t->cmt & 1) && !clamp_t;
    out->pw = pw;
    out->ph = ph;

    // The key: the tile's parameters, and what its TMEM bytes and palette hold.
    uint32_t params[3] = {
        t->fmt | (t->siz << 3) | (tlut << 5) | (variant << 7) | (t->cms << 9) | (t->cmt << 11) | (t->palette << 13),
        w | (h << 16),
        t->line | (t->tmem << 16),
    };
    uint32_t row_bytes = t->line * 8;
    uint32_t bytes = row_bytes * h;
    if (bytes > 4096 || bytes == 0) {
        bytes = 4096;
    }
    uint32_t first = t->siz == G_IM_SIZ_4b ? t->palette * 16u : 0;
    uint32_t count = t->siz == G_IM_SIZ_4b ? 16 : 256;

    uint32_t h1 = mix32(params[0] ^ 0x51ED270Bu) + mix32(params[1] ^ 0x2545F491u) + mix32(params[2]);
    uint32_t h2 = mix32(h1 ^ 0x9E3779B9u);
    // Only the bytes the decoder reads count: w texels of each of h lines.
    ReadShape rs = { t->line, ((w << t->siz) + 1) >> 1, h };
    uint32_t read_q = (h - 1) * t->line + ((rs.row_bytes + 7) >> 3);
    if (read_q > 512) {
        read_q = 512;
    }
    bool keyed = segments_key(t->tmem, read_q, &rs, &h1, &h2) &&
                 (!paletted || segments_key(0x100 + first, count, NULL, &h1, &h2));
    if (!keyed) {
        // Hash the TMEM bytes themselves.
        stat_tex_fallbacks++;
        pending_flush();
        h1 = 2166136261u;
        h2 = 0x9E3779B9u;
#define MIX(v) do { uint32_t v_ = (v); h1 = (h1 ^ v_) * 16777619u; h2 += v_; h2 = (h2 << 11) | (h2 >> 21); } while (0)
        MIX(params[0]);
        MIX(params[1]);
        MIX(params[2]);
        uint32_t base = (t->tmem * 8) >> 2;
        for (uint32_t i = 0; i < (bytes + 3) >> 2; i++) {
            MIX(tmem32[(base + i) & 1023]);
        }
        if (paletted) {
            for (uint32_t i = 0; i < count; i += 2) {
                MIX((tmem16(0x800 + (first + i) * 8) << 16) | tmem16(0x800 + (first + i + 1) * 8));
            }
        }
#undef MIX
    }

    for (int32_t i = buckets[h1 & (TEX_BUCKETS - 1)]; i >= 0; i = entries[i].next) {
        TexEntry* e = &entries[i];
        if (e->h1 == h1 && e->h2 == h2 && e->pw == pw && e->ph == ph) {
            e->last_frame = frame;
            out->ptr = e->ptr;
            out->pvr_fmt = e->pvr_fmt;
            return true;
        }
    }

    // Decode.
    pending_flush();
    uint32_t pvr_fmt;
    if (variant == TEXVAR_NOALPHA) {
        pvr_fmt = PVR_TXRFMT_RGB565;
    } else if (variant == TEXVAR_ALPHAONLY) {
        pvr_fmt = PVR_TXRFMT_ARGB4444;
    } else if ((t->siz == G_IM_SIZ_16b && t->fmt == G_IM_FMT_RGBA) || (paletted && tlut == G_TT_RGBA16)) {
        pvr_fmt = PVR_TXRFMT_ARGB1555;
    } else {
        pvr_fmt = PVR_TXRFMT_ARGB4444;
    }

    uint32_t min = pw < ph ? pw : ph;
    for (uint32_t y = 0; y < ph; y++) {
        uint32_t sy = y < h ? y : (clamp_t ? h - 1 : y % h);
        for (uint32_t x = 0; x < pw; x++) {
            uint32_t sx = x < w ? x : (clamp_s ? w - 1 : x % w);
            uint32_t c = fetch(t, sx, sy, tlut);
            uint16_t p;
            if (variant == TEXVAR_NOALPHA) {
                p = to_565(c);
            } else if (variant == TEXVAR_ALPHAONLY) {
                p = 0x0FFF | ((c & 0xF0) << 8);
            } else if (pvr_fmt == PVR_TXRFMT_ARGB1555) {
                p = to_1555(c);
            } else {
                p = to_4444(c);
            }
            decode_buf[twiddle(x, y, min)] = p;
        }
    }

    uint32_t size = pw * ph * 2;
    pvr_ptr_t ptr = vram_alloc(size);
    if (ptr == NULL || free_list < 0) {
        static bool warned;
        if (!warned) {
            printf("[q64dc] gfx: out of texture memory\n");
            warned = true;
        }
        if (ptr != NULL) {
            pvr_mem_free(ptr);
        }
        return false;
    }
    // Plain stores: the store queues belong to the open display list.
    volatile uint32_t* dst = (volatile uint32_t*)ptr;
    const uint32_t* src = (const uint32_t*)decode_buf;
    for (uint32_t i = 0; i < size / 4; i++) {
        dst[i] = src[i];
    }

    int32_t index = free_list;
    TexEntry* e = &entries[index];
    free_list = e->next;
    e->h1 = h1;
    e->h2 = h2;
    e->ptr = ptr;
    e->pvr_fmt = pvr_fmt;
    e->pw = pw;
    e->ph = ph;
    e->last_frame = frame;
    e->next = buckets[h1 & (TEX_BUCKETS - 1)];
    buckets[h1 & (TEX_BUCKETS - 1)] = index;
    stat_tex_uploads++;
    stat_tex_entries++;

    out->ptr = ptr;
    out->pvr_fmt = pvr_fmt;
    return true;
}
