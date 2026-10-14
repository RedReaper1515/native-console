#include "emitter.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace snesrecomp {

namespace {

std::string hex(uint32_t v, int digits)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*X", digits, v);
    return buf;
}

std::string key_name(uint32_t key) { return hex(key_pc(key), 6) + "_" + std::to_string(key_mode(key)); }
std::string label(uint32_t key) { return "L_" + key_name(key); }
std::string function_name(uint32_t entry) { return "rc_" + key_name(entry); }

std::string disasm(const Insn &in)
{
    const SnesOpInfo *info = snes_opinfo(in.op);
    std::string text = info->mnemonic;
    if (in.len > 1) text += " $" + hex(in.operand, 2 * (in.len - 1));
    return text;
}

class FunctionEmitter {
public:
    FunctionEmitter(const Analyzer &an, const Function &fn, std::ostream &out) : an_(an), fn_(fn), out_(out)
    {
        members_.insert(fn.insns.begin(), fn.insns.end());
    }

    void emit()
    {
        // Every instruction is a resume point: code yields after any
        // instruction (an event, or the SA-1 reaching the S-CPU's time), and
        // the dispatcher must be able to continue there natively.
        resume_.insert(fn_.insns.begin(), fn_.insns.end());
        out_ << "void " << function_name(fn_.entry) << "(SnesCpu *c, uint32_t entry)\n{\n";
        if (fn_.wram) {
            // WRAM code: leave as soon as code pages are written, so a
            // routine that changes its own bytes is checked again.
            out_ << "    SnesMachine *m = SNES_MACHINE(c);\n";
            out_ << "    const uint64_t gen = m->rc_ram_gen;\n";
        }
        out_ << "    switch (entry) {\n";
        for (uint32_t key : fn_.insns) {
            if (resume_.count(key)) out_ << "    case 0x" << hex(key, 7) << "u: goto " << label(key) << ";\n";
        }
        out_ << "    default: return;\n    }\n";
        for (size_t i = 0; i < fn_.insns.size(); i++) {
            uint32_t following = i + 1 < fn_.insns.size() ? fn_.insns[i + 1] : 0;
            emit_insn(an_.insns.at(fn_.insns[i]), following);
        }
        out_ << "}\n\n";
    }

private:
    std::string yield() const
    {
        return fn_.wram ? "if (SNES_RC_YIELD(c) || m->rc_ram_gen != gen) return;" : "if (SNES_RC_YIELD(c)) return;";
    }

    // Continue at `key` (the next instruction or a branch target).
    void continue_at(uint32_t key, uint32_t following, const std::string &ind)
    {
        if (key == following) return;
        if (members_.count(key)) out_ << ind << "goto " << label(key) << ";\n";
        else out_ << ind << "return;\n";
    }

    void emit_insn(const Insn &in, uint32_t following)
    {
        const std::string ind = "    ";
        out_ << label(in.key) << ": /* " << hex(in.pc >> 16, 2) << ":" << hex(in.pc & 0xFFFFu, 4) << " " << disasm(in)
             << " */\n";
        out_ << ind << "snes_rc_fetch(c, 0x" << hex(in.pc, 6) << "u);\n";
        out_ << ind << "snes_op_" << hex(in.op, 2) << "(c, " << unsigned(in.mode) << "u);\n";
        out_ << ind << "c->instructions++;\n";

        switch (in.flow) {
        case SNES_FLOW_BRANCH_COND:
            out_ << ind << "if (c->branched) {\n";
            out_ << ind << "    c->branched = 0;\n";
            out_ << ind << "    " << yield() << "\n";
            continue_at(in.target, 0, ind + "    ");
            out_ << ind << "}\n";
            out_ << ind << yield() << "\n";
            continue_at(in.next, following, ind);
            return;
        case SNES_FLOW_BRANCH:
        case SNES_FLOW_JUMP:
            out_ << ind << "c->branched = 0;\n";
            out_ << ind << yield() << "\n";
            continue_at(in.target, following, ind);
            return;
        case SNES_FLOW_BLOCK:
            // MVN/MVP moves one byte and steps back onto itself until done.
            out_ << ind << "if (c->branched) {\n";
            out_ << ind << "    c->branched = 0;\n";
            out_ << ind << "    " << yield() << "\n";
            out_ << ind << "    goto " << label(in.key) << ";\n";
            out_ << ind << "}\n";
            out_ << ind << yield() << "\n";
            continue_at(in.next, following, ind);
            return;
        default:
            break;
        }
        if (in.exits) {
            // Calls, returns, indirect jumps, and mode changes the analyzer
            // cannot follow: the run loop continues at c->pc.
            out_ << ind << "c->branched = 0;\n";
            out_ << ind << "return;\n";
            return;
        }
        out_ << ind << yield() << "\n";
        continue_at(in.next, following, ind);
    }

