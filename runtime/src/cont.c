// Serial interface: N64 controllers read from Dreamcast controllers, plus the Rumble Pak calls.

#include <kos.h>
#include <dc/maple/purupuru.h>
#include <math.h>
#include <stdlib.h>

#include "audio_hle.h"
#include "profile.h"
#include "test.h"
#include "ultra.h"
#include "gfx.h"

#define MAXCONTROLLERS 4

#define CONT_TYPE_NORMAL       0x0005
#define CONT_CARD_ON           0x01
#define CONT_NO_RESPONSE_ERROR 0x08

#define PFS_ERR_DEVICE 11

// N64 button bits.
#define N64_A       0x8000
#define N64_B       0x4000
#define N64_Z       0x2000
#define N64_START   0x1000
#define N64_D_UP    0x0800
#define N64_D_DOWN  0x0400
#define N64_D_LEFT  0x0200
#define N64_D_RIGHT 0x0100
#define N64_L       0x0020
#define N64_R       0x0010
// No N64 button uses this bit, and Quest 64 never reads it. Dreamcast X maps here so a mod can
// bind it (Eltale Reborn: sprint) without taking Z, which opens chests.
#define N64_X       0x0040
#define N64_C_UP    0x0008
#define N64_C_DOWN  0x0004
#define N64_C_LEFT  0x0002
#define N64_C_RIGHT 0x0001

#define TRIGGER_THRESHOLD 128

// Sega's soft reset: A, B, X, Y and Start held together on any controller. In the game it starts
// over at the title screen; on the title screen or the intro before it, it leaves for the BIOS menu.
#define RESET_CHORD (CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START)

#define GAME_MODE         0x8007B2E0u  // u16
#define GAME_STATE        0x8007B2E4u  // u32
#define GAME_MODE_TITLE   3
#define GAME_MODE_INTRO   4
// When a mode's loop returns with this set, the main loop (func_80000C50) initializes the game
// again, the way the ending goes back to the intro.
#define GAME_STATE_REINIT 0x8000

// The music (UpdateBGM and func_80026658, once a frame): the song asked for, and while bit 0 of
// the flags is set, frames to wait before the change.
#define BGM_NEXT        0x8008FCC1u  // s8, -1 for none
#define BGM_FLAGS       0x8008FCC2u  // u16
#define BGM_DELAY       0x8008FCC4u  // u16
#define BGM_CHANGE      0x1
#define BGM_UPDATE_FUNC 0x80026658u
#define SEQ_STOP_FUNC   0x800253F0u  // (player): stops a sequence player's song

static bool reset_pending;
// The chord's buttons stay away from the game until they are all let go.
static bool reset_held;

static void sfx_rumble_tick(void);

typedef struct {
    bool connected;
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
} PadState;

static PadState pads[MAXCONTROLLERS];
static uint8_t num_channels = MAXCONTROLLERS;

// The Dreamcast stick reports -128..127 per axis and rests within a few units of the centre; an
// N64 stick reaches about 80 at full tilt. Below STICK_DEADZONE (as a distance from the centre)
// the stick is centred, and from there to STICK_FULL it ramps up to N64_STICK_MAX, keeping its
// direction. STICK_FULL is under 127 so that worn sticks and the diagonals still reach full speed.
#define STICK_DEADZONE 14
#define STICK_FULL     110
#define N64_STICK_MAX  80

static void scale_stick(int x, int y, int8_t* out_x, int8_t* out_y) {
    float r = sqrtf((float)(x * x + y * y));
    if (r <= STICK_DEADZONE) {
        *out_x = 0;
        *out_y = 0;
        return;
    }
    float t = (r - STICK_DEADZONE) / (STICK_FULL - STICK_DEADZONE);
    if (t > 1.0f) {
        t = 1.0f;
    }
    float k = t * N64_STICK_MAX / r;
    *out_x = (int8_t)lrintf(x * k);
    *out_y = (int8_t)lrintf(y * k);
}

