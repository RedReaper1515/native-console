/*
 * Super Nintendo machine: the S-CPU (65C816 + the 5A22's I/O), work RAM,
 * the cartridge, DMA/HDMA, timing and interrupts, around the PPU and APU.
 * The interpreter and recompiled code both run against this struct; the
 * CPU is its first member so bus callbacks recover the machine from the
 * SnesCpu pointer at no cost.
 *
 * Time is counted in master clocks (21.477 MHz NTSC). A scanline is 1364
 * clocks, a frame 262 lines (263 in an interlaced even field; one line of
 * the odd non-interlaced field is 4 clocks short). Events (line render,
 * HDMA, DRAM refresh, V-blank, IRQ timer, auto-joypad) are scheduled at
 * absolute times and run between instructions, and before any I/O access
 * so that hardware registers always see an up-to-date machine.
 */
#ifndef SNES_H
#define SNES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snes_apu.h"
#include "snes_cpu.h"
#include "snes_ppu.h"
#include "snes_sa1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNES_MASTER_HZ 21477272u
#define SNES_CLOCKS_PER_LINE 1364u
#define SNES_LINES_NTSC 262u
#define SNES_WRAM_SIZE 0x20000u
#define SNES_SRAM_MAX 0x40000u
#define SNES_ROM_MAX 0x800000u

/* Controller buttons, as the JOYn registers report them (active high). */
enum {
    SNES_BTN_R = 0x0010,
    SNES_BTN_L = 0x0020,
    SNES_BTN_X = 0x0040,
    SNES_BTN_A = 0x0080,
    SNES_BTN_RIGHT = 0x0100,
    SNES_BTN_LEFT = 0x0200,
    SNES_BTN_DOWN = 0x0400,
    SNES_BTN_UP = 0x0800,
    SNES_BTN_START = 0x1000,
    SNES_BTN_SELECT = 0x2000,
    SNES_BTN_Y = 0x4000,
    SNES_BTN_B = 0x8000,
};

/* Scheduled events; each has an absolute master-clock time, UINT64_MAX = idle. */
enum {
    SNES_EVENT_LINE_END,  /* the line ends; V advances */
    SNES_EVENT_RENDER,    /* the picture line's active display ends: render it */
    SNES_EVENT_HDMA,      /* HDMA transfers for the next line (H = 1104) */
    SNES_EVENT_HDMA_INIT, /* HDMA channels reload at the start of the frame */
    SNES_EVENT_REFRESH,   /* DRAM refresh pauses the CPU for 40 clocks */
    SNES_EVENT_IRQ,       /* the H/V timer IRQ fires */
    SNES_EVENT_JOYPAD,    /* auto-joypad read starts or finishes */
    SNES_EVENT_COUNT,
};

typedef enum {
    SNES_MAP_LOROM,
    SNES_MAP_HIROM,
    SNES_MAP_EXHIROM,
    SNES_MAP_SA1,
} SnesMapper;

/* Where a 4 KiB page of the 24-bit address space goes (snes_cart.c). */
enum {
    SNES_PAGE_OPEN, /* open bus */
    SNES_PAGE_ROM,
    SNES_PAGE_SRAM,
    SNES_PAGE_WRAM,
    SNES_PAGE_SYSTEM, /* $0000-$7FFF of banks 00-3F/80-BF: WRAM mirror and I/O */
    SNES_PAGE_COPROC, /* handled by the coprocessor (SA-1 registers, BW-RAM, I-RAM) */
};

typedef struct SnesCartInfo {
    char title[22];
    SnesMapper mapper;
    uint8_t map_mode;    /* header $xFD5 */
    uint8_t cart_type;   /* header $xFD6 */
    uint32_t rom_size;
    uint32_t sram_size;
    bool battery;
    bool fastrom;
    uint8_t region;      /* header $xFD9 */
    uint32_t header_at;  /* ROM offset of the header ($7FC0, $FFC0 or $40FFC0) */
    const char *coprocessor; /* NULL, or the name of an unsupported chip */
} SnesCartInfo;

typedef struct SnesDmaChannel {
    uint8_t control;       /* $43x0 DMAPx */
    uint8_t b_addr;        /* $43x1 BBADx */
    uint16_t a_addr;       /* $43x2-3 A1Tx */
    uint8_t a_bank;        /* $43x4 A1Bx */
    uint16_t size;         /* $43x5-6 DASx (also the HDMA indirect address) */
    uint8_t ind_bank;      /* $43x7 DASBx */
    uint16_t hdma_addr;    /* $43x8-9 A2Ax */
    uint8_t line_counter;  /* $43xA NTRLx */
    uint8_t unused_b;      /* $43xB / $43xF */
    bool dma_enable;
    bool hdma_enable;
    bool hdma_completed;
    bool hdma_do_transfer;
} SnesDmaChannel;

