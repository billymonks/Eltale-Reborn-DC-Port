// The field camera (func_80013008) and the battle camera (func_80013A20) in native code, from
// Quest64-Recomp's patches/camera.c (Modern controls and camera by CanadaX21).
// Classic controls run the original camera. Modern controls add the free camera on the triggers, the
// focus camera on B, the field camera's zoom on both triggers (cont.c), a smooth battle aim and action
// cameras that swing behind Brian. The PC's camera tilt has no input on a Dreamcast pad and is left out.
//
// The game's data is read through rdram, where halfwords and bytes are swizzled, so the camera is
// addressed by its N64 address and the offsets below rather than as a struct.
// A camera (0x88 bytes): 0x00 mode (0/2 field, 1 fixed, 3 fixed aim, 4 battle, 5 after battle),
// 0x04 timer, u16 0x08 flags (0x04 turn to orbit_yaw, 0x20 orbit behind target, 0x40 action camera),
// 0x80 the target's transform. A transform: position at 0x00, y rotation at 0x10.
// A floor poll: x (the probe radius going in), y (the floor coming out), z, normal, u16 0x18 flags
// (0x8000: no floor).

#include "recomp.h"

#undef RECOMP_RB
#define RECOMP_RB cam_rb

static uint8_t* cam_rdram;
static uint8_t* cam_rb;
static recomp_context* cam_ctx;

#define CAM_MODE(c)         MEM_W(0x00, c)
#define CAM_TIMER(c)        MEM_W(0x04, c)
#define CAM_FLAGS(c)        MEM_HU(0x08, c)
#define CAM_X(c)            MEM_F(0x0C, c)
#define CAM_Y(c)            MEM_F(0x10, c)
#define CAM_Z(c)            MEM_F(0x14, c)
#define CAM_FOCUS_X(c)      MEM_F(0x18, c)
#define CAM_FOCUS_Y(c)      MEM_F(0x1C, c)
#define CAM_FOCUS_Z(c)      MEM_F(0x20, c)
#define CAM_PITCH(c)        MEM_F(0x24, c)
#define CAM_TARGET_YAW(c)   MEM_F(0x28, c)  // Brian walks relative to this
#define CAM_YAW(c)          MEM_F(0x2C, c)
#define CAM_ORBIT_SPEED(c)  MEM_F(0x3C, c)
#define CAM_SPEED_X(c)      MEM_F(0x40, c)
#define CAM_SPEED_Y(c)      MEM_F(0x44, c)
#define CAM_SPEED_Z(c)      MEM_F(0x48, c)
#define CAM_DISTANCE(c)     MEM_F(0x4C, c)  // ideal planar distance
#define CAM_ORBIT_YAW(c)    MEM_F(0x54, c)
#define CAM_HEIGHT(c)       MEM_F(0x58, c)  // local camera height
#define CAM_PULL(c)         MEM_F(0x70, c)
#define CAM_DRAG(c)         MEM_F(0x74, c)
#define CAM_ORBIT_PULL(c)   MEM_F(0x78, c)
#define CAM_ORBIT_DRAG(c)   MEM_F(0x7C, c)
#define CAM_TARGET(c)       MEM_WU(0x80, c)
#define CAMERA_WORDS        (0x88 / 4)

#define POS_X(t)            MEM_F(0x00, t)
#define POS_Y(t)            MEM_F(0x04, t)
#define POS_Z(t)            MEM_F(0x08, t)
#define YROT(t)             MEM_F(0x10, t)

#define gGameState              MEM_W(0, 0x8007B2E4u)   // 0x01 item menu, 0x02/0x80 text box or scene change
#define gBattleState            MEM_HU(0, 0x8008C592u)  // 0x01 in battle
#define gButtonsHeld            MEM_HU(0, 0x80092874u)
#define gButtonsPressed         MEM_HU(0, 0x80092876u)
#define gStickX                 MEM_B(0, 0x80092871u)
#define gStickY                 MEM_B(0, 0x80092872u)
#define gSubmapHeader           MEM_WU(0, 0x80084F1Cu)  // changes with every area
#define gBrianY                 MEM_F(0, 0x8007BAD0u)
#define gCameraFloorFlags       MEM_HU(0, 0x80086BBCu)
#define gBattleCameraYawOffset  MEM_F(0, 0x80086EFCu)
#define gBattleCenterX          MEM_F(0, 0x8008C59Cu)
#define gBattleCenterZ          MEM_F(0, 0x8008C5A0u)
#define BRIAN_TRANSFORM         0x8007BACCu
#define CAMERA_FLOOR_POLL       0x80086BA0u
#define DEFAULT_CAMERA          0x80086B00u             // the battle camera saved during an action camera
#define B_BUTTON 0x4000
#define L_TRIG   0x0020

