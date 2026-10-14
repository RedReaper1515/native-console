#include "analyzer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>

namespace nesrecomp {

Analyzer::Analyzer(const Rom &r, AnalyzerOptions opts) : rom(r), options(std::move(opts)) {}

bool Analyzer::decode(uint32_t key, Insn &in) const
{
    if (!rom.is_code(key)) return false;
    const uint32_t pc = key_pc(key), page = key_page(key);
    // A key in this page, or 0 when the address is outside it.
    auto here = [&](uint32_t addr) -> uint32_t {
        return (addr >= 0x8000u && addr <= 0xFFFFu && slot_of(addr) == slot_of(pc)) ? make_key(page, addr) : 0u;
    };
    in = Insn{};
    in.key = key;
    in.op = rom.read(key);
    in.len = static_cast<uint8_t>(nes_insn_length(in.op));
    // Operand bytes past the end of the page belong to whatever is mapped
    // in the next slot: the handler reads them through the bus, but the
    // analyzer cannot.
    in.operand_known = here(pc + in.len - 1u) != 0;
    if (in.operand_known) {
        for (uint32_t i = 1; i < in.len; i++) {
            in.operand |= static_cast<uint16_t>(rom.read(make_key(page, pc + i)) << (8 * (i - 1)));
        }
    }
    const NesOpInfo *info = nes_opinfo(in.op);
    in.flow = info->flow;
    const uint32_t after = pc + in.len;
    switch (in.flow) {
    case NES_FLOW_NEXT:
        in.next_addr = static_cast<uint16_t>(after);
        in.next = here(after);
        in.exits = in.next == 0; // runs off the end of its page
        break;
    case NES_FLOW_BRANCH_COND:
        in.next_addr = static_cast<uint16_t>(after);
        in.next = here(after);
        if (in.operand_known) {
            in.target_addr = static_cast<uint16_t>(after + static_cast<uint32_t>(static_cast<int8_t>(in.operand)));
            in.target = here(in.target_addr);
        }
        break;
    case NES_FLOW_JUMP:
        if (in.operand_known) {
            in.target_addr = in.operand;
            in.target = here(in.target_addr);
        }
        in.exits = in.target == 0;
        break;
    case NES_FLOW_CALL:
        // Leaves for the run loop; the code after the call is a resume
        // label in the same function.
        if (in.operand_known) in.target_addr = in.operand;
        in.next_addr = static_cast<uint16_t>(after);
        in.next = here(after);
        in.exits = true;
        break;
    case NES_FLOW_BREAK:
        // The handler returns (RTI) to after the byte that follows BRK.
        in.next_addr = static_cast<uint16_t>(after);
        in.next = here(after);
        in.exits = true;
        break;
    default: // JUMP_INDIRECT, RETURN, JAM
        in.exits = true;
        break;
    }
    return true;
}

void Analyzer::add_function(uint32_t key)
{
    if (!key || !rom.is_code(key)) return;
    function_entries.insert(key);
    labels.insert(key);
}

void Analyzer::add_label(uint32_t key)
{
    if (key && rom.is_code(key)) labels.insert(key);
}

// What game code practically never contains, and data decoded as code often
// does: BRK, the JAMs and the other unofficial opcodes, and a jump, call or
// jump vector in $0800-$5FFF (RAM mirrors and I/O: nothing executes there).
static bool unlikely(const Insn &in)
{
    if (in.op == 0x00 || !nes_op_official(in.op)) return true;
    const bool transfers = in.flow == NES_FLOW_JUMP || in.flow == NES_FLOW_CALL || in.flow == NES_FLOW_JUMP_INDIRECT;
    return transfers && in.operand_known && in.operand >= 0x0800u && in.operand < 0x6000u;
}

// Jump tables. The 6502 has no indexed jump, so code dispatches through a
// table of addresses in one of two ways:
//
//     LDA lo,X / STA ptr   ...   LDA hi,X / STA ptr+1   ...   JMP (ptr)
//     LDA hi,X / PHA / LDA lo,X / PHA / RTS                  (address - 1)
//
// where lo and hi are either the two bytes of one table of words (hi = lo +
// 1, entries two bytes apart) or two separate tables of low and high bytes.
// The pointer is often filled in one routine and jumped through in another,
// so the two halves are matched across everything accepted so far
// (Analyzer::note_tables, read_tables).

// Does the instruction straddle accepted code: start inside an accepted
// instruction, or cover the first byte of one?
bool Analyzer::overlaps_code(const Insn &in) const
{
    const uint32_t at = rom.offset(in.key);
    if (owner_[at] & 2u) return true;
    for (uint32_t i = 1; i < in.len && in.operand_known; i++) {
        if (owner_[at + i] & 1u) return true;
    }
    return false;
}

// The keys an address outside the page (or a call target) may have. More
// than one candidate means the bank is a guess.
void Analyzer::guess(uint32_t from, uint16_t addr, std::vector<uint32_t> &out) const
{
    bool certain = false;
    for (uint32_t page : rom.pages_at(from, addr, certain)) {
        const uint32_t key = make_key(page, addr);
        if (rom.is_code(key)) out.push_back(key);
    }
}

// Record what a region says about jump tables: an indexed load from ROM
// stored straight into a RAM pointer byte or pushed, and the pointers that
// JMP (ind) goes through.
void Analyzer::note_tables(const std::vector<Insn> &found)
{
    std::map<uint16_t, const Insn *> by_pc;
    for (const Insn &in : found) by_pc[key_pc(in.key)] = &in;
    struct Use {
        uint32_t from;  // key of the load
        uint16_t table; // the ROM address loaded from
        uint16_t where; // the pointer byte stored to, or the push's address
    };
    std::vector<Use> stores, pushes;
    for (const auto &[pc, in] : by_pc) {
        if (in->flow == NES_FLOW_JUMP_INDIRECT && in->operand_known && in->operand < 0x8000u) {
            jump_pointers_.insert(in->operand);
        }
        const bool indexed_load = in->op == 0xBD || in->op == 0xB9; // LDA abs,X / LDA abs,Y
        if (!indexed_load || !in->operand_known || in->operand < 0x8000u) continue;
        const auto next = by_pc.find(static_cast<uint16_t>(pc + 3u));
        if (next == by_pc.end() || !next->second->operand_known) continue;
        const Insn &use = *next->second;
        if ((use.op == 0x85 || use.op == 0x8D) && use.operand < 0x8000u) { // STA zp / STA abs
            stores.push_back({ in->key, in->operand, use.operand });
        } else if (use.op == 0x48) { // PHA
            pushes.push_back({ in->key, in->operand, next->first });
        }
    }
    for (const Use &lo : stores) {
        for (const Use &hi : stores) {
            if (hi.where == static_cast<uint16_t>(lo.where + 1u) && hi.table != lo.table) {
                tables_.push_back({ lo.from, lo.where, lo.table, hi.table, false, false });
            }
        }
    }
    // The high byte is pushed first, the low byte a few bytes later.
    for (size_t i = 0; i + 1 < pushes.size(); i++) {
        const Use &hi = pushes[i], &lo = pushes[i + 1];
        if (lo.where - hi.where <= 12u && hi.table != lo.table) {
            tables_.push_back({ hi.from, 0, lo.table, hi.table, true, false });
        }
    }
}

// Read every table that is known to be jumped through and has not been read:
// one group of candidate keys per entry. A table's length is unknown; read
// while entries point into ROM and the table has not run into code (or, for
// separate low and high tables, into each other).
void Analyzer::read_tables(std::vector<std::vector<uint32_t>> &entries)
{
    for (Table &table : tables_) {
        if (table.read || (!table.minus_one && !jump_pointers_.count(table.pointer))) continue;
        table.read = true;
        bool certain = false;
        const std::vector<uint32_t> lo_pages = rom.pages_at(table.from, table.lo, certain);
        const std::vector<uint32_t> hi_pages = rom.pages_at(table.from, table.hi, certain);
        if (lo_pages.size() != 1 || hi_pages.size() != 1) continue; // the table's own bank is unknown
        const bool split = table.hi != static_cast<uint16_t>(table.lo + 1u);
        const uint32_t step = split ? 1u : 2u;
        uint32_t limit = 256;
        if (split) limit = std::min<uint32_t>(limit, table.lo < table.hi ? table.hi - table.lo : table.lo - table.hi);
        size_t count = 0;
        for (uint32_t i = 0; i < limit; i++) {
            const uint32_t lo_at = table.lo + step * i, hi_at = table.hi + step * i;
            if (lo_at > 0xFFFFu || hi_at > 0xFFFFu) break;
            if (slot_of(lo_at) != slot_of(table.lo) || slot_of(hi_at) != slot_of(table.hi)) break;
            const uint32_t lo = make_key(lo_pages[0], lo_at), hi = make_key(hi_pages[0], hi_at);
            if (owner_[rom.offset(lo)] || owner_[rom.offset(hi)]) break;
            const uint16_t target =
                static_cast<uint16_t>((rom.read(lo) | (rom.read(hi) << 8)) + (table.minus_one ? 1u : 0u));
            if (target < 0x8000u) break;
            std::vector<uint32_t> keys;
            guess(table.from, target, keys);
            if (keys.empty()) break;
            entries.push_back(std::move(keys));
            count++;
        }
        if (count) {
            tables_found++;
            table_entries += count;
        }
    }
}

// Explore the code a root reaches by falling through, branching and jumping
// within its page. Calls, return sites and everything in another page
// become further roots. A guessed root's region is accepted only if it
// looks like code: every instruction decodes, none is BRK or an unofficial
// opcode, none straddles accepted code, and it is not implausibly large. A
// rejected region takes the roots it would have spawned with it, which is
// what keeps data from snowballing into more "code" through the random
// calls it contains.
void Analyzer::explore_region(const Root &root, std::deque<Root> &queue)
{
    if (insns.count(root.key) || rejected_.count(root.key)) return;
    const bool known = root.depth == 0;
    std::vector<Insn> found;
    // Guessed roots: one group per address, all the pages it may be in.
    struct Child {
        std::vector<uint32_t> keys;
        bool callee;
        bool table = false; // a jump-table entry: a guess even with one candidate page
    };
    std::vector<Child> children;
    std::set<uint32_t> seen;
    std::vector<uint32_t> stack{ root.key };
    bool reject = false;
    auto child = [&](uint32_t from, uint16_t addr, bool callee = false) {
        std::vector<uint32_t> keys;
        guess(from, addr, keys);
        if (!keys.empty()) children.push_back({ std::move(keys), callee, false });
    };

    while (!stack.empty() && !reject) {
        const uint32_t key = stack.back();
        stack.pop_back();
        if (!key || insns.count(key) || !seen.insert(key).second) continue;
        Insn in;
        if (!decode(key, in)) {
            reject = !known;
            continue;
        }
        if (!known && (unlikely(in) || overlaps_code(in) || found.size() >= options.max_speculative_region)) {
            reject = true;
            break;
        }
        switch (in.flow) {
        case NES_FLOW_CALL:
            if (in.target_addr) child(key, in.target_addr, true);
            child(key, in.next_addr); // the callee returns: a guess
            break;
        case NES_FLOW_BREAK: child(key, in.next_addr); break;
        case NES_FLOW_BRANCH_COND:
            if (in.target) stack.push_back(in.target);
            else if (in.target_addr) child(key, in.target_addr);
            if (in.next) stack.push_back(in.next);
            else child(key, in.next_addr);
            break;
        case NES_FLOW_JUMP:
            if (in.target) stack.push_back(in.target);
            else if (in.target_addr) child(key, in.target_addr);
            break;
        case NES_FLOW_NEXT:
            if (in.next) stack.push_back(in.next);
            else child(key, in.next_addr);
            break;
        default: break;
        }
        found.push_back(in);
    }
    if (reject) {
        rejected_.insert(root.key);
        regions_rejected++;
        return;
    }

    for (const Insn &in : found) {
        const uint32_t at = rom.offset(in.key);
        owner_[at] |= 1u;
        for (uint32_t i = 1; i < in.len && in.operand_known; i++) owner_[at + i] |= 2u;
        if (in.flow == NES_FLOW_JUMP_INDIRECT) indirect_jumps++;
        if (in.flow != NES_FLOW_CALL && in.flow != NES_FLOW_BREAK && in.exits && (in.next_addr || in.target_addr))
            page_exits++;
        if (in.target) add_label(in.target);
        insns.emplace(in.key, in);
    }
    // Jump tables this region fills or jumps through: every entry is a
    // guessed root.
    if (options.jump_tables) {
        note_tables(found);
        std::vector<std::vector<uint32_t>> entries;
        read_tables(entries);
        for (auto &keys : entries) children.push_back({ std::move(keys), false, true });
    }
    // Everything the run loop can be asked to continue at: callees start
    // functions; return sites and code in other pages are resume labels
    // (run() starts a function at any label no function covers).
    for (const Child &group : children) {
        for (uint32_t key : group.keys) {
            if (group.callee) add_function(key);
            else add_label(key);
        }
    }
    if (known) add_label(root.key);
    for (const Child &group : children) {
        // A single candidate is as sure as the code that names it; several
        // are a guess about the bank, and those are what the depth counts.
        const unsigned depth = (group.keys.size() > 1 || group.table) ? root.depth + 1u : std::max(root.depth, 1u);
        if (depth > options.speculation_depth) continue;
        for (uint32_t key : group.keys) {
            if (!insns.count(key)) queue.push_back({ key, depth });
        }
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

    owner_.assign(rom.info.prg_size, 0);
    std::deque<Root> queue;
    // Known to run: the reset vector under the power-on mapping, and every
    // trace key. Guesses: the NMI and IRQ handlers, and the vectors of every
    // other page that can sit at $E000 (boards that switch all of PRG keep
    // vectors in every bank).
    const uint32_t power_on = rom.power_on_page(3);
    for (uint32_t page : rom.pages_in_slot(3)) {
        for (uint16_t vector : { uint16_t(NES_VEC_RESET), uint16_t(NES_VEC_NMI), uint16_t(NES_VEC_IRQ) }) {
            const uint32_t from = make_key(page, vector);
            const uint16_t target = static_cast<uint16_t>(rom.read(from) | (rom.read(make_key(page, vector + 1u)) << 8));
            if (target < 0x8000u) continue;
            std::vector<uint32_t> keys;
            guess(from, target, keys);
            const uint32_t mapped = slot_of(target) == 3 ? page : rom.power_on_page(slot_of(target));
            for (uint32_t key : keys) {
                add_function(key);
                const bool known = vector == NES_VEC_RESET && page == power_on && key_page(key) == mapped;
                if (known) queue.push_front({ key, 0u });
                else queue.push_back({ key, 1u });
            }
        }
    }
    for (uint32_t key : options.trace_keys) {
        if (!rom.is_code(key)) continue;
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
    // reaches starts one, and so does code cut off by the size cap (the run
    // loop must be able to continue there natively).
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
                for (uint32_t s : { in.next, in.target }) {
                    if (!s || members.count(s) || !insns.count(s)) continue;
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
        std::set<uint32_t> members;
        std::deque<uint32_t> queue{ entry };
        while (!queue.empty() && members.size() < options.max_function_insns) {
            uint32_t key = queue.front();
            queue.pop_front();
            if (members.count(key) || !insns.count(key)) continue;
            if (key != entry && function_entries.count(key)) continue; // another function's entry
            members.insert(key);
            const Insn &in = insns.at(key);
            // next and target are keys in the same page, or 0.
            if (in.next) queue.push_back(in.next);
            if (in.target) queue.push_back(in.target);
        }
        // Address order, so straight-line code falls through.
        fn.insns.assign(members.begin(), members.end());
        functions.push_back(std::move(fn));
    }
}

} // namespace nesrecomp
