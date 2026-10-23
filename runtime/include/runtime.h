#ifndef __RUNTIME_H__
#define __RUNTIME_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "recomp.h"

// Quest 64 runs in the N64's base 4 MB of RDRAM; it doesn't use the Expansion Pak.
#define RDRAM_SIZE (4 * 1024 * 1024)

// Modded builds (make MOD=...) put their mods' code, data and heap right after the runtime's
// 64 KB (ultra.h). Vanilla builds have none.
#ifndef MOD_RAM_SIZE
#define MOD_RAM_SIZE 0
#endif
#define MOD_RAM_BASE 0x80410000u

// The part of rdram display lists and textures may come from: mods draw from their own memory.
#if MOD_RAM_SIZE
#define RDRAM_MAPPED_SIZE (MOD_RAM_BASE - 0x80000000u + MOD_RAM_SIZE)
#else
#define RDRAM_MAPPED_SIZE RDRAM_SIZE
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t* rdram;

// Defined in the generated lookup.cpp, which is compiled as C.
gpr get_entrypoint_address(void);

bool rom_open(void);
uint32_t rom_size(void);
// Copies ROM bytes into rdram, converting from the ROM's big-endian layout to rdram's word order.
void rom_read_to_rdram(uint32_t rom_addr, uint32_t ram_addr, uint32_t size);

void function_lookup_init(void);

// Modded builds only (runtime/mod/modrt.c): loads the mods into rdram, finds their functions.
void mods_init(void);
recomp_func_t* mods_get_function(uint32_t vram);

__attribute__((noreturn)) void runtime_fatal(const char* fmt, ...);
__attribute__((noreturn)) void runtime_unimplemented(const char* name);

#ifdef __cplusplus
}
#endif

#endif
