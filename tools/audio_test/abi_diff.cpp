// Compares runtime/src/audio_hle.c with Quest64-Recomp's recompilation of the game's audio
// microcode (rsp/aspMain.cpp, generated from the ROM by RSPRecomp) on random command lists shaped
// like libultra's synthesizer output. Built and run on the host by tools/audio_test/run.sh.
//
// usage: abi_diff <quest64.us.z64> [seeds] [first seed]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "librecomp/rsp.hpp"

extern RspExitReason aspMain(uint8_t* rdram, uint32_t ucode_addr);

extern "C" {
#include "audio_hle.h"
}

namespace {

constexpr uint32_t RAM_SIZE = 8 * 1024 * 1024;
constexpr uint32_t UCODE_DATA = 0x72BD0;  // where the game keeps aspMain's data
constexpr uint32_t UCODE_DATA_ROM = 0x737D0;
constexpr uint32_t TASK = 0x100000;
constexpr uint32_t LIST = 0x101000;
constexpr uint32_t SAMPLES = 0x200000;
constexpr uint32_t BOOKS = 0x210000;
constexpr uint32_t LOOPS = 0x211000;
constexpr uint32_t ADPCM_STATE = 0x212000;
constexpr uint32_t RES_STATE = 0x212400;
constexpr uint32_t ENV_STATE = 0x212800;
constexpr uint32_t POLEF_STATE = 0x212C00;
constexpr uint32_t DUMPS = 0x300000;

// DMEM buffers, relative to the microcode's 0x5C0 base, as the game's are.
constexpr uint32_t FRAME = 0x120;
constexpr uint32_t D_IN = 0x000, D_DEC = 0x060, D_RES = 0x1C0, D_ML = 0x300, D_MR = 0x420,
                   D_AL = 0x540, D_AR = 0x660, D_T1 = 0x780, D_T2 = 0x8A0, D_END = 0x9C0;

struct Region {
    const char* name;
    uint32_t dmem, len;
};
const Region regions[] = {
    {"adpcm out", D_DEC, 0x160}, {"resample out", D_RES, FRAME}, {"main L", D_ML, FRAME},
    {"main R", D_MR, FRAME},     {"aux L", D_AL, FRAME},         {"aux R", D_AR, FRAME},
    {"dmemmove", D_T1, FRAME},   {"polef", D_T2, FRAME},
};
constexpr int NREGIONS = sizeof(regions) / sizeof(regions[0]);
constexpr uint32_t AI_BUF = DUMPS + 0x10000;

uint32_t rng_state;
uint32_t rnd() {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}
int32_t rnd_range(int32_t lo, int32_t hi) {
    return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1));
}

void w32(uint8_t* ram, uint32_t p, uint32_t v) {
    *(uint32_t*)(ram + p) = v;
}
void h16(uint8_t* ram, uint32_t p, int16_t v) {
    *(int16_t*)(ram + (p ^ 2)) = v;
}
int16_t r16(const uint8_t* ram, uint32_t p) {
    return *(const int16_t*)(ram + (p ^ 2));
}

struct List {
    std::vector<uint32_t> w;
    void add(uint32_t op, uint32_t flags, uint32_t lo, uint32_t w1) {
        w.push_back((op << 24) | ((flags & 0xFF) << 16) | (lo & 0xFFFF));
        w.push_back(w1);
    }
};

enum { SPNOOP, ADPCM, CLEARBUFF, ENVMIXER, LOADBUFF, RESAMPLE, SAVEBUFF, SEGMENT, SETBUFF, SETVOL,
       DMEMMOVE, LOADADPCM, MIXER, INTERLEAVE, POLEF, SETLOOP };

struct Voice {
    int npred;
    int16_t book_scale;
};

