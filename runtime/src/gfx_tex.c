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
uint32_t stat_same_loads;

#ifdef GFX_DIAG
GfxDiagTex gfx_diag_tex;
uint32_t gfx_diag_salt;
#endif

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
    GCOUNT(copies, 1);
    GCOUNT(copy_bytes, ld->kind == LOAD_BLOCK ? ld->p0 * 8 : (ld->kind == LOAD_TILE ? ld->p2 * ld->p3 : ld->p0 * 2));
    if (ld->kind == LOAD_BLOCK) {
        copy_block(ld);
    } else if (ld->kind == LOAD_TILE) {
        copy_tile(ld);
    } else {
        copy_tlut(ld);
    }
    stat_tmem_copies++;
}

#ifdef GFX_XCHECK
static void shadow_flush(void);
static void shadow_compare(void);
#endif

// Brings TMEM up to date with the queued loads.
static void pending_flush(void) {
#ifdef GFX_XCHECK
    shadow_flush();
#endif
    for (uint32_t i = 0; i < pending_count; i++) {
        copy_load(&pending[(pending_first + i) % PENDING_MAX]);
    }
    pending_first = 0;
    pending_count = 0;
#ifdef GFX_XCHECK
    shadow_compare();
#endif
}

#ifdef GFX_XCHECK
uint32_t gfx_xcheck_runs[XC_COUNT], gfx_xcheck_fails[XC_COUNT];

// The queue as the original pending_add keeps it, copying into a TMEM of its own, compared with
// the real TMEM after each flush.
static TmemLoad shadow[PENDING_MAX];
static uint32_t shadow_first, shadow_count;
static uint32_t shadow_tmem[1024], real_tmem[1024];

// copy_load into the shadow TMEM.
static void shadow_copy(const TmemLoad* ld) {
    memcpy(real_tmem, tmem32, sizeof(tmem32));
    memcpy(tmem32, shadow_tmem, sizeof(tmem32));
    copy_load(ld);
    memcpy(shadow_tmem, tmem32, sizeof(tmem32));
    memcpy(tmem32, real_tmem, sizeof(tmem32));
}

static void shadow_add(const TmemLoad* ld, bool overwrites) {
    if (overwrites || ld->kind == LOAD_TLUT) {
        for (uint32_t i = 0; i < shadow_count; i++) {
            TmemLoad* p = &shadow[(shadow_first + i) % PENDING_MAX];
            bool inside = ld->nq >= 512 || (((p->q0 - ld->q0) & 511) + p->nq <= ld->nq);
            if (inside && (overwrites || p->kind == LOAD_TLUT)) {
                p->dead = true;
            }
        }
    }
    if (shadow_count == PENDING_MAX) {
        shadow_copy(&shadow[shadow_first]);
        shadow_first = (shadow_first + 1) % PENDING_MAX;
        shadow_count--;
    }
    shadow[(shadow_first + shadow_count) % PENDING_MAX] = *ld;
    shadow_count++;
}

static void shadow_flush(void) {
    for (uint32_t i = 0; i < shadow_count; i++) {
        shadow_copy(&shadow[(shadow_first + i) % PENDING_MAX]);
    }
    shadow_first = 0;
    shadow_count = 0;
}

static void shadow_compare(void) {
    gfx_xcheck_runs[XC_LOAD]++;
    if (memcmp(tmem32, shadow_tmem, sizeof(tmem32)) != 0 && gfx_xcheck_fails[XC_LOAD]++ < 8) {
        uint32_t at = 0;
        while (tmem32[at] == shadow_tmem[at]) {
            at++;
        }
        printf("[q64dc] xcheck tmem: word %u is %08X, the original's queue gives %08X\n", (unsigned)at,
               (unsigned)tmem32[at], (unsigned)shadow_tmem[at]);
    }
}
#endif

// pending_add, taking the loads the new one hides out of the queue at once rather than marking
// them: the queue then only holds loads that still decide some TMEM bytes, which are few, and
// they are copied in the same order as before.
static void pending_add_compact(const TmemLoad* ld, bool overwrites) {
    GCOUNT(pend_scan, pending_count);
    uint32_t keep = 0;
    for (uint32_t i = 0; i < pending_count; i++) {
        const TmemLoad* p = &pending[(pending_first + i) % PENDING_MAX];
        bool inside = ld->nq >= 512 || (((p->q0 - ld->q0) & 511) + p->nq <= ld->nq);
        if (p->dead || (inside && (overwrites || (ld->kind == LOAD_TLUT && p->kind == LOAD_TLUT)))) {
            continue;
        }
        if (keep != i) {
            pending[(pending_first + keep) % PENDING_MAX] = *p;
        }
        keep++;
    }
    pending_count = keep;
    if (pending_count == PENDING_MAX) {
        copy_load(&pending[pending_first]);
        pending_first = (pending_first + 1) % PENDING_MAX;
        pending_count--;
    }
    pending[(pending_first + pending_count) % PENDING_MAX] = *ld;
    pending_count++;
}

