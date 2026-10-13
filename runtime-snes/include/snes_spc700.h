/*
 * SPC700 (Sony S-SMP) CPU core state.
 *
 * The instruction set is implemented once, in snes_spc700_impl.h, as a
 * template over the bus: the includer defines how a bus read, write and
 * internal (idle) cycle are performed. The library instantiates it twice:
 *  - inside snes_apu.c over the APU memory map (timers, DSP, ports);
 *  - in snes_spc700.c over a flat 64 KiB RAM that logs every bus cycle,
 *    for the SingleStepTests harness (tests/snes/sst_spc700.c).
 * Both share every line of instruction code; there are no function
 * pointers on the hot path.
 *
 * The state holds no pointers (the machine is saved as one block).
 */
#ifndef SNES_SPC700_H
#define SNES_SPC700_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Halt states (SLEEP and STOP both stop the core until reset; the SNES
 * has no interrupt that could wake it). */
enum { SNES_SPC700_RUN = 0, SNES_SPC700_SLEEP = 1, SNES_SPC700_STOP = 2 };

typedef struct SnesSpc700 {
    uint16_t pc;
    uint8_t a, x, y, sp;
    /* PSW, one byte per flag (each 0 or 1). */
    uint8_t fn, fv, fp, fb, fh, fi, fz, fc;
    uint8_t halt; /* SNES_SPC700_RUN / _SLEEP / _STOP */
    uint8_t pad_[3];
} SnesSpc700;

static inline uint8_t snes_spc700_get_psw(const SnesSpc700 *c)
{
    return (uint8_t)(c->fc | (c->fz << 1) | (c->fi << 2) | (c->fh << 3) | (c->fb << 4) | (c->fp << 5) |
                     (c->fv << 6) | (c->fn << 7));
}

static inline void snes_spc700_set_psw(SnesSpc700 *c, uint8_t p)
{
    c->fc = (uint8_t)(p & 1u);
    c->fz = (uint8_t)((p >> 1) & 1u);
    c->fi = (uint8_t)((p >> 2) & 1u);
    c->fh = (uint8_t)((p >> 3) & 1u);
    c->fb = (uint8_t)((p >> 4) & 1u);
    c->fp = (uint8_t)((p >> 5) & 1u);
    c->fv = (uint8_t)((p >> 6) & 1u);
    c->fn = (uint8_t)((p >> 7) & 1u);
}

/* ---- Flat test bus (for the SingleStepTests harness) --------------- */

enum { SNES_SPC700_TX_READ = 0, SNES_SPC700_TX_WRITE = 1, SNES_SPC700_TX_IDLE = 2 };

typedef struct SnesSpc700Txn {
    uint16_t addr;  /* 0 for idle cycles */
    uint8_t value;  /* byte read or written; 0 for idle cycles */
    uint8_t type;   /* SNES_SPC700_TX_* */
    uint8_t old;    /* for writes: the RAM byte before the write */
} SnesSpc700Txn;

/* A test context, not machine state (it holds pointers). */
typedef struct SnesSpc700FlatBus {
    uint8_t *ram;          /* 64 KiB, flat: no I/O, no boot ROM */
    SnesSpc700Txn *log;    /* may be NULL */
    uint32_t log_cap;
    uint32_t log_n;        /* cycles logged (continues past log_cap uncounted) */
    uint64_t cycles;       /* every bus cycle, logged or not */
} SnesSpc700FlatBus;

/* Run one instruction (or, when halted, one halted iteration: a dummy
 * read of PC and an idle cycle) against the flat bus. */
void snes_spc700_step_flat(SnesSpc700 *c, SnesSpc700FlatBus *bus);

#ifdef __cplusplus
}
#endif

#endif /* SNES_SPC700_H */
