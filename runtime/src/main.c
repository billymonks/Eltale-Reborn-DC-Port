#include <kos.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "runtime.h"
#include "funcs.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

uint8_t* rdram;
uint32_t recomp_cop1_rounding_mode;

void runtime_fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
    exit(1);
}

void runtime_unimplemented(const char* name) {
    runtime_fatal("[q64dc] unimplemented runtime function: %s", name);
}

void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    runtime_fatal("[q64dc] jump table miss in %s at 0x%08X (table 0x%08X)", func, vram, jtbl);
}

void do_break(uint32_t vram) {
    runtime_fatal("[q64dc] break instruction at 0x%08X", vram);
}

void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
    (void)rdram;
    (void)ctx;
    runtime_fatal("[q64dc] syscall at 0x%08X", (uint32_t)instruction_vram);
}

void pause_self(uint8_t* rdram) {
    (void)rdram;
    runtime_unimplemented("pause_self");
}

gpr cop0_status_read(recomp_context* ctx) {
    return ctx->status_reg;
}

void cop0_status_write(recomp_context* ctx, gpr value) {
    ctx->status_reg = value;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printf("[q64dc] Quest 64 Dreamcast runtime\n");

    if (!rom_open()) {
        runtime_fatal("[q64dc] couldn't find the Quest 64 ROM");
    }

    rdram = memalign(32, RDRAM_SIZE);
    if (rdram == NULL) {
        runtime_fatal("[q64dc] couldn't allocate %d bytes of RDRAM", RDRAM_SIZE);
    }
    memset(rdram, 0, RDRAM_SIZE);

    // The N64 boot code copies the first megabyte after the header to the entrypoint.
    rom_read_to_rdram(0x1000, 0x80000400, 0x100000);
    // osMemSize, read by osInitialize.
    MEM_W(0, 0x80000318u) = RDRAM_SIZE;

    function_lookup_init();

    static recomp_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.f_odd = &ctx.f0.u32h;

    printf("[q64dc] entering the game at 0x%08X\n", (unsigned)get_entrypoint_address());
    recomp_entrypoint(rdram, &ctx);

    runtime_fatal("[q64dc] the game returned from its entrypoint");
}
