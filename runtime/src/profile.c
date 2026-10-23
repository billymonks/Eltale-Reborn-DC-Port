// Sampling profiler: TMU1 interrupts about 1000 times a second and counts the interrupted program
// counter in 32-byte buckets over the program's code. profile_dump prints the non-empty buckets as
// "[q64dc] prof <address> <count>" lines; scripts/profile.py maps them to functions.
// TMU1 is otherwise only used by timer_spin_sleep, which nothing calls after startup.

#include <kos.h>
#include <stdio.h>
#include <stdlib.h>

#include "profile.h"
#include "ultra.h"

#define PROFILE_HZ   1000
#define BUCKET_SHIFT 5

#define TEXT_START ((uint32_t)&_executable_start)
#define TEXT_END   ((uint32_t)&_etext)

static uint16_t* buckets;
static uint32_t bucket_count;
static uint32_t outside;
static uint32_t samples;
static volatile bool running;
static uint32_t start_vi, start_swaps;
static uint32_t start_misses, start_reads, start_read_ms;

bool profile_running(void) {
    return running;
}

static void profile_irq(irq_t code, irq_context_t* context, void* data) {
    (void)code;
    (void)data;
    uint32_t offset = context->pc - TEXT_START;
    uint32_t index = offset >> BUCKET_SHIFT;
    if (index < bucket_count) {
        if (buckets[index] != 0xFFFF) buckets[index]++;
    } else {
        outside++;
    }
    samples++;
    timer_clear(TMU1);
}

void profile_start(void) {
    if (!buckets) {
        bucket_count = ((TEXT_END - TEXT_START) >> BUCKET_SHIFT) + 1;
        buckets = calloc(bucket_count, sizeof(uint16_t));
        if (!buckets) {
            printf("[q64dc] profile: no memory\n");
            return;
        }
    }
    for (uint32_t i = 0; i < bucket_count; i++) buckets[i] = 0;
    outside = 0;
    samples = 0;
    irq_set_handler(EXC_TMU1_TUNI1, profile_irq, NULL);
    timer_prime(TMU1, PROFILE_HZ, 1);
    timer_clear(TMU1);
    timer_start(TMU1);
    start_vi = stat_vi_count;
    start_swaps = stat_swap_count;
    rom_totals(&start_misses, &start_reads, &start_read_ms);
    running = true;
    printf("[q64dc] profile: started\n");
}

void profile_dump(void) {
    if (!running) return;
    timer_stop(TMU1);
    timer_disable_ints(TMU1);
    irq_set_handler(EXC_TMU1_TUNI1, NULL, NULL);
    running = false;
    printf("[q64dc] profile: %lu samples, %lu outside the image; %lu retraces, %lu swaps\n",
           (unsigned long)samples, (unsigned long)outside, (unsigned long)(stat_vi_count - start_vi),
           (unsigned long)(stat_swap_count - start_swaps));
    uint32_t misses, reads, read_ms;
    rom_totals(&misses, &reads, &read_ms);
    printf("[q64dc] profile: ROM %lu blocks in %lu reads, %lu ms\n",
           (unsigned long)(misses - start_misses), (unsigned long)(reads - start_reads),
           (unsigned long)(read_ms - start_read_ms));
    for (uint32_t i = 0; i < bucket_count; i++) {
        if (buckets[i]) {
            printf("[q64dc] prof %08lx %u\n",
                   (unsigned long)(TEXT_START + (i << BUCKET_SHIFT)), buckets[i]);
        }
    }
    printf("[q64dc] profile: end\n");
}
