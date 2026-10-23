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

// The ROM doesn't fit in RAM, so it's read from the file in 32 KB blocks kept in an LRU cache.
// A miss reads up to READ_RUN blocks in one disc command: the game reads its files front to back
// in 16 KB pieces, so the blocks after a miss are usually next.
// For comparisons, RT_DEFINES can set LRU_BLOCKS and READ_RUN, ROM_NO_PIN skips the audio data
// below, and ROM_SYNC reads PI DMAs on the game thread.
#define BLOCK_SHIFT 15
#define BLOCK_SIZE  (1u << BLOCK_SHIFT)
#ifndef LRU_BLOCKS
#define LRU_BLOCKS  96
#endif
#ifndef READ_RUN
#define READ_RUN    4
#endif
#define MAX_BLOCKS  1024

// Quest 64's audio banks, sample tables and sequences (Quest64Syms: .instrument_ctl to the end of
// .SE_tbl). The audio thread reads samples from them in 0x500-byte DMAs every frame and doesn't
// wait for them before its task runs, so they're read at boot and kept.
#define PIN_START   0x00E7E800u
#define PIN_END     0x00F94380u
#define PIN_FIRST   (PIN_START >> BLOCK_SHIFT)
#define PIN_BLOCKS  (((PIN_END - 1) >> BLOCK_SHIFT) - PIN_FIRST + 1)
#define SLOT_COUNT  (LRU_BLOCKS + PIN_BLOCKS)

typedef struct {
    uint32_t rom_block;
    uint32_t last_use;
    bool valid;
} Slot;

static file_t rom_file = FILEHND_INVALID;
static uint32_t rom_file_size;
static uint8_t* cache_data;
static uint8_t* staging;
static Slot slots[SLOT_COUNT];
static int16_t slot_of[MAX_BLOCKS];  // -1: not cached
static uint32_t cache_clock;

// Guards the cache and the request queue. The disc is only read by one thread at a time: the
// boot thread before the game starts, then the reader thread.
static mutex_t lock = MUTEX_INITIALIZER;
static condvar_t done_cv = COND_INITIALIZER;

bool rom_trace;
uint32_t rom_trace_tag;

static uint32_t stat_hits, stat_misses, stat_reads, stat_read_us, stat_max_read_us;
static uint32_t stat_sync, stat_async;
static uint32_t total_misses, total_reads;
static uint64_t total_read_us;

void rom_totals(uint32_t* misses, uint32_t* reads, uint32_t* read_ms) {
    mutex_lock(&lock);
    *misses = total_misses + stat_misses;
    *reads = total_reads + stat_reads;
    *read_ms = (uint32_t)((total_read_us + stat_read_us) / 1000);
    mutex_unlock(&lock);
}

// Prints the cache's activity since the last call, if there was any.
void rom_print_stats(void) {
    mutex_lock(&lock);
    uint32_t hits = stat_hits, misses = stat_misses, reads = stat_reads, read_us = stat_read_us;
    uint32_t max_read_us = stat_max_read_us, sync = stat_sync, async = stat_async;
    stat_hits = stat_misses = stat_reads = stat_read_us = stat_max_read_us = 0;
    stat_sync = stat_async = 0;
    total_misses += misses;
    total_reads += reads;
    total_read_us += read_us;
    uint32_t all_misses = total_misses, all_ms = (uint32_t)(total_read_us / 1000);
    mutex_unlock(&lock);
    if (hits == 0 && misses == 0) {
        return;
    }
    printf("[q64dc] ROM cache: %u hits, %u misses in %u reads, %u ms reading (longest read %u ms); "
           "DMAs %u at once, %u by the reader; %u misses, %u ms since boot\n",
           (unsigned)hits, (unsigned)misses, (unsigned)reads, (unsigned)(read_us / 1000),
           (unsigned)(max_read_us / 1000), (unsigned)sync, (unsigned)async, (unsigned)all_misses,
           (unsigned)all_ms);
}

static uint8_t* slot_data(int slot) {
    return cache_data + (uint32_t)slot * BLOCK_SIZE;
}

// Reads blocks [first, first + count) into dst; past the end of the file is zeros. Called without
// the lock.
static void disc_read(uint32_t first, uint32_t count, uint8_t* dst) {
    uint32_t offset = first << BLOCK_SHIFT;
    uint32_t bytes = count << BLOCK_SHIFT;
    size_t got = 0;
    uint64_t t0 = timer_us_gettime64();
    if (offset < rom_file_size) {
        fs_seek(rom_file, (off_t)offset, SEEK_SET);
        ssize_t n = fs_read(rom_file, dst, bytes);
        got = n > 0 ? (size_t)n : 0;
    }
    if (got < bytes) {
        memset(dst + got, 0, bytes - got);
    }
    uint32_t us = (uint32_t)(timer_us_gettime64() - t0);

    mutex_lock(&lock);
    stat_reads++;
    stat_misses += count;
    stat_read_us += us;
    if (us > stat_max_read_us) {
        stat_max_read_us = us;
    }
    mutex_unlock(&lock);
    if (rom_trace) {
        printf("[q64dc] rom read: blocks %u-%u (0x%06X) for queue %08X, %u us, t %u ms\n", (unsigned)first,
               (unsigned)(first + count - 1), (unsigned)offset, (unsigned)rom_trace_tag, (unsigned)us,
               (unsigned)(t0 / 1000));
    }
}

