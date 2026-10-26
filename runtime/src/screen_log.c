#include <kos.h>
#include <arch/irq.h>
#include <arch/memory.h>
#include <dc/fb_console.h>
#include <stdio.h>

#include "screen_log.h"

#ifdef SCREEN_LOG

static const char* last_mark = "(none)";
static volatile int in_exc;

static void halt(void) {
    for (;;) {
    }
}

// dbgio only. pvr_shutdown and printf take a mutex, and this runs inside the
// exception, which is what produced "mutex_lock: called inside an interrupt".
static void on_exc(irq_t code, irq_context_t* ctx, void* data) {
    (void)data;
    if (in_exc) {
        halt();
    }
    in_exc = 1;
    uint32_t tea = *(volatile uint32_t*)SH4_REG_MMU_TEA;
    dbgio_printf("\nEXC %04x\nPC %08lx\nPR %08lx\nSP %08lx\nTEA %08lx\nlast: %s\n",
                 (unsigned)code, (unsigned long)ctx->pc, (unsigned long)ctx->pr,
                 (unsigned long)ctx->r[15], (unsigned long)tea, last_mark);
    halt();
}

void screen_log_init(void) {
    static const irq_t codes[] = {
        EXC_INSTR_ADDRESS,
        EXC_ILLEGAL_INSTR,
        EXC_SLOT_ILLEGAL_INSTR,
        EXC_GENERAL_FPU,
        EXC_SLOT_FPU,
        EXC_DATA_ADDRESS_READ,
        EXC_DATA_ADDRESS_WRITE,
        EXC_FPU,
        EXC_USER_BREAK_PRE,
    };
    dbgio_dev_select("fb");
    dbgio_enable();
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        irq_set_handler(codes[i], on_exc, NULL);
    }
    printf("[q64dc] screen log\n");
}

void screen_mark(const char* msg) {
    last_mark = msg;
    printf("[q64dc] %s\n", msg);
}

void screen_log_pvr(int up) {
    (void)up;
}

#endif
