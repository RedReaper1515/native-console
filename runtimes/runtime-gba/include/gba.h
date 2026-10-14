/*
 * Game Boy Advance machine: memory, I/O, timers, DMA, interrupts and video
 * timing around the ARM7TDMI core. The interpreter and recompiled code both
 * run against this struct; the CPU is its first member so bus callbacks can
 * recover the machine from the GbaCpu pointer at no cost.
 */
#ifndef GBA_H
#define GBA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gba_bus.h"
#include "gba_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GBA_BIOS_SIZE 0x4000u
#define GBA_EWRAM_SIZE 0x40000u
#define GBA_IWRAM_SIZE 0x8000u
#define GBA_IO_SIZE 0x400u
#define GBA_PALETTE_SIZE 0x400u
#define GBA_VRAM_SIZE 0x18000u
#define GBA_OAM_SIZE 0x400u
#define GBA_ROM_MAX 0x2000000u
#define GBA_SRAM_SIZE 0x10000u

#define GBA_SCREEN_WIDTH 240
#define GBA_SCREEN_HEIGHT 160
#define GBA_CYCLES_PER_LINE 1232u
#define GBA_LINES_PER_FRAME 228u
#define GBA_CYCLES_PER_FRAME (GBA_CYCLES_PER_LINE * GBA_LINES_PER_FRAME)
#define GBA_HDRAW_CYCLES 1006u
#define GBA_CPU_HZ 16777216u

/* Interrupt sources (IE/IF bits). */
enum {
    GBA_IRQ_VBLANK = 1u << 0,
    GBA_IRQ_HBLANK = 1u << 1,
    GBA_IRQ_VCOUNT = 1u << 2,
    GBA_IRQ_TIMER0 = 1u << 3,
    GBA_IRQ_SERIAL = 1u << 7,
    GBA_IRQ_DMA0 = 1u << 8,
    GBA_IRQ_KEYPAD = 1u << 12,
    GBA_IRQ_GAMEPAK = 1u << 13,
};

/* Keypad bits (KEYINPUT is active low; the API takes active-high masks). */
enum {
    GBA_KEY_A = 1u << 0,
    GBA_KEY_B = 1u << 1,
    GBA_KEY_SELECT = 1u << 2,
    GBA_KEY_START = 1u << 3,
    GBA_KEY_RIGHT = 1u << 4,
    GBA_KEY_LEFT = 1u << 5,
    GBA_KEY_UP = 1u << 6,
    GBA_KEY_DOWN = 1u << 7,
    GBA_KEY_R = 1u << 8,
    GBA_KEY_L = 1u << 9,
};

/* Scheduled events. Each has an absolute cycle time; UINT64_MAX = idle. */
enum {
    GBA_EVENT_HBLANK,   /* HBlank starts on the current line */
    GBA_EVENT_LINE_END, /* line ends, VCOUNT advances */
    GBA_EVENT_TIMER0,   /* timer overflows, 4 slots */
    GBA_EVENT_TIMER1,
    GBA_EVENT_TIMER2,
    GBA_EVENT_TIMER3,
    GBA_EVENT_DMA,      /* a DMA channel's start delay has elapsed */
    GBA_EVENT_IRQ,      /* pending IRQ becomes visible to the CPU */
    GBA_EVENT_APU,      /* next audio output sample (every 512 cycles) */
    GBA_EVENT_COUNT,
};

typedef enum {
    GBA_SAVE_NONE,
    GBA_SAVE_SRAM,
    GBA_SAVE_FLASH64,
    GBA_SAVE_FLASH128,
    GBA_SAVE_EEPROM,
} GbaSaveType;

typedef struct GbaTimer {
    uint16_t reload;
    uint16_t control;
    uint16_t counter;       /* value at `since` */
    uint64_t since;         /* cycle the counter value was captured */
    bool running;           /* enabled and not in count-up mode */
} GbaTimer;

