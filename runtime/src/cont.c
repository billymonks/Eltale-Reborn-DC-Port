// Serial interface: N64 controllers read from Dreamcast controllers, plus the Rumble Pak calls.

#include <kos.h>
#include <math.h>

#include "test.h"
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

// Dreamcast A/B are N64 A/B, the D-pad is the C buttons (the four spirits in battle), X and the
// left trigger are Z, Y and the right trigger are R, Start is Start.
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
        scale_stick(st->joyx, -st->joyy, &pads[i].stick_x, &pads[i].stick_y);
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
    pak_flush_idle();
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
