// Scripted input for headless test runs. A disc built with `make cdi TEST_SCRIPT=<file>` carries
// the script as /cd/q64test.txt; the runtime runs it from the first controller poll, one command
// at a time, and its input is added to controller 1. Discs without the file behave normally.
//
// Commands, one per line (# starts a comment, times are milliseconds of real time):
//   wait <ms>                         do nothing
//   hold <buttons> <ms>               hold buttons, e.g. A, S+A, CU (buttons: A B Z S L R X CU CD CL CR DU DD DL DR; X is Dreamcast X's bit)
//   tap <buttons> [count] [gap ms]    press for 100 ms, then release for gap (default 400)
//   stick <x> <y> <ms>                hold the stick (N64 units, -80..80); 0 ms keeps it held until changed
//   talk <timeout ms>                 tap A until Brian is free to move (no text, menu, warp or load)
//   wait_free <timeout ms>            wait until Brian is free to move
//   wait16 <addr> <mask> <value> <timeout ms>    wait until (halfword & mask) == value
//   battle_walk <timeout ms> [seed]   sway in place, with every encounter roll forced, until a battle starts;
//                                     a seed (hex) pins the game's random numbers, so the formation is the same
//   to_zone <index>                   put Brian inside one of the room's random-battle zones
//   to_battle                         put Brian in a zone, 70 units from one of the room's battle points
//   warp <map> <room> <spawn>         start a warp, then wait until the new room is loaded and free
//   to_npc <type> <distance>          put Brian <distance> in front of the first NPC of a type (6: innkeeper), facing it
//   place <x> <z> [<x> <z>]           put Brian at a point of the room, facing the second point if given
//   setbit <addr> <bit>               set bit <bit> counting from the byte at addr (hex), e.g. a mod's flag array
//   poke8|poke16|poke32 <addr> <value>   (hex)
//   peek32 <addr>                     print a word
//   shot <name>                       print a SHOT line; run_flycast.sh saves build/screens/<name>.png
//   log <text>
//   pos                               print Brian's position and step, the retrace and frame counts and the time
//   watch <addr>                      print every change of a halfword, checked at each controller poll (hex, up to 4)
//   burn <ms>                         busy-wait that long in every controller poll, to slow the game down (0: off)
//   profile_start, profile_dump       sample the program counter in between (profile.c)
//   pace_start, pace_dump <label>     record each vblank's flips, swaps and task hand-overs in between (events.c)
//   pacing 0|1                        hand frames to the PowerVR as soon as they are built / on the frame cadence
//   romtrace [0|1]                    print every ROM cache miss (rom.c)
//   mem                               print the heap's size and use, and the largest block malloc can still give
//   timebase frames|ms                count script times in controller polls (1000/30 ms each) instead of real time,
//                                     so a build that runs slower or faster gets the same input on the same game frame
//                                     (LOCKSTEP builds always count in their virtual time; see ultra.h)
//   hash <label>                      print a hash of each 64 KB block of RDRAM (scripts/compare_runs.py)
//   hashsub <block>                   print a hash of each 1 KB of one 64 KB block
//   dump <addr> <words>               print words of RDRAM (hex address)
//   scan <hi16>                       print the first 64 words of RDRAM whose top halfword is hi16 (hex), e.g.
//                                     DEAD or BEEF for codegen.abi_poison's junk
//   loop                              start over

#include <kos.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ultra.h"
#include "test.h"
#include "profile.h"
#include "gfx.h"

