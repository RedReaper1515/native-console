/*
 * VR4300 interpreter, exceptions, coprocessor 0 and the TLB.
 *
 * The instruction semantics are in n64_cpu_impl.h and n64_fpu_impl.h; this
 * file sequences them (the delay slot), raises exceptions, and keeps the
 * system control state. Exception entry, the COP0 register layout, address
 * translation and the TLB follow ares (n64/cpu/exceptions.cpp, context.cpp,
 * memory.cpp, tlb.cpp, interpreter-scc.cpp, ISC). Ported from ares
 * (https://github.com/ares-emulator/ares). Copyright (c) 2004-2025 ares
 * team, Near et al. Permission to use, copy, modify, and/or distribute this
 * software for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear in all
 * copies.
 *
 * Two things differ from ares on purpose, both for determinism: Random
 * counts down once per instruction instead of being a host random number,
 * and interrupts are taken only at sync points (n64_cpu_step), which both
 * the interpreter and generated code honour.
 */
#include "n64_cpu_impl.h"

#include <string.h>

#define N64_OP_FLAGS(name, flags) flags,
const uint16_t n64_op_flags[N64_OP_COUNT] = { N64_OPS(N64_OP_FLAGS) };
#undef N64_OP_FLAGS
#define N64_OP_NAME(name, flags) #name,
const char *const n64_op_names[N64_OP_COUNT] = { N64_OPS(N64_OP_NAME) };
#undef N64_OP_NAME

void n64_fpu_set_host_round(uint32_t mode)
{
    static const int modes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
    fesetround(modes[mode & 3u]);
}

void n64_cpu_update_mode(N64Cpu *c)
{
    const uint32_t sr = c->cp0_status;
    unsigned ksu = (sr >> 3) & 3u;
    if (sr & (N64_SR_EXL | N64_SR_ERL)) ksu = 0;
    c->kernel = ksu == 0;
    if (ksu == 0) c->addr64 = (sr & N64_SR_KX) != 0;
    else if (ksu == 1) c->addr64 = (sr & N64_SR_SX) != 0;
    else c->addr64 = (sr & N64_SR_UX) != 0;
}

void n64_cpu_reset(N64Cpu *c)
{
    N64CpuLinks links = c->links;
    memset(c, 0, sizeof *c);
    c->links = links;
    c->gpr[29] = 0xFFFFFFFFA4001FF0ull;
    c->pc = 0xFFFFFFFFBFC00000ull;
    c->cp0_status = N64_SR_ERL | N64_SR_BEV;
    c->cp0_config = 0x7006E463u;
    for (int i = 0; i < 32; i++) {
        c->tlb[i].vmask = ~(uint64_t)0x1FFFu & 0xFFFFFFFFFFull;
        c->tlb[i].page_off = 0xFFFu;
        c->tlb[i].odd_bit = 0x1000u;
    }
    c->next_sync = 0;
    n64_cpu_update_mode(c);
}

/* ---- exceptions ---- */

static void set_bad_vaddr(N64Cpu *c, uint64_t va)
{
    c->cp0_bad_vaddr = va;
    c->cp0_entry_hi = (c->cp0_entry_hi & 0xFFull) | (va & 0x000000FFFFFFE000ull) | (va & 0xC000000000000000ull);
    c->cp0_context = (c->cp0_context & ~0x7FFFF0ull) | (((va >> 13) & 0x7FFFFull) << 4);
    c->cp0_xcontext = (c->cp0_xcontext & ~0x1FFFFFFFFull) | (((va >> 13) & 0x7FFFFFFull) << 4) | ((va >> 62) << 31);
}

void n64_cpu_exception(N64Cpu *c, uint32_t code, uint32_t ce, bool tlb_miss)
{
    uint64_t base = (c->cp0_status & N64_SR_BEV) ? 0xFFFFFFFFBFC00200ull : 0xFFFFFFFF80000000ull;
    uint64_t offset = 0x180;
    if (tlb_miss && !(c->cp0_status & N64_SR_EXL)) offset = c->addr64 ? 0x080 : 0x000;
    c->cp0_cause = (c->cp0_cause & ~(0x7Cu | 0x30000000u)) | ((code & 31u) << 2) | ((ce & 3u) << 28);
    if (!(c->cp0_status & N64_SR_EXL)) {
        c->cp0_epc = c->pc;
        if (c->in_delay) {
            c->cp0_cause |= N64_CAUSE_BD;
            c->cp0_epc -= 4;
        } else {
            c->cp0_cause &= ~N64_CAUSE_BD;
        }
        c->cp0_status |= N64_SR_EXL;
    }
    c->pc = base + offset;
    c->next_pc = c->pc;
    c->delay_pending = 0;
    c->exc = 1;
    n64_cpu_update_mode(c);
}