// Dreamcast A/B are N64 A/B. The D-pad is the C buttons (the four spirits in battle). Y is R
// (the item menu). X is the unused N64_X bit. The left trigger is L, the right trigger is Z
// (chests), Start is Start.
static uint16_t map_buttons(const cont_state_t* st) {
    uint32_t b = st->buttons;
    uint16_t out = 0;
    if (b & CONT_A) out |= N64_A;
    if (b & CONT_B) out |= N64_B;
    if (b & CONT_X) out |= N64_X;
    if (b & CONT_Y) out |= N64_R;
    if (b & CONT_START) out |= N64_START;
    if (b & CONT_DPAD_UP) out |= N64_C_UP;
    if (b & CONT_DPAD_DOWN) out |= N64_C_DOWN;
    if (b & CONT_DPAD_LEFT) out |= N64_C_LEFT;
    if (b & CONT_DPAD_RIGHT) out |= N64_C_RIGHT;
    if (st->ltrig >= TRIGGER_THRESHOLD) out |= N64_L;
    if (st->rtrig >= TRIGGER_THRESHOLD) out |= N64_Z;
    return out;
}

static void exit_to_bios(void) {
    printf("[q64dc] soft reset on the title screen: leaving for the BIOS menu\n");
    pak_flush_now();
    // exit() runs the C++ destructors, and the runtime's own threads (retraces, disc reads) would
    // otherwise still reach the scheduler's tables while they go.
    sched_lock();
    arch_set_exit_path(ARCH_EXIT_MENU);
    exit(0);
}

void cont_soft_reset(void) {
    uint16_t mode = MEM_HU(0, GAME_MODE);
    if (mode == GAME_MODE_TITLE || mode == GAME_MODE_INTRO) {
        exit_to_bios();
    }
    // Before the main loop's first pass, or for a moment between rooms; the music isn't set up
    // for soft_reset_stop_music in the first.
    if (mode == 0) {
        return;
    }
    printf("[q64dc] soft reset in game mode %u: back to the title screen\n", (unsigned)mode);
    reset_pending = true;
}

// The ending's way out stops its song before the main loop initializes the game again; the other
// modes' loops don't, and a song still playing then never stops.
void soft_reset_stop_music(uint8_t* rdram, recomp_context* ctx) {
    if (!reset_pending) {
        return;
    }
    MEM_B(0, BGM_NEXT) = -1;
    MEM_HU(0, BGM_FLAGS) |= BGM_CHANGE;
    MEM_HU(0, BGM_DELAY) = 0;
    LOOKUP_FUNC(BGM_UPDATE_FUNC)(rdram, ctx);
    // The field starts its rooms' songs on the player itself, past the music's own record of them.
    ctx->r4 = 0;
    LOOKUP_FUNC(SEQ_STOP_FUNC)(rdram, ctx);
}

void soft_reset_reinit(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    if (reset_pending) {
        reset_pending = false;
        MEM_HU(0, GAME_MODE) = GAME_MODE_TITLE;
    }
}

void cont_poll(void) {
    uint32_t chord_buttons = 0;
    bool chord = false;
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        maple_device_t* dev = maple_enum_type(i, MAPLE_FUNC_CONTROLLER);
        cont_state_t* st = dev != NULL ? (cont_state_t*)maple_dev_status(dev) : NULL;
        if (st == NULL) {
            pads[i].connected = false;
            continue;
        }
        pads[i].connected = true;
        pads[i].buttons = map_buttons(st);
        scale_stick(st->joyx, -st->joyy, &pads[i].stick_x, &pads[i].stick_y);
        chord_buttons |= st->buttons & RESET_CHORD;
        chord |= (st->buttons & RESET_CHORD) == RESET_CHORD;
    }
    if (chord && !reset_held) {
        reset_held = true;
        cont_soft_reset();
    } else if (chord_buttons == 0) {
        reset_held = false;
    }
    if (reset_held) {
        for (int i = 0; i < MAXCONTROLLERS; i++) {
            pads[i].buttons = 0;
        }
    }
    // Until the main loop starts over: whatever the mode's loop was doing, it ends at its next
    // check of the mode.
    if (reset_pending) {
        MEM_HU(0, GAME_MODE) = GAME_MODE_TITLE;
        MEM_W(0, GAME_STATE) |= GAME_STATE_REINIT;
    }
    // The game needs a controller in port 1 to get past its checks; without one (an emulator
    // run with no input), report an idle controller.
    if (!pads[0].connected) {
        pads[0].connected = true;
        pads[0].buttons = 0;
        pads[0].stick_x = 0;
        pads[0].stick_y = 0;
    }
    test_input(&pads[0].buttons, &pads[0].stick_x, &pads[0].stick_y);
