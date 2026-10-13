#include "analyzer.h"

#include <algorithm>
#include <array>

extern "C" {
#include "gba_cpu_decode.h"
}

namespace gbarecomp {

namespace {

// What the analyzer knows about a register within one linear run of code:
// enough to resolve BX targets loaded from literal pools and the jump-table
// idioms compilers emit for switch statements.
struct Value {
    enum Kind : uint8_t { Unknown, Const, Scaled, TablePtr, TableVal } kind = Unknown;
    uint32_t v = 0;     // Const: the value; TablePtr/TableVal: table base
    uint32_t count = 0; // Scaled/TablePtr/TableVal: number of table entries
};

struct Regs {
    std::array<Value, 16> r{};
    std::array<uint32_t, 16> bound{}; // entries allowed by a preceding cmp + branch, 0 = unknown
    int cmp_reg = -1;
    uint32_t cmp_imm = 0;

    void clobber(uint32_t mask)
    {
        for (int i = 0; i < 16; i++) {
            if (mask & (1u << i)) {
                r[i] = Value{};
                bound[i] = 0;
                if (cmp_reg == i) cmp_reg = -1;
            }
        }
    }
    void set_const(int reg, uint32_t v)
    {
        clobber(1u << reg);
        r[reg].kind = Value::Const;
        r[reg].v = v;
    }
    // Number of valid indices after `cmp reg, #imm` and a branch away on
    // condition `cond` (taken = out of range). 0 if unknown.
    void bound_from_branch_away(uint32_t cond)
    {
        if (cmp_reg < 0) return;
        switch (cond) {
        case GBA_COND_HI: case GBA_COND_GT: bound[cmp_reg] = cmp_imm + 1; break;
        case GBA_COND_CS: case GBA_COND_GE: bound[cmp_reg] = cmp_imm; break;
        default: break;
        }
    }
    // Same, for ARM instructions executed only when in range (addls/ldrls).
    uint32_t bound_for_cond(int reg, uint32_t cond) const
    {
        if (cmp_reg != reg) return 0;
        if (cond == GBA_COND_LS || cond == GBA_COND_LE) return cmp_imm + 1;
        if (cond == GBA_COND_CC || cond == GBA_COND_LT) return cmp_imm;
        return 0;
    }
};

constexpr uint32_t kCallerSaved = 0x500Fu; // r0-r3, r12, r14

uint32_t ror32(uint32_t v, uint32_t n)
{
    n &= 31u;
    return n ? (v >> n) | (v << (32u - n)) : v;
}

// Registers an ARM instruction may write (conservative).
uint32_t arm_dests(uint32_t op, GbaArmClass cls)
{
    uint32_t rn = (op >> 16) & 15u, rd = (op >> 12) & 15u;
    switch (cls) {
    case GBA_ARM_DP: {
        uint32_t opcode = (op >> 21) & 15u;
        return (opcode >= 8 && opcode <= 11) ? 0u : (1u << rd);
    }
    case GBA_ARM_MRS: return 1u << rd;
    case GBA_ARM_MSR: return 0;
    case GBA_ARM_MUL: return 1u << rn; // Rd is bits 16-19
    case GBA_ARM_MULL: return (1u << rn) | (1u << rd);
    case GBA_ARM_SWP: return 1u << rd;
    case GBA_ARM_SDT:
    case GBA_ARM_HALF: {
        uint32_t mask = (op & (1u << 20)) ? (1u << rd) : 0u;
        if (!(op & (1u << 24)) || (op & (1u << 21))) mask |= 1u << rn;
        return mask;
    }
    case GBA_ARM_BLOCK: {
        uint32_t mask = (op & (1u << 20)) ? (op & 0xFFFFu) : 0u;
        if (op & (1u << 21)) mask |= 1u << rn;
        return mask;
    }
    case GBA_ARM_B: return (op & (1u << 24)) ? kCallerSaved : 0u;
    case GBA_ARM_BX: return 0;
    default: return 0xFFFFu;
    }
}

// Registers a Thumb instruction may write (conservative).
uint32_t thumb_dests(uint16_t op, GbaThumbClass cls)
{
    uint32_t rd3 = op & 7u;
    switch (cls) {
    case GBA_THUMB_SHIFT: case GBA_THUMB_ADD_SUB: case GBA_THUMB_LDST_REG:
    case GBA_THUMB_LDST_SIGN: case GBA_THUMB_LDST_IMM: case GBA_THUMB_LDST_HALF:
        return 1u << rd3;
    case GBA_THUMB_IMM8: case GBA_THUMB_LDR_PC: case GBA_THUMB_LDST_SP: case GBA_THUMB_ADD_PC_SP:
        return 1u << ((op >> 8) & 7u);
    case GBA_THUMB_ALU: {
        uint32_t aop = (op >> 6) & 15u;
        return (aop == 8 || aop == 10 || aop == 11) ? 0u : (1u << rd3);
    }
    case GBA_THUMB_HI_REG: {
        uint32_t hop = (op >> 8) & 3u;
        return (hop == 0 || hop == 2) ? (1u << ((op & 7u) | ((op >> 4) & 8u))) : 0u;
    }
    case GBA_THUMB_ADD_SP: return 1u << 13;
    case GBA_THUMB_PUSH_POP: return (1u << 13) | ((op & (1u << 11)) ? (op & 0xFFu) : 0u);
    case GBA_THUMB_LDM_STM: return (1u << ((op >> 8) & 7u)) | ((op & (1u << 11)) ? (op & 0xFFu) : 0u);
    case GBA_THUMB_BL: return (op & (1u << 11)) ? kCallerSaved : (1u << 14);
    case GBA_THUMB_BCC: case GBA_THUMB_B: return 0;
    default: return 0xFFFFu;
    }
}

} // namespace

Analyzer::Analyzer(const Rom &rom_, AnalyzerOptions options_) : rom(rom_), options(std::move(options_)) {}

bool Analyzer::is_data(uint32_t addr, uint32_t size) const
{
    for (uint32_t a = addr & ~3u; a < addr + size; a += 4) {
        if (data_words.count(a)) return true;
    }
    return false;
}

void Analyzer::push(uint32_t key)
{
    if (!insns.count(key)) worklist_.push_back(key);
}

void Analyzer::add_function(uint32_t key)
{
    if (!rom.contains(key_addr(key), key_thumb(key) ? 2 : 4)) return;
    if (!key_thumb(key) && (key_addr(key) & 3u)) return; // misaligned ARM target
    function_entries.insert(key);
    labels.insert(key);
    push(key);
}

void Analyzer::drain()
{
    while (!worklist_.empty()) {
        uint32_t key = worklist_.back();
        worklist_.pop_back();
        explore(key);
    }
}

void Analyzer::run()
{
    // The cartridge header starts with an ARM branch to the entry point.
    add_function(make_key(kRomBase, false));
    for (uint32_t key : options.extra_entries) add_function(key);
    drain();
    if (options.pointer_scan) {
        scan_pointers();
        drain();
    }
    for (uint32_t key : options.trace_keys) {
        if (!rom.contains(key_addr(key), key_thumb(key) ? 2 : 4)) continue;
        labels.insert(key);
        push(key);
    }
    drain();
    build_functions();

    // Trace keys that no function reaches start functions of their own. They
    // are taken in address order, each covering what it reaches, so a run
    // of code only the trace knows (code copied to RAM, say) becomes one
    // function instead of one function per traced instruction.
    std::set<uint32_t> covered;
    for (const Function &fn : functions) covered.insert(fn.insns.begin(), fn.insns.end());
    std::vector<uint32_t> keys(options.trace_keys.begin(), options.trace_keys.end());
    std::sort(keys.begin(), keys.end(), [](uint32_t a, uint32_t b) { return key_addr(a) < key_addr(b); });
    bool added = false;
    for (uint32_t key : keys) {
        if (!insns.count(key) || covered.count(key) || function_entries.count(key)) continue;
        function_entries.insert(key);
        trace_entries++;
        added = true;
        for (uint32_t k : reach(key)) covered.insert(k);
    }
    if (added) build_functions();
}

void Analyzer::explore(uint32_t start)
{
    const bool thumb = key_thumb(start);
    const uint32_t size = thumb ? 2u : 4u;
    uint32_t addr = key_addr(start);
    Regs regs;
    bool prev_mov_lr_pc = false; // ARM "mov lr, pc" just before

    for (;;) {
        uint32_t key = make_key(addr, thumb);
        if (insns.count(key)) return;
        if (!rom.contains(addr, size) || is_data(addr, size)) return;

        Insn in;
        in.addr = addr;
        in.thumb = thumb;
        in.size = static_cast<uint8_t>(size);
        in.op = thumb ? rom.read16(addr) : rom.read32(addr);
        const uint32_t next = make_key(addr + size, thumb);
        bool stop = false;

        if (!thumb) {
            const uint32_t op = in.op;
            const uint32_t cond = op >> 28;
            const GbaArmClass cls = gba_arm_classify(op);
            const uint32_t rn = (op >> 16) & 15u, rd = (op >> 12) & 15u, rm = op & 15u;
            in.conditional = cond != GBA_COND_AL;
            uint32_t written = arm_dests(op, cls);
            bool handled = false;

            if (cond == GBA_COND_NV) {
                // Never executes on the ARM7TDMI.
            } else if (cls == GBA_ARM_B) {
                uint32_t target = addr + 8 + static_cast<uint32_t>(static_cast<int32_t>(op << 8) >> 6);
                in.target = make_key(target, false);
                if (op & (1u << 24)) {
                    in.flow = Flow::Call;
                    add_function(in.target);
                } else {
                    in.flow = in.conditional ? Flow::CondBranch : Flow::Branch;
                    labels.insert(in.target);
                    push(in.target);
                    stop = !in.conditional;
                }
            } else if (cls == GBA_ARM_BX) {
                const Value &v = regs.r[rm];
                if (v.kind == Value::Const) add_function(make_key(v.v & ~1u, v.v & 1u));
                if (rm == 14) {
                    in.flow = Flow::Return;
                } else if (prev_mov_lr_pc) {
                    in.flow = Flow::CallIndirect;
                } else {
                    in.flow = Flow::Indirect;
                    indirect_unresolved += v.kind != Value::Const;
                }
                stop = !in.conditional && in.flow != Flow::CallIndirect;
            } else if (cls == GBA_ARM_SWI) {
                labels.insert(next); // the BIOS returns here
            } else if (cls == GBA_ARM_UNDEF) {
                in.flow = Flow::Stop;
                stop = !in.conditional;
            } else if (cls == GBA_ARM_MSR) {
                if (!(op & (1u << 22))) {
                    in.exits_after = true;
                    labels.insert(next);
                }
            } else if (cls == GBA_ARM_DP) {
                const uint32_t opcode = (op >> 21) & 15u;
                const bool imm = (op & (1u << 25)) != 0;
                if (opcode == 0xA && imm && (op & (1u << 20))) {
                    regs.cmp_reg = static_cast<int>(rn);
                    regs.cmp_imm = ror32(op & 0xFFu, ((op >> 8) & 15u) * 2u);
                }
                if (rd == 15 && !(opcode >= 8 && opcode <= 11)) {
                    // addls pc, pc, rX, lsl #2: a table of branches follows.
                    uint32_t count = 0;
                    if (opcode == 4 && rn == 15 && !imm && (op & 0xFF0u) == 0x100u) {
                        count = regs.bound_for_cond(static_cast<int>(rm), cond);
                    }
                    if (count) {
                        in.flow = Flow::Table;
                        for (uint32_t i = 0; i < count; i++) {
                            uint32_t t = make_key(addr + 8 + 4 * i, false);
                            in.table.push_back(t);
                            labels.insert(t);
                            push(t);
                        }
                        tables_found++;
                    } else if (opcode == 0xD && !imm && rm == 14 && (op & 0xFF0u) == 0) {
                        in.flow = Flow::Return; // mov pc, lr
                    } else {
                        in.flow = prev_mov_lr_pc ? Flow::CallIndirect : Flow::Indirect;
                        indirect_unresolved += in.flow == Flow::Indirect;
                    }
                    stop = !in.conditional && in.flow != Flow::CallIndirect;
                } else if (opcode == 0xD && imm) {
                    regs.set_const(static_cast<int>(rd), ror32(op & 0xFFu, ((op >> 8) & 15u) * 2u));
                    handled = true;
                } else if ((opcode == 4 || opcode == 2) && imm && rn == 15) {
                    uint32_t v = ror32(op & 0xFFu, ((op >> 8) & 15u) * 2u);
                    regs.set_const(static_cast<int>(rd), opcode == 4 ? addr + 8 + v : addr + 8 - v);
                    handled = true;
                }
            } else if (cls == GBA_ARM_SDT) {
                const bool load = (op & (1u << 20)) != 0, pre = (op & (1u << 24)) != 0;
                const bool reg_off = (op & (1u << 25)) != 0, byte = (op & (1u << 22)) != 0;
                const bool wb = (op & (1u << 21)) != 0, up = (op & (1u << 23)) != 0;
                if (load && rn == 15 && pre && !wb && !reg_off && !byte) {
                    uint32_t lit = up ? addr + 8 + (op & 0xFFFu) : addr + 8 - (op & 0xFFFu);
                    if ((lit & 3u) == 0 && rom.contains(lit, 4)) {
                        data_words.insert(lit);
                        if (rd != 15) {
                            regs.set_const(static_cast<int>(rd), rom.read32(lit));
                            handled = true;
                        }
                    }
                }
                if (load && rd == 15) {
                    // ldrls pc, [pc, rX, lsl #2]: a table of addresses follows.
                    uint32_t count = 0;
                    if (rn == 15 && pre && !wb && reg_off && up && !byte && ((op >> 4) & 0xFFu) == 0x10u) {
                        count = regs.bound_for_cond(static_cast<int>(rm), cond);
                    }
                    if (count) {
                        in.flow = Flow::Table;
                        for (uint32_t i = 0; i < count; i++) {
                            uint32_t slot = addr + 8 + 4 * i;
                            if (!rom.contains(slot, 4)) break;
                            data_words.insert(slot);
                            uint32_t t = make_key(rom.read32(slot) & ~3u, false);
                            in.table.push_back(t);
                            labels.insert(t);
                            push(t);
                        }
                        tables_found++;
                    } else if (rn == 13 && !pre) {
                        in.flow = Flow::Return; // ldr pc, [sp], #4
                    } else {
                        in.flow = prev_mov_lr_pc ? Flow::CallIndirect : Flow::Indirect;
                        indirect_unresolved += in.flow == Flow::Indirect;
                    }
                    stop = !in.conditional && in.flow != Flow::CallIndirect;
                }
            } else if (cls == GBA_ARM_BLOCK) {
                if ((op & (1u << 20)) && (op & 0x8000u)) {
                    in.flow = Flow::Return;
                    stop = !in.conditional;
                }
            } else if (cls == GBA_ARM_HALF || cls == GBA_ARM_MUL || cls == GBA_ARM_MULL || cls == GBA_ARM_SWP) {
                if (written & 0x8000u) {
                    in.flow = Flow::Indirect;
                    indirect_unresolved++;
                    stop = !in.conditional;
                }
            }

            if (!handled) regs.clobber(written);
            if (in.flow == Flow::Call || in.flow == Flow::CallIndirect) {
                regs.clobber(kCallerSaved);
                labels.insert(next);
            }
            prev_mov_lr_pc = op == 0xE1A0E00Fu;
        } else {
            const uint16_t op = static_cast<uint16_t>(in.op);
            const GbaThumbClass cls = gba_thumb_classify(op);
            uint32_t written = thumb_dests(op, cls);
            bool handled = false;

            switch (cls) {
            case GBA_THUMB_B: {
                uint32_t target = addr + 4 + static_cast<uint32_t>(static_cast<int32_t>(static_cast<uint32_t>(op) << 21) >> 20);
                in.flow = Flow::Branch;
                in.target = make_key(target, true);
                labels.insert(in.target);
                push(in.target);
                stop = true;
                break;
            }
            case GBA_THUMB_BCC: {
                uint32_t cond = (op >> 8) & 15u;
                uint32_t target = addr + 4 + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(op & 0xFFu)) * 2);
                in.target = make_key(target, true);
                in.flow = cond == 14 ? Flow::Branch : Flow::CondBranch;
                labels.insert(in.target);
                push(in.target);
                regs.bound_from_branch_away(cond);
                stop = cond == 14;
                break;
            }
            case GBA_THUMB_BL:
                if (!(op & (1u << 11)) && rom.contains(addr + 2, 2)) {
                    uint16_t suffix = rom.read16(addr + 2);
                    if ((suffix & 0xF800u) == 0xF800u) {
                        // Prefix of a BL pair: the suffix at addr + 2 calls.
                        int32_t hi = static_cast<int32_t>(static_cast<uint32_t>(op & 0x7FFu) << 21) >> 9;
                        uint32_t target = addr + 4 + static_cast<uint32_t>(hi) + ((suffix & 0x7FFu) << 1);
                        add_function(make_key(target, true));
                        insns[key] = in; // prefix itself falls through
                        Insn sfx;
                        sfx.addr = addr + 2;
                        sfx.op = suffix;
                        sfx.thumb = true;
                        sfx.size = 2;
                        sfx.flow = Flow::Call;
                        sfx.target = make_key(target, true);
                        uint32_t sfx_key = make_key(addr + 2, true);
                        if (!insns.count(sfx_key)) insns[sfx_key] = sfx;
                        labels.insert(make_key(addr + 4, true));
                        regs.clobber(kCallerSaved);
                        addr += 4;
                        continue;
                    }
                }
                if (op & (1u << 11)) {
                    // Lone suffix: calls LR + offset, target unknown here.
                    in.flow = Flow::CallIndirect;
                    labels.insert(next);
                }
                break;
            case GBA_THUMB_HI_REG: {
                uint32_t hop = (op >> 8) & 3u;
                uint32_t rd = (op & 7u) | ((op >> 4) & 8u);
                uint32_t rs = (op >> 3) & 15u;
                const Value &v = regs.r[rs];
                if (hop == 3) { // BX
                    if (v.kind == Value::Const) add_function(make_key(v.v & ~1u, v.v & 1u));
                    in.flow = rs == 14 ? Flow::Return : Flow::Indirect;
                    indirect_unresolved += in.flow == Flow::Indirect && v.kind != Value::Const;
                    stop = true;
                } else if (rd == 15 && hop != 1) {
                    if (hop == 2 && v.kind == Value::TableVal && v.count) {
                        in.flow = Flow::Table;
                        for (uint32_t i = 0; i < v.count; i++) {
                            uint32_t slot = v.v + 4 * i;
                            if (!rom.contains(slot, 4)) break;
                            data_words.insert(slot);
                            uint32_t t = make_key(rom.read32(slot) & ~1u, true);
                            in.table.push_back(t);
                            labels.insert(t);
                            push(t);
                        }
                        tables_found++;
                    } else if (hop == 2 && v.kind == Value::Const) {
                        in.flow = Flow::Branch;
                        in.target = make_key(v.v & ~1u, true);
                        labels.insert(in.target);
                        push(in.target);
                    } else {
                        in.flow = (hop == 2 && rs == 14) ? Flow::Return : Flow::Indirect;
                        indirect_unresolved += in.flow == Flow::Indirect;
                    }
                    stop = true;
                } else if (hop == 2 && v.kind != Value::Unknown) {
                    Value copy = v;
                    regs.clobber(1u << rd);
                    regs.r[rd] = copy;
                    handled = true;
                } else if (hop == 0 && rd != 15) {
                    const Value &a = regs.r[rd];
                    if ((a.kind == Value::Const && v.kind == Value::Scaled) ||
                        (a.kind == Value::Scaled && v.kind == Value::Const)) {
                        Value p;
                        p.kind = Value::TablePtr;
                        p.v = a.kind == Value::Const ? a.v : v.v;
                        p.count = a.kind == Value::Scaled ? a.count : v.count;
                        regs.clobber(1u << rd);
                        regs.r[rd] = p;
                        handled = true;
                    }
                }
                break;
            }
            case GBA_THUMB_PUSH_POP:
                if ((op & (1u << 11)) && (op & 0x100u)) {
                    in.flow = Flow::Return;
                    stop = true;
                }
                break;
            case GBA_THUMB_IMM8: {
                uint32_t rd = (op >> 8) & 7u, imm = op & 0xFFu;
                uint32_t sub = (op >> 11) & 3u;
                if (sub == 0) {
                    regs.set_const(static_cast<int>(rd), imm);
                    handled = true;
                } else if (sub == 1) {
                    regs.cmp_reg = static_cast<int>(rd);
                    regs.cmp_imm = imm;
                }
                break;
            }
            case GBA_THUMB_LDR_PC: {
                uint32_t rd = (op >> 8) & 7u;
                uint32_t lit = ((addr + 4) & ~3u) + ((op & 0xFFu) << 2);
                if (rom.contains(lit, 4)) {
                    data_words.insert(lit);
                    regs.set_const(static_cast<int>(rd), rom.read32(lit));
                    handled = true;
                }
                break;
            }
            case GBA_THUMB_ADD_PC_SP:
                if (!(op & (1u << 11))) {
                    regs.set_const(static_cast<int>((op >> 8) & 7u), ((addr + 4) & ~3u) + ((op & 0xFFu) << 2));
                    handled = true;
                }
                break;
            case GBA_THUMB_SHIFT: {
                uint32_t rd = op & 7u, rs = (op >> 3) & 7u;
                uint32_t type = (op >> 11) & 3u, amount = (op >> 6) & 31u;
                if (type == 0 && amount == 2 && regs.bound[rs]) {
                    Value s;
                    s.kind = Value::Scaled;
                    s.count = regs.bound[rs];
                    regs.clobber(1u << rd);
                    regs.r[rd] = s;
                    handled = true;
                }
                break;
            }
            case GBA_THUMB_ADD_SUB: {
                uint32_t rd = op & 7u, rs = (op >> 3) & 7u, rn = (op >> 6) & 7u;
                if (!(op & (1u << 10)) && !(op & (1u << 9))) { // add rd, rs, rn
                    const Value &a = regs.r[rs], &b = regs.r[rn];
                    if ((a.kind == Value::Const && b.kind == Value::Scaled) ||
                        (a.kind == Value::Scaled && b.kind == Value::Const)) {
                        Value p;
                        p.kind = Value::TablePtr;
                        p.v = a.kind == Value::Const ? a.v : b.v;
                        p.count = a.kind == Value::Scaled ? a.count : b.count;
                        regs.clobber(1u << rd);
                        regs.r[rd] = p;
                        handled = true;
                    }
                }
                break;
            }
            case GBA_THUMB_LDST_IMM: {
                uint32_t rd = op & 7u, rb = (op >> 3) & 7u;
                bool word_load = ((op >> 11) & 3u) == 1u;
                if (word_load && ((op >> 6) & 31u) == 0 && regs.r[rb].kind == Value::TablePtr) {
                    Value t = regs.r[rb];
                    t.kind = Value::TableVal;
                    regs.clobber(1u << rd);
                    regs.r[rd] = t;
                    handled = true;
                }
                break;
            }
            case GBA_THUMB_LDST_REG: {
                uint32_t rd = op & 7u, rb = (op >> 3) & 7u, ro = (op >> 6) & 7u;
                bool word_load = ((op >> 10) & 3u) == 2u;
                const Value &a = regs.r[rb], &b = regs.r[ro];
                if (word_load && ((a.kind == Value::Const && b.kind == Value::Scaled) ||
                                  (a.kind == Value::Scaled && b.kind == Value::Const))) {
                    Value t;
                    t.kind = Value::TableVal;
                    t.v = a.kind == Value::Const ? a.v : b.v;
                    t.count = a.kind == Value::Scaled ? a.count : b.count;
                    regs.clobber(1u << rd);
                    regs.r[rd] = t;
                    handled = true;
                }
                break;
            }
            case GBA_THUMB_SWI:
                labels.insert(next);
                break;
            case GBA_THUMB_UNDEF:
                in.flow = Flow::Stop;
                stop = true;
                break;
            default:
                break;
            }

            if (!handled) regs.clobber(written);
            if (in.flow == Flow::CallIndirect) regs.clobber(kCallerSaved);
        }

