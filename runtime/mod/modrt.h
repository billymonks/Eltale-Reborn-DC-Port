#ifndef __MODRT_H__
#define __MODRT_H__

// Runtime support for statically recompiled mods (modded builds only). tools/modrecomp generates
// the tables declared here (mods_data.c) and one wrapper per import a mod uses, which calls the
// matching modrt_ function with that mod's ModInfo.

#include "runtime.h"

typedef struct {
    const char* key;
    uint32_t u32;    // Enum: the index of the choice; Number: the value truncated
    double dbl;
    const char* str;
} ModConfigValue;

typedef struct {
    const char* id;
    const char* prefix;
    uint32_t version[3];
    const ModConfigValue* config;
    uint32_t num_config;
} ModInfo;

typedef struct {
    uint32_t vram;
    recomp_func_t* func;
} ModFunc;

extern const uint32_t mods_image_vram;
extern const uint32_t mods_image_words;
extern const uint32_t mods_end_vram;
extern const uint32_t mods_image[];
extern const uint32_t mods_num_funcs;
extern const ModFunc mods_funcs[];
extern const uint32_t mods_count;
extern const ModInfo* const mods_list[];

// Calls a mod's hook with a copy of the context, which is restored afterwards, as N64ModernRuntime
// does. Return hooks read the hooked function's return value from that copy.
void mod_call_hook(uint8_t* rdram, recomp_context* ctx, recomp_func_t* hook);

#define MODRT_IMPORT(name) void modrt_##name(uint8_t* rdram, recomp_context* ctx, const ModInfo* mod)

MODRT_IMPORT(recomp_printf);
MODRT_IMPORT(recomp_alloc);
MODRT_IMPORT(recomp_free);
MODRT_IMPORT(recomp_get_config_u32);
MODRT_IMPORT(recomp_get_config_double);
MODRT_IMPORT(recomp_get_config_string);
MODRT_IMPORT(recomp_free_config_string);
MODRT_IMPORT(recomp_get_mod_version);
MODRT_IMPORT(recomp_is_dependency_met);
MODRT_IMPORT(recomphook_get_return_s32);
MODRT_IMPORT(recomphook_get_return_u32);
MODRT_IMPORT(recomphook_get_return_ptr);
MODRT_IMPORT(recomphook_get_return_s16);
MODRT_IMPORT(recomphook_get_return_u16);
MODRT_IMPORT(recomphook_get_return_s8);
MODRT_IMPORT(recomphook_get_return_u8);
MODRT_IMPORT(recomphook_get_return_s64);
MODRT_IMPORT(recomphook_get_return_u64);
MODRT_IMPORT(recomphook_get_return_float);
MODRT_IMPORT(recomphook_get_return_double);

// Native library functions that the runtime implements (tools/modrecomp's runtime_natives): Eltale
// Reborn's ambient sound, in ambience.c.
MODRT_IMPORT(native_ambience_frame);
MODRT_IMPORT(native_ambience_set);
MODRT_IMPORT(native_ambience_play);

#endif
