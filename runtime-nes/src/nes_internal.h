/*
 * Shared between the machine's own files (nes.c, nes_bus.c): the device
 * clocks and where, inside a CPU cycle, the CPU and the PPU meet.
 */
#ifndef NES_INTERNAL_H
#define NES_INTERNAL_H

#include "nes.h"

/* CPU cycle n (the bus call that makes c->cycles == n) covers PPU dots
 * 3n-3 .. 3n-1. A register access sees the PPU after it has run this many
 * of the cycle's three dots; the interrupt inputs are sampled at the end
 * of a cycle, after all three. Measured against blargg's ppu_vbl_nmi
 * tests; see docs/NES-PLAN.md. */
#define NES_PPU_READ_DOTS 1u
#define NES_PPU_WRITE_DOTS 2u
#define NES_PPU_MASK_WRITE_DOTS 3u

/* Run the PPU up to a dot count, or the APU up to a CPU cycle count, noting
 * interrupt edges on the way. */
void nes_sync_ppu(NesMachine *m, uint64_t target_dots);
void nes_sync_apu(NesMachine *m, uint64_t target_cycles);
/* After anything that can move the PPU's NMI output or finish a frame. */
void nes_ppu_outputs(NesMachine *m);
/* Re-evaluate the IRQ line; `at` is the CPU cycle of the change. */
void nes_update_irq(NesMachine *m, uint64_t at);
/* Choose m->next_event and when the next DMA is due. */
void nes_schedule(NesMachine *m);

/* The PPU is fetching tiles (PPUMASK shows the background or sprites). */
static inline bool nes_ppu_rendering(const NesPpu *p) { return (p->mask & 0x18u) != 0; }

/* The PPU's address lines can change without a register access, and the
 * cartridge is waiting for them to: the PPU is fetching, or a $2006 write
 * is on its way to v (it gets there three dots later). */
static inline bool nes_ppu_address_moves(const NesMachine *m)
{
    return nes_cart_watches_ppu(m) && (nes_ppu_rendering(&m->ppu) || m->ppu.transfer_delay);
}

/* The NMI input must be active this many dots before the end of a cycle for
 * the CPU's edge detector to catch it in that cycle. */
#define NES_NMI_SETUP_DOTS 1u

/* Whether the NMI edge reached the CPU by the time the instruction that
 * just ran sampled its interrupt inputs (the end of cycle poll_at). nmi_at
 * counts dots completed when the line went active. */
static inline bool nes_nmi_sampled(const NesMachine *m)
{
    return m->nmi_at + NES_NMI_SETUP_DOTS <= m->cpu.poll_at * NES_DOTS_PER_CYCLE;
}

/* The IRQ line is active now. */
static inline bool nes_irq_line(const NesMachine *m)
{
    return m->irq_since != UINT64_MAX && m->irq_until == UINT64_MAX;
}

/* Whether the IRQ line was active, and not masked, when the instruction
 * that just ran sampled its interrupt inputs. */
static inline bool nes_irq_sampled(const NesMachine *m)
{
    return m->irq_since <= m->cpu.poll_at && m->cpu.poll_at < m->irq_until && !m->cpu.poll_i;
}

#endif /* NES_INTERNAL_H */
