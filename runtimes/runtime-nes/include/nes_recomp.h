/*
 * Interface between recompiled code (emitted by nesrecomp) and the runtime.
 *
 * A recompiled program is a table of entry points keyed by the CPU address
 * and the 8 KiB PRG ROM page mapped there: the cartridge switches banks,
 * so one address holds different code at different times, and one ROM page
 * can appear at more than one address. The run loop (nes.c) builds the key
 * from the PC and the cartridge's current page table before every step; on
 * a hit it calls the native function, which runs until it leaves its known
 * code (a call, return, indirect jump, or a branch out of its page) or
 * yields; on a miss it interprets one instruction. Code outside PRG ROM
 * (work RAM, PRG RAM) has no key and is always interpreted.
 *
 * Because the key names the bytes, not just the address, an entry can never
 * run against the wrong code: whatever the analyzer guessed about banking
 * only decides how much gets compiled. Native code stays within one 8 KiB
 * page per function, and returns to the run loop when the cartridge changes
 * its PRG mapping (NES_ATTENTION_REMAP), so the next lookup sees the new
 * bank.
 *
 * Generated code performs the same bus cycles as the interpreter (it calls
 * the same handlers from nes_cpu_impl.h, including the opcode fetch), so
 * cycle counts, open bus and I/O timing match and the two can run in
 * lockstep (--differential).
 */
#ifndef NES_RECOMP_H
#define NES_RECOMP_H

#include "nes.h"
#include "nes_bus.h"
#include "nes_cpu_impl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Entry key: PRG page number (ROM offset / 8 KiB) << 16 | CPU address. */
#define NES_RC_KEY(page, pc) (((uint32_t)(page) << 16) | ((uint32_t)(pc) & 0xFFFFu))
#define NES_RC_KEY_PC(key) ((uint16_t)((key) & 0xFFFFu))
#define NES_RC_KEY_PAGE(key) ((key) >> 16)
/* No key: the address is not in PRG ROM. */
#define NES_RC_NO_KEY 0xFFFFFFFFu

typedef void (*NesRcFunc)(NesCpu *c, uint32_t entry);

typedef struct NesRcEntry {
    uint32_t key;
    NesRcFunc fn;
} NesRcEntry;

typedef struct NesRcProgram {
    const NesRcEntry *entries; /* sorted by key */
    uint32_t entry_count;
    uint32_t rom_size;      /* the whole .nes file */
    const char *rom_sha256; /* lowercase hex of the file this was built from */
    const char *title;
    /* The ROM compiled into the executable, or NULL. */
    const uint8_t *(*embedded_rom)(size_t *size);
} NesRcProgram;

/* Install a program (NULL removes it). Returns false if the loaded ROM does
 * not match the one the program was built from; the machine then runs
 * interpreted. */
bool nes_rc_install(NesMachine *m, const NesRcProgram *program);

/* Table lookup used by the run loop; NULL when there is no entry. */
NesRcFunc nes_rc_lookup(NesMachine *m, uint32_t key);

/* The key of the code at `pc` under the current PRG mapping. */
static inline uint32_t nes_rc_key_at(const NesMachine *m, uint16_t pc)
{
    if (pc < 0x8000u) return NES_RC_NO_KEY;
    return NES_RC_KEY(m->cart.prg_page[(pc >> 13) & 3u] / NES_PRG_PAGE, pc);
}

/* Called by generated code when c->cycles reaches c->next_sync: catches the
 * devices up in place, exactly where the interpreter's run loop would, and
 * says whether native code must return to the run loop (an interrupt can be
 * taken, the CPU jammed, a frame completed, or PRG banks were switched). */
bool nes_rc_sync(NesCpu *c);

#define NES_RC_YIELD(c) ((c)->cycles >= (c)->next_sync && nes_rc_sync(c))

/* The opcode fetch of an instruction at a known address. */
static inline void nes_rc_fetch(NesCpu *c, uint16_t pc)
{
    c->pc = (uint16_t)(pc + 1u);
    (void)nes_bus_read(c, pc);
}

#ifdef __cplusplus
}
#endif

#endif /* NES_RECOMP_H */