typedef struct SnesMachine {
    SnesCpu cpu; /* must stay first */

    /* Memory. */
    uint8_t wram[SNES_WRAM_SIZE];
    uint8_t sram[SNES_SRAM_MAX];
    const uint8_t *rom; /* owned, SNES_ROM_MAX at most */
    uint32_t rom_size;
    SnesCartInfo cart;
    bool sram_dirty;
    uint8_t page_type[4096];   /* per 4 KiB page of the 24-bit space */
    uint32_t page_offset[4096]; /* ROM/SRAM offset of the page's first byte (see snes_cart.c) */
    uint32_t page_mask[4096];   /* offset mask within the page mapping */

    /* S-CPU I/O. */
    uint32_t wram_port;       /* $2181-$2183, 17 bits */
    uint8_t nmitimen;         /* $4200 */
    uint8_t wrio;             /* $4201 */
    uint8_t wrmpya, wrmpyb;   /* $4202/$4203 */
    uint16_t wrdiva;          /* $4204/$4205 */
    uint8_t wrdivb;           /* $4206 */
    uint16_t htime, vtime;    /* $4207-$420A */
    uint8_t memsel;           /* $420D */
    uint16_t rddiv, rdmpy;    /* $4214-$4217 */
    uint32_t alu_shift;
    uint8_t alu_mul_left, alu_div_left; /* steps of the multiplier/divider still to run */
    uint64_t alu_edges;       /* bus cycles counted for the ALU (see snes_io.c) */
    uint64_t alu_synced;      /* alu_edges already applied */
    uint16_t joy[4];          /* $4218-$421F */
    uint16_t buttons[2];      /* controller state set by the frontend */
    uint16_t shift[2];        /* controller serial shift registers ($4016/$4017) */
    bool strobe;              /* $4016 bit 0 */
    bool joypad_busy;         /* auto-joypad read in progress (HVBJOY bit 0) */

    /* Interrupts. */
    bool rdnmi;               /* $4210 bit 7 */
    bool nmi_transition;      /* NMI edge waiting to be taken */
    uint64_t nmi_at;          /* when it happened */
    bool timeup;              /* $4211 bit 7: H/V IRQ flag */
    uint64_t irq_since;       /* since when the IRQ line has been asserted; UINT64_MAX = not */
    bool coproc_irq;          /* external IRQ line (cartridge coprocessor) */
    uint8_t attention;        /* see snes_update_attention() */

    /* DMA. */
    SnesDmaChannel dma[8];
    bool in_dma;

    /* Video timing. */
    uint16_t vcounter;
    uint64_t line_start;      /* master clock the current line began */
    uint32_t line_length;     /* clocks in the current line */
    uint16_t lines_this_frame;
    uint16_t vdisp;           /* first V-blank line: 225, or 240 with overscan */
    bool field;               /* interlace field / odd frame */
    uint64_t frame_count;
    bool frame_ready;

    /* APU clock: S-CPU master clocks already converted to SMP time. */
    uint64_t apu_synced_at;
    int64_t apu_debt;         /* in units of 1 / (SNES_MASTER_HZ) SMP cycles */

    /* Scheduler. */
    uint64_t event_at[SNES_EVENT_COUNT];
    uint64_t next_event;
    bool in_events;

    /* Recompiled program (snes_recomp.h), or NULL to interpret everything. */
    const struct SnesRcProgram *rc_program;
    bool in_native;
    uint32_t *rc_hash;           /* entry index per slot, UINT32_MAX = empty */
    uint32_t rc_hash_mask;
    uint8_t rc_code_pages[512];  /* 256-byte WRAM pages that hold compiled code */
    uint64_t rc_ram_gen;         /* bumped by writes to those pages */
    uint64_t *rc_verified;       /* per RAM block: generation it was last found intact */

    /* Diagnostics. */
    struct SnesCoverage *coverage;
    void (*trace)(struct SnesMachine *m); /* called before each interpreted instruction */
    void *trace_user;
    /* Called on every S-CPU access to the APU ports ($2140-$217F). */
    void (*apu_port_trace)(struct SnesMachine *m, unsigned port, uint8_t value, bool write);

    SnesPpu ppu;
    SnesApu apu;
    SnesSa1 sa1; /* sa1.present on SA-1 cartridges */
} SnesMachine;

#define SNES_MACHINE(cpu_ptr) ((SnesMachine *)(void *)(cpu_ptr))

/* attention: bit 0 = an interrupt can be taken, bit 1 = the CPU sleeps
 * (WAI/STP), bit 2 = a frame completed (cleared by the run loop when native
 * code returns). Recompiled code returns to the run loop when it is set. */
#define SNES_ATTENTION_FRAME 4u

SnesMachine *snes_create(void);
void snes_destroy(SnesMachine *m);

