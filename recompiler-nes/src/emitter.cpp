#include "emitter.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace nesrecomp {

namespace {

std::string hex(uint32_t v, int digits)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*X", digits, v);
    return buf;
}

std::string key_name(uint32_t key) { return hex(key_page(key), 2) + "_" + hex(key_pc(key), 4); }
std::string label(uint32_t key) { return "L_" + key_name(key); }
std::string function_name(uint32_t entry) { return "rc_" + key_name(entry); }

std::string disasm(const Insn &in)
{
    std::string text = nes_opinfo(in.op)->mnemonic;
    if (in.len > 1 && in.operand_known) text += " $" + hex(in.operand, 2 * (in.len - 1));
    else if (in.len > 1) text += " (operand in the next page)";
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
        // instruction (an event, an interrupt, a bank switch), and the run
        // loop must be able to continue there natively.
        out_ << "void " << function_name(fn_.entry) << "(NesCpu *c, uint32_t entry)\n{\n";
        out_ << "    switch (entry) {\n";
        for (uint32_t key : fn_.insns) out_ << "    case 0x" << hex(key, 6) << "u: goto " << label(key) << ";\n";
        out_ << "    default: return;\n    }\n";
        for (size_t i = 0; i < fn_.insns.size(); i++) {
            uint32_t following = i + 1 < fn_.insns.size() ? fn_.insns[i + 1] : 0;
            emit_insn(an_.insns.at(fn_.insns[i]), following);
        }
        out_ << "}\n\n";
    }

