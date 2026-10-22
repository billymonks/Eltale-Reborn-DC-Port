// libultra functions with little or nothing to do on the Dreamcast.

#include "ultra.h"

// Boot parameters libultra reads from low memory.
#define OS_TV_TYPE   0x80000300u
#define OS_ROM_BASE  0x80000308u
#define OS_RESET_TYPE 0x8000030Cu
#define OS_MEM_SIZE  0x80000318u
#define OS_TV_NTSC   1

static uint32_t int_mask = 0xFFFFFFFFu;
static uint32_t fpc_csr = 0x01000800u;

void __osInitialize_common_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    MEM_W(0, OS_TV_TYPE) = OS_TV_NTSC;
    MEM_W(0, OS_ROM_BASE) = (int32_t)0xB0000000u;
    MEM_W(0, OS_RESET_TYPE) = 0;
    MEM_W(0, OS_MEM_SIZE) = RDRAM_SIZE;
}

// Only one game thread runs at a time and events are delivered through the scheduler, so there
// are no interrupts to mask.
void __osDisableInt_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 1;
}

void __osRestoreInt_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    sched_check_yield();
}

void osSetIntMask_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = int_mask;
    int_mask = ctx->r4;
}

void __osSetFpcCsr_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = fpc_csr;
    fpc_csr = ctx->r4;
    set_cop1_cs(fpc_csr);
}

void osInvalDCache_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osInvalICache_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osWritebackDCache_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osWritebackDCacheAll_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osVirtualToPhysical_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t addr = ctx->r4;
    if (addr >= 0x80000000u && addr < 0xC0000000u) {
        addr &= 0x1FFFFFFFu;
    }
    ctx->r2 = addr;
}

void rmonPrintf_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

// libultra's 64-bit integer helpers: o32 passes each 64-bit argument in a register pair, high
// word first, and returns the result in v0 (high) and v1 (low).
static inline uint64_t arg_pair(gpr hi, gpr lo) {
    return ((uint64_t)hi << 32) | lo;
}

static inline void return_pair(recomp_context* ctx, uint64_t v) {
    ctx->r2 = (gpr)(v >> 32);
    ctx->r3 = (gpr)v;
}

void __ll_div_recomp(uint8_t* rdram, recomp_context* ctx) {
    int64_t a = (int64_t)arg_pair(ctx->r4, ctx->r5);
    int64_t b = (int64_t)arg_pair(ctx->r6, ctx->r7);
    return_pair(ctx, b == 0 ? 0 : (uint64_t)(a / b));
}

void __ll_mul_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint64_t a = arg_pair(ctx->r4, ctx->r5);
    uint64_t b = arg_pair(ctx->r6, ctx->r7);
    return_pair(ctx, a * b);
}

void __ull_div_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint64_t a = arg_pair(ctx->r4, ctx->r5);
    uint64_t b = arg_pair(ctx->r6, ctx->r7);
    return_pair(ctx, b == 0 ? 0 : a / b);
}

void __ull_rem_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint64_t a = arg_pair(ctx->r4, ctx->r5);
    uint64_t b = arg_pair(ctx->r6, ctx->r7);
    return_pair(ctx, b == 0 ? a : a % b);
}
