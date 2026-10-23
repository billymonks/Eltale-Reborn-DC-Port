// Statically recompiles N64Recomp mods (the contents of their .nrm files) for the Dreamcast build.
//
// usage: modrecomp <game symbol toml> <load address> <ram size> <output dir> <mod dir>... [--set <mod>.<option>=<value>]...
//
// Each mod dir holds mod_syms.bin, mod_binary.bin and mod.toml; its name is the mod's C prefix.
// The mods are placed one after another from the load address the way N64ModernRuntime places
// them (each section 16-byte aligned with its bss after it, 0x400 bytes between mods), and their
// R_MIPS_32 data relocations are applied at build time. Outputs, in the output dir:
//   <prefix>_<n>.c    the recompiled functions, which address their own sections through constants
//   mods_data.c       the RDRAM image, the function table for lookups, config values, import wrappers
//   mods_hooks.h      prototypes for the game code that calls into the mods
//   mods_hooks.json   the patches and hooks to wire into the game's recompiler config
//   mods_layout.txt   each section's link and load address, to find a mod's symbols from its map

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fmt/format.h"
#include "rabbitizer.hpp"
#include "recompiler/context.h"
#include "toml++/toml.hpp"

namespace fs = std::filesystem;

namespace {

// Imports from the base recomp ("*") that runtime/mod/modrt.c implements.
const std::set<std::string> runtime_imports {
    "recomp_printf",
    "recomp_alloc",
    "recomp_free",
    "recomp_get_config_u32",
    "recomp_get_config_double",
    "recomp_get_config_string",
    "recomp_free_config_string",
    "recomp_get_mod_version",
    "recomp_is_dependency_met",
    "recomphook_get_return_s32",
    "recomphook_get_return_u32",
    "recomphook_get_return_ptr",
    "recomphook_get_return_s16",
    "recomphook_get_return_u16",
    "recomphook_get_return_s8",
    "recomphook_get_return_u8",
    "recomphook_get_return_s64",
    "recomphook_get_return_u64",
    "recomphook_get_return_float",
    "recomphook_get_return_double",
};

constexpr size_t funcs_per_file = 150;

[[noreturn]] void fail(const std::string& msg) {
    fmt::print(stderr, "modrecomp: error: {}\n", msg);
    std::exit(EXIT_FAILURE);
}

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream file{ path, std::ios::binary };
    if (!file.good()) {
        fail(fmt::format("couldn't open {}", path.string()));
    }
    return std::vector<uint8_t>{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

void write_if_changed(const fs::path& path, const std::string& text) {
    std::ifstream in{ path, std::ios::binary };
    if (in.good()) {
        std::string old{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        if (old == text) {
            return;
        }
    }
    in.close();
    std::ofstream out{ path, std::ios::binary };
    out << text;
}

std::string c_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((unsigned char)c < 0x20) {
            out += fmt::format("\\x{:02X}\"\"", (unsigned char)c);
        } else {
            out += c;
        }
    }
    return out + "\"";
}

uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

struct ConfigValue {
    std::string key;
    uint32_t u32 = 0;
    double dbl = 0.0;
    std::string str;
    std::string type;
    std::vector<std::string> options;
};

struct Hook {
    std::string game_func;
    std::string kind; // "patch", "hook" or "return_hook"
    std::string call;
};

struct Mod {
    std::string prefix;
    fs::path dir;
    std::string id;
    uint32_t version[3] = { 0, 0, 0 };
    std::vector<ConfigValue> config;
    std::vector<uint8_t> binary;
    N64Recomp::Context context;
    std::vector<uint32_t> section_load;
    uint32_t start = 0;
    uint32_t end = 0;
    std::unordered_map<std::string, std::string> exports; // export name -> C name
    std::vector<Hook> hooks;
    std::set<std::string> imports_used;
};

void read_manifest(Mod& mod, const std::map<std::string, std::string>& overrides) {
    toml::table manifest_file;
    try {
        manifest_file = toml::parse_file((mod.dir / "mod.toml").string());
    } catch (const toml::parse_error& e) {
        fail(fmt::format("couldn't parse {}: {}", (mod.dir / "mod.toml").string(), e.description()));
    }
    const auto manifest = manifest_file["manifest"];
    mod.id = manifest["id"].value_or(std::string{});
    std::string version = manifest["version"].value_or(std::string{ "0.0.0" });
    std::sscanf(version.c_str(), "%u.%u.%u", &mod.version[0], &mod.version[1], &mod.version[2]);

    if (const toml::array* libs = manifest["native_libraries"].as_array(); libs != nullptr && !libs->empty()) {
        fail(fmt::format("{} uses native libraries, which the Dreamcast build can't load", mod.id));
    }

    std::set<std::string> used_overrides;
    if (const toml::array* options = manifest["config_options"].as_array(); options != nullptr) {
        for (const toml::node& node : *options) {
            const toml::table& opt = *node.as_table();
            ConfigValue value;
            value.key = opt["id"].value_or(std::string{});
            value.type = opt["type"].value_or(std::string{});
            std::string override_value;
            auto ov = overrides.find(mod.prefix + "." + value.key);
            bool has_override = ov != overrides.end();
            if (has_override) {
                used_overrides.insert(ov->first);
            }
            if (value.type == "Enum") {
                if (const toml::array* names = opt["options"].as_array(); names != nullptr) {
                    for (const toml::node& n : *names) {
                        value.options.push_back(n.value_or(std::string{}));
                    }
                }
                auto index_of = [&](const std::string& name) -> uint32_t {
                    for (size_t i = 0; i < value.options.size(); i++) {
                        if (value.options[i] == name) {
                            return (uint32_t)i;
                        }
                    }
                    if (!name.empty() && std::all_of(name.begin(), name.end(), ::isdigit)) {
                        return (uint32_t)std::stoul(name);
                    }
                    fail(fmt::format("{}: config option {} has no choice \"{}\"", mod.id, value.key, name));
                };
                std::string def = opt["default"].value_or(std::string{});
                value.u32 = index_of(has_override ? ov->second : def);
                value.dbl = value.u32;
            } else if (value.type == "Number") {
                value.dbl = has_override ? std::stod(ov->second) : opt["default"].value_or(0.0);
                value.u32 = (uint32_t)(int64_t)value.dbl;
            } else if (value.type == "String") {
                value.str = has_override ? ov->second : opt["default"].value_or(std::string{});
            } else {
                fail(fmt::format("{}: config option {} has unsupported type {}", mod.id, value.key, value.type));
            }
            mod.config.push_back(std::move(value));
        }
    }
    for (const auto& [key, val] : overrides) {
        if (key.starts_with(mod.prefix + ".") && !used_overrides.contains(key)) {
            fail(fmt::format("--set {}: {} has no such config option", key, mod.id));
        }
    }
}

} // namespace