// Queues a load. `overwrites` says it writes all of its qwords; then any queued load inside
// its range is dropped. A palette load drops earlier palette loads inside its range, which
// wrote the same bytes of each qword.
static void pending_add(const TmemLoad* ld, bool overwrites) {
#ifdef GFX_DIAG
    // Nothing is decoded from TMEM then.
    if (gfx_diag == DIAG_NO_TEXTURE) {
        return;
    }
#endif
#ifdef GFX_XCHECK
    shadow_add(ld, overwrites);
#endif
    if (GFX_OPT_ON(OPT_LOAD)) {
        pending_add_compact(ld, overwrites);
        return;
    }
    if (pending_count == 0) {
        pending[0] = *ld;
        pending_first = 0;
        pending_count = 1;
        return;
    }
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
    // At the last full sum: the sampled lines' sum and the newest ROM DMA into the source's pages
    // (OPT_KEEP), and how many full sums in a row came out the same.
    uint32_t sig, seq;
    uint8_t stable;
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
#ifdef GFX_DIAG
    gfx_diag_tex.sum_bytes += n * 4;
#endif
    for (; n >= 4; n -= 4, p += 4) {
        __builtin_prefetch(p + 8);
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

// sum_bytes a 32-byte line at a time, with the next lines already asked for from RAM, so the
// loop waits less on each line fill. The words are added in the same order.
static void sum_bytes_lines(uint32_t addr, uint32_t len, uint32_t* pa, uint32_t* pb) {
    uint32_t a = *pa, b = *pb;
    uint32_t start = addr & ~3u;
    const uint32_t* p = (const uint32_t*)RDRAM_PTR(start);
    uint32_t n = (addr + len + 3 - start) >> 2;
#ifdef GFX_DIAG
    gfx_diag_tex.sum_bytes += n * 4;
#endif
    __builtin_prefetch(p + 8);
    __builtin_prefetch(p + 16);
    for (; n != 0 && ((uintptr_t)p & 31) != 0; n--) {
        a += *p++;
        b += a;
    }
    for (; n >= 8; n -= 8, p += 8) {
        __builtin_prefetch(p + 24);
        a += p[0];
        b += a;
        a += p[1];
        b += a;
        a += p[2];
        b += a;
        a += p[3];
        b += a;
        a += p[4];
        b += a;
        a += p[5];
        b += a;
        a += p[6];
        b += a;
        a += p[7];
        b += a;
    }
    for (; n != 0; n--) {
        a += *p++;
        b += a;
    }
    *pa = a;
    *pb = b;
}

static void load_sums_lines(const TmemLoad* ld, uint32_t* pa, uint32_t* pb) {
    if (ld->kind == LOAD_BLOCK) {
        sum_bytes_lines(ld->src, ld->p0 * 8, pa, pb);
    } else if (ld->kind == LOAD_TILE) {
        for (uint32_t row = 0; row < ld->p3; row++) {
            sum_bytes_lines(tile_row_src(ld, row), ld->p2, pa, pb);
        }
    } else {
        sum_bytes_lines(ld->src, ld->p0 * 2, pa, pb);
    }
}

// The bytes from a load's first source address to the end of the last one it reads.
static uint32_t load_span(const TmemLoad* ld) {
    if (ld->kind == LOAD_BLOCK) {
        return ld->p0 * 8;
    }
    if (ld->kind == LOAD_TILE) {
        return ld->p3 != 0 ? tile_row_src(ld, ld->p3 - 1) + ld->p2 - ld->src : 0;
    }
    return ld->p0 * 2;
}

// The newest ROM DMA into the pages of [addr, addr + bytes), and whether every page had one.
static uint32_t span_dma_seq(uint32_t addr, uint32_t bytes, bool* all) {
    uint32_t seq = 0, off = addr & 0x1FFFFFFFu;
    *all = bytes != 0;
    for (uint32_t p = off >> 12; bytes != 0 && p <= (off + bytes - 1) >> 12; p++) {
        uint32_t s = p < (RDRAM_SIZE >> 12) ? rom_page_seq[p] : 0;
        *all &= s != 0;
        seq = s > seq ? s : seq;
    }
    return seq;
}

// OPT_KEEP: a source of at least KEEP_MIN_BYTES whose last KEEP_STABLE full sums came out the
// same, and that no ROM DMA has written since, is taken as unchanged while every eighth 32-byte
// line (block) or row (tile), and the last, sum as they did; every KEEP_FULL frames it's summed
// in full. Palettes, which the CPU animates, are always summed in full.
#define KEEP_MIN_BYTES 1024
#define KEEP_STABLE    4
#define KEEP_STRIDE    8
#define KEEP_FULL      8

static uint32_t sample_sig(const TmemLoad* ld) {
    uint32_t a = 1, b = 0;
    if (ld->kind == LOAD_TILE) {
        for (uint32_t row = 0; row < ld->p3; row += KEEP_STRIDE) {
            sum_bytes_lines(tile_row_src(ld, row), ld->p2, &a, &b);
        }
        if (ld->p3 > 1 && (ld->p3 - 1) % KEEP_STRIDE != 0) {
            sum_bytes_lines(tile_row_src(ld, ld->p3 - 1), ld->p2, &a, &b);
        }
    } else {
        uint32_t len = load_span(ld);
        for (uint32_t off = 0; off < len; off += 32 * KEEP_STRIDE) {
            sum_bytes_lines(ld->src + off, len - off < 32 ? len - off : 32, &a, &b);
        }
        if (len > 32) {
            sum_bytes_lines(ld->src + len - 32, 32, &a, &b);
        }
    }
    return a ^ (b * 0x9E3779B1u);
}

static bool keep_eligible(const TmemLoad* ld, const SrcHash* old, uint32_t seq) {
    uint32_t bytes = ld->kind == LOAD_TILE ? ld->p2 * ld->p3 : (ld->kind == LOAD_BLOCK ? ld->p0 * 8 : 0);
    return bytes >= KEEP_MIN_BYTES && old->stable >= KEEP_STABLE && old->seq == seq &&
           ((src_gen + (old->k0 >> 5)) & (KEEP_FULL - 1)) != 0;
}

static void load_sums(const TmemLoad* ld, uint32_t* pa, uint32_t* pb) {
    uint32_t k0 = ld->src, k1 = ld->kind | (ld->siz << 4) | ((uint32_t)ld->width << 8);
    uint32_t k2 = ld->p0 | (ld->p1 << 16), k3 = ld->p2 | (ld->p3 << 16);
    uint32_t slot = mix32(k0 ^ mix32(k1 ^ k2 * 31u ^ k3 * 977u)) & (SRC_HASHES - 1);
    uint32_t victim = slot;
    bool keep = GFX_OPT_ON(OPT_KEEP);
#ifdef GFX_COUNT
    bool track = true;
#else
    bool track = keep;
#endif
    // The source's own sums from an earlier task, which it then replaces.
    SrcHash* old = NULL;
    for (uint32_t i = 0; i < 4; i++) {
        SrcHash* s = &src_hashes[(slot + i) & (SRC_HASHES - 1)];
        if (s->gen == src_gen) {
            if (s->k0 == k0 && s->k1 == k1 && s->k2 == k2 && s->k3 == k3) {
                GCOUNT(sum_hits, 1);
                *pa = s->a;
                *pb = s->b;
                return;
            }
        } else {
            victim = (slot + i) & (SRC_HASHES - 1);
            if (track && s->k0 == k0 && s->k1 == k1 && s->k2 == k2 && s->k3 == k3) {
                old = s;
            }
        }
    }
    if (old != NULL) {
        victim = (uint32_t)(old - src_hashes);
    }
    bool rom = false;
    uint32_t seq = track ? span_dma_seq(ld->src, load_span(ld), &rom) : 0;
#ifdef GFX_COUNT
    uint32_t read = ld->kind == LOAD_TILE ? ld->p2 * ld->p3 : load_span(ld);
    GCOUNT(sum_misses, 1);
    GCOUNT(sum_bytes, read);
    GCOUNT(sum_bytes_rom, rom ? read : 0);
#endif

    uint32_t a = 1, b = 0, sig = 0;
    bool kept = false;
    if (keep && old != NULL && ld->kind != LOAD_TLUT && keep_eligible(ld, old, seq)) {
        sig = sample_sig(ld);
        kept = sig == old->sig;
    }
    if (kept) {
        a = old->a;
        b = old->b;
#ifdef GFX_XCHECK
        uint32_t ra = 1, rb = 0;
        load_sums_lines(ld, &ra, &rb);
        gfx_xcheck_runs[XC_KEEP]++;
        if ((ra != a || rb != b) && gfx_xcheck_fails[XC_KEEP]++ < 8) {
            printf("[q64dc] xcheck keep: %08X+%u kind %u changed outside the sampled lines\n", (unsigned)ld->src,
                   (unsigned)load_span(ld), ld->kind);
        }
#endif
    } else if (GFX_OPT_ON(OPT_SUM)) {
        load_sums_lines(ld, &a, &b);
#ifdef GFX_XCHECK
        uint32_t ra = 1, rb = 0;
        if (ld->kind == LOAD_BLOCK) {
            sum_bytes(ld->src, ld->p0 * 8, &ra, &rb);
        } else if (ld->kind == LOAD_TILE) {
            for (uint32_t row = 0; row < ld->p3; row++) {
                sum_bytes(tile_row_src(ld, row), ld->p2, &ra, &rb);
            }
        } else {
            sum_bytes(ld->src, ld->p0 * 2, &ra, &rb);
        }
        gfx_xcheck_runs[XC_SUM]++;
        if ((ra != a || rb != b) && gfx_xcheck_fails[XC_SUM]++ < 8) {
            printf("[q64dc] xcheck sum: %08X+%u %08X/%08X, original %08X/%08X\n", (unsigned)ld->src, (unsigned)ld->kind,
                   (unsigned)a, (unsigned)b, (unsigned)ra, (unsigned)rb);
        }
#endif
    } else if (ld->kind == LOAD_BLOCK) {
        sum_bytes(ld->src, ld->p0 * 8, &a, &b);
    } else if (ld->kind == LOAD_TILE) {
        for (uint32_t row = 0; row < ld->p3; row++) {
            sum_bytes(tile_row_src(ld, row), ld->p2, &a, &b);
        }
    } else {
        sum_bytes(ld->src, ld->p0 * 2, &a, &b);
    }
    if (keep && !kept && ld->kind != LOAD_TLUT) {
        sig = sample_sig(ld);
    }
#ifdef GFX_COUNT
    if (old == NULL) {
        GCOUNT(sum_new, 1);
    } else if (old->a != a || old->b != b) {
        static uint32_t logged;
        const char* why = old->seq != seq ? "after a DMA" : (rom ? "in ROM pages" : "elsewhere");
        if (old->seq != seq) {
            GCOUNT(sum_changed_dma, 1);
        } else if (rom) {
            GCOUNT(sum_changed_rom, 1);
        } else {
            GCOUNT(sum_changed_other, 1);
        }
        if (logged++ < 200) {
            printf("[q64dc] count: source %08X+%u (kind %u, %ux%u) changed %s\n", (unsigned)ld->src,
                   (unsigned)load_span(ld), ld->kind, (unsigned)ld->p2, (unsigned)ld->p3, why);
        }
    }
#endif
    uint8_t stable = 0;
    if (kept) {
        stable = old->stable;
    } else if (old != NULL && old->a == a && old->b == b) {
        stable = old->stable < 255 ? old->stable + 1 : 255;
    }
    src_hashes[victim] = (SrcHash){ k0, k1, k2, k3, src_gen, a, b, sig, seq, stable };
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

// OPT_SAME: the last load and its key. A load with the same parameters and key writes TMEM with
// what the last one did, and nothing has written it since; segments and the queue already have it.
static TmemLoad same_ld;
static uint32_t same_k1, same_k2;
static bool same_ok;

static bool load_repeats(const TmemLoad* ld, bool keyed, uint32_t k1, uint32_t k2) {
#ifndef GFX_DIAG
    // GFX_DIAG builds keep the last load while OPT_SAME is off too, for when it is switched on.
    if (!GFX_OPT_ON(OPT_SAME)) {
        return false;
    }
#endif
    bool repeat = keyed && same_ok && same_k1 == k1 && same_k2 == k2 && ld->kind == same_ld.kind &&
                  ld->siz == same_ld.siz && ld->tmem == same_ld.tmem && ld->line == same_ld.line &&
                  ld->width == same_ld.width && ld->q0 == same_ld.q0 && ld->nq == same_ld.nq && ld->src == same_ld.src &&
                  ld->p0 == same_ld.p0 && ld->p1 == same_ld.p1 && ld->p2 == same_ld.p2 && ld->p3 == same_ld.p3;
    same_ld = *ld;
    same_k1 = k1;
    same_k2 = k2;
    same_ok = keyed;
    if (repeat && GFX_OPT_ON(OPT_SAME)) {
        stat_same_loads++;
        return true;
    }
    return false;
}

bool tmem_load_block(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    GCOUNT(loads, 1);
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
        return true;
    }
    TmemLoad ld = { .kind = LOAD_BLOCK, .tmem = t->tmem, .q0 = t->tmem, .nq = qwords, .src = src, .p0 = qwords, .p1 = dxt };
    uint32_t k1, k2;
    load_key(&ld, &k1, &k2);
    if (load_repeats(&ld, true, k1, k2)) {
        return false;
    }
    pending_add(&ld, true);
    segments_write(ld.q0, ld.nq, true, SEG_DATA, k1, k2, NULL);
    return true;
}

bool tmem_load_tile(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    GCOUNT(loads, 1);
    const GfxTile* t = &gfx_tiles[tile];
    uint32_t s0 = uls >> 2, t0 = ult >> 2, s1 = lrs >> 2, t1 = lrt >> 2;
    if (s1 < s0 || t1 < t0) {
        return true;
    }
    TmemLoad ld = { .kind = LOAD_TILE, .siz = gfx_timg.siz, .tmem = t->tmem, .line = t->line, .width = gfx_timg.width,
                    .q0 = t->tmem, .src = gfx_timg.addr, .p0 = s0, .p1 = t0 };
    ld.p2 = (((s1 - s0 + 1) << ld.siz) + 1) >> 1;
    // Rows stop at the first one outside RDRAM, as the copy would. Rows only move up through
    // RDRAM, so when the first and last are inside, all are.
    uint32_t rows = 0;
    if (GFX_OPT_ON(OPT_ROWS) && rdram_range_ok(tile_row_src(&ld, 0), ld.p2) &&
        rdram_range_ok(tile_row_src(&ld, t1 - t0), ld.p2)) {
        rows = t1 - t0 + 1;
    }
#ifdef GFX_XCHECK
    uint32_t fast_rows = rows;
    rows = 0;
#else
    if (rows == 0)
#endif
    {
        while (rows <= t1 - t0 && rdram_range_ok(tile_row_src(&ld, rows), ld.p2)) {
            rows++;
        }
    }
#ifdef GFX_XCHECK
    if (GFX_OPT_ON(OPT_ROWS)) {
        gfx_xcheck_runs[XC_ROWS]++;
        if (fast_rows != 0 && fast_rows != rows && gfx_xcheck_fails[XC_ROWS]++ < 8) {
            printf("[q64dc] xcheck rows: %u, the loop %u\n", (unsigned)fast_rows, (unsigned)rows);
        }
    }
#endif
    if (rows == 0) {
        return true;
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
    if (load_repeats(&ld, whole || partial, k1, k2)) {
        return false;
    }
    pending_add(&ld, whole);
    segments_write(ld.q0, ld.nq, whole || partial, SEG_DATA, k1, k2, partial ? &ld : NULL);
    return true;
}

bool tmem_load_tlut(int tile, uint32_t count) {
    GCOUNT(loads, 1);
    const GfxTile* t = &gfx_tiles[tile];
    uint32_t src = gfx_timg.addr;
    if (!rdram_range_ok(src, count * 2)) {
        return true;
    }
    TmemLoad ld = { .kind = LOAD_TLUT, .tmem = t->tmem, .q0 = t->tmem, .nq = count > 512 ? 512 : count, .src = src, .p0 = count };
    uint32_t k1, k2;
    load_key(&ld, &k1, &k2);
    if (load_repeats(&ld, true, k1, k2)) {
        return false;
    }
    pending_add(&ld, false);
    segments_write(ld.q0, ld.nq, true, SEG_TLUT, k1, k2, NULL);
    return true;
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

// --- Texture memory ---
//
// Textures are powers of two from 128 bytes (8x8) to 128 KB (256x256). The cache takes the
// PowerVR's texture memory from KallistiOS as one block at the first upload and splits it as a
// buddy system: a free block is whole or two free halves, and two free halves join again, so
// textures freed in any order leave blocks as large as the space allows. A failed
// pvr_mem_malloc prints "out of PVR memory", so KallistiOS's heap is not asked again.

#define VRAM_MIN_ORDER 7  // log2 of the smallest block, 8x8 texels
#define VRAM_ORDERS    11 // block sizes, up to 256x256 texels
#define VRAM_RESERVE   (16 * 1024)  // left to KallistiOS, whose heap rounds to 4 KB pages
#define VRAM_MAX       (8 * 1024 * 1024)

static uint8_t* vram_base;
static uint32_t vram_size, vram_used, vram_peak;
// A bit for each block of each size, set while the block is free.
static uint32_t vram_bits[(VRAM_MAX >> VRAM_MIN_ORDER) / 16];
static uint32_t* vram_free_bits[VRAM_ORDERS];
static uint32_t vram_free_count[VRAM_ORDERS];
static uint32_t vram_hint[VRAM_ORDERS];  // no free block in the words before this one

static inline uint32_t order_bytes(uint32_t k) {
    return 1u << (k + VRAM_MIN_ORDER);
}

static void vram_mark_free(uint32_t k, uint32_t block) {
    uint32_t w = block >> 5;
    vram_free_bits[k][w] |= 1u << (block & 31);
    vram_free_count[k]++;
    if (w < vram_hint[k]) {
        vram_hint[k] = w;
    }
}

static inline void vram_mark_used(uint32_t k, uint32_t block) {
    vram_free_bits[k][block >> 5] &= ~(1u << (block & 31));
    vram_free_count[k]--;
}

static inline bool vram_is_free(uint32_t k, uint32_t block) {
    return (vram_free_bits[k][block >> 5] >> (block & 31)) & 1;
}

static bool vram_init(void) {
    static bool tried;
    if (tried) {
        return vram_base != NULL;
    }
    tried = true;
    uint32_t avail = (uint32_t)pvr_mem_available();
    uint32_t size = avail > VRAM_RESERVE ? (avail - VRAM_RESERVE) & ~(order_bytes(0) - 1) : 0;
    if (size > VRAM_MAX) {
        size = VRAM_MAX;
    }
    while (size >= order_bytes(VRAM_ORDERS - 1) && (vram_base = pvr_mem_malloc(size)) == NULL) {
        size -= 64 * 1024;
    }
    if (vram_base == NULL) {
        return false;
    }
    vram_size = size;
    uint32_t* bits = vram_bits;
    for (uint32_t k = 0; k < VRAM_ORDERS; k++) {
        vram_free_bits[k] = bits;
        bits += ((size >> (k + VRAM_MIN_ORDER)) + 31) >> 5;
    }
    // The largest aligned blocks that fit, from the bottom.
    for (uint32_t off = 0; off + order_bytes(0) <= size;) {
        uint32_t k = VRAM_ORDERS - 1;
        while ((off & (order_bytes(k) - 1)) != 0 || off + order_bytes(k) > size) {
            k--;
        }
        vram_mark_free(k, off >> (k + VRAM_MIN_ORDER));
        off += order_bytes(k);
    }
    printf("[q64dc] gfx: %u KB of VRAM for textures\n", (unsigned)(size / 1024));
    return true;
}

// The lowest free block of order k, split from a larger one if need be: its offset, or -1.
static int32_t vram_take(uint32_t k) {
    uint32_t j = k;
    while (j < VRAM_ORDERS && vram_free_count[j] == 0) {
        j++;
    }
    if (j == VRAM_ORDERS) {
        return -1;
    }
    const uint32_t* bits = vram_free_bits[j];
    uint32_t w = vram_hint[j];
    while (bits[w] == 0) {
        w++;
    }
    vram_hint[j] = w;
    uint32_t block = (w << 5) + (uint32_t)__builtin_ctz(bits[w]);
    vram_mark_used(j, block);
    while (j > k) {
        j--;
        block <<= 1;
        vram_mark_free(j, block + 1);
    }
    vram_used += order_bytes(k);
    if (vram_used > vram_peak) {
        vram_peak = vram_used;
    }
    return (int32_t)(block << (k + VRAM_MIN_ORDER));
}

static void vram_give(uint32_t off, uint32_t k) {
    vram_used -= order_bytes(k);
    uint32_t block = off >> (k + VRAM_MIN_ORDER);
    for (; k + 1 < VRAM_ORDERS; k++) {
        uint32_t buddy = block ^ 1;
        if (((buddy + 1) << (k + VRAM_MIN_ORDER)) > vram_size || !vram_is_free(k, buddy)) {
            break;
        }
        vram_mark_used(k, buddy);
        block >>= 1;
    }
    vram_mark_free(k, block);
}

static uint32_t vram_largest_free(void) {
    for (int k = VRAM_ORDERS - 1; k >= 0; k--) {
        if (vram_free_count[k] != 0) {
            return order_bytes((uint32_t)k);
        }
    }
    return 0;
}

// --- The cache ---

typedef struct {
    uint32_t h1, h2;
    uint32_t vram;  // offset in the cache's block
    uint32_t pvr_fmt;
    uint16_t pw, ph;
    uint32_t last_frame;
    int32_t next;          // in a bucket, or the free list
    int16_t older, newer;  // in the order of last use
    uint8_t order;
} TexEntry;

static TexEntry entries[TEX_MAX_ENTRIES];
static int32_t buckets[TEX_BUCKETS];
static int32_t free_list;
static int32_t lru_oldest = -1, lru_newest = -1;
static uint32_t frame;
static uint32_t frame_bytes, last_frame_bytes;  // textures used by a frame
static uint32_t tex_count[VRAM_ORDERS];
static uint32_t stat_tex_failed;
static uint32_t period_uploads, period_upload_bytes, period_evictions, period_evicted_bytes, period_frame_peak;
// Word loads below. A uint16_t array is only 2-byte aligned, and the SH-4 faults a
// 32-bit load there. Flycast does not.
static uint16_t decode_buf[MAX_TEXELS] __attribute__((aligned(32)));

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
    last_frame_bytes = frame_bytes;
    if (frame_bytes > period_frame_peak) {
        period_frame_peak = frame_bytes;
    }
    frame_bytes = 0;
    frame++;
    same_ok = false;
#ifdef GFX_DIAG
    if (gfx_diag == DIAG_NO_CHECKSUM) {
        return;
    }
#endif
    src_gen++;
}

void tex_frame_end(void) {
    pending_flush();
}


// The PowerVR may still be drawing the previous frame, so anything used in the last two frames
// stays.
#define TEX_KEEP_FRAMES 2

static inline bool tex_busy(const TexEntry* e) {
    return frame - e->last_frame <= TEX_KEEP_FRAMES;
}

static void lru_unlink(int32_t index) {
    TexEntry* e = &entries[index];
    if (e->older >= 0) {
        entries[e->older].newer = e->newer;
    } else {
        lru_oldest = e->newer;
    }
    if (e->newer >= 0) {
        entries[e->newer].older = e->older;
    } else {
        lru_newest = e->older;
    }
}

// Entries are kept from least to most recently used, so the ones in use come last.
static void lru_push(int32_t index) {
    TexEntry* e = &entries[index];
    e->older = (int16_t)lru_newest;
    e->newer = -1;
    if (lru_newest >= 0) {
        entries[lru_newest].newer = (int16_t)index;
    } else {
        lru_oldest = index;
    }
    lru_newest = index;
}

static void remove_entry(int32_t index) {
    TexEntry* e = &entries[index];
    int32_t* link = &buckets[e->h1 & (TEX_BUCKETS - 1)];
    while (*link != index) {
        link = &entries[*link].next;
    }
    *link = e->next;
    lru_unlink(index);
    vram_give(e->vram, e->order);
    tex_count[e->order]--;
    period_evictions++;
    period_evicted_bytes += order_bytes(e->order);
    e->next = free_list;
    free_list = index;
    stat_tex_entries--;
}

// For a texture that no free block fits, when the least recently used texture is smaller:
// frees the aligned span of the texture's size whose newest texture is the oldest, if none of
// its textures is in use. A span lies inside one texture at least its size, or holds only
// smaller textures and free blocks, which join once those go. Below 4 KB, freeing the oldest
// textures one at a time soon opens a block, and the spans would be too many to count.
#define SPAN_MIN_ORDER 5  // 4 KB
static uint32_t span_newest[VRAM_MAX >> (SPAN_MIN_ORDER + VRAM_MIN_ORDER)];

static bool evict_span(uint32_t k) {
    uint32_t shift = k + VRAM_MIN_ORDER;
    uint32_t spans = vram_size >> shift;
    memset(span_newest, 0, spans * sizeof(span_newest[0]));
    int32_t big = -1;
    for (int32_t i = lru_oldest; i >= 0; i = entries[i].newer) {
        const TexEntry* e = &entries[i];
        if (e->order >= k) {
            if (big < 0 && !tex_busy(e)) {
                big = i;
            }
        } else if ((e->vram >> shift) < spans) {
            span_newest[e->vram >> shift] = tex_busy(e) ? UINT32_MAX : e->last_frame + 1;
        }
    }
    uint32_t best = big >= 0 ? entries[big].last_frame + 1 : UINT32_MAX;
    int32_t span = -1;
    for (uint32_t s = 0; s < spans; s++) {
        if (span_newest[s] != 0 && span_newest[s] < best) {
            best = span_newest[s];
            span = (int32_t)s;
        }
    }
    if (span >= 0) {
        for (int32_t i = lru_oldest; i >= 0;) {
            int32_t next = entries[i].newer;
            if (entries[i].order < k && (entries[i].vram >> shift) == (uint32_t)span) {
                remove_entry(i);
            }
            i = next;
        }
        return true;
    }
    if (big >= 0) {
        remove_entry(big);
        return true;
    }
    return false;
}

// A block of order k, freeing the least recently used textures until one fits: its offset, or
// -1 when the textures left are all in use.
static int32_t vram_alloc(uint32_t k) {
    int32_t off;
    while ((off = vram_take(k)) < 0) {
        int32_t oldest = lru_oldest;
        if (oldest < 0 || tex_busy(&entries[oldest])) {
            return -1;
        }
        if (k >= SPAN_MIN_ORDER && entries[oldest].order < k) {
            if (!evict_span(k)) {
                return -1;
            }
        } else {
            remove_entry(oldest);
        }
    }
    return off;
}

uint32_t tex_vram_free(void) {
    return vram_size - vram_used;
}

// Test discs, once a second.
void tex_print_stats(void) {
    char sizes[96];
    int n = 0;
    for (int k = VRAM_ORDERS - 1; k >= 0 && n < (int)sizeof(sizes) - 16; k--) {
        uint32_t b = order_bytes((uint32_t)k);
        if (tex_count[k] != 0) {
            n += snprintf(sizes + n, sizeof(sizes) - n, b >= 1024 ? " %uK:%u" : " %u:%u",
                          (unsigned)(b >= 1024 ? b / 1024 : b), (unsigned)tex_count[k]);
        }
    }
    sizes[n] = 0;
    printf("[q64dc] vram: %u of %u KB in use (peak %u), %u textures, last frame %u KB (peak %u); "
           "in the last second %u uploads (%u KB), %u evictions (%u KB); largest free %u KB; %u failed; sizes%s\n",
           (unsigned)(vram_used / 1024), (unsigned)(vram_size / 1024), (unsigned)(vram_peak / 1024),
           (unsigned)stat_tex_entries, (unsigned)(last_frame_bytes / 1024), (unsigned)(period_frame_peak / 1024),
           (unsigned)period_uploads, (unsigned)(period_upload_bytes / 1024), (unsigned)period_evictions,
           (unsigned)(period_evicted_bytes / 1024), (unsigned)(vram_largest_free() / 1024),
           (unsigned)stat_tex_failed, sizes);
    period_uploads = 0;
    period_upload_bytes = 0;
    period_evictions = 0;
    period_evicted_bytes = 0;
    period_frame_peak = 0;
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
    GCOUNT(lookup_slow, 1);
    const GfxTile* t = &gfx_tiles[tile];
#ifdef GFX_DIAG
    gfx_diag_tex.lookups++;
#endif
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
#ifdef GFX_DIAG
    h1 += gfx_diag_salt * 0x9E3779B1u;
#endif

    for (int32_t i = buckets[h1 & (TEX_BUCKETS - 1)]; i >= 0; i = entries[i].next) {
        TexEntry* e = &entries[i];
        if (e->h1 == h1 && e->h2 == h2 && e->pw == pw && e->ph == ph) {
            if (e->last_frame != frame) {
                e->last_frame = frame;
                lru_unlink(i);
                lru_push(i);
                frame_bytes += order_bytes(e->order);
            }
            out->ptr = vram_base + e->vram;
            out->pvr_fmt = e->pvr_fmt;
            return true;
        }
    }

    // Decode.
    pending_flush();
    GCOUNT(decodes, 1);
#ifdef GFX_DIAG
    gfx_diag_tex.decodes++;
#endif
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
    uint32_t order = (uint32_t)__builtin_ctz(size) - VRAM_MIN_ORDER;
    if (free_list < 0 && lru_oldest >= 0 && !tex_busy(&entries[lru_oldest])) {
        remove_entry(lru_oldest);
    }
    int32_t off = -1;
    if (free_list >= 0 && (vram_base != NULL || vram_init())) {
        off = vram_alloc(order);
    }
    if (off < 0) {
        static bool warned;
        stat_tex_failed++;
        if (!warned) {
            uint32_t busy = 0;
            for (int32_t i = lru_newest; i >= 0 && tex_busy(&entries[i]); i = entries[i].older) {
                busy += order_bytes(entries[i].order);
            }
            printf("[q64dc] gfx: out of texture memory: no room for %ux%u, %u textures, %u of %u KB in use, "
                   "%u KB of it by the last %u frames\n",
                   (unsigned)pw, (unsigned)ph, (unsigned)stat_tex_entries, (unsigned)(vram_used / 1024),
                   (unsigned)(vram_size / 1024), (unsigned)(busy / 1024), (unsigned)(TEX_KEEP_FRAMES + 1));
            warned = true;
        }
        return false;
    }
    pvr_ptr_t ptr = vram_base + off;
    // Plain stores: the store queues belong to the open display list.
    volatile uint32_t* dst = (volatile uint32_t*)ptr;
    const uint32_t* src = (const uint32_t*)decode_buf;
#ifdef GFX_DIAG
    // Without the upload the block keeps whatever texture was there before.
    uint32_t words = gfx_diag == DIAG_NO_UPLOAD ? 0 : size / 4;
    gfx_diag_tex.upload_bytes += words * 4;
    for (uint32_t i = 0; i < words; i++) {
        dst[i] = src[i];
    }
#else
    for (uint32_t i = 0; i < size / 4; i++) {
        dst[i] = src[i];
    }
#endif

    int32_t index = free_list;
    TexEntry* e = &entries[index];
    free_list = e->next;
    e->h1 = h1;
    e->h2 = h2;
    e->vram = (uint32_t)off;
    e->order = (uint8_t)order;
    e->pvr_fmt = pvr_fmt;
    e->pw = pw;
    e->ph = ph;
    e->last_frame = frame;
    e->next = buckets[h1 & (TEX_BUCKETS - 1)];
    buckets[h1 & (TEX_BUCKETS - 1)] = index;
    lru_push(index);
    tex_count[order]++;
    frame_bytes += size;
    stat_tex_uploads++;
    stat_tex_entries++;
    period_uploads++;
    period_upload_bytes += size;

    out->ptr = ptr;
    out->pvr_fmt = pvr_fmt;
    return true;
}

// tex_lookup's key and cache search, without its decode and TMEM-hash paths in between, so the
// usual hit runs through a few cache lines of code. Anything else is left to tex_lookup, which
// starts over.
bool tex_lookup_fast(int tile, int tlut, int variant, GfxTexture* out) {
    GCOUNT(lookups, 1);
    const GfxTile* t = &gfx_tiles[tile];
    bool clamp_s = (t->cms & 2) || t->masks == 0;
    bool clamp_t = (t->cmt & 2) || t->maskt == 0;
    uint32_t w = tile_extent(t->uls, t->lrs, t->masks, clamp_s);
    uint32_t h = tile_extent(t->ult, t->lrt, t->maskt, clamp_t);
    uint32_t pw = pow2_at_least(w), ph = pow2_at_least(h);
    if (pw * ph > MAX_TEXELS) {
        return tex_lookup(tile, tlut, variant, out);
    }
    bool paletted = t->siz <= G_IM_SIZ_8b && (tlut != G_TT_NONE || t->fmt == G_IM_FMT_CI);
    int key_tlut = paletted && tlut == G_TT_NONE ? G_TT_RGBA16 : tlut;
    uint32_t params[3] = {
        t->fmt | (t->siz << 3) | (key_tlut << 5) | (variant << 7) | (t->cms << 9) | (t->cmt << 11) | (t->palette << 13),
        w | (h << 16),
        t->line | (t->tmem << 16),
    };
    uint32_t first = t->siz == G_IM_SIZ_4b ? t->palette * 16u : 0;
    uint32_t count = t->siz == G_IM_SIZ_4b ? 16 : 256;
    uint32_t h1 = mix32(params[0] ^ 0x51ED270Bu) + mix32(params[1] ^ 0x2545F491u) + mix32(params[2]);
    uint32_t h2 = mix32(h1 ^ 0x9E3779B9u);
    ReadShape rs = { t->line, ((w << t->siz) + 1) >> 1, h };
    uint32_t read_q = (h - 1) * t->line + ((rs.row_bytes + 7) >> 3);
    if (read_q > 512) {
        read_q = 512;
    }
    if (!segments_key(t->tmem, read_q, &rs, &h1, &h2) ||
        (paletted && !segments_key(0x100 + first, count, NULL, &h1, &h2))) {
        return tex_lookup(tile, tlut, variant, out);
    }
#ifdef GFX_DIAG
    h1 += gfx_diag_salt * 0x9E3779B1u;
#endif
    for (int32_t i = buckets[h1 & (TEX_BUCKETS - 1)]; i >= 0; i = entries[i].next) {
        TexEntry* e = &entries[i];
        if (e->h1 == h1 && e->h2 == h2 && e->pw == pw && e->ph == ph) {
            if (e->last_frame != frame) {
                e->last_frame = frame;
                lru_unlink(i);
                lru_push(i);
                frame_bytes += order_bytes(e->order);
            }
#ifdef GFX_DIAG
            gfx_diag_tex.lookups++;
#endif
            out->clamp_s = clamp_s;
            out->clamp_t = clamp_t;
            out->mirror_s = (t->cms & 1) && !clamp_s;
            out->mirror_t = (t->cmt & 1) && !clamp_t;
            out->pw = pw;
            out->ph = ph;
            out->ptr = vram_base + e->vram;
            out->pvr_fmt = e->pvr_fmt;
#ifdef GFX_XCHECK
            GfxTexture ref;
            bool ok = tex_lookup(tile, tlut, variant, &ref);
            gfx_xcheck_runs[XC_TEX]++;
            if ((!ok || ref.ptr != out->ptr || ref.pvr_fmt != out->pvr_fmt || ref.pw != out->pw || ref.ph != out->ph ||
                 ref.clamp_s != out->clamp_s || ref.clamp_t != out->clamp_t || ref.mirror_s != out->mirror_s ||
                 ref.mirror_t != out->mirror_t) &&
                gfx_xcheck_fails[XC_TEX]++ < 8) {
                printf("[q64dc] xcheck tex: tile %d fast %p %ux%u, original %d %p %ux%u\n", tile, out->ptr,
                       (unsigned)out->pw, (unsigned)out->ph, (int)ok, ref.ptr, (unsigned)ref.pw, (unsigned)ref.ph);
            }
#endif
            return true;
        }
    }
    return tex_lookup(tile, tlut, variant, out);
}
