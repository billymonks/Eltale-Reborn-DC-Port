// Loads statically recompiled mods into rdram and implements the recomp functions they import.

#include <kos.h>
#include <stdio.h>
#include <string.h>

#include "modrt.h"
#include "../src/test.h"
#include "../src/ultra.h"

// --- Loading and function lookup ---

static void heap_init(uint32_t start, uint32_t end);

void mods_init(void) {
    memcpy(RDRAM_PTR(mods_image_vram), mods_image, mods_image_words * 4);
    heap_init((mods_end_vram + 15) & ~15u, MOD_RAM_BASE + MOD_RAM_SIZE);
    for (uint32_t i = 0; i < mods_count; i++) {
        const ModInfo* mod = mods_list[i];
        printf("[q64dc] mod %s %u.%u.%u\n", mod->id, (unsigned)mod->version[0], (unsigned)mod->version[1], (unsigned)mod->version[2]);
        // The config is fixed at build time: mod.toml's defaults, or MOD_CONFIG. Enums show the
        // index of their choice.
        for (uint32_t c = 0; c < mod->num_config; c++) {
            const ModConfigValue* v = &mod->config[c];
            if (v->str != NULL && v->str[0] != '\0') {
                printf("[q64dc]   config %s = \"%s\"\n", v->key, v->str);
            } else if (v->dbl == (double)v->u32) {
                printf("[q64dc]   config %s = %u\n", v->key, (unsigned)v->u32);
            } else {
                printf("[q64dc]   config %s = %g\n", v->key, v->dbl);
            }
        }
    }
    printf("[q64dc] mods at 0x%08X-0x%08X, %u KB of mod heap\n", (unsigned)MOD_RAM_BASE, (unsigned)mods_end_vram,
           (unsigned)((MOD_RAM_BASE + MOD_RAM_SIZE - mods_end_vram) / 1024));
}

recomp_func_t* mods_get_function(uint32_t vram) {
    uint32_t lo = 0, hi = mods_num_funcs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (mods_funcs[mid].vram < vram) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < mods_num_funcs && mods_funcs[lo].vram == vram ? mods_funcs[lo].func : NULL;
}

// --- Hooks ---

#define HOOK_DEPTH 32

// Game threads run one at a time but can switch inside a hook, so each thread has its own stack.
static __thread const recomp_context* hook_saved[HOOK_DEPTH];
static __thread int hook_depth;

void mod_call_hook(uint8_t* rdram, recomp_context* ctx, recomp_func_t* hook) {
    recomp_context saved = *ctx;
    if (hook_depth < HOOK_DEPTH) {
        hook_saved[hook_depth] = &saved;
    }
    hook_depth++;
    hook(rdram, ctx);
    hook_depth--;
    *ctx = saved;
}

static const recomp_context* hook_context(void) {
    if (hook_depth <= 0 || hook_depth > HOOK_DEPTH) {
        runtime_fatal("[q64dc] recomphook_get_return_* called outside a return hook");
    }
    return hook_saved[hook_depth - 1];
}

MODRT_IMPORT(recomphook_get_return_s32) { (void)rdram; (void)mod; ctx->r2 = hook_context()->r2; }
MODRT_IMPORT(recomphook_get_return_u32) { (void)rdram; (void)mod; ctx->r2 = hook_context()->r2; }
MODRT_IMPORT(recomphook_get_return_ptr) { (void)rdram; (void)mod; ctx->r2 = hook_context()->r2; }
MODRT_IMPORT(recomphook_get_return_s16) { (void)rdram; (void)mod; ctx->r2 = (gpr)(int32_t)(int16_t)hook_context()->r2; }
MODRT_IMPORT(recomphook_get_return_u16) { (void)rdram; (void)mod; ctx->r2 = hook_context()->r2 & 0xFFFF; }
MODRT_IMPORT(recomphook_get_return_s8) { (void)rdram; (void)mod; ctx->r2 = (gpr)(int32_t)(int8_t)hook_context()->r2; }
MODRT_IMPORT(recomphook_get_return_u8) { (void)rdram; (void)mod; ctx->r2 = hook_context()->r2 & 0xFF; }
MODRT_IMPORT(recomphook_get_return_float) { (void)rdram; (void)mod; ctx->f0.fl = hook_context()->f0.fl; }
MODRT_IMPORT(recomphook_get_return_double) { (void)rdram; (void)mod; ctx->f0.d = hook_context()->f0.d; }

