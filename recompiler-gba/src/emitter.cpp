#include "emitter.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

extern "C" {
#include "gba_cpu_decode.h"
}

namespace fs = std::filesystem;

namespace gbarecomp {

namespace {

std::string hex8(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

std::string label(uint32_t key) { return "L_" + hex8(key_addr(key)); }

std::string function_name(uint32_t entry)
{
    return "rc_" + hex8(key_addr(entry)) + (key_thumb(entry) ? "_t" : "_a");
}

const char *arm_handler(GbaArmClass cls)
{
    switch (cls) {
    case GBA_ARM_BX: return "gba_arm_bx";
    case GBA_ARM_SWP: return "gba_arm_swp";
    case GBA_ARM_MUL: return "gba_arm_mul";
    case GBA_ARM_MULL: return "gba_arm_mull";
    case GBA_ARM_HALF: return "gba_arm_half";
    case GBA_ARM_MRS: return "gba_arm_mrs";
    case GBA_ARM_MSR: return "gba_arm_msr";
    case GBA_ARM_DP: return "gba_arm_dp";
    case GBA_ARM_SDT: return "gba_arm_sdt";
    case GBA_ARM_BLOCK: return "gba_arm_block";
    case GBA_ARM_B: return "gba_arm_b";
    default: return nullptr; // SWI / undefined: no opcode argument
    }
}

const char *thumb_handler(GbaThumbClass cls)
{
    switch (cls) {
    case GBA_THUMB_SHIFT: return "gba_thumb_shift_imm";
    case GBA_THUMB_ADD_SUB: return "gba_thumb_add_sub";
    case GBA_THUMB_IMM8: return "gba_thumb_imm8";
    case GBA_THUMB_ALU: return "gba_thumb_alu";
    case GBA_THUMB_HI_REG: return "gba_thumb_hi_reg";
    case GBA_THUMB_LDR_PC: return "gba_thumb_ldr_pc";
    case GBA_THUMB_LDST_REG: return "gba_thumb_ldst_reg";
    case GBA_THUMB_LDST_SIGN: return "gba_thumb_ldst_sign";
    case GBA_THUMB_LDST_IMM: return "gba_thumb_ldst_imm";
    case GBA_THUMB_LDST_HALF: return "gba_thumb_ldst_half";
    case GBA_THUMB_LDST_SP: return "gba_thumb_ldst_sp";
    case GBA_THUMB_ADD_PC_SP: return "gba_thumb_add_pc_sp";
    case GBA_THUMB_ADD_SP: return "gba_thumb_add_sp";
    case GBA_THUMB_PUSH_POP: return "gba_thumb_push_pop";
    case GBA_THUMB_LDM_STM: return "gba_thumb_ldm_stm";
    case GBA_THUMB_BCC: return "gba_thumb_bcc";
    case GBA_THUMB_B: return "gba_thumb_b";
    case GBA_THUMB_BL: return "gba_thumb_bl";
    default: return nullptr;
    }
}

// Does control fall through to the next instruction?
bool falls_through(const Insn &in)
{
    switch (in.flow) {
    case Flow::Next: case Flow::CondBranch: case Flow::Call: case Flow::CallIndirect:
        return !in.exits_after;
    case Flow::Branch:
        // ARM B with a condition falls through when it is not taken.
        return in.conditional;
    default:
        return in.conditional;
    }
}

// Can the instruction write R15 (and so set c->branched)? Instructions that
// cannot need no check after their handler.
bool may_branch(const Insn &in)
{
    if (!in.thumb) {
        const uint32_t op = in.op;
        const uint32_t rd = (op >> 12) & 15u, rn = (op >> 16) & 15u;
        const bool load = (op >> 20) & 1u;
        const bool base_written = !((op >> 24) & 1u) || ((op >> 21) & 1u); // post-indexed, or writeback
        switch (gba_arm_classify(op)) {
        case GBA_ARM_DP: return rd == 15;
        case GBA_ARM_MUL: return rn == 15;                // Rd is in bits 16-19
        case GBA_ARM_MULL: return rd == 15 || rn == 15;   // RdLo, RdHi
        case GBA_ARM_SWP: return rd == 15;
        case GBA_ARM_SDT:
        case GBA_ARM_HALF: return (load && rd == 15) || (rn == 15 && base_written);
        case GBA_ARM_BLOCK: return rn == 15 || (load && ((op & 0x8000u) || (op & 0xFFFFu) == 0)); // empty list loads R15
        case GBA_ARM_MRS:
        case GBA_ARM_MSR: return false;
        default: return true; // B, BL, BX, SWI, undefined
        }
    }
    const uint16_t op = static_cast<uint16_t>(in.op);
    switch (gba_thumb_classify(op)) {
    case GBA_THUMB_HI_REG: {
        const uint32_t rd = (op & 7u) | ((op >> 4) & 8u);
        const uint32_t kind = (op >> 8) & 3u; // ADD, CMP, MOV, BX
        return kind == 3 || (kind != 1 && rd == 15);
    }
    case GBA_THUMB_PUSH_POP: return (op & 0x0800u) && ((op & 0x100u) || (op & 0xFFu) == 0);
    case GBA_THUMB_LDM_STM: return (op & 0x0800u) && (op & 0xFFu) == 0;
    case GBA_THUMB_BL: return (op & 0x0800u) != 0;
    case GBA_THUMB_BCC: case GBA_THUMB_SWI: case GBA_THUMB_B: case GBA_THUMB_UNDEF: return true;
    default: return false;
    }
}

class FunctionEmitter {
public:
    FunctionEmitter(const Analyzer &an, const Function &fn, std::ostream &out) : an_(an), fn_(fn), out_(out)
    {
        members_.insert(fn.insns.begin(), fn.insns.end());
    }

    void emit()
    {
        plan_labels();
        const bool thumb = key_thumb(fn_.entry);
        out_ << "void " << function_name(fn_.entry) << "(GbaMachine *m, uint32_t entry)\n{\n";
        out_ << "    GbaCpu *c = &m->cpu;\n";
        out_ << "    switch (entry) {\n";
        for (uint32_t key : fn_.insns) {
            if (resume_.count(key)) out_ << "    case 0x" << hex8(key_addr(key)) << "u: goto " << label(key) << ";\n";
        }
        out_ << "    default: return;\n    }\n";
        (void)thumb;

        for (size_t i = 0; i < fn_.insns.size(); i++) {
            uint32_t key = fn_.insns[i];
            uint32_t following = i + 1 < fn_.insns.size() ? fn_.insns[i + 1] : 0xFFFFFFFFu;
            emit_insn(an_.insns.at(key), following);
        }
        out_ << "}\n\n";
    }

private:
    void plan_labels()
    {
        for (uint32_t key : fn_.insns) {
            if (an_.labels.count(key) || key == fn_.entry) resume_.insert(key);
        }
        needed_ = resume_;
        for (size_t i = 0; i < fn_.insns.size(); i++) {
            const Insn &in = an_.insns.at(fn_.insns[i]);
            uint32_t following = i + 1 < fn_.insns.size() ? fn_.insns[i + 1] : 0xFFFFFFFFu;
            if ((in.flow == Flow::Branch || in.flow == Flow::CondBranch) && members_.count(in.target)) {
                needed_.insert(in.target);
            }
            if (in.flow == Flow::Table) {
                for (uint32_t t : in.table) {
                    if (members_.count(t)) needed_.insert(t);
                }
            }
            uint32_t next = make_key(in.addr + in.size, in.thumb);
            if (falls_through(in) && next != following && members_.count(next)) needed_.insert(next);
        }
    }

    // Leave for the dispatcher with the pipeline pointing at `addr`.
    std::string exit_at(uint32_t addr, bool thumb) const
    {
        return "c->r[15] = 0x" + hex8(addr + (thumb ? 4u : 8u)) + "u; return;";
    }

    void emit_insn(const Insn &in, uint32_t following)
    {
        const uint32_t key = make_key(in.addr, in.thumb);
        const uint32_t pc = in.addr + (in.thumb ? 4u : 8u);
        const uint32_t next_addr = in.addr + in.size;
        const uint32_t next = make_key(next_addr, in.thumb);
        const std::string ind = "    ";

        if (needed_.count(key)) out_ << label(key) << ":\n";
        out_ << ind << "/* " << hex8(in.addr) << ": " << (in.thumb ? hex8(in.op).substr(4) : hex8(in.op)) << " */\n";
        out_ << ind << "c->r[15] = 0x" << hex8(pc) << "u;\n";
        out_ << ind << "gba_rc_fetch(m, 0x" << hex8(pc) << "u, " << (in.thumb ? 0 : 1) << ");\n";
        out_ << ind << "m->instructions++;\n";

        std::string call;
        bool intra_target = (in.flow == Flow::Branch || in.flow == Flow::CondBranch) && members_.count(in.target);
        std::string inner = ind;
        bool wrapped = false;

        if (!in.thumb) {
            const uint32_t cond = in.op >> 28;
            const GbaArmClass cls = gba_arm_classify(in.op);
            const char *h = arm_handler(cls);
            if (cls == GBA_ARM_SWI) call = "gba_op_swi(c);";
            else if (cls == GBA_ARM_UNDEF) call = "gba_op_undefined(c);";
            else call = std::string(h) + "(c, 0x" + hex8(in.op) + "u);";
            if (cond == GBA_COND_NV) {
                out_ << ind << "/* never executes */\n";
                call.clear();
            } else if (cond != GBA_COND_AL) {
                out_ << ind << "if (gba_cond(c, " << cond << "u)) {\n";
                inner = ind + "    ";
                wrapped = true;
            }
        } else {
            const uint16_t op = static_cast<uint16_t>(in.op);
            const GbaThumbClass cls = gba_thumb_classify(op);
            const char *h = thumb_handler(cls);
            if (cls == GBA_THUMB_SWI) call = "gba_op_swi(c);";
            else if (cls == GBA_THUMB_UNDEF) call = "gba_op_undefined(c);";
            else call = std::string(h) + "(c, 0x" + hex8(op).substr(4) + "u);";
        }

        if (!call.empty()) {
            out_ << inner << call << "\n";
            if (intra_target) {
                out_ << inner << "if (c->branched) {\n";
                out_ << inner << "    c->branched = false;\n";
                out_ << inner << "    gba_rc_refill(m, 0x" << hex8(key_addr(in.target)) << "u, " << (in.thumb ? 0 : 1) << ");\n";
                out_ << inner << "    if (GBA_RC_YIELD(m)) return;\n";
                out_ << inner << "    goto " << label(in.target) << ";\n";
                out_ << inner << "}\n";
            } else if (may_branch(in)) {
                out_ << inner << "if (c->branched) return;\n";
            }
        }
        if (wrapped) out_ << ind << "}\n";

        if (in.exits_after) {
            out_ << ind << exit_at(next_addr, in.thumb) << "\n";
            return;
        }
        if (in.flow == Flow::Table && !in.conditional) {
            // A table jump that did not branch cannot happen; leave safely.
            out_ << ind << exit_at(next_addr, in.thumb) << "\n";
            return;
        }
        if (!falls_through(in) && !(in.flow == Flow::Branch && in.conditional)) {
            // Unconditional control transfer already returned; nothing follows.
            if (!(in.flow == Flow::Branch && intra_target)) out_ << ind << exit_at(next_addr, in.thumb) << "\n";
            return;
        }
        out_ << ind << "if (GBA_RC_YIELD(m)) { " << exit_at(next_addr, in.thumb) << " }\n";
        if (next != following) {
            if (members_.count(next)) out_ << ind << "goto " << label(next) << ";\n";
            else out_ << ind << exit_at(next_addr, in.thumb) << "\n";
        }
    }

    const Analyzer &an_;
    const Function &fn_;
    std::ostream &out_;
    std::set<uint32_t> members_;
    std::set<uint32_t> resume_;
    std::set<uint32_t> needed_;
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

bool emit_project(const Rom &rom, const Analyzer &an, const EmitOptions &options, EmitStats &stats,
                  std::string &error)
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

    // Which function the dispatcher enters for each resume label: the
    // function it is the entry of, else the first function containing it.
    std::map<uint32_t, uint32_t> owner; // label key -> function entry
    for (const Function &fn : an.functions) {
        if (an.labels.count(fn.entry)) owner[fn.entry] = fn.entry;
    }
    for (const Function &fn : an.functions) {
        for (uint32_t key : fn.insns) {
            if (an.labels.count(key) && !owner.count(key)) owner[key] = fn.entry;
        }
    }

    std::ostringstream header;
    header << "/* Generated by gbarecomp. Do not edit. */\n#pragma once\n";
    if (!options.inline_handlers) header << "#define GBA_RC_OUTLINE_HANDLERS 1\n";
    header << "#include \"gba_recomp.h\"\n\n";
    for (const Function &fn : an.functions) header << "void " << function_name(fn.entry) << "(GbaMachine *m, uint32_t entry);\n";
    if (!write_file(gen / "rc_functions.h", header.str(), error)) return false;

    size_t file_index = 0;
    for (size_t first = 0; first < an.functions.size();) {
        std::ostringstream src;
        src << "/* Generated by gbarecomp from " << rom.title << " (" << rom.game_code << "). Do not edit. */\n";
        src << "#include \"rc_functions.h\"\n\n";
        size_t last = first;
        if (options.functions_per_file > 0) {
            last = std::min(first + options.functions_per_file, an.functions.size());
        } else {
            for (size_t insns = 0; last < an.functions.size() && (last == first || insns < options.instructions_per_file); last++)
                insns += an.functions[last].insns.size();
        }
        for (size_t i = first; i < last; i++) {
            FunctionEmitter(an, an.functions[i], src).emit();
            stats.instructions += an.functions[i].insns.size();
        }
        char name[32];
        std::snprintf(name, sizeof name, "rc_%04zu.c", file_index++);
        if (!write_file(gen / name, src.str(), error)) return false;
        first = last;
    }
    stats.files = file_index;
    stats.functions = an.functions.size();
    stats.entries = owner.size();

    // RAM code: each function compiled from a RAM image gets the bytes it was
    // compiled from, which the runtime compares with live RAM before use.
    std::map<uint32_t, int> ram_block; // function entry -> block index
    std::ostringstream blocks;
    std::vector<std::pair<uint32_t, uint32_t>> block_ranges;
    for (const Function &fn : an.functions) {
        if (!is_ram_address(key_addr(fn.entry))) continue;
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t key : fn.insns) {
            const Insn &in = an.insns.at(key);
            lo = std::min(lo, in.addr);
            hi = std::max(hi, in.addr + in.size);
        }
        const uint8_t *bytes = rom.ptr(lo, hi - lo);
        if (!bytes) continue;
        int index = static_cast<int>(block_ranges.size());
        ram_block[fn.entry] = index;
        block_ranges.emplace_back(lo, hi - lo);
        blocks << "static const uint8_t ram_bytes_" << index << "[" << (hi - lo) << "] = {";
        for (uint32_t i = 0; i < hi - lo; i++) {
            blocks << (i ? "," : "") << (i % 24 ? "" : "\n    ") << static_cast<unsigned>(bytes[i]);
        }
        blocks << "\n};\n";
        stats.ram_functions++;
        stats.ram_bytes += hi - lo;
    }

    std::ostringstream table;
    table << "/* Generated by gbarecomp. Do not edit. */\n#include \"rc_functions.h\"\n\n";
    table << blocks.str() << "\n";
    if (!block_ranges.empty()) {
        table << "static const GbaRcRamBlock ram_blocks[] = {\n";
        for (size_t i = 0; i < block_ranges.size(); i++) {
            table << "    { 0x" << hex8(block_ranges[i].first) << "u, " << block_ranges[i].second << "u, ram_bytes_" << i << " },\n";
        }
        table << "};\n\n";
    }
    table << "static const GbaRcEntry entries[] = {\n";
    for (const auto &[key, fn] : owner) {
        auto b = ram_block.find(fn);
        table << "    { 0x" << hex8(key) << "u, " << function_name(fn) << ", " << (b == ram_block.end() ? -1 : b->second)
              << " },\n";
    }
    table << "};\n\n";
    table << "const uint8_t *gba_rc_embedded_rom(size_t *size);\n\n";
    table << "const GbaRcProgram gba_rc_program = {\n";
    table << "    entries,\n    " << owner.size() << "u,\n    " << rom.data.size() << "u,\n";
    table << "    " << c_string(rom.sha256) << ",\n    " << c_string(rom.title) << ",\n";
    table << "    " << (block_ranges.empty() ? "NULL" : "ram_blocks") << ",\n    " << block_ranges.size() << "u,\n";
    table << "    gba_rc_embedded_rom,\n};\n";
    if (!write_file(gen / "rc_table.c", table.str(), error)) return false;

    // The ROM itself goes into the executable: a Windows resource with MSVC
    // (and MinGW), .incbin elsewhere. Large ROMs as C arrays compile slowly.
    std::ostringstream rom_c;
    rom_c << "/* Generated by gbarecomp: the ROM compiled into this executable. */\n";
    rom_c << "#include <stddef.h>\n#include <stdint.h>\n\n";
    rom_c << "#if defined(_WIN32)\n#include <windows.h>\n\n";
    rom_c << "const uint8_t *gba_rc_embedded_rom(size_t *size)\n{\n";
    rom_c << "    HRSRC res = FindResourceA(NULL, \"GBAROM\", MAKEINTRESOURCEA(10)); /* RT_RCDATA */\n";
    rom_c << "    HGLOBAL data = res ? LoadResource(NULL, res) : NULL;\n";
    rom_c << "    if (!data) return NULL;\n";
    rom_c << "    *size = SizeofResource(NULL, res);\n";
    rom_c << "    return (const uint8_t *)LockResource(data);\n}\n\n";
    rom_c << "#else\n";
    rom_c << "#if defined(__APPLE__)\n#define GBA_RC_SYM(x) \"_\" #x\n#define GBA_RC_SECTION \".const_data\"\n";
    rom_c << "#else\n#define GBA_RC_SYM(x) #x\n#define GBA_RC_SECTION \".section .rodata\"\n#endif\n";
    rom_c << "__asm__(GBA_RC_SECTION \"\\n.balign 16\\n.globl \" GBA_RC_SYM(gba_rc_rom_start) \"\\n\" GBA_RC_SYM(gba_rc_rom_start) \":\\n\"\n";
    rom_c << "        \".incbin \\\"\" GBA_RC_ROM_FILE \"\\\"\\n.globl \" GBA_RC_SYM(gba_rc_rom_end) \"\\n\" GBA_RC_SYM(gba_rc_rom_end) \":\\n.text\\n\");\n";
    rom_c << "extern const uint8_t gba_rc_rom_start[], gba_rc_rom_end[];\n\n";
    rom_c << "const uint8_t *gba_rc_embedded_rom(size_t *size)\n{\n";
    rom_c << "    *size = (size_t)(gba_rc_rom_end - gba_rc_rom_start);\n";
    rom_c << "    return gba_rc_rom_start;\n}\n#endif\n";
    if (!write_file(gen / "rc_rom.c", rom_c.str(), error)) return false;

    fs::create_directories(out / "rom", ec);
    {
        std::ofstream rom_out(out / "rom" / (options.name + ".gba"), std::ios::binary);
        rom_out.write(reinterpret_cast<const char *>(rom.data.data()), static_cast<std::streamsize>(rom.data.size()));
        if (!rom_out) {
            error = "cannot write the ROM copy into " + (out / "rom").string();
            return false;
        }
    }

    std::ostringstream main_c;
    const std::string rom_file = "NULL"; /* the ROM is embedded; a path on the command line overrides it */
    main_c << "/* Generated by gbarecomp. */\n#include \"gba_recomp.h\"\n#include \"gba_runner.h\"\n";
    main_c << "#ifdef GBA_HAVE_SDL\n#include \"gba_sdl.h\"\n#endif\n\n";
    main_c << "extern const GbaRcProgram gba_rc_program;\n\n";
    main_c << "int main(int argc, char **argv)\n{\n";
    main_c << "#ifdef GBA_HAVE_SDL\n";
    main_c << "    if (!gba_sdl_wants_headless(argc, argv)) return gba_sdl_main(argc, argv, &gba_rc_program, " << rom_file << ");\n";
    main_c << "#endif\n";
    main_c << "    return gba_runner_main(argc, argv, &gba_rc_program, " << rom_file << ");\n}\n";
    if (!write_file(out / "main.c", main_c.str(), error)) return false;

    std::ostringstream cmake;
    cmake << "# Generated by gbarecomp.\n";
    cmake << "cmake_minimum_required(VERSION 3.16)\n";
    cmake << "# Release unless told otherwise. Before project(): with MSVC and a\n";
    cmake << "# single-configuration generator, project() itself makes it Debug.\n";
    cmake << "if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n";
    cmake << "    set(CMAKE_BUILD_TYPE Release CACHE STRING \"Build type\" FORCE)\nendif()\n";
    cmake << "project(" << options.name << " C)\n\n";
    cmake << "set(CMAKE_C_STANDARD 11)\nset(CMAKE_C_STANDARD_REQUIRED ON)\n\n";
    cmake << "# Runtime snapshot copied in by gbarecomp. Test tooling may link a\n";
    cmake << "# prebuilt runtime instead (GBART_PREBUILT_LIB + GBART_PREBUILT_INCLUDE).\n";
    cmake << "set(GBART_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/runtime-gba\")\n";
    cmake << "if(GBART_PREBUILT_LIB)\n";
    cmake << "    add_library(gbaruntime STATIC IMPORTED)\n";
    cmake << "    set_target_properties(gbaruntime PROPERTIES IMPORTED_LOCATION \"${GBART_PREBUILT_LIB}\"\n";
    cmake << "        INTERFACE_INCLUDE_DIRECTORIES \"${GBART_PREBUILT_INCLUDE}\")\n";
    cmake << "else()\n";
    cmake << "    file(GLOB GBART_SOURCES \"${GBART_DIR}/src/*.c\")\n";
    cmake << "    add_library(gbaruntime STATIC ${GBART_SOURCES})\n";
    cmake << "    target_include_directories(gbaruntime PUBLIC \"${GBART_DIR}/include\")\n";
    cmake << "endif()\n\n";
    cmake << "file(GLOB RC_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c\")\n";
    cmake << "add_executable(" << options.name << " main.c ${RC_SOURCES})\n";
    cmake << "target_include_directories(" << options.name << " PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/generated\")\n";
    cmake << "target_link_libraries(" << options.name << " PRIVATE gbaruntime)\n";
    cmake << "if(NOT WIN32)\n    target_link_libraries(" << options.name << " PRIVATE m)\nendif()\n";
    cmake << "if(WIN32)\n    target_link_libraries(" << options.name << " PRIVATE ws2_32) # link cable (gba_net.c)\nendif()\n";
    cmake << "if(MSVC)\n    target_compile_options(" << options.name << " PRIVATE /bigobj)\nendif()\n\n";
    cmake << "# The ROM is compiled into the executable (generated/rc_rom.c).\n";
    cmake << "set(GBA_RC_ROM \"${CMAKE_CURRENT_SOURCE_DIR}/rom/" << options.name << ".gba\")\n";
    cmake << "if(WIN32)\n";
    cmake << "    enable_language(RC)\n";
    cmake << "    file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\" \"GBAROM RCDATA \\\"${GBA_RC_ROM}\\\"\\n\")\n";
    cmake << "    target_sources(" << options.name << " PRIVATE \"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\")\n";
    cmake << "    set_source_files_properties(\"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\" PROPERTIES OBJECT_DEPENDS \"${GBA_RC_ROM}\")\n";
    cmake << "else()\n";
    cmake << "    set_source_files_properties(generated/rc_rom.c PROPERTIES\n";
    cmake << "        COMPILE_DEFINITIONS \"GBA_RC_ROM_FILE=\\\"${GBA_RC_ROM}\\\"\" OBJECT_DEPENDS \"${GBA_RC_ROM}\")\n";
    cmake << "endif()\n\n";
    cmake << "# Window, input, audio and the settings menu (Dear ImGui) when SDL2 is\n";
    cmake << "# available; headless otherwise.\n";
    cmake << "find_package(SDL2 QUIET)\n";
    cmake << "if(TARGET SDL2::SDL2)\n";
    cmake << "    enable_language(CXX)\n";
    cmake << "    set(GBA_IMGUI_DIR \"${GBART_DIR}/vendor/imgui\")\n";
    cmake << "    target_sources(" << options.name << " PRIVATE \"${GBART_DIR}/platform/gba_sdl.cpp\"\n";
    cmake << "        \"${GBART_DIR}/platform/gba_input.cpp\"\n";
    cmake << "        \"${GBA_IMGUI_DIR}/imgui.cpp\" \"${GBA_IMGUI_DIR}/imgui_draw.cpp\"\n";
    cmake << "        \"${GBA_IMGUI_DIR}/imgui_tables.cpp\" \"${GBA_IMGUI_DIR}/imgui_widgets.cpp\"\n";
    cmake << "        \"${GBA_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp\"\n";
    cmake << "        \"${GBA_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp\")\n";
    cmake << "    target_include_directories(" << options.name << " PRIVATE \"${GBART_DIR}/platform\" \"${GBA_IMGUI_DIR}\")\n";
    cmake << "    set_target_properties(" << options.name << " PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)\n";
    cmake << "    target_compile_definitions(" << options.name << " PRIVATE GBA_HAVE_SDL)\n";
    cmake << "    target_link_libraries(" << options.name << " PRIVATE SDL2::SDL2)\n";
    cmake << "    get_target_property(_sdl2_type SDL2::SDL2 TYPE)\n";
    cmake << "    if(WIN32 AND _sdl2_type STREQUAL \"SHARED_LIBRARY\")\n";
    cmake << "        add_custom_command(TARGET " << options.name << " POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different\n";
    cmake << "            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:" << options.name << ">)\n";
    cmake << "    endif()\n";
    cmake << "    # macOS .app bundle (off unless NATIVE_CONSOLE_MACOS_BUNDLE=ON)\n";
    cmake << "    include(\"${GBART_DIR}/cmake/NativeConsoleMacBundle.cmake\")\n";
    cmake << "    native_console_macos_bundle(" << options.name << ")\n";
    cmake << "endif()\n";
    if (!write_file(out / "CMakeLists.txt", cmake.str(), error)) return false;

    // Snapshot the runtime so the project builds on its own.
    fs::path rt_src(options.runtime_dir);
    fs::path rt_dst = out / "runtime-gba";
    for (const char *sub : { "include", "src", "platform", "cmake" }) {
        fs::create_directories(rt_dst / sub, ec);
        fs::copy(rt_src / sub, rt_dst / sub, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy runtime from " + (rt_src / sub).string() + ": " + ec.message();
            return false;
        }
    }
    // The settings menu's Dear ImGui: a runtime that carries its own copy,
    // else the one the Game Boy runtime next to it vendors.
    fs::path imgui_src = rt_src / "vendor" / "imgui";
    if (!fs::exists(imgui_src / "imgui.h")) {
        fs::path rt_dir = rt_src.filename().empty() ? rt_src.parent_path() : rt_src; // "runtime-gba/"
        imgui_src = rt_dir.parent_path() / "runtime" / "vendor" / "imgui";
    }
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

} // namespace gbarecomp
