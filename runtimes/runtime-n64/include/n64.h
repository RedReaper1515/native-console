/*
 * The Nintendo 64: the machine around the VR4300 core.
 *
 * N64Machine owns RDRAM, the cartridge, the RSP and the RDP, and the RCP's
 * interfaces (MI, VI, AI, PI, RI, SI) and the PIF. Its run loop interleaves
 * the CPU (interpreted, or recompiled code through n64_recomp.h) with
 * events kept as absolute CPU cycles, and catches the RSP up whenever the
 * CPU could observe it.
 *
 * Determinism: nothing here reads the host clock or keeps state outside
 * the struct. The same ROM, saves and input give the same machine state,
 * interpreted or recompiled.
 */
#ifndef N64_H
#define N64_H

#include "n64_cpu.h"
#include "n64_rdp.h"
#include "n64_rsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Controller buttons (the joybus status word's top 16 bits). */
#define N64_BTN_A 0x8000u
#define N64_BTN_B 0x4000u
#define N64_BTN_Z 0x2000u
#define N64_BTN_START 0x1000u
#define N64_BTN_DUP 0x0800u
#define N64_BTN_DDOWN 0x0400u
#define N64_BTN_DLEFT 0x0200u
#define N64_BTN_DRIGHT 0x0100u
#define N64_BTN_L 0x0020u
#define N64_BTN_R 0x0010u
#define N64_BTN_CUP 0x0008u
#define N64_BTN_CDOWN 0x0004u
#define N64_BTN_CLEFT 0x0002u
#define N64_BTN_CRIGHT 0x0001u

/* MI interrupt bits (MI_INTR). */
#define N64_MI_SP 0x01u
#define N64_MI_SI 0x02u
#define N64_MI_AI 0x04u
#define N64_MI_VI 0x08u
#define N64_MI_PI 0x10u
#define N64_MI_DP 0x20u

/* Video output: up to 640 x 576 (PAL), as the VI scans it out. */
#define N64_FB_WIDTH 640
#define N64_FB_HEIGHT 576

/* Audio leaves the machine at one fixed rate, resampled from the AI's
 * DAC rate (which games set). */
#define N64_AUDIO_RATE 48000u
#define N64_AUDIO_RING 16384u /* stereo frames */

enum N64SaveType { N64_SAVE_NONE = 0, N64_SAVE_EEPROM_4K, N64_SAVE_EEPROM_16K, N64_SAVE_SRAM, N64_SAVE_SRAM_96K, N64_SAVE_FLASH };

typedef struct N64Cart {
    char title[21];
    char id[5];     /* the game code: category, two letters, region */
    uint8_t revision;
    uint32_t entry; /* the header's entry point */
    uint32_t cic;   /* 6101, 6102, 6103, 6105, 6106 (7xxx PAL ones map onto these), 5101, 8303 */
    uint32_t cic_seed;
    bool ipl3_known; /* the boot code is one of the known IPL3s, so it can run */
    bool pal;
    uint8_t eeprom_kbit; /* 0, 4 or 16 */
    uint32_t sram_size;  /* 0, 32 KiB, 96 KiB or 128 KiB */
    bool flash;
    uint8_t flash_model; /* see N64Flash */
    bool cpak, rpak, rtc;
} N64Cart;

/* Event slots (absolute CPU cycles; UINT64_MAX when idle). */
enum {
    N64_EV_VI = 0,  /* the next scanline */
    N64_EV_AI,      /* the next audio sample */
    N64_EV_PI,      /* a PI DMA finishes */
    N64_EV_SI,      /* an SI DMA finishes */
    N64_EV_TIMER,   /* Count reaches Compare */
    N64_EV_RSP,     /* let a running RSP make progress */
    N64_EV_EEPROM,  /* an EEPROM write finishes */
    N64_EV_FLASH,   /* a FlashRAM operation finishes */
    N64_EV_COUNT,
};