// One frame's command list. `first` makes every voice start (A_INIT).
List make_list(bool first, int nvoices, const Voice* voices, int frame) {
    List l;
    l.add(SEGMENT, 0, 0, 0);
    l.add(CLEARBUFF, 0, 0, D_END);
    for (int v = 0; v < nvoices; v++) {
        l.add(LOADADPCM, 0, voices[v].npred * 32, BOOKS + v * 0x100);
        l.add(SETLOOP, 0, 0, LOOPS + v * 0x20);
        int k = rnd_range(1, 10);
        uint32_t nbytes = (uint32_t)k * 9;
        l.add(SETBUFF, 0, D_IN, (0u << 16) | ((nbytes + 7) & ~7u));
        l.add(LOADBUFF, 0, 0, SAMPLES + (uint32_t)(frame * nvoices + v) * 0x60);
        l.add(SETBUFF, 0, D_IN, (D_DEC << 16) | (uint32_t)(k * 32));
        uint32_t aflags = first ? 1 : (rnd() % 5 == 0 ? 2 : 0);
        l.add(ADPCM, aflags, 0, ADPCM_STATE + v * 0x20);

        l.add(SETBUFF, 0, D_DEC + 32, (D_RES << 16) | FRAME);
        l.add(RESAMPLE, first ? 1 : 0, rnd_range(0x1000, 0xFFFF), RES_STATE + v * 0x20);

        l.add(SETVOL, 0x04 | 0x02, rnd_range(-0x8000, 0x7FFF), 0);
        l.add(SETVOL, 0x04, rnd_range(-0x8000, 0x7FFF), 0);
        l.add(SETVOL, 0x02, rnd_range(-0x8000, 0x7FFF), (uint32_t)rnd_range(0xC000, 0x14000));
        l.add(SETVOL, 0x00, rnd_range(-0x8000, 0x7FFF), (uint32_t)rnd_range(0xC000, 0x14000));
        l.add(SETVOL, 0x08, rnd_range(0, 0x7FFF), (uint32_t)rnd_range(0, 0x7FFF));
        l.add(SETBUFF, 0, D_RES, (D_ML << 16) | FRAME);
        l.add(SETBUFF, 0x08, D_MR, (D_AL << 16) | D_AR);
        uint32_t eflags = (first ? 1 : 0) | (rnd() % 2 ? 0x08 : 0);
        l.add(ENVMIXER, eflags, 0, ENV_STATE + v * 0x50);
    }
    l.add(SETBUFF, 0, 0, (0u << 16) | FRAME);
    l.add(MIXER, 0, rnd_range(-0x8000, 0x7FFF), (D_AL << 16) | D_ML);
    l.add(MIXER, 0, rnd_range(-0x8000, 0x7FFF), (D_AR << 16) | D_MR);
    l.add(DMEMMOVE, 0, D_AL, (D_T1 << 16) | FRAME);
    l.add(LOADADPCM, 0, 32, BOOKS + 0x1000);
    l.add(SETBUFF, 0, D_AR, (D_T2 << 16) | FRAME);
    // POLEF's A_INIT takes its history from whatever the last command left in the microcode's
    // scratch area, which audio_hle.c doesn't model; a zeroed state starts both sides alike.
    l.add(POLEF, 0, rnd_range(0, 0x7FFF), POLEF_STATE);

    for (int r = 0; r < NREGIONS; r++) {
        l.add(SETBUFF, 0, 0, (regions[r].dmem << 16) | regions[r].len);
        l.add(SAVEBUFF, 0, 0, DUMPS + r * 0x1000);
    }
    l.add(SETBUFF, 0, 0, (0u << 16) | FRAME);
    l.add(INTERLEAVE, 0, 0, (D_ML << 16) | D_MR);
    l.add(SETBUFF, 0, 0, (0u << 16) | (FRAME * 2));
    l.add(SAVEBUFF, 0, 0, AI_BUF);
    return l;
}

void write_task(uint8_t* ram, const List& l) {
    for (size_t i = 0; i < l.w.size(); i++) {
        w32(ram, LIST + i * 4, l.w[i]);
    }
    for (int i = 0; i < 16; i++) {
        w32(ram, TASK + i * 4, 0);
    }
    w32(ram, TASK + 0x00, 2);
    w32(ram, TASK + 0x18, 0x80000000 | UCODE_DATA);
    w32(ram, TASK + 0x1C, 0x800);
    w32(ram, TASK + 0x30, 0x80000000 | LIST);
    w32(ram, TASK + 0x34, (uint32_t)(l.w.size() * 4));
}

