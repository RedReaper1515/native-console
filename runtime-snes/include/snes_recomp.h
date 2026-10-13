/*
 * Interface between recompiled code (emitted by snesrecomp) and the
 * runtime.
 *
 * A recompiled program is a table of entry points keyed by the 24-bit PC
 * and the CPU mode (E, M, X): the same bytes decode differently in
 * different register widths, so code reached in two modes is compiled
 * twice. The run loop (snes.c) looks the current key up before every step;
 * on a hit it calls the native function, which runs until it leaves its
 * known code (a call, return, indirect jump, or a mode it cannot know
 * statically, such as after PLP) or yields; on a miss it interprets one
 * instruction.
 *
 * Generated code performs the same bus cycles as the interpreter (it calls
 * the same handlers from snes_cpu_impl.h, including the opcode fetch), so
 * cycle counts, open bus and I/O timing match and the two can run in
 * lockstep (--differential).
 */
#ifndef SNES_RECOMP_H
#define SNES_RECOMP_H

#include "snes.h"
#include "snes_bus.h"
#include "snes_cpu_impl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Entry key: PC (bank << 16 | address) | mode << 24 (SNES_MODE_* bits). */
#define SNES_RC_KEY(pc24, mode) (((uint32_t)(pc24) & 0xFFFFFFu) | ((uint32_t)(mode) << 24))
#define SNES_RC_KEY_PC(key) ((key) & 0xFFFFFFu)
#define SNES_RC_KEY_MODE(key) ((key) >> 24)

typedef void (*SnesRcFunc)(SnesCpu *c, uint32_t entry);

typedef struct SnesRcEntry {
    uint32_t key;
    SnesRcFunc fn;
    int32_t block; /* RAM code: index into ram_blocks; -1 for ROM */
} SnesRcEntry;

/* Code that runs from work RAM was compiled from a snapshot of it. Before
 * native code for it runs, WRAM must still hold exactly these bytes. */
typedef struct SnesRcRamBlock {
    uint32_t wram_offset; /* offset in the 128 KiB of WRAM */
    uint32_t size;
    const uint8_t *bytes;
} SnesRcRamBlock;

typedef struct SnesRcProgram {
    const SnesRcEntry *entries; /* sorted by key */
    uint32_t entry_count;
    uint32_t rom_size;
    const char *rom_sha256; /* lowercase hex of the ROM this was built from */
    const char *title;
    const SnesRcRamBlock *ram_blocks;
    uint32_t ram_block_count;
    /* The ROM compiled into the executable, or NULL. */
    const uint8_t *(*embedded_rom)(size_t *size);
} SnesRcProgram;

/* Install a program (NULL removes it). Returns false if the loaded ROM does
 * not match the one the program was built from; the machine then runs
 * interpreted. */
bool snes_rc_install(SnesMachine *m, const SnesRcProgram *program);

/* Table lookup used by the run loop; NULL when there is no entry, or when
 * it is RAM code whose bytes no longer match what was compiled. */
SnesRcFunc snes_rc_lookup(SnesMachine *m, uint32_t key);

/* Map a CPU address to its WRAM offset, or -1 when it is not work RAM
 * (banks 7E-7F, and $0000-$1FFF of banks 00-3F/80-BF). */
static inline int32_t snes_rc_wram_offset(uint32_t addr)
{
    uint32_t bank = (addr >> 16) & 0xFFu, off = addr & 0xFFFFu;
    if (bank == 0x7Eu || bank == 0x7Fu) return (int32_t)(((bank & 1u) << 16) | off);
    if (!(bank & 0x40u) && off < 0x2000u) return (int32_t)off;
    return -1;
}

/* Called by generated code when c->cycles reaches c->next_sync: runs due
 * events in place, exactly where the interpreter's run loop would, and
 * says whether native code must return to the run loop (an interrupt can
 * be taken, the CPU sleeps, or a frame completed). */
bool snes_rc_sync(SnesCpu *c);

#define SNES_RC_YIELD(c) ((c)->cycles >= (c)->next_sync && snes_rc_sync(c))

/* The opcode fetch of an instruction at a known address. */
static inline void snes_rc_fetch(SnesCpu *c, uint32_t pc24)
{
    c->pc = (uint16_t)((pc24 + 1u) & 0xFFFFu);
    (void)snes_bus_read(c, pc24);
}

#ifdef __cplusplus
}
#endif

#endif /* SNES_RECOMP_H */
