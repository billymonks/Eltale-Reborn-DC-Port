#ifndef __RECOMP_H__
#define __RECOMP_H__

// Dreamcast (SH-4) version of N64Recomp's recomp.h, for output generated with `gpr_32bit = true`.
// GPRs are 32 bits wide: the recompiler rejects every instruction that needs the upper half of a
// 64-bit register, and libultra's 64-bit helpers (__ll_div, __ull_rem, ...) are runtime functions.

#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#if !defined(__GNUC__)
    #error "The Dreamcast runtime expects GCC"
#endif

#define RECOMP_FUNC __attribute__((noipa))
// codegen.inline_leaves: small functions without calls, which callers in the same file may inline. Upstream's noipa
// pairs with optimize("rounding-math") to keep folding away from the FPU's rounding mode; the runtime builds
// without rounding-math, so inlining changes nothing there. noclone keeps one copy under the function's name.
#define RECOMP_LEAF_FUNC __attribute__((noclone))
#define SET_FENV_ACCESS()

typedef uint32_t gpr;

#define SIGNED(val) \
    ((int32_t)(val))

#define ADD32(a, b) \
    ((gpr)((a) + (b)))

#define SUB32(a, b) \
    ((gpr)((a) - (b)))

// rdram holds the N64's KSEG0 (0x80000000) as 32-bit words in host order, so halfword and byte
// accesses flip the low address bits.
// RECOMP_RB points 0x80000000 below rdram. The generated functions redefine it as their local
// `uint8_t* restrict const rb`: a separate restrict pointer keeps GCC from folding the bias into
// every offset as a 32-bit literal, so it keeps one base register per MIPS base register and uses
// @(disp,Rn), and it still knows those accesses can't touch the context.
#define RECOMP_RB (rdram - 0x80000000u)

#define RDRAM_PTR(addr) \
    (RECOMP_RB + (uint32_t)(addr))

#define MEM_W(offset, reg) \
    (*(int32_t*)(RECOMP_RB + (gpr)(reg) + (gpr)(offset)))

#define MEM_WU(offset, reg) \
    (*(uint32_t*)(RECOMP_RB + (gpr)(reg) + (gpr)(offset)))

#define MEM_H(offset, reg) \
    (*(int16_t*)(RECOMP_RB + (((gpr)(reg) + (gpr)(offset)) ^ 2)))

#define MEM_B(offset, reg) \
    (*(int8_t*)(RECOMP_RB + (((gpr)(reg) + (gpr)(offset)) ^ 3)))

#define MEM_HU(offset, reg) \
    (*(uint16_t*)(RECOMP_RB + (((gpr)(reg) + (gpr)(offset)) ^ 2)))

#define MEM_BU(offset, reg) \
    (*(uint8_t*)(RECOMP_RB + (((gpr)(reg) + (gpr)(offset)) ^ 3)))

// sp-relative halfwords and bytes. sp is a multiple of 4, so the swizzle only touches the offset.
#define MEM_H_SP(offset, sp) \
    (*(int16_t*)(RECOMP_RB + (gpr)(sp) + ((gpr)(offset) ^ 2)))

#define MEM_B_SP(offset, sp) \
    (*(int8_t*)(RECOMP_RB + (gpr)(sp) + ((gpr)(offset) ^ 3)))

#define MEM_HU_SP(offset, sp) \
    (*(uint16_t*)(RECOMP_RB + (gpr)(sp) + ((gpr)(offset) ^ 2)))

#define MEM_BU_SP(offset, sp) \
    (*(uint8_t*)(RECOMP_RB + (gpr)(sp) + ((gpr)(offset) ^ 3)))

// A new stack pointer the compiler can't see through: frame offsets then stay small and positive
// from it (@(disp,Rn) only reaches 0-60) instead of turning negative from the caller's sp.
#define SP_ADJ(sp, imm) \
    ({ gpr sp_ = (gpr)((sp) + (imm)); __asm__("" : "+r"(sp_)); sp_; })