void n64_cpu_address_error(N64Cpu *c, uint64_t vaddr, bool store)
{
    set_bad_vaddr(c, vaddr);
    n64_cpu_exception(c, store ? N64_EXC_ADES : N64_EXC_ADEL, 0, false);
}

void n64_cpu_reserved(N64Cpu *c) { n64_cpu_exception(c, N64_EXC_RI, 0, false); }

void n64_cpu_unusable(N64Cpu *c, uint32_t cop) { n64_cpu_exception(c, N64_EXC_CPU, cop, false); }

void n64_cpu_set_irq(N64Cpu *c, uint32_t ip_bit, bool level)
{
    if (level) c->cp0_cause |= ip_bit;
    else c->cp0_cause &= ~ip_bit;
}

bool n64_cpu_poll_interrupt(N64Cpu *c)
{
    const uint32_t sr = c->cp0_status;
    if (!(c->cp0_cause & sr & N64_SR_IM)) return false;
    if (!(sr & N64_SR_IE) || (sr & (N64_SR_EXL | N64_SR_ERL))) return false;
    if (c->delay_pending) return false;
    c->cycles += 1;
    c->in_delay = 0;
    n64_cpu_exception(c, N64_EXC_INT, 0, false);
    c->exc = 0;
    return true;
}

/* ---- Count, Compare, Random ---- */

uint64_t n64_cpu_timer_due(const N64Cpu *c)
{
    const uint64_t now = c->count2 + c->cycles; /* twice the count, plus the half step */
    uint32_t delta = c->cp0_compare - (uint32_t)(now >> 1);
    uint64_t steps = delta ? delta : 0x100000000ull;
    return ((now & ~(uint64_t)1u) + 2u * steps) - c->count2;
}

static uint32_t random_value(const N64Cpu *c)
{
    uint64_t n = c->instructions - c->random_base;
    if (c->cp0_wired > 31u) return (uint32_t)((31u - n) & 63u);
    return 31u - (uint32_t)(n % (32u - c->cp0_wired));
}

/* ---- coprocessor 0 registers ---- */

uint64_t n64_cpu_cop0_read(N64Cpu *c, unsigned reg)
{
    switch (reg & 31u) {
    case 0: return c->cp0_index & 0x8000003Fu;
    case 1: return random_value(c);
    case 2: return c->cp0_entry_lo[0];
    case 3: return c->cp0_entry_lo[1];
    case 4: return c->cp0_context;
    case 5: return c->cp0_page_mask;
    case 6: return c->cp0_wired;
    case 8: return c->cp0_bad_vaddr;
    case 9: return n64_cpu_count(c);
    case 10: return c->cp0_entry_hi;
    case 11: return c->cp0_compare;
    case 12: return c->cp0_status;
    case 13: return c->cp0_cause;
    case 14: return c->cp0_epc;
    case 15: return 0x0B22u;
    case 16: return c->cp0_config;
    case 17: return c->cp0_ll_addr;
    case 18: return c->cp0_watch_lo;
    case 19: return c->cp0_watch_hi;
    case 20: return c->cp0_xcontext;
    case 26: return c->cp0_parity;
    case 27: return 0;
    case 28: return c->cp0_tag_lo;
    case 29: return 0;
    case 30: return c->cp0_error_epc;
    default: return c->cp0_latch;
    }
}