typedef struct GbaDma {
    uint32_t sad, dad;       /* programmed registers */
    uint16_t count;
    uint16_t control;
    uint32_t src, dst;       /* internal running addresses */
    uint32_t remaining;      /* internal count */
    uint32_t latch;          /* last value transferred (DMA open bus) */
    bool pending;            /* waiting for its start delay */
    uint64_t start_at;
} GbaDma;

typedef struct GbaApuChannel {
    bool enabled;
    uint8_t volume;
    uint8_t env_period, env_timer;
    bool env_up;
    uint16_t length;
    uint32_t period, timer; /* cycles per waveform step */
    uint32_t pos;           /* duty / wave position */
    uint32_t lfsr;          /* noise */
} GbaApuChannel;

typedef struct GbaFifo {
    int8_t data[32];
    uint32_t read, count;
    int8_t sample; /* currently output */
    uint32_t underflows, overflows; /* pops from an empty FIFO, bytes dropped */
} GbaFifo;

#define GBA_APU_RING 8192u /* output frames, power of two */
#define GBA_APU_RATE 32768u

/* Output-stage DC blocker state: previous input and output (Q16). */
typedef struct GbaDcBlock {
    int32_t prev_in;
    int64_t out;
} GbaDcBlock;

typedef struct GbaApu {
    GbaApuChannel ch[4];
    GbaFifo fifo[2];
    GbaDcBlock dc[2];
    uint8_t wave[32]; /* two 16-byte banks */
    uint32_t sweep_freq;
    uint8_t sweep_period, sweep_timer;
    uint32_t seq_cycles, seq_step;
    int16_t out[GBA_APU_RING * 2];
    uint32_t out_read, out_write;
} GbaApu;

typedef struct GbaEeprom {
    uint8_t in[96];  /* bits of the request being received */
    uint32_t in_count;
    uint8_t out[68]; /* bits of the pending read reply */
    uint32_t out_pos;
    uint32_t width;  /* address bits seen (6 or 14), 0 before the first request */
} GbaEeprom;

/* Cartridge GPIO port with a Seiko S-3511 real-time clock (gba_rtc.c). */
typedef struct GbaRtc {
    bool present;      /* detected from the ROM when it is loaded */
    bool wall_clock;   /* time from the host's local clock; else emulated time */
    int64_t start;     /* emulated clock: seconds since 2000-01-01 at reset */
    int64_t offset;    /* seconds the game moved the clock by (persisted) */
    uint8_t control;   /* status register (persisted) */
    uint8_t gpio_data, gpio_dir, gpio_ctrl;
    uint8_t pins;      /* SCK, SIO, CS levels the chip last saw */
    uint8_t sio_out;   /* bit the chip drives on SIO while it is read */
    uint8_t state;     /* see gba_rtc.c */
    uint8_t command;
    uint8_t shift, bits;
    uint8_t buf[7];
    uint8_t length, index; /* parameter bytes in the command, current byte */
} GbaRtc;

#define GBA_RTC_STATE_SIZE 16 /* trailer appended to the .sav */