int main(int argc, const char** argv) {
    std::vector<std::string> positional;
    std::map<std::string, std::string> overrides;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--set" && i + 1 < argc) {
            std::string kv = argv[++i];
            size_t eq = kv.find('=');
            if (eq == std::string::npos || kv.find('.') > eq) {
                fail(fmt::format("--set expects <mod>.<option>=<value>, got {}", kv));
            }
            overrides[kv.substr(0, eq)] = kv.substr(eq + 1);
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() < 5) {
        fmt::print(stderr, "usage: {} <game symbol toml> <load address> <ram size> <output dir> <mod dir>... [--set <mod>.<option>=<value>]...\n", argv[0]);
        return EXIT_FAILURE;
    }
    const fs::path symbols_path = positional[0];
    const uint32_t load_address = (uint32_t)std::stoul(positional[1], nullptr, 0);
    const uint32_t ram_size = (uint32_t)std::stoul(positional[2], nullptr, 0);
    const fs::path out_dir = positional[3];
    fs::create_directories(out_dir);

    // The game's functions, named the way the recompiler CLI names them.
    std::vector<uint8_t> no_rom{};
    N64Recomp::Context game{};
    if (!N64Recomp::Context::from_symbol_file(symbols_path, std::move(no_rom), game, false)) {
        fail(fmt::format("couldn't load the game's symbols from {}", symbols_path.string()));
    }
    for (auto& func : game.functions) {
        if (N64Recomp::reimplemented_funcs.contains(func.name) || N64Recomp::ignored_funcs.contains(func.name) ||
            N64Recomp::renamed_funcs.contains(func.name)) {
            func.name += "_recomp";
        }
    }
    std::unordered_map<uint32_t, uint16_t> sections_by_vrom{};
    for (uint16_t i = 0; i < game.sections.size(); i++) {
        sections_by_vrom[game.sections[i].rom_addr] = i;
    }
    auto game_func_name = [&](uint32_t section_vrom, uint32_t vram) -> std::string {
        auto sec = sections_by_vrom.find(section_vrom);
        if (sec == sections_by_vrom.end()) {
            fail(fmt::format("no game section at ROM 0x{:08X}", section_vrom));
        }
        for (const auto& func : game.functions) {
            if (func.section_index == sec->second && func.vram == vram) {
                return func.name;
            }
        }
        fail(fmt::format("no game function at 0x{:08X} in section {}", vram, game.sections[sec->second].name));
    };

    RabbitizerConfig_Cfg.pseudos.pseudoMove = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBeqz = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBnez = false;
    RabbitizerConfig_Cfg.pseudos.pseudoNot = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBal = false;

    // Load every mod and give it its place in RDRAM.
    std::vector<Mod> mods;
    mods.reserve(positional.size() - 4);
    uint32_t cur = align_up(load_address, 16);
    for (size_t m = 4; m < positional.size(); m++) {
        Mod& mod = mods.emplace_back();
        mod.dir = positional[m];
        mod.prefix = mod.dir.filename().string();
        read_manifest(mod, overrides);

        std::vector<uint8_t> syms = read_file(mod.dir / "mod_syms.bin");
        mod.binary = read_file(mod.dir / "mod_binary.bin");
        std::span<const char> syms_span{ reinterpret_cast<const char*>(syms.data()), syms.size() };
        N64Recomp::ModSymbolsError err = N64Recomp::parse_mod_symbols(syms_span, mod.binary, sections_by_vrom, mod.context);
        if (err != N64Recomp::ModSymbolsError::Good) {
            fail(fmt::format("{}: couldn't parse mod_syms.bin (error {})", mod.id, (int)err));
        }
        N64Recomp::Context& ctx = mod.context;
        if (!ctx.import_reference_context(game)) {
            fail(fmt::format("{}: couldn't import the game's symbols", mod.id));
        }
        ctx.gpr_32bit = true;
        ctx.rom = mod.binary;

        if (!ctx.callbacks.empty() || !ctx.event_symbols.empty() || !ctx.dependency_events.empty()) {
            fail(fmt::format("{} uses mod events/callbacks (RECOMP_CALLBACK/RECOMP_DECLARE_EVENT), which the Dreamcast build doesn't support yet", mod.id));
        }

        // Resolve the game function each R_MIPS_26 reference points at.
        std::unordered_map<uint32_t, std::vector<size_t>> ref_by_vram;
        for (size_t i = 0; i < ctx.num_regular_reference_symbols(); i++) {
            const auto& sym = ctx.get_regular_reference_symbol(i);
            if (sym.section_index != N64Recomp::SectionAbsolute) {
                ref_by_vram[ctx.get_reference_section_vram(sym.section_index) + sym.section_offset].push_back(i);
            }
        }
        for (auto& section : ctx.sections) {
            for (auto& reloc : section.relocs) {
                if (reloc.type != N64Recomp::RelocType::R_MIPS_26 || !reloc.reference_symbol || !ctx.is_regular_reference_section(reloc.target_section)) {
                    continue;
                }
                uint32_t target = ctx.get_reference_section_vram(reloc.target_section) + reloc.target_section_offset;
                bool found = false;
                if (auto it = ref_by_vram.find(target); it != ref_by_vram.end()) {
                    for (size_t idx : it->second) {
                        if (ctx.get_reference_symbol(reloc.target_section, idx).section_index == reloc.target_section) {
                            reloc.symbol_index = idx;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    fail(fmt::format("{}: call to 0x{:08X}, which isn't a known game function", mod.id, target));
                }
            }
        }

        // Section placement, as N64ModernRuntime's init_mod_code does it.
        mod.start = cur;
        mod.section_load.resize(ctx.sections.size());
        for (size_t s = 0; s < ctx.sections.size(); s++) {
            const auto& section = ctx.sections[s];
            if (section.fixed_address) {
                fail(fmt::format("{}: fixed-address sections aren't supported", mod.id));
            }
            if ((section.ram_addr & 15) != 0) {
                fail(fmt::format("{}: section {} isn't 16-byte aligned", mod.id, s));
            }
            mod.section_load[s] = cur;
            cur += section.size + section.bss_size;
            cur = align_up(cur, 16);
        }
        mod.end = cur;
        cur += 0x400;

        // Names: exports keep theirs, the rest are named after their original address.
        std::unordered_set<size_t> export_set{ ctx.exported_funcs.begin(), ctx.exported_funcs.end() };
        for (size_t f = 0; f < ctx.functions.size(); f++) {
            auto& func = ctx.functions[f];
            if (export_set.contains(f)) {
                std::string name = func.name;
                func.name = fmt::format("{}_export_{}", mod.prefix, name);
                mod.exports[name] = func.name;
            } else {
                func.name = fmt::format("{}_func_{:08X}", mod.prefix, func.vram);
            }
        }
    }
    const uint32_t mods_end = cur;
    if (mods_end - load_address > ram_size) {
        fail(fmt::format("the mods need 0x{:X} bytes but MOD_RAM_SIZE is 0x{:X}", mods_end - load_address, ram_size));
    }

    // Imports: the runtime's, or another mod's exports.
    std::unordered_map<std::string, const Mod*> mods_by_id;
    for (const Mod& mod : mods) {
        mods_by_id[mod.id] = &mod;
    }
    for (Mod& mod : mods) {
        N64Recomp::Context& ctx = mod.context;
        for (auto& imp : ctx.import_symbols) {
            const std::string& dep = ctx.dependencies[imp.dependency_index];
            const std::string name = imp.base.name;
            if (dep == N64Recomp::DependencyBaseRecomp) {
                if (!runtime_imports.contains(name)) {
                    fail(fmt::format("{} imports {} from the recomp, which the Dreamcast runtime doesn't implement (see runtime/mod/modrt.c)", mod.id, name));
                }
                mod.imports_used.insert(name);
                imp.base.name = fmt::format("{}_import_{}", mod.prefix, name);
            } else {
                auto other = mods_by_id.find(dep);
                if (other == mods_by_id.end()) {
                    fail(fmt::format("{} imports {} from {}, which isn't part of this build", mod.id, name, dep));
                }
                auto exp = other->second->exports.find(name);
                if (exp == other->second->exports.end()) {
                    fail(fmt::format("{} imports {} from {}, which doesn't export it", mod.id, name, dep));
                }
                imp.base.name = exp->second;
            }
        }
    }

    // Patches and hooks on game functions.
    for (Mod& mod : mods) {
        const N64Recomp::Context& ctx = mod.context;
        for (const auto& rep : ctx.replacements) {
            mod.hooks.push_back({ game_func_name(rep.original_section_vrom, rep.original_vram), "patch", ctx.functions[rep.func_index].name });
        }
        for (const auto& hook : ctx.hooks) {
            bool at_return = (hook.flags & N64Recomp::HookFlags::AtReturn) == N64Recomp::HookFlags::AtReturn;
            mod.hooks.push_back({ game_func_name(hook.original_section_vrom, hook.original_vram), at_return ? "return_hook" : "hook",
                ctx.functions[hook.func_index].name });
        }
    }

    // Recompile the code.
    std::string all_protos;
    for (Mod& mod : mods) {
        for (const auto& func : mod.context.functions) {
            all_protos += fmt::format("RECOMP_FUNC void {}(uint8_t* rdram, recomp_context* ctx);\n", func.name);
        }
        for (const auto& name : mod.imports_used) {
            all_protos += fmt::format("RECOMP_FUNC void {}_import_{}(uint8_t* rdram, recomp_context* ctx);\n", mod.prefix, name);
        }
    }

    std::vector<std::string> outputs;
    for (Mod& mod : mods) {
        N64Recomp::Context& ctx = mod.context;

        std::set<std::string> game_calls;
        for (const auto& section : ctx.sections) {
            for (const auto& reloc : section.relocs) {
                if (reloc.type == N64Recomp::RelocType::R_MIPS_26 && reloc.reference_symbol && ctx.is_regular_reference_section(reloc.target_section)) {
                    game_calls.insert(ctx.get_reference_symbol(reloc.target_section, reloc.symbol_index).name);
                }
                if (reloc.type == N64Recomp::RelocType::R_MIPS_32 && reloc.reference_symbol && !ctx.is_regular_reference_section(reloc.target_section)) {
                    fail(fmt::format("{}: data at 0x{:08X} points at an import, which isn't supported", mod.id, reloc.address));
                }
            }
        }

        std::string prelude = "// Generated by tools/modrecomp from " + mod.id + ". Don't edit.\n#include \"mod_recomp.h\"\n\n";
        prelude += "static const int32_t mod_section_addresses[] = {";
        for (uint32_t addr : mod.section_load) {
            prelude += fmt::format(" (int32_t)0x{:08X},", addr);
        }
        prelude += " 0 };\nstatic const int32_t mod_ref_section_addresses[] = {";
        for (const auto& section : game.sections) {
            prelude += fmt::format(" (int32_t)0x{:08X},", section.ram_addr);
        }
        prelude += " 0 };\n\n" + all_protos;
        for (const auto& name : game_calls) {
            prelude += fmt::format("RECOMP_FUNC void {}(uint8_t* rdram, recomp_context* ctx);\n", name);
        }
        prelude += "\n";

        std::vector<std::vector<uint32_t>> static_funcs(ctx.sections.size());
        size_t file_index = 0;
        for (size_t start = 0; start < ctx.functions.size(); start += funcs_per_file, file_index++) {
            std::ostringstream body;
            body << prelude;
            for (size_t f = start; f < std::min(ctx.functions.size(), start + funcs_per_file); f++) {
                if (!N64Recomp::recompile_function(ctx, f, body, static_funcs, true)) {
                    fail(fmt::format("{}: couldn't recompile {}", mod.id, ctx.functions[f].name));
                }
            }
            std::string name = fmt::format("{}_{}.c", mod.prefix, file_index);
            write_if_changed(out_dir / name, body.str());
            outputs.push_back(name);
        }
        for (const auto& list : static_funcs) {
            if (!list.empty()) {
                fail(fmt::format("{}: jumps into the middle of a function (0x{:08X}) aren't supported", mod.id, list[0]));
            }
        }
    }

    // The RDRAM image, with the mods' own R_MIPS_32 pointers moved to where they're loaded.
    const uint32_t image_start = mods.front().start;
    uint32_t image_end = image_start;
    for (const Mod& mod : mods) {
        for (size_t s = 0; s < mod.context.sections.size(); s++) {
            image_end = std::max(image_end, mod.section_load[s] + mod.context.sections[s].size);
        }
    }
    image_end = align_up(image_end, 4);
    std::vector<uint8_t> image(image_end - image_start, 0);
    for (const Mod& mod : mods) {
        const auto& ctx = mod.context;
        for (size_t s = 0; s < ctx.sections.size(); s++) {
            const auto& section = ctx.sections[s];
            if (section.rom_addr + section.size > mod.binary.size()) {
                fail(fmt::format("{}: section {} is past the end of mod_binary.bin", mod.id, s));
            }
            std::copy_n(mod.binary.begin() + section.rom_addr, section.size, image.begin() + (mod.section_load[s] - image_start));
        }
        for (size_t s = 0; s < ctx.sections.size(); s++) {
            const auto& section = ctx.sections[s];
            for (const auto& reloc : section.relocs) {
                if (reloc.type != N64Recomp::RelocType::R_MIPS_32 || reloc.reference_symbol) {
                    continue;
                }
                if (reloc.target_section >= ctx.sections.size()) {
                    fail(fmt::format("{}: R_MIPS_32 at 0x{:08X} targets section {}", mod.id, reloc.address, reloc.target_section));
                }
                uint32_t at = reloc.address - section.ram_addr + mod.section_load[s] - image_start;
                uint32_t word = (uint32_t)image[at] << 24 | (uint32_t)image[at + 1] << 16 | (uint32_t)image[at + 2] << 8 | image[at + 3];
                word += mod.section_load[reloc.target_section] - ctx.sections[reloc.target_section].ram_addr;
                image[at] = word >> 24;
                image[at + 1] = word >> 16;
                image[at + 2] = word >> 8;
                image[at + 3] = word;
            }
        }
    }

    // Lookup table for calls through function pointers, sorted by loaded address.
    std::vector<std::pair<uint32_t, std::string>> func_table;
    for (const Mod& mod : mods) {
        const auto& ctx = mod.context;
        for (const auto& func : ctx.functions) {
            uint32_t loaded = func.vram - ctx.sections[func.section_index].ram_addr + mod.section_load[func.section_index];
            func_table.emplace_back(loaded, func.name);
        }
    }
    std::sort(func_table.begin(), func_table.end());

    std::string data = "// Generated by tools/modrecomp. Don't edit.\n#include \"modrt.h\"\n\n" + all_protos + "\n";
    data += fmt::format("const uint32_t mods_image_vram = 0x{:08X};\nconst uint32_t mods_image_words = {};\nconst uint32_t mods_end_vram = 0x{:08X};\n",
        image_start, image.size() / 4, mods_end);
    data += "const uint32_t mods_image[] __attribute__((aligned(32))) = {";
    for (size_t i = 0; i < image.size(); i += 4) {
        if ((i / 4) % 8 == 0) {
            data += "\n   ";
        }
        data += fmt::format(" 0x{:08X},", (uint32_t)image[i] << 24 | (uint32_t)image[i + 1] << 16 | (uint32_t)image[i + 2] << 8 | image[i + 3]);
    }
    data += "\n};\n\n";
    data += fmt::format("const uint32_t mods_num_funcs = {};\nconst ModFunc mods_funcs[] = {{\n", func_table.size());
    for (const auto& [vram, name] : func_table) {
        data += fmt::format("    {{ 0x{:08X}, {} }},\n", vram, name);
    }
    data += "};\n\n";

    for (const Mod& mod : mods) {
        data += fmt::format("static const ModConfigValue {}_config[] = {{\n", mod.prefix);
        for (const auto& cv : mod.config) {
            data += fmt::format("    {{ {}, {}u, {:.17g}, {} }},\n", c_string(cv.key), cv.u32, cv.dbl, c_string(cv.str));
        }
        data += "    { 0, 0, 0, 0 }\n};\n";
        data += fmt::format("const ModInfo {}_info = {{ {}, {}, {{ {}, {}, {} }}, {}_config, {} }};\n",
            mod.prefix, c_string(mod.id), c_string(mod.prefix), mod.version[0], mod.version[1], mod.version[2], mod.prefix, mod.config.size());
        for (const auto& name : mod.imports_used) {
            data += fmt::format("RECOMP_FUNC void {0}_import_{1}(uint8_t* rdram, recomp_context* ctx) {{ modrt_{1}(rdram, ctx, &{0}_info); }}\n",
                mod.prefix, name);
        }
        data += "\n";
    }
    data += fmt::format("const uint32_t mods_count = {};\nconst ModInfo* const mods_list[] = {{", mods.size());
    for (const Mod& mod : mods) {
        data += fmt::format(" &{}_info,", mod.prefix);
    }
    data += " 0 };\n";
    write_if_changed(out_dir / "mods_data.c", data);
    outputs.push_back("mods_data.c");

    // Glue for the game's side: what its hooks call, and the list of hooks for its config.
    std::string hooks_h = "// Generated by tools/modrecomp. Don't edit.\n#pragma once\n#include \"recomp.h\"\n\n"
        "void mod_call_hook(uint8_t* rdram, recomp_context* ctx, recomp_func_t* hook);\n";
    std::string json = "[\n";
    bool first = true;
    for (const Mod& mod : mods) {
        for (const Hook& hook : mod.hooks) {
            hooks_h += fmt::format("RECOMP_FUNC void {}(uint8_t* rdram, recomp_context* ctx);\n", hook.call);
            json += fmt::format("{}  {{ \"mod\": \"{}\", \"func\": \"{}\", \"kind\": \"{}\", \"call\": \"{}\" }}", first ? "" : ",\n",
                mod.prefix, hook.game_func, hook.kind, hook.call);
            first = false;
        }
    }
    json += "\n]\n";
    write_if_changed(out_dir / "mods_hooks.h", hooks_h);
    write_if_changed(out_dir / "mods_hooks.json", json);

    // Where each section went, for tools that look up a mod's symbols (scripts/test_syms.py).
    std::string layout = "# mod section link_address load_address size\n";
    for (const Mod& mod : mods) {
        for (size_t s = 0; s < mod.context.sections.size(); s++) {
            const auto& section = mod.context.sections[s];
            layout += fmt::format("{} {} 0x{:08X} 0x{:08X} 0x{:X}\n", mod.prefix, s, section.ram_addr, mod.section_load[s],
                section.size + section.bss_size);
        }
    }
    write_if_changed(out_dir / "mods_layout.txt", layout);

    // Remove outputs left over from an earlier, larger build.
    for (const auto& entry : fs::directory_iterator(out_dir)) {
        if (entry.path().extension() == ".c" &&
            std::find(outputs.begin(), outputs.end(), entry.path().filename().string()) == outputs.end()) {
            fs::remove(entry.path());
        }
    }

    // Summary.
    for (const Mod& mod : mods) {
        const auto& ctx = mod.context;
        uint32_t size = 0, bss = 0;
        for (const auto& section : ctx.sections) {
            size += section.size;
            bss += section.bss_size;
        }
        size_t patches = 0, hooks = 0, ret_hooks = 0;
        for (const Hook& h : mod.hooks) {
            (h.kind == "patch" ? patches : h.kind == "hook" ? hooks : ret_hooks)++;
        }
        fmt::print("modrecomp: {} {}.{}.{} at 0x{:08X}-0x{:08X} ({} KB): {} functions, {} sections, {} bytes of sections + {} bss; "
                   "{} patches, {} hooks, {} return hooks; imports: ",
            mod.id, mod.version[0], mod.version[1], mod.version[2], mod.start, mod.end, (mod.end - mod.start + 1023) / 1024,
            ctx.functions.size(), ctx.sections.size(), size, bss, patches, hooks, ret_hooks);
        bool first_import = true;
        for (const auto& name : mod.imports_used) {
            fmt::print("{}{}", first_import ? "" : ", ", name);
            first_import = false;
        }
        fmt::print("\n");
    }
    fmt::print("modrecomp: mods use 0x{:X} of 0x{:X} bytes of mod RAM; the rest is the recomp_alloc heap\n", mods_end - load_address, ram_size);
    return EXIT_SUCCESS;
}