private:
    // After every instruction: due events, a pending interrupt, or a PRG
    // bank switch (the bytes at the next address may have changed) send
    // execution back to the run loop.
    static const char *yield() { return "if (NES_RC_YIELD(c)) return;"; }

    // Continue at `key`: the next instruction or a branch target in this
    // page, or 0 when it is in another page.
    void continue_at(uint32_t key, uint32_t following, const std::string &ind)
    {
        if (key && key == following) return;
        if (key && members_.count(key)) out_ << ind << "goto " << label(key) << ";\n";
        else out_ << ind << "return;\n";
    }

    void emit_insn(const Insn &in, uint32_t following)
    {
        const std::string ind = "    ";
        out_ << label(in.key) << ": /* " << hex(key_page(in.key), 2) << ":" << hex(key_pc(in.key), 4) << " "
             << disasm(in) << " */\n";
        out_ << ind << "nes_rc_fetch(c, 0x" << hex(key_pc(in.key), 4) << "u);\n";
        out_ << ind << "nes_op_" << hex(in.op, 2) << "(c);\n";
        out_ << ind << "c->instructions++;\n";

        if (in.flow == NES_FLOW_BRANCH_COND) {
            out_ << ind << "if (c->branched) {\n";
            out_ << ind << "    c->branched = 0;\n";
            out_ << ind << "    " << yield() << "\n";
            continue_at(in.target, 0, ind + "    ");
            out_ << ind << "}\n";
            out_ << ind << yield() << "\n";
            continue_at(in.next, following, ind);
            return;
        }
        if (in.flow == NES_FLOW_JUMP && in.target) {
            out_ << ind << "c->branched = 0;\n";
            out_ << ind << yield() << "\n";
            continue_at(in.target, following, ind);
            return;
        }
        if (in.exits) {
            // Calls, returns, indirect jumps, and anything that leaves the
            // page: the run loop continues at c->pc under the cartridge's
            // mapping of that moment.
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

    // Which function the run loop enters at each instruction: the function
    // it is the entry of, else the first function containing it.
    std::map<uint32_t, size_t> owner; // key -> function index
    for (size_t i = 0; i < an.functions.size(); i++) owner[an.functions[i].entry] = i;
    for (size_t i = 0; i < an.functions.size(); i++) {
        for (uint32_t key : an.functions[i].insns) {
            if (!owner.count(key)) owner[key] = i;
        }
    }

    std::ostringstream header;
    header << "/* Generated by nesrecomp. Do not edit. */\n#pragma once\n";
    if (options.inline_handlers) header << "#define NES_RC_INLINE_OPS 1\n";
    header << "#include \"nes_recomp.h\"\n\n";
    for (const Function &fn : an.functions) header << "void " << function_name(fn.entry) << "(NesCpu *c, uint32_t entry);\n";
    if (!write_file(gen / "rc_functions.h", header.str(), error)) return false;

    size_t file_index = 0;
    for (size_t first = 0; first < an.functions.size(); first += options.functions_per_file) {
        std::ostringstream src;
        src << "/* Generated by nesrecomp from \"" << rom.title << "\". Do not edit. */\n";
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

    std::ostringstream table;
    table << "/* Generated by nesrecomp. Do not edit. */\n#include \"rc_functions.h\"\n\n";
    table << "static const NesRcEntry entries[] = {\n";
    for (const auto &[key, f] : owner) {
        table << "    { 0x" << hex(key, 6) << "u, " << function_name(an.functions[f].entry) << " },\n";
    }
    if (owner.empty()) table << "    { 0xFFFFFFFFu, 0 },\n";
    table << "};\n\n";
    table << "const uint8_t *nes_rc_embedded_rom(size_t *size);\n\n";
    table << "const NesRcProgram nes_rc_program = {\n";
    table << "    entries,\n    " << owner.size() << "u,\n    " << rom.file.size() << "u,\n";
    table << "    " << c_string(rom.sha256) << ",\n    " << c_string(rom.title) << ",\n";
    table << "    nes_rc_embedded_rom,\n};\n";
    if (!write_file(gen / "rc_table.c", table.str(), error)) return false;

    // The ROM goes into the executable: a Windows resource with MSVC (and
    // MinGW), .incbin elsewhere.
    std::ostringstream rom_c;
    rom_c << "/* Generated by nesrecomp: the ROM compiled into this executable. */\n";
    rom_c << "#include <stddef.h>\n#include <stdint.h>\n\n";
    rom_c << "#if defined(_WIN32)\n#include <windows.h>\n\n";
    rom_c << "const uint8_t *nes_rc_embedded_rom(size_t *size)\n{\n";
    rom_c << "    HRSRC res = FindResourceA(NULL, \"NESROM\", MAKEINTRESOURCEA(10)); /* RT_RCDATA */\n";
    rom_c << "    HGLOBAL data = res ? LoadResource(NULL, res) : NULL;\n";
    rom_c << "    if (!data) return NULL;\n";
    rom_c << "    *size = SizeofResource(NULL, res);\n";
    rom_c << "    return (const uint8_t *)LockResource(data);\n}\n\n";
    rom_c << "#else\n";
    rom_c << "#if defined(__APPLE__)\n#define NES_RC_SYM(x) \"_\" #x\n#define NES_RC_SECTION \".const_data\"\n";
    rom_c << "#else\n#define NES_RC_SYM(x) #x\n#define NES_RC_SECTION \".section .rodata\"\n#endif\n";
    rom_c << "__asm__(NES_RC_SECTION \"\\n.balign 16\\n.globl \" NES_RC_SYM(nes_rc_rom_start) \"\\n\" NES_RC_SYM(nes_rc_rom_start) \":\\n\"\n";
    rom_c << "        \".incbin \\\"\" NES_RC_ROM_FILE \"\\\"\\n.globl \" NES_RC_SYM(nes_rc_rom_end) \"\\n\" NES_RC_SYM(nes_rc_rom_end) \":\\n.text\\n\");\n";
    rom_c << "extern const uint8_t nes_rc_rom_start[], nes_rc_rom_end[];\n\n";
    rom_c << "const uint8_t *nes_rc_embedded_rom(size_t *size)\n{\n";
    rom_c << "    *size = (size_t)(nes_rc_rom_end - nes_rc_rom_start);\n";
    rom_c << "    return nes_rc_rom_start;\n}\n#endif\n";
    if (!write_file(gen / "rc_rom.c", rom_c.str(), error)) return false;

    fs::create_directories(out / "rom", ec);
    {
        std::ofstream rom_out(out / "rom" / (options.name + ".nes"), std::ios::binary);
        rom_out.write(reinterpret_cast<const char *>(rom.file.data()), static_cast<std::streamsize>(rom.file.size()));
        if (!rom_out) {
            error = "cannot write the ROM copy into " + (out / "rom").string();
            return false;
        }
    }

    std::ostringstream main_c;
    main_c << "/* Generated by nesrecomp. */\n#include \"nes_recomp.h\"\n#include \"nes_runner.h\"\n";
    main_c << "#ifdef NES_HAVE_SDL\n#include \"nes_sdl.h\"\n#endif\n\n";
    main_c << "extern const NesRcProgram nes_rc_program;\n\n";
    main_c << "int main(int argc, char **argv)\n{\n";
    main_c << "#ifdef NES_HAVE_SDL\n";
    main_c << "    if (!nes_sdl_wants_headless(argc, argv)) return nes_sdl_main(argc, argv, &nes_rc_program, NULL);\n";
    main_c << "#endif\n";
    main_c << "    return nes_runner_main(argc, argv, &nes_rc_program, NULL);\n}\n";
    if (!write_file(out / "main.c", main_c.str(), error)) return false;

    const std::string &n = options.name;
    std::ostringstream cmake;
    cmake << "# Generated by nesrecomp.\n";
    cmake << "cmake_minimum_required(VERSION 3.16)\n";
    cmake << "# Release unless told otherwise. Before project(): with MSVC and a\n";
    cmake << "# single-configuration generator, project() itself makes it Debug.\n";
    cmake << "if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n";
    cmake << "    set(CMAKE_BUILD_TYPE Release CACHE STRING \"Build type\" FORCE)\nendif()\n";
    cmake << "project(" << n << " C)\n\n";
    cmake << "set(CMAKE_C_STANDARD 11)\nset(CMAKE_C_STANDARD_REQUIRED ON)\n\n";
    cmake << "# Runtime snapshot copied in by nesrecomp. Test tooling may link a\n";
    cmake << "# prebuilt runtime instead (NESRT_PREBUILT_LIBS + NESRT_PREBUILT_INCLUDE).\n";
    cmake << "set(NESRT_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/runtime-nes\")\n";
    cmake << "if(NESRT_PREBUILT_LIBS)\n";
    cmake << "    add_library(nesruntime INTERFACE)\n";
    cmake << "    target_link_libraries(nesruntime INTERFACE ${NESRT_PREBUILT_LIBS})\n";
    cmake << "    target_include_directories(nesruntime INTERFACE \"${NESRT_PREBUILT_INCLUDE}\")\n";
    cmake << "else()\n";
    cmake << "    file(GLOB NESRT_SOURCES \"${NESRT_DIR}/src/*.c\")\n";
    cmake << "    add_library(nesruntime STATIC ${NESRT_SOURCES})\n";
    cmake << "    target_include_directories(nesruntime PUBLIC \"${NESRT_DIR}/include\")\n";
    cmake << "    if(NOT WIN32)\n        target_link_libraries(nesruntime PUBLIC m)\n    endif()\n";
    cmake << "endif()\n\n";
    cmake << "file(GLOB RC_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c\")\n";
    cmake << "add_executable(" << n << " main.c ${RC_SOURCES})\n";
    cmake << "target_include_directories(" << n << " PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/generated\")\n";
    cmake << "target_link_libraries(" << n << " PRIVATE nesruntime)\n";
    cmake << "if(NOT WIN32)\n    target_link_libraries(" << n << " PRIVATE m)\nendif()\n";
    cmake << "if(MSVC)\n    target_compile_options(" << n << " PRIVATE /bigobj)\nendif()\n\n";
    cmake << "# The ROM is compiled into the executable (generated/rc_rom.c).\n";
    cmake << "set(NES_RC_ROM \"${CMAKE_CURRENT_SOURCE_DIR}/rom/" << n << ".nes\")\n";
    cmake << "if(WIN32)\n";
    cmake << "    enable_language(RC)\n";
    cmake << "    file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\" \"NESROM RCDATA \\\"${NES_RC_ROM}\\\"\\n\")\n";
    cmake << "    target_sources(" << n << " PRIVATE \"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\")\n";
    cmake << "    set_source_files_properties(\"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\" PROPERTIES OBJECT_DEPENDS \"${NES_RC_ROM}\")\n";
    cmake << "else()\n";
    cmake << "    set_source_files_properties(generated/rc_rom.c PROPERTIES\n";
    cmake << "        COMPILE_DEFINITIONS \"NES_RC_ROM_FILE=\\\"${NES_RC_ROM}\\\"\" OBJECT_DEPENDS \"${NES_RC_ROM}\")\n";
    cmake << "endif()\n\n";
    cmake << "# Window, input, audio and the settings menu (Dear ImGui) when SDL2 is\n";
    cmake << "# available; headless otherwise.\n";
    cmake << "find_package(SDL2 QUIET)\n";
    cmake << "if(TARGET SDL2::SDL2)\n";
    cmake << "    enable_language(CXX)\n";
    cmake << "    set(NES_IMGUI_DIR \"${NESRT_DIR}/vendor/imgui\")\n";
    cmake << "    target_sources(" << n << " PRIVATE \"${NESRT_DIR}/platform/nes_sdl.cpp\"\n";
    cmake << "        \"${NESRT_DIR}/platform/nes_input.cpp\"\n";
    cmake << "        \"${NES_IMGUI_DIR}/imgui.cpp\" \"${NES_IMGUI_DIR}/imgui_draw.cpp\"\n";
    cmake << "        \"${NES_IMGUI_DIR}/imgui_tables.cpp\" \"${NES_IMGUI_DIR}/imgui_widgets.cpp\"\n";
    cmake << "        \"${NES_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp\"\n";
    cmake << "        \"${NES_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp\")\n";
    cmake << "    target_include_directories(" << n << " PRIVATE \"${NESRT_DIR}/platform\" \"${NES_IMGUI_DIR}\")\n";
    cmake << "    set_target_properties(" << n << " PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)\n";
    cmake << "    target_compile_definitions(" << n << " PRIVATE NES_HAVE_SDL)\n";
    cmake << "    target_link_libraries(" << n << " PRIVATE SDL2::SDL2)\n";
    cmake << "    get_target_property(_sdl2_type SDL2::SDL2 TYPE)\n";
    cmake << "    if(WIN32 AND _sdl2_type STREQUAL \"SHARED_LIBRARY\")\n";
    cmake << "        add_custom_command(TARGET " << n << " POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different\n";
    cmake << "            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:" << n << ">)\n";
    cmake << "    endif()\n";
    cmake << "    # macOS .app bundle (off unless NATIVE_CONSOLE_MACOS_BUNDLE=ON)\n";
    cmake << "    include(\"${NESRT_DIR}/cmake/NativeConsoleMacBundle.cmake\")\n";
    cmake << "    native_console_macos_bundle(" << n << ")\n";
    cmake << "endif()\n";
    if (!write_file(out / "CMakeLists.txt", cmake.str(), error)) return false;

    // Snapshot the runtime so the project builds on its own.
    fs::path rt_src(options.runtime_dir);
    fs::path rt_dst = out / "runtime-nes";
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
    if (!fs::exists(imgui_src / "imgui.h")) imgui_src = rt_src.parent_path() / "runtime" / "vendor" / "imgui";
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

} // namespace nesrecomp