// The game functions' stack: the fifth argument's slot at 0x10, the zoom's floor poll at 0x18.
#define FRAME_SIZE 0x38
#define FRAME_PROBE 0x18

void func_80013D30(uint8_t* rdram, recomp_context* ctx);
void func_80013008(uint8_t* rdram, recomp_context* ctx);
void func_80013D3C(uint8_t* rdram, recomp_context* ctx);
void func_80013DE0(uint8_t* rdram, recomp_context* ctx);
void func_8000EE60(uint8_t* rdram, recomp_context* ctx);
void func_80014720(uint8_t* rdram, recomp_context* ctx);
void func_80023210(uint8_t* rdram, recomp_context* ctx);

#define PI 3.141592653589793
#define TWO_PI 6.283185307179586

#define SNAP_FRAMES 16
#define SNAP_PULL 0.2f
#define FIELD_PULL 0.005f
#define FIELD_DRAG 0.8f
#define BATTLE_PULL 0.5f
#define BATTLE_DRAG 0.4f
#define BATTLE_TURN 0.10f
#define TRIGGER_ON 0.5f
#define ZOOM_OUT 1.0f
#define ZOOM_RISE 0.5f
#define ZOOM_EASE 0.15f
#define ROOF_H 15.0f
#define ROOF_STEP 0.05f
#define ZOOM_MIN 0.2f
#define NO_FLOOR -1.0e9f
#define FREE_ENTER 0.25f
#define FREE_DEAD 0.10f
#define FREE_YAW_RATE 0.069f
#define FREE_FOLLOW 0.5f
#define FAST_Y 0.35f
#define FAST_Y_FRAMES 30
#define AIM_K 0.0016f
#define AIM_DCAP 33.0f
#define SIDE_MIN 0.35f
#define SWING_EASE 0.2f
#define LOCK_LEAD 0.157f

// Camera turn speed, 0.1 to 2.0 (the PC's 10% to 200%).
#ifndef CAMERA_TURN_SPEED
#define CAMERA_TURN_SPEED 1.0f
#endif

static inline uint32_t float_bits(float f) {
    union { float f; uint32_t u; } v = { f };
    return v.u;
}

// CameraSetup(cam)
static void camera_setup(uint32_t cam) {
    cam_ctx->r4 = cam;
    func_80013D30(cam_rdram, cam_ctx);
}

// CameraTurnToAngle(cam, target position)
static void camera_turn_to_angle(uint32_t cam, uint32_t target) {
    cam_ctx->r4 = cam;
    cam_ctx->r5 = target;
    func_80013DE0(cam_rdram, cam_ctx);
}

// PollWorldCollision(x, y, z, flags, out)
static void poll_world_collision(float x, float y, float z, uint16_t flags, uint32_t out) {
    cam_ctx->f12.fl = x;
    cam_ctx->f14.fl = y;
    cam_ctx->r6 = float_bits(z);
    cam_ctx->r7 = flags;
    MEM_W(0x10, cam_ctx->r29) = (int32_t)out;
    func_8000EE60(cam_rdram, cam_ctx);
}

// CameraAboveNpcs(cam, height)
static float camera_above_npcs(uint32_t cam, float height) {
    cam_ctx->r4 = cam;
    cam_ctx->r5 = float_bits(height);
    func_80014720(cam_rdram, cam_ctx);
    return cam_ctx->f0.fl;
}

// CameraFollowHeight(cam, target)
static void camera_follow_height(uint32_t cam, uint32_t target) {
    cam_ctx->r4 = cam;
    cam_ctx->r5 = target;
    func_80013D3C(cam_rdram, cam_ctx);
}

static float game_atan2(float y, float x) {
    cam_ctx->f12.fl = y;
    cam_ctx->f14.fl = x;
    func_80023210(cam_rdram, cam_ctx);
    return cam_ctx->f0.fl;
}

