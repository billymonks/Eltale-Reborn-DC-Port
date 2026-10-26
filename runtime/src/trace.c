// codegen.abi_trace (see recomp.h): taint for the registers the generator's analysis calls dead, and a
// line for the first use of a tainted value at each instruction that can make it matter.

#include <stddef.h>
#include <stdio.h>

#include "recomp.h"

static const uint16_t gpr_offset[32] = {
    offsetof(recomp_context, r0),  offsetof(recomp_context, r1),  offsetof(recomp_context, r2),
    offsetof(recomp_context, r3),  offsetof(recomp_context, r4),  offsetof(recomp_context, r5),
    offsetof(recomp_context, r6),  offsetof(recomp_context, r7),  offsetof(recomp_context, r8),
    offsetof(recomp_context, r9),  offsetof(recomp_context, r10), offsetof(recomp_context, r11),
    offsetof(recomp_context, r12), offsetof(recomp_context, r13), offsetof(recomp_context, r14),
    offsetof(recomp_context, r15), offsetof(recomp_context, r16), offsetof(recomp_context, r17),
    offsetof(recomp_context, r18), offsetof(recomp_context, r19), offsetof(recomp_context, r20),
    offsetof(recomp_context, r21), offsetof(recomp_context, r22), offsetof(recomp_context, r23),
    offsetof(recomp_context, r24), offsetof(recomp_context, r25), offsetof(recomp_context, r26),
    offsetof(recomp_context, r27), offsetof(recomp_context, r28), offsetof(recomp_context, r29),
    offsetof(recomp_context, r30), offsetof(recomp_context, r31),
};

void trace_set(recomp_context* ctx, uint32_t mask, uint32_t origin) {
    ctx->taint |= mask;
    for (int n = 0; n < 32; n++) {
        if (mask & (1u << n)) {
            ctx->taint_origin[n] = origin;
        }
    }
}

void trace_spread(recomp_context* ctx, uint32_t from, uint32_t to) {
    uint32_t origin = ctx->taint_origin[__builtin_ctz(from)];
    ctx->taint |= to;
    for (int n = 0; n < 32; n++) {
        if (to & (1u << n)) {
            ctx->taint_origin[n] = origin;
        }
    }
}

// Sites already printed: instruction, register, kind and origin.
#define SEEN_SIZE 16384
static uint32_t seen_vram[SEEN_SIZE];
static uint32_t seen_key[SEEN_SIZE];
static uint32_t seen_count, hit_count;

void trace_hit(recomp_context* ctx, uint32_t regs, uint32_t vram, int kind) {
    for (int n = 0; n < 32; n++) {
        if (!(regs & (1u << n))) {
            continue;
        }
        hit_count++;
        uint32_t origin = ctx->taint_origin[n];
        uint32_t key = origin ^ ((uint32_t)n << 24) ^ ((uint32_t)kind << 16) ^ 1;
        uint32_t h = (vram * 2654435761u ^ key * 40503u) & (SEEN_SIZE - 1);
        int fresh = 1;
        for (uint32_t probe = 0; probe < SEEN_SIZE; probe++, h = (h + 1) & (SEEN_SIZE - 1)) {
            if (seen_key[h] == 0) {
                seen_vram[h] = vram;
                seen_key[h] = key;
                seen_count++;
                break;
            }
            if (seen_vram[h] == vram && seen_key[h] == key) {
                fresh = 0;
                break;
            }
        }
        if (fresh) {
            uint32_t value = n == 0 ? 0 : *(const uint32_t*)((const uint8_t*)ctx + gpr_offset[n]);
            printf("[q64dc] TRACE %c %08X r%d=%08X from %08X (%u sites)\n", kind, (unsigned)vram, n,
                   (unsigned)value, (unsigned)origin, (unsigned)seen_count);
        }
    }
}