void n64_cpu_cop0_write(N64Cpu *c, unsigned reg, uint64_t v)
{
    c->cp0_latch = v;
    switch (reg & 31u) {
    case 0: c->cp0_index = (uint32_t)v & 0x8000003Fu; break;
    case 2: c->cp0_entry_lo[0] = v & 0x3FFFFFFFu; break;
    case 3: c->cp0_entry_lo[1] = v & 0x3FFFFFFFu; break;
    case 4: c->cp0_context = (c->cp0_context & 0x7FFFF0ull) | (v & ~0x7FFFFFull); break;
    case 5: c->cp0_page_mask = (uint32_t)v & 0x01FFE000u; break;
    case 6:
        c->cp0_wired = (uint32_t)v & 63u;
        c->random_base = c->instructions;
        break;
    case 9:
        c->count2 = ((uint64_t)(uint32_t)v << 1) - c->cycles;
        n64_bus_timer_changed(c);
        break;
    case 10: c->cp0_entry_hi = v & 0xC00000FFFFFFE0FFull; break;
    case 11:
        c->cp0_compare = (uint32_t)v;
        c->cp0_cause &= ~N64_CAUSE_IP_TIMER;
        n64_bus_timer_changed(c);
        break;
    case 12:
        c->cp0_status = (uint32_t)v & N64_SR_WRITABLE;
        n64_cpu_update_mode(c);
        n64_bus_interrupt_check(c);
        break;
    case 13:
        c->cp0_cause = (c->cp0_cause & ~0x300u) | ((uint32_t)v & 0x300u);
        n64_bus_interrupt_check(c);
        break;
    case 14: c->cp0_epc = v; break;
    case 16: c->cp0_config = (c->cp0_config & ~0x0F00800Fu) | ((uint32_t)v & 0x0F00800Fu); break;
    case 17: c->cp0_ll_addr = (uint32_t)v; break;
    case 18: c->cp0_watch_lo = (uint32_t)v & 0xFFFFFFFBu; break;
    case 19: c->cp0_watch_hi = (uint32_t)v & 0xFu; break;
    case 20: c->cp0_xcontext = (c->cp0_xcontext & 0x1FFFFFFF0ull) | (v & ~0x1FFFFFFFFull); break;
    case 26: c->cp0_parity = (uint32_t)v & 0xFFu; break;
    case 28: c->cp0_tag_lo = (uint32_t)v; break;
    case 30: c->cp0_error_epc = v; break;
    default: break;
    }
}

/* ---- the TLB ---- */

static void tlb_write(N64Cpu *c, unsigned index)
{
    if (index >= 32u) return;
    N64TlbEntry *e = &c->tlb[index];
    uint32_t pm = c->cp0_page_mask & (0xAAAu << 13);
    pm |= pm >> 1;
    e->page_mask = pm;
    e->global = (uint8_t)((c->cp0_entry_lo[0] & c->cp0_entry_lo[1]) & 1u);
    e->vmask = ~(uint64_t)(pm | 0x1FFFu) & 0xFFFFFFFFFFull;
    e->page_off = (pm | 0x1FFFu) >> 1;
    e->odd_bit = e->page_off + 1u;
    e->vpn = c->cp0_entry_hi & e->vmask;
    e->region = (uint8_t)(c->cp0_entry_hi >> 62);
    e->asid = (uint8_t)c->cp0_entry_hi;
    e->entry_hi = e->vpn | ((uint64_t)e->region << 62) | e->asid;
    for (int k = 0; k < 2; k++) {
        uint64_t lo = c->cp0_entry_lo[k];
        e->valid[k] = (uint8_t)((lo >> 1) & 1u);
        e->dirty[k] = (uint8_t)((lo >> 2) & 1u);
        e->pfn[k] = (uint32_t)(((lo >> 6) & 0xFFFFFFu) << 12);
        e->entry_lo[k] = (lo & 0x3FFFFFFEull) | e->global;
    }
}

void n64_cpu_tlbr(N64Cpu *c)
{
    unsigned index = c->cp0_index & 63u;
    if (index >= 32u) return;
    const N64TlbEntry *e = &c->tlb[index];
    c->cp0_page_mask = e->page_mask;
    c->cp0_entry_hi = e->entry_hi;
    c->cp0_entry_lo[0] = e->entry_lo[0];
    c->cp0_entry_lo[1] = e->entry_lo[1];
}

void n64_cpu_tlbwi(N64Cpu *c) { tlb_write(c, c->cp0_index & 63u); }

void n64_cpu_tlbwr(N64Cpu *c) { tlb_write(c, random_value(c)); }

void n64_cpu_tlbp(N64Cpu *c)
{
    const uint64_t hi = c->cp0_entry_hi;
    c->cp0_index = 0x80000000u;
    for (unsigned n = 0; n < 32u; n++) {
        const N64TlbEntry *e = &c->tlb[n];
        uint64_t mask = ~(uint64_t)(e->page_mask | 0x1FFFu) & 0xFFFFFFFFFFull;
        if ((e->vpn & mask) != (hi & mask)) continue;
        if (e->region != (uint8_t)(hi >> 62)) continue;
        if (!e->global && e->asid != (uint8_t)hi) continue;
        c->cp0_index = n;
        break;
    }
}