// Game globals (the Eltale Reborn mod's include/q64/game.h).
#define GAME_STATE      0x8007B2E4u
#define PLAYER_STATE    0x8007BAB8u
#define PLAYER_POS      0x8007BACCu  // x, y, z, then rotation x, y, z
#define WARP_TIMER      0x8007B2ECu
#define WARP_FADE_TYPE  0x8007B2F0u
#define WARP_FADE_AT    0x8007B2F2u
#define WARP_MAP        0x80085368u
#define WARP_ROOM       0x8008536Cu
#define WARP_ENTRY      0x80085370u
#define ENTRY_MODE      0x8007BA4Cu
#define NEXT_MAP        0x80084EE4u
#define NEXT_ROOM       0x80084EE8u
#define NPCS            0x8007BD30u
#define NPC_COUNT       0x8007BCE4u
#define NPC_SIZE        0x84
#define GAME_MODE       0x8007B2E0u
#define BATTLE_STATE    0x8008C592u  // bit 0: a battle is going on
#define ENCOUNTER_ZONES 0x8008C560u  // -> {zones, count}, zones of 0x18 bytes starting {s16 x, z, w, d}
#define ENCOUNTER_SITE  0x8008C564u  // -> {positions, formations, s16 (x, z)* battle points, count}
#define ALLOW_BATTLES   0x80084F10u  // bit 0: the room has random battles
#define ENCOUNTER_STEP  0x8007BA5Cu  // f32: distance Brian moved this frame; no roll while 0
#define ENCOUNTER_DIST  0x8008C574u  // f32: a roll every 50 units walked
#define ENCOUNTER_DANGER 0x8008C578u // u16: the roll starts a battle if rand(2000) < danger
#define RAND_SEED       0x8004D748u  // u32: get_rand's LCG state

#define GAME_STATE_BUSY 0xC7  // text, menus, choice, warp, loading
#define GAME_MODE_FIELD 1     // the gameplay loop (2: menu screen, 3: title, 4: intro)

#define MAX_LINES 512

static char* lines[MAX_LINES];
static int line_count;
static int pc;
static bool active;

static unsigned burn_ms;

#define MAX_WATCHES 4
static uint32_t watch_addr[MAX_WATCHES];
static uint16_t watch_value[MAX_WATCHES];
static int watch_count;
static uint64_t step_start;
static bool step_started;
static uint16_t held_buttons;
static int8_t held_x, held_y;
static bool stick_latched;
static int8_t latched_x, latched_y;
static bool frame_time;
static uint32_t polls;

// FNV-1a over words: an RDRAM block's contents, compared between runs.
static uint32_t hash_words(uint32_t addr, uint32_t words) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < words; i++) {
        h = (h ^ (uint32_t)MEM_W(i * 4, addr)) * 16777619u;
    }
    return h;
}

static const struct {
    const char* name;
    uint16_t bit;
} button_names[] = {
    { "A", 0x8000 }, { "B", 0x4000 }, { "Z", 0x2000 }, { "S", 0x1000 },
    { "DU", 0x0800 }, { "DD", 0x0400 }, { "DL", 0x0200 }, { "DR", 0x0100 },
    { "L", 0x0020 }, { "R", 0x0010 }, { "X", 0x0040 },
    { "CU", 0x0008 }, { "CD", 0x0004 }, { "CL", 0x0002 }, { "CR", 0x0001 },
};

static float mem_f32(uint32_t addr) {
    float f;
    int32_t w = MEM_W(0, addr);
    memcpy(&f, &w, 4);
    return f;
}

static void set_f32(uint32_t addr, float f) {
    int32_t w;
    memcpy(&w, &f, 4);
    MEM_W(0, addr) = w;
}

static uint16_t parse_buttons(const char* s) {
    uint16_t out = 0;
    char buf[64];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char* tok = strtok(buf, "+"); tok != NULL; tok = strtok(NULL, "+")) {
        bool found = false;
        for (size_t i = 0; i < sizeof(button_names) / sizeof(button_names[0]); i++) {
            if (strcmp(tok, button_names[i].name) == 0) {
                out |= button_names[i].bit;
                found = true;
            }
        }
        if (!found) {
            printf("[q64dc] test: unknown button %s\n", tok);
        }
    }
    return out;
}

static bool brian_free(void) {
    return (MEM_W(0, GAME_STATE) & GAME_STATE_BUSY) == 0 && MEM_HU(0, GAME_MODE) == GAME_MODE_FIELD &&
           MEM_HU(0, PLAYER_STATE) <= 2;
}

