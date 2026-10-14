/*
 * Interface between recompiled code (emitted by n64recomp) and the runtime.
 *
 * A recompiled program is a table of entry points keyed by the virtual
 * address the CPU runs them at. N64 code runs from RDRAM, where the game
 * (or the boot) copied it, and the same address can hold different code
 * over time (overlays). So every function carries the bytes it was
 * compiled from, and the dispatcher runs it only while RDRAM holds exactly
 * those bytes at the physical address the entry translates to. A store or
 * a DMA into a granule of RDRAM that holds compiled code marks the
 * functions there unverified (n64_bus_code_written); they are compared
 * again before they next run. Several functions may share an address; the
 * first whose bytes match runs. (NES lesson: key code by its bytes, not
 * only its address; docs/ADDING-A-CONSOLE.md.)
 *
 * Generated code calls the same handlers as the interpreter
 * (n64_cpu_impl.h), charges the same cycles, and returns to the run loop
 * at the same sync points, so the two can run in lockstep
 * (--differential).
 */
#ifndef N64_RECOMP_H
#define N64_RECOMP_H

#include "n64.h"
#include "n64_cpu_impl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*N64RcFunc)(N64Cpu *c, uint32_t entry);

typedef struct N64RcEntry {
    uint32_t vaddr;  /* the 32-bit virtual address (KSEG0 for most code) */
    N64RcFunc fn;
    int32_t block;   /* the function's RAM block (bytes it was compiled from) */
} N64RcEntry;

typedef struct N64RcBlock {
    uint32_t paddr; /* where in RDRAM the function's bytes live */
    uint32_t size;
    const uint8_t *bytes;
} N64RcBlock;

typedef struct N64RcProgram {
    const N64RcEntry *entries;
    uint32_t entry_count;
    const N64RcBlock *blocks;
    uint32_t block_count;
    uint32_t rom_size;
    const char *rom_sha256; /* lowercase hex of the ROM this was built from */
    const char *title;
    /* The ROM compiled into the executable, or NULL. */
    const uint8_t *(*embedded_rom)(size_t *size);
} N64RcProgram;

/* Install a program (NULL removes it). Returns false when the loaded ROM
 * is not the one it was built from; the machine then runs interpreted. */
bool n64_rc_install(N64Machine *m, const N64RcProgram *program);

/* The native function for the code at `pc`, or NULL (not compiled, or its
 * bytes are not in RDRAM). */
N64RcFunc n64_rc_lookup(N64Machine *m, uint64_t pc);

/* Called by generated code at a sync point once c->cycles reached
 * c->next_sync: runs due events in place and says whether native code
 * must return to the run loop (an interrupt can be taken, or a frame is
 * complete). */
bool n64_rc_sync(N64Cpu *c);

#define N64_RC_YIELD(c) ((c)->cycles >= (c)->next_sync && n64_rc_sync(c))

/* The handlers as ordinary functions (n64_cpu.c). Generated code inlines
 * the small ones (n64_op_*, with the instruction word a constant) and calls
 * these for the large ones: memory, the FPU, and anything that can raise an
 * exception. Inlined into every instruction, those made a game's C take
 * minutes to compile. Branches and jumps have no handler. */
#define N64_OOL_DECLARE(name, flags) void n64_ool_##name(N64Cpu *c, uint32_t i);
N64_OPS(N64_OOL_DECLARE)
#undef N64_OOL_DECLARE

/* At the head of an idle loop (n64_is_idle_loop): take the turns that only
 * pass time before the next sync at once, and leave the turn that reaches
 * it to run as written, so the sync happens on the same instruction with
 * the same counts as stepping gives. A turn is two cycles and two
 * instructions. */
static inline uint64_t n64_idle_turns(const N64Cpu *c)
{
    return c->cycles < c->next_sync ? (c->next_sync - c->cycles - 1u) / 2u : 0u;
}

static inline void n64_rc_idle(N64Cpu *c)
{
    const uint64_t n = n64_idle_turns(c);
    c->cycles += 2u * n;
    c->instructions += 2u * n;
    c->idled += 2u * n;
}

#ifdef __cplusplus
}
#endif

#endif /* N64_RECOMP_H */