        insns[key] = in;
        if (stop) return;
        addr += size;
    }
}

void Analyzer::scan_pointers()
{
    // Function pointers in ROM data: a word that points into the ROM at a
    // function prologue (Thumb push {.., lr} or ARM stmfd sp!, {.., lr}).
    for (uint32_t off = 0; off + 4 <= rom.data.size(); off += 4) {
        uint32_t v = rom.read32(kRomBase + off);
        uint32_t target = v & ~1u;
        if (!rom.contains(target, 4) || function_entries.count(make_key(target, v & 1u))) continue;
        if (v & 1u) {
            if ((rom.read16(target) & 0xFF00u) != 0xB500u) continue;
        } else {
            if ((target & 3u) || (rom.read32(target) & 0xFFFF4000u) != 0xE92D4000u) continue;
        }
        if (is_data(target, 2)) continue;
        pointer_entries++;
        add_function(make_key(target, v & 1u));
    }
}

// The instructions a function starting at `entry` consists of, in address
// order: everything reachable from it without entering another function.
std::vector<uint32_t> Analyzer::reach(uint32_t entry) const
{
    std::set<uint32_t> seen;
    std::vector<uint32_t> stack{ entry };
    auto follow = [&](uint32_t key) {
        if (!insns.count(key) || seen.count(key)) return;
        // A branch to another function's entry is a tail call: leave
        // through the dispatcher instead of duplicating that function.
        if (key != entry && function_entries.count(key)) return;
        stack.push_back(key);
    };
    while (!stack.empty()) {
        uint32_t key = stack.back();
        stack.pop_back();
        if (!seen.insert(key).second) continue;
        const Insn &in = insns.at(key);
        uint32_t next = make_key(in.addr + in.size, in.thumb);
        switch (in.flow) {
        case Flow::Next: case Flow::Call: case Flow::CallIndirect:
            follow(next);
            break;
        case Flow::CondBranch:
            follow(in.target);
            follow(next);
            break;
        case Flow::Branch:
            follow(in.target);
            break;
        case Flow::Table:
            for (uint32_t t : in.table) follow(t);
            if (in.conditional) follow(next);
            break;
        default:
            if (in.conditional) follow(next); // conditional return/indirect
            break;
        }
    }
    std::vector<uint32_t> out(seen.begin(), seen.end());
    std::sort(out.begin(), out.end(), [](uint32_t a, uint32_t b) { return key_addr(a) < key_addr(b); });
    return out;
}

void Analyzer::build_functions()
{
    functions.clear();
    for (uint32_t entry : function_entries) {
        if (!insns.count(entry)) continue;
        Function fn;
        fn.entry = entry;
        fn.insns = reach(entry);
        functions.push_back(std::move(fn));
    }
}

} // namespace gbarecomp
