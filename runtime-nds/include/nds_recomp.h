/*
 * Interface between recompiled code (emitted by ndsrecomp) and the runtime.
 *
 * A recompiled program is, for each CPU, a table of entry points: guest
 * address (bit 0 set for Thumb) -> native function. The run loop looks the
 * current PC up before every step; on a hit it calls the native function,
 * which runs until it branches out of the code it was compiled from or has
 * to yield; on a miss it interprets one instruction.
 *
 * All DS code runs from RAM, and overlays are loaded over one another, so
 * an address does not say which code is there. Every native function
 * therefore carries a hash of the exact bytes it was compiled from (a
 * "block": one or more address ranges), and runs only while guest memory
 * holds those bytes at those addresses. Several functions may share an entry
 * key, one per overlay that has code there; the one whose bytes are in
 * memory is the one that runs. Nothing else has to know which overlay is
 * loaded. A function does not depend on anything outside its block: calls
 * and indirect jumps leave through the run loop, and literal pools are read
 * from memory when the instruction runs.
 *
 * Checking is lazy. Writes to words that hold compiled code bump a
 * generation counter for their 4 KiB page (nds_rc_note_write, called by the
 * bus); a block is hashed again only when a page it lies in has moved, or
 * the memory map has, since it was last checked.
 *
 * Native code keeps the interpreter's cycle accounting (one opcode fetch per
 * instruction, two after a branch) and stops at exactly the instruction the
 * interpreter's loop would stop at, so the two can be compared slice by
 * slice (nds_run --differential).
 */
#ifndef NDS_RECOMP_H
#define NDS_RECOMP_H

#include "nds.h"
#include "nds_rc_hash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* typedef void (*NdsRcFunc)(NdsMachine *m, uint32_t entry) is in nds.h.
 * `entry` is the address to start at, which is any instruction of the
 * function: one that is not part of it returns at once, having run
 * nothing. */

typedef struct NdsRcEntry {
    uint32_t key;   /* guest address | 1 for Thumb */
    uint32_t block; /* index into NdsRcProgram.blocks */
    NdsRcFunc fn;
} NdsRcEntry;

typedef struct NdsRcRange {
    uint32_t addr;
    uint32_t size;
} NdsRcRange;

/* The bytes a function was compiled from: its instructions, as runs of
 * addresses in ascending order, and nds_rc_hash() over them in that order. */
typedef struct NdsRcBlock {
    uint32_t first_range; /* index into NdsRcProgram.ranges */
    uint32_t range_count;
    uint64_t hash;
} NdsRcBlock;

typedef struct NdsRcProgram {
    /* Per CPU ([NDS_ARM9], [NDS_ARM7]): sorted by key; entries that share a
     * key are adjacent. */
    const NdsRcEntry *entries[2];
    uint32_t entry_count[2];
    const NdsRcBlock *blocks;
    uint32_t block_count;
    const NdsRcRange *ranges;
    uint32_t range_count;
    const char *title;     /* from the header of the ROM this was built from */
    const char *game_code;
    /* The ROM compiled into the executable, or NULL (see rc_rom.c). */
    const uint8_t *(*embedded_rom)(size_t *size);
} NdsRcProgram;

/* Install a program (NULL removes it). It need not match the loaded ROM:
 * functions whose bytes are not in memory simply never run. Returns false
 * only if memory for the lookup tables cannot be had. */
bool nds_rc_install(NdsMachine *m, const NdsRcProgram *program);

/* The native function for `key` on `cpu` whose bytes are in memory now, or
 * NULL. *block gets its block index. */
NdsRcFunc nds_rc_lookup(NdsMachine *m, int cpu, uint32_t key, uint32_t *block);

/* Run native code for `cpu` from its current PC if there is any. Returns
 * false, having changed nothing, if there is not: the caller interprets one
 * instruction. Called by the run loop. */
bool nds_rc_run(NdsMachine *m, int cpu);

/* Everything about memory may have changed (reset, a state was loaded, a
 * BIOS was replaced, WRAM or the TCMs moved): check every block again. */
void nds_rc_invalidate(NdsMachine *m);

#ifdef __cplusplus
}
#endif

#endif /* NDS_RECOMP_H */
