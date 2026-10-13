// The C project: one C function per analyzed function, the entry table and
// the bytes each function was compiled from, the ROM, and a snapshot of the
// runtime.
//
// Generated code calls the interpreter's handlers (n64_cpu_impl.h) with
// constant instruction words and sequences the delay slots itself. Cycles
// and the instruction count are added in batches, flushed before anything
// that can observe them: an access to memory, a system control
// instruction, an instruction that can raise an exception, a label, and an
// exit. It returns to the run loop at the same sync points as the
// interpreter (n64_cpu_decode.h), after an exception, at calls, returns and
// indirect jumps, and when a store changed code that was compiled.
#include "emitter.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace n64recomp {

namespace {

std::string hex8(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

// The 64-bit PC for a 32-bit address (sign-extended, as the CPU holds it).
std::string pc64(uint32_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%016llXull", (unsigned long long)(uint64_t)(int64_t)(int32_t)v);
    return buf;
}

std::string label(uint32_t a) { return "L_" + hex8(a); }

const char *cond_name(N64Op op)
{
    switch (op) {
    case N64_OP_beq: case N64_OP_beql: return "n64_cond_beq(c, ";
    case N64_OP_bne: case N64_OP_bnel: return "n64_cond_bne(c, ";
    case N64_OP_blez: case N64_OP_blezl: return "n64_cond_blez(c, ";
    case N64_OP_bgtz: case N64_OP_bgtzl: return "n64_cond_bgtz(c, ";
    case N64_OP_bltz: case N64_OP_bltzl: case N64_OP_bltzal: case N64_OP_bltzall: return "n64_cond_bltz(c, ";
    case N64_OP_bgez: case N64_OP_bgezl: case N64_OP_bgezal: case N64_OP_bgezall: return "n64_cond_bgez(c, ";
    default: return nullptr;
    }
}

class FunctionEmitter {
public:
    FunctionEmitter(const Function &fn, std::string &out, bool inline_handlers)
        : fn_(fn), out_(out), inline_handlers_(inline_handlers)
    {
    }

    void emit(const std::string &name)
    {
        plan();
        out_ += "void " + name + "(N64Cpu *c, uint32_t entry)\n{\n";
        out_ += "    N64Machine *m = N64_MACHINE(c);\n    const uint64_t gen = m->rc_invalidations;\n";
        out_ += "    (void)m;\n    (void)gen;\n    switch (entry) {\n";
        for (uint32_t a : fn_.labels) out_ += "    case 0x" + hex8(a) + "u: goto " + label(a) + ";\n";
        out_ += "    default: return;\n    }\n";
        for (size_t i = 0; i < order_.size(); i++) {
            const uint32_t following = i + 1 < order_.size() ? order_[i + 1] : 0xFFFFFFFFu;
            emit_insn(fn_.insns.at(order_[i]), following);
        }
        out_ += "}\n\n";
    }

private:
    bool compiled(uint32_t a) const
    {
        auto it = fn_.insns.find(a);
        return it != fn_.insns.end() && !it->second.bad;
    }
    bool is_transfer(const Insn &in) const { return (in.flags & (N64_F_BRANCH | N64_F_JUMP | N64_F_JUMPR)) != 0; }

    // Which instructions are emitted on their own, in order, and which need
    // a C label (dispatcher entries and goto targets).
    void plan()
    {
        std::set<uint32_t> slots, targets;
        for (const auto &[a, in] : fn_.insns) {
            if (in.bad || !is_transfer(in)) continue;
            slots.insert(a + 4u);
            if (in.has_target) targets.insert(in.target);
        }
        for (const auto &[a, in] : fn_.insns) {
            if (in.bad) continue;
            if (slots.count(a) && !fn_.labels.count(a) && !targets.count(a)) continue;
            order_.push_back(a);
        }
        for (size_t i = 0; i < order_.size(); i++) {
            const Insn &in = fn_.insns.at(order_[i]);
            const uint32_t following = i + 1 < order_.size() ? order_[i + 1] : 0xFFFFFFFFu;
            const uint32_t next = is_transfer(in) ? in.vaddr + 8u : in.vaddr + 4u;
            if (compiled(next) && next != following) needed_.insert(next);
            if (in.has_target && compiled(in.target)) needed_.insert(in.target);
        }
        for (uint32_t a : fn_.labels) needed_.insert(a);
    }

    void flush()
    {
        if (!cycles_ && !insns_) return;
        out_ += "    c->cycles += " + std::to_string(cycles_) + "u;\n";
        out_ += "    c->instructions += " + std::to_string(insns_) + "u;\n";
        cycles_ = insns_ = 0;
    }

    void exit_to(const std::string &pc_expr)
    {
        flush();
        out_ += "    c->pc = " + pc_expr + ";\n    return;\n";
    }

    void continue_to(uint32_t a, uint32_t following)
    {
        if (!compiled(a)) {
            exit_to(pc64(a));
            return;
        }
        flush();
        if (a != following) out_ += "    goto " + label(a) + ";\n";
    }

    // Small handlers are inlined (n64_op_*); the instruction word is a
    // constant, so they fold to a few host instructions. The large ones
    // are called (n64_ool_*): memory, the FPU, and whatever can raise an
    // exception or touches time.
    std::string call(const Insn &in) const
    {
        const bool large =
            (in.flags & (N64_F_MEM | N64_F_FPU | N64_F_FAULT | N64_F_SYNC | N64_F_TIME | N64_F_EXIT)) != 0;
        return std::string(large && !inline_handlers_ ? "n64_ool_" : "n64_op_") + n64_op_names[in.op] + "(c, 0x" +
               hex8(in.word) + "u);";
    }

    // A branch to itself over an empty delay slot (n64_is_idle_loop).
    bool idle_loop(const Insn &in) const
    {
        if (!is_transfer(in) || !in.has_target || in.target != in.vaddr) return false;
        auto slot = fn_.insns.find(in.vaddr + 4u);
        return slot != fn_.insns.end() &&
               n64_is_idle_loop((uint64_t)(int64_t)(int32_t)in.vaddr, in.word, slot->second.word);
    }

    // A delay slot, run as part of its branch. `cont` is where execution
    // goes after it.
    void emit_slot(const Insn &s, const std::string &cont)
    {
        cycles_ += 1u + n64_op_extra_cycles(s.op);
        insns_ += 1u;
        const uint16_t f = s.flags;
        if (f & (N64_F_FAULT | N64_F_MEM | N64_F_SYNC | N64_F_TIME)) flush();
        if (f & N64_F_FAULT) out_ += "    c->pc = " + pc64(s.vaddr) + ";\n    c->in_delay = 1;\n";
        out_ += "    " + call(s) + "\n";
        if (f & N64_F_FAULT) {
            out_ += "    c->in_delay = 0;\n";
            out_ += "    if (c->exc) {\n        c->exc = 0;\n        return;\n    }\n";
        }
        if (f & N64_F_STORE) out_ += "    if (m->rc_invalidations != gen) {\n        c->pc = " + cont + ";\n        return;\n    }\n";
    }

    void yield_to(const std::string &pc_expr)
    {
        out_ += "    if (N64_RC_YIELD(c)) {\n        c->pc = " + pc_expr + ";\n        return;\n    }\n";
    }

    void emit_transfer(const Insn &in, uint32_t following)
    {
        const Insn &slot = fn_.insns.at(in.vaddr + 4u);
        const uint32_t a = in.vaddr;
        const char *word = nullptr;
        char wbuf[16];
        std::snprintf(wbuf, sizeof wbuf, "0x%08Xu", in.word);
        word = wbuf;
        cycles_ += 1u;
        insns_ += 1u;
        if (in.flags & N64_F_BRANCH) {
            std::string cond;
            if (const char *n = cond_name(in.op)) cond = std::string(n) + word + ")";
            else cond = std::string("n64_cond_bc1(c, ") + ((in.op == N64_OP_bc1t || in.op == N64_OP_bc1tl) ? "true" : "false") + ")";
            if (in.flags & N64_F_FAULT) {
                flush();
                out_ += "    c->pc = " + pc64(a) + ";\n";
            }
            out_ += "    {\n    const bool taken = " + cond + ";\n";
            if (in.flags & N64_F_FAULT) out_ += "    if (c->exc) {\n        c->exc = 0;\n        return;\n    }\n";
            if (in.flags & N64_F_LINK) out_ += "    n64_link(c, " + std::string(word) + ", " + pc64(a) + ");\n";
            out_ += "    if (taken) {\n";
            {
                const uint32_t saved_c = cycles_, saved_i = insns_;
                emit_slot(slot, pc64(in.target));
                flush();
                yield_to(pc64(in.target));
                if (compiled(in.target)) out_ += "    goto " + label(in.target) + ";\n";
                else out_ += "    c->pc = " + pc64(in.target) + ";\n    return;\n";
                cycles_ = saved_c;
                insns_ = saved_i;
            }
            out_ += "    }\n";
            if (in.flags & N64_F_LIKELY) cycles_ += 1u; /* the nullified delay slot */
            else emit_slot(slot, pc64(a + 8u));
            flush();
            yield_to(pc64(a + 8u));
            out_ += "    }\n";
            continue_to(a + 8u, following);
            return;
        }
        if (in.flags & N64_F_JUMP) {
            if (in.flags & N64_F_LINK) out_ += "    n64_link(c, " + std::string(word) + ", " + pc64(a) + ");\n";
            emit_slot(slot, pc64(in.target));
            if ((in.flags & N64_F_LINK) || !compiled(in.target)) {
                exit_to(pc64(in.target));
                return;
            }
            flush();
            yield_to(pc64(in.target));
            continue_to(in.target, following);
            return;
        }
        // JR, JALR: the target is read before the link and the delay slot.
        out_ += "    {\n    const uint64_t target = c->gpr[" + std::to_string(N64_RS(in.word)) + "];\n";
        if (in.flags & N64_F_LINK) out_ += "    n64_link(c, " + std::string(word) + ", " + pc64(a) + ");\n";
        emit_slot(slot, "target");
        flush();
        out_ += "    c->pc = target;\n    return;\n    }\n";
    }

    void emit_insn(const Insn &in, uint32_t following)
    {
        if (needed_.count(in.vaddr)) {
            flush();
            out_ += label(in.vaddr) + ":\n";
        }
        out_ += "    /* " + hex8(in.vaddr) + ": " + n64_op_names[in.op] + " */\n";
        if (is_transfer(in)) {
            if (idle_loop(in)) {
                flush();
                out_ += "    n64_rc_idle(c);\n";
            }
            emit_transfer(in, following);
            return;
        }
        cycles_ += 1u + n64_op_extra_cycles(in.op);
        insns_ += 1u;
        const uint16_t f = in.flags;
        const uint32_t next = in.vaddr + 4u;
        if (f & (N64_F_FAULT | N64_F_MEM | N64_F_SYNC | N64_F_TIME | N64_F_EXIT)) flush();
        if (f & N64_F_FAULT) out_ += "    c->pc = " + pc64(in.vaddr) + ";\n";
        out_ += "    " + call(in) + "\n";
        if (in.op == N64_OP_eret) {
            out_ += "    if (c->exc) {\n        c->exc = 0;\n        return;\n    }\n";
            out_ += "    c->pc = c->next_pc;\n    return;\n";
            return;
        }
        if (f & N64_F_EXIT) {
            out_ += "    c->exc = 0;\n    return;\n";
            return;
        }
        if (f & N64_F_FAULT) out_ += "    if (c->exc) {\n        c->exc = 0;\n        return;\n    }\n";
        if (in.op == N64_OP_tlbwi || in.op == N64_OP_tlbwr) {
            exit_to(pc64(next)); // the mapping of the code may have changed
            return;
        }
        if (f & N64_F_STORE) out_ += "    if (m->rc_invalidations != gen) {\n        c->pc = " + pc64(next) + ";\n        return;\n    }\n";
        if (f & N64_F_SYNC) yield_to(pc64(next));
        continue_to(next, following);
    }

    const Function &fn_;
    std::string &out_;
    std::vector<uint32_t> order_;
    std::set<uint32_t> needed_;
    uint32_t cycles_ = 0, insns_ = 0;
    bool inline_handlers_;
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

std::string function_name(const Function &fn, size_t index)
{
    return "rc_" + hex8(fn.entry) + "_" + std::to_string(index);
}

void emit_function(const Function &fn, size_t index, std::string &out, bool inline_handlers)
{
    FunctionEmitter(fn, out, inline_handlers).emit(function_name(fn, index));
}

bool emit_project(const Image &image, const Analyzer &an, const EmitOptions &options, EmitStats &stats,
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

    std::string header = "/* Generated by n64recomp. Do not edit. */\n#pragma once\n";
    header += "#define N64_CONSTANT_INSNS 1 /* every handler gets a constant instruction word */\n";
    header += "#include \"n64_recomp.h\"\n\n";
    for (size_t i = 0; i < an.functions.size(); i++)
        header += "void " + function_name(an.functions[i], i) + "(N64Cpu *c, uint32_t entry);\n";
    if (!write_file(gen / "rc_functions.h", header, error)) return false;

    size_t file_index = 0;
    for (size_t first = 0; first < an.functions.size(); first += options.functions_per_file) {
        std::string src = "/* Generated by n64recomp from \"" + std::string(image.cart.title) + "\". Do not edit. */\n";
        src += "#include \"rc_functions.h\"\n\n";
        size_t last = std::min(first + options.functions_per_file, an.functions.size());
        for (size_t i = first; i < last; i++) {
            emit_function(an.functions[i], i, src, options.inline_handlers);
            stats.instructions += an.functions[i].insns.size();
        }
        char name[32];
        std::snprintf(name, sizeof name, "rc_%04zu.c", file_index++);
        if (!write_file(gen / name, src, error)) return false;
    }
    stats.files = file_index;
    stats.functions = an.functions.size();

    // The bytes each function was compiled from, and the entry table.
    std::ostringstream table;
    table << "/* Generated by n64recomp. Do not edit. */\n#include \"rc_functions.h\"\n\n";
    std::vector<std::pair<uint32_t, uint32_t>> blocks; // paddr, size
    for (size_t f = 0; f < an.functions.size(); f++) {
        const Function &fn = an.functions[f];
        const View &v = image.views[fn.view];
        const uint32_t pa = fn.lo - fn.vp, size = fn.hi - fn.lo;
        blocks.emplace_back(pa, size);
        table << "static const uint8_t block_" << f << "[" << size << "] = {";
        for (uint32_t i = 0; i < size; i++) table << (i ? "," : "") << (i % 24 ? "" : "\n    ") << unsigned(v.bytes[pa - v.paddr + i]);
        table << "\n};\n";
        stats.block_bytes += size;
    }
    table << "\nstatic const N64RcBlock blocks[] = {\n";
    for (size_t f = 0; f < blocks.size(); f++)
        table << "    { 0x" << hex8(blocks[f].first) << "u, " << blocks[f].second << "u, block_" << f << " },\n";
    if (blocks.empty()) table << "    { 0u, 0u, NULL },\n";
    table << "};\n\nstatic const N64RcEntry entries[] = {\n";
    for (size_t f = 0; f < an.functions.size(); f++) {
        for (uint32_t a : an.functions[f].labels) {
            table << "    { 0x" << hex8(a) << "u, " << function_name(an.functions[f], f) << ", " << f << " },\n";
            stats.entries++;
        }
    }
    if (!stats.entries) table << "    { 0u, NULL, 0 },\n";
    table << "};\n\n";
    table << "const uint8_t *n64_rc_embedded_rom(size_t *size);\n\n";
    table << "const N64RcProgram n64_rc_program = {\n";
    table << "    entries,\n    " << stats.entries << "u,\n    blocks,\n    " << blocks.size() << "u,\n";
    table << "    " << image.rom.size() << "u,\n    " << c_string(image.sha256) << ",\n";
    table << "    " << c_string(image.cart.title) << ",\n    n64_rc_embedded_rom,\n};\n";
    if (!write_file(gen / "rc_table.c", table.str(), error)) return false;

    // The ROM goes into the executable: a Windows resource with MSVC, .incbin
    // elsewhere. A multi-megabyte C array compiles slowly.
    std::ostringstream rom_c;
    rom_c << "/* Generated by n64recomp: the ROM compiled into this executable. */\n";
    rom_c << "#include <stddef.h>\n#include <stdint.h>\n\n";
    rom_c << "#if defined(_WIN32)\n#include <windows.h>\n\n";
    rom_c << "const uint8_t *n64_rc_embedded_rom(size_t *size)\n{\n";
    rom_c << "    HRSRC res = FindResourceA(NULL, \"N64ROM\", MAKEINTRESOURCEA(10)); /* RT_RCDATA */\n";
    rom_c << "    HGLOBAL data = res ? LoadResource(NULL, res) : NULL;\n";
    rom_c << "    if (!data) return NULL;\n";
    rom_c << "    *size = SizeofResource(NULL, res);\n";
    rom_c << "    return (const uint8_t *)LockResource(data);\n}\n\n";
    rom_c << "#else\n";
    rom_c << "#if defined(__APPLE__)\n#define N64_RC_SYM(x) \"_\" #x\n#define N64_RC_SECTION \".const_data\"\n";
    rom_c << "#else\n#define N64_RC_SYM(x) #x\n#define N64_RC_SECTION \".section .rodata\"\n#endif\n";
    rom_c << "__asm__(N64_RC_SECTION \"\\n.balign 16\\n.globl \" N64_RC_SYM(n64_rc_rom_start) \"\\n\" N64_RC_SYM(n64_rc_rom_start) \":\\n\"\n";
    rom_c << "        \".incbin \\\"\" N64_RC_ROM_FILE \"\\\"\\n.globl \" N64_RC_SYM(n64_rc_rom_end) \"\\n\" N64_RC_SYM(n64_rc_rom_end) \":\\n.text\\n\");\n";
    rom_c << "extern const uint8_t n64_rc_rom_start[], n64_rc_rom_end[];\n\n";
    rom_c << "const uint8_t *n64_rc_embedded_rom(size_t *size)\n{\n";
    rom_c << "    *size = (size_t)(n64_rc_rom_end - n64_rc_rom_start);\n";
    rom_c << "    return n64_rc_rom_start;\n}\n#endif\n";
    if (!write_file(gen / "rc_rom.c", rom_c.str(), error)) return false;

    fs::create_directories(out / "rom", ec);
    {
        std::ofstream rom_out(out / "rom" / (options.name + ".z64"), std::ios::binary);
        rom_out.write(reinterpret_cast<const char *>(image.rom.data()), static_cast<std::streamsize>(image.rom.size()));
        if (!rom_out) {
            error = "cannot write the ROM copy into " + (out / "rom").string();
            return false;
        }
    }

    std::ostringstream main_c;
    main_c << "/* Generated by n64recomp. */\n#include \"n64_recomp.h\"\n#include \"n64_runner.h\"\n";
    main_c << "#ifdef N64_HAVE_SDL\n#include \"n64_sdl.h\"\n#endif\n\n";
    main_c << "extern const N64RcProgram n64_rc_program;\n\n";
    main_c << "int main(int argc, char **argv)\n{\n";
    main_c << "#ifdef N64_HAVE_SDL\n";
    main_c << "    if (!n64_sdl_wants_headless(argc, argv)) return n64_sdl_main(argc, argv, &n64_rc_program, NULL);\n";
    main_c << "#endif\n";
    main_c << "    return n64_runner_main(argc, argv, &n64_rc_program, NULL);\n}\n";
    if (!write_file(out / "main.c", main_c.str(), error)) return false;

    const std::string &n = options.name;
    std::ostringstream cmake;
    cmake << "# Generated by n64recomp.\n";
    cmake << "cmake_minimum_required(VERSION 3.16)\n";
    cmake << "project(" << n << " C)\n\n";
    cmake << "set(CMAKE_C_STANDARD 11)\nset(CMAKE_C_STANDARD_REQUIRED ON)\n";
    cmake << "if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n";
    cmake << "    set(CMAKE_BUILD_TYPE Release CACHE STRING \"Build type\" FORCE)\nendif()\n\n";
    cmake << "# The FPU reads the host's exception flags straight after each operation:\n";
    cmake << "# no fused multiply-adds, no motion across the flag reads.\n";
    cmake << "if(MSVC)\n    set(N64_STRICT_FP /fp:strict)\nelse()\n    set(N64_STRICT_FP -ffp-contract=off -frounding-math)\nendif()\n\n";
    cmake << "# The RDP's rasteriser shares its work with helper threads (Win32\n";
    cmake << "# threads, pthreads elsewhere).\n";
    cmake << "find_package(Threads REQUIRED)\n\n";
    cmake << "# Runtime snapshot copied in by n64recomp. Test tooling may link a\n";
    cmake << "# prebuilt runtime instead (N64RT_PREBUILT_LIBS + N64RT_PREBUILT_INCLUDE).\n";
    cmake << "set(N64RT_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/runtime-n64\")\n";
    cmake << "if(N64RT_PREBUILT_LIBS)\n";
    cmake << "    add_library(n64runtime INTERFACE)\n";
    cmake << "    target_link_libraries(n64runtime INTERFACE ${N64RT_PREBUILT_LIBS} Threads::Threads)\n";
    cmake << "    if(NOT WIN32)\n        target_link_libraries(n64runtime INTERFACE m)\n    endif()\n";
    cmake << "    target_include_directories(n64runtime INTERFACE \"${N64RT_PREBUILT_INCLUDE}\")\n";
    cmake << "else()\n";
    cmake << "    file(GLOB N64RT_SOURCES \"${N64RT_DIR}/src/*.c\")\n";
    cmake << "    add_library(n64runtime STATIC ${N64RT_SOURCES})\n";
    cmake << "    target_include_directories(n64runtime PUBLIC \"${N64RT_DIR}/include\")\n";
    cmake << "    target_compile_options(n64runtime PRIVATE ${N64_STRICT_FP})\n";
    cmake << "    target_link_libraries(n64runtime PUBLIC Threads::Threads)\n";
    cmake << "    if(NOT WIN32)\n        target_link_libraries(n64runtime PUBLIC m)\n    endif()\n";
    cmake << "endif()\n\n";
    cmake << "file(GLOB RC_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c\")\n";
    cmake << "add_executable(" << n << " main.c ${RC_SOURCES})\n";
    cmake << "target_include_directories(" << n << " PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/generated\")\n";
    cmake << "target_compile_options(" << n << " PRIVATE ${N64_STRICT_FP})\n";
    cmake << "target_link_libraries(" << n << " PRIVATE n64runtime)\n";
    cmake << "if(NOT WIN32)\n    target_link_libraries(" << n << " PRIVATE m)\nendif()\n";
    cmake << "if(MSVC)\n    target_compile_options(" << n << " PRIVATE /bigobj)\nendif()\n\n";
    cmake << "# The ROM is compiled into the executable (generated/rc_rom.c).\n";
    cmake << "set(N64_RC_ROM \"${CMAKE_CURRENT_SOURCE_DIR}/rom/" << n << ".z64\")\n";
    cmake << "if(WIN32)\n";
    cmake << "    enable_language(RC)\n";
    cmake << "    file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\" \"N64ROM RCDATA \\\"${N64_RC_ROM}\\\"\\n\")\n";
    cmake << "    target_sources(" << n << " PRIVATE \"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\")\n";
    cmake << "    set_source_files_properties(\"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\" PROPERTIES OBJECT_DEPENDS \"${N64_RC_ROM}\")\n";
    cmake << "else()\n";
    cmake << "    set_source_files_properties(generated/rc_rom.c PROPERTIES\n";
    cmake << "        COMPILE_DEFINITIONS \"N64_RC_ROM_FILE=\\\"${N64_RC_ROM}\\\"\" OBJECT_DEPENDS \"${N64_RC_ROM}\")\n";
    cmake << "endif()\n\n";
    cmake << "# Window, input, audio and the settings menu (Dear ImGui) when SDL2 is\n";
    cmake << "# available; headless otherwise.\n";
    cmake << "find_package(SDL2 QUIET)\n";
    cmake << "if(TARGET SDL2::SDL2 AND EXISTS \"${N64RT_DIR}/platform/n64_sdl.cpp\")\n";
    cmake << "    enable_language(CXX)\n";
    cmake << "    set(N64_IMGUI_DIR \"${N64RT_DIR}/vendor/imgui\")\n";
    cmake << "    target_sources(" << n << " PRIVATE \"${N64RT_DIR}/platform/n64_sdl.cpp\"\n";
    cmake << "        \"${N64RT_DIR}/platform/n64_input.cpp\"\n";
    cmake << "        \"${N64_IMGUI_DIR}/imgui.cpp\" \"${N64_IMGUI_DIR}/imgui_draw.cpp\"\n";
    cmake << "        \"${N64_IMGUI_DIR}/imgui_tables.cpp\" \"${N64_IMGUI_DIR}/imgui_widgets.cpp\"\n";
    cmake << "        \"${N64_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp\"\n";
    cmake << "        \"${N64_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp\")\n";
    cmake << "    target_include_directories(" << n << " PRIVATE \"${N64RT_DIR}/platform\" \"${N64_IMGUI_DIR}\")\n";
    cmake << "    set_target_properties(" << n << " PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)\n";
    cmake << "    target_compile_definitions(" << n << " PRIVATE N64_HAVE_SDL)\n";
    cmake << "    target_link_libraries(" << n << " PRIVATE SDL2::SDL2)\n";
    cmake << "    get_target_property(_sdl2_type SDL2::SDL2 TYPE)\n";
    cmake << "    if(WIN32 AND _sdl2_type STREQUAL \"SHARED_LIBRARY\")\n";
    cmake << "        add_custom_command(TARGET " << n << " POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different\n";
    cmake << "            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:" << n << ">)\n";
    cmake << "    endif()\n";
    cmake << "    # macOS .app bundle (off unless NATIVE_CONSOLE_MACOS_BUNDLE=ON)\n";
    cmake << "    include(\"${N64RT_DIR}/cmake/NativeConsoleMacBundle.cmake\")\n";
    cmake << "    native_console_macos_bundle(" << n << ")\n";
    cmake << "endif()\n";
    if (!write_file(out / "CMakeLists.txt", cmake.str(), error)) return false;

    // Snapshot the runtime so the project builds on its own.
    fs::path rt_src(options.runtime_dir);
    fs::path rt_dst = out / "runtime-n64";
    for (const char *sub : { "include", "src", "platform", "cmake" }) {
        if (!fs::exists(rt_src / sub)) continue;
        fs::create_directories(rt_dst / sub, ec);
        fs::copy(rt_src / sub, rt_dst / sub, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy runtime from " + (rt_src / sub).string() + ": " + ec.message();
            return false;
        }
    }
    fs::path imgui_src = rt_src / "vendor" / "imgui";
    if (!fs::exists(imgui_src / "imgui.h")) imgui_src = rt_src.parent_path() / "runtime" / "vendor" / "imgui";
    if (fs::exists(imgui_src / "imgui.h")) {
        fs::create_directories(rt_dst / "vendor" / "imgui", ec);
        fs::copy(imgui_src, rt_dst / "vendor" / "imgui", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy Dear ImGui from " + imgui_src.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

} // namespace n64recomp