bool test_init(void) {
    FILE* f = fopen("/cd/q64test.txt", "r");
    if (f == NULL) {
        return false;
    }
    char buf[256];
    while (line_count < MAX_LINES && fgets(buf, sizeof(buf), f) != NULL) {
        char* s = buf;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        size_t n = strcspn(s, "#\r\n");
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
            n--;
        }
        if (n == 0) {
            continue;
        }
        s[n] = '\0';
        lines[line_count++] = strdup(s);
    }
    fclose(f);
    active = line_count > 0;
    printf("[q64dc] ************************************************************\n"
           "[q64dc] TEST DISC: playing /cd/q64test.txt (%d commands) as controller 1\n"
           "[q64dc] ************************************************************\n",
           line_count);
    return active;
}

bool test_disc(void) {
    return line_count > 0;
}

static void next(void) {
    pc++;
    step_started = false;
    held_buttons = 0;
    held_x = held_y = 0;
}

// Runs the current command; returns true when it's done.
static bool step(uint64_t now) {
    const char* line = lines[pc];
    char cmd[16] = "";
    sscanf(line, "%15s", cmd);
    const char* args = line + strlen(cmd);
    uint64_t elapsed = now - step_start;

    if (strcmp(cmd, "wait") == 0) {
        unsigned ms = 0;
        sscanf(args, "%u", &ms);
        return elapsed >= ms;
    }
    if (strcmp(cmd, "hold") == 0) {
        char b[64] = "";
        unsigned ms = 0;
        sscanf(args, "%63s %u", b, &ms);
        held_buttons = parse_buttons(b);
        return elapsed >= ms;
    }
    if (strcmp(cmd, "tap") == 0) {
        char b[64] = "";
        unsigned count = 1, gap = 400;
        sscanf(args, "%63s %u %u", b, &count, &gap);
        uint64_t period = 100 + gap;
        held_buttons = (elapsed % period) < 100 ? parse_buttons(b) : 0;
        return elapsed >= period * count;
    }
    if (strcmp(cmd, "stick") == 0) {
        int x = 0, y = 0;
        unsigned ms = 0;
        sscanf(args, "%d %d %u", &x, &y, &ms);
        if (ms == 0) {
            stick_latched = x != 0 || y != 0;
            latched_x = (int8_t)x;
            latched_y = (int8_t)y;
            return true;
        }
        held_x = (int8_t)x;
        held_y = (int8_t)y;
        return elapsed >= ms;
    }
    if (strcmp(cmd, "talk") == 0 || strcmp(cmd, "wait_free") == 0) {
        unsigned timeout = 30000;
        sscanf(args, "%u", &timeout);
        // Free for half a second, so a text box between two pages doesn't count.
        static uint64_t free_since;
        if (elapsed == 0 || !brian_free()) {
            free_since = now;
        } else if (now - free_since >= 500) {
            return true;
        }
        // Not while Brian is free: A then starts his 15-frame staff action (player state 3), and
        // at 30 fps the taps would keep him in it.
        if (cmd[0] == 't') {
            held_buttons = !brian_free() && (elapsed % 700) < 100 ? 0x8000 : 0;
        }
        if (elapsed >= timeout) {
            printf("[q64dc] test: %s timed out (state 0x%X mode %u player %u)\n", cmd, (unsigned)MEM_W(0, GAME_STATE),
                   (unsigned)MEM_HU(0, GAME_MODE), (unsigned)MEM_HU(0, PLAYER_STATE));
            return true;
        }
        return false;
    }
    if (strcmp(cmd, "battle_walk") == 0) {
        unsigned timeout = 60000, seed = 0;
        bool seeded = sscanf(args, "%u %x", &timeout, &seed) == 2;
        if (MEM_HU(0, BATTLE_STATE) & 1) {
            printf("[q64dc] test: battle started after %u ms\n", (unsigned)elapsed);
            return true;
        }
        if (elapsed >= timeout) {
            printf("[q64dc] test: no battle within %u ms\n", timeout);
            return true;
        }
        // Every encounter roll succeeds; the game still needs one of the room's battle points
        // 50-90 units from Brian (to_battle), and a roll only happens while he moves, so sway
        // left and right in place.
        static uint64_t last_log;
        if (elapsed == 0 || now - last_log >= 2000) {
            last_log = now;
            printf("[q64dc] test: battle_walk step %.2f dist %.1f danger %u flags 0x%X battle 0x%X allow 0x%X Brian (%d, %d) state %u\n",
                   mem_f32(ENCOUNTER_STEP), mem_f32(ENCOUNTER_DIST), (unsigned)MEM_HU(0, ENCOUNTER_DANGER),
                   (unsigned)MEM_HU(0, 0x8007BB2Cu), (unsigned)MEM_HU(0, BATTLE_STATE), (unsigned)MEM_HU(0, ALLOW_BATTLES),
                   (int)mem_f32(PLAYER_POS), (int)mem_f32(PLAYER_POS + 8), (unsigned)MEM_HU(0, PLAYER_STATE));
        }
        set_f32(ENCOUNTER_DIST, 50.0f);
        MEM_HU(0, ENCOUNTER_DANGER) = 2000;
        if (seeded) {
            MEM_W(0, RAND_SEED) = (int32_t)seed;
        }
        held_x = (elapsed / 300) % 2 ? 40 : -40;
        return false;
    }
    if (strcmp(cmd, "wait16") == 0) {
        unsigned addr = 0, mask = 0, value = 0, timeout = 30000;
        sscanf(args, "%x %x %x %u", &addr, &mask, &value, &timeout);
        if ((MEM_HU(0, addr) & mask) == value) {
            return true;
        }
        if (elapsed >= timeout) {
            printf("[q64dc] test: wait16 %08X timed out (0x%04X)\n", addr, (unsigned)MEM_HU(0, addr));
            return true;
        }
        return false;
    }
    if (strcmp(cmd, "warp") == 0) {
        int map = 0, room = 0, spawn = 0;
        sscanf(args, "%i %i %i", &map, &room, &spawn);
        static bool started;
        if (elapsed == 0 || !started) {
            MEM_W(0, WARP_MAP) = map;
            MEM_W(0, WARP_ROOM) = room;
            MEM_W(0, WARP_ENTRY) = spawn;
            MEM_W(0, ENTRY_MODE) = 0;
            MEM_HU(0, WARP_FADE_TYPE) = 0;
            MEM_HU(0, WARP_FADE_AT) = 0xFFFF;
            MEM_W(0, WARP_TIMER) = 1;
            MEM_W(0, GAME_STATE) = (MEM_W(0, GAME_STATE) & ~0x02) | 0x80;
            printf("[q64dc] test: warp to map %d room %d spawn %d\n", map, room, spawn);
            started = true;
            return false;
        }
        if (MEM_W(0, NEXT_MAP) == map && MEM_W(0, NEXT_ROOM) == room && brian_free()) {
            started = false;
            printf("[q64dc] test: arrived in map %d room %d after %u ms\n", map, room, (unsigned)elapsed);
            return true;
        }
        if (elapsed >= 30000) {
            printf("[q64dc] test: warp timed out (state 0x%X, room %d/%d)\n", (unsigned)MEM_W(0, GAME_STATE),
                   (int)MEM_W(0, NEXT_MAP), (int)MEM_W(0, NEXT_ROOM));
            started = false;
            return true;
        }
        return false;
    }
    if (strcmp(cmd, "to_npc") == 0) {
        unsigned type = 0;
        float dist = 20.0f;
        sscanf(args, "%u %f", &type, &dist);
        int count = MEM_W(0, NPC_COUNT);
        for (int i = 0; i < count && i < 16; i++) {
            uint32_t npc = NPCS + i * NPC_SIZE;
            uint32_t def = (uint32_t)MEM_W(0x80, npc);
            if (def == 0 || MEM_HU(0x02, def) != type) {
                continue;
            }
            float nx = mem_f32(npc + 0x14), nz = mem_f32(npc + 0x1C), rot = mem_f32(npc + 0x24);
            float bx = nx + sinf(rot) * dist, bz = nz + cosf(rot) * dist;
            set_f32(PLAYER_POS + 0x00, bx);
            set_f32(PLAYER_POS + 0x08, bz);
            set_f32(PLAYER_POS + 0x10, atan2f(nx - bx, nz - bz));
            printf("[q64dc] test: Brian at (%d, %d) facing NPC %d at (%d, %d)\n", (int)bx, (int)bz, i, (int)nx, (int)nz);
            return true;
        }
        printf("[q64dc] test: no NPC of type %u here\n", type);
        return true;
    }
    if (strcmp(cmd, "place") == 0) {
        float x = 0, z = 0, fx = 0, fz = 0;
        int n = sscanf(args, "%f %f %f %f", &x, &z, &fx, &fz);
        set_f32(PLAYER_POS + 0x00, x);
        set_f32(PLAYER_POS + 0x08, z);
        if (n == 4) {
            set_f32(PLAYER_POS + 0x10, atan2f(fx - x, fz - z));
        }
        printf("[q64dc] test: Brian placed at (%d, %d)\n", (int)x, (int)z);
        return true;
    }
    if (strcmp(cmd, "setbit") == 0) {
        unsigned addr = 0, bit = 0;
        sscanf(args, "%x %u", &addr, &bit);
        MEM_BU(0, addr + bit / 8) |= (uint8_t)(1u << (bit % 8));
        return true;
    }
    if (strcmp(cmd, "to_zone") == 0) {
        unsigned index = 0;
        sscanf(args, "%u", &index);
        uint32_t table = (uint32_t)MEM_W(0, ENCOUNTER_ZONES);
        if (table < 0x80000000u || table >= 0x80400000u || index >= (uint32_t)MEM_W(4, table)) {
            printf("[q64dc] test: no encounter zone %u here\n", index);
            return true;
        }
        uint32_t zone = (uint32_t)MEM_W(0, table) + index * 0x18;
        int x = MEM_H(0, zone), z = MEM_H(2, zone), w = MEM_H(4, zone), d = MEM_H(6, zone);
        // Whether (x, z) is the zone's corner or its centre, a quarter of the size in is inside.
        set_f32(PLAYER_POS + 0x00, (float)(x + w / 4));
        set_f32(PLAYER_POS + 0x08, (float)(z + d / 4));
        printf("[q64dc] test: Brian in encounter zone %u (%d, %d, %d x %d)\n", index, x, z, w, d);
        return true;
    }
    if (strcmp(cmd, "to_battle") == 0) {
        uint32_t table = (uint32_t)MEM_W(0, ENCOUNTER_ZONES), site = (uint32_t)MEM_W(0, ENCOUNTER_SITE);
        if (!(MEM_HU(0, ALLOW_BATTLES) & 1) || table < 0x80000000u || site < 0x80000000u) {
            printf("[q64dc] test: no random battles here (allow 0x%X)\n", (unsigned)MEM_HU(0, ALLOW_BATTLES));
            return true;
        }
        uint32_t zones = (uint32_t)MEM_W(0, table), points = (uint32_t)MEM_W(8, site);
        unsigned zone_count = (unsigned)MEM_W(4, table), point_count = (unsigned)MEM_W(12, site);
        for (unsigned p = 0; p < point_count && p < 256; p++) {
            int px = MEM_H(0, points + p * 4), pz = MEM_H(2, points + p * 4);
            for (int dir = 0; dir < 8; dir++) {
                float bx = px + 70.0f * cosf(dir * 0.785398f), bz = pz + 70.0f * sinf(dir * 0.785398f);
                for (unsigned z = 0; z < zone_count && z < 64; z++) {
                    uint32_t e = zones + z * 0x18;
                    int x0 = MEM_H(0, e), z0 = MEM_H(2, e), w = MEM_H(4, e), d = MEM_H(6, e);
                    if (bx > x0 + 10 && bx < x0 + w - 10 && bz > z0 + 10 && bz < z0 + d - 10) {
                        set_f32(PLAYER_POS + 0x00, bx);
                        set_f32(PLAYER_POS + 0x08, bz);
                        printf("[q64dc] test: Brian at (%d, %d) in zone %u, 70 from battle point %u (%d, %d)\n",
                               (int)bx, (int)bz, z, p, px, pz);
                        return true;
                    }
                }
            }
        }
        printf("[q64dc] test: no battle point with room for Brian (%u points, %u zones)\n", point_count, zone_count);
        return true;
    }
    if (strcmp(cmd, "peek32") == 0) {
        unsigned addr = 0;
        sscanf(args, "%x", &addr);
        printf("[q64dc] test: %08X = %d (0x%08X)\n", addr, (int)MEM_W(0, addr), (unsigned)MEM_W(0, addr));
        return true;
    }
    if (strncmp(cmd, "poke", 4) == 0) {
        unsigned addr = 0, value = 0;
        sscanf(args, "%x %x", &addr, &value);
        if (strcmp(cmd, "poke8") == 0) {
            MEM_BU(0, addr) = (uint8_t)value;
        } else if (strcmp(cmd, "poke16") == 0) {
            MEM_HU(0, addr) = (uint16_t)value;
        } else {
            MEM_W(0, addr) = (int32_t)value;
        }
        return true;
    }
    if (strcmp(cmd, "shot") == 0 || strcmp(cmd, "log") == 0) {
        while (*args == ' ') {
            args++;
        }
        if (cmd[0] == 's') {
#ifdef LOCKSTEP
            // The game stands still in virtual time while the RSP thread draws the tasks it was
            // sent and the host takes the picture, so two builds' shots show the same game frame
            // whatever their speed.
            for (int i = 0; i < 400 && rsp_busy(); i++) {
                thd_sleep(5);
            }
            thd_sleep(250);
#endif
            printf("[q64dc] SHOT %s\n", args);
#ifdef LOCKSTEP
            thd_sleep(1500);
#endif
        } else {
            printf("[q64dc] test: %s\n", args);
        }
        return true;
    }
    if (strcmp(cmd, "mem") == 0) {
        struct mallinfo mi = mallinfo();
        uint32_t lo = 0, hi = 16 * 1024 * 1024;
        while (hi - lo > 4096) {
            uint32_t mid = (lo + hi) / 2;
            void* p = malloc(mid);
            if (p) {
                free(p);
                lo = mid;
            } else {
                hi = mid;
            }
        }
        printf("[q64dc] test: mem: heap %u KB, %u KB of it free; largest new block %u KB\n",
               (unsigned)(mi.arena / 1024), (unsigned)(mi.fordblks / 1024), (unsigned)(lo / 1024));
        return true;
    }
    if (strcmp(cmd, "romtrace") == 0) {
        unsigned on = 1;
        sscanf(args, "%u", &on);
        rom_trace = on != 0;
        return true;
    }
    if (strcmp(cmd, "pos") == 0) {
        printf("[q64dc] test: pos %.2f %.2f %.2f step %.3f VI %u swaps %u t %u ms\n",
               mem_f32(PLAYER_POS), mem_f32(PLAYER_POS + 4), mem_f32(PLAYER_POS + 8),
               mem_f32(ENCOUNTER_STEP), (unsigned)stat_vi_count, (unsigned)stat_swap_count,
               (unsigned)now);
        return true;
    }
    if (strcmp(cmd, "watch") == 0) {
        if (watch_count < MAX_WATCHES) {
            uint32_t addr = 0;
            sscanf(args, "%lx", (unsigned long*)&addr);
            watch_addr[watch_count] = addr;
            watch_value[watch_count] = MEM_HU(0, addr);
            watch_count++;
        }
        return true;
    }
    if (strcmp(cmd, "burn") == 0) {
        burn_ms = 0;
        sscanf(args, "%u", &burn_ms);
        return true;
    }
    if (strcmp(cmd, "frameskip") == 0) {
        gfx_skip_set(atoi(args));
        return true;
    }
#if defined(GFX_DIAG) && defined(GFX_OPT)
    if (strcmp(cmd, "gfxopt") == 0) {
        gfx_diag_select(atoi(args), -1);
        return true;
    }
    if (strcmp(cmd, "gfxdiag") == 0) {
        gfx_diag_select(-1, atoi(args));
        return true;
    }
#endif
    if (strcmp(cmd, "profile_start") == 0) {
        profile_start();
        return true;
    }
    if (strcmp(cmd, "profile_dump") == 0) {
        profile_dump();
        return true;
    }
    if (strcmp(cmd, "pace_start") == 0) {
        pace_start();
        return true;
    }
    if (strcmp(cmd, "pace_dump") == 0) {
        while (*args == ' ') {
            args++;
        }
        pace_dump(args);
        return true;
    }
    if (strcmp(cmd, "pacing") == 0) {
        events_set_paced(atoi(args) != 0);
        return true;
    }
    if (strcmp(cmd, "timebase") == 0) {
        char base[16] = "";
        sscanf(args, "%15s", base);
        frame_time = strcmp(base, "frames") == 0;
        return true;
    }
    if (strcmp(cmd, "hash") == 0) {
        while (*args == ' ') {
            args++;
        }
        printf("[q64dc] HASH %s poll %u vi %u", args, (unsigned)polls, (unsigned)stat_vi_count);
        for (uint32_t block = 0; block < RDRAM_SIZE / 0x10000; block++) {
            printf(" %08X", (unsigned)hash_words(0x80000000u + block * 0x10000, 0x10000 / 4));
        }
        printf("\n");
        return true;
    }
    if (strcmp(cmd, "hashsub") == 0) {
        unsigned block = 0;
        sscanf(args, "%x", &block);
        printf("[q64dc] HASHSUB %02X poll %u", block, (unsigned)polls);
        for (uint32_t sub = 0; sub < 64; sub++) {
            printf(" %08X", (unsigned)hash_words(0x80000000u + block * 0x10000 + sub * 0x400, 0x400 / 4));
        }
        printf("\n");
        return true;
    }
    if (strcmp(cmd, "dump") == 0) {
        unsigned addr = 0, words = 0;
        sscanf(args, "%x %u", &addr, &words);
        for (unsigned i = 0; i < words; i += 8) {
            printf("[q64dc] DUMP %08X", addr + i * 4);
            for (unsigned j = i; j < words && j < i + 8; j++) {
                printf(" %08X", (unsigned)MEM_W(0, addr + j * 4));
            }
            printf("\n");
        }
        return true;
    }
    if (strcmp(cmd, "scan") == 0) {
        unsigned hi = 0, found = 0;
        sscanf(args, "%x", &hi);
        for (uint32_t addr = 0x80000000u; addr < 0x80000000u + RDRAM_SIZE && found < 64; addr += 4) {
            uint32_t v = (uint32_t)MEM_W(0, addr);
            if (v >> 16 == hi) {
                printf("[q64dc] SCAN %08X %08X\n", (unsigned)addr, (unsigned)v);
                found++;
            }
        }
        printf("[q64dc] SCAN %04X: %u found\n", hi, found);
        return true;
    }
    if (strcmp(cmd, "loop") == 0) {
        pc = -1;
        return true;
    }
    printf("[q64dc] test: unknown command: %s\n", line);
    return true;
}

