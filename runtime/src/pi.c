// Parallel interface: cartridge DMA, served from the ROM file.

#include <stdio.h>

#include "ultra.h"

#define OS_READ  0
#define OS_WRITE 1

// OSPiHandle fields.
#define PIHANDLE_TYPE        0x04
#define PIHANDLE_BASEADDRESS 0x0C

void osCreatePiManager_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
}

void osCartRomInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t handle = OS_CART_HANDLE;
    for (uint32_t i = 0; i < 0x74; i += 4) {
        MEM_W(i, handle) = 0;
    }
    MEM_BU(PIHANDLE_TYPE, handle) = 0;
    MEM_W(PIHANDLE_BASEADDRESS, handle) = (int32_t)0xB0000000u;
    ctx->r2 = handle;
}

void osPiStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t mb = ctx->r4;
    uint32_t direction = ctx->r6;
    uint32_t dev_addr = ctx->r7;
    uint32_t dram_addr = STACK_ARG(ctx, 4);
    uint32_t size = STACK_ARG(ctx, 5);
    uint32_t mq = STACK_ARG(ctx, 6);

    // Cartridge addresses may come as ROM offsets or as PI bus addresses (0x10000000 + offset),
    // and RDRAM addresses as physical or KSEG0.
    uint32_t rom_addr = dev_addr & 0x0FFFFFFFu;
    if (dram_addr < 0x80000000u) {
        dram_addr |= 0x80000000u;
    }
    dram_addr &= 0x9FFFFFFFu;

    if (direction == OS_READ) {
        rom_dma_to_rdram(rom_addr, dram_addr, size);
        stat_dma_count++;
        stat_dma_bytes += size;
    } else {
        printf("[q64dc] ignored PI write: 0x%X bytes to cart 0x%08X\n", (unsigned)size, (unsigned)dev_addr);
    }

    // OSIoMesg, as osPiStartDma fills it in.
    MEM_BU(0x02, mb) = (uint8_t)ctx->r5;
    MEM_W(0x04, mb) = (int32_t)mq;
    MEM_W(0x08, mb) = (int32_t)dram_addr;
    MEM_W(0x0C, mb) = (int32_t)dev_addr;
    MEM_W(0x10, mb) = (int32_t)size;

    if (mq != 0) {
        mq_send_nonblock(mq, mb);
    }
    ctx->r2 = 0;
}