// 64-bit results are split over v0 (high word) and v1 (low word) in o32.
MODRT_IMPORT(recomphook_get_return_s64) {
    (void)rdram;
    (void)mod;
    const recomp_context* saved = hook_context();
    ctx->r2 = saved->r2;
    ctx->r3 = saved->r3;
}

MODRT_IMPORT(recomphook_get_return_u64) {
    (void)rdram;
    (void)mod;
    const recomp_context* saved = hook_context();
    ctx->r2 = saved->r2;
    ctx->r3 = saved->r3;
}

// --- Heap (recomp_alloc), in mod RAM after the mods ---
// First fit over a list of blocks that each start with a 16-byte header; free blocks merge with
// free neighbours.

typedef struct {
    uint32_t size;  // including the header
    uint32_t used;
    uint32_t prev;  // previous block's address, 0 for the first
    uint32_t pad;
} HeapBlock;

static uint32_t heap_start, heap_end;

static HeapBlock* block_at(uint32_t addr) {
    return (HeapBlock*)RDRAM_PTR(addr);
}

static void heap_init(uint32_t start, uint32_t end) {
    heap_start = start;
    heap_end = end;
    if (end - start < 2 * sizeof(HeapBlock)) {
        heap_end = heap_start;
        return;
    }
    HeapBlock* b = block_at(start);
    b->size = end - start;
    b->used = 0;
    b->prev = 0;
}

static uint32_t heap_alloc(uint32_t size) {
    uint32_t need = ((size + 15) & ~15u) + sizeof(HeapBlock);
    for (uint32_t addr = heap_start; addr < heap_end; addr += block_at(addr)->size) {
        HeapBlock* b = block_at(addr);
        if (b->used || b->size < need) {
            continue;
        }
        if (b->size - need >= 2 * sizeof(HeapBlock)) {
            uint32_t rest = addr + need;
            HeapBlock* r = block_at(rest);
            r->size = b->size - need;
            r->used = 0;
            r->prev = addr;
            if (rest + r->size < heap_end) {
                block_at(rest + r->size)->prev = rest;
            }
            b->size = need;
        }
        b->used = 1;
        return addr + sizeof(HeapBlock);
    }
    return 0;
}

static void heap_free(uint32_t ptr) {
    if (ptr == 0) {
        return;
    }
    uint32_t addr = ptr - sizeof(HeapBlock);
    if (addr < heap_start || addr >= heap_end || !block_at(addr)->used) {
        runtime_fatal("[q64dc] recomp_free of 0x%08X, which recomp_alloc didn't return", (unsigned)ptr);
    }
    HeapBlock* b = block_at(addr);
    b->used = 0;
    uint32_t next = addr + b->size;
    if (next < heap_end && !block_at(next)->used) {
        b->size += block_at(next)->size;
        if (addr + b->size < heap_end) {
            block_at(addr + b->size)->prev = addr;
        }
    }
    if (b->prev != 0 && !block_at(b->prev)->used) {
        HeapBlock* p = block_at(b->prev);
        p->size += b->size;
        if (b->prev + p->size < heap_end) {
            block_at(b->prev + p->size)->prev = b->prev;
        }
    }
}

MODRT_IMPORT(recomp_alloc) {
    (void)mod;
    uint32_t ptr = heap_alloc(ctx->r4);
    if (ptr == 0) {
        runtime_fatal("[q64dc] %s: recomp_alloc(%u) is out of mod heap (MOD_RAM_SIZE)", mod->id, (unsigned)ctx->r4);
    }
    memset(RDRAM_PTR(ptr), 0, (ctx->r4 + 3) & ~3u);
    (void)rdram;
    ctx->r2 = ptr;
}

MODRT_IMPORT(recomp_free) {
    (void)rdram;
    (void)mod;
    heap_free(ctx->r4);
}

// --- Config ---