/* FlashRAM state (n64_cart.c). */
typedef struct N64Flash {
    uint8_t model;        /* 0: Macronix MX29L1100 (word-indexed), 1: MX29L1101 */
    uint8_t mode;         /* 0 read array, 1 status, 2 silicon ID, 3 load page */
    uint8_t status;       /* bit 7 ready, 3 erase ok, 2 program ok, 1 erase busy, 0 program busy */
    uint8_t erase_setup;  /* 0 none, 1 chip, 2 sector */
    uint8_t erase_sector;
    uint8_t busy;         /* 0 none, 1 erase, 2 program */
    uint8_t page[128];
    uint32_t offset;      /* the PI offset into domain 2 */
    uint16_t cir_high;
    bool cir_valid;
    bool open_bus;
    bool status_stale;
    uint16_t stale_value;
    uint8_t pending_cmd, pending_count;
    uint16_t burst;
} N64Flash;

/* One RDRAM chip's registers (n64_rdram.c, from ares). */
typedef struct N64RdramChip {
    bool present, enable, auto_current;
    uint16_t device_id;
    uint8_t write_delay;
    uint8_t cci, cc_internal, cc_low, cc_high; /* 6-bit current control values */
    uint32_t device_type, device_id_reg, delay, mode;
    uint32_t refresh_interval, refresh_row, ras_interval, min_interval, address_select;
    uint32_t device_manufacturer, row;
} N64RdramChip;

/* The PIF's boot handshake (n64_pif.c, from ares's PIF::mainHLE). */
enum { N64_PIF_RUN = 0, N64_PIF_WAIT_LOCKOUT, N64_PIF_WAIT_GET_CHECKSUM, N64_PIF_WAIT_CHECK_CHECKSUM,
       N64_PIF_WAIT_TERMINATE };

struct N64RcProgram;

