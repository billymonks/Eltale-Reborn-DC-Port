// Serial interface: N64 controllers read from Dreamcast controllers, plus the Rumble Pak calls.

#include <kos.h>

#include "ultra.h"

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
#define N64_C_UP    0x0008
#define N64_C_DOWN  0x0004
#define N64_C_LEFT  0x0002
#define N64_C_RIGHT 0x0001

#define TRIGGER_THRESHOLD 128

typedef struct {
    bool connected;
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
} PadState;

static PadState pads[MAXCONTROLLERS];
static uint8_t num_channels = MAXCONTROLLERS;

static int8_t scale_axis(int value) {
    // Dreamcast sticks span -128..127; an N64 stick reaches about +-80.
    int v = value * 80 / 127;
    return (int8_t)(v > 80 ? 80 : v < -80 ? -80 : v);
}

// Default mapping, to be revisited once the game is playable: Dreamcast A/B are N64 A/B, the
// D-pad is the C buttons, X is Z, Y is R, the triggers are Z and R, Start is Start.
static uint16_t map_buttons(const cont_state_t* st) {
    uint32_t b = st->buttons;
    uint16_t out = 0;
    if (b & CONT_A) out |= N64_A;
    if (b & CONT_B) out |= N64_B;
    if (b & CONT_X) out |= N64_Z;
    if (b & CONT_Y) out |= N64_R;
    if (b & CONT_START) out |= N64_START;
    if (b & CONT_DPAD_UP) out |= N64_C_UP;
    if (b & CONT_DPAD_DOWN) out |= N64_C_DOWN;
    if (b & CONT_DPAD_LEFT) out |= N64_C_LEFT;
    if (b & CONT_DPAD_RIGHT) out |= N64_C_RIGHT;
    if (st->ltrig >= TRIGGER_THRESHOLD) out |= N64_Z;
    if (st->rtrig >= TRIGGER_THRESHOLD) out |= N64_R;
    return out;
}

void cont_poll(void) {
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        maple_device_t* dev = maple_enum_type(i, MAPLE_FUNC_CONTROLLER);
        cont_state_t* st = dev != NULL ? (cont_state_t*)maple_dev_status(dev) : NULL;
        if (st == NULL) {
            pads[i].connected = false;
            continue;
        }
        pads[i].connected = true;
        pads[i].buttons = map_buttons(st);
        pads[i].stick_x = scale_axis(st->joyx);
        pads[i].stick_y = scale_axis(-st->joyy);
    }
    // The game needs a controller in port 1 to get past its checks; without one (an emulator
    // run with no input), report an idle controller.
    if (!pads[0].connected) {
        pads[0].connected = true;
        pads[0].buttons = 0;
        pads[0].stick_x = 0;
        pads[0].stick_y = 0;
    }
#ifdef Q64DC_AUTOPRESS
    // Headless test builds: tap Start, then A, every few seconds to get through the menus.
    static uint32_t polls;
    uint32_t phase = polls++ % 360;
    if (phase < 6) {
        pads[0].buttons |= N64_START;
    } else if (phase >= 180 && phase < 186) {
        pads[0].buttons |= N64_A;
    }
#endif
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
