#include <kos.h>
#include <malloc.h>
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

static file_t rom_file = FILEHND_INVALID;
static uint32_t rom_file_size;
static uint8_t* cache_data;
static CacheBlock cache_blocks[CACHE_BLOCK_COUNT];
static uint32_t cache_clock;

static uint32_t stat_hits, stat_misses, stat_read_us, stat_max_read_us;
static uint32_t total_misses;
static uint64_t total_read_us;

// Prints the cache's activity since the last call, if there was any.
void rom_print_stats(void) {
    if (stat_hits == 0 && stat_misses == 0) {
        return;
    }
    total_misses += stat_misses;
    total_read_us += stat_read_us;
    printf("[q64dc] ROM cache: %u hits, %u misses, %u ms reading (longest block %u ms); %u misses, %u ms since boot\n",
           (unsigned)stat_hits, (unsigned)stat_misses, (unsigned)(stat_read_us / 1000),
           (unsigned)(stat_max_read_us / 1000), (unsigned)total_misses, (unsigned)(total_read_us / 1000));
    stat_hits = stat_misses = stat_read_us = stat_max_read_us = 0;
}

bool rom_open(void) {
    for (size_t i = 0; i < sizeof(rom_paths) / sizeof(rom_paths[0]); i++) {
        file_t f = fs_open(rom_paths[i], O_RDONLY);
        if (f == FILEHND_INVALID) {
            continue;
        }
        uint8_t header[4];
        if (fs_read(f, header, sizeof(header)) != sizeof(header) || header[0] != 0x80 || header[1] != 0x37) {
            printf("[q64dc] %s isn't a big-endian (.z64) N64 ROM\n", rom_paths[i]);
            fs_close(f);
            continue;
        }
        rom_file_size = (uint32_t)fs_total(f);
        rom_file = f;
        printf("[q64dc] ROM: %s (%u bytes)\n", rom_paths[i], (unsigned)rom_file_size);

        // Sector-aligned reads into 32-byte aligned buffers go to the drive as one DMA transfer;
        // anything else is read a sector at a time.
        cache_data = memalign(32, CACHE_BLOCK_SIZE * CACHE_BLOCK_COUNT);
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
            stat_hits++;
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
    uint64_t t0 = timer_us_gettime64();
    if (offset < rom_file_size) {
        fs_seek(rom_file, (off_t)offset, SEEK_SET);
        ssize_t n = fs_read(rom_file, data, CACHE_BLOCK_SIZE);
        got = n > 0 ? (size_t)n : 0;
    }
    uint32_t us = (uint32_t)(timer_us_gettime64() - t0);
    stat_misses++;
    stat_read_us += us;
    if (us > stat_max_read_us) {
        stat_max_read_us = us;
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
