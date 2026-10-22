#include <algorithm>
#include <vector>

#include "runtime.h"
#include "recomp_overlays.inl"

extern "C" {
int32_t* section_addresses;
}

namespace {

struct LookupEntry {
    uint32_t vram;
    recomp_func_t* func;
};

// Per section: its functions sorted by vram. Sections that share a vram range (the title, intro and
// ending overlays at 0x80100000) are resolved through the one currently loaded there.
std::vector<std::vector<LookupEntry>> section_lookup;
std::vector<bool> section_loaded;
std::vector<int32_t> section_address_storage;

bool section_contains(const SectionTableEntry& section, uint32_t vram) {
    return vram >= section.ram_addr && vram < section.ram_addr + section.size;
}

bool sections_overlap(const SectionTableEntry& a, const SectionTableEntry& b) {
    return a.ram_addr < b.ram_addr + b.size && b.ram_addr < a.ram_addr + a.size;
}

}

void function_lookup_init(void) {
    section_lookup.resize(num_sections);
    section_loaded.assign(num_sections, false);
    section_address_storage.resize(num_sections);
    section_addresses = section_address_storage.data();

    for (size_t i = 0; i < num_sections; i++) {
        const SectionTableEntry& section = section_table[i];
        section_address_storage[i] = (int32_t)section.ram_addr;

        auto& entries = section_lookup[i];
        entries.reserve(section.num_funcs);
        for (size_t f = 0; f < section.num_funcs; f++) {
            entries.push_back({ section.ram_addr + section.funcs[f].offset, section.funcs[f].func });
        }
        std::sort(entries.begin(), entries.end(), [](const LookupEntry& a, const LookupEntry& b) { return a.vram < b.vram; });
    }

    // Sections that nothing else overlaps are always resident.
    for (size_t i = 0; i < num_sections; i++) {
        bool shared = false;
        for (size_t j = 0; j < num_sections; j++) {
            if (i != j && sections_overlap(section_table[i], section_table[j])) {
                shared = true;
            }
        }
        section_loaded[i] = !shared;
    }
}

extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size) {
    (void)ram_addr;
    for (size_t i = 0; i < num_sections; i++) {
        const SectionTableEntry& section = section_table[i];
        if (section.rom_addr < rom || section.rom_addr >= rom + size || section_loaded[i]) {
            continue;
        }
        for (size_t j = 0; j < num_sections; j++) {
            if (j != i && sections_overlap(section, section_table[j])) {
                section_loaded[j] = false;
            }
        }
        section_loaded[i] = true;
    }
}

extern "C" recomp_func_t* get_function(int32_t vram) {
    uint32_t addr = (uint32_t)vram;
    for (size_t i = 0; i < num_sections; i++) {
        if (!section_loaded[i] || !section_contains(section_table[i], addr)) {
            continue;
        }
        const auto& entries = section_lookup[i];
        auto it = std::lower_bound(entries.begin(), entries.end(), addr,
            [](const LookupEntry& e, uint32_t v) { return e.vram < v; });
        if (it != entries.end() && it->vram == addr) {
            return it->func;
        }
    }
    runtime_fatal("[q64dc] no function at 0x%08X", addr);
}