typedef struct GbaMachine {
    GbaCpu cpu; /* must stay first */

    /* Memory. */
    uint8_t bios[GBA_BIOS_SIZE];
    uint8_t ewram[GBA_EWRAM_SIZE];
    uint8_t iwram[GBA_IWRAM_SIZE];
    uint8_t io[GBA_IO_SIZE]; /* backing store for plain registers */
    uint8_t palette[GBA_PALETTE_SIZE];
    uint8_t vram[GBA_VRAM_SIZE];
    uint8_t oam[GBA_OAM_SIZE];
    uint8_t *rom;
    uint32_t rom_size;
    uint32_t rom_mask; /* power-of-two size - 1 for mirroring reads */
    uint8_t save[0x20000];
    GbaSaveType save_type;
    bool save_dirty;
    /* Flash chip command state (see gba_bus.c). */
    uint8_t flash_stage;
    uint8_t flash_bank;
    bool flash_id_mode;
    bool flash_erase_armed;
    bool flash_program_next;
    bool flash_bank_next;
    GbaEeprom eeprom;
    GbaRtc rtc;

    /* Bus state. */
    bool bios_hle;       /* built-in BIOS: SWIs are performed in C */
    uint32_t bios_latch; /* last opcode fetched from BIOS */
    uint32_t memcnt;     /* 0x04000800 internal memory control */
    uint8_t wait_n16[16], wait_s16[16], wait_n32[16], wait_s32[16];
    uint32_t last_code_region;

    /* Game Pak prefetch buffer (WAITCNT bit 14; gba_cart_access in
     * gba_bus.c): while the cartridge bus is idle it reads the halfwords
     * after the last opcode fetched from ROM, up to eight. */
    bool pf_enabled;
    bool pf_full;       /* it filled up: stopped until the CPU has taken all 8 */
    bool pf_following;  /* reading went past its first cycle since the last restart */
    bool pf_boundary;   /* the read in progress is at a 128 KiB boundary (never completes) */
    bool pf_force_nseq; /* the next opcode fetch from ROM is non-sequential */
    uint8_t pf_count;   /* halfwords buffered */
    uint8_t pf_fetch;   /* cycles into the read in progress, 0 = none */
    uint8_t pf_wait;    /* cycles that read takes */
    uint32_t pf_check;  /* next halfword the CPU can take from the prefetcher, 0 = none */
    uint32_t pf_read;   /* halfword being read, or read next */
    uint64_t pf_at;     /* first cycle the prefetcher has not been run for */
    /* A data access came after the last opcode fetch, so the next one is
     * non-sequential (set by the bus, used by gba_access_cycles). */
    bool fetch_nseq;

    /* Interrupts and power. */
    uint16_t ie, if_, ime;
    bool halted;
    bool stopped;
    bool intr_wait_active;     /* HLE IntrWait in progress (see gba_bios.c) */
    uint32_t intr_wait_return; /* ...for the SWI with this return address */
    uint8_t attention; /* see gba_update_attention() */

    /* Video timing. */
    uint16_t vcount;
    uint16_t dispstat;
    uint64_t line_start; /* cycle the current line began */
    uint64_t frame_count;

    GbaTimer timer[4];
    GbaDma dma[4];
    GbaApu apu;
    int dma_active; /* channel being transferred, or -1 */

    /* Scheduler. */
    uint64_t event_at[GBA_EVENT_COUNT];
    uint64_t next_event;

    /* Keypad (active-low KEYINPUT value). */
    uint16_t keyinput;

    /* mGBA debug output port (0x04FFF600-0x04FFF7FF). */
    bool debug_enabled;
    char debug_string[0x101];
    void (*debug_log)(struct GbaMachine *m, int level, const char *text);
    /* Called before each interpreted instruction when set (tracing). */
    void (*step_hook)(struct GbaMachine *m);

    /* Affine BG2/BG3 internal reference points (20.8 fixed point). */
    int32_t ppu_ref_x[2], ppu_ref_y[2];

    /* Frame output: BGR555 converted to 0xAARRGGBB by the PPU. */
    uint32_t framebuffer[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];
    bool frame_ready;

    /* Recompiled program (gba_recomp.h), or NULL to interpret everything. */
    const struct GbaRcProgram *rc_program;
    bool pipe_stale; /* recompiled code ran; pipe[] must be primed before interpreting */
    bool in_native;  /* recompiled code is executing (pipe[] is not maintained) */
    bool in_events;  /* gba_process_events is running */
    uint8_t rc_code_pages[1152]; /* RAM pages holding compiled code: EWRAM 0-1023, IWRAM 1024+ */
    uint64_t rc_ram_gen;         /* bumped by writes to those pages */
    uint64_t *rc_verified;       /* per RAM block: generation it was last found intact */
    uint32_t *rc_index;          /* entry lookup hash: entry index + 1, 0 = empty (gba_recomp.c) */
    uint32_t rc_index_shift, rc_index_mask;

    /* Diagnostics. */
    uint64_t instructions;
    uint64_t interpreted_instructions;
    struct GbaCoverage *coverage; /* optional, see gba_coverage.c */

    /* Link cable (gba_link.c). These stay last: a save state from before
     * they existed is the struct up to here, and still loads. */
    uint64_t sio_at;       /* next link event (a scheduler slot outside event_at) */
    struct GbaLink *link;  /* process-owned; NULL = no cable, serial registers are plain storage */
} GbaMachine;