static void read_string(uint8_t* rdram, uint32_t addr, char* out, size_t size) {
    size_t i = 0;
    for (; i + 1 < size; i++) {
        char c = (char)MEM_BU(i, addr);
        if (c == '\0') {
            break;
        }
        out[i] = c;
    }
    out[i] = '\0';
}

static const ModConfigValue* config_value(uint8_t* rdram, recomp_context* ctx, const ModInfo* mod) {
    char key[64];
    read_string(rdram, ctx->r4, key, sizeof(key));
    for (uint32_t i = 0; i < mod->num_config; i++) {
        if (strcmp(mod->config[i].key, key) == 0) {
            return &mod->config[i];
        }
    }
    // Mods read their options every frame, and the serial port is slow, so each missing one is
    // reported once.
    static char reported[16][64];
    static uint32_t num_reported;
    for (uint32_t i = 0; i < num_reported; i++) {
        if (strcmp(reported[i], key) == 0) {
            return NULL;
        }
    }
    if (num_reported < 16) {
        strcpy(reported[num_reported++], key);
    }
    printf("[q64dc] %s asked for config option \"%s\", which its mod.toml doesn't have; it reads 0\n", mod->id, key);
    return NULL;
}

MODRT_IMPORT(recomp_get_config_u32) {
    const ModConfigValue* v = config_value(rdram, ctx, mod);
    ctx->r2 = v != NULL ? v->u32 : 0;
}

MODRT_IMPORT(recomp_get_config_double) {
    const ModConfigValue* v = config_value(rdram, ctx, mod);
    ctx->f0.d = v != NULL ? v->dbl : 0.0;
}

MODRT_IMPORT(recomp_get_config_string) {
    const ModConfigValue* v = config_value(rdram, ctx, mod);
    const char* s = v != NULL && v->str != NULL ? v->str : "";
    uint32_t len = (uint32_t)strlen(s);
    uint32_t ptr = heap_alloc(len + 1);
    if (ptr == 0) {
        runtime_fatal("[q64dc] %s: recomp_get_config_string is out of mod heap", mod->id);
    }
    for (uint32_t i = 0; i <= len; i++) {
        MEM_B(i, ptr) = s[i];
    }
    ctx->r2 = ptr;
}

MODRT_IMPORT(recomp_free_config_string) {
    (void)rdram;
    (void)mod;
    heap_free(ctx->r4);
}

MODRT_IMPORT(recomp_get_mod_version) {
    MEM_W(0, ctx->r4) = (int32_t)mod->version[0];
    MEM_W(0, ctx->r5) = (int32_t)mod->version[1];
    MEM_W(0, ctx->r6) = (int32_t)mod->version[2];
}

// The PC recomp can fade flickering surfaces with a smoothed alpha. This port doesn't, so the
// option is always off and the mod keeps the original flicker.
MODRT_IMPORT(recomp_get_smooth_transparency) {
    (void)rdram;
    (void)mod;
    ctx->r2 = 0;
}

MODRT_IMPORT(recomp_is_dependency_met) {
    (void)mod;
    char id[64];
    read_string(rdram, ctx->r4, id, sizeof(id));
    ctx->r2 = 2; // DEPENDENCY_STATUS_NOT_FOUND
    for (uint32_t i = 0; i < mods_count; i++) {
        if (strcmp(mods_list[i]->id, id) == 0) {
            ctx->r2 = 0; // DEPENDENCY_STATUS_FOUND
        }
    }
}

// --- recomp_printf ---
// Mod logging goes to the serial console on test discs (or always with -DMOD_PRINTF=1), at most
// PRINTF_PER_SECOND messages a second; a burst over that is counted and reported instead.

#ifndef MOD_PRINTF
#define MOD_PRINTF 0
#endif
#define PRINTF_PER_SECOND 30

// The o32 argument slots: the first four in a0-a3, the rest on the caller's stack above its home space.
static uint32_t arg_slot(uint8_t* rdram, recomp_context* ctx, uint32_t n) {
    switch (n) {
        case 0: return ctx->r4;
        case 1: return ctx->r5;
        case 2: return ctx->r6;
        case 3: return ctx->r7;
        default: return (uint32_t)MEM_W(4 * n, ctx->r29);
    }
}