void n64_cpu_eret(N64Cpu *c)
{
    if (c->cp0_status & N64_SR_ERL) {
        c->next_pc = c->cp0_error_epc;
        c->cp0_status &= ~N64_SR_ERL;
    } else {
        c->next_pc = c->cp0_epc;
        c->cp0_status &= ~N64_SR_EXL;
    }
    c->llbit = 0;
    c->delay_pending = 0;
    n64_cpu_update_mode(c);
    n64_bus_interrupt_check(c);
}

/* Which kind of segment an address is in for the current mode. */
enum { SEG_UNUSED, SEG_MAPPED, SEG_DIRECT29, SEG_DIRECT32 };

static int segment_of(const N64Cpu *c, uint64_t va)
{
    const uint32_t sr = c->cp0_status;
    unsigned ksu = (sr & (N64_SR_EXL | N64_SR_ERL)) ? 0u : ((sr >> 3) & 3u);
    if (!c->addr64) {
        uint32_t s = (uint32_t)va >> 29;
        if (ksu == 0) return s < 4 ? SEG_MAPPED : (s < 6 ? SEG_DIRECT29 : SEG_MAPPED);
        if (ksu == 1) return s < 4 || s == 6 ? SEG_MAPPED : SEG_UNUSED;
        return s < 4 ? SEG_MAPPED : SEG_UNUSED;
    }
    if (va <= 0x000000FFFFFFFFFFull) return SEG_MAPPED; /* xkuseg, xsuseg, xuseg */
    if (ksu == 2) return SEG_UNUSED;
    if (va >= 0x4000000000000000ull && va <= 0x400000FFFFFFFFFFull) return SEG_MAPPED; /* xksseg, xsseg */
    if (ksu == 1) {
        if (va >= 0xFFFFFFFFC0000000ull && va <= 0xFFFFFFFFDFFFFFFFull) return SEG_MAPPED; /* csseg */
        return SEG_UNUSED;
    }
    if (va >= 0x8000000000000000ull && va <= 0xBFFFFFFFFFFFFFFFull) {
        /* xkphys: eight cache-attribute windows of 4 GiB each. */
        if ((va & 0x07FFFFFF00000000ull) == 0) return SEG_DIRECT32;
        return SEG_UNUSED;
    }
    if (va >= 0xC000000000000000ull && va <= 0xC00000FF7FFFFFFFull) return SEG_MAPPED; /* xkseg */
    if (va >= 0xFFFFFFFF80000000ull && va <= 0xFFFFFFFFBFFFFFFFull) return SEG_DIRECT29; /* ckseg0, ckseg1 */
    if (va >= 0xFFFFFFFFC0000000ull) return SEG_MAPPED;                                     /* ckseg2, ckseg3 */
    return SEG_UNUSED;
}

static const N64TlbEntry *tlb_match(const N64Cpu *c, uint64_t va)
{
    const uint8_t asid = (uint8_t)c->cp0_entry_hi, region = (uint8_t)(va >> 62);
    for (unsigned n = 0; n < 32u; n++) {
        const N64TlbEntry *e = &c->tlb[n];
        if (!e->global && e->asid != asid) continue;
        if ((va & e->vmask) != e->vpn) continue;
        if (e->region != region) continue;
        return e;
    }
    return NULL;
}

bool n64_cpu_translate(N64Cpu *c, uint64_t va, bool store, uint32_t *paddr)
{
    if (!c->addr64 && (uint64_t)(int64_t)(int32_t)(uint32_t)va != va) {
        n64_cpu_address_error(c, va, store);
        return false;
    }
    switch (segment_of(c, va)) {
    case SEG_DIRECT29: *paddr = (uint32_t)va & 0x1FFFFFFFu; return true;
    case SEG_DIRECT32: *paddr = (uint32_t)va; return true;
    case SEG_MAPPED: {
        const N64TlbEntry *e = tlb_match(c, va);
        if (!e) {
            set_bad_vaddr(c, va);
            n64_cpu_exception(c, store ? N64_EXC_TLBS : N64_EXC_TLBL, 0, true);
            return false;
        }
        int odd = (va & e->odd_bit) != 0;
        if (!e->valid[odd]) {
            set_bad_vaddr(c, va);
            n64_cpu_exception(c, store ? N64_EXC_TLBS : N64_EXC_TLBL, 0, false);
            return false;
        }
        if (store && !e->dirty[odd]) {
            set_bad_vaddr(c, va);
            n64_cpu_exception(c, N64_EXC_MOD, 0, false);
            return false;
        }
        *paddr = e->pfn[odd] + ((uint32_t)va & e->page_off);
        return true;
    }
    default: n64_cpu_address_error(c, va, store); return false;
    }
}