#define GBA_MACHINE(cpu_ptr) ((GbaMachine *)(void *)(cpu_ptr))

/* Cycles for one access with the waitstates in effect, prefetch buffer
 * aside. A sequential ROM burst cannot cross a 128 KiB boundary. */
static inline uint32_t gba_wait_cycles(const GbaMachine *m, uint32_t region, uint32_t addr, bool word, bool seq)
{
    if (seq && region >= 8 && region <= 13 && (addr & 0x1FFFFu) == 0) seq = false;
    if (word) return seq ? m->wait_s32[region] : m->wait_n32[region];
    return seq ? m->wait_s16[region] : m->wait_n16[region];
}

/* A cartridge (ROM or SRAM) access while the prefetch buffer is enabled
 * (gba_bus.c). */
void gba_cart_access(GbaMachine *m, uint32_t addr, bool word, int access);

/* Cycles for one bus access (the bus access flags from gba_bus.h: bit 0 =
 * sequential, bit 1 = opcode fetch). Shared by the bus and by recompiled
 * code, which charges its instruction fetches through this so both stay in
 * cycle lockstep. An opcode fetch the CPU marks sequential is still a
 * non-sequential access when a data access came in between. */
static inline void gba_access_cycles(GbaMachine *m, uint32_t addr, bool word, int access)
{
    uint32_t region = addr >> 24;
    if (region > 15) region = 1; /* unmapped: 1 cycle */
    if (access & GBA_ACCESS_CODE) {
        if (m->fetch_nseq) access &= ~GBA_ACCESS_SEQ;
        m->fetch_nseq = false;
    }
    if (m->pf_enabled && region >= 8) {
        gba_cart_access(m, addr, word, access);
        return;
    }
    m->cpu.cycles += gba_wait_cycles(m, region, addr, word, (access & GBA_ACCESS_SEQ) != 0);
}

/* attention: bit 0 = an enabled interrupt is pending (IME && IE & IF),
 * bit 1 = halted or stopped, bit 2 = a frame completed (cleared by the run
 * loop when native code returns), bit 3 = stalled on the link cable: the
 * machine may not run until the other players catch up (gba_link.h; the run
 * loop returns to its caller until gba_link_wait clears it). Recompiled code
 * returns to the run loop when it is set (see gba_recomp.h); call after
 * changing IME, IE, IF or the power state. */
#define GBA_ATTENTION_FRAME 4u
#define GBA_ATTENTION_STALL 8u
static inline void gba_update_attention(GbaMachine *m)
{
    m->attention = (uint8_t)((m->attention & (GBA_ATTENTION_FRAME | GBA_ATTENTION_STALL)) |
                             (((m->ime & 1u) && (m->ie & m->if_)) ? 1u : 0u) |
                             ((m->halted || m->stopped) ? 2u : 0u));
}

GbaMachine *gba_create(void);
void gba_destroy(GbaMachine *m);

/* ROM data is copied; returns false on an invalid size. */
bool gba_load_rom(GbaMachine *m, const uint8_t *data, size_t size);
bool gba_load_bios(GbaMachine *m, const uint8_t *data, size_t size);

/* Reset. With skip_bios the CPU and I/O start in the state the BIOS leaves
 * when it jumps to the cartridge. */
void gba_reset(GbaMachine *m, bool skip_bios);

/* Run until at least `cycles` more cycles have elapsed (recompiled code
 * where available, the interpreter elsewhere). */
void gba_run_cycles(GbaMachine *m, uint64_t cycles);

/* One run-loop iteration; a halted CPU sleeps no further than `limit`. */
void gba_run_slice(GbaMachine *m, uint64_t limit);

/* Run until the next frame completes (VBlank start). With a link cable
 * attached these also return early while the machine is stalled waiting
 * for the other players (frame_ready still false; see gba_link.h). */