/* ROM data is copied (a 512-byte copier header is dropped). Returns false
 * if no valid header is found. */
bool snes_load_rom(SnesMachine *m, const uint8_t *data, size_t size);

/* Power on and reset: the CPU starts at the reset vector. */
void snes_power(SnesMachine *m);

/* Run until at least `clocks` more master clocks have elapsed. */
void snes_run_clocks(SnesMachine *m, uint64_t clocks);

/* One run-loop iteration (a native call or one interpreted instruction). */
void snes_run_slice(SnesMachine *m, uint64_t limit);

/* Run until the next frame completes (V-blank start). */
void snes_run_frame(SnesMachine *m);

void snes_set_buttons(SnesMachine *m, int port, uint16_t buttons);

/* Side-effect-free read for debuggers, tracers and test harnesses. */
uint8_t snes_peek(SnesMachine *m, uint32_t addr);

/* A write to WRAM at `offset` (0-0x1FFFF): compiled code in that page
 * must be checked again before it runs (snes_recomp.c). */
static inline void snes_wram_written(SnesMachine *m, uint32_t offset)
{
    if (m->rc_code_pages[(offset >> 8) & 0x1FFu]) m->rc_ram_gen++;
}

/* Coverage of recompiled code (snes_coverage.c): counts interpreted
 * instructions, and records where interpreted code was entered (and the
 * WRAM it ran from), for snesrecomp --trace. */
void snes_coverage_enable(SnesMachine *m);
void snes_coverage_free(SnesMachine *m);
void snes_coverage_note(SnesMachine *m, uint32_t key);   /* an interpreted instruction */
void snes_coverage_entry(SnesMachine *m, uint32_t key);  /* execution resumes at key */
void snes_coverage_branched(SnesMachine *m);             /* the next interpreted instruction is an entry */
/* The same for SA-1 instructions: only its ROM code feeds the trace (its
 * low memory is I-RAM, not WRAM). */
void snes_coverage_sa1_note(SnesMachine *m, uint32_t key);
void snes_coverage_sa1_entry(SnesMachine *m, uint32_t key);
void snes_coverage_sa1_branched(SnesMachine *m);
void snes_coverage_report(const SnesMachine *m);
bool snes_coverage_write_trace(const SnesMachine *m, const char *path);

/* Internal entry points shared between runtime files. */
void snes_schedule(SnesMachine *m, int event, uint64_t at);
void snes_process_events(SnesMachine *m);
static inline void snes_catch_up(SnesMachine *m)
{
    if (m->cpu.cycles >= m->next_event) snes_process_events(m);
}
void snes_update_attention(SnesMachine *m);
void snes_update_irq(SnesMachine *m);  /* re-evaluate the IRQ line (TIMEUP, coprocessor) */
/* The cartridge's IRQ line, driven at master clock `at` (the coprocessor's time). */
void snes_set_coproc_irq(SnesMachine *m, bool level, uint64_t at);
void snes_schedule_irq(SnesMachine *m); /* reprogram the H/V timer event */
void snes_sync_apu(SnesMachine *m);
uint16_t snes_hcounter(const SnesMachine *m); /* dot 0-339 */

uint8_t snes_io_read(SnesMachine *m, uint32_t addr);   /* $2180-$2183, $4016-$4017, $4200-$421F */
void snes_io_write(SnesMachine *m, uint32_t addr, uint8_t v);
void snes_joypad_event(SnesMachine *m);

uint8_t snes_dma_read(SnesMachine *m, uint32_t addr);  /* $4300-$437F */
void snes_dma_write(SnesMachine *m, uint32_t addr, uint8_t v);
void snes_dma_run(SnesMachine *m);   /* $420B written: run the enabled channels */
void snes_hdma_init(SnesMachine *m); /* frame start */
void snes_hdma_line(SnesMachine *m); /* once per visible line */

bool snes_cart_parse(const uint8_t *rom, size_t size, SnesCartInfo *info);
uint32_t snes_rom_mirror(uint32_t addr, uint32_t size); /* how cartridge address lines mirror ROM */
void snes_cart_map(SnesMachine *m);
uint8_t snes_cart_read(SnesMachine *m, uint32_t addr, uint8_t mdr);
void snes_cart_write(SnesMachine *m, uint32_t addr, uint8_t v);

/* Bus access without CPU timing, for DMA (A-bus) and the debugger. */
uint8_t snes_bus_read_raw(SnesMachine *m, uint32_t addr, uint8_t mdr);
void snes_bus_write_raw(SnesMachine *m, uint32_t addr, uint8_t v);
uint8_t snes_bbus_read(SnesMachine *m, uint8_t reg, uint8_t mdr);   /* $21xx */
void snes_bbus_write(SnesMachine *m, uint8_t reg, uint8_t v);

#ifdef __cplusplus
}
#endif

#endif /* SNES_H */