#ifdef PERF_HUD
    // Both triggers and X. Quest 64 never reads X, and the three are swallowed so L and the
    // right trigger do not reach the game while the chord is held.
    {
        static int chord_was;
        int l = (pads[0].buttons & N64_L) != 0;
        int r = (pads[0].buttons & N64_Z) != 0;
        int x = (pads[0].buttons & N64_X) != 0;
        if (l && r && x) {
            if (!chord_was) {
                profile_chord();
            }
            pads[0].buttons &= (uint16_t)~(N64_L | N64_Z | N64_X);
        }
        chord_was = l && r && x;
    }
    // Both triggers and Y cycle the audio fast path. Y is the N64 R button (item menu), so the
    // chord is swallowed and does not open the menu. X stays the profiler.
    {
        static int audio_was;
        int l = (pads[0].buttons & N64_L) != 0;
        int r = (pads[0].buttons & N64_Z) != 0;
        int y = (pads[0].buttons & N64_R) != 0;
        int x = (pads[0].buttons & N64_X) != 0;
        if (l && r && y && !x) {
            if (!audio_was) {
                audio_hle_cycle_mode();
            }
            pads[0].buttons &= (uint16_t)~(N64_L | N64_Z | N64_R);
        }
        audio_was = l && r && y && !x;
    }
    // Both triggers and A cycle frame skip. A is swallowed so the chord does not also confirm.
    {
        static int skip_was;
        int l = (pads[0].buttons & N64_L) != 0;
        int r = (pads[0].buttons & N64_Z) != 0;
        int a = (pads[0].buttons & N64_A) != 0;
        int x = (pads[0].buttons & N64_X) != 0;
        int y = (pads[0].buttons & N64_R) != 0;
        if (l && r && a && !x && !y) {
            if (!skip_was) {
                gfx_skip_cycle();
            }
            pads[0].buttons &= (uint16_t)~(N64_L | N64_Z | N64_A);
        }
        skip_was = l && r && a && !x && !y;
    }
#endif
#ifdef GFX_DIAG
    // Both triggers and B cycle the set of renderer optimizations. B is swallowed so it does not
    // cancel.
    {
        static int diag_was;
        int l = (pads[0].buttons & N64_L) != 0;
        int r = (pads[0].buttons & N64_Z) != 0;
        int b = (pads[0].buttons & N64_B) != 0;
        int other = (pads[0].buttons & (N64_A | N64_X | N64_R)) != 0;
        if (l && r && b && !other) {
            if (!diag_was) {
                gfx_diag_cycle();
            }
            pads[0].buttons &= (uint16_t)~(N64_L | N64_Z | N64_B);
        }
        diag_was = l && r && b && !other;
    }
    // Both triggers and D-pad down (the N64's C-down) cycle the diagnostic modes.
    {
        static int mode_was;
        int l = (pads[0].buttons & N64_L) != 0;
        int r = (pads[0].buttons & N64_Z) != 0;
        int cd = (pads[0].buttons & N64_C_DOWN) != 0;
        if (l && r && cd) {
            if (!mode_was) {
                gfx_diag_mode_cycle();
            }
            pads[0].buttons &= (uint16_t)~(N64_L | N64_Z | N64_C_DOWN);
        }
        mode_was = l && r && cd;
    }
#endif
    pak_flush_idle();
    sfx_rumble_tick();
}

static uint8_t write_status(uint32_t status_array) {
    uint8_t pattern = 0;
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        uint32_t entry = status_array + 4 * i;
        if (pads[i].connected && i < num_channels) {
            MEM_HU(0, entry) = CONT_TYPE_NORMAL;
            // The Controller Pak saves live on port 1.
            MEM_BU(2, entry) = i == 0 ? CONT_CARD_ON : 0;
            MEM_BU(3, entry) = 0;
            pattern |= 1 << i;
        } else {
            MEM_BU(3, entry) = CONT_NO_RESPONSE_ERROR;
        }
    }
    return pattern;
}

static void send_si_done(uint32_t mq) {
    // The SI interrupt goes to the queue registered for OS_EVENT_SI; games normally pass the same
    // queue to the osCont calls.
    sched_check_yield();
    if (!events_post(OS_EVENT_SI) && mq != 0) {
        mq_send_nonblock(mq, 0);
    }
}

void osContInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    cont_poll();
    MEM_BU(0, ctx->r5) = write_status(ctx->r6);
    ctx->r2 = 0;
}

void osContSetCh_recomp(uint8_t* rdram, recomp_context* ctx) {
    num_channels = (uint8_t)ctx->r4 > MAXCONTROLLERS ? MAXCONTROLLERS : (uint8_t)ctx->r4;
    ctx->r2 = 0;
}

void osContStartQuery_recomp(uint8_t* rdram, recomp_context* ctx) {
    cont_poll();
    send_si_done(ctx->r4);
    ctx->r2 = 0;
}

void osContGetQuery_recomp(uint8_t* rdram, recomp_context* ctx) {
    write_status(ctx->r4);
}

void osContStartReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    cont_poll();
    send_si_done(ctx->r4);
    ctx->r2 = 0;
}

void osContGetReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t data = ctx->r4;
    for (int i = 0; i < num_channels; i++) {
        uint32_t entry = data + 6 * i;
        if (pads[i].connected) {
            MEM_HU(0, entry) = pads[i].buttons;
            MEM_B(2, entry) = pads[i].stick_x;
            MEM_B(3, entry) = pads[i].stick_y;
            MEM_BU(4, entry) = 0;
        } else {
            MEM_BU(4, entry) = CONT_NO_RESPONSE_ERROR;
        }
    }
}

// Jump pack. Quest 64 never starts the Rumble Pak (osMotorStart has no callers). The PC recomp
// rumbles the same impacts from the sound that actually plays, and this is that table: low and high
// are the two motors, 0-255, and frames is how many retraces the pulse holds. A hit landing (0x18)
// is both a staff hit and Brian getting hurt. A miss plays the swing (0x17), and footsteps reuse
// the door effect (0x21); neither of those rumbles in a fight. A flyby or a swing only
// rumbles while a scene has Brian.
//
// The pack has one motor. Strength is the louder of the two, on the pack's 1-7 scale, and the mix
// sets the rate: a heavy low motor is slower, a sharp high one is faster. The pulse is sent when
// the controllers are next read and stopped after that many retraces. A weaker pulse waits out
// the one already playing. The decay bit of the frequency byte makes a Sega pack ignore the command,
// so the rate stays under it.

#define PLAYER_STATE       0x8007BAB8u
#define PLAYER_STATE_TALK  7
#define PLAYER_STATE_ITEM  11
#define BATTLE_STATE       0x8008C592u
#define BATTLE_ACTIVE      0x0001
#define GAME_MODE_GAMEPLAY 1
#define GAME_MODE_ENDING   5

#define SFX_COUNT 0x46

struct SfxRumble {
    uint8_t low;
    uint8_t high;
    uint8_t frames;
    bool cutscene_only;
};

static const struct SfxRumble sfx_rumble_for[SFX_COUNT] = {
    [0x07] = {220, 160, 16, false}, // Solvaring's rock, Mammon's last spell
    [0x0A] = {150, 110, 12, false}, // an enemy beaten
    [0x0C] = {200, 150, 14, false}, // a column bursting up
    [0x0D] = {240, 190, 16, false}, // an explosion
    [0x0F] = {100, 150, 8, false},  // Fire Ball
    [0x10] = {255, 170, 20, false}, // Rolling Rock, an earthquake
    [0x15] = {60, 90, 6, true},     // a projectile flying past
    [0x16] = {140, 160, 12, false}, // Ultimate Wind
    [0x17] = {80, 130, 7, true},    // a swing that missed, and Wind Cutter
    [0x18] = {230, 200, 16, false}, // a hit that landed: staff hit, or Brian hurt
    [0x29] = {200, 170, 16, false}, // a big area spell
    [0x2A] = {150, 140, 12, false}, // Cyclone
    [0x2C] = {140, 100, 10, false}, // Rock
    [0x2E] = {200, 150, 14, false}, // dark magic
    [0x31] = {200, 160, 14, false}, // Beigis's second spell
    [0x38] = {90, 140, 8, false},   // Ice Knife
};