static int format_mips(uint8_t* rdram, recomp_context* ctx, char* out, size_t size) {
    char fmt[256];
    read_string(rdram, ctx->r4, fmt, sizeof(fmt));
    uint32_t slot = 1;
    size_t len = 0;
    const char* p = fmt;
#define APPEND(...) do { \
        int n_ = snprintf(out + len, len < size ? size - len : 0, __VA_ARGS__); \
        if (n_ > 0) len += (size_t)n_; \
    } while (0)
    while (*p != '\0') {
        if (*p != '%') {
            if (len + 1 < size) {
                out[len] = *p;
            }
            len++;
            p++;
            continue;
        }
        // Copy one conversion spec, dropping its length modifiers and resolving '*'.
        char spec[32];
        size_t s = 0;
        spec[s++] = *p++;
        int longs = 0;
        int star_values[2];
        int stars = 0;
        while (*p != '\0' && strchr("-+ #0123456789.*hlLqjzt", *p) != NULL && s < sizeof(spec) - 2) {
            if (*p == 'l' || *p == 'q' || *p == 'L') {
                longs += *p == 'q' ? 2 : 1;
            } else if (*p == '*') {
                if (stars < 2) {
                    star_values[stars++] = (int32_t)arg_slot(rdram, ctx, slot++);
                }
                spec[s++] = '*';
            } else if (*p != 'h' && *p != 'j' && *p != 'z' && *p != 't') {
                spec[s++] = *p;
            }
            p++;
        }
        char conv = *p;
        if (conv == '\0') {
            break;
        }
        p++;
        if (conv == '%') {
            APPEND("%%");
            continue;
        }
        bool wide = longs >= 2;
        bool is_float = strchr("fFeEgGaA", conv) != NULL;
        if (wide || is_float) {
            slot = (slot + 1) & ~1u;
        }
        uint64_t value = arg_slot(rdram, ctx, slot++);
        if (wide || is_float) {
            value = value << 32 | arg_slot(rdram, ctx, slot++);
        }
        if (is_float || wide) {
            spec[s++] = 'l';
            if (!is_float) {
                spec[s++] = 'l';
            }
        }
        spec[s++] = conv;
        spec[s] = '\0';

        char str[256];
        if (conv == 's') {
            read_string(rdram, (uint32_t)value, str, sizeof(str));
        }
        if (conv == 'n') {
            continue;
        }
#define APPEND_SPEC(v) do { \
            if (stars == 2) APPEND(spec, star_values[0], star_values[1], v); \
            else if (stars == 1) APPEND(spec, star_values[0], v); \
            else APPEND(spec, v); \
        } while (0)
        if (conv == 's') {
            APPEND_SPEC(str);
        } else if (is_float) {
            union { uint64_t u; double d; } f = { value };
            APPEND_SPEC(f.d);
        } else if (conv == 'p') {
            spec[s - 1] = 'X';
            APPEND_SPEC((unsigned)value);
        } else if (wide) {
            APPEND_SPEC((long long)value);
        } else if (strchr("di", conv) != NULL) {
            APPEND_SPEC((int)(int32_t)value);
        } else {
            APPEND_SPEC((unsigned)value);
        }
#undef APPEND_SPEC
    }
#undef APPEND
    if (size > 0) {
        out[len < size ? len : size - 1] = '\0';
    }
    return (int)len;
}

MODRT_IMPORT(recomp_printf) {
    static uint64_t window_start;
    static uint32_t in_window, dropped;

    if ((!MOD_PRINTF && !test_disc()) || pace_running()) {
        ctx->r2 = 0;
        return;
    }
    uint64_t now = timer_ms_gettime64();
    if (now - window_start >= 1000) {
        if (dropped != 0) {
            printf("[q64dc] %u mod messages dropped\n", (unsigned)dropped);
        }
        window_start = now;
        in_window = 0;
        dropped = 0;
    }
    if (in_window >= PRINTF_PER_SECOND) {
        dropped++;
        ctx->r2 = 0;
        return;
    }
    in_window++;

    char line[512];
    int len = format_mips(rdram, ctx, line, sizeof(line));
    (void)mod;
    fputs(line, stdout);
    ctx->r2 = (gpr)len;
}