void gba_run_frame(GbaMachine *m);

void gba_set_keys(GbaMachine *m, uint16_t pressed_mask);

/* Save states (gba_state.c), taken and loaded between frames. A state
 * loads only into a machine running the same ROM, built with the same
 * GbaMachine layout; load returns false otherwise and changes nothing.
 * Bump GBA_STATE_VERSION when the meaning of a field changes. */
#define GBA_STATE_VERSION 1u
size_t gba_state_size(void);
void gba_state_save(const GbaMachine *m, uint8_t *out); /* gba_state_size() bytes */
bool gba_state_load(GbaMachine *m, const uint8_t *in, size_t size);
bool gba_state_save_file(const GbaMachine *m, const char *path);
bool gba_state_load_file(GbaMachine *m, const char *path);

/* Raise interrupt flags (IF |= bits). */
void gba_raise_irq(GbaMachine *m, uint16_t bits);

/* Side-effect-free read for debuggers, tracers and test harnesses. */
uint32_t gba_peek32(GbaMachine *m, uint32_t addr);
uint8_t gba_peek8(GbaMachine *m, uint32_t addr);

/* Internal entry points shared between runtime files. */
void gba_schedule(GbaMachine *m, int event, uint64_t at);
void gba_schedule_sio(GbaMachine *m, uint64_t at); /* the link cable's slot, sio_at */
void gba_process_events(GbaMachine *m);
uint16_t gba_io_read16(GbaMachine *m, uint32_t offset);
void gba_io_write16(GbaMachine *m, uint32_t offset, uint16_t value, uint16_t byte_mask);
void gba_update_waitstates(GbaMachine *m);
void gba_prime_pipeline(GbaMachine *m);
uint32_t gba_open_bus(GbaMachine *m);

void gba_timer_write_control(GbaMachine *m, int index, uint16_t value);
void gba_timer_write_reload(GbaMachine *m, int index, uint16_t value);
uint16_t gba_timer_read_counter(GbaMachine *m, int index);
void gba_timer_overflow_event(GbaMachine *m, int index);

void gba_dma_write_control(GbaMachine *m, int index, uint16_t value);
void gba_dma_trigger(GbaMachine *m, int timing); /* 1 = VBlank, 2 = HBlank, 3 = special */
void gba_dma_run_pending(GbaMachine *m);

void gba_ppu_render_line(GbaMachine *m, int line);

/*
 * Rotated and scaled layers at a higher resolution. With a scale of 2 to
 * GBA_AFFINE_SCALE_MAX, a line that shows a rotated or scaled background
 * (BG2 and BG3 of modes 1 and 2, the bitmap of modes 3 to 5) or sprite is
 * drawn a second time with that many samples of it to a pixel each way,
 * into a second picture `scale` times the size. `framebuffer`, and
 * everything a game can read, is drawn exactly as before: the larger
 * picture is for the display.
 *
 * gba_ppu_large_frame() is the last complete frame's larger picture, 240 *
 * scale pixels wide (0xFFRRGGBB, a row every *width pixels), or NULL when
 * that frame had no such line (then `framebuffer` is all there is). Tiled
 * backgrounds, ordinary sprites and lines without a rotated or scaled
 * layer are the console's pixels, repeated. Lines with a mosaic or green
 * swap, and frames drawn while a texture pack is recording, are not drawn
 * larger; an OBJ window keeps the console's outline.
 *
 * The larger picture is kept beside the machine, not in it (a save state
 * is the machine's bytes), and one machine at a time has one. Returns
 * false, changing nothing, for a scale out of range or without memory.
 */
#define GBA_AFFINE_SCALE_MAX 4
bool gba_ppu_set_affine_scale(GbaMachine *m, int scale);
int gba_ppu_affine_scale(const GbaMachine *m);
const uint32_t *gba_ppu_large_frame(const GbaMachine *m, int *width, int *height);

/* Audio (gba_apu.c). read_samples drains interleaved stereo int16 frames
 * at GBA_APU_RATE. */
