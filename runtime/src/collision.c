// Native replacement for the game's point-in-shape floor test, func_8000F238 (and the edge test
// it calls three times per triangle, func_8000F5A4). The floor probe runs it for every nearby
// shape of every actor each frame, which made the two the game's costliest functions in battle.
// The steps, comparisons and float operations are the original's, in its order.
//
// func_8000F238(f12 x, f14 z, a2 scale, a3 mask, sp+0x10 shape, sp+0x14 result) returns 1 when
// (x, z) lies in a triangle of the shape (scaled by scale) whose flags share a bit with mask, and
// fills in the result's height (+0x04), normal (+0x0C) and flags (+0x18).
// A shape: s16 bounding box min x, max x (+0x00, +0x02), min z, max z (+0x08, +0x0A), triangles
// (+0x14), triangle count (u16, +0x18), vertices (+0x1C). A triangle (0x14 bytes): s16 vertex
// indices, u16 flags, float normal x, y, z. A vertex (8 bytes): s16 x, y, z.
//
// Built with -DFLOOR_VERIFY (and the config's hook changed to floor_verify_begin, plus a hook
// calling floor_verify_end before 0x8000F564), the original runs too and every result is compared.

#include <stdint.h>
#include <stdio.h>

#include "recomp.h"

// The original rounds each product and each sum, as the generated code does (-ffp-contract=off).
// The runtime's flags let GCC fuse them into FMAC, which rounds once.
#pragma GCC optimize("fp-contract=off")

typedef struct {
    int hit, has_height;
    float height, nx, ny, nz;
    uint16_t flags;
} FloorHit;

static inline float as_float(uint32_t bits) {
    union { uint32_t u; float f; } v = { bits };
    return v.f;
}

static inline uint32_t as_bits(float f) {
    union { float f; uint32_t u; } v = { f };
    return v.u;
}

// func_8000F5A4: 1 if the edge (ax, az)-(bx, bz) crosses the line z = pz left of px.
static int edge_crosses(float px, float pz, float ax, float az, float bx, float bz) {
    if (!((az <= pz && pz < bz) || (bz <= pz && pz < az))) {
        return 0;
    }
    if (ax <= px && bx < px) {
        return 1;
    }
    if (px <= ax && px < bx) {
        return 0;
    }
    if (az == bz) {
        return 0;
    }
    float slope = (ax - bx) / (az - bz);
    float cross_x = (pz - az) * slope + ax;
    return cross_x < px;
}