// Doubleword accesses only come from ldc1/sdc1 (FPU doubles); 64-bit GPR loads/stores are rejected.
#define SD(val, offset, reg) { \
    MEM_WU((offset) + 4, reg) = (uint32_t)((uint64_t)(val) >> 0); \
    MEM_WU((offset) + 0, reg) = (uint32_t)((uint64_t)(val) >> 32); \
}

#define LD(offset, reg) \
    ((uint64_t)(uint32_t)MEM_WU((offset) + 4, reg) | ((uint64_t)(uint32_t)MEM_WU(offset, reg) << 32))

static inline gpr do_lwl(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = offset + reg;
    uint32_t loaded_value = MEM_WU(0, address & ~3u);
    gpr misalignment = address & 3;
    gpr masked_value = initial_value & ~(0xFFFFFFFFu << (misalignment * 8));
    return masked_value | (loaded_value << (misalignment * 8));
}

static inline gpr do_lwr(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = offset + reg;
    uint32_t loaded_value = MEM_WU(0, address & ~3u);
    gpr misalignment = address & 3;
    gpr masked_value = initial_value & ~(0xFFFFFFFFu >> (24 - misalignment * 8));
    return masked_value | (loaded_value >> (24 - misalignment * 8));
}

static inline void do_swl(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = offset + reg;
    gpr word_address = address & ~3u;
    uint32_t initial_value = MEM_WU(0, word_address);
    gpr misalignment = address & 3;
    uint32_t masked_initial_value = initial_value & ~(0xFFFFFFFFu >> (misalignment * 8));
    MEM_WU(0, word_address) = masked_initial_value | (val >> (misalignment * 8));
}

static inline void do_swr(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = offset + reg;
    gpr word_address = address & ~3u;
    uint32_t initial_value = MEM_WU(0, word_address);
    gpr misalignment = address & 3;
    uint32_t masked_initial_value = initial_value & ~(0xFFFFFFFFu << (24 - misalignment * 8));
    MEM_WU(0, word_address) = masked_initial_value | (val << (24 - misalignment * 8));
}

// MIPS leaves division by zero undefined and doesn't trap; SH-4 division is a libgcc call, so
// guard it and return what the VR4300 produces.
static inline void DIV32(int32_t a, int32_t b, gpr* lo, gpr* hi) {
    if (b == 0) {
        *lo = a < 0 ? 1 : (gpr)-1;
        *hi = (gpr)a;
    } else if (a == INT32_MIN && b == -1) {
        *lo = (gpr)a;
        *hi = 0;
    } else {
        *lo = (gpr)(a / b);
        *hi = (gpr)(a % b);
    }
}

static inline void DIVU32(uint32_t a, uint32_t b, gpr* lo, gpr* hi) {
    if (b == 0) {
        *lo = 0xFFFFFFFFu;
        *hi = a;
    } else {
        *lo = a / b;
        *hi = a % b;
    }
}

// The SH-4 FPU only rounds to nearest or towards zero; the N64's ceil/floor modes (2, 3) are
// remembered for cfc1 but computed to nearest.
#ifdef __cplusplus
extern "C"
#else
extern
#endif
uint32_t recomp_cop1_rounding_mode;

static inline uint32_t get_cop1_cs(void) {
    return recomp_cop1_rounding_mode;
}

static inline void set_cop1_cs(uint32_t val) {
    recomp_cop1_rounding_mode = val & 3;
    uint32_t fpscr = __builtin_sh_get_fpscr() & ~3u;
    __builtin_sh_set_fpscr(fpscr | (recomp_cop1_rounding_mode == 1 ? 1u : 0u));
}

#define S32(val) \
    ((int32_t)(val))

#define U32(val) \
    ((uint32_t)(val))

#define S64(val) \
    ((int64_t)(val))

#define U64(val) \
    ((uint64_t)(val))