// The sound can start on the audio thread. The pack is commanded from the controller poll, on the
// game thread, which is also the thread that writes the VMU.
static int pulse_low;
static int pulse_high;
static int pulse_frames;
static uint32_t pulse_vi;
static uint32_t pulse_seq;
static uint32_t pend_seq;
static int pend_low;
static int pend_high;
static int pend_frames;

static int rumble_peak(int low, int high) {
    return low > high ? low : high;
}

static bool cutscene_playing(void) {
    uint16_t mode = MEM_HU(0, GAME_MODE);
    uint16_t state;

    if (mode == GAME_MODE_INTRO || mode == GAME_MODE_ENDING) {
        return true;
    }
    if (mode != GAME_MODE_GAMEPLAY) {
        return false;
    }
    if (MEM_HU(0, BATTLE_STATE) & BATTLE_ACTIVE) {
        return false;
    }
    state = MEM_HU(0, PLAYER_STATE);
    return state == PLAYER_STATE_TALK || state == PLAYER_STATE_ITEM;
}

static void request_rumble_pulse(int low, int high, int frames) {
    int old = irq_disable();
    int playing = pulse_frames > 0 ? rumble_peak(pulse_low, pulse_high) : 0;
    int pending = pend_seq != pulse_seq ? rumble_peak(pend_low, pend_high) : 0;
    int blocked = playing > pending ? playing : pending;
    bool busy = pulse_frames > 0 || pend_seq != pulse_seq;

    if (busy && blocked > rumble_peak(low, high)) {
        irq_restore(old);
        return;
    }
    pend_low = low;
    pend_high = high;
    pend_frames = frames;
    pend_seq++;
    irq_restore(old);
}

static maple_device_t* jump_pack(void) {
    return maple_enum_type(0, MAPLE_FUNC_PURUPURU);
}

static void jump_pack_start(int low, int high) {
    maple_device_t* dev = jump_pack();
    purupuru_effect_t fx = {0};
    int peak = rumble_peak(low, high);
    int sum = low + high;
    int freq;
    int power;

    if (dev == NULL) {
        return;
    }
    power = (peak * 7 + 127) / 255;
    if (power < 1) {
        power = 1;
    }
    freq = sum > 0 ? 12 + (high * 32) / sum : 20;
    fx.motor = 1;
    fx.cont = true;
    fx.fpow = (uint32_t)power;
    fx.freq = (uint8_t)freq;
    purupuru_rumble(dev, &fx);
}

static void jump_pack_stop(void) {
    maple_device_t* dev = jump_pack();
    purupuru_effect_t fx = {0};

    if (dev == NULL) {
        return;
    }
    fx.motor = 1;
    purupuru_rumble(dev, &fx);
}

static void sfx_rumble_tick(void) {
    int low = 0;
    int high = 0;
    bool start = false;
    bool stop = false;
    int old = irq_disable();

    if (pend_seq != pulse_seq) {
        pulse_low = pend_low;
        pulse_high = pend_high;
        pulse_frames = pend_frames;
        pulse_vi = stat_vi_count;
        pulse_seq = pend_seq;
        low = pulse_low;
        high = pulse_high;
        start = true;
    } else if (pulse_frames > 0) {
        uint32_t passed = stat_vi_count - pulse_vi;
        if (passed > 0) {
            pulse_vi = stat_vi_count;
            if (passed >= (uint32_t)pulse_frames) {
                pulse_frames = 0;
                stop = true;
            } else {
                pulse_frames -= (int)passed;
            }
        }
    }
    irq_restore(old);
    if (start) {
        jump_pack_start(low, high);
    } else if (stop) {
        jump_pack_stop();
    }
}

void sfx_rumble(uint8_t* rdram, int id, int volume) {
    const struct SfxRumble* fx;

    (void)rdram;
    if (volume == 0 || id < 0 || id >= SFX_COUNT) {
        return;
    }
    fx = &sfx_rumble_for[id];
    if ((fx->low == 0 && fx->high == 0) || (fx->cutscene_only && !cutscene_playing())) {
        return;
    }
    request_rumble_pulse(fx->low, fx->high, fx->frames);
}

// Port 1 holds the Controller Pak, so there's no Rumble Pak to find.
void osMotorInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = PFS_ERR_DEVICE;
}

void osMotorStart_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}

void osMotorStop_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}