static void floor_test(uint8_t* rdram, recomp_context* ctx, FloorHit* out) {
    float px = ctx->f12.fl;
    float pz = ctx->f14.fl;
    float scale = as_float(ctx->r6);
    uint32_t mask = ctx->r7 & 0xFFFF;
    uint32_t shape = (uint32_t)MEM_W(0x10, ctx->r29);

    out->hit = 0;
    uint32_t tri = (uint32_t)MEM_W(0x14, shape);
    uint32_t verts = (uint32_t)MEM_W(0x1C, shape);
    if (tri == 0) {
        return;
    }
    float min_x = (float)MEM_H(0x00, shape) * scale;
    float max_x = (float)MEM_H(0x02, shape) * scale;
    float min_z = (float)MEM_H(0x08, shape) * scale;
    float max_z = (float)MEM_H(0x0A, shape) * scale;
    if (px < min_x || max_x < px || pz < min_z || max_z < pz) {
        return;
    }

    for (uint32_t count = MEM_HU(0x18, shape); count != 0; count--, tri += 0x14) {
        uint32_t flags = MEM_HU(0x06, tri);
        if ((flags & mask) == 0) {
            continue;
        }
        uint32_t v0 = verts + (uint32_t)((int32_t)MEM_H(0x00, tri) << 3);
        uint32_t v1 = verts + (uint32_t)((int32_t)MEM_H(0x02, tri) << 3);
        uint32_t v2 = verts + (uint32_t)((int32_t)MEM_H(0x04, tri) << 3);
        float x0 = (float)MEM_H(0, v0) * scale, z0 = (float)MEM_H(4, v0) * scale;
        float x1 = (float)MEM_H(0, v1) * scale, z1 = (float)MEM_H(4, v1) * scale;
        float x2 = (float)MEM_H(0, v2) * scale, z2 = (float)MEM_H(4, v2) * scale;

        int crossings;
        if ((px == x0 && pz == z0) || (px == x1 && pz == z1) || (px == x2 && pz == z2)) {
            crossings = 1;
        } else {
            crossings = edge_crosses(px, pz, x0, z0, x1, z1) + edge_crosses(px, pz, x1, z1, x2, z2) +
                        edge_crosses(px, pz, x0, z0, x2, z2);
        }
        if (crossings != 1) {
            continue;
        }

        out->hit = 1;
        out->flags = (uint16_t)flags;
        out->nx = as_float((uint32_t)MEM_W(0x08, tri));
        out->ny = as_float((uint32_t)MEM_W(0x0C, tri));
        out->nz = as_float((uint32_t)MEM_W(0x10, tri));
        out->has_height = out->ny != 0.0f;
        if (out->has_height) {
            float y0 = (float)MEM_H(2, v0) * scale;
            float d = (out->nx * px + out->nz * pz) - ((out->nx * x0 + out->ny * y0) + out->nz * z0);
            out->height = -d / out->ny;
        }
        return;
    }
}

void floor_shape_test(uint8_t* rdram, recomp_context* ctx) {
    FloorHit hit;
    floor_test(rdram, ctx, &hit);
    ctx->r2 = hit.hit;
    if (hit.hit) {
        uint32_t result = (uint32_t)MEM_W(0x14, ctx->r29);
        if (hit.has_height) {
            MEM_W(0x04, result) = (int32_t)as_bits(hit.height);
        }
        MEM_H(0x18, result) = (int16_t)hit.flags;
        MEM_W(0x0C, result) = (int32_t)as_bits(hit.nx);
        MEM_W(0x10, result) = (int32_t)as_bits(hit.ny);
        MEM_W(0x14, result) = (int32_t)as_bits(hit.nz);
    }
}

#ifdef FLOOR_VERIFY
static FloorHit verify_hit;
static uint32_t verify_result, verify_calls, verify_hits, verify_bad;

void floor_verify_begin(uint8_t* rdram, recomp_context* ctx) {
    floor_test(rdram, ctx, &verify_hit);
    verify_result = (uint32_t)MEM_W(0x14, ctx->r29);
}

void floor_verify_end(uint8_t* rdram, recomp_context* ctx) {
    const FloorHit* h = &verify_hit;
    uint32_t r = verify_result;
    int bad = (int)ctx->r2 != h->hit;
    if (!bad && h->hit) {
        bad = MEM_HU(0x18, r) != h->flags || (uint32_t)MEM_W(0x0C, r) != as_bits(h->nx) ||
              (uint32_t)MEM_W(0x10, r) != as_bits(h->ny) || (uint32_t)MEM_W(0x14, r) != as_bits(h->nz) ||
              (h->has_height && (uint32_t)MEM_W(0x04, r) != as_bits(h->height));
    }
    verify_calls++;
    verify_hits += h->hit;
    if (bad) {
        verify_bad++;
        if (verify_bad <= 20) {
            printf("[q64dc] floor: mismatch: game %d height %08lx, native %d height %08lx\n", (int)ctx->r2,
                   (unsigned long)(uint32_t)MEM_W(0x04, r), h->hit, (unsigned long)as_bits(h->height));
        }
    }
    if ((verify_calls & 0x3FFF) == 0) {
        printf("[q64dc] floor: %lu calls, %lu hits, %lu mismatches\n", (unsigned long)verify_calls,
               (unsigned long)verify_hits, (unsigned long)verify_bad);
    }
}
#endif
