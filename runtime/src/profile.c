// Sampling profiler: TMU1 interrupts about 1000 times a second and counts the interrupted program
// counter in 32-byte buckets over the program's code. profile_dump prints the non-empty buckets as
// "[q64dc] prof <address> <count>" lines; scripts/profile.py maps them to functions.
// TMU1 is otherwise only used by timer_spin_sleep, which nothing calls after startup.

#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#ifdef PERF_HUD
static uint32_t* func_addr;
static uint8_t* func_group;
static uint16_t* func_count;
static uint32_t func_n;
static uint32_t func_outside;
static volatile int func_phase;
static uint64_t func_t0;
static int func_page;
static char func_rows[16][16];
static int func_nrows;
static uint32_t func_table_bytes;
#ifdef GFX_DIAG
// Third page: the same samples by 32-byte bucket of code, for the hottest instructions.
#define FUNC_PAGES 3
static uint16_t* pc_count;
static uint32_t pc_buckets;
#else
#define FUNC_PAGES 2
#endif

static void func_sample(uint32_t pc) {
    if (!func_n || pc < func_addr[0] || pc >= func_addr[func_n]) {
        func_outside++;
        return;
    }
    uint32_t lo = 0, hi = func_n;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (func_addr[mid] <= pc) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    if (func_count[lo] != 0xFFFF) {
        func_count[lo]++;
    }
}
#endif

