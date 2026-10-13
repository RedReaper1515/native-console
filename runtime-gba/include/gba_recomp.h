/*
 * Interface between recompiled code (emitted by gbarecomp) and the runtime.
 *
 * A recompiled program is a table of entry points: guest address (bit 0 set
 * for Thumb) -> native function + resume label. The run loop in gba.c looks
 * the current PC up before every step; on a hit it calls the native
 * function, which runs until it branches out of its known blocks, performs
 * a call, or yields; on a miss it interprets one instruction.
 *
 * Generated code keeps the same cycle accounting as the interpreter (one
 * pipeline fetch per instruction, N+S refill per branch), so the two can be
 * compared in lockstep.
 */
#ifndef GBA_RECOMP_H
#define GBA_RECOMP_H

#include "gba.h"
#include "gba_cpu_decode.h"
#include "gba_cpu_impl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GbaRcFunc)(GbaMachine *m, uint32_t entry);

typedef struct GbaRcEntry {
    uint32_t key;  /* guest address | 1 for Thumb */
    GbaRcFunc fn;
    int32_t block; /* RAM code: index into ram_blocks; -1 for ROM */
} GbaRcEntry;

/* Code that runs from IWRAM/EWRAM was compiled from a snapshot of RAM. Before
 * native code for it runs, the RAM must still hold exactly these bytes. */
typedef struct GbaRcRamBlock {
    uint32_t addr;
    uint32_t size;
    const uint8_t *bytes;
} GbaRcRamBlock;

typedef struct GbaRcProgram {
    const GbaRcEntry *entries; /* sorted by key */
    uint32_t entry_count;
    uint32_t rom_size;
    const char *rom_sha256; /* lowercase hex of the ROM this was built from */
    const char *title;
    const GbaRcRamBlock *ram_blocks;
    uint32_t ram_block_count;
    /* The ROM compiled into the executable, or NULL (see gba_rc_rom.c). */
    const uint8_t *(*embedded_rom)(size_t *size);
} GbaRcProgram;

/* Install a program (NULL removes it). Returns false if the loaded ROM does
 * not match the one the program was built from; the machine then runs
 * interpreted. */
bool gba_rc_install(GbaMachine *m, const GbaRcProgram *program);

/* Table lookup used by the run loop; NULL when the PC has no entry, or when
 * it is RAM code whose bytes no longer match what was compiled. */
GbaRcFunc gba_rc_lookup(GbaMachine *m, uint32_t key);

/* Generated projects define GBA_RC_OUTLINE_HANDLERS (see gba_cpu_impl.h):
 * the fetch and refill helpers below, and the slow path of the yield check,
 * are then one out-of-line copy per generated file instead of being inlined
 * after every instruction. That is most of the C compiler's work on a
 * generated game: Minish Cap builds in 19 s instead of 142 s, and its native
 * code runs about 12% slower. */
#if defined(GBA_RC_OUTLINE_HANDLERS) && defined(_MSC_VER)
#define GBA_RC_HELPER static __declspec(noinline)
#elif defined(GBA_RC_OUTLINE_HANDLERS)
#define GBA_RC_HELPER static __attribute__((noinline, unused))
#else
#define GBA_RC_HELPER static inline
#endif

/* Checked by generated code after every instruction. Due events run in
 * place, exactly where the interpreter's run loop runs them; the result
 * says whether native code must return to the run loop: an enabled
 * interrupt can be taken, the CPU halted, a frame just completed (so the
 * frontend can present it), or the link cable stalled the machine. */
GBA_RC_HELPER bool gba_rc_yield_slow(GbaMachine *m)
{
    if (m->cpu.cycles >= m->next_event) gba_process_events(m);
    return (m->attention & 0xEu) || ((m->attention & 1u) && !(m->cpu.cpsr & GBA_CPSR_I));
}
/* Nothing due and no attention flag set: nothing to do (the common case). */
static inline bool gba_rc_yield(GbaMachine *m)
{
    return (m->cpu.cycles >= m->next_event || m->attention) && gba_rc_yield_slow(m);
}
#define GBA_RC_YIELD(m) gba_rc_yield(m)

/* The pipeline fetch of an instruction (at its address + 8 / + 4). */
GBA_RC_HELPER void gba_rc_fetch(GbaMachine *m, uint32_t addr, bool word)
{
    gba_access_cycles(m, addr, word, GBA_ACCESS_CODE | GBA_ACCESS_SEQ);
}

/* Refill after a branch taken inside a function (the target's two fetches). */
GBA_RC_HELPER void gba_rc_refill(GbaMachine *m, uint32_t target, bool word)
{
    gba_access_cycles(m, target, word, GBA_ACCESS_CODE | GBA_ACCESS_NSEQ);
    gba_access_cycles(m, target + (word ? 4u : 2u), word, GBA_ACCESS_CODE | GBA_ACCESS_SEQ);
}

#ifdef __cplusplus
}
#endif

#endif /* GBA_RECOMP_H */
