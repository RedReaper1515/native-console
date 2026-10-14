/*
 * SPC700 core over a flat 64 KiB RAM that logs every bus cycle. This is
 * the instantiation the SingleStepTests harness drives; the APU
 * instantiates the same instruction code (snes_spc700_impl.h) over its
 * memory map in snes_apu.c.
 */
#include "snes_spc700.h"

static inline void flat_log(SnesSpc700FlatBus *b, uint8_t type, uint16_t addr, uint8_t value, uint8_t old)
{
    b->cycles++;
    if (b->log && b->log_n < b->log_cap) {
        SnesSpc700Txn *t = &b->log[b->log_n];
        t->addr = addr;
        t->value = value;
        t->type = type;
        t->old = old;
    }
    b->log_n++;
}

static inline uint8_t flat_read(SnesSpc700FlatBus *b, uint16_t addr)
{
    uint8_t v = b->ram[addr];
    flat_log(b, SNES_SPC700_TX_READ, addr, v, 0);
    return v;
}

static inline void flat_write(SnesSpc700FlatBus *b, uint16_t addr, uint8_t v)
{
    flat_log(b, SNES_SPC700_TX_WRITE, addr, v, b->ram[addr]);
    b->ram[addr] = v;
}

static inline void flat_idle(SnesSpc700FlatBus *b) { flat_log(b, SNES_SPC700_TX_IDLE, 0, 0, 0); }

#define SPC_FN spc700_step_flat_impl
#define SPC_CTX SnesSpc700FlatBus *
#define SPC_READ(ctx, addr) flat_read((ctx), (addr))
#define SPC_WRITE(ctx, addr, v) flat_write((ctx), (addr), (v))
#define SPC_IDLE(ctx) flat_idle(ctx)
#include "snes_spc700_impl.h"

void snes_spc700_step_flat(SnesSpc700 *c, SnesSpc700FlatBus *bus) { spc700_step_flat_impl(c, bus); }