void test_input(uint16_t* buttons, int8_t* stick_x, int8_t* stick_y) {
    if (!active) {
        return;
    }
    for (int i = 0; i < watch_count; i++) {
        uint16_t v = MEM_HU(0, watch_addr[i]);
        if (v != watch_value[i]) {
            printf("[q64dc] test: watch %08X: %u -> %u (frame %u, buttons %04X)\n", (unsigned)watch_addr[i],
                   (unsigned)watch_value[i], (unsigned)v, (unsigned)stat_swap_count, (unsigned)held_buttons);
            watch_value[i] = v;
        }
    }
    if (burn_ms) {
        uint64_t until = timer_ms_gettime64() + burn_ms;
        while (timer_ms_gettime64() < until) {
        }
    }
    polls++;
#ifdef LOCKSTEP
    // The virtual clock, which follows the retraces the game saw.
    uint64_t now = events_now_us() / 1000;
#else
    uint64_t now = frame_time ? (uint64_t)polls * 1000 / 30 : timer_ms_gettime64();
#endif
    // A few commands can finish in one poll; the rest wait for later polls.
    for (int guard = 0; guard < 16 && pc < line_count; guard++) {
        if (!step_started) {
            step_started = true;
            step_start = now;
        }
        if (!step(now)) {
            break;
        }
        next();
    }
    if (pc >= line_count) {
        printf("[q64dc] test: script finished\n");
        active = false;
    }
    *buttons |= held_buttons;
    if (held_x != 0 || held_y != 0) {
        *stick_x = held_x;
        *stick_y = held_y;
    } else if (stick_latched) {
        *stick_x = latched_x;
        *stick_y = latched_y;
    }
}
