// FPU state of the threads the runtime starts.
//
// libultra starts every N64 thread with FCSR.FS set, so the FPU flushes denormal results to zero
// and the game never computes with a denormal. The SH-4 does that with FPSCR.DN. KallistiOS sets
// DN for its main thread only; every thread it creates starts with FPSCR 0. With DN clear, an
// operation that underflows leaves a denormal, and the next FADD, FSUB, FMUL, FDIV, FMAC, FSQRT,
// FCNVSD or FCNVDS that reads one raises the FPU error exception, which can't be masked.
// KallistiOS's handler skips the instruction and leaves its destination as it was. While Brian
// stands still, the follow camera's orbit speed (camera struct +0x3C) decays to the denormal
// 2.8e-45, which its damping can't round any lower, and from then on the sine and cosine that
// place the camera come out of skipped instructions every frame. Emulators don't raise the
// exception.

#include <kos.h>

#include "ultra.h"

#define FPSCR_DN (1u << 18)

void fpu_thread_init(void) {
    // Keeps FR, SZ and PR. The generated code switches precision by toggling PR, so DN stays set.
    __builtin_sh_set_fpscr(__builtin_sh_get_fpscr() | FPSCR_DN);
}
