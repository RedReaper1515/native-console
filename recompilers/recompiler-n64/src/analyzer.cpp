#include "analyzer.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <string>

namespace n64recomp {

namespace {

bool is_transfer(uint16_t f) { return (f & (N64_F_BRANCH | N64_F_JUMP | N64_F_JUMPR)) != 0; }

// What a game never runs: reserved encodings, and the CPU's absent COP2.
bool implausible(N64Op op)
{
    switch (op) {
    case N64_OP_invalid:
    case N64_OP_cop2_invalid:
    case N64_OP_mfc2:
    case N64_OP_dmfc2:
    case N64_OP_cfc2:
    case N64_OP_mtc2:
    case N64_OP_dmtc2:
    case N64_OP_ctc2: return true;
    default: return false;
    }
}

} // namespace

bool Analyzer::decode(const Root &where, uint32_t vaddr, Insn &out) const
{
    const View &v = image.views[where.view];
    const uint32_t pa = vaddr - where.vp;
    if (vaddr & 3u || !v.contains(pa)) return false;
    if (v.full && !v.present[pa >> 12]) return false; // empty RAM, not a run of NOPs
    out = Insn{};
    out.vaddr = vaddr;
    out.word = v.word(pa);
    out.op = n64_decode(out.word);
    out.flags = n64_op_flags[out.op];
    const uint64_t pc = (uint64_t)(int64_t)(int32_t)vaddr;
    if (out.flags & N64_F_BRANCH) {
        out.target = (uint32_t)n64_branch_target(pc, out.word);
        out.has_target = true;
    } else if (out.flags & N64_F_JUMP) {
        out.target = (uint32_t)n64_jump_target(pc, out.word);
        out.has_target = true;
    }
    out.bad = implausible(out.op);
    return true;
}

// A label is where the dispatcher may enter, so it has to be an instruction
// the function compiles. A branch left to the interpreter (bad) belongs to
// its function without being one.
void Analyzer::add_label(size_t function, uint32_t vaddr)
{
    Function &f = functions[function];
    auto it = f.insns.find(vaddr);
    if (it != f.insns.end() && !it->second.bad) f.labels.insert(vaddr);
}

// A page is snapshotted again whenever anything in it changed, so code that
// shares a page with data, or with another overlay's tail, is seen in many
// views and analyzed once in each. Functions that compile the same
// instructions from the same bytes at the same addresses are one function:
// keep the first and give it the labels of the rest.
void Analyzer::merge_duplicates()
{
    std::map<std::string, size_t> seen;
    std::vector<Function> kept;
    for (Function &fn : functions) {
        const View &v = image.views[fn.view];
        const uint32_t pa = fn.lo - fn.vp, size = fn.hi - fn.lo;
        std::string key(reinterpret_cast<const char *>(&fn.vp), sizeof fn.vp);
        key.append(reinterpret_cast<const char *>(&fn.lo), sizeof fn.lo);
        key.append(reinterpret_cast<const char *>(v.bytes.data() + (pa - v.paddr)), size);
        for (const auto &[a, in] : fn.insns) {
            const uint32_t tag = a | (in.bad ? 1u : 0u);
            key.append(reinterpret_cast<const char *>(&tag), sizeof tag);
        }
        auto [it, fresh] = seen.emplace(std::move(key), kept.size());
        if (fresh) {
            kept.push_back(std::move(fn));
        } else {
            kept[it->second].labels.insert(fn.labels.begin(), fn.labels.end());
            duplicates++;
        }
    }
    functions = std::move(kept);
}

// A page snapshot's code that a whole image already compiled: the image's
// function at this address, at the same mapping, whose bytes the snapshot
// repeats as far as the page reaches. (The page differs from the image
// somewhere else - its data, or another function.) The image's function
// gets the label; the snapshot is not compiled again.
bool Analyzer::in_full_view(const Root &root)
{
    const View &page = image.views[root.view];
    if (page.boot || page.full) return false;
    for (size_t v = 1; v < image.views.size(); v++) {
        const View &full = image.views[v];
        if (!full.full) continue;
        auto it = owner_[v].find(root.vaddr);
        if (it == owner_[v].end()) continue;
        const Function &fn = functions[it->second];
        if (fn.vp != root.vp) continue;
        const uint32_t lo = std::max(fn.lo - fn.vp, page.paddr);
        const uint32_t hi = std::min(fn.hi - fn.vp, page.paddr + (uint32_t)page.bytes.size());
        if (lo >= hi || !std::equal(full.bytes.begin() + lo, full.bytes.begin() + hi, page.bytes.begin() + (lo - page.paddr)))
            continue;
        add_label(it->second, root.vaddr);
        return true;
    }
    return false;
}

void Analyzer::explore(const Root &root, std::vector<Root> &queue)
{
    auto &owner = owner_[root.view];
    if (auto it = owner.find(root.vaddr); it != owner.end()) {
        if (functions[it->second].vp == root.vp) add_label(it->second, root.vaddr);
        return;
    }
    if (in_full_view(root)) {
        covered++;
        return;
    }
    Insn first;
    if (!decode(root, root.vaddr, first) || first.bad) return;

    const size_t index = functions.size();
    functions.push_back(Function{});
    Function &fn = functions.back();
    fn.view = root.view;
    fn.entry = root.vaddr;
    fn.vp = root.vp;
    fn.labels.insert(root.vaddr);

    std::deque<uint32_t> work{ root.vaddr };
    // A function is checked against RDRAM as one range, from its first
    // instruction to its last. A jump far from where it began (a tail call,
    // a shared exit) starts a function of its own, or the range would take
    // in everything between.
    const uint32_t reach = (uint32_t)options.max_function_insns * 4u;
    auto visit = [&](uint32_t a, bool label) {
        if (a - root.vaddr + reach > 2u * reach) {
            queue.push_back(Root{ root.view, a, root.vp });
            return;
        }
        if (auto o = owner.find(a); o != owner.end() && o->second != index) {
            // Another function's code: an exit to the dispatcher, which
            // enters that function here.
            add_label(o->second, a);
            return;
        }
        if (label) fn.labels.insert(a);
        if (!fn.insns.count(a)) work.push_back(a);
    };
    while (!work.empty()) {
        const uint32_t a = work.front();
        work.pop_front();
        if (fn.insns.count(a)) continue;
        if (owner.count(a) && owner[a] != index) continue;
        if (fn.insns.size() >= options.max_function_insns) {
            queue.push_back(Root{ root.view, a, root.vp });
            continue;
        }
        Insn in;
        if (!decode(root, a, in)) continue;
        if (in.bad) {
            // Decoded data: control never gets here natively.
            fn.labels.erase(a);
            continue;
        }
        if (is_transfer(in.flags)) {
            Insn slot;
            const bool slot_ok = decode(root, a + 4u, slot) && !slot.bad &&
                                 !is_transfer(slot.flags) && !(slot.flags & N64_F_EXIT) &&
                                 slot.op != N64_OP_tlbwi && slot.op != N64_OP_tlbwr;
            if (!slot_ok) {
                // The interpreter runs a branch whose delay slot we cannot
                // compile, and whatever follows.
                in.bad = true;
                fn.insns[a] = in;
                owner[a] = index;
                fn.labels.erase(a);
                bad++;
                continue;
            }
            fn.insns[a] = in;
            owner[a] = index;
            fn.insns[a + 4u] = slot; // the delay slot belongs to the branch
            owner[a + 4u] = index;
            if (in.flags & N64_F_BRANCH) {
                visit(in.target, true);
                visit(a + 8u, true);
            } else if (in.flags & N64_F_JUMP) {
                if (in.flags & N64_F_LINK) {
                    const View &v = image.views[root.view];
                    if (v.contains(in.target - root.vp)) queue.push_back(Root{ root.view, in.target, root.vp });
                    visit(a + 8u, true); // the return site
                } else {
                    visit(in.target, true);
                }
            } else if (in.flags & N64_F_LINK) { // JALR
                visit(a + 8u, true);
            }
            continue;
        }
        fn.insns[a] = in;
        owner[a] = index;
        if (in.flags & N64_F_EXIT) {
            // SYSCALL and BREAK handlers return past them.
            if (in.op == N64_OP_syscall || in.op == N64_OP_break) visit(a + 4u, true);
            continue;
        }
        if (in.flags & N64_F_MEM) fn.labels.insert(a); // ERET after a TLB miss returns here
        visit(a + 4u, (in.flags & N64_F_SYNC) != 0);
    }
    if (fn.insns.empty()) {
        functions.pop_back();
        return;
    }
    // A label must be a compiled instruction of this function.
    for (auto it = fn.labels.begin(); it != fn.labels.end();) {
        auto ins = fn.insns.find(*it);
        if (ins == fn.insns.end() || ins->second.bad) it = fn.labels.erase(it);
        else ++it;
    }
    fn.lo = fn.insns.begin()->first;
    fn.hi = fn.insns.rbegin()->first + 4u;
}

void Analyzer::run()
{
    owner_.assign(image.views.size(), {});
    // The boot image and the whole images first, to the end: a page
    // snapshot is then compiled only where they do not already hold its
    // code (in_full_view).
    std::vector<Root> queue, pages;
    const View &boot = image.views[0];
    const uint32_t boot_vp = image.entry_vaddr - (image.entry_vaddr & 0x1FFFFFFFu);
    queue.push_back(Root{ 0, image.entry_vaddr, boot_vp });

    for (const TraceEntry &e : image.trace_entries) {
        const uint32_t vp = e.vaddr - e.paddr;
        bool in_snapshot = false;
        for (size_t v = 1; v < image.views.size(); v++) {
            const View &view = image.views[v];
            if (!view.contains(e.paddr)) continue;
            if (view.full) {
                // Only where code is known to have run from these bytes:
                // the address may have held another overlay when it ran.
                if (!view.ran[e.paddr >> 12]) continue;
                queue.push_back(Root{ (int)v, e.vaddr, vp });
            } else {
                pages.push_back(Root{ (int)v, e.vaddr, vp });
            }
            in_snapshot = true;
        }
        if (!in_snapshot && boot.contains(e.paddr)) queue.push_back(Root{ 0, e.vaddr, vp });
        trace_roots++;
    }
    for (size_t i = 0; i < queue.size(); i++) {
        Root r = queue[i];
        explore(r, queue);
    }
    for (size_t i = 0; i < pages.size(); i++) {
        Root r = pages[i];
        explore(r, pages);
    }
    merge_duplicates();
    for (const Function &f : functions) instructions += f.insns.size();
    if (options.verbose)
        std::fprintf(stderr, "analysis: %zu functions, %zu instructions, %zu views\n", functions.size(), instructions,
                     image.views.size());
}

} // namespace n64recomp
