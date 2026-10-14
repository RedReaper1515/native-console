/*
 * NES machine: the 2A03's CPU and I/O, 2 KiB of work RAM, the cartridge,
 * controllers, timing and interrupts, around the PPU and APU. The
 * interpreter and recompiled code both run against this struct; the CPU is
 * its first member so bus callbacks recover the machine from the NesCpu
 * pointer at no cost.
 *
 * Time is counted in CPU cycles (1.789773 MHz NTSC); every bus access is
 * one cycle. The PPU runs three dots per CPU cycle and the APU one step,
 * but neither is stepped with the CPU: each is caught up when the CPU
 * touches its registers, and at the next moment it could change something
 * the CPU sees by itself (the NMI line, an IRQ, a DMC fetch, the end of a
 * frame). Those moments are m->next_event; the run loop and recompiled
 * code compare the cycle counter with it between instructions.
 */
#ifndef NES_H
#define NES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nes_apu.h"
#include "nes_cart.h"
#include "nes_cpu.h"
#include "nes_ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NES_CPU_HZ_NUM 39375000u /* CPU clock = 39375000 / 22 Hz (the 21.477 MHz master clock / 12) */
#define NES_CPU_HZ_DEN 22u
#define NES_DOTS_PER_CYCLE 3u
#define NES_RAM_SIZE 0x800u
#define NES_WIDTH 256
#define NES_HEIGHT 240

/* Controller buttons, in the order the shift register reports them. */
enum {
    NES_BTN_A = 0x01,
    NES_BTN_B = 0x02,
    NES_BTN_SELECT = 0x04,
    NES_BTN_START = 0x08,
    NES_BTN_UP = 0x10,
    NES_BTN_DOWN = 0x20,
    NES_BTN_LEFT = 0x40,
    NES_BTN_RIGHT = 0x80,
};

/* attention bits: why recompiled code must return to the run loop. */
enum {
    NES_ATTENTION_INTERRUPT = 1u, /* an NMI or IRQ can be taken */
    NES_ATTENTION_HALT = 2u,      /* the CPU is jammed */
    NES_ATTENTION_FRAME = 4u,     /* a frame completed */
    NES_ATTENTION_REMAP = 8u,     /* the cartridge switched PRG banks */
};

struct NesRcProgram;
struct NesCoverage;

typedef struct NesMachine {
    NesCpu cpu; /* must stay first */

    /* Memory. */
    uint8_t ram[NES_RAM_SIZE];
    uint8_t prg_ram[NES_PRG_RAM_MAX];
    uint8_t chr_ram[NES_CHR_RAM_MAX];
    uint8_t nt_ram[NES_NT_RAM_SIZE];
    bool prg_ram_dirty;        /* battery RAM written since the frontend last saved it */

    /* The cartridge image (owned copy of the whole file) and its parts. */
    const uint8_t *file;
    uint32_t file_size;
    const uint8_t *prg;
    const uint8_t *chr;
    NesCartInfo info;
    NesCart cart;

    /* Controllers. */
    uint8_t buttons[2];        /* NES_BTN_* state set by the frontend */
    uint8_t shift[2];          /* the controllers' shift registers */
    bool strobe;               /* the OUT0 pin: the controllers' strobe line */
    bool strobe_latch;         /* $4016 bit 0 as last written */
    uint64_t strobe_at;        /* the cycle from which the pin shows the latch (UINT64_MAX: it does) */

    /* Device clocks. */
    uint64_t ppu_dots;         /* dots the PPU has run */
    uint64_t apu_cycles;       /* CPU cycles the APU has run */
    uint64_t next_event;       /* CPU cycle of the next catch-up */
    bool in_sync;              /* inside nes_sync (no nested catch-ups) */

    /* Interrupts. The NMI input is edge sensitive: nmi_pending is the
     * latched edge and nmi_at the PPU dot count at which the line went
     * active. IRQ is a level: the line has been active since CPU cycle
     * irq_since (UINT64_MAX = never) and was released in cycle irq_until
     * (UINT64_MAX while it is still active). The release time is kept
     * because the CPU samples the line a cycle before an instruction ends:
     * an instruction whose last cycle acknowledges the interrupt is still
     * followed by it. */
    bool nmi_line;
    bool nmi_pending;
    uint64_t nmi_at;
    uint64_t irq_since;
    uint64_t irq_until;
    uint8_t attention;

    /* OAM DMA ($4014) and the DMC's sample fetches take the bus from the
     * CPU, which can only be stopped on a read: nes_bus_read() runs the
     * DMA before the read whose cycle would reach dma_at (nes_bus.c). */
    uint64_t dma_at;           /* the first cycle count at which a read is halted (UINT64_MAX: none due) */
    uint64_t dmc_at;           /* the same for the DMC's next fetch alone */
    uint64_t dmc_stolen;       /* CPU cycles DMAs with a DMC fetch have taken in all */
    uint16_t dmc_addr;         /* DMC fetch waiting for its read cycle: the sample address */
    bool dmc_pending;          /* the DMC has asked for a byte and the read cycle has not come yet */
    bool dmc_dummy;            /* ... and a cycle has passed since, so the next get cycle is its read */
    bool dmc_irq_before;       /* the DMC's IRQ flag before that fetch (the fetch may raise it) */
    bool oam_pending;          /* $4014 was written: OAM DMA starts at the next read */
    uint8_t oam_page;
    bool in_dma;

    uint64_t frame_count;
    bool frame_ready;

    /* Recompiled program (nes_recomp.h), or NULL to interpret everything. */
    const struct NesRcProgram *rc_program;
    bool in_native;
    uint32_t *rc_hash;         /* entry index per slot, UINT32_MAX = empty */
    uint32_t rc_hash_mask;

    /* Diagnostics. */
    struct NesCoverage *coverage;
    void (*trace)(struct NesMachine *m); /* called before each interpreted instruction */
    void *trace_user;

    NesPpu ppu;
    NesApu apu;
} NesMachine;

