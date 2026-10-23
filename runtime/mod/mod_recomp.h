#ifndef __MOD_RECOMP_H__
#define __MOD_RECOMP_H__

// Included by the recompiled mod code. Each generated file defines mod_section_addresses (where its
// mod's sections are loaded) and mod_ref_section_addresses (the game's sections) as constants, so
// the compiler folds every relocated address.

#include "recomp.h"

#undef RELOC_HI16
#undef RELOC_LO16

#define RELOC_HI16(section_index, offset) \
    HI16(mod_section_addresses[section_index] + (offset))

#define RELOC_LO16(section_index, offset) \
    LO16(mod_section_addresses[section_index] + (offset))

#define REF_RELOC_HI16(section_index, offset) \
    HI16(mod_ref_section_addresses[section_index] + (offset))

#define REF_RELOC_LO16(section_index, offset) \
    LO16(mod_ref_section_addresses[section_index] + (offset))

#endif
