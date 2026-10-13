#include "emitter.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

extern "C" {
#include "nds_cpu_decode.h"
#include "nds_rc_hash.h"
}

namespace fs = std::filesystem;

namespace ndsrecomp {

namespace {

std::string hex8(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

std::string hex16(uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string label(uint32_t key) { return "L_" + hex8(key_addr(key)); }

std::string function_name(const CodeImage &image, uint32_t entry)
{
    return "rc_" + image.name + "_" + hex8(key_addr(entry)) + (key_thumb(entry) ? "_t" : "_a");
}

// The handler for an ARM class, without its processor prefix; nullptr for
// the classes that take no opcode or do nothing.
const char *arm_handler(NdsArmClass cls)
{
    switch (cls) {
    case NDS_ARM_BX: return "arm_bx";
    case NDS_ARM_SWP: return "arm_swp";
    case NDS_ARM_MUL: return "arm_mul";
    case NDS_ARM_MULL: return "arm_mull";
    case NDS_ARM_HALF: return "arm_half";
    case NDS_ARM_MRS: return "arm_mrs";
    case NDS_ARM_MSR: return "arm_msr";
    case NDS_ARM_DP: return "arm_dp";
    case NDS_ARM_SDT: return "arm_sdt";
    case NDS_ARM_BLOCK: return "arm_block";
    case NDS_ARM_B: return "arm_b";
    case NDS_ARM_BLX_REG: return "arm_blx_reg";
    case NDS_ARM_BLX_IMM: return "arm_blx_imm";
    case NDS_ARM_CLZ: return "arm_clz";
    case NDS_ARM_QARITH: return "arm_qarith";
    case NDS_ARM_DSP_MUL: return "arm_dsp_mul";
    case NDS_ARM_DOUBLE: return "arm_double";
    case NDS_ARM_CP_REG: return "arm_cp_reg";
    default: return nullptr; // SWI, BKPT, undefined, PLD
    }
}

const char *thumb_handler(NdsThumbClass cls)
{
    switch (cls) {
    case NDS_THUMB_SHIFT: return "thumb_shift_imm";
    case NDS_THUMB_ADD_SUB: return "thumb_add_sub";
    case NDS_THUMB_IMM8: return "thumb_imm8";
    case NDS_THUMB_ALU: return "thumb_alu";
    case NDS_THUMB_HI_REG: return "thumb_hi_reg";
    case NDS_THUMB_LDR_PC: return "thumb_ldr_pc";
    case NDS_THUMB_LDST_REG: return "thumb_ldst_reg";
    case NDS_THUMB_LDST_SIGN: return "thumb_ldst_sign";
    case NDS_THUMB_LDST_IMM: return "thumb_ldst_imm";
    case NDS_THUMB_LDST_HALF: return "thumb_ldst_half";
    case NDS_THUMB_LDST_SP: return "thumb_ldst_sp";
    case NDS_THUMB_ADD_PC_SP: return "thumb_add_pc_sp";
    case NDS_THUMB_ADD_SP: return "thumb_add_sp";
    case NDS_THUMB_PUSH_POP: return "thumb_push_pop";
    case NDS_THUMB_LDM_STM: return "thumb_ldm_stm";
    case NDS_THUMB_BCC: return "thumb_bcc";
    case NDS_THUMB_B: return "thumb_b";
    case NDS_THUMB_BL: return "thumb_bl";
    default: return nullptr; // SWI, BKPT, undefined
    }
}

// Does control fall through to the next instruction?
bool falls_through(const Insn &in)
{
    switch (in.flow) {
    case Flow::Next: case Flow::CondBranch: case Flow::Call: case Flow::CallIndirect:
        return !in.exits_after;
    default:
        // A branch, return, indirect jump, table jump or trap with an ARM
        // condition falls through when the condition fails.
        return in.conditional;
    }
}

// A branch whose target is in the instruction itself. Only these may jump
// straight to a label: any other write to R15 goes where a register says,
// whatever the analysis guessed.
bool is_direct_branch(const Insn &in, int v5)
{
    if (in.flow != Flow::Branch && in.flow != Flow::CondBranch) return false;
    if (!in.thumb) return nds_arm_classify(in.op, v5) == NDS_ARM_B && !(in.op & (1u << 24));
    const NdsThumbClass cls = nds_thumb_classify(static_cast<uint16_t>(in.op), v5);
    return cls == NDS_THUMB_B || cls == NDS_THUMB_BCC;
}

// Can the instruction write R15 (and so set c->branched)? Instructions that
// cannot need no check after their handler. When in doubt, yes.
bool may_branch(const Insn &in, int v5)
{
    if (!in.thumb) {
        const uint32_t op = in.op;
        const uint32_t rd = (op >> 12) & 15u, rn = (op >> 16) & 15u;
        const bool load = (op >> 20) & 1u;
        const bool base_written = !((op >> 24) & 1u) || ((op >> 21) & 1u); // post-indexed, or writeback
        switch (nds_arm_classify(op, v5)) {
        case NDS_ARM_DP: return rd == 15;
        case NDS_ARM_MUL: return rn == 15;              // Rd is in bits 16-19
        case NDS_ARM_MULL: return rd == 15 || rn == 15; // RdLo, RdHi
        case NDS_ARM_SWP: return rd == 15;
        case NDS_ARM_SDT:
        case NDS_ARM_HALF: return (load && rd == 15) || (rn == 15 && base_written);
        case NDS_ARM_BLOCK: return rn == 15 || (load && ((op & 0x8000u) || (op & 0xFFFFu) == 0));
        case NDS_ARM_MRS:
        case NDS_ARM_MSR:
        case NDS_ARM_PLD: return false;
        default: return true; // B, BL, BX, BLX, SWI, BKPT, undefined, and the ARMv5 additions
        }
    }
    const uint16_t op = static_cast<uint16_t>(in.op);
    switch (nds_thumb_classify(op, v5)) {
    case NDS_THUMB_HI_REG: {
        const uint32_t rd = (op & 7u) | ((op >> 4) & 8u);
        const uint32_t kind = (op >> 8) & 3u; // ADD, CMP, MOV, BX/BLX
        return kind == 3 || (kind != 1 && rd == 15);
    }
    case NDS_THUMB_PUSH_POP: return (op & 0x0800u) && ((op & 0x100u) || (op & 0xFFu) == 0);
    case NDS_THUMB_LDM_STM: return (op & 0xFFu) == 0; // an empty list transfers R15 on the ARM7
    case NDS_THUMB_BL: return (op & 0xF800u) != 0xF000u; // the prefix only sets LR
    case NDS_THUMB_BCC: case NDS_THUMB_SWI: case NDS_THUMB_B: case NDS_THUMB_UNDEF: case NDS_THUMB_BKPT:
        return true;
    default: return false;
    }
}

class FunctionEmitter {
public:
    FunctionEmitter(const AnalysedImage &owner, const Function &fn, std::ostream &out)
        : image_(owner.image), an_(*owner.analyzer), symbols_(owner.symbols), fn_(fn), out_(out),
          v5_(owner.image.v5()), p_(owner.image.arm7 ? "nds7" : "nds9")
    {
        members_.insert(fn.insns.begin(), fn.insns.end());
    }

    void emit()
    {
        plan_labels();
        const bool thumb = key_thumb(fn_.entry);
        const uint32_t low = key_addr(fn_.insns.front());

        out_ << "/* " << image_.name << " " << hex8(key_addr(fn_.entry)) << (thumb ? " (Thumb)" : " (ARM)");
        if (symbols_) {
            if (const DsdFunction *named = symbols_->function_at(key_addr(fn_.entry))) out_ << ": " << named->name;
        }
        out_ << " */\n";
        out_ << "void " << function_name(image_, fn_.entry) << "(NdsMachine *m, uint32_t entry)\n{\n";
        out_ << "    NdsCpu *c = &m->" << (image_.arm7 ? "arm7" : "arm9") << ";\n";
        // Any instruction is a place to come back in at: the run loop
        // resumes a function wherever it yielded.
        out_ << "    switch ((entry - 0x" << hex8(low) << "u) >> " << (thumb ? 1 : 2) << ") {\n";
        for (size_t i = 0; i < fn_.insns.size(); i++) {
            uint32_t key = fn_.insns[i];
            uint32_t following = i + 1 < fn_.insns.size() ? fn_.insns[i + 1] : 0xFFFFFFFFu;
            out_ << "    case " << ((key_addr(key) - low) >> (thumb ? 1 : 2)) << "u:\n";
            emit_insn(an_.insns.at(key), following);
        }
        out_ << "    default:\n        return;\n    }\n}\n\n";
    }

private:
    void plan_labels()
    {
        for (size_t i = 0; i < fn_.insns.size(); i++) {
            const Insn &in = an_.insns.at(fn_.insns[i]);
            uint32_t following = i + 1 < fn_.insns.size() ? fn_.insns[i + 1] : 0xFFFFFFFFu;
            if (is_direct_branch(in, v5_) && members_.count(in.target)) needed_.insert(in.target);
            uint32_t next = make_key(in.addr + in.size, in.thumb);
            if (falls_through(in) && next != following && members_.count(next)) needed_.insert(next);
        }
    }

    // Leave for the run loop with the pipeline pointing at `addr`.
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
        const std::string ind = "        ";

        if (needed_.count(key)) out_ << "    " << label(key) << ":\n";
        out_ << ind << "/* " << hex8(in.addr) << ": " << (in.thumb ? hex8(in.op).substr(4) : hex8(in.op)) << " */\n";
        out_ << ind << "if (" << p_ << "_rc_step(m, 0x" << hex8(pc) << "u)) return;\n";

        std::string call;
        std::string inner = ind;
        bool wrapped = false;

        if (!in.thumb) {
            const uint32_t cond = in.op >> 28;
            const NdsArmClass cls = nds_arm_classify(in.op, v5_);
            const char *h = arm_handler(cls);
            if (cls == NDS_ARM_SWI) call = p_ + "_op_swi(c);";
            else if (cls == NDS_ARM_UNDEF) call = p_ + "_op_undefined(c);";
            else if (cls == NDS_ARM_BKPT) call = p_ + "_op_prefetch_abort(c);";
            else if (h) call = p_ + "_" + h + "(c, 0x" + hex8(in.op) + "u);";
            if (cond == NDS_COND_NV && !v5_) {
                out_ << ind << "/* never executes */\n";
                call.clear();
            } else if (cond != NDS_COND_AL && cond != NDS_COND_NV) {
                out_ << ind << "if (nds_cond(c, " << cond << "u)) {\n";
                inner = ind + "    ";
                wrapped = true;
            }
        } else {
            const uint16_t op = static_cast<uint16_t>(in.op);
            const NdsThumbClass cls = nds_thumb_classify(op, v5_);
            const char *h = thumb_handler(cls);
            if (cls == NDS_THUMB_SWI) call = p_ + "_op_swi(c);";
            else if (cls == NDS_THUMB_UNDEF) call = p_ + "_op_undefined(c);";
            else if (cls == NDS_THUMB_BKPT) call = p_ + "_op_prefetch_abort(c);";
            else call = p_ + "_" + h + "(c, 0x" + hex8(op).substr(4) + "u);";
        }

        if (!call.empty()) {
            out_ << inner << call << "\n";
            if (is_direct_branch(in, v5_) && members_.count(in.target)) {
                out_ << inner << "if (c->branched) {\n";
                out_ << inner << "    " << p_ << "_rc_refill(m, 0x" << hex8(key_addr(in.target)) << "u, "
                     << (in.thumb ? 2 : 4) << "u);\n";
                out_ << inner << "    goto " << label(in.target) << ";\n";
                out_ << inner << "}\n";
            } else if (may_branch(in, v5_)) {
                out_ << inner << "if (c->branched) return;\n";
            }
        }
        if (wrapped) out_ << ind << "}\n";

        if (!falls_through(in)) {
            // It branched, and has left above. Reaching here means it did
            // not after all (or its effect needs the run loop: MSR): go on
            // at the next instruction, through the run loop.
            out_ << ind << exit_at(next_addr, in.thumb) << "\n";
            return;
        }
        if (next != following) {
            if (members_.count(next)) out_ << ind << "goto " << label(next) << ";\n";
            else out_ << ind << exit_at(next_addr, in.thumb) << "\n";
        }
    }

    const CodeImage &image_;
    const Analyzer &an_;
    const DsdModule *symbols_;
    const Function &fn_;
    std::ostream &out_;
    const int v5_;
    const std::string p_;
    std::set<uint32_t> members_;
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
    out.close();
    if (!out) {
        error = "cannot write " + path.string();
        return false;
    }
    return true;
}

std::string c_string(const std::string &s)
{
    std::string out = "\"";
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20 || c >= 0x7F) {
            char escape[8];
            std::snprintf(escape, sizeof escape, "\\%03o", static_cast<unsigned>(c));
            out += escape;
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

// Text from the ROM header, made safe inside a C comment.
std::string comment_text(const std::string &s)
{
    std::string out;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        out += (c >= 0x20 && c < 0x7F && ch != '*' && ch != '/' && ch != '\\') ? ch : '_';
    }
    return out;
}

struct EntryRow {
    uint32_t key;
    uint32_t block;
    std::string fn;
};

} // namespace

bool emit_project(const Rom &rom, const Analysis &program, const EmitOptions &options, EmitStats &stats,
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

    std::ostringstream decls;
    decls << "/* Generated by ndsrecomp. Do not edit. */\n#pragma once\n#include \"nds_recomp.h\"\n\n";

    std::ostringstream ranges, blocks;
    std::vector<EntryRow> entries[2];
    uint32_t range_count = 0, block_count = 0;
    size_t file_count = 0;

    for (const auto &owner : program.images) {
        const CodeImage &image = owner->image;
        const Analyzer &an = *owner->analyzer;
        const int cpu = image.arm7 ? 1 : 0;

        // The block of each function: its instructions as runs of
        // addresses, and the hash of those bytes.
        std::map<uint32_t, uint32_t> block_of; // function entry -> block index
        for (const Function &fn : an.functions) {
            if (fn.insns.empty()) continue;
            decls << "void " << function_name(image, fn.entry) << "(NdsMachine *m, uint32_t entry);\n";
            const uint32_t first_range = range_count;
            uint64_t hash = NDS_RC_HASH_INIT;
            uint32_t run_start = 0, run_end = 0;
            auto flush = [&]() {
                if (run_end == run_start) return;
                ranges << "    { 0x" << hex8(run_start) << "u, " << (run_end - run_start) << "u },\n";
                hash = nds_rc_hash(hash, image.bytes.data() + (run_start - image.base), run_end - run_start);
                stats.code_bytes += run_end - run_start;
                range_count++;
            };
            for (uint32_t key : fn.insns) {
                const Insn &in = an.insns.at(key);
                if (in.addr != run_end) {
                    flush();
                    run_start = in.addr;
                }
                run_end = in.addr + in.size;
            }
            flush();
            blocks << "    { " << first_range << "u, " << (range_count - first_range) << "u, 0x" << hex16(hash)
                   << "ull }, /* " << function_name(image, fn.entry) << " */\n";
            block_of[fn.entry] = block_count++;
        }

        // Which function the run loop enters for each label: the function
        // it is the entry of, else the first function containing it.
        std::map<uint32_t, uint32_t> label_owner; // label key -> function entry
        for (const Function &fn : an.functions) {
            if (!fn.insns.empty() && an.labels.count(fn.entry)) label_owner[fn.entry] = fn.entry;
        }
        for (const Function &fn : an.functions) {
            for (uint32_t key : fn.insns) {
                if (an.labels.count(key) && !label_owner.count(key)) label_owner[key] = fn.entry;
            }
        }
        for (const auto &[key, fn] : label_owner) {
            entries[cpu].push_back({ key, block_of.at(fn), function_name(image, fn) });
        }

        // The functions themselves, a few thousand instructions to a file.
        size_t image_file = 0;
        for (size_t first = 0; first < an.functions.size();) {
            std::ostringstream src;
            src << "/* Generated by ndsrecomp from " << comment_text(rom.header.title) << " ("
                << comment_text(rom.header.game_code) << "), " << image.name << ". Do not edit. */\n";
            if (!options.inline_handlers) src << "#define NDS_RC_OUTLINE_HANDLERS 1\n";
            src << "#include \"" << (image.arm7 ? "nds7_recomp.h" : "nds9_recomp.h") << "\"\n";
            src << "#include \"rc_decls.h\"\n\n";
            size_t last = first, count = 0;
            for (; last < an.functions.size() && (last == first || count < options.instructions_per_file); last++) {
                count += an.functions[last].insns.size();
            }
            for (size_t i = first; i < last; i++) {
                if (an.functions[i].insns.empty()) continue;
                FunctionEmitter(*owner, an.functions[i], src).emit();
                stats.instructions += an.functions[i].insns.size();
                stats.functions++;
            }
            char name[96];
            std::snprintf(name, sizeof name, "rc_%s_%04zu.c", image.name.c_str(), image_file++);
            if (!write_file(gen / name, src.str(), error)) return false;
            file_count++;
            first = last;
        }
    }
    stats.files = file_count;
    stats.blocks = block_count;
    stats.ranges = range_count;
    if (!write_file(gen / "rc_decls.h", decls.str(), error)) return false;

    std::ostringstream table;
    table << "/* Generated by ndsrecomp. Do not edit. */\n#include \"rc_decls.h\"\n\n";
    table << "static const NdsRcRange ranges[] = {\n" << ranges.str();
    if (!range_count) table << "    { 0u, 0u },\n";
    table << "};\n\n";
    table << "static const NdsRcBlock blocks[] = {\n" << blocks.str();
    if (!block_count) table << "    { 0u, 0u, 0ull },\n";
    table << "};\n\n";
    for (int cpu = 0; cpu < 2; cpu++) {
        // Sorted by key; functions of different overlays at one address stay
        // next to each other.
        std::stable_sort(entries[cpu].begin(), entries[cpu].end(),
                         [](const EntryRow &a, const EntryRow &b) { return a.key < b.key; });
        table << "static const NdsRcEntry entries" << (cpu ? 7 : 9) << "[] = {\n";
        for (const EntryRow &row : entries[cpu]) {
            table << "    { 0x" << hex8(row.key) << "u, " << row.block << "u, " << row.fn << " },\n";
        }
        if (entries[cpu].empty()) table << "    { 0u, 0u, 0 },\n";
        table << "};\n\n";
        stats.entries[cpu] = entries[cpu].size();
    }
    table << "const uint8_t *nds_rc_embedded_rom(size_t *size);\n\n";
    table << "const NdsRcProgram nds_rc_program = {\n";
    table << "    { entries9, entries7 },\n";
    table << "    { " << entries[0].size() << "u, " << entries[1].size() << "u },\n";
    table << "    blocks,\n    " << block_count << "u,\n    ranges,\n    " << range_count << "u,\n";
    table << "    " << c_string(rom.header.title) << ",\n    " << c_string(rom.header.game_code) << ",\n";
    table << "    nds_rc_embedded_rom,\n};\n";
    if (!write_file(gen / "rc_table.c", table.str(), error)) return false;

    // The ROM itself goes into the executable: a Windows resource with MSVC
    // (and MinGW), .incbin elsewhere. Large ROMs as C arrays compile slowly.
    std::ostringstream rom_c;
    rom_c << "/* Generated by ndsrecomp: the ROM compiled into this executable. */\n";
    rom_c << "#include <stddef.h>\n#include <stdint.h>\n\n";
    rom_c << "#if defined(_WIN32)\n#include <windows.h>\n\n";
    rom_c << "const uint8_t *nds_rc_embedded_rom(size_t *size)\n{\n";
    rom_c << "    HRSRC res = FindResourceA(NULL, \"NDSROM\", MAKEINTRESOURCEA(10)); /* RT_RCDATA */\n";
    rom_c << "    HGLOBAL data = res ? LoadResource(NULL, res) : NULL;\n";
    rom_c << "    if (!data) return NULL;\n";
    rom_c << "    *size = SizeofResource(NULL, res);\n";
    rom_c << "    return (const uint8_t *)LockResource(data);\n}\n\n";
    rom_c << "#else\n";
    rom_c << "#if defined(__APPLE__)\n#define NDS_RC_SYM(x) \"_\" #x\n#define NDS_RC_SECTION \".const_data\"\n";
    rom_c << "#else\n#define NDS_RC_SYM(x) #x\n#define NDS_RC_SECTION \".section .rodata\"\n#endif\n";
    rom_c << "__asm__(NDS_RC_SECTION \"\\n.balign 16\\n.globl \" NDS_RC_SYM(nds_rc_rom_start) \"\\n\" NDS_RC_SYM(nds_rc_rom_start) \":\\n\"\n";
    rom_c << "        \".incbin \\\"\" NDS_RC_ROM_FILE \"\\\"\\n.globl \" NDS_RC_SYM(nds_rc_rom_end) \"\\n\" NDS_RC_SYM(nds_rc_rom_end) \":\\n.text\\n\");\n";
    rom_c << "extern const uint8_t nds_rc_rom_start[], nds_rc_rom_end[];\n\n";
    rom_c << "const uint8_t *nds_rc_embedded_rom(size_t *size)\n{\n";
    rom_c << "    *size = (size_t)(nds_rc_rom_end - nds_rc_rom_start);\n";
    rom_c << "    return nds_rc_rom_start;\n}\n#endif\n";
    if (!write_file(gen / "rc_rom.c", rom_c.str(), error)) return false;

    fs::create_directories(out / "rom", ec);
    {
        std::ofstream rom_out(out / "rom" / (options.name + ".nds"), std::ios::binary);
        rom_out.write(reinterpret_cast<const char *>(rom.data.data()), static_cast<std::streamsize>(rom.data.size()));
        rom_out.close();
        if (!rom_out) {
            error = "cannot write the ROM copy into " + (out / "rom").string();
            return false;
        }
    }

    std::ostringstream main_c;
    main_c << "/* Generated by ndsrecomp. */\n#include \"nds_recomp.h\"\n#include \"nds_runner.h\"\n";
    main_c << "#ifdef NDS_HAVE_SDL\n#include \"nds_sdl.h\"\n#endif\n\n";
    main_c << "extern const NdsRcProgram nds_rc_program;\n\n";
    main_c << "int main(int argc, char **argv)\n{\n";
    main_c << "    /* The ROM is embedded; a path on the command line overrides it. The\n";
    main_c << "     * window unless a test or diagnostic option asks for a headless run. */\n";
    main_c << "#ifdef NDS_HAVE_SDL\n";
    main_c << "    if (!nds_sdl_wants_headless(argc, argv)) return nds_sdl_main(argc, argv, &nds_rc_program, NULL);\n";
    main_c << "#endif\n";
    main_c << "    return nds_runner_main(argc, argv, &nds_rc_program, NULL);\n}\n";
    if (!write_file(out / "main.c", main_c.str(), error)) return false;

    std::ostringstream cmake;
    cmake << "# Generated by ndsrecomp.\n";
    cmake << "cmake_minimum_required(VERSION 3.16)\n";
    cmake << "# Release unless told otherwise. Before project(): with MSVC and a\n";
    cmake << "# single-configuration generator, project() itself makes it Debug.\n";
    cmake << "if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n";
    cmake << "    set(CMAKE_BUILD_TYPE Release CACHE STRING \"Build type\" FORCE)\nendif()\n";
    cmake << "project(" << options.name << " C)\n\n";
    cmake << "set(CMAKE_C_STANDARD 11)\nset(CMAKE_C_STANDARD_REQUIRED ON)\n\n";
    cmake << "# Runtime snapshot copied in by ndsrecomp. Test tooling may link a\n";
    cmake << "# prebuilt runtime instead (NDSRT_PREBUILT_LIB + NDSRT_PREBUILT_INCLUDE).\n";
    cmake << "set(NDSRT_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/runtime-nds\")\n";
    cmake << "if(NDSRT_PREBUILT_LIB)\n";
    cmake << "    add_library(ndsruntime STATIC IMPORTED)\n";
    cmake << "    set_target_properties(ndsruntime PROPERTIES IMPORTED_LOCATION \"${NDSRT_PREBUILT_LIB}\"\n";
    cmake << "        INTERFACE_INCLUDE_DIRECTORIES \"${NDSRT_PREBUILT_INCLUDE}\")\n";
    cmake << "else()\n";
    cmake << "    file(GLOB NDSRT_SOURCES \"${NDSRT_DIR}/src/*.c\")\n";
    cmake << "    add_library(ndsruntime STATIC ${NDSRT_SOURCES})\n";
    cmake << "    target_include_directories(ndsruntime PUBLIC \"${NDSRT_DIR}/include\")\n";
    cmake << "    target_include_directories(ndsruntime PRIVATE \"${NDSRT_DIR}/src\")\n";
    cmake << "endif()\n\n";
    cmake << "file(GLOB RC_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c\")\n";
    cmake << "add_executable(" << options.name << " main.c ${RC_SOURCES})\n";
    cmake << "target_include_directories(" << options.name << " PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/generated\")\n";
    cmake << "target_link_libraries(" << options.name << " PRIVATE ndsruntime)\n";
    cmake << "# The larger 3D picture's lines are shared between threads (nds_gx_workers.c).\n";
    cmake << "find_package(Threads REQUIRED)\n";
    cmake << "target_link_libraries(" << options.name << " PRIVATE Threads::Threads)\n";
    cmake << "if(NOT WIN32)\n    target_link_libraries(" << options.name << " PRIVATE m)\nendif()\n";
    cmake << "if(MSVC)\n    target_compile_options(" << options.name << " PRIVATE /bigobj)\nendif()\n\n";
    cmake << "# The ROM is compiled into the executable (generated/rc_rom.c).\n";
    cmake << "set(NDS_RC_ROM \"${CMAKE_CURRENT_SOURCE_DIR}/rom/" << options.name << ".nds\")\n";
    cmake << "if(WIN32)\n";
    cmake << "    enable_language(RC)\n";
    cmake << "    file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\" \"NDSROM RCDATA \\\"${NDS_RC_ROM}\\\"\\n\")\n";
    cmake << "    target_sources(" << options.name << " PRIVATE \"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\")\n";
    cmake << "    set_source_files_properties(\"${CMAKE_CURRENT_BINARY_DIR}/rom.rc\" PROPERTIES OBJECT_DEPENDS \"${NDS_RC_ROM}\")\n";
    cmake << "else()\n";
    cmake << "    set_source_files_properties(generated/rc_rom.c PROPERTIES\n";
    cmake << "        COMPILE_DEFINITIONS \"NDS_RC_ROM_FILE=\\\"${NDS_RC_ROM}\\\"\" OBJECT_DEPENDS \"${NDS_RC_ROM}\")\n";
    cmake << "endif()\n\n";
    cmake << "# Window, input, audio and the settings menu (Dear ImGui) when SDL2 is\n";
    cmake << "# available; headless otherwise.\n";
    cmake << "find_package(SDL2 QUIET)\n";
    cmake << "if(TARGET SDL2::SDL2)\n";
    cmake << "    enable_language(CXX)\n";
    cmake << "    set(NDS_IMGUI_DIR \"${NDSRT_DIR}/vendor/imgui\")\n";
    cmake << "    file(GLOB NDS_PLATFORM_SOURCES \"${NDSRT_DIR}/platform/*.cpp\")\n";
    cmake << "    target_sources(" << options.name << " PRIVATE ${NDS_PLATFORM_SOURCES}\n";
    cmake << "        \"${NDS_IMGUI_DIR}/imgui.cpp\" \"${NDS_IMGUI_DIR}/imgui_draw.cpp\"\n";
    cmake << "        \"${NDS_IMGUI_DIR}/imgui_tables.cpp\" \"${NDS_IMGUI_DIR}/imgui_widgets.cpp\"\n";
    cmake << "        \"${NDS_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp\"\n";
    cmake << "        \"${NDS_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp\")\n";
    cmake << "    target_include_directories(" << options.name << " PRIVATE \"${NDSRT_DIR}/platform\" \"${NDS_IMGUI_DIR}\")\n";
    cmake << "    set_target_properties(" << options.name << " PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)\n";
    cmake << "    target_compile_definitions(" << options.name << " PRIVATE NDS_HAVE_SDL)\n";
    cmake << "    target_link_libraries(" << options.name << " PRIVATE SDL2::SDL2)\n";
    cmake << "    get_target_property(_sdl2_type SDL2::SDL2 TYPE)\n";
    cmake << "    if(WIN32 AND _sdl2_type STREQUAL \"SHARED_LIBRARY\")\n";
    cmake << "        add_custom_command(TARGET " << options.name << " POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different\n";
    cmake << "            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:" << options.name << ">)\n";
    cmake << "    endif()\n";
    cmake << "    # macOS .app bundle (off unless NATIVE_CONSOLE_MACOS_BUNDLE=ON)\n";
    cmake << "    include(\"${NDSRT_DIR}/cmake/NativeConsoleMacBundle.cmake\")\n";
    cmake << "    native_console_macos_bundle(" << options.name << ")\n";
    cmake << "endif()\n";
    if (!write_file(out / "CMakeLists.txt", cmake.str(), error)) return false;

    // Snapshot the runtime so the project builds on its own.
    fs::path rt_src(options.runtime_dir);
    fs::path rt_dst = out / "runtime-nds";
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
        fs::path rt_dir = rt_src.filename().empty() ? rt_src.parent_path() : rt_src; // "runtime-nds/"
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

} // namespace ndsrecomp