static int lru_victim_locked(void) {
    int victim = 0;
    for (int i = 0; i < LRU_BLOCKS; i++) {
        if (!slots[i].valid) {
            return i;
        }
        if (slots[i].last_use < slots[victim].last_use) {
            victim = i;
        }
    }
    return victim;
}

// Brings a block into the cache, with the uncached blocks after it up to a run of READ_RUN.
static void fetch(uint32_t block) {
    mutex_lock(&lock);
    if (slot_of[block] >= 0) {
        mutex_unlock(&lock);
        return;
    }
    uint32_t count = 1;
    while (count < READ_RUN && block + count < MAX_BLOCKS && slot_of[block + count] < 0 &&
           ((block + count) << BLOCK_SHIFT) < rom_file_size) {
        count++;
    }
    mutex_unlock(&lock);

    disc_read(block, count, staging);

    mutex_lock(&lock);
    for (uint32_t i = 0; i < count; i++) {
        if (slot_of[block + i] >= 0) {
            continue;
        }
        int s = lru_victim_locked();
        if (slots[s].valid) {
            slot_of[slots[s].rom_block] = -1;
        }
        memcpy(slot_data(s), staging + (i << BLOCK_SHIFT), BLOCK_SIZE);
        slots[s].rom_block = block + i;
        slots[s].valid = true;
        slots[s].last_use = ++cache_clock;
        slot_of[block + i] = (int16_t)s;
    }
    mutex_unlock(&lock);
}

static bool cached_locked(uint32_t rom_addr, uint32_t size) {
    if (size == 0) {
        return true;
    }
    for (uint32_t b = rom_addr >> BLOCK_SHIFT; b <= (rom_addr + size - 1) >> BLOCK_SHIFT; b++) {
        if (slot_of[b] < 0) {
            return false;
        }
    }
    return true;
}

