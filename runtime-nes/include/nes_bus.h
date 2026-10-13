/*
 * The bus the 6502 core calls. The final program provides these: the machine
 * for the game, a transaction-replaying bus for the SingleStepTests harness.
 *
 * Each read or write is exactly one CPU cycle, and every cycle of the 6502
 * is one of the two: the chip has no internal cycles, an instruction that
 * has nothing to transfer reads a byte and discards it. Those reads reach
 * the bus like any other (a discarded read of $2002 or $4015 still clears
 * flags), in the order the chip makes them. The implementation adds one to
 * c->cycles per call and keeps c->mdr (open bus).
 *
 * A bus that halts the CPU on a read (the 2A03's DMAs pull RDY low) spends
 * the extra cycles inside nes_bus_read, before the read is made, and sets
 * c->stalled. Nothing else in the core depends on it but the four opcodes
 * whose result changes when RDY is low in a particular cycle
 * (nes_sh_store); they clear the flag themselves before the read they
 * care about.
 *
 * Addresses are the 16 bits the CPU drives. Mirrors, I/O and the cartridge
 * are the bus's business.
 */
#ifndef NES_BUS_H
#define NES_BUS_H

#include "nes_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t nes_bus_read(NesCpu *c, uint16_t addr);
void nes_bus_write(NesCpu *c, uint16_t addr, uint8_t value);

/* The moment BRK and the IRQ/NMI sequence choose their vector: four cycles
 * in, with PCH and PCL pushed and P about to be (so c->cycles is the
 * sequence's start plus 4, in BRK's case counting its opcode fetch).
 * `vector` is the one the sequence started for ($FFFE for BRK and IRQ,
 * $FFFA for NMI); the return value is the one it fetches two cycles later.
 * The 6502 decides this late, from its NMI latch at that moment, so an NMI
 * edge that arrived during those first four cycles takes the sequence over:
 * it pushes the P of a BRK (B set) or of an IRQ, but jumps through $FFFA.
 * An edge that arrives later waits for the handler's first instruction.
 * The machine returns $FFFA when its NMI edge is latched by now, and clears
 * the latch, since this call is what acknowledges an NMI; otherwise it
 * returns `vector`. Not a bus cycle: c->cycles does not move. The
 * SingleStepTests harness returns `vector`. */
uint16_t nes_bus_interrupt_vector(NesCpu *c, uint16_t vector);

#ifdef __cplusplus
}
#endif

#endif /* NES_BUS_H */