typedef struct N64Machine {
    N64Cpu cpu; /* first: generated code gets the machine from the CPU */
    N64Rsp rsp;
    N64Rdp rdp;

    uint8_t *rdram;
    uint32_t rdram_size;
    uint8_t *rom;
    uint32_t rom_size;
    N64Cart cart;

    /* PIF RAM, 64 bytes at 0x1FC007C0. */
    uint8_t pif_ram[64];
    bool pif_joy_armed; /* a command list was written with the "run" bit */

    /* MI */
    uint32_t mi_mode, mi_intr, mi_mask;

    /* VI: the 14 registers, and the beam. */
    uint32_t vi_regs[14];
    uint32_t vi_line;   /* current half-line counter / 2 */
    uint32_t vi_field;
    uint32_t vi_frac;   /* fractional CPU cycles carried between lines */

    /* AI */
    uint32_t ai_dram_addr[2], ai_length[2];
    uint32_t ai_count;  /* buffers queued (0-2) */
    bool ai_enable;
    uint32_t ai_dacrate, ai_bitrate;
    uint32_t ai_frac;
    int16_t ai_left, ai_right; /* the DAC's current output */
    /* Resampler to N64_AUDIO_RATE: phase in 1/2^32 output periods. */
    uint64_t ai_phase;
    int16_t audio_ring[N64_AUDIO_RING * 2];
    uint32_t audio_head, audio_tail;
    bool audio_off; /* reduced-workload runs skip producing samples */

    /* PI */
    uint32_t pi_dram_addr, pi_cart_addr, pi_rd_len, pi_wr_len, pi_status;
    uint32_t pi_bsd[8]; /* DOM1 LAT/PWD/PGS/RLS, DOM2 LAT/PWD/PGS/RLS */
    uint32_t pi_latch;
    uint32_t cart_addr; /* the cartridge bus address the PI last selected */

    /* RI and the RDRAM chips' registers (n64_rdram.c). IPL3 programs them;
     * until every chip is mapped where it is (rdram_identity), RDRAM
     * accesses go through the chips' device IDs. */
    uint32_t ri_regs[8];
    bool ri_current_loaded;
    N64RdramChip rdram_chips[4];
    bool rdram_identity;
    uint32_t rdram_rng; /* deterministic noise for under-powered chips */

    /* The PIF's boot handshake (N64_PIF_*), and ROM lockout. */
    uint8_t pif_state;
    bool pif_rom_lockout;
    /* Skip IPL3 and start from the state it leaves (n64_boot.c), even when
     * the cartridge's IPL3 is a known one. */
    bool hle_boot;

    /* SI */
    uint32_t si_dram_addr, si_status;
    uint8_t si_pending; /* 1: PIF -> RDRAM read in flight, 2: RDRAM -> PIF write */

    /* Controllers. */
    uint16_t buttons[4];
    int8_t stick_x[4], stick_y[4];
    uint8_t controllers; /* bit n: a controller in port n */

    /* Cartridge saves. */
    uint8_t eeprom[2048];
    uint8_t *sram; /* sram_size bytes, or the 128 KiB of FlashRAM */
    N64Flash flash;
    bool eeprom_busy;
    uint8_t mempak[4][0x8000];
    bool save_dirty;

    /* ISViewer: the debug output channel homebrew and test ROMs write to
     * at 0x13FF0000 (buffer at 0x13FF0020, length at 0x13FF0014). */
    uint8_t isv_buffer[0x10000];
    char *isv_log; /* the text so far, NUL-terminated (grows) */
    size_t isv_len, isv_cap;

    /* Events. */
    uint64_t events[N64_EV_COUNT];
    uint64_t next_event;
    /* RSP catch-up: CPU cycle up to which the RSP has run, and the third of
     * a CPU cycle carried between catch-ups (the RSP runs at 2/3 of the
     * CPU clock). */
    uint64_t rsp_synced;
    uint32_t rsp_frac;

    /* Video output. */
    uint32_t framebuffer[N64_FB_WIDTH * N64_FB_HEIGHT];
    int out_width, out_height;
    uint64_t frame_count;
    bool frame_ready;
    bool video_off; /* reduced-workload runs skip the scan-out */
    bool vi_raw;    /* scan out without the VI's filters: nearest neighbour, no anti-aliasing (n64_vi.c) */
    /* The picture again from the RDP's larger one (n64_set_internal_scale):
     * out_width * large_scale by out_height * large_scale pixels, a line
     * every out_width * large_scale. NULL at an internal scale of 1;
     * large_scale is 0 while there is nothing in it. It is no part of the
     * console: framebuffer above is drawn as it always was. */
    uint32_t *large_frame;
    int large_scale;

    /* Recompiled code (n64_recomp.c). */
    const struct N64RcProgram *rc_program;
    uint32_t *rc_hash;
    uint32_t rc_hash_mask;
    uint8_t *rc_code_map;     /* one byte per 64-byte granule of RDRAM */
    uint32_t *rc_granule_fns; /* first entry of each granule's function list (index into rc_granule_list) */
    uint32_t *rc_granule_list;
    uint8_t *rc_verified;     /* per RAM block: 1 when its bytes are known to match */
    uint64_t rc_invalidations;

    /* Coverage and traces (n64_coverage.c). */
    void *coverage;

    /* The interpreter stops once the CPU has retired this many instructions
     * (the differential runner holds the interpreter to the recompiled
     * machine's slices with it). UINT64_MAX: no limit. */
    uint64_t stop_instructions;

    /* Called before each interpreted instruction (debugging aid). */
    void (*trace)(struct N64Machine *m);

    /* Host time spent in each coprocessor and the scan-out, in
     * n64_host_ticks() units, kept while perf_on (the runner's --perf). The
     * RDP's share while the RSP was driving it is also in perf_rdp_in_rsp. */
    bool perf_on;
    uint64_t perf_rsp, perf_rdp, perf_rdp_in_rsp, perf_vi;
} N64Machine;

#define N64_MACHINE(c) ((N64Machine *)(c))

