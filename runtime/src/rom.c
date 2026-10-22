#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ultra.h"

// Searched in order: the game disc, the dcload host filesystem, an SD card adapter.
static const char* const rom_paths[] = {
    "/cd/quest64.z64",
    "/pc/quest64.us.z64",
    "/sd/quest64.z64",
};

// The ROM doesn't fit in RAM, so it's read from the file in blocks kept in a small LRU cache.
#define CACHE_BLOCK_SIZE  (32 * 1024)
#define CACHE_BLOCK_COUNT 48

typedef struct {
    uint32_t rom_block;
    uint32_t last_use;
    bool valid;
} CacheBlock;

static FILE* rom_file;
static uint32_t rom_file_size;
static uint8_t* cache_data;
static CacheBlock cache_blocks[CACHE_BLOCK_COUNT];
static uint32_t cache_clock;

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

        cache_data = malloc(CACHE_BLOCK_SIZE * CACHE_BLOCK_COUNT);
        if (cache_data == NULL) {
            runtime_fatal("[q64dc] couldn't allocate the ROM cache");
        }
        return true;
    }
    return false;
}

uint32_t rom_size(void) {
    return rom_file_size;
}

static const uint8_t* cache_get_block(uint32_t rom_block) {
    int victim = 0;
    for (int i = 0; i < CACHE_BLOCK_COUNT; i++) {
        if (cache_blocks[i].valid && cache_blocks[i].rom_block == rom_block) {
            cache_blocks[i].last_use = ++cache_clock;
            return cache_data + i * CACHE_BLOCK_SIZE;
        }
        if (!cache_blocks[i].valid) {
            victim = i;
        } else if (cache_blocks[victim].valid && cache_blocks[i].last_use < cache_blocks[victim].last_use) {
            victim = i;
        }
    }

    uint8_t* data = cache_data + victim * CACHE_BLOCK_SIZE;
    uint32_t offset = rom_block * CACHE_BLOCK_SIZE;
    size_t got = 0;
    if (offset < rom_file_size) {
        fseek(rom_file, (long)offset, SEEK_SET);
        got = fread(data, 1, CACHE_BLOCK_SIZE, rom_file);
    }
    if (got < CACHE_BLOCK_SIZE) {
        memset(data + got, 0, CACHE_BLOCK_SIZE - got);
    }
    cache_blocks[victim].rom_block = rom_block;
    cache_blocks[victim].valid = true;
    cache_blocks[victim].last_use = ++cache_clock;
    return data;
}

void rom_dma_to_rdram(uint32_t rom_addr, uint32_t rdram_addr, uint32_t size) {
    if (rdram_addr < 0x80000000u || rdram_addr - 0x80000000u + size > RDRAM_SIZE + RUNTIME_RAM_SIZE) {
        runtime_fatal("[q64dc] ROM read outside RDRAM: rom 0x%08X ram 0x%08X size 0x%X", rom_addr, rdram_addr, size);
    }

    while (size > 0) {
        uint32_t block = rom_addr / CACHE_BLOCK_SIZE;
        uint32_t in_block = rom_addr % CACHE_BLOCK_SIZE;
        uint32_t chunk = CACHE_BLOCK_SIZE - in_block;
        if (chunk > size) {
            chunk = size;
        }
        const uint8_t* src = cache_get_block(block) + in_block;

        if (((rdram_addr | chunk) & 3) == 0) {
            // Big-endian ROM words to rdram's host-order words.
            uint32_t* dst = (uint32_t*)RDRAM_PTR(rdram_addr);
            for (uint32_t i = 0; i < chunk / 4; i++) {
                dst[i] = ((uint32_t)src[4 * i] << 24) | ((uint32_t)src[4 * i + 1] << 16) |
                         ((uint32_t)src[4 * i + 2] << 8) | src[4 * i + 3];
            }
        } else {
            for (uint32_t i = 0; i < chunk; i++) {
                MEM_BU(0, rdram_addr + i) = src[i];
            }
        }

        rom_addr += chunk;
        rdram_addr += chunk;
        size -= chunk;
    }
}

void rom_read_to_rdram(uint32_t rom_addr, uint32_t ram_addr, uint32_t size) {
    rom_dma_to_rdram(rom_addr, ram_addr, size);
}
