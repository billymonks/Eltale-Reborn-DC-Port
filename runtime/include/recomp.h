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
#define RDRAM_PTR(addr) \
    (rdram + (uint32_t)((uint32_t)(addr) - 0x80000000u))

#define MEM_W(offset, reg) \
    (*(int32_t*)RDRAM_PTR((reg) + (offset)))

#define MEM_WU(offset, reg) \
    (*(uint32_t*)RDRAM_PTR((reg) + (offset)))

#define MEM_H(offset, reg) \
    (*(int16_t*)RDRAM_PTR(((reg) + (offset)) ^ 2))

#define MEM_B(offset, reg) \
    (*(int8_t*)RDRAM_PTR(((reg) + (offset)) ^ 3))

#define MEM_HU(offset, reg) \
    (*(uint16_t*)RDRAM_PTR(((reg) + (offset)) ^ 2))

#define MEM_BU(offset, reg) \
    (*(uint8_t*)RDRAM_PTR(((reg) + (offset)) ^ 3))

// Doubleword accesses only come from ldc1/sdc1 (FPU doubles); 64-bit GPR loads/stores are rejected.
#define SD(val, offset, reg) { \
    *(uint32_t*)RDRAM_PTR((reg) + (offset) + 4) = (uint32_t)((uint64_t)(val) >> 0); \
    *(uint32_t*)RDRAM_PTR((reg) + (offset) + 0) = (uint32_t)((uint64_t)(val) >> 32); \
}

static inline uint64_t load_doubleword(uint8_t* rdram, gpr reg, gpr offset) {
    uint64_t lo = (uint64_t)MEM_WU(reg, offset + 4);
    uint64_t hi = (uint64_t)MEM_WU(reg, offset + 0);
    return lo | (hi << 32);
}

#define LD(offset, reg) \
    load_doubleword(rdram, offset, reg)

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

typedef struct {
    gpr r0,  r1,  r2,  r3,  r4,  r5,  r6,  r7,
        r8,  r9,  r10, r11, r12, r13, r14, r15,
        r16, r17, r18, r19, r20, r21, r22, r23,
        r24, r25, r26, r27, r28, r29, r30, r31;
    fpr f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,
        f8,  f9,  f10, f11, f12, f13, f14, f15,
        f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31;
    gpr hi, lo;
    uint32_t* f_odd;
    uint32_t status_reg;
    uint8_t mips3_float_mode;
} recomp_context;

#define CHECK_FR(ctx, idx)

#ifdef __cplusplus
extern "C" {
#endif

void cop0_status_write(recomp_context* ctx, gpr value);
gpr cop0_status_read(recomp_context* ctx);
void switch_error(const char* func, uint32_t vram, uint32_t jtbl);
void do_break(uint32_t vram);

typedef void (recomp_func_t)(uint8_t* rdram, recomp_context* ctx);
typedef void (recomp_func_ext_t)(uint8_t* rdram, recomp_context* ctx, uintptr_t arg);

recomp_func_t* get_function(int32_t vram);

#define LOOKUP_FUNC(val) \
    get_function((int32_t)(val))

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

#ifdef __cplusplus
}
#endif

#endif
