#include "analyzer.h"

#include <algorithm>
#include <array>

extern "C" {
#include "nds_cpu_decode.h"
}

namespace ndsrecomp {

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
        case NDS_COND_HI: case NDS_COND_GT: bound[cmp_reg] = cmp_imm + 1; break;
        case NDS_COND_CS: case NDS_COND_GE: bound[cmp_reg] = cmp_imm; break;
        default: break;
        }
    }
    // Same, for ARM instructions executed only when in range (addls/ldrls).
    uint32_t bound_for_cond(int reg, uint32_t cond) const
    {
        if (cmp_reg != reg) return 0;
        if (cond == NDS_COND_LS || cond == NDS_COND_LE) return cmp_imm + 1;
        if (cond == NDS_COND_CC || cond == NDS_COND_LT) return cmp_imm;
        return 0;
    }
};

constexpr uint32_t kCallerSaved = 0x500Fu; // r0-r3, r12, r14
constexpr uint32_t kMaxTable = 1024;       // a larger "table" is a misread compare

uint32_t ror32(uint32_t v, uint32_t n)
{
    n &= 31u;
    return n ? (v >> n) | (v << (32u - n)) : v;
}

// Registers an ARM instruction may write (conservative).
uint32_t arm_dests(uint32_t op, NdsArmClass cls)
{
    uint32_t rn = (op >> 16) & 15u, rd = (op >> 12) & 15u;
    switch (cls) {
    case NDS_ARM_DP: {
        uint32_t opcode = (op >> 21) & 15u;
        return (opcode >= 8 && opcode <= 11) ? 0u : (1u << rd);
    }
    case NDS_ARM_MRS: return 1u << rd;
    case NDS_ARM_MSR: return 0;
    case NDS_ARM_MUL: return 1u << rn; // Rd is bits 16-19
    case NDS_ARM_MULL: return (1u << rn) | (1u << rd);
    case NDS_ARM_SWP: return 1u << rd;
    case NDS_ARM_SDT:
    case NDS_ARM_HALF: {
        uint32_t mask = (op & (1u << 20)) ? (1u << rd) : 0u;
        if (!(op & (1u << 24)) || (op & (1u << 21))) mask |= 1u << rn;
        return mask;
    }
    case NDS_ARM_BLOCK: {
        uint32_t mask = (op & (1u << 20)) ? (op & 0xFFFFu) : 0u;
        if (op & (1u << 21)) mask |= 1u << rn;
        return mask;
    }
    case NDS_ARM_B: return (op & (1u << 24)) ? kCallerSaved : 0u;
    case NDS_ARM_BX: return 0;
    case NDS_ARM_BLX_REG:
    case NDS_ARM_BLX_IMM: return kCallerSaved;
    case NDS_ARM_PLD: return 0;
    case NDS_ARM_CLZ:
    case NDS_ARM_QARITH: return 1u << rd;
    case NDS_ARM_DSP_MUL: return (1u << rn) | (1u << rd); // Rd, or RdHi and RdLo
    case NDS_ARM_DOUBLE: {
        // Bit 5 clear is LDRD, which loads Rd and Rd + 1.
        uint32_t mask = (op & 0x20u) ? 0u : ((1u << rd) | (1u << ((rd + 1u) & 15u)));
        if (!(op & (1u << 24)) || (op & (1u << 21))) mask |= 1u << rn;
        return mask;
    }
    case NDS_ARM_CP_REG:
        // MRC into R15 sets the flags, not the register.
        return ((op & (1u << 20)) && rd != 15) ? (1u << rd) : 0u;
    default: return 0xFFFFu;
    }
}