static void profile_irq(irq_t code, irq_context_t* context, void* data) {
    (void)code;
    (void)data;
#ifdef PERF_HUD
    if (func_phase == 1) {
        func_sample(context->pc);
#ifdef GFX_DIAG
        uint32_t bucket = (context->pc - TEXT_START) >> BUCKET_SHIFT;
        if (bucket < pc_buckets && pc_count[bucket] != 0xFFFF) {
            pc_count[bucket]++;
        }
#endif
        samples++;
        timer_clear(TMU1);
        return;
    }
#endif
    uint32_t offset = context->pc - TEXT_START;
    uint32_t index = offset >> BUCKET_SHIFT;
    if (buckets && index < bucket_count) {
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

#ifdef PERF_HUD
static file_t open_disc(const char* a, const char* b) {
    file_t f = fs_open(a, O_RDONLY);
    if (f == FILEHND_INVALID) {
        f = fs_open(b, O_RDONLY);
    }
    return f;
}

static int load_tables(void) {
    if (func_addr) {
        return 1;
    }
    file_t f = open_disc("/cd/profaddr.bin", "/cd/PROFADDR.BIN");
    if (f == FILEHND_INVALID) {
        return 0;
    }
    uint32_t n = 0;
    if (fs_read(f, &n, 4) != 4 || n == 0 || n > 20000) {
        fs_close(f);
        return 0;
    }
    func_addr = malloc((n + 1) * 4);
    func_group = malloc(n);
    func_count = calloc(n, sizeof(uint16_t));
    if (!func_addr || !func_group || !func_count) {
        fs_close(f);
        return 0;
    }
    if (fs_read(f, func_addr, (n + 1) * 4) != (ssize_t)((n + 1) * 4) ||
        fs_read(f, func_group, n) != (ssize_t)n) {
        fs_close(f);
        return 0;
    }
    fs_close(f);
    func_n = n;
    func_table_bytes = 4 + (n + 1) * 4 + n;
    return 1;
}

static void read_name(uint32_t index, char* out) {
    memset(out, 0, 13);
    file_t f = open_disc("/cd/profname.bin", "/cd/PROFNAME.BIN");
    if (f == FILEHND_INVALID) {
        snprintf(out, 13, "%04X", (unsigned)index);
        return;
    }
    fs_seek(f, (off_t)index * 12, SEEK_SET);
    fs_read(f, out, 12);
    fs_close(f);
    for (int i = 0; i < 12; i++) {
        if (out[i] == ' ') {
            out[i] = 0;
            break;
        }
    }
}

static void fill_rows(void) {
    // Same order as scripts/prof_syms.py. Assigned from linker-map address ranges.
    static const char* const gname[] = {
        "REND", "AUD", "GAME", "COL", "ROM", "RT", "MOD", "KOS", "LIBC", "LIBM", "GCC", "UNK"
    };
    func_nrows = 0;
    if (func_page == 0) {
        uint32_t taken = 0;
        int ntop = 0;
        uint32_t top_i[12];
        uint32_t top_c[12];
        for (int k = 0; k < 12; k++) {
            uint32_t best = 0;
            int bi = -1;
            for (uint32_t i = 0; i < func_n; i++) {
                int skip = 0;
                for (int u = 0; u < ntop; u++) {
                    if (top_i[u] == i) skip = 1;
                }
                if (!skip && func_count[i] > best) {
                    best = func_count[i];
                    bi = (int)i;
                }
            }
            if (bi < 0 || best == 0) {
                break;
            }
            top_i[ntop] = (uint32_t)bi;
            top_c[ntop] = best;
            taken += best;
            ntop++;
        }
        uint32_t pctbase = samples ? samples : 1;
        for (int k = 0; k < ntop && func_nrows < 14; k++) {
            char name[13];
            read_name(top_i[k], name);
            snprintf(func_rows[func_nrows], 16, "%u %s", (unsigned)(top_c[k] * 100 / pctbase), name);
            func_nrows++;
        }
        uint32_t other = samples > taken + func_outside ? samples - taken - func_outside : 0;
        snprintf(func_rows[func_nrows++], 16, "%u OTHER", (unsigned)(other * 100 / pctbase));
        snprintf(func_rows[func_nrows++], 16, "%u OUT", (unsigned)(func_outside * 100 / pctbase));
#ifdef GFX_DIAG
    } else if (func_page == 2) {
        // The 14 busiest buckets, in tenths of a percent of all samples, with their addresses
        // for sh-elf-addr2line on the build's ELF. Also printed on the serial port.
        enum { TOP = 14 };
        uint32_t top_i[TOP], top_c[TOP], taken = 0;
        int ntop = 0;
        for (uint32_t i = 0; i < pc_buckets; i++) {
            uint32_t c = pc_count[i];
            if (c == 0 || (ntop == TOP && c <= top_c[TOP - 1])) {
                continue;
            }
            int k = ntop < TOP ? ntop++ : TOP - 1;
            while (k > 0 && top_c[k - 1] < c) {
                top_c[k] = top_c[k - 1];
                top_i[k] = top_i[k - 1];
                k--;
            }
            top_c[k] = c;
            top_i[k] = i;
        }
        uint32_t pctbase = samples ? samples : 1;
        printf("[q64dc] prof pc: %u samples\n", (unsigned)samples);
        for (int k = 0; k < ntop; k++) {
            uint32_t p10 = top_c[k] * 1000 / pctbase;
            uint32_t addr = TEXT_START + (top_i[k] << BUCKET_SHIFT);
            snprintf(func_rows[func_nrows++], 16, "%u.%u %08X", (unsigned)(p10 / 10), (unsigned)(p10 % 10),
                     (unsigned)addr);
            printf("[q64dc] prof pc %08X %u\n", (unsigned)addr, (unsigned)top_c[k]);
            taken += top_c[k];
        }
        uint32_t rest = samples > taken ? samples - taken : 0;
        snprintf(func_rows[func_nrows++], 16, "%u REST", (unsigned)(rest * 100 / pctbase));
#endif
    } else {
        uint32_t g[12] = {0};
        for (uint32_t i = 0; i < func_n; i++) {
            uint8_t grp = func_group[i] < 12 ? func_group[i] : 11;
            g[grp] += func_count[i];
        }
        uint32_t pctbase = samples ? samples : 1;
        for (int i = 0; i < 12 && func_nrows < 15; i++) {
            snprintf(func_rows[func_nrows++], 16, "%u %s", (unsigned)(g[i] * 100 / pctbase), gname[i]);
        }
        snprintf(func_rows[func_nrows++], 16, "%u OUT", (unsigned)(func_outside * 100 / pctbase));
    }
}

void profile_chord(void) {
    if (func_phase == 1) {
        return;
    }
    if (func_phase == 2) {
        func_page = (func_page + 1) % FUNC_PAGES;
        fill_rows();
        return;
    }
    if (!load_tables()) {
        snprintf(func_rows[0], 16, "NO SYM");
        func_nrows = 1;
        func_phase = 2;
        return;
    }
#ifdef GFX_DIAG
    if (pc_count == NULL) {
        uint32_t n = ((TEXT_END - TEXT_START) >> BUCKET_SHIFT) + 1;
        pc_count = calloc(n, sizeof(uint16_t));
        pc_buckets = pc_count != NULL ? n : 0;
    }
    if (pc_count != NULL) {
        memset(pc_count, 0, pc_buckets * sizeof(uint16_t));
    }
#endif
    memset(func_count, 0, func_n * sizeof(uint16_t));
    func_outside = 0;
    samples = 0;
    func_t0 = timer_us_gettime64();
    func_phase = 1;
    irq_set_handler(EXC_TMU1_TUNI1, profile_irq, NULL);
    timer_prime(TMU1, PROFILE_HZ, 1);
    timer_clear(TMU1);
    timer_start(TMU1);
}

void profile_poll(void) {
    if (func_phase != 1) {
        return;
    }
    if (timer_us_gettime64() - func_t0 < 10000000) {
        return;
    }
    timer_stop(TMU1);
    timer_disable_ints(TMU1);
    irq_set_handler(EXC_TMU1_TUNI1, NULL, NULL);
    func_phase = 2;
    func_page = 0;
    fill_rows();
}

int profile_mode(void) {
    return func_phase == 1 ? 1 : func_phase == 2 ? 2 + func_page : 0;
}

int profile_secs(void) {
    if (func_phase != 1) {
        return 0;
    }
    uint64_t left = 10000000 - (timer_us_gettime64() - func_t0);
    return (int)(left / 1000000);
}

const char* profile_row(int i) {
    if (i < 0 || i >= func_nrows) {
        return "";
    }
    return func_rows[i];
}

uint32_t profile_table_bytes(void) {
    return func_table_bytes;
}
#endif