void run_recomp(uint8_t* ram) {
    memcpy(&dmem[0xFC0], ram + TASK, 0x40);
    dma_rdram_to_dmem(ram, 0x0000, 0x80000000 | UCODE_DATA, 0xF80 - 1);
    RspExitReason r = aspMain(ram, 0);
    if (r != RspExitReason::Broke) {
        fprintf(stderr, "aspMain exited with %d\n", (int)r);
        exit(2);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: abi_diff <rom> [seeds] [first seed]\n");
        return 1;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    std::vector<uint8_t> rom(16 * 1024 * 1024);
    rom.resize(fread(rom.data(), 1, rom.size(), f));
    fclose(f);
    int seeds = argc > 2 ? atoi(argv[2]) : 200;
    int first_seed = argc > 3 ? atoi(argv[3]) : 1;

    recomp::rsp::constants_init();
    std::vector<uint8_t> a(RAM_SIZE), b(RAM_SIZE);

    int failures = 0;
    std::vector<long> mism(NREGIONS + 2, 0);
    long samples_checked = 0;
    for (int seed = first_seed; seed < first_seed + seeds; seed++) {
        rng_state = (uint32_t)seed * 2654435761u;
        memset(a.data(), 0, RAM_SIZE);
        for (uint32_t i = 0; i < 0xF80; i += 4) {
            const uint8_t* s = &rom[UCODE_DATA_ROM + i];
            w32(a.data(), UCODE_DATA + i, (uint32_t)s[0] << 24 | s[1] << 16 | s[2] << 8 | s[3]);
        }
        for (uint32_t i = 0; i < 0x8000; i += 4) {
            w32(a.data(), SAMPLES + i, rnd() ^ (rnd() << 16));
        }
        // ADPCM frame headers: scale 0-12 mostly, some 13-15; the predictor within the book.
        Voice voices[3];
        int nvoices = rnd_range(1, 3);
        for (int v = 0; v < 3; v++) {
            voices[v].npred = rnd_range(1, 4);
            int16_t lim = (rnd() % 4 == 0) ? 0x7FFF : 0x1800;
            for (int i = 0; i < voices[v].npred * 16; i++) {
                h16(a.data(), BOOKS + v * 0x100 + i * 2, (int16_t)rnd_range(-lim, lim));
            }
            for (int i = 0; i < 16; i++) {
                h16(a.data(), LOOPS + v * 0x20 + i * 2, (int16_t)rnd_range(-0x8000, 0x7FFF));
            }
        }
        for (int i = 0; i < 16; i++) {
            h16(a.data(), BOOKS + 0x1000 + i * 2, (int16_t)rnd_range(-0x3000, 0x3000));
        }
        for (int frame = 0; frame < 8; frame++) {
            for (int v = 0; v < nvoices; v++) {
                for (int k = 0; k < 10; k++) {
                    uint32_t p = SAMPLES + (uint32_t)(frame * nvoices + v) * 0x60 + k * 9;
                    int scale = rnd() % 8 == 0 ? rnd_range(13, 15) : rnd_range(0, 12);
                    uint8_t hdr = (uint8_t)(scale << 4 | rnd_range(0, voices[v].npred - 1));
                    a[(p) ^ 3] = hdr;
                }
            }
        }
        b = a;

        for (int frame = 0; frame < 8; frame++) {
            List l = make_list(frame == 0, nvoices, voices, frame);
            write_task(a.data(), l);
            write_task(b.data(), l);
            run_recomp(a.data());
            audio_hle_run(b.data(), RAM_SIZE, 0x80000000 | TASK);

            bool bad = false;
            for (int r = 0; r <= NREGIONS + 1; r++) {
                uint32_t base, len;
                const char* name;
                if (r < NREGIONS) {
                    base = DUMPS + r * 0x1000, len = regions[r].len, name = regions[r].name;
                } else if (r == NREGIONS) {
                    base = AI_BUF, len = FRAME * 2, name = "interleave";
                } else {
                    base = ADPCM_STATE, len = (uint32_t)nvoices * 0x20, name = "adpcm state";
                }
                int n = 0, first_i = -1;
                for (uint32_t i = 0; i < len; i += 2) {
                    if (r16(a.data(), base + i) != r16(b.data(), base + i)) {
                        if (first_i < 0) first_i = (int)i / 2;
                        n++;
                    }
                }
                samples_checked += len / 2;
                mism[r] += n;
                if (n > 0) {
                    bad = true;
                    if (failures < 12) {
                        int i = first_i;
                        printf("seed %d frame %d %s: %d/%u differ, first [%d] recomp %d hle %d",
                               seed, frame, name, n, len / 2, i, r16(a.data(), base + i * 2),
                               r16(b.data(), base + i * 2));
                        printf(" | next:");
                        for (int j = i + 1; j < i + 5 && (uint32_t)j < len / 2; j++) {
                            printf(" %d/%d", r16(a.data(), base + j * 2), r16(b.data(), base + j * 2));
                        }
                        printf("\n");
                    }
                }
            }
            if (bad) {
                failures++;
            }
        }
    }
    printf("--- %d seeds x 8 frames, %ld samples compared\n", seeds, samples_checked);
    for (int r = 0; r <= NREGIONS + 1; r++) {
        printf("  %-13s %ld mismatches\n",
               r < NREGIONS ? regions[r].name : r == NREGIONS ? "interleave" : "adpcm state", mism[r]);
    }
    printf("%s\n", failures ? "MISMATCH" : "IDENTICAL");
    return failures ? 1 : 0;
}