N64Machine *n64_create(void);
void n64_destroy(N64Machine *m);

/* Load a ROM (z64 big-endian, v64 byte-swapped or n64 little-endian; the
 * order is detected and normalised). Parses the header, detects the CIC
 * and the save hardware. */
bool n64_load_rom(N64Machine *m, const uint8_t *data, size_t size);

/* Power on: reset every device and run the boot (n64_boot.c). */
void n64_power(N64Machine *m);

/* Run until the VI completes a field (a frame). */
void n64_run_frame(N64Machine *m);

/*
 * Internal resolution: 1 (the console's own picture and nothing more), 2
 * or 4 times the size, each way, that the RDP draws at (n64_rdp.h,
 * n64_rdp_set_scale). Everything the game can read, and framebuffer[], are
 * as they were; the larger picture is scanned out beside them into
 * large_frame, without the VI's anti-aliasing, dither and divot filters,
 * which work on the console's own pixels. Returns false, and leaves the
 * scale at 1, when there is no memory for it.
 *
 * n64_large_frame() is the last field's larger picture, 0x00RRGGBB, or
 * NULL when there is none. Its size is out_width and out_height times
 * large_scale, which is the internal scale for a picture that has a
 * framebuffer pixel for every pixel of the display, and half of it for the
 * usual one with a framebuffer pixel for every two.
 */
bool n64_set_internal_scale(N64Machine *m, int scale);
int n64_internal_scale(const N64Machine *m);
const uint32_t *n64_large_frame(const N64Machine *m, int *width, int *height);

/* One run-loop step: take due events and interrupts, then run native code
 * for one entry, or interpret up to the next sync point at which an event
 * is due. Returns after at most `limit` cycles when interpreting. */
void n64_run_slice(N64Machine *m, uint64_t limit);

/* Events. */
void n64_schedule(N64Machine *m, int ev, uint64_t at);
void n64_process_events(N64Machine *m);
void n64_update_sync(N64Machine *m);

/* Interrupt lines. */
void n64_mi_raise(N64Machine *m, uint32_t bits);
void n64_mi_lower(N64Machine *m, uint32_t bits);

/* Run the RSP up to the CPU's current cycle. */
void n64_sync_rsp(N64Machine *m);

/* Input. */
void n64_set_buttons(N64Machine *m, int port, uint16_t buttons, int8_t x, int8_t y);

/* Audio: up to max stereo frames at N64_AUDIO_RATE; returns how many. */
size_t n64_read_audio(N64Machine *m, int16_t *out, size_t max);

/* Saves. The machine's battery memory as one image (EEPROM, SRAM or
 * FlashRAM, then the four Controller Paks), and back. */
size_t n64_save_size(const N64Machine *m);
void n64_save_export(const N64Machine *m, uint8_t *out);
void n64_save_import(N64Machine *m, const uint8_t *data, size_t size);

/* Debug reads (no side effects) of a physical address. */
uint8_t n64_peek(const N64Machine *m, uint32_t paddr);

/* RDRAM bytes [paddr, paddr + size) were written by a device (PI, SI, SP
 * DMA, the RDP): recompiled code there must be checked again. */
void n64_ram_written(N64Machine *m, uint32_t paddr, uint32_t size);

/* A monotonic host clock for measurements, and its ticks per second. */
uint64_t n64_host_ticks(void);
uint64_t n64_host_ticks_per_second(void);

/* VI scan-out of the current field into m->framebuffer (n64_vi.c). */
void n64_vi_render(N64Machine *m);

/* The HLE boot (n64_boot.c). */
void n64_boot(N64Machine *m);

/* Cartridge (n64_cart.c). */
bool n64_cart_parse(N64Cart *cart, const uint8_t *rom, size_t size);
uint32_t n64_cic_from_ipl3(const uint8_t *rom, uint32_t *seed);
void n64_cartdb_lookup(N64Cart *cart);

#ifdef __cplusplus
}
#endif

#endif /* N64_H */