// neg.s and abs.s work on the sign bit in an integer register. GCC treats the SH-4's fneg/fabs as
// precision-independent and may run them with FPSCR.PR=1 between double operations; hardware
// allows that, but emulators before Flycast 2.7 apply them to the even register of the pair.
static inline float sign_op_s(float val, uint32_t xor_mask, uint32_t and_mask) {
    union {
        float f;
        uint32_t u;
    } v = { val };
    __asm__("" : "+r"(v.u));
    v.u = (v.u & and_mask) ^ xor_mask;
    return v.f;
}

#define NEG_S(val) \
    sign_op_s((val), 0x80000000u, 0xFFFFFFFFu)

#define ABS_S(val) \
    sign_op_s((val), 0u, 0x7FFFFFFFu)

// codegen.regcache = "gpr+fpr": the FPU's own fneg and fabs, which only touch the sign bit like the integer
// versions above (Flycast 2.7 and later run them correctly between double operations).
#define FNEG_S(val) \
    (-(val))

#define FABS_S(val) \
    __builtin_fabsf(val)

#define MUL_S(val1, val2) \
    ((val1) * (val2))

#define MUL_D(val1, val2) \
    ((val1) * (val2))

#define DIV_S(val1, val2) \
    ((val1) / (val2))

#define DIV_D(val1, val2) \
    ((val1) / (val2))

#define CVT_S_W(val) \
    ((float)((int32_t)(val)))

#define CVT_D_W(val) \
    ((double)((int32_t)(val)))

#define CVT_D_L(val) \
    ((double)((int64_t)(val)))

#define CVT_S_L(val) \
    ((float)((int64_t)(val)))

#define CVT_D_S(val) \
    ((double)(val))

#define CVT_S_D(val) \
    ((float)(val))

#define TRUNC_W_S(val) \
    ((int32_t)(val))

#define TRUNC_W_D(val) \
    ((int32_t)(val))

#define TRUNC_L_S(val) \
    ((int64_t)(val))

#define TRUNC_L_D(val) \
    ((int64_t)(val))

#define DEFAULT_ROUNDING_MODE 0

static inline int32_t do_cvt_w_s(float val) {
    return recomp_cop1_rounding_mode == 1 ? (int32_t)val : (int32_t)lrintf(val);
}

#define CVT_W_S(val) \
    do_cvt_w_s(val)

static inline int64_t do_cvt_l_s(float val) {
    return recomp_cop1_rounding_mode == 1 ? (int64_t)val : (int64_t)llrintf(val);
}

#define CVT_L_S(val) \
    do_cvt_l_s(val);

static inline int32_t do_cvt_w_d(double val) {
    return recomp_cop1_rounding_mode == 1 ? (int32_t)val : (int32_t)lrint(val);
}

#define CVT_W_D(val) \
    do_cvt_w_d(val)

static inline int64_t do_cvt_l_d(double val) {
    return recomp_cop1_rounding_mode == 1 ? (int64_t)val : (int64_t)llrint(val);
}

#define CVT_L_D(val) \
    do_cvt_l_d(val)

#define NAN_CHECK(val)

typedef union {
    double d;
    struct {
        float fl;
        float fh;
    };
    struct {
        uint32_t u32l;
        uint32_t u32h;
    };
    uint64_t u64;
} fpr;

_Static_assert(sizeof(double) == 8, "fpr needs 64-bit doubles: build with -m4-single, not -m4-single-only");

// codegen.regcache = "gpr+fpr": an even FPR used only as a single or a raw word, kept in a local.
typedef union {
    float fl;
    uint32_t u32l;
} fpr32;

// lwc1/swc1 straight to and from a float local. rdram holds host-order words, so a float load is the word.
#define MEM_F(offset, reg) \
    (*(float*)(RECOMP_RB + (gpr)(reg) + (gpr)(offset)))

