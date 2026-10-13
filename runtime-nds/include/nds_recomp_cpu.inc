/*
 * What generated code for one DS processor is written in terms of: that
 * processor's instruction handlers and the helpers below. A template like
 * nds_cpu_impl.inc; nds9_recomp.h includes it with NDS_ARCH 5 and
 * nds7_recomp.h with NDS_ARCH 4.
 *
 * Every instruction the recompiler emits has the same shape:
 *
 *     if (nds9_rc_step(m, <address + 8 or + 4>)) return;
 *     nds9_arm_dp(c, 0xE0810002u);              (the handler, opcode constant)
 *     if (c->branched) return;                  (if the instruction can write R15)
 *
 * rc_step stands where the interpreter's loop comes round between two
 * instructions. If the loop would not go straight on to this one (the slice
 * is over, the CPU went to sleep or is held, an interrupt can be taken) it
 * says so, with R15 already holding what the run loop needs to carry on from
 * here, and the function returns. Otherwise it does what the interpreter's
 * step does before it executes: R15 gets the pipeline value and the opcode
 * fetch at that address is charged. So native code stops exactly where the
 * interpreter would.
 *
 * Generated projects define NDS_RC_OUTLINE_HANDLERS (ndsrecomp's default):
 * the handlers and these helpers are then one out-of-line copy per generated
 * file instead of being folded into every instruction, which is most of the
 * C compiler's work on a generated game.
 */
#ifndef NDS_ARCH
#error "include nds9_recomp.h or nds7_recomp.h, not this file"
#endif

#include "nds_cpu_arch.inc"

#ifndef NDS_RC_HELPER
#if defined(NDS_RC_OUTLINE_HANDLERS) && defined(_MSC_VER)
#define NDS_RC_HELPER static __declspec(noinline)
#elif defined(NDS_RC_OUTLINE_HANDLERS)
#define NDS_RC_HELPER static __attribute__((noinline, unused))
#else
#define NDS_RC_HELPER static inline
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if NDS_V5
#define NDS_RC_CPU(m) (&(m)->arm9)
#define NDS_RC_INDEX NDS_ARM9
#define NDS_RC_CODE_CYCLES(m, addr) nds9_code_cycles((m), (addr))
#else
#define NDS_RC_CPU(m) (&(m)->arm7)
#define NDS_RC_INDEX NDS_ARM7
#define NDS_RC_CODE_CYCLES(m, addr) NDS7_CYCLES_ACCESS
#endif

/* Between two instructions. r15 is the next one's address + 8 (ARM) or + 4
 * (Thumb): the value R15 has while it executes, which is also where the
 * interpreter's step fetches from as it starts, and what R15 must hold for
 * the run loop to carry on from that instruction. Returns true if the run
 * loop has to act first. */
NDS_RC_HELPER bool NDS_FN(rc_step)(NdsMachine *m, uint32_t r15)
{
    NdsCpu *c = NDS_RC_CPU(m);
    c->r[15] = r15;
    if (c->cycles >= m->cpu_limit[NDS_RC_INDEX] ||
        ((m->halted[NDS_RC_INDEX] | m->stalled[NDS_RC_INDEX] | m->irq_line[NDS_RC_INDEX]) &&
         (m->halted[NDS_RC_INDEX] || m->stalled[NDS_RC_INDEX] || !(c->cpsr & NDS_CPSR_I)))) {
        return true;
    }
    c->cycles += NDS_RC_CODE_CYCLES(m, r15);
    m->instructions[NDS_RC_INDEX]++;
    return false;
}

/* A branch taken inside a function: the two fetches at the target that
 * refill the pipeline. size is 4 (ARM) or 2 (Thumb). */
NDS_RC_HELPER void NDS_FN(rc_refill)(NdsMachine *m, uint32_t target, uint32_t size)
{
    NdsCpu *c = NDS_RC_CPU(m);
    c->branched = false;
    c->cycles += NDS_RC_CODE_CYCLES(m, target);
    c->cycles += NDS_RC_CODE_CYCLES(m, target + size);
}

#undef NDS_RC_CPU
#undef NDS_RC_INDEX
#undef NDS_RC_CODE_CYCLES

#ifdef __cplusplus
}
#endif

#include "nds_cpu_arch_end.inc"