// Registers a Thumb instruction may write (conservative).
uint32_t thumb_dests(uint16_t op, NdsThumbClass cls)
{
    uint32_t rd3 = op & 7u;
    switch (cls) {
    case NDS_THUMB_SHIFT: case NDS_THUMB_ADD_SUB: case NDS_THUMB_LDST_REG:
    case NDS_THUMB_LDST_SIGN: case NDS_THUMB_LDST_IMM: case NDS_THUMB_LDST_HALF:
        return 1u << rd3;
    case NDS_THUMB_IMM8: case NDS_THUMB_LDR_PC: case NDS_THUMB_LDST_SP: case NDS_THUMB_ADD_PC_SP:
        return 1u << ((op >> 8) & 7u);
    case NDS_THUMB_ALU: {
        uint32_t aop = (op >> 6) & 15u;
        return (aop == 8 || aop == 10 || aop == 11) ? 0u : (1u << rd3);
    }
    case NDS_THUMB_HI_REG: {
        uint32_t hop = (op >> 8) & 3u;
        if (hop == 3) return (op & 0x80u) ? kCallerSaved : 0u; // BLX, BX
        return (hop == 0 || hop == 2) ? (1u << ((op & 7u) | ((op >> 4) & 8u))) : 0u;
    }
    case NDS_THUMB_ADD_SP: return 1u << 13;
    case NDS_THUMB_PUSH_POP: return (1u << 13) | ((op & (1u << 11)) ? (op & 0xFFu) : 0u);
    case NDS_THUMB_LDM_STM: return (1u << ((op >> 8) & 7u)) | ((op & (1u << 11)) ? (op & 0xFFu) : 0u);
    case NDS_THUMB_BL: return ((op & 0xF800u) == 0xF000u) ? (1u << 14) : kCallerSaved;
    case NDS_THUMB_BCC: case NDS_THUMB_B: return 0;
    default: return 0xFFFFu;
    }
}

} // namespace

Analyzer::Analyzer(const CodeImage &image_, AnalyzerOptions options_) : image(image_), options(std::move(options_))
{
    // Sorted, with ranges that touch or overlap made one, so that is_data()
    // need only look at the range that starts last at or before an address.
    std::sort(options.data_ranges.begin(), options.data_ranges.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (const auto &range : options.data_ranges) {
        if (range.second <= range.first) continue;
        if (!merged.empty() && range.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, range.second);
        } else {
            merged.push_back(range);
        }
    }
    options.data_ranges = std::move(merged);
    for (uint32_t word : options.data_words) data_words.insert(word & ~3u);
}

bool Analyzer::is_data(uint32_t addr, uint32_t size) const
{
    for (uint32_t a = addr & ~3u; a < addr + size; a += 4) {
        if (data_words.count(a)) return true;
    }
    // The last range that starts at or before the address.
    auto it = std::upper_bound(options.data_ranges.begin(), options.data_ranges.end(),
                               std::make_pair(addr, 0xFFFFFFFFu));
    if (it != options.data_ranges.begin() && addr < std::prev(it)->second) return true;
    return it != options.data_ranges.end() && it->first < addr + size;
}

void Analyzer::push(uint32_t key)
{
    if (!insns.count(key)) worklist_.push_back(key);
}

bool Analyzer::add_function(uint32_t key)
{
    const uint32_t size = key_thumb(key) ? 2u : 4u;
    if (!image.contains(key_addr(key), size) || is_data(key_addr(key), size)) return false;
    if (!key_thumb(key) && (key_addr(key) & 3u)) return false; // misaligned ARM target
    labels.insert(key);
    push(key);
    return function_entries.insert(key).second;
}

// The target of a call: a function here, or something for another image.
void Analyzer::call_target(uint32_t key)
{
    if (image.contains(key_addr(key), key_thumb(key) ? 2u : 4u)) add_function(key);
    else external_targets.insert(key);
}

// The target of a branch. One that changes instruction set, or leaves the
// image, is a function of its own: a function here is all ARM or all Thumb.
void Analyzer::branch_target(uint32_t key, bool from_thumb)
{
    if (!image.contains(key_addr(key), key_thumb(key) ? 2u : 4u)) {
        external_targets.insert(key);
    } else if (key_thumb(key) != from_thumb) {
        add_function(key);
    } else {
        labels.insert(key);
        push(key);
    }
}

void Analyzer::drain()
{
    while (!worklist_.empty()) {
        uint32_t key = worklist_.back();
        worklist_.pop_back();
        explore(key);
    }
}

void Analyzer::seed()
{
    for (uint32_t key : options.entries) add_function(key);
    drain();
}

bool Analyzer::add_entry(uint32_t key)
{
    bool added = add_function(key);
    drain();
    return added;
}

bool Analyzer::looks_like_function(uint32_t value) const
{
    const bool thumb = (value & 1u) != 0;
    const uint32_t target = value & ~1u;
    if (!image.contains(target, 4) || is_data(target, thumb ? 2u : 4u)) return false;
    if (thumb) return (image.read16(target) & 0xFF00u) == 0xB500u; // push {.., lr}
    if (target & 3u) return false;
    const uint32_t op = image.read32(target);
    // stmfd sp!, {.., lr}, or str lr, [sp, #-4]!
    return (op & 0xFFFF4000u) == 0xE92D4000u || op == 0xE52DE004u;
}

