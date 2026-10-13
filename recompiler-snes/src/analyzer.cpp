#include "analyzer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>

namespace snesrecomp {

namespace {

constexpr unsigned kNativeModes[4] = { 0u, SNES_MODE_X8, SNES_MODE_M8, SNES_MODE_M8 | SNES_MODE_X8 };

bool valid_mode(unsigned mode) { return mode <= 3u || mode == SNES_MODE_EMULATION; }

// The mode after REP/SEP #imm.
unsigned mode_after_rep_sep(uint8_t op, uint8_t imm, unsigned mode)
{
    if (mode == SNES_MODE_EMULATION) return mode; // M and X stay forced to 1
    if (op == 0xC2) { // REP
        if (imm & 0x20) mode &= ~SNES_MODE_M8;
        if (imm & 0x10) mode &= ~SNES_MODE_X8;
    } else { // SEP
        if (imm & 0x20) mode |= SNES_MODE_M8;
        if (imm & 0x10) mode |= SNES_MODE_X8;
    }
    return mode;
}

} // namespace

Analyzer::Analyzer(const Rom &r, AnalyzerOptions opts) : rom(r), options(std::move(opts)) {}

bool Analyzer::decode(uint32_t key, Insn &in) const
{
    const uint32_t pc = key_pc(key);
    const unsigned mode = key_mode(key);
    if (!valid_mode(mode) || !rom.is_code(pc)) return false;
    in = Insn{};
    in.key = key;
    in.pc = pc;
    in.mode = static_cast<uint8_t>(mode);
    in.op = rom.read(pc);
    in.len = static_cast<uint8_t>(snes_insn_length(in.op, mode));
    for (uint32_t i = 1; i < in.len; i++) {
        uint32_t a = pc_add(pc, i);
        if (!rom.is_code(a)) return false;
        in.operand |= static_cast<uint32_t>(rom.read(a)) << (8 * (i - 1));
    }
    const SnesOpInfo *info = snes_opinfo(in.op);
    in.flow = info->flow;
    const uint32_t after = pc_add(pc, in.len);
    const uint32_t bank = pc & 0xFF0000u;
    switch (in.flow) {
    case SNES_FLOW_NEXT:
        in.next = make_key(after, mode);
        break;
    case SNES_FLOW_MODE_SET:
        in.next = make_key(after, mode_after_rep_sep(in.op, static_cast<uint8_t>(in.operand), mode));
        break;
    case SNES_FLOW_MODE_XCE:
    case SNES_FLOW_MODE_PLP:
    case SNES_FLOW_WAIT:
        // Leave for the dispatcher; the continuation is a resume label
        // (for XCE in each mode it can produce, see explore()).
        in.exits = true;
        in.next = make_key(after, mode);
        break;
    case SNES_FLOW_SOFT_INT:
        // BRK/COP: the handler returns (RTI) to after the signature byte.
        in.exits = true;
        in.next = make_key(after, mode);
        break;
    case SNES_FLOW_BLOCK:
        in.next = make_key(after, mode);
        break;
    case SNES_FLOW_BRANCH_COND:
        in.next = make_key(after, mode);
        in.target = make_key(bank | ((after + static_cast<uint32_t>(static_cast<int8_t>(in.operand))) & 0xFFFFu), mode);
        in.target_in_function = true;
        break;
    case SNES_FLOW_BRANCH:
        if (in.op == 0x80) in.target = make_key(bank | ((after + static_cast<uint32_t>(static_cast<int8_t>(in.operand))) & 0xFFFFu), mode);
        else in.target = make_key(bank | ((after + in.operand) & 0xFFFFu), mode); // BRL
        in.target_in_function = true;
        break;
    case SNES_FLOW_JUMP:
        in.target = make_key(bank | (in.operand & 0xFFFFu), mode);
        in.target_in_function = true;
        break;
    case SNES_FLOW_JUMP_LONG:
        in.target = make_key(in.operand & 0xFFFFFFu, mode);
        in.exits = true;
        break;
    case SNES_FLOW_JUMP_IND:
        in.exits = true;
        break;
    case SNES_FLOW_CALL:
        in.target = make_key(bank | (in.operand & 0xFFFFu), mode);
        in.next = make_key(after, mode);
        in.exits = true;
        break;
    case SNES_FLOW_CALL_LONG:
        in.target = make_key(in.operand & 0xFFFFFFu, mode);
        in.next = make_key(after, mode);
        in.exits = true;
        break;
    case SNES_FLOW_CALL_IND:
        in.next = make_key(after, mode);
        in.exits = true;
        break;
    default: // RETURN, RETURN_LONG, RETURN_INT, STOP
        in.exits = true;
        break;
    }
    return true;
}

std::vector<uint32_t> Analyzer::read_jump_table(const Insn &in) const
{
    // JMP (a,X) / JSR (a,X): 16-bit targets in the program bank. The
    // table's length is unknown; read while entries point at code.
    std::vector<uint32_t> out;
    const uint32_t bank = in.pc & 0xFF0000u;
    const uint32_t base = in.operand & 0xFFFFu;
    for (uint32_t i = 0; i < 128; i++) {
        uint32_t at = bank | ((base + 2u * i) & 0xFFFFu);
        if (!rom.is_code(at)) break;
        uint32_t target = bank | rom.read16(at);
        if ((target & 0xFFFFu) == 0 || (target & 0xFFFFu) == 0xFFFFu || !rom.is_code(target)) break;
        // The table cannot run into the instruction that uses it.
        if (at <= in.pc && in.pc < at + 2u) break;
        out.push_back(make_key(target, in.mode));
    }
    return out;
}

void Analyzer::add_function(uint32_t key)
{
    if (!key || !valid_mode(key_mode(key)) || !rom.is_code(key_pc(key))) return;
    function_entries.insert(key);
    labels.insert(key);
}

void Analyzer::add_label(uint32_t key)
{
    if (!key || !valid_mode(key_mode(key))) return;
    labels.insert(key);
}

// Opcodes compiled game code practically never contains, and data often
// does ($00 is BRK).
static bool unlikely_opcode(uint8_t op) { return op == 0x00 || op == 0x02 || op == 0x42 || op == 0xDB; }

// Does the instruction straddle accepted code: start inside an accepted
// instruction, or cover the first byte of one?
bool Analyzer::overlaps_code(const Insn &in) const
{
    if (owner_[in.pc] & 2u) return true;
    for (uint32_t i = 1; i < in.len; i++) {
        if (owner_[pc_add(in.pc, i)] & 1u) return true;
    }
    return false;
}

// Explore the code a root reaches by falling through, branching and jumping
// within its bank. Calls, long jumps, jump-table entries and continuations
// whose mode is a guess become further roots. A guessed root's region is
// accepted only if it looks like code: every instruction decodes inside
// code, none is BRK/COP/WDM/STP, none straddles accepted code, and it is
// not implausibly large. A rejected region takes the roots it would have
// spawned with it, which is what keeps data from snowballing into more
// "code" through the random calls it contains.
void Analyzer::explore_region(const Root &root, std::deque<Root> &queue)
{
    if (insns.count(root.key) || rejected_.count(root.key)) return;
    const bool known = root.depth == 0;
    std::vector<Insn> found;
    std::vector<uint32_t> children; // guessed roots
    std::set<uint32_t> seen;
    std::vector<uint32_t> stack{ root.key };
    bool reject = false;

    while (!stack.empty() && !reject) {
        const uint32_t key = stack.back();
        stack.pop_back();
        if (!key || insns.count(key) || !seen.insert(key).second) continue;
        Insn in;
        if (!decode(key, in)) {
            reject = !known;
            continue;
        }
        if (!known && (unlikely_opcode(in.op) || overlaps_code(in) || found.size() >= options.max_speculative_region)) {
            reject = true;
            break;
        }
        if (in.flow == SNES_FLOW_JUMP_IND && in.op == 0x7C) in.table = read_jump_table(in);
        else if (in.flow == SNES_FLOW_CALL_IND) in.table = read_jump_table(in);

        switch (in.flow) {
        case SNES_FLOW_CALL:
        case SNES_FLOW_CALL_LONG:
            children.push_back(in.target);
            children.push_back(in.next); // returns in the caller's mode: a guess
            break;
        case SNES_FLOW_CALL_IND:
            children.insert(children.end(), in.table.begin(), in.table.end());
            children.push_back(in.next);
            break;
        case SNES_FLOW_JUMP_LONG: children.push_back(in.target); break;
        case SNES_FLOW_JUMP_IND: children.insert(children.end(), in.table.begin(), in.table.end()); break;
        case SNES_FLOW_MODE_XCE: {
            // After XCE the mode depends on the carry: known after CLC or SEC,
            // else both results are possible.
            const uint32_t after = key_pc(in.next);
            const uint8_t before = rom.read(pc_add(in.pc, 0xFFFFu));
            const unsigned native_mode = in.mode == SNES_MODE_EMULATION ? (SNES_MODE_M8 | SNES_MODE_X8) : in.mode;
            if (before == 0x18) stack.push_back(make_key(after, native_mode));
            else if (before == 0x38) stack.push_back(make_key(after, SNES_MODE_EMULATION));
            else {
                children.push_back(make_key(after, native_mode));
                children.push_back(make_key(after, SNES_MODE_EMULATION));
            }
            break;
        }
        case SNES_FLOW_WAIT: stack.push_back(in.next); break; // the handler's RTI restores the mode
        case SNES_FLOW_MODE_PLP:
        case SNES_FLOW_SOFT_INT: children.push_back(in.next); break;
        case SNES_FLOW_BRANCH_COND:
        case SNES_FLOW_BRANCH:
        case SNES_FLOW_JUMP:
            stack.push_back(in.target);
            stack.push_back(in.next);
            break;
        default: stack.push_back(in.next); break;
        }
        found.push_back(std::move(in));
    }
    if (reject) {
        rejected_.insert(root.key);
        regions_rejected++;
        return;
    }

    for (Insn &in : found) {
        owner_[in.pc] |= 1u;
        for (uint32_t i = 1; i < in.len; i++) owner_[pc_add(in.pc, i)] |= 2u;
        if (!in.table.empty()) tables_found++;
        else if (in.flow == SNES_FLOW_JUMP_IND || in.flow == SNES_FLOW_CALL_IND) indirect_unresolved++;
        // Resume labels and function entries, as the dispatcher needs them.
        switch (in.flow) {
        case SNES_FLOW_CALL:
        case SNES_FLOW_CALL_LONG:
            add_function(in.target);
            add_label(in.next);
            break;
        case SNES_FLOW_CALL_IND:
            for (uint32_t t : in.table) add_function(t);
            add_label(in.next);
            break;
        case SNES_FLOW_JUMP_LONG: add_function(in.target); break;
        case SNES_FLOW_JUMP_IND:
            for (uint32_t t : in.table) add_label(t);
            break;
        case SNES_FLOW_MODE_XCE: {
            const uint32_t after = key_pc(in.next);
            const uint8_t before = rom.read(pc_add(in.pc, 0xFFFFu));
            const unsigned native_mode = in.mode == SNES_MODE_EMULATION ? (SNES_MODE_M8 | SNES_MODE_X8) : in.mode;
            if (before != 0x38) add_label(make_key(after, native_mode));
            if (before != 0x18) add_label(make_key(after, SNES_MODE_EMULATION));
            break;
        }
        case SNES_FLOW_MODE_PLP:
        case SNES_FLOW_WAIT:
        case SNES_FLOW_SOFT_INT: add_label(in.next); break;
        case SNES_FLOW_BRANCH_COND:
        case SNES_FLOW_BRANCH:
        case SNES_FLOW_JUMP: add_label(in.target); break;
        default: break;
        }
        const uint32_t key = in.key;
        insns.emplace(key, std::move(in));
    }
    if (known) add_label(root.key);
    const unsigned depth = root.depth + 1;
    if (depth > options.speculation_depth) return;
    for (uint32_t child : children) {
        if (child && valid_mode(key_mode(child)) && !insns.count(child)) queue.push_back({ child, depth });
    }
}

void Analyzer::run()
{
    const auto started = std::chrono::steady_clock::now();
    auto progress = [&](const char *what) {
        if (!options.verbose) return;
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::fprintf(stderr, "[%7.2fs] %s: %zu instructions, %zu entries, %zu labels, %zu functions, %zu rejected\n", s,
                     what, insns.size(), function_entries.size(), labels.size(), functions.size(), regions_rejected);
    };

    owner_.assign(1u << 24, 0);
    std::deque<Root> queue;
    // Known to run: the reset vector (emulation mode) and every trace key.
    // Guesses: the other vectors (native handlers can be entered with any
    // M/X; the emulation-mode handlers are often unused).
    const struct { uint16_t addr; bool native; } vectors[] = {
        { SNES_VEC_RESET, false }, { SNES_VEC_NMI_E, false }, { SNES_VEC_IRQ_E, false }, { SNES_VEC_COP_E, false },
        { SNES_VEC_NMI_N, true }, { SNES_VEC_IRQ_N, true }, { SNES_VEC_BRK_N, true }, { SNES_VEC_COP_N, true },
    };
    for (const auto &v : vectors) {
        uint16_t target = rom.read16(v.addr);
        if (target < 0x8000u) continue;
        const unsigned depth = v.addr == SNES_VEC_RESET ? 0u : 1u;
        if (v.native) {
            for (unsigned m : kNativeModes) {
                add_function(make_key(target, m));
                queue.push_back({ make_key(target, m), depth });
            }
        } else {
            add_function(make_key(target, SNES_MODE_EMULATION));
            if (depth == 0) queue.push_front({ make_key(target, SNES_MODE_EMULATION), depth });
            else queue.push_back({ make_key(target, SNES_MODE_EMULATION), depth });
        }
    }
    for (uint32_t key : options.trace_keys) {
        if (!valid_mode(key_mode(key)) || !rom.is_code(key_pc(key))) continue;
        add_label(key);
        queue.push_front({ key, 0u });
    }
    // Breadth-first: code known to run is accepted first, then guesses in
    // order of distance from it, so data rarely beats real code to a byte.
    while (!queue.empty()) {
        const Root root = queue.front();
        queue.pop_front();
        explore_region(root, queue);
    }
    owner_.clear();
    owner_.shrink_to_fit();
    progress("explored");
    // Group into functions until every place a function leaves to is a
    // resume label some function covers: a trace entry or label no function
    // reaches starts one, and so does code cut off by the size cap (the
    // dispatcher must be able to continue there natively).
    for (int round = 0; round < 64; round++) {
        build_functions();
        std::set<uint32_t> covered;
        for (const Function &fn : functions) covered.insert(fn.insns.begin(), fn.insns.end());
        bool added = false;
        for (uint32_t key : options.trace_keys) {
            if (insns.count(key) && !covered.count(key) && function_entries.insert(key).second) {
                trace_entries++;
                added = true;
            }
        }
        for (const Function &fn : functions) {
            std::set<uint32_t> members(fn.insns.begin(), fn.insns.end());
            for (uint32_t key : fn.insns) {
                const Insn &in = insns.at(key);
                std::vector<uint32_t> succ;
                if (in.next) succ.push_back(in.next);
                if (in.target && in.target_in_function) succ.push_back(in.target);
                if (in.flow == SNES_FLOW_JUMP_IND) succ.insert(succ.end(), in.table.begin(), in.table.end());
                for (uint32_t s : succ) {
                    if (members.count(s) || !insns.count(s)) continue;
                    labels.insert(s);
                    if (!covered.count(s) && function_entries.insert(s).second) added = true;
                }
            }
        }
        for (uint32_t key : labels) {
            if (insns.count(key) && !covered.count(key) && function_entries.insert(key).second) added = true;
        }
        progress("grouped");
        if (!added) break;
    }
}

void Analyzer::build_functions()
{
    functions.clear();
    for (uint32_t entry : function_entries) {
        if (!insns.count(entry)) continue;
        Function fn;
        fn.entry = entry;
        fn.wram = rom.is_wram(key_pc(entry));
        std::set<uint32_t> members;
        std::deque<uint32_t> queue{ entry };
        while (!queue.empty() && members.size() < options.max_function_insns) {
            uint32_t key = queue.front();
            queue.pop_front();
            if (members.count(key) || !insns.count(key)) continue;
            if (key != entry && function_entries.count(key)) continue; // another function's entry
            // Keep WRAM and ROM code in separate functions (WRAM code is
            // checked against live RAM before it runs).
            if (rom.is_wram(key_pc(key)) != fn.wram) continue;
            members.insert(key);
            const Insn &in = insns.at(key);
            if (in.next) queue.push_back(in.next);
            if (in.target && in.target_in_function) queue.push_back(in.target);
            if (in.flow == SNES_FLOW_JUMP_IND) {
                for (uint32_t t : in.table) queue.push_back(t);
            }
        }
        fn.insns.assign(members.begin(), members.end());
        // Address order (then mode), so straight-line code falls through.
        std::sort(fn.insns.begin(), fn.insns.end(), [](uint32_t a, uint32_t b) {
            return key_pc(a) != key_pc(b) ? key_pc(a) < key_pc(b) : key_mode(a) < key_mode(b);
        });
        functions.push_back(std::move(fn));
    }
}

} // namespace snesrecomp