static float game_sinf(float x) {
    cam_ctx->f12.fl = x;
    ultra_sinf(cam_rdram, cam_ctx);
    return cam_ctx->f0.fl;
}

static float game_cosf(float x) {
    cam_ctx->f12.fl = x;
    ultra_cosf(cam_rdram, cam_ctx);
    return cam_ctx->f0.fl;
}

static float wrap(float a) {
    if (a > PI) {
        a -= TWO_PI;
    } else if (a < -PI) {
        a += TWO_PI;
    }
    return a;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void copy_camera(uint32_t dst, uint32_t src) {
    for (uint32_t i = 0; i < CAMERA_WORDS; i++) {
        MEM_W(i * 4, dst) = MEM_W(i * 4, src);
    }
}

// Returns whether the eye was swung aside up close, which keeps Brian's walking direction.
static int field_follow(uint32_t cam, uint32_t target) {
    int behind = 0;
    float dx = POS_X(target) - CAM_X(cam);
    float dz = POS_Z(target) - CAM_Z(cam);
    float dist = sqrtf(dx * dx + dz * dz);
    float ratio, diff;
    float pushX = 0.0f;
    float pushZ = 0.0f;

    if (dist != 0.0f) {
        if (CAM_DISTANCE(cam) < dist) {
            ratio = CAM_DISTANCE(cam) / dist;
            pushX = (dx - dx * ratio) * CAM_PULL(cam);
            pushZ = (dz - dz * ratio) * CAM_PULL(cam);
        } else if (dist <= CAM_DISTANCE(cam)) {
            ratio = CAM_DISTANCE(cam) / dist;
            pushX = (dx - dx * ratio) * CAM_PULL(cam) * 0.5;
            pushZ = (dz - dz * ratio) * CAM_PULL(cam) * 0.5;
        }
    }
    CAM_SPEED_X(cam) += pushX;
    CAM_SPEED_Z(cam) += pushZ;
    CAM_X(cam) += CAM_SPEED_X(cam);
    CAM_Z(cam) += CAM_SPEED_Z(cam);
    CAM_SPEED_X(cam) *= CAM_DRAG(cam);
    CAM_SPEED_Z(cam) *= CAM_DRAG(cam);
    if (dist < 25.0f && !(gGameState & 0x80)) {
        diff = wrap(YROT(target) - CAM_TARGET_YAW(cam));
        if (diff < 1.5707963267948966 && diff > -1.5707963267948966) {
            CAM_ORBIT_YAW(cam) = ((diff < 0.0f) ? 1.5707963267948966 : -1.5707963267948966) + YROT(target);
        }
        CAM_FLAGS(cam) |= 4;
        behind = 1;
    } else if (CAM_FLAGS(cam) & 0x20) {
        CAM_ORBIT_YAW(cam) = wrap(YROT(target) + PI);
    }
    camera_turn_to_angle(cam, target);
    return behind;
}

// With `fast` the eye eases straight to its height instead of using the original spring.
static void field_height(uint32_t cam, uint32_t target, int fast) {
    const uint32_t poll = CAMERA_FLOOR_POLL;
    float floor, want, dy;

    MEM_F(0x00, poll) = 4.0f;
    poll_world_collision(CAM_X(cam), CAM_Y(cam), CAM_Z(cam), 0x602, poll);
    CAM_X(cam) = MEM_F(0x00, poll);
    CAM_Z(cam) = MEM_F(0x08, poll);
    if (MEM_HU(0x18, poll) & 0x8000) {
        floor = POS_Y(target);
    } else {
        floor = MEM_F(0x04, poll);
    }
    gCameraFloorFlags = MEM_HU(0x18, poll);
    if (floor < POS_Y(target) + CAM_HEIGHT(cam)) {
        floor = POS_Y(target) + CAM_HEIGHT(cam);
    }
    want = camera_above_npcs(cam, floor);
    if ((gBattleState & 1) && want < gBrianY) {
        want = gBrianY;
    }
    dy = want - CAM_Y(cam);
    if (fast) {
        CAM_Y(cam) += dy * FAST_Y;
        CAM_SPEED_Y(cam) = 0.0f;
    } else {
        dy *= CAM_PULL(cam) * ((dy < 0.0f) ? 0.2 : 0.8);
        CAM_SPEED_Y(cam) += dy;
        CAM_Y(cam) += CAM_SPEED_Y(cam);
        CAM_SPEED_Y(cam) *= CAM_DRAG(cam);
    }
}

static void field_look(uint32_t cam, uint32_t target, int behind) {
    float dx, dy, dz, dist;

    camera_follow_height(cam, target);
    dx = CAM_X(cam) - CAM_FOCUS_X(cam);
    dy = CAM_Y(cam) - CAM_FOCUS_Y(cam);
    dz = CAM_Z(cam) - CAM_FOCUS_Z(cam);
    if (dx > -0.5 && dx < 0.5 && dz > -0.5 && dz < 0.5) {
        CAM_X(cam) += 0.5;
        CAM_Z(cam) += 0.5;
    }
    dist = sqrtf(dx * dx + dz * dz);
    if (dy != 0.0f || dist != 0.0f) {
        CAM_PITCH(cam) = game_atan2(dy, dist);
    }
    if (dx != 0.0f || dz != 0.0f) {
        CAM_YAW(cam) = game_atan2(dx, dz);
        if (!behind) {
            CAM_TARGET_YAW(cam) = CAM_YAW(cam);
        }
    }
}

static void classic_field(uint32_t cam) {
    uint32_t target = CAM_TARGET(cam);
    int behind;

    camera_setup(cam);
    if (!(gGameState & 0x82) && !(gBattleState & 0x200) && !(CAM_FLAGS(cam) & 0x40)) {
        if ((gButtonsHeld & B_BUTTON) || CAM_MODE(cam) == 2) {
            CAM_FLAGS(cam) |= 0x24;
        } else {
            CAM_FLAGS(cam) &= ~0x24;
        }
    } else {
        CAM_FLAGS(cam) &= ~0x24;
    }
    behind = field_follow(cam, target);
    field_height(cam, target, 0);
    field_look(cam, target, behind);
}

// From the arena's centre past Brian, in 22.5 degree steps.
static float classic_battle_base(uint32_t subject) {
    float angle = wrap(game_atan2(gBattleCenterX - POS_X(subject), gBattleCenterZ - POS_Z(subject)) + PI);
    angle = (int32_t)(angle / 0.39269908169872414) * 0.39269908169872414 - 0.19634954084936207;
    return wrap(angle);
}

static void classic_battle(uint32_t cam) {
    if (CAM_FLAGS(cam) & 0x40) {
        CAM_TIMER(cam)--;
        if (CAM_TIMER(cam) == 0) {
            CAM_FLAGS(cam) &= ~0x40;
            copy_camera(cam, DEFAULT_CAMERA);
        }
    } else {
        CAM_ORBIT_YAW(cam) = wrap(classic_battle_base(CAM_TARGET(cam)) + gBattleCameraYawOffset);
        if (gButtonsPressed & L_TRIG) {
            gBattleCameraYawOffset = wrap(gBattleCameraYawOffset + 0.7853981633974483);
        }
    }
    cam_ctx->r4 = cam;
    func_80013008(cam_rdram, cam_ctx);
}

// The camera stick with the dead zone taken out.
static void camera_stick(float dead, float* x) {
    float sx, sy, mag, scale;
    cont_camera_stick(&sx, &sy);
    mag = sqrtf(sx * sx + sy * sy);
    if (mag <= dead) {
        *x = 0.0f;
        return;
    }
    scale = (mag > 1.0f ? 1.0f : mag);
    scale = (scale - dead) / (1.0f - dead) / mag;
    *x = sx * scale;
}

static int sTargetHeld;
static int sTargetPrev;
static int sSnapFrames;

static void read_target(void) {
    sTargetPrev = sTargetHeld;
    sTargetHeld = cont_camera_target() > TRIGGER_ON;
}

// Following the camera while walking toward it would spin Brian and the camera around each other.
static int stick_pulled_back(void) {
    int32_t x = gStickX, y = gStickY;
    if (x < 0) {
        x = -x;
    }
    return 2 * y < -(x + 16);
}

static int sZoomOn;
static int sZoomPrev;
static float sZoom;
static float sZoomCap = 1.0f;
static uint32_t sZoomArea;

static float floor_at(float x, float y, float z) {
    uint32_t probe = cam_ctx->r29 + FRAME_PROBE;
    MEM_F(0x00, probe) = 4.0f;
    poll_world_collision(x, y, z, 0x602, probe);
    if (MEM_HU(0x18, probe) & 0x8000) {
        return NO_FLOOR;
    }
    return MEM_F(0x04, probe);
}

// Backs the zoom off while the zoomed eye would be over a roof.
static void zoom_apply(uint32_t cam, uint32_t who) {
    float want, dx, dz, len, d0, h0, d1, h1, f0, f1;
    float bx = POS_X(who), by = POS_Y(who), bz = POS_Z(who);
    int held = cont_camera_zoom() > TRIGGER_ON;

    if (gSubmapHeader != sZoomArea) {
        sZoomArea = gSubmapHeader;
        sZoomCap = 1.0f;
    }
    if (held && !sZoomPrev && !(gGameState & 0x87) && !(gBattleState & 0x201)) {
        sZoomOn = !sZoomOn;
        sZoomCap = 1.0f;
    }
    sZoomPrev = held;
    d0 = CAM_DISTANCE(cam);
    h0 = CAM_HEIGHT(cam);
    if (sZoomOn) {
        dx = CAM_X(cam) - bx;
        dz = CAM_Z(cam) - bz;
        len = sqrtf(dx * dx + dz * dz);
        if (len > 1.0f) {
            dx /= len;
            dz /= len;
            d1 = d0 * (1.0f + ZOOM_OUT * sZoomCap);
            h1 = h0 + d0 * ZOOM_OUT * ZOOM_RISE * sZoomCap;
            f1 = floor_at(bx + dx * d1, by + h1, bz + dz * d1);
            f0 = floor_at(bx + dx * d0, by + h0, bz + dz * d0);
            if (f0 == NO_FLOOR) {
                f0 = by;
            }
            if (f1 != NO_FLOOR && f1 - f0 > ROOF_H) {
                sZoomCap -= ROOF_STEP;
                if (sZoomCap < ZOOM_MIN) {
                    sZoomCap = ZOOM_MIN;
                }
            }
        }
    }
    want = sZoomOn ? sZoomCap : 0.0f;
    sZoom += (want - sZoom) * ZOOM_EASE;
    if (sZoom < 0.001f) {
        sZoom = 0.0f;
        return;
    }
    CAM_DISTANCE(cam) = d0 * (1.0f + ZOOM_OUT * sZoom);
    CAM_HEIGHT(cam) = h0 + d0 * ZOOM_OUT * ZOOM_RISE * sZoom;
}

// The zoom only changes the room's camera preset for one update.
static float sSaveD, sSaveH;
static int sViewSet;

static void view_pre(uint32_t cam, uint32_t target, int field) {
    sSaveD = CAM_DISTANCE(cam);
    sSaveH = CAM_HEIGHT(cam);
    sViewSet = 1;
    if (field) {
        zoom_apply(cam, target);
    }
}

static void view_post(uint32_t cam) {
    if (sViewSet) {
        CAM_DISTANCE(cam) = sSaveD;
        CAM_HEIGHT(cam) = sSaveH;
        sViewSet = 0;
    }
}

static int sFree;
static float sFreeYaw;
static uint32_t sFreeArea;
static int sWasBattle;
static int sFastYFrames;

static void free_leave(uint32_t cam) {
    if (sFree) {
        sFree = 0;
        CAM_ORBIT_SPEED(cam) = 0.0f;
        CAM_SPEED_X(cam) = 0.0f;
        CAM_SPEED_Z(cam) = 0.0f;
    }
}

static int sAimSet;
static float sAimBase;
static int sActCam;             // 0 none, 1 Brian's swinging behind him, 2 Brian's already behind, 3 someone else's
static int sActTimer;
static float sActBehind;
static int sLockPrev;
static int sLockSnapFrames;
static int sLockEasing;

// Brian's action cameras swing behind him, and the saved battle camera is turned to match so it stays there after.
static void act_begin(uint32_t cam) {
    uint32_t t = CAM_TARGET(cam);
    uint32_t b = BRIAN_TRANSFORM;
    uint32_t s = DEFAULT_CAMERA;
    float dx = POS_X(t) - POS_X(b);
    float dz = POS_Z(t) - POS_Z(b);
    float dev, ex, ez, r, turn;

    sActTimer = CAM_TIMER(cam);
    if (dx < -2.0f || dx > 2.0f || dz < -2.0f || dz > 2.0f) {
        sActCam = 3;
        return;
    }
    sActBehind = wrap(YROT(b) + PI);
    dev = wrap(sActBehind - CAM_ORBIT_YAW(cam));
    if (sActCam != 1 && dev > -SIDE_MIN && dev < SIDE_MIN) {
        sActCam = 2;
        return;
    }
    sActCam = 1;
    ex = CAM_X(s) - POS_X(b);
    ez = CAM_Z(s) - POS_Z(b);
    r = sqrtf(ex * ex + ez * ez);
    turn = wrap(sActBehind - game_atan2(ex, ez));
    CAM_X(s) = POS_X(b) + game_sinf(sActBehind) * r;
    CAM_Z(s) = POS_Z(b) + game_cosf(sActBehind) * r;
    CAM_YAW(s) = wrap(CAM_YAW(s) + turn);
    CAM_TARGET_YAW(s) = wrap(CAM_TARGET_YAW(s) + turn);
    gBattleCameraYawOffset = wrap(gBattleCameraYawOffset + wrap(sActBehind - CAM_ORBIT_YAW(s)));
    CAM_ORBIT_YAW(s) = sActBehind;
    CAM_ORBIT_SPEED(s) = 0.0f;
}

static void act_swing(uint32_t cam) {
    uint32_t p = CAM_TARGET(cam);
    float ex = CAM_X(cam) - POS_X(p);
    float ez = CAM_Z(cam) - POS_Z(p);
    float r = sqrtf(ex * ex + ez * ez);
    float a = game_atan2(ex, ez);
    a = wrap(a + wrap(sActBehind - a) * SWING_EASE);
    CAM_X(cam) = POS_X(p) + game_sinf(a) * r;
    CAM_Z(cam) = POS_Z(p) + game_cosf(a) * r;
    CAM_ORBIT_YAW(cam) = a;
    CAM_ORBIT_SPEED(cam) = 0.0f;
}

static int sStyleSeen = -1;

// Mods that draw their own battle camera still have Brian walking relative to the game's camera, so they can hold it to
// the original code.
static int sOriginalCamera;

void camera_set_original(int on) {
    sOriginalCamera = on != 0;
}

// Switching to Classic mid-battle puts the camera straight at the original framing, since Modern's yaw offset is
// relative to its own aim.
static int style_modern(uint32_t cam) {
    int modern = !sOriginalCamera && controls_modern();
    if (modern != sStyleSeen) {
        if (!modern) {
            if (sStyleSeen >= 0 && (CAM_MODE(cam) == 0 || CAM_MODE(cam) == 2 || CAM_MODE(cam) == 4)) {
                CAM_ORBIT_PULL(cam) = FIELD_PULL;
                CAM_ORBIT_DRAG(cam) = FIELD_DRAG;
                CAM_ORBIT_SPEED(cam) = 0.0f;
                CAM_SPEED_X(cam) = 0.0f;
                CAM_SPEED_Y(cam) = 0.0f;
                CAM_SPEED_Z(cam) = 0.0f;
            }
            if (sStyleSeen >= 0 && CAM_MODE(cam) == 4 && !(CAM_FLAGS(cam) & 0x40)) {
                uint32_t subject = CAM_TARGET(cam);
                float angle = classic_battle_base(subject);
                float dx = CAM_X(cam) - POS_X(subject);
                float dz = CAM_Z(cam) - POS_Z(subject);
                float r = sqrtf(dx * dx + dz * dz);
                gBattleCameraYawOffset = 0.0f;
                CAM_ORBIT_YAW(cam) = angle;
                CAM_X(cam) = POS_X(subject) + game_sinf(angle) * r;
                CAM_Z(cam) = POS_Z(subject) + game_cosf(angle) * r;
            }
        } else {
            sFree = 0;
            sZoomOn = 0;
            sZoom = 0.0f;
            sSnapFrames = 0;
            sFastYFrames = 0;
            sAimSet = 0;
            sActCam = 0;
            sLockPrev = 0;
            sLockSnapFrames = 0;
            sLockEasing = 0;
            sWasBattle = CAM_MODE(cam) == 4;
        }
        sStyleSeen = modern;
    }
    return modern;
}

// Camera modes 0 and 2. The battle (4) and after battle (5) modes also end here.
static void modern_field(uint32_t cam) {
    int behind = 0;
    float sx;
    uint32_t target = CAM_TARGET(cam);
    // Mode 5 holds for over a second after a battle before handing back to the field mode.
    int field = CAM_MODE(cam) == 0 || CAM_MODE(cam) == 2 || CAM_MODE(cam) == 5;
    int battle = CAM_MODE(cam) == 4;
    int action = CAM_FLAGS(cam) & 0x40;
    int gated = (gGameState & 0x82) || (gBattleState & 0x200) || action;
    int is_brian = target == BRIAN_TRANSFORM;
    int snap_press = 0;
    int follow;

    camera_setup(cam);
    read_target();

    if (battle != sWasBattle) {
        sWasBattle = battle;
    }
    if (!battle) {
        sAimSet = 0;
        sActCam = 0;
        sLockPrev = 0;
        sLockSnapFrames = 0;
        sLockEasing = 0;
    }

    camera_stick(FREE_DEAD, &sx);
    if (!field || gated || !is_brian || gSubmapHeader != sFreeArea) {
        free_leave(cam);
    }
    if (field && !gated && is_brian && !(gGameState & 0x01) && !sFree) {
        float rx, ry;
        cont_camera_stick(&rx, &ry);
        if (rx * rx + ry * ry > FREE_ENTER * FREE_ENTER) {
            sFree = 1;
            sFreeArea = gSubmapHeader;
            sFreeYaw = game_atan2(CAM_X(cam) - POS_X(target), CAM_Z(cam) - POS_Z(target));
            sSnapFrames = 0;
        }
    }
    if (field && sTargetHeld && !sTargetPrev && !(gGameState & 0x01)) {
        free_leave(cam);
    }
    if (sFree) {
        sFreeYaw = wrap(sFreeYaw - FREE_YAW_RATE * CAMERA_TURN_SPEED * sx);
    }

    view_pre(cam, target, field);

    if (sFree) {
        CAM_FLAGS(cam) &= ~0x24;
    } else if (!gated) {
        // Not while the item menu is open: the triggers page its list.
        if (field && !(gGameState & 0x01) && sTargetHeld && !sTargetPrev) {
            sSnapFrames = SNAP_FRAMES;
            sFastYFrames = FAST_Y_FRAMES;
            CAM_ORBIT_SPEED(cam) = 0.0f;
            snap_press = 1;
        }
        if (!field) {
            sSnapFrames = 0;
        }
        if (sSnapFrames > 0) {
            follow = snap_press;
        } else if (field && sTargetHeld && !(gGameState & 0x01)) {
            follow = !stick_pulled_back();
        } else {
            follow = CAM_MODE(cam) == 2;
        }
        if (follow) {
            CAM_FLAGS(cam) |= 0x24;
        } else {
            CAM_FLAGS(cam) &= ~0x24;
        }
    } else {
        CAM_FLAGS(cam) &= ~0x24;
        sSnapFrames = 0;
    }

    if (sFree) {
        float want_x = POS_X(target) + game_sinf(sFreeYaw) * CAM_DISTANCE(cam);
        float want_z = POS_Z(target) + game_cosf(sFreeYaw) * CAM_DISTANCE(cam);
        CAM_X(cam) += (want_x - CAM_X(cam)) * FREE_FOLLOW;
        CAM_Z(cam) += (want_z - CAM_Z(cam)) * FREE_FOLLOW;
        CAM_SPEED_X(cam) = 0.0f;
        CAM_SPEED_Z(cam) = 0.0f;
        CAM_ORBIT_SPEED(cam) = 0.0f;
    } else {
        if (sSnapFrames > 0) {
            sSnapFrames--;
            CAM_FLAGS(cam) |= 0x04;
            CAM_ORBIT_PULL(cam) = SNAP_PULL;
            CAM_ORBIT_DRAG(cam) = 0.0f;
        } else if (battle && sLockEasing) {
            // The battle spring overshoots a big swing.
            CAM_ORBIT_PULL(cam) = SNAP_PULL;
            CAM_ORBIT_DRAG(cam) = 0.0f;
        } else if (battle) {
            CAM_ORBIT_PULL(cam) = BATTLE_PULL;
            CAM_ORBIT_DRAG(cam) = BATTLE_DRAG;
        } else {
            CAM_ORBIT_PULL(cam) = FIELD_PULL;
            CAM_ORBIT_DRAG(cam) = FIELD_DRAG;
        }
        behind = field_follow(cam, target);
    }
    field_height(cam, target, sFree || sFastYFrames > 0);
    if (sFastYFrames > 0) {
        sFastYFrames--;
    }
    view_post(cam);
    field_look(cam, target, behind);
}

// Camera mode 4.
static void modern_battle(uint32_t cam) {
    float angle, sx, dx, dz, d, lim;
    int held;
    uint32_t subject;

    if (CAM_FLAGS(cam) & 0x40) {
        sLockEasing = 0;
        if (sActCam == 0 || CAM_TIMER(cam) > sActTimer) {
            act_begin(cam);
        }
        if (sActCam == 1) {
            act_swing(cam);
        }
        CAM_TIMER(cam)--;
        sActTimer = CAM_TIMER(cam);
        if (CAM_TIMER(cam) == 0) {
            CAM_FLAGS(cam) &= ~0x40;
            copy_camera(cam, DEFAULT_CAMERA);
            if (sActCam == 1 || sActCam == 2) {
                sFastYFrames = FAST_Y_FRAMES;
            }
            sActCam = 0;
        }
    } else {
        sActCam = 0;
        subject = CAM_TARGET(cam);
        dx = gBattleCenterX - POS_X(subject);
        dz = gBattleCenterZ - POS_Z(subject);
        angle = wrap(game_atan2(dx, dz) + PI);
        if (!sAimSet) {
            sAimBase = angle;
            sAimSet = 1;
        } else {
            d = sqrtf(dx * dx + dz * dz);
            lim = AIM_K * (d < AIM_DCAP ? d : AIM_DCAP);
            sAimBase = wrap(sAimBase + clampf(wrap(angle - sAimBase), -lim, lim));
        }
        angle = sAimBase;
        held = cont_camera_target() > TRIGGER_ON;
        if (held) {
            // Brian walks relative to the camera, so the lock trails his facing by at most LOCK_LEAD.
            float behind = wrap(YROT(subject) + PI);
            float want = behind;
            if (!sLockPrev) {
                sLockSnapFrames = SNAP_FRAMES;
                CAM_ORBIT_SPEED(cam) = 0.0f;
            }
            sLockEasing = sLockSnapFrames > 0;
            if (sLockSnapFrames > 0) {
                sLockSnapFrames--;
            } else {
                float eye = game_atan2(CAM_X(cam) - POS_X(subject), CAM_Z(cam) - POS_Z(subject));
                want = wrap(eye + clampf(wrap(behind - eye), -LOCK_LEAD, LOCK_LEAD));
            }
            gBattleCameraYawOffset = wrap(want - angle);
        } else {
            sLockEasing = 0;
            camera_stick(FREE_DEAD, &sx);
            gBattleCameraYawOffset = wrap(gBattleCameraYawOffset - BATTLE_TURN * CAMERA_TURN_SPEED * sx);
        }
        sLockPrev = held;
        CAM_ORBIT_YAW(cam) = wrap(angle + gBattleCameraYawOffset);
    }
    cam_ctx->r4 = cam;
    func_80013008(cam_rdram, cam_ctx);
}

static void camera_enter(uint8_t* rdram, recomp_context* ctx) {
    cam_rdram = rdram;
    cam_rb = rdram - 0x80000000u;
    cam_ctx = ctx;
}

void camera_field(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cam = ctx->r4;
    gpr sp = ctx->r29;

    camera_enter(rdram, ctx);
    ctx->r29 = sp - FRAME_SIZE;
    if (style_modern(cam)) {
        modern_field(cam);
    } else {
        classic_field(cam);
    }
    ctx->r29 = sp;
}

void camera_battle(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cam = ctx->r4;
    gpr sp = ctx->r29;

    camera_enter(rdram, ctx);
    ctx->r29 = sp - FRAME_SIZE;
    if (style_modern(cam)) {
        modern_battle(cam);
    } else {
        classic_battle(cam);
    }
    ctx->r29 = sp;
}