bool n64_cpu_translate_quiet(const N64Cpu *c, uint64_t va, uint32_t *paddr)
{
    if (!c->addr64 && (uint64_t)(int64_t)(int32_t)(uint32_t)va != va) return false;
    switch (segment_of(c, va)) {
    case SEG_DIRECT29: *paddr = (uint32_t)va & 0x1FFFFFFFu; return true;
    case SEG_DIRECT32: *paddr = (uint32_t)va; return true;
    case SEG_MAPPED: {
        const N64TlbEntry *e = tlb_match(c, va);
        if (!e) return false;
        int odd = (va & e->odd_bit) != 0;
        if (!e->valid[odd]) return false;
        *paddr = e->pfn[odd] + ((uint32_t)va & e->page_off);
        return true;
    }
    default: return false;
    }
}

bool n64_cpu_fetch(N64Cpu *c, uint32_t *insn)
{
    const uint64_t pc = c->pc;
    uint32_t pa;
    if (pc & 3u) {
        n64_cpu_address_error(c, pc, false);
        return false;
    }
    if (n64_fast_ram(c, pc, 4, &pa)) {
        *insn = n64_be32(c->links.rdram + pa);
        return true;
    }
    if (!n64_cpu_translate(c, pc, false, &pa)) return false;
    *insn = n64_in_ram(c, pa, 4) ? n64_be32(c->links.rdram + pa) : n64_bus_fetch(c, pa);
    return true;
}

/* ---- the interpreter ---- */

static bool branch_cond(N64Cpu *c, N64Op op, uint32_t i)
{
    switch (op) {
    case N64_OP_beq: case N64_OP_beql: return n64_cond_beq(c, i);
    case N64_OP_bne: case N64_OP_bnel: return n64_cond_bne(c, i);
    case N64_OP_blez: case N64_OP_blezl: return n64_cond_blez(c, i);
    case N64_OP_bgtz: case N64_OP_bgtzl: return n64_cond_bgtz(c, i);
    case N64_OP_bltz: case N64_OP_bltzl: case N64_OP_bltzal: case N64_OP_bltzall: return n64_cond_bltz(c, i);
    case N64_OP_bgez: case N64_OP_bgezl: case N64_OP_bgezal: case N64_OP_bgezall: return n64_cond_bgez(c, i);
    case N64_OP_bc1f: case N64_OP_bc1fl: return n64_cond_bc1(c, false);
    case N64_OP_bc1t: case N64_OP_bc1tl: return n64_cond_bc1(c, true);
    default: return false;
    }
}

/* Every operation with a handler. Branches and jumps have none: their
 * caller sequences them (n64_cond_*, n64_link). */