typedef struct {
    // sp, v0-a3 and the callee-saved registers are synced at calls and returns (codegen.regcache).
    // They come first so each is one mov.l @(disp,Rn) from the context pointer, which reaches 60 bytes.
    gpr r29, r2,  r3,  r4,  r5,  r6,  r7,  r16,
        r17, r18, r19, r20, r21, r22, r23, r30,
        r31, r1,  r8,  r9,  r10, r11, r12, r13,
        r14, r15, r24, r25, r26, r27, r28, r0;
    // The generated code writes odd FPU registers as f_odd[(n - 1) * 2], the high word of the even
    // register below. It's an array over the registers rather than a pointer to them so that the
    // compiler sees those writes as writes to the context, which the generated functions take as
    // a restrict pointer.
    union {
        struct {
            fpr f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,
                f8,  f9,  f10, f11, f12, f13, f14, f15,
                f16, f17, f18, f19, f20, f21, f22, f23,
                f24, f25, f26, f27, f28, f29, f30, f31;
        };
        struct {
            uint32_t f_odd_pad;
            uint32_t f_odd[63];
        };
    };
    gpr hi, lo;
    uint32_t status_reg;
    uint8_t mips3_float_mode;
    // codegen.abi_trace: a bit per GPR whose value the generator's analysis called dead (bit 0: hi/lo),
    // and where each was declared dead (a call's address, or a function's address + 1 for its entry).
    uint32_t taint;
    uint32_t taint_origin[32];
} recomp_context;

#define CHECK_FR(ctx, idx)

// codegen.abi_poison: junk in the caller-saved registers the generator's analysis says nothing reads
// (after calls and at function entry), to find code that reads them anyway.
#define RECOMP_POISON_REG(ctx, mask, tag, n) \
    if ((mask) & (1u << (n))) { (ctx)->r##n = (tag) | (n); }

#define RECOMP_POISON(ctx, mask, tag) do { \
    RECOMP_POISON_REG(ctx, mask, tag, 1) RECOMP_POISON_REG(ctx, mask, tag, 2) RECOMP_POISON_REG(ctx, mask, tag, 3) \
    RECOMP_POISON_REG(ctx, mask, tag, 4) RECOMP_POISON_REG(ctx, mask, tag, 5) RECOMP_POISON_REG(ctx, mask, tag, 6) \
    RECOMP_POISON_REG(ctx, mask, tag, 7) RECOMP_POISON_REG(ctx, mask, tag, 8) RECOMP_POISON_REG(ctx, mask, tag, 9) \
    RECOMP_POISON_REG(ctx, mask, tag, 10) RECOMP_POISON_REG(ctx, mask, tag, 11) RECOMP_POISON_REG(ctx, mask, tag, 12) \
    RECOMP_POISON_REG(ctx, mask, tag, 13) RECOMP_POISON_REG(ctx, mask, tag, 14) RECOMP_POISON_REG(ctx, mask, tag, 15) \
    RECOMP_POISON_REG(ctx, mask, tag, 24) RECOMP_POISON_REG(ctx, mask, tag, 25) \
} while (0)

#ifdef __cplusplus
extern "C" {
#endif

// codegen.abi_trace: the registers the analysis calls dead are tainted instead of overwritten, the taint
// follows the values through registers (not memory), and the first store, branch, jump, address, FPU
// move or runtime call argument at each instruction that uses a tainted value is printed (trace.c).
void trace_set(recomp_context* ctx, uint32_t mask, uint32_t origin);
void trace_spread(recomp_context* ctx, uint32_t from, uint32_t to);
void trace_hit(recomp_context* ctx, uint32_t regs, uint32_t vram, int kind);

#define TRACE_SET(ctx, mask, origin) trace_set(ctx, mask, origin)
#define TRACE_ALU(ctx, use, def) do { \
    uint32_t t_ = (ctx)->taint & (uint32_t)(use); \
    (ctx)->taint &= ~(uint32_t)(def); \
    if (__builtin_expect(t_ != 0, 0)) { trace_spread(ctx, t_, def); } \
} while (0)
#define TRACE_LOAD(ctx, base, use, def, vram) do { \
    uint32_t t_ = (ctx)->taint & (uint32_t)(use); \
    (ctx)->taint &= ~(uint32_t)(def); \
    if (__builtin_expect(t_ != 0, 0)) { \
        if (t_ & (uint32_t)(base)) { trace_hit(ctx, t_ & (uint32_t)(base), vram, 'L'); } \
        trace_spread(ctx, t_, def); \
    } \
} while (0)
#define TRACE_SINK(ctx, use, vram, kind) do { \
    uint32_t t_ = (ctx)->taint & (uint32_t)(use); \
    if (__builtin_expect(t_ != 0, 0)) { trace_hit(ctx, t_, vram, kind); } \
} while (0)