bool Analyzer::looks_like_leaf(uint32_t value) const
{
    const bool thumb = (value & 1u) != 0;
    const uint32_t target = value & ~1u;
    const uint32_t size = thumb ? 2u : 4u;
    if (!thumb && (target & 3u)) return false;
    if (!image.contains(target, size) || is_data(target, size)) return false;

    // What is before it must end there: an instruction that never goes on
    // to the next one, a literal pool or a jump table, or the padding that
    // aligns a function after one of those.
    auto ends = [&](uint32_t addr, bool t) {
        const auto it = insns.find(make_key(addr, t));
        if (it == insns.end() || it->second.conditional) return false;
        const Flow flow = it->second.flow;
        return flow == Flow::Return || flow == Flow::Branch || flow == Flow::Indirect || flow == Flow::Table ||
               flow == Flow::Stop;
    };
    const bool aligned = !(target & 3u) && target >= image.base + 4u;
    const bool boundary =
        target == image.base || (target >= image.base + 4u && data_words.count((target - 1u) & ~3u) != 0) ||
        (target >= image.base + 2u && ends(target - 2u, true)) || (aligned && ends(target - 4u, false)) ||
        // A Thumb function, then two bytes of padding.
        (aligned && image.read16(target - 2u) == 0 && ends(target - 4u, true));
    if (!boundary) return false;

    // What is at it: no stack frame, nothing undefined, no call (the return
    // address would be lost), and soon a return or a jump. Data read as
    // code rarely gets that far: a pointer, a small number or text is a
    // conditional ARM instruction, so the first one must not be.
    const int v5 = image.v5();
    uint32_t addr = target;
    for (int i = 0; i < 24; i++, addr += size) {
        if (!image.contains(addr, size) || is_data(addr, size)) return false;
        if (thumb) {
            const uint16_t op = image.read16(addr);
            if (i == 0 && op == 0) return false;
            const NdsThumbClass cls = nds_thumb_classify(op, v5);
            if (cls == NDS_THUMB_UNDEF || cls == NDS_THUMB_BKPT || cls == NDS_THUMB_BL ||
                cls == NDS_THUMB_PUSH_POP) {
                return false;
            }
            if (cls == NDS_THUMB_B) return true;
            if (cls == NDS_THUMB_HI_REG && (op & 0xFF00u) == 0x4700u) return (op & 0x80u) == 0; // bx, not blx
        } else {
            const uint32_t op = image.read32(addr);
            const uint32_t cond = op >> 28;
            if (cond == NDS_COND_NV || (i == 0 && cond != NDS_COND_AL)) return false;
            const NdsArmClass cls = nds_arm_classify(op, v5);
            if (cls == NDS_ARM_UNDEF || cls == NDS_ARM_BKPT || cls == NDS_ARM_BLX_REG) return false;
            if (cls == NDS_ARM_B) {
                if (op & (1u << 24)) return false;
                if (cond == NDS_COND_AL) return true;
            } else if (cls == NDS_ARM_BX || (arm_dests(op, cls) & 0x8000u)) {
                if (cond == NDS_COND_AL) return true;
            }
        }
    }
    return false;
}