void gba_apu_reset(GbaMachine *m);
void gba_apu_sample_event(GbaMachine *m);
void gba_apu_timer_overflow(GbaMachine *m, int timer);
uint16_t gba_apu_read16(GbaMachine *m, uint32_t offset);
void gba_apu_write16(GbaMachine *m, uint32_t offset, uint16_t value, uint16_t byte_mask);
void gba_apu_fifo_write(GbaMachine *m, int fifo, uint32_t value, int bytes);
size_t gba_apu_read_samples(GbaMachine *m, int16_t *out, size_t max_frames);
void gba_apu_report(const GbaMachine *m); /* FIFO underflow/overflow counts, if any */
void gba_dma_sound_request(GbaMachine *m, int fifo);

/* EEPROM saves (gba_eeprom.c). */
bool gba_eeprom_address(const GbaMachine *m, uint32_t addr);

/* Real-time clock (gba_rtc.c). The GPIO registers sit at 0x080000C4-C9 in
 * every ROM mirror; they read back only once the game enables reading
 * (0x080000C8 bit 0), otherwise ROM data shows through. */
static inline bool gba_gpio_address(const GbaMachine *m, uint32_t addr)
{
    return m->rtc.present && (addr & 0x01FFFFFFu) - 0xC4u < 6u;
}
bool gba_rtc_detect(const uint8_t *rom, size_t size);
void gba_rtc_reset(GbaMachine *m);
uint32_t gba_gpio_read(GbaMachine *m, uint32_t addr, int size, uint32_t rom_value);
void gba_gpio_write(GbaMachine *m, uint32_t addr, uint32_t value, int size);
int64_t gba_rtc_now(const GbaMachine *m); /* seconds since 2000-01-01 00:00, as the game sees them */
bool gba_rtc_parse_time(const char *text, int64_t *seconds); /* "YYYY-MM-DD[ HH:MM[:SS]]" */
void gba_rtc_save_state(const GbaMachine *m, uint8_t out[GBA_RTC_STATE_SIZE]);
bool gba_rtc_load_state(GbaMachine *m, const uint8_t *in, size_t size);
/* Serial port with a link cable attached (gba_link.c): SIOCNT and
 * SIOMLT_SEND go through these, and sio_at runs gba_sio_event. */
uint16_t gba_sio_read_cnt(GbaMachine *m);
void gba_sio_write(GbaMachine *m, uint32_t offset, uint16_t value, uint16_t byte_mask);
void gba_sio_event(GbaMachine *m);
void gba_sio_state_loaded(GbaMachine *m);
void gba_eeprom_write_bit(GbaMachine *m, uint32_t bit);
uint32_t gba_eeprom_read_bit(GbaMachine *m);
void gba_eeprom_request_done(GbaMachine *m);
void gba_ppu_frame_start(GbaMachine *m);                        /* VBlank: reload BG2/3 reference points */
void gba_ppu_reference_written(GbaMachine *m, uint32_t offset); /* BGxX/BGxY written */

/* Built-in BIOS (gba_bios.c). install writes the vector/IRQ-dispatch image;
 * hle_swi performs the call the CPU just trapped into at 0x08. */
void gba_bios_install_hle(GbaMachine *m);

/* Coverage of recompiled code (gba_coverage.c): counts interpreted
 * instructions per memory region, and records where interpreted code was
 * entered (and the RAM it ran from), for gbarecomp --trace. */
void gba_coverage_enable(GbaMachine *m);
void gba_coverage_free(GbaMachine *m);
void gba_coverage_note(GbaMachine *m, uint32_t key);
void gba_coverage_entry(GbaMachine *m, uint32_t key); /* execution resumes at key */
void gba_coverage_branched(GbaMachine *m);            /* the next interpreted instruction is an entry */
void gba_coverage_report(const GbaMachine *m);
bool gba_coverage_write_trace(const GbaMachine *m, const char *path);
bool gba_bios_hle_swi(GbaMachine *m);

#ifdef __cplusplus
}
#endif

#endif /* GBA_H */
