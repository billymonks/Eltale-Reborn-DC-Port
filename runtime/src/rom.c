#include <stdio.h>
#include <string.h>

#include "runtime.h"

// Searched in order: the game disc, the dcload host filesystem, an SD card adapter.
static const char* const rom_paths[] = {
    "/cd/quest64.z64",
    "/pc/quest64.us.z64",
    "/sd/quest64.z64",
};

static FILE* rom_file;
static uint32_t rom_file_size;

bool rom_open(void) {
    for (size_t i = 0; i < sizeof(rom_paths) / sizeof(rom_paths[0]); i++) {
        FILE* f = fopen(rom_paths[i], "rb");
        if (f == NULL) {
            continue;
        }
        uint8_t header[4];
        if (fread(header, 1, sizeof(header), f) != sizeof(header) || header[0] != 0x80 || header[1] != 0x37) {
            printf("[q64dc] %s isn't a big-endian (.z64) N64 ROM\n", rom_paths[i]);
            fclose(f);
            continue;
        }
        fseek(f, 0, SEEK_END);
        rom_file_size = (uint32_t)ftell(f);
        rom_file = f;
        printf("[q64dc] ROM: %s (%u bytes)\n", rom_paths[i], (unsigned)rom_file_size);
        return true;
    }
    return false;
}

uint32_t rom_size(void) {
    return rom_file_size;
}

void rom_read_to_rdram(uint32_t rom_addr, uint32_t ram_addr, uint32_t size) {
    if ((rom_addr | ram_addr | size) & 3) {
        runtime_fatal("[q64dc] unaligned ROM read: rom 0x%08X ram 0x%08X size 0x%X", rom_addr, ram_addr, size);
    }
    if (ram_addr < 0x80000000u || ram_addr - 0x80000000u + size > RDRAM_SIZE) {
        runtime_fatal("[q64dc] ROM read outside RDRAM: ram 0x%08X size 0x%X", ram_addr, size);
    }

    uint8_t* dst = rdram + (ram_addr - 0x80000000u);
    fseek(rom_file, (long)rom_addr, SEEK_SET);
    size_t got = fread(dst, 1, size, rom_file);
    if (got < size) {
        memset(dst + got, 0, size - got);
    }

    uint32_t* words = (uint32_t*)dst;
    for (uint32_t i = 0; i < size / 4; i++) {
        words[i] = __builtin_bswap32(words[i]);
    }
}