#define N64_HANDLED_OPS(X) \
    X(invalid) X(sll) X(srl) X(sra) X(sllv) X(srlv) \
    X(srav) X(syscall) X(break) X(sync) X(mfhi) X(mthi) \
    X(mflo) X(mtlo) X(dsllv) X(dsrlv) X(dsrav) X(mult) \
    X(multu) X(div) X(divu) X(dmult) X(dmultu) X(ddiv) \
    X(ddivu) X(add) X(addu) X(sub) X(subu) X(and) \
    X(or) X(xor) X(nor) X(slt) X(sltu) X(dadd) \
    X(daddu) X(dsub) X(dsubu) X(tge) X(tgeu) X(tlt) \
    X(tltu) X(teq) X(tne) X(dsll) X(dsrl) X(dsra) \
    X(dsll32) X(dsrl32) X(dsra32) X(tgei) X(tgeiu) X(tlti) \
    X(tltiu) X(teqi) X(tnei) X(addi) X(addiu) X(slti) \
    X(sltiu) X(andi) X(ori) X(xori) X(lui) X(daddi) \
    X(daddiu) X(ldl) X(ldr) X(lb) X(lh) X(lwl) \
    X(lw) X(lbu) X(lhu) X(lwr) X(lwu) X(sb) \
    X(sh) X(swl) X(sw) X(sdl) X(sdr) X(swr) \
    X(cache) X(ll) X(lld) X(ld) X(sc) X(scd) \
    X(sd) X(lwc1) X(ldc1) X(swc1) X(sdc1) X(mfc0) \
    X(dmfc0) X(mtc0) X(dmtc0) X(cop0_nop) X(tlbr) X(tlbwi) \
    X(tlbwr) X(tlbp) X(eret) X(mfc1) X(dmfc1) X(cfc1) \
    X(mtc1) X(dmtc1) X(ctc1) X(cop1_unimpl) X(fadd_s) X(fsub_s) \
    X(fmul_s) X(fdiv_s) X(fsqrt_s) X(fabs_s) X(fmov_s) X(fneg_s) \
    X(fround_l_s) X(ftrunc_l_s) X(fceil_l_s) X(ffloor_l_s) X(fround_w_s) X(ftrunc_w_s) \
    X(fceil_w_s) X(ffloor_w_s) X(fcvt_d_s) X(fcvt_w_s) X(fcvt_l_s) X(fc_s) \
    X(fadd_d) X(fsub_d) X(fmul_d) X(fdiv_d) X(fsqrt_d) X(fabs_d) \
    X(fmov_d) X(fneg_d) X(fround_l_d) X(ftrunc_l_d) X(fceil_l_d) X(ffloor_l_d) \
    X(fround_w_d) X(ftrunc_w_d) X(fceil_w_d) X(ffloor_w_d) X(fcvt_s_d) X(fcvt_w_d) \
    X(fcvt_l_d) X(fc_d) X(fcvt_s_w) X(fcvt_d_w) X(fcvt_s_l) X(fcvt_d_l) \
    X(mfc2) X(dmfc2) X(cfc2) X(mtc2) X(dmtc2) X(ctc2) \
    X(cop2_invalid)

static void execute(N64Cpu *c, N64Op op, uint32_t i)
{
    switch (op) {
#define N64_DISPATCH(name) \
    case N64_OP_##name: n64_op_##name(c, i); break;
        N64_HANDLED_OPS(N64_DISPATCH)
#undef N64_DISPATCH
    default: break; /* branches and jumps are sequenced by the caller */
    }
}

/* The handlers as ordinary functions. Generated code calls these for the
 * operations whose handlers are large (memory, the FPU, anything that can
 * raise an exception): inlined into every instruction they made a game's
 * build take minutes (n64_recomp.h). */
#define N64_OUT_OF_LINE(name) \
    void n64_ool_##name(N64Cpu *c, uint32_t i) { n64_op_##name(c, i); }
N64_HANDLED_OPS(N64_OUT_OF_LINE)
#undef N64_OUT_OF_LINE

bool n64_cpu_step(N64Cpu *c)
{
    const bool delay = c->delay_pending != 0;
    const uint64_t pc = c->pc;
    uint32_t i;

    c->delay_pending = 0;
    c->in_delay = delay;
    c->cycles += 1;
    if (!n64_cpu_fetch(c, &i)) {
        c->in_delay = 0;
        c->exc = 0;
        return true;
    }
    c->next_pc = delay ? c->delay_target : pc + 4u;
    const N64Op op = n64_decode(i);
    const uint16_t flags = n64_op_flags[op];
    bool sync = delay || (flags & N64_F_SYNC);
    c->cycles += n64_op_extra_cycles(op);
    c->instructions++;
    c->interpreted++;

    if (flags & (N64_F_BRANCH | N64_F_JUMP | N64_F_JUMPR)) {
        uint64_t target;
        bool taken = true;
        if (flags & N64_F_BRANCH) {
            taken = branch_cond(c, op, i);
            target = n64_branch_target(pc, i);
        } else if (flags & N64_F_JUMP) {
            target = n64_jump_target(pc, i);
        } else {
            target = c->gpr[N64_RS(i)];
        }
        if (!c->exc) {
            if (flags & N64_F_LINK) n64_link(c, i, pc);
            if (taken) {
                c->delay_pending = 1;
                c->delay_target = target;
            } else if (flags & N64_F_LIKELY) {
                /* The nullified delay slot still takes its cycle. */
                c->next_pc = pc + 8u;
                c->cycles += 1;
                sync = true;
            } else {
                c->delay_pending = 1;
                c->delay_target = pc + 8u;
            }
        }
    } else {
        execute(c, op, i);
    }
    c->in_delay = 0;
    if (c->exc) {
        c->exc = 0;
        return true;
    }
    c->pc = c->next_pc;
    return c->delay_pending ? false : sync;
}
