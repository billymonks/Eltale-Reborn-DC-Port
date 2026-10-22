// RDP texture memory and the PowerVR texture cache.
//
// Loads are emulated into a 4 KB TMEM, including the swapped odd rows, so tiles read texels
// the way the RDP does. A texture is decoded from TMEM when a tile is drawn, and cached in
// VRAM under a hash of the TMEM bytes, palette and tile parameters it was decoded from.

#include <stdio.h>
#include <string.h>

#include "gfx_internal.h"

GfxTile gfx_tiles[8];
GfxImage gfx_timg;

uint32_t stat_tex_uploads;
uint32_t stat_tex_entries;

// Stored as 32-bit words in host order, like RDRAM, so byte b of TMEM is at b ^ 3.
static uint32_t tmem32[1024];
#define TMEM8(a) (((uint8_t*)tmem32)[((a) & 0xFFF) ^ 3])

static inline uint32_t tmem16(uint32_t a) {
    return (TMEM8(a) << 8) | TMEM8(a + 1);
}

static inline bool rdram_range_ok(uint32_t addr, uint32_t len) {
    uint32_t off = addr - 0x80000000u;
    return off < RDRAM_SIZE && len <= RDRAM_SIZE - off;
}

static inline uint8_t rd8(uint32_t addr) {
    return *(uint8_t*)RDRAM_PTR(addr ^ 3);
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
    for (uint32_t j = 0; j < qwords; j++) {
        uint32_t q = (t->tmem + j) & 511;
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

void tmem_load_tile(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    const GfxTile* t = &gfx_tiles[tile];
    uint32_t siz = gfx_timg.siz;
    uint32_t s0 = uls >> 2, t0 = ult >> 2, s1 = lrs >> 2, t1 = lrt >> 2;
    if (s1 < s0 || t1 < t0) {
        return;
    }
    uint32_t row_bytes = (((s1 - s0 + 1) << siz) + 1) >> 1;
    uint32_t stride = t->line * 8;
    for (uint32_t row = 0; row <= t1 - t0; row++) {
        uint32_t src = gfx_timg.addr + (((((t0 + row) * gfx_timg.width) + s0) << siz) >> 1);
        if (!rdram_range_ok(src, row_bytes)) {
            return;
        }
        uint32_t dst = t->tmem * 8 + row * stride;
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
void tmem_load_tlut(int tile, uint32_t count) {
    const GfxTile* t = &gfx_tiles[tile];
    uint32_t src = gfx_timg.addr;
    if (!rdram_range_ok(src, count * 2)) {
        return;
    }
    uint32_t dst = t->tmem * 8;
    for (uint32_t i = 0; i < count; i++) {
        TMEM8(dst + i * 8) = rd8(src + i * 2);
        TMEM8(dst + i * 8 + 1) = rd8(src + i * 2 + 1);
    }
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

    // Hash the TMEM bytes the tile covers, its palette and its parameters.
    uint32_t h1 = 2166136261u, h2 = 0x9E3779B9u;
#define MIX(v) do { uint32_t v_ = (v); h1 = (h1 ^ v_) * 16777619u; h2 += v_; h2 = (h2 << 11) | (h2 >> 21); } while (0)
    MIX(t->fmt | (t->siz << 3) | (tlut << 5) | (variant << 7) | (t->cms << 9) | (t->cmt << 11) | (t->palette << 13));
    MIX(w | (h << 16));
    MIX(t->line | (t->tmem << 16));
    uint32_t row_bytes = t->line * 8;
    uint32_t bytes = row_bytes * h;
    if (bytes > 4096 || bytes == 0) {
        bytes = 4096;
    }
    uint32_t base = (t->tmem * 8) >> 2;
    for (uint32_t i = 0; i < (bytes + 3) >> 2; i++) {
        MIX(tmem32[(base + i) & 1023]);
    }
    if (paletted) {
        uint32_t first = t->siz == G_IM_SIZ_4b ? t->palette * 16u : 0;
        uint32_t count = t->siz == G_IM_SIZ_4b ? 16 : 256;
        for (uint32_t i = 0; i < count; i += 2) {
            MIX((tmem16(0x800 + (first + i) * 8) << 16) | tmem16(0x800 + (first + i + 1) * 8));
        }
    }
#undef MIX

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