    const Analyzer &an_;
    const Function &fn_;
    std::ostream &out_;
    std::set<uint32_t> members_;
    std::set<uint32_t> resume_;
};

bool write_file(const fs::path &path, const std::string &text, std::string &error)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot write " + path.string();
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

std::string c_string(const std::string &s)
{
    std::string out = "\"";
    for (char ch : s) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    return out + "\"";
}

} // namespace

bool emit_project(const Rom &rom, const Analyzer &an, const EmitOptions &options, EmitStats &stats, std::string &error)
{
    std::error_code ec;
    fs::path out(options.out_dir);
    fs::path gen = out / "generated";
    fs::create_directories(gen, ec);
    if (ec) {
        error = "cannot create " + gen.string() + ": " + ec.message();
        return false;
    }
    for (const auto &entry : fs::directory_iterator(gen)) {
        if (entry.path().extension() == ".c" || entry.path().extension() == ".h") fs::remove(entry.path(), ec);
    }

    // Which function the dispatcher enters at each instruction: the function
    // it is the entry of, else the first function containing it.
    std::map<uint32_t, size_t> owner; // key -> function index
    for (size_t i = 0; i < an.functions.size(); i++) owner[an.functions[i].entry] = i;
    for (size_t i = 0; i < an.functions.size(); i++) {
        for (uint32_t key : an.functions[i].insns) {
            if (!owner.count(key)) owner[key] = i;
        }
    }

    std::ostringstream header;
    header << "/* Generated by snesrecomp. Do not edit. */\n#pragma once\n";
    if (options.inline_handlers) header << "#define SNES_RC_INLINE_OPS 1\n";
    header << "#include \"snes_recomp.h\"\n\n";
    for (const Function &fn : an.functions) header << "void " << function_name(fn.entry) << "(SnesCpu *c, uint32_t entry);\n";
    if (!write_file(gen / "rc_functions.h", header.str(), error)) return false;

    size_t file_index = 0;
    for (size_t first = 0; first < an.functions.size(); first += options.functions_per_file) {
        std::ostringstream src;
        src << "/* Generated by snesrecomp from \"" << rom.title << "\". Do not edit. */\n";
        src << "#include \"rc_functions.h\"\n\n";
        size_t last = std::min(first + options.functions_per_file, an.functions.size());
        for (size_t i = first; i < last; i++) {
            FunctionEmitter(an, an.functions[i], src).emit();
            stats.instructions += an.functions[i].insns.size();
        }
        char name[32];
        std::snprintf(name, sizeof name, "rc_%04zu.c", file_index++);
        if (!write_file(gen / name, src.str(), error)) return false;
    }
    stats.files = file_index;
    stats.functions = an.functions.size();
    stats.entries = owner.size();

    // WRAM code: each such function gets the bytes it was compiled from,
    // which the runtime compares with live WRAM before running it.
    std::map<size_t, int> ram_block; // function index -> block index
    std::ostringstream blocks;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (size_t f = 0; f < an.functions.size(); f++) {
        const Function &fn = an.functions[f];
        if (!fn.wram) continue;
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t key : fn.insns) {
            const Insn &in = an.insns.at(key);
            for (uint32_t i = 0; i < in.len; i++) {
                int32_t o = snes_rc_wram_offset(pc_add(in.pc, i));
                if (o < 0) continue;
                lo = std::min(lo, static_cast<uint32_t>(o));
                hi = std::max(hi, static_cast<uint32_t>(o) + 1u);
            }
        }
        if (lo >= hi) continue;
        int index = static_cast<int>(ranges.size());
        ram_block[f] = index;
        ranges.emplace_back(lo, hi - lo);
        blocks << "static const uint8_t ram_bytes_" << index << "[" << (hi - lo) << "] = {";
        for (uint32_t i = 0; i < hi - lo; i++) {
            blocks << (i ? "," : "") << (i % 24 ? "" : "\n    ") << unsigned(an.rom.machine()->wram[lo + i]);
        }
        blocks << "\n};\n";
        stats.wram_functions++;
        stats.wram_bytes += hi - lo;
    }

    std::ostringstream table;
    table << "/* Generated by snesrecomp. Do not edit. */\n#include \"rc_functions.h\"\n\n";
    table << blocks.str() << "\n";
    if (!ranges.empty()) {
        table << "static const SnesRcRamBlock ram_blocks[] = {\n";
        for (size_t i = 0; i < ranges.size(); i++) {
            table << "    { 0x" << hex(ranges[i].first, 5) << "u, " << ranges[i].second << "u, ram_bytes_" << i << " },\n";
        }
        table << "};\n\n";
    }
    table << "static const SnesRcEntry entries[] = {\n";
    for (const auto &[key, f] : owner) {
        auto b = ram_block.find(f);
        table << "    { 0x" << hex(key, 7) << "u, " << function_name(an.functions[f].entry) << ", "
              << (b == ram_block.end() ? -1 : b->second) << " },\n";
    }
    table << "};\n\n";
    table << "const uint8_t *snes_rc_embedded_rom(size_t *size);\n\n";
    table << "const SnesRcProgram snes_rc_program = {\n";
    table << "    entries,\n    " << owner.size() << "u,\n    " << rom.data.size() << "u,\n";
    table << "    " << c_string(rom.sha256) << ",\n    " << c_string(rom.title) << ",\n";
    table << "    " << (ranges.empty() ? "NULL" : "ram_blocks") << ",\n    " << ranges.size() << "u,\n";
    table << "    snes_rc_embedded_rom,\n};\n";
    if (!write_file(gen / "rc_table.c", table.str(), error)) return false;

    // The ROM goes into the executable: a Windows resource with MSVC (and
    // MinGW), .incbin elsewhere. Large ROMs as C arrays compile slowly.
    std::ostringstream rom_c;
    rom_c << "/* Generated by snesrecomp: the ROM compiled into this executable. */\n";
    rom_c << "#include <stddef.h>\n#include <stdint.h>\n\n";
    rom_c << "#if defined(_WIN32)\n#include <windows.h>\n\n";
    rom_c << "const uint8_t *snes_rc_embedded_rom(size_t *size)\n{\n";
    rom_c << "    HRSRC res = FindResourceA(NULL, \"SNESROM\", MAKEINTRESOURCEA(10)); /* RT_RCDATA */\n";
    rom_c << "    HGLOBAL data = res ? LoadResource(NULL, res) : NULL;\n";
    rom_c << "    if (!data) return NULL;\n";
    rom_c << "    *size = SizeofResource(NULL, res);\n";
    rom_c << "    return (const uint8_t *)LockResource(data);\n}\n\n";
    rom_c << "#else\n";
    rom_c << "#if defined(__APPLE__)\n#define SNES_RC_SYM(x) \"_\" #x\n#define SNES_RC_SECTION \".const_data\"\n";
    rom_c << "#else\n#define SNES_RC_SYM(x) #x\n#define SNES_RC_SECTION \".section .rodata\"\n#endif\n";
    rom_c << "__asm__(SNES_RC_SECTION \"\\n.balign 16\\n.globl \" SNES_RC_SYM(snes_rc_rom_start) \"\\n\" SNES_RC_SYM(snes_rc_rom_start) \":\\n\"\n";
    rom_c << "        \".incbin \\\"\" SNES_RC_ROM_FILE \"\\\"\\n.globl \" SNES_RC_SYM(snes_rc_rom_end) \"\\n\" SNES_RC_SYM(snes_rc_rom_end) \":\\n.text\\n\");\n";
    rom_c << "extern const uint8_t snes_rc_rom_start[], snes_rc_rom_end[];\n\n";
    rom_c << "const uint8_t *snes_rc_embedded_rom(size_t *size)\n{\n";
    rom_c << "    *size = (size_t)(snes_rc_rom_end - snes_rc_rom_start);\n";
    rom_c << "    return snes_rc_rom_start;\n}\n#endif\n";
    if (!write_file(gen / "rc_rom.c", rom_c.str(), error)) return false;

    fs::create_directories(out / "rom", ec);
    {
        std::ofstream rom_out(out / "rom" / (options.name + ".sfc"), std::ios::binary);
        rom_out.write(reinterpret_cast<const char *>(rom.data.data()), static_cast<std::streamsize>(rom.data.size()));
        if (!rom_out) {
            error = "cannot write the ROM copy into " + (out / "rom").string();
            return false;
        }
    }

    std::ostringstream main_c;
    main_c << "/* Generated by snesrecomp. */\n#include \"snes_recomp.h\"\n#include \"snes_runner.h\"\n";
    main_c << "#ifdef SNES_HAVE_SDL\n#include \"snes_sdl.h\"\n#endif\n\n";
    main_c << "extern const SnesRcProgram snes_rc_program;\n\n";
    main_c << "int main(int argc, char **argv)\n{\n";
    main_c << "#ifdef SNES_HAVE_SDL\n";
    main_c << "    if (!snes_sdl_wants_headless(argc, argv)) return snes_sdl_main(argc, argv, &snes_rc_program, NULL);\n";
    main_c << "#endif\n";
    main_c << "    return snes_runner_main(argc, argv, &snes_rc_program, NULL);\n}\n";
    if (!write_file(out / "main.c", main_c.str(), error)) return false;

    const std::string &n = options.name;
    std::ostringstream cmake;
    cmake << "# Generated by snesrecomp.\n";
    cmake << "cmake_minimum_required(VERSION 3.16)\n";
    cmake << "# Release unless told otherwise. Before project(): with MSVC and a\n";
    cmake << "# single-configuration generator, project() itself makes it Debug.\n";
    cmake << "if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n";
    cmake << "    set(CMAKE_BUILD_TYPE Release CACHE STRING \"Build type\" FORCE)\nendif()\n";
    cmake << "project(" << n << " C)\n\n";
    cmake << "set(CMAKE_C_STANDARD 11)\nset(CMAKE_C_STANDARD_REQUIRED ON)\n\n";
    cmake << "# Runtime snapshot copied in by snesrecomp. Test tooling may link a\n";
    cmake << "# prebuilt runtime instead (SNESRT_PREBUILT_LIBS + SNESRT_PREBUILT_INCLUDE).\n";
    cmake << "set(SNESRT_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/runtime-snes\")\n";
    cmake << "if(SNESRT_PREBUILT_LIBS)\n";
    cmake << "    add_library(snesruntime INTERFACE)\n";
    cmake << "    target_link_libraries(snesruntime INTERFACE ${SNESRT_PREBUILT_LIBS})\n";
    cmake << "    target_include_directories(snesruntime INTERFACE \"${SNESRT_PREBUILT_INCLUDE}\")\n";
    cmake << "else()\n";
    cmake << "    file(GLOB SNESRT_SOURCES \"${SNESRT_DIR}/src/*.c\")\n";
    cmake << "    add_library(snesruntime STATIC ${SNESRT_SOURCES})\n";
    cmake << "    target_include_directories(snesruntime PUBLIC \"${SNESRT_DIR}/include\")\n";
    cmake << "    if(NOT WIN32)\n        target_link_libraries(snesruntime PUBLIC m)\n    endif()\n";
    cmake << "endif()\n\n";
    cmake << "file(GLOB RC_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c\")\n";
    cmake << "add_executable(" << n << " main.c ${RC_SOURCES})\n";
    cmake << "target_include_directories(" << n << " PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/generated\")\n";
    cmake << "target_link_libraries(" << n << " PRIVATE snesruntime)\n";
    cmake << "if(NOT WIN32)\n    target_link_libraries(" << n << " PRIVATE m)\nendif()\n";
    cmake << "if(MSVC)\n    target_compile_options(" << n << " PRIVATE /bigobj)\nendif()\n\n";
    cmake << "# The ROM is compiled into the executable (generated/rc_rom.c).\n";
    cmake << "set(SNES_RC_ROM \"${CMAKE_CURRENT_SOURCE_DIR}/rom/" << n << ".sfc\")\n";
    cmake << "if(WIN32)\n";
    cmake << "    enable_language(RC)\n";
    cmake << "    file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\" \"SNESROM RCDATA \\\"${SNES_RC_ROM}\\\"\\n\")\n";
    cmake << "    target_sources(" << n << " PRIVATE \"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\")\n";
    cmake << "    set_source_files_properties(\"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\" PROPERTIES OBJECT_DEPENDS \"${SNES_RC_ROM}\")\n";
    cmake << "else()\n";
    cmake << "    set_source_files_properties(generated/rc_rom.c PROPERTIES\n";
    cmake << "        COMPILE_DEFINITIONS \"SNES_RC_ROM_FILE=\\\"${SNES_RC_ROM}\\\"\" OBJECT_DEPENDS \"${SNES_RC_ROM}\")\n";
    cmake << "endif()\n\n";
    cmake << "# Window, input, audio and the settings menu (Dear ImGui) when SDL2 is\n";
    cmake << "# available; headless otherwise.\n";
    cmake << "find_package(SDL2 QUIET)\n";
    cmake << "if(TARGET SDL2::SDL2)\n";
    cmake << "    enable_language(CXX)\n";
    cmake << "    set(SNES_IMGUI_DIR \"${SNESRT_DIR}/vendor/imgui\")\n";
    cmake << "    target_sources(" << n << " PRIVATE \"${SNESRT_DIR}/platform/snes_sdl.cpp\"\n";
    cmake << "        \"${SNESRT_DIR}/platform/snes_input.cpp\"\n";
    cmake << "        \"${SNES_IMGUI_DIR}/imgui.cpp\" \"${SNES_IMGUI_DIR}/imgui_draw.cpp\"\n";
    cmake << "        \"${SNES_IMGUI_DIR}/imgui_tables.cpp\" \"${SNES_IMGUI_DIR}/imgui_widgets.cpp\"\n";
    cmake << "        \"${SNES_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp\"\n";
    cmake << "        \"${SNES_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp\")\n";
    cmake << "    target_include_directories(" << n << " PRIVATE \"${SNESRT_DIR}/platform\" \"${SNES_IMGUI_DIR}\")\n";
    cmake << "    set_target_properties(" << n << " PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)\n";
    cmake << "    target_compile_definitions(" << n << " PRIVATE SNES_HAVE_SDL)\n";
    cmake << "    target_link_libraries(" << n << " PRIVATE SDL2::SDL2)\n";
    cmake << "    get_target_property(_sdl2_type SDL2::SDL2 TYPE)\n";
    cmake << "    if(WIN32 AND _sdl2_type STREQUAL \"SHARED_LIBRARY\")\n";
    cmake << "        add_custom_command(TARGET " << n << " POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different\n";
    cmake << "            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:" << n << ">)\n";
    cmake << "    endif()\n";
    cmake << "    # macOS .app bundle (off unless NATIVE_CONSOLE_MACOS_BUNDLE=ON)\n";
    cmake << "    include(\"${SNESRT_DIR}/cmake/NativeConsoleMacBundle.cmake\")\n";
    cmake << "    native_console_macos_bundle(" << n << ")\n";
    cmake << "endif()\n";
    if (!write_file(out / "CMakeLists.txt", cmake.str(), error)) return false;

    // Snapshot the runtime so the project builds on its own.
    fs::path rt_src(options.runtime_dir);
    fs::path rt_dst = out / "runtime-snes";
    for (const char *sub : { "include", "src", "platform", "cmake" }) {
        fs::create_directories(rt_dst / sub, ec);
        fs::copy(rt_src / sub, rt_dst / sub, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy runtime from " + (rt_src / sub).string() + ": " + ec.message();
            return false;
        }
    }
    // Dear ImGui for the settings menu: the copy vendored with the Game Boy
    // runtime in the source tree, or one already in a runtime snapshot.
    fs::path imgui_src = rt_src / "vendor" / "imgui";
    if (!fs::exists(imgui_src / "imgui.h")) imgui_src = rt_src.parent_path() / "runtime-gb" / "vendor" / "imgui";
    if (!fs::exists(imgui_src / "imgui.h")) {
        error = "Dear ImGui not found in " + (rt_src / "vendor" / "imgui").string() + " or " + imgui_src.string();
        return false;
    }
    fs::create_directories(rt_dst / "vendor" / "imgui", ec);
    fs::copy(imgui_src, rt_dst / "vendor" / "imgui", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "cannot copy Dear ImGui from " + imgui_src.string() + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace snesrecomp
