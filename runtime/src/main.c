#include <kos.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "screen_log.h"
#include "test.h"
#include "ultra.h"
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
#ifdef SCREEN_LOG
    for (;;) {
    }
#else
    exit(1);
#endif
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

gpr cop0_status_read(recomp_context* ctx) {
    return ctx->status_reg;
}

void cop0_status_write(recomp_context* ctx, gpr value) {
    ctx->status_reg = value;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    screen_log_init();
    printf("[q64dc] Quest 64 Dreamcast runtime\n");

    screen_mark("rom open");
    if (!rom_open()) {
        runtime_fatal("[q64dc] couldn't find the Quest 64 ROM");
    }
    screen_mark("rom ready");

    rdram = memalign(32, RDRAM_SIZE + RUNTIME_RAM_SIZE + MOD_RAM_SIZE);
    if (rdram == NULL) {
        runtime_fatal("[q64dc] couldn't allocate %d bytes of RDRAM", RDRAM_SIZE + RUNTIME_RAM_SIZE + MOD_RAM_SIZE);
    }
    memset(rdram, 0, RDRAM_SIZE + RUNTIME_RAM_SIZE + MOD_RAM_SIZE);
    screen_mark("rdram");

    // The N64 boot code copies the first megabyte after the header to the entrypoint.
    rom_read_to_rdram(0x1000, 0x80000400, 0x100000);
    MEM_W(0, 0x80000318u) = RDRAM_SIZE;
    screen_mark("boot copy");
#if MOD_RAM_SIZE
    mods_init();
    screen_mark("mods");
#endif

    function_lookup_init();
    test_init();
    pak_init();
    screen_mark("gfx init");
    gfx_init();
    screen_mark("audio init");
    audio_init();
    screen_mark("sched");
    sched_init();
    screen_mark("events");
    events_init();

    // The boot code runs on this thread until it starts the first game thread, which never
    // returns control here.
    static recomp_context ctx;
    memset(&ctx, 0, sizeof(ctx));

    screen_mark("enter game");
    printf("[q64dc] entering the game at 0x%08X\n", (unsigned)get_entrypoint_address());
    recomp_entrypoint(rdram, &ctx);

    runtime_fatal("[q64dc] the game returned from its entrypoint");
}