// Copies cached ROM to rdram: big-endian ROM words to rdram's host-order words.
static void copy_locked(uint32_t rom_addr, uint32_t rdram_addr, uint32_t size) {
    while (size > 0) {
        uint32_t block = rom_addr >> BLOCK_SHIFT;
        uint32_t in_block = rom_addr & (BLOCK_SIZE - 1);
        uint32_t chunk = BLOCK_SIZE - in_block;
        if (chunk > size) {
            chunk = size;
        }
        int s = slot_of[block];
        slots[s].last_use = ++cache_clock;
        stat_hits++;
        const uint8_t* src = slot_data(s) + in_block;
        if (((rdram_addr | chunk) & 3) == 0) {
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

static void check_range(uint32_t rom_addr, uint32_t rdram_addr, uint32_t size) {
    if (rdram_addr < 0x80000000u || rdram_addr - 0x80000000u + size > RDRAM_SIZE + RUNTIME_RAM_SIZE) {
        runtime_fatal("[q64dc] ROM read outside RDRAM: rom 0x%08X ram 0x%08X size 0x%X", rom_addr, rdram_addr, size);
    }
    if ((uint64_t)rom_addr + size > (uint64_t)MAX_BLOCKS << BLOCK_SHIFT) {
        runtime_fatal("[q64dc] ROM read past 32 MB: rom 0x%08X size 0x%X", rom_addr, size);
    }
}

// Reads on the calling thread, a block at a time.
void rom_dma_to_rdram(uint32_t rom_addr, uint32_t rdram_addr, uint32_t size) {
    check_range(rom_addr, rdram_addr, size);
    while (size > 0) {
        uint32_t chunk = BLOCK_SIZE - (rom_addr & (BLOCK_SIZE - 1));
        if (chunk > size) {
            chunk = size;
        }
        for (;;) {
            mutex_lock(&lock);
            if (cached_locked(rom_addr, chunk)) {
                copy_locked(rom_addr, rdram_addr, chunk);
                mutex_unlock(&lock);
                break;
            }
            mutex_unlock(&lock);
            fetch(rom_addr >> BLOCK_SHIFT);
        }
        rom_addr += chunk;
        rdram_addr += chunk;
        size -= chunk;
    }
}

void rom_read_to_rdram(uint32_t rom_addr, uint32_t ram_addr, uint32_t size) {
    rom_dma_to_rdram(rom_addr, ram_addr, size);
}

// --- PI DMA ---
//
// As on the N64, osPiStartDma only starts a transfer, and the message arrives when it's done.
// A transfer whose blocks are all cached is copied at once, unless an earlier one still pending
// sends to the same queue or writes the same memory, which keeps each queue's messages and each
// byte's writes in order. Others wait in a queue for the reader thread, which runs below the game
// threads, so the disc is read while the game would wait anyway, and the game threads go on
// meanwhile (the KOS GD-ROM driver polls during commands but sleeps during the transfer).
#define QUEUE_SIZE   64
#define READER_PRIO  (PRIO_DEFAULT + 1)

typedef struct {
    uint32_t rom, dram, size, mq, mb;
} Request;

static Request queue[QUEUE_SIZE];
static uint32_t queue_head, queue_tail;  // pending: [tail, head); the one at tail is in progress
static semaphore_t reader_sem;

static void* reader_thread(void* param) {
    (void)param;
    for (;;) {
        sem_wait(&reader_sem);
        mutex_lock(&lock);
        Request r = queue[queue_tail % QUEUE_SIZE];
        mutex_unlock(&lock);

        rom_trace_tag = r.mq;
        rom_dma_to_rdram(r.rom, r.dram, r.size);

        // Sent while the request is still pending, so no later transfer to the queue can pass it.
        if (r.mq != 0) {
            mq_send_nonblock(r.mq, r.mb);
        }
        mutex_lock(&lock);
        queue_tail++;
        cond_broadcast(&done_cv);
        mutex_unlock(&lock);
    }
    return NULL;
}

void rom_start_dma(uint32_t rom_addr, uint32_t rdram_addr, uint32_t size, uint32_t mq, uint32_t mb) {
    check_range(rom_addr, rdram_addr, size);
#ifdef ROM_SYNC
    rom_dma_to_rdram(rom_addr, rdram_addr, size);
    if (mq != 0) {
        mq_send_nonblock(mq, mb);
    }
    return;
#endif
    mutex_lock(&lock);
    bool wait = false;
    for (uint32_t i = queue_tail; i != queue_head && !wait; i++) {
        const Request* r = &queue[i % QUEUE_SIZE];
        wait = (mq != 0 && r->mq == mq) || (rdram_addr < r->dram + r->size && r->dram < rdram_addr + size);
    }
    if (!wait && cached_locked(rom_addr, size)) {
        copy_locked(rom_addr, rdram_addr, size);
        stat_sync++;
        mutex_unlock(&lock);
        if (mq != 0) {
            mq_send_nonblock(mq, mb);
        }
        return;
    }
    if (queue_head - queue_tail >= QUEUE_SIZE) {
        runtime_fatal("[q64dc] too many PI DMAs in flight");
    }
    queue[queue_head % QUEUE_SIZE] = (Request){rom_addr, rdram_addr, size, mq, mb};
    queue_head++;
    stat_async++;
    mutex_unlock(&lock);
    sem_signal(&reader_sem);
}

static bool audio_pending_locked(void) {
    for (uint32_t i = queue_tail; i != queue_head; i++) {
        const Request* r = &queue[i % QUEUE_SIZE];
        if (r->rom < PIN_END && r->rom + r->size > PIN_START) {
            return true;
        }
    }
    return false;
}

void rom_wait_audio(void) {
    mutex_lock(&lock);
    while (audio_pending_locked()) {
        cond_wait(&done_cv, &lock);
    }
    mutex_unlock(&lock);
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
        if (rom_file_size > MAX_BLOCKS << BLOCK_SHIFT) {
            runtime_fatal("[q64dc] the ROM is over 32 MB");
        }

        // Sector-aligned reads into 32-byte aligned buffers go to the drive as one DMA transfer;
        // anything else is read a sector at a time.
        cache_data = memalign(32, SLOT_COUNT * BLOCK_SIZE);
        staging = memalign(32, READ_RUN * BLOCK_SIZE);
        if (cache_data == NULL || staging == NULL) {
            runtime_fatal("[q64dc] couldn't allocate the ROM cache");
        }
        memset(slot_of, 0xFF, sizeof(slot_of));

#ifndef ROM_NO_PIN
        if (rom_file_size >= PIN_END) {
            uint64_t t0 = timer_us_gettime64();
            disc_read(PIN_FIRST, PIN_BLOCKS, slot_data(LRU_BLOCKS));
            for (uint32_t i = 0; i < PIN_BLOCKS; i++) {
                slots[LRU_BLOCKS + i].rom_block = PIN_FIRST + i;
                slots[LRU_BLOCKS + i].valid = true;
                slot_of[PIN_FIRST + i] = (int16_t)(LRU_BLOCKS + i);
            }
            printf("[q64dc] ROM: audio data (%u KB) read in %u ms\n", (unsigned)(PIN_BLOCKS * BLOCK_SIZE / 1024),
                   (unsigned)((timer_us_gettime64() - t0) / 1000));
        }
#endif

        sem_init(&reader_sem, 0);
        kthread_attr_t attr = {0};
        attr.stack_size = 16 * 1024;
        attr.prio = READER_PRIO;
        attr.label = "n64 pi";
        attr.create_detached = true;
        if (thd_create_ex(&attr, reader_thread, NULL) == NULL) {
            runtime_fatal("[q64dc] couldn't create the PI thread");
        }
        return true;
    }
    return false;
}

uint32_t rom_size(void) {
    return rom_file_size;
}