void cop0_status_write(recomp_context* ctx, gpr value);
gpr cop0_status_read(recomp_context* ctx);
void switch_error(const char* func, uint32_t vram, uint32_t jtbl);
void do_break(uint32_t vram);

typedef void (recomp_func_t)(uint8_t* rdram, recomp_context* ctx);
typedef void (recomp_func_ext_t)(uint8_t* rdram, recomp_context* ctx, uintptr_t arg);

recomp_func_t* get_function(int32_t vram);

#define LOOKUP_FUNC(val) \
    get_function((int32_t)(val))

// codegen.lookup_cache: indirect calls into the main code segment, which stays loaded, go through a small
// direct-mapped cache in front of get_function(); overlay addresses (0x80100000 up), whose functions change with
// what's loaded there, always ask get_function().
typedef struct {
    uint32_t vram;
    recomp_func_t* func;
} recomp_lookup_entry;
__attribute__((weak)) recomp_lookup_entry recomp_lookup_cache[256];

static __attribute__((noinline, unused)) recomp_func_t* lookup_func_miss(uint32_t vram) {
    recomp_func_t* func = get_function((int32_t)vram);
    if (vram < 0x80100000u) {
        recomp_lookup_entry* e = &recomp_lookup_cache[(vram >> 2) & 255];
        e->vram = vram;
        e->func = func;
    }
    return func;
}

static inline recomp_func_t* lookup_func_cached(uint32_t vram) {
    recomp_lookup_entry* e = &recomp_lookup_cache[(vram >> 2) & 255];
    if (__builtin_expect(e->vram == vram, 1)) {
        return e->func;
    }
    return lookup_func_miss(vram);
}

#define LOOKUP_FUNC_CACHED(val) \
    lookup_func_cached((uint32_t)(val))

extern int32_t* section_addresses;

#define LO16(x) \
    ((x) & 0xFFFF)

#define HI16(x) \
    (((x) >> 16) + (((x) >> 15) & 1))

#define RELOC_HI16(section_index, offset) \
    HI16(section_addresses[section_index] + (offset))

#define RELOC_LO16(section_index, offset) \
    LO16(section_addresses[section_index] + (offset))

void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram);

void pause_self(uint8_t *rdram);

void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);

// Replaces the game's initPFSPaks (pak.c).
void pak_check(uint8_t* rdram, recomp_context* ctx);

// Replaces the game's floor test of a point against a shape, func_8000F238 (collision.c).
void floor_shape_test(uint8_t* rdram, recomp_context* ctx);

// In the main loop once it has initialized the game again: after a soft reset, the title's game
// mode instead of the intro's (cont.c).
void soft_reset_reinit(uint8_t* rdram, recomp_context* ctx);
// Before it does: after a soft reset, stops the music the way the ending does (cont.c).
void soft_reset_stop_music(uint8_t* rdram, recomp_context* ctx);

// Replace libultra's __sinf and __cosf with the same double-precision steps in native code (sincos.c).
void ultra_sinf(uint8_t* rdram, recomp_context* ctx);
void ultra_cosf(uint8_t* rdram, recomp_context* ctx);

// In alAudioFrame after the players' handlers: plays the voices' updates on the AICA (aica.c).
void aica_audio_frame(uint8_t* rdram, recomp_context* ctx);

// A sound effect is about to start, with the id and the volume it actually plays at. Impacts rumble
// the jump pack (cont.c).
void sfx_rumble(uint8_t* rdram, int id, int volume);

#ifdef __cplusplus
}
#endif

#endif