void Analyzer::finish()
{
    std::vector<uint32_t> keys;
    for (const TraceKey &trace : options.trace_keys) {
        const uint32_t key = trace.key;
        const uint32_t size = key_thumb(key) ? 2u : 4u;
        if (!image.contains(key_addr(key), size) || is_data(key_addr(key), size)) continue;
        if (!key_thumb(key) && (key_addr(key) & 3u)) continue;
        // Other code than the run saw: another overlay's, at this address.
        if (trace.has_opcode &&
            trace.opcode != (key_thumb(key) ? image.read16(key_addr(key)) : image.read32(key_addr(key)))) {
            continue;
        }
        keys.push_back(key);
        labels.insert(key);
        push(key);
    }
    drain();
    build_functions();

    // Trace keys that no function reaches start functions of their own. They
    // are taken in address order, each covering what it reaches, so a run of
    // code only the trace knows becomes one function instead of one function
    // per traced instruction.
    std::set<uint32_t> covered;
    for (const Function &fn : functions) covered.insert(fn.insns.begin(), fn.insns.end());
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
    const int v5 = image.v5();
    uint32_t addr = key_addr(start);
    Regs regs;
    bool prev_mov_lr_pc = false; // ARM "mov lr, pc" just before

    for (;;) {
        uint32_t key = make_key(addr, thumb);
        if (insns.count(key)) return;
        if (!image.contains(addr, size) || is_data(addr, size)) return;

        Insn in;
        in.addr = addr;
        in.thumb = thumb;
        in.size = static_cast<uint8_t>(size);
        in.op = thumb ? image.read16(addr) : image.read32(addr);
        const uint32_t next = make_key(addr + size, thumb);
        bool stop = false;

        if (!thumb) {
            const uint32_t op = in.op;
            const uint32_t cond = op >> 28;
            const NdsArmClass cls = nds_arm_classify(op, v5);
            const uint32_t rn = (op >> 16) & 15u, rd = (op >> 12) & 15u, rm = op & 15u;
            // On the ARM9 condition 1111 is not a condition: it selects the
            // unconditional instructions.
            const bool never = cond == NDS_COND_NV && !v5;
            in.conditional = cond != NDS_COND_AL && !(v5 && cond == NDS_COND_NV);
            uint32_t written = arm_dests(op, cls);
            bool handled = false;

            if (never) {
                // Never executes on the ARM7TDMI.
                written = 0;
            } else if (cls == NDS_ARM_B) {
                uint32_t target = addr + 8 + static_cast<uint32_t>(static_cast<int32_t>(op << 8) >> 6);
                in.target = make_key(target, false);
                if (op & (1u << 24)) {
                    in.flow = Flow::Call;
                    call_target(in.target);
                } else {
                    in.flow = in.conditional ? Flow::CondBranch : Flow::Branch;
                    branch_target(in.target, false);
                    stop = !in.conditional;
                }
            } else if (cls == NDS_ARM_BLX_IMM) {
                // To Thumb code; bit 24 supplies bit 1 of the target.
                uint32_t target = addr + 8 + static_cast<uint32_t>(static_cast<int32_t>(op << 8) >> 6) +
                                  ((op >> 23) & 2u);
                in.target = make_key(target, true);
                in.flow = Flow::Call;
                call_target(in.target);
            } else if (cls == NDS_ARM_BX || cls == NDS_ARM_BLX_REG) {
                const Value &v = regs.r[rm];
                if (v.kind == Value::Const) call_target(make_key(v.v & ~1u, v.v & 1u));
                if (cls == NDS_ARM_BLX_REG || prev_mov_lr_pc) {
                    in.flow = Flow::CallIndirect;
                } else if (rm == 14) {
                    in.flow = Flow::Return;
                } else {
                    in.flow = Flow::Indirect;
                    indirect_unresolved += v.kind != Value::Const;
                }
                stop = !in.conditional && in.flow != Flow::CallIndirect;
            } else if (cls == NDS_ARM_SWI) {
                labels.insert(next); // the BIOS returns here
            } else if (cls == NDS_ARM_UNDEF || cls == NDS_ARM_BKPT) {
                in.flow = Flow::Stop;
                stop = !in.conditional;
            } else if (cls == NDS_ARM_MSR) {
                if (!(op & (1u << 22))) {
                    in.exits_after = true;
                    labels.insert(next);
                }
            } else if (cls == NDS_ARM_DP) {
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
                    if (count && count <= kMaxTable) {
                        in.flow = Flow::Table;
                        for (uint32_t i = 0; i < count; i++) {
                            uint32_t t = make_key(addr + 8 + 4 * i, false);
                            if (!image.contains(key_addr(t), 4)) break;
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
            } else if (cls == NDS_ARM_SDT) {
                const bool load = (op & (1u << 20)) != 0, pre = (op & (1u << 24)) != 0;
                const bool reg_off = (op & (1u << 25)) != 0, byte = (op & (1u << 22)) != 0;
                const bool wb = (op & (1u << 21)) != 0, up = (op & (1u << 23)) != 0;
                if (load && rn == 15 && pre && !wb && !reg_off && !byte) {
                    uint32_t lit = up ? addr + 8 + (op & 0xFFFu) : addr + 8 - (op & 0xFFFu);
                    if ((lit & 3u) == 0 && image.contains(lit, 4)) {
                        data_words.insert(lit);
                        if (rd != 15) {
                            regs.set_const(static_cast<int>(rd), image.read32(lit));
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
                    const bool table_form = rn == 15 && pre && !wb && reg_off && up && !byte &&
                                            ((op >> 4) & 0xFFu) == 0x10u;
                    if (!count && table_form && !in.conditional) {
                        // The same jump with the index clamped some other
                        // way (a BIOS's software interrupt table): the table
                        // is as long as the words after it are addresses of
                        // code in this image.
                        while (count < kMaxTable && image.contains(addr + 8 + 4 * count, 4)) {
                            uint32_t word = image.read32(addr + 8 + 4 * count);
                            uint32_t target = (v5 && (word & 1u)) ? (word & ~1u) : word;
                            if ((!(v5 && (word & 1u)) && (word & 3u)) || !image.contains(target, 4)) break;
                            count++;
                        }
                    }
                    if (count && count <= kMaxTable) {
                        in.flow = Flow::Table;
                        for (uint32_t i = 0; i < count; i++) {
                            uint32_t slot = addr + 8 + 4 * i;
                            if (!image.contains(slot, 4)) break;
                            data_words.insert(slot);
                            // The ARM9 takes the instruction set from bit 0
                            // of a loaded PC; the ARM7 stays in ARM state.
                            uint32_t word = image.read32(slot);
                            uint32_t t = (v5 && (word & 1u)) ? make_key(word, true) : make_key(word & ~3u, false);
                            in.table.push_back(t);
                            branch_target(t, false);
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
            } else if (cls == NDS_ARM_BLOCK) {
                if ((op & (1u << 20)) && (op & 0x8000u)) {
                    in.flow = Flow::Return;
                    stop = !in.conditional;
                }
            } else if (cls != NDS_ARM_MRS && cls != NDS_ARM_PLD) {
                // Multiplies, swaps, halfword and doubleword transfers and
                // the ARMv5 arithmetic: a branch only if they write R15.
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
            const NdsThumbClass cls = nds_thumb_classify(op, v5);
            uint32_t written = thumb_dests(op, cls);
            bool handled = false;

            switch (cls) {
            case NDS_THUMB_B: {
                uint32_t target = addr + 4 + static_cast<uint32_t>(static_cast<int32_t>(static_cast<uint32_t>(op) << 21) >> 20);
                in.flow = Flow::Branch;
                in.target = make_key(target, true);
                branch_target(in.target, true);
                stop = true;
                break;
            }
            case NDS_THUMB_BCC: {
                // Condition 14 reaches here on the ARM7 only, where it
                // branches always.
                uint32_t cond = (op >> 8) & 15u;
                uint32_t target = addr + 4 + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(op & 0xFFu)) * 2);
                in.target = make_key(target, true);
                in.flow = cond == 14 ? Flow::Branch : Flow::CondBranch;
                branch_target(in.target, true);
                regs.bound_from_branch_away(cond);
                stop = cond == 14;
                break;
            }
            case NDS_THUMB_BL:
                if ((op & 0xF800u) == 0xF000u && image.contains(addr + 2, 2)) {
                    uint16_t suffix = image.read16(addr + 2);
                    const bool bl = (suffix & 0xF800u) == 0xF800u;
                    // The BLX suffix, to ARM code: ARMv5 only, and bit 0 of
                    // the offset must be clear.
                    const bool blx = v5 && (suffix & 0xF801u) == 0xE800u;
                    if (bl || blx) {
                        int32_t hi = static_cast<int32_t>(static_cast<uint32_t>(op & 0x7FFu) << 21) >> 9;
                        uint32_t target = addr + 4 + static_cast<uint32_t>(hi) + ((suffix & 0x7FFu) << 1);
                        uint32_t target_key = bl ? make_key(target, true) : make_key(target & ~3u, false);
                        call_target(target_key);
                        insns[key] = in; // the prefix itself falls through
                        Insn sfx;
                        sfx.addr = addr + 2;
                        sfx.op = suffix;
                        sfx.thumb = true;
                        sfx.size = 2;
                        sfx.flow = Flow::Call;
                        sfx.target = target_key;
                        uint32_t sfx_key = make_key(addr + 2, true);
                        if (!insns.count(sfx_key)) insns[sfx_key] = sfx;
                        labels.insert(make_key(addr + 4, true));
                        regs.clobber(kCallerSaved);
                        addr += 4;
                        continue;
                    }
                }
                if ((op & 0xF800u) != 0xF000u) {
                    // A suffix on its own calls LR + offset, target unknown here.
                    in.flow = Flow::CallIndirect;
                    labels.insert(next);
                }
                break;
            case NDS_THUMB_HI_REG: {
                uint32_t hop = (op >> 8) & 3u;
                uint32_t rd = (op & 7u) | ((op >> 4) & 8u);
                uint32_t rs = (op >> 3) & 15u;
                const Value &v = regs.r[rs];
                if (hop == 3 && v5 && (op & 0x80u)) { // BLX
                    if (v.kind == Value::Const) call_target(make_key(v.v & ~1u, v.v & 1u));
                    in.flow = Flow::CallIndirect;
                    labels.insert(next);
                } else if (hop == 3) { // BX
                    if (rs == 15) {
                        // bx pc: to the ARM code at the next word boundary.
                        in.flow = Flow::Branch;
                        in.target = make_key((addr + 4) & ~3u, false);
                        branch_target(in.target, true);
                    } else {
                        if (v.kind == Value::Const) call_target(make_key(v.v & ~1u, v.v & 1u));
                        in.flow = rs == 14 ? Flow::Return : Flow::Indirect;
                        indirect_unresolved += in.flow == Flow::Indirect && v.kind != Value::Const;
                    }
                    stop = true;
                } else if (rd == 15 && hop != 1) {
                    if (hop == 2 && v.kind == Value::TableVal && v.count && v.count <= kMaxTable) {
                        in.flow = Flow::Table;
                        for (uint32_t i = 0; i < v.count; i++) {
                            uint32_t slot = v.v + 4 * i;
                            if (!image.contains(slot, 4)) break;
                            data_words.insert(slot);
                            uint32_t t = make_key(image.read32(slot) & ~1u, true);
                            in.table.push_back(t);
                            branch_target(t, true);
                        }
                        tables_found++;
                    } else if (hop == 2 && v.kind == Value::Const) {
                        in.flow = Flow::Branch;
                        in.target = make_key(v.v & ~1u, true);
                        branch_target(in.target, true);
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
            case NDS_THUMB_PUSH_POP:
                if ((op & (1u << 11)) && (op & 0x100u)) {
                    in.flow = Flow::Return;
                    stop = true;
                }
                break;
            case NDS_THUMB_IMM8: {
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
            case NDS_THUMB_LDR_PC: {
                uint32_t rd = (op >> 8) & 7u;
                uint32_t lit = ((addr + 4) & ~3u) + ((op & 0xFFu) << 2);
                if (image.contains(lit, 4)) {
                    data_words.insert(lit);
                    regs.set_const(static_cast<int>(rd), image.read32(lit));
                    handled = true;
                }
                break;
            }
            case NDS_THUMB_ADD_PC_SP:
                if (!(op & (1u << 11))) {
                    regs.set_const(static_cast<int>((op >> 8) & 7u), ((addr + 4) & ~3u) + ((op & 0xFFu) << 2));
                    handled = true;
                }
                break;
            case NDS_THUMB_SHIFT: {
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
            case NDS_THUMB_ADD_SUB: {
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
            case NDS_THUMB_LDST_IMM: {
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
            case NDS_THUMB_LDST_REG: {
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
            case NDS_THUMB_SWI:
                labels.insert(next);
                break;
            case NDS_THUMB_UNDEF:
            case NDS_THUMB_BKPT:
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

// The instructions a function starting at `entry` consists of, in address
// order: everything reachable from it without entering another function.
std::vector<uint32_t> Analyzer::reach(uint32_t entry) const
{
    std::set<uint32_t> seen;
    std::vector<uint32_t> stack{ entry };
    auto follow = [&](uint32_t key) {
        if (!insns.count(key) || seen.count(key)) return;
        // A branch to another function's entry is a tail call: leave
        // through the run loop instead of duplicating that function. So is
        // one into the other instruction set.
        if (key != entry && function_entries.count(key)) return;
        if (key_thumb(key) != key_thumb(entry)) return;
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
            if (in.conditional) follow(next);
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

} // namespace ndsrecomp