#define NES_MACHINE(cpu_ptr) ((NesMachine *)(void *)(cpu_ptr))

NesMachine *nes_create(void);
void nes_destroy(NesMachine *m);

/* The file is copied. Returns false if it is not an iNES image or its
 * mapper is not implemented (m->info is filled in either case when the
 * header parses, so the caller can say which). */
bool nes_load_rom(NesMachine *m, const uint8_t *data, size_t size);

/* Power on: RAM cleared, every device in its power-on state, the CPU at
 * the reset vector. Battery RAM is kept. */
void nes_power(NesMachine *m);

/* The reset button: the CPU restarts, RAM and the cartridge are kept. */
void nes_reset(NesMachine *m);

/* Run until at least `cycles` more CPU cycles have elapsed. */
void nes_run_cycles(NesMachine *m, uint64_t cycles);

/* One run-loop iteration (a native call or one interpreted instruction). */
void nes_run_slice(NesMachine *m);

/* Run until the next frame completes (the picture's last line is drawn). */
void nes_run_frame(NesMachine *m);

void nes_set_buttons(NesMachine *m, int port, uint8_t buttons);

/* Side-effect-free read for debuggers, tracers and test harnesses. */
uint8_t nes_peek(NesMachine *m, uint16_t addr);

/* The current frame as 0x00RRGGBB, NES_WIDTH x NES_HEIGHT. */
void nes_frame_rgb(const NesMachine *m, uint32_t *out);

/* Coverage of recompiled code (nes_coverage.c): counts interpreted
 * instructions and records where interpreted code was entered, for
 * nesrecomp --trace. */
void nes_coverage_enable(NesMachine *m);
void nes_coverage_free(NesMachine *m);
void nes_coverage_note(NesMachine *m, uint32_t key);  /* an interpreted instruction */
void nes_coverage_entry(NesMachine *m, uint32_t key); /* execution resumes at key */
void nes_coverage_branched(NesMachine *m);            /* the next interpreted instruction is an entry */
void nes_coverage_report(const NesMachine *m);
bool nes_coverage_write_trace(const NesMachine *m, const char *path);

/* Internal entry points shared between runtime files. */

/* Catch the PPU and APU up to the CPU, note interrupt edges with the cycle
 * they happened in, and choose the next event. */
void nes_sync(NesMachine *m);
static inline void nes_catch_up(NesMachine *m)
{
    if (m->cpu.cycles >= m->next_event) nes_sync(m);
}
/* Recompute m->attention and the CPU's next_sync from the interrupt lines. */
void nes_update_attention(NesMachine *m);

uint8_t nes_io_read(NesMachine *m, uint16_t addr, uint8_t mdr); /* $4000-$401F */
void nes_io_write(NesMachine *m, uint16_t addr, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* NES_H */
