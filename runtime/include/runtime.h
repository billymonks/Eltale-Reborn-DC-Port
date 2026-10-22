#ifndef __RUNTIME_H__
#define __RUNTIME_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "recomp.h"

// Quest 64 runs in the N64's base 4 MB of RDRAM; it doesn't use the Expansion Pak.
#define RDRAM_SIZE (4 * 1024 * 1024)

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

__attribute__((noreturn)) void runtime_fatal(const char* fmt, ...);
__attribute__((noreturn)) void runtime_unimplemented(const char* name);

#ifdef __cplusplus
}
#endif

#endif
