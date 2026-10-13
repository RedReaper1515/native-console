/*
 * Nintendo DS machine: two CPUs, their memory maps, I/O, timers, DMA,
 * interrupts, the card and video timing. The interpreter and recompiled code
 * both run against this struct. The ARM9 is its first member and the ARM7
 * its second, so each processor's bus callbacks recover the machine from
 * their NdsCpu pointer at no cost.
 *
 * Time. Everything is scheduled in system cycles of the 33.51 MHz clock
 * (nds_common.h). arm7.cycles counts those directly; arm9.cycles counts the
 * ARM9's own clock, which is twice as fast, so the ARM9's position in system
 * time is arm9.cycles / 2.
 *
 * The two CPUs take turns in slices. A slice ends at the next scheduled
 * event at the latest, and is cut short the moment one CPU does something
 * the other must see (an interrupt request across the IPC, say), so the
 * other catches up to that point before anything else happens. Each CPU
 * always stops on an instruction boundary, whether it is being interpreted
 * or running recompiled code, which keeps the two modes in lockstep.
 */
#ifndef NDS_H
#define NDS_H

#include "nds_2d.h"
#include "nds_bus.h"
#include "nds_common.h"
#include "nds_cpu.h"
#include "nds_firmware.h"
#include "nds_gx.h"
#include "nds_rtc.h"
#include "nds_save.h"
#include "nds_spi.h"
#include "nds_spu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NDS_MAIN_RAM_SIZE 0x400000u
#define NDS_SHARED_WRAM_SIZE 0x8000u
#define NDS_ARM7_WRAM_SIZE 0x10000u
#define NDS_ITCM_SIZE 0x8000u
#define NDS_DTCM_SIZE 0x4000u
#define NDS_VRAM_SIZE 0xA4000u /* blocks A-I */
#define NDS_PALETTE_SIZE 0x800u
#define NDS_OAM_SIZE 0x800u
#define NDS_BIOS9_SIZE 0x1000u
#define NDS_BIOS7_SIZE 0x4000u
#define NDS_ROM_MAX 0x20000000u /* 512 MiB */

/* Where each VRAM block starts inside NdsMachine.vram. */
#define NDS_VRAM_A 0x00000u
#define NDS_VRAM_B 0x20000u
#define NDS_VRAM_C 0x40000u
#define NDS_VRAM_D 0x60000u
#define NDS_VRAM_E 0x80000u
#define NDS_VRAM_F 0x90000u
#define NDS_VRAM_G 0x94000u
#define NDS_VRAM_H 0x98000u
#define NDS_VRAM_I 0xA0000u

/* The HBlank flag goes up 1606 cycles into the line on the ARM9 (GBATEK,
 * "DS Video Stuff"); the line is rendered, and HBlank DMA and interrupts
 * happen, at that point. */
#define NDS_HBLANK_CYCLES 1606u

/* How far one CPU may run ahead of the other while both are running. */
#define NDS_SYNC_CYCLES 32u

/* What an access costs, in each CPU's own cycles. The ARM9's figures are an
 * average over its caches, not a model of them: see docs/NDS-PLAN.md, P8. */
#define NDS9_CYCLES_TCM 1u       /* ITCM, DTCM */
#define NDS9_CYCLES_CODE 2u      /* an opcode from main RAM */
#define NDS9_CYCLES_DATA 2u      /* a data access to main RAM */
#define NDS9_CYCLES_BUS 4u       /* anything else: I/O, VRAM, WRAM, BIOS */
#define NDS7_CYCLES_ACCESS 1u

/* Recompiled code (nds_recomp.h) can run from four pieces of RAM. They are
 * laid end to end in one "code space", so that one generation counter per
 * 4 KiB page and one bit per word cover them all. */
#define NDS_RC_MAIN 0x000000u      /* main RAM */
#define NDS_RC_ITCM 0x400000u
#define NDS_RC_SHARED 0x408000u    /* shared WRAM */
#define NDS_RC_ARM7_WRAM 0x410000u
#define NDS_RC_SPACE 0x420000u
#define NDS_RC_PAGES (NDS_RC_SPACE >> 12)
/* Entries in each CPU's cache of the last function found for an address. */
#define NDS_RC_CACHE 4096u

/* Sound output kept for a frontend to collect: stereo frames, about a
 * quarter of a second. When nobody collects, the oldest are dropped. */
#define NDS_AUDIO_RING 8192u

/* Interrupt sources (IE/IF bits). */
enum {
    NDS_IRQ_VBLANK = 1u << 0,
    NDS_IRQ_HBLANK = 1u << 1,
    NDS_IRQ_VCOUNT = 1u << 2,
    NDS_IRQ_TIMER0 = 1u << 3,
    NDS_IRQ_RTC = 1u << 7,       /* ARM7 */
    NDS_IRQ_DMA0 = 1u << 8,
    NDS_IRQ_KEYPAD = 1u << 12,
    NDS_IRQ_GBA_SLOT = 1u << 13,
    NDS_IRQ_IPC_SYNC = 1u << 16,
    NDS_IRQ_IPC_SEND_EMPTY = 1u << 17,
    NDS_IRQ_IPC_RECV = 1u << 18,
    NDS_IRQ_CARD_DONE = 1u << 19,
    NDS_IRQ_CARD_IREQ = 1u << 20,
    NDS_IRQ_GXFIFO = 1u << 21,   /* ARM9 */
    NDS_IRQ_LID = 1u << 22,      /* ARM7 */
    NDS_IRQ_SPI = 1u << 23,      /* ARM7 */
    NDS_IRQ_WIFI = 1u << 24,     /* ARM7 */
};

/* Buttons, as a mask of what is pressed. Bits 0-9 are KEYINPUT's order; X
 * and Y follow (they are on the ARM7's EXTKEYIN). */
enum {
    NDS_KEY_A = 1u << 0,
    NDS_KEY_B = 1u << 1,
    NDS_KEY_SELECT = 1u << 2,
    NDS_KEY_START = 1u << 3,
    NDS_KEY_RIGHT = 1u << 4,
    NDS_KEY_LEFT = 1u << 5,
    NDS_KEY_UP = 1u << 6,
    NDS_KEY_DOWN = 1u << 7,
    NDS_KEY_R = 1u << 8,
    NDS_KEY_L = 1u << 9,
    NDS_KEY_X = 1u << 10,
    NDS_KEY_Y = 1u << 11,
    NDS_KEY_DEBUG = 1u << 12,
};

enum { NDS_ARM9 = 0, NDS_ARM7 = 1 };

/* Scheduled events. Each has an absolute time in system cycles;
 * UINT64_MAX = idle. */
enum {
    NDS_EVENT_HBLANK,   /* HBlank starts on the current line */
    NDS_EVENT_LINE_END, /* the line ends, VCOUNT advances */
    NDS_EVENT_TIMER9_0, /* a timer overflows: four per CPU */
    NDS_EVENT_TIMER9_1,
    NDS_EVENT_TIMER9_2,
    NDS_EVENT_TIMER9_3,
    NDS_EVENT_TIMER7_0,
    NDS_EVENT_TIMER7_1,
    NDS_EVENT_TIMER7_2,
    NDS_EVENT_TIMER7_3,
    NDS_EVENT_CARD,     /* the card has the next data word ready */
    NDS_EVENT_GX,       /* the geometry engine finishes a command something is waiting on */
    NDS_EVENT_SOUND,    /* one mixer period: every NDS_CYCLES_PER_SAMPLE cycles */
    NDS_EVENT_COUNT,
};

typedef struct NdsTimer {
    uint16_t reload;
    uint16_t control;
    uint16_t counter; /* value at `since` */
    uint64_t since;   /* system cycle the counter value was captured */
    bool running;     /* enabled and not in count-up mode */
} NdsTimer;

typedef struct NdsDma {
    uint32_t control;   /* DMAxCNT as last written: count in the low bits, control above */
    uint32_t src, dst;  /* internal running addresses */
    uint32_t remaining; /* internal count */
} NdsDma;

/* One direction of the IPC FIFO: sixteen words. */
typedef struct NdsIpcFifo {
    uint32_t data[16];
    uint8_t read, count;
    uint32_t last; /* the word most recently read, returned when empty */
} NdsIpcFifo;

/* The game card's ROM interface (nds_card.c). */
enum { NDS_CARD_REPLY_DATA, NDS_CARD_REPLY_CHIP_ID, NDS_CARD_REPLY_NOTHING };

typedef struct NdsCard {
    uint16_t auxspicnt[2]; /* per CPU */
    uint32_t romctrl[2];
    uint8_t command[2][8];
    uint8_t auxspi_data;   /* what the last AUXSPI transfer returned */
    uint8_t owner;         /* CPU that started the transfer in progress */
    uint32_t address;      /* next ROM byte of the transfer in progress */
    uint32_t remaining;    /* bytes of it still to deliver */
    uint32_t word;         /* the word waiting in the data register */
    bool word_ready;
    uint8_t reply;         /* what the transfer returns: NDS_CARD_REPLY_* */
    uint32_t word_cycles;  /* system cycles per word at the selected rate */
} NdsCard;

/* Where each part of VRAM is mapped right now: pointers into
 * NdsMachine.vram, 16 KiB pages unless noted, NULL where nothing is mapped.
 * Derived from the VRAMCNT registers by nds_vram_remap(), so it is rebuilt,
 * not saved. */
typedef struct NdsVramMap {
    uint8_t *lcdc[64];      /* 0x06800000: the blocks one after another (41 pages used) */
    uint8_t *bg_a[32];      /* engine A BG, 512 KiB */
    uint8_t *obj_a[16];     /* engine A OBJ, 256 KiB */
    uint8_t *bg_b[8];       /* engine B BG, 128 KiB */
    uint8_t *obj_b[8];      /* engine B OBJ, 128 KiB */
    uint8_t *arm7[16];      /* the ARM7's 0x06000000, 256 KiB */
    uint8_t *texture[4];    /* texture image slots, 128 KiB each */
    uint8_t *tex_pal[6];    /* texture palette slots, 16 KiB each */
    uint8_t *bg_ext_a[4];   /* engine A BG extended palette slots, 8 KiB each */
    uint8_t *bg_ext_b[4];
    uint8_t *obj_ext_a;     /* OBJ extended palettes, 8 KiB */
    uint8_t *obj_ext_b;
} NdsVramMap;

struct NdsMachine;
struct NdsRcProgram;
struct NdsRcBlockState;
/* A recompiled function: see nds_recomp.h. */
typedef void (*NdsRcFunc)(struct NdsMachine *m, uint32_t entry);

/* The last function found for an entry key. A key of all ones is "empty". */
struct NdsRcCacheEntry {
    uint32_t key;
    uint32_t block;
    NdsRcFunc fn;
};

typedef struct NdsMachine {
    NdsCpu arm9; /* must stay first */
    NdsCpu arm7; /* must stay second */

    /* Memory. */
    uint8_t main_ram[NDS_MAIN_RAM_SIZE];
    uint8_t shared_wram[NDS_SHARED_WRAM_SIZE];
    uint8_t arm7_wram[NDS_ARM7_WRAM_SIZE];
    uint8_t itcm[NDS_ITCM_SIZE];
    uint8_t dtcm[NDS_DTCM_SIZE];
    uint8_t vram[NDS_VRAM_SIZE];
    uint8_t palette[NDS_PALETTE_SIZE];
    uint8_t oam[NDS_OAM_SIZE];
    uint8_t bios9[NDS_BIOS9_SIZE];
    uint8_t bios7[NDS_BIOS7_SIZE];
    uint8_t io9[0x800];  /* backing store for plain ARM9 registers, 0x04000000-0x040007FF */
    uint8_t io7[0x800];  /* backing store for plain ARM7 registers */
    uint8_t wifi[0x8000]; /* 0x04800000: Wi-Fi registers and RAM, plain storage for now */

    /* The card. rom is the whole image; rom_mask covers its size rounded
     * up to a power of two. */
    uint8_t *rom;
    uint32_t rom_size;
    uint32_t rom_mask;
    uint32_t chip_id;
    NdsCard card;

    /* Memory control. */
    uint8_t vramcnt[9];   /* VRAMCNT_A..I */
    uint8_t wramcnt;
    uint16_t exmemcnt[2]; /* the ARM9's register and the ARM7's view of it */
    uint8_t postflg[2];
    uint16_t powcnt1;     /* ARM9: graphics power, display swap */
    uint16_t powcnt2;     /* ARM7: sound, Wi-Fi */
    uint32_t biosprot;    /* ARM7: BIOS addresses below this read as ones from outside it */

    /* Interrupts and power, per CPU. */
    uint32_t ie[2], if_[2];
    uint8_t ime[2];
    bool irq_line[2]; /* IME and (IE & IF): the CPU takes an interrupt when its I flag allows */
    bool halted[2];   /* stopped until IE & IF is non-zero */
    /* Held in mid-instruction by a device: the ARM9, while the geometry
     * command FIFO has no room for what it wrote. Nothing wakes it but the
     * device. */
    bool stalled[2];

    /* Video timing. */
    uint16_t vcount;
    uint16_t dispstat[2]; /* each CPU has its own enables and match line; the flags are shared */
    int32_t vcount_write; /* a value written to VCOUNT, taken at the next line; -1 = none */
    uint64_t line_start;  /* system cycle the current line began */
    uint64_t frame_count;
    bool frame_ready;

    NdsTimer timer[2][4];
    NdsDma dma[2][4];

    /* Video (nds_video.c): the two 2D engines, A then B, and the 3D engine
     * with the system time it has been run to. */
    Nds2d engine[2];
    NdsGx gx;
    uint64_t gx_time;
    bool gx_in_dma; /* a geometry-FIFO DMA is feeding the engine right now */

    /* IPC: ipc_fifo[n] carries words sent by CPU n. */
    uint16_t ipcsync[2];
    uint16_t ipcfifocnt[2];
    NdsIpcFifo ipc_fifo[2];

    /* Maths unit (ARM9). */
    uint16_t divcnt, sqrtcnt;
    uint64_t div_numer, div_denom, div_result, div_remainder;
    uint64_t sqrt_param;
    uint32_t sqrt_result;

    /* The ARM7's serial devices: the SPI bus (power management, firmware
     * flash, touch screen controller) and the real-time clock. The firmware
     * image is built from `settings` (nds_firmware.h): no Nintendo code. */
    NdsUserSettings settings;
    /* The touch calibration the running program was started with: what a
     * pixel is turned into controller readings by. settings can be replaced
     * while it runs; this follows at the next reset. */
    NdsUserSettings touch_calibration;
    uint8_t firmware[NDS_FIRMWARE_SIZE];
    bool firmware_dirty; /* the program wrote the flash since this was last cleared */
    NdsSpi spi;
    NdsRtc rtc;
    /* What the clock reads at reset, in seconds since 1970-01-01 00:00 of
     * its own (local) time; emulated time is added to it. */
    int64_t rtc_base;
    bool powered_off; /* the program switched the console off */

    /* Sound (nds_spu.c): one stereo frame every NDS_CYCLES_PER_SAMPLE
     * cycles, 32,728.5 Hz, into a ring a frontend reads from. */
    NdsSpu spu;
    int16_t audio_ring[NDS_AUDIO_RING][2];
    uint32_t audio_written; /* frames produced, counted from reset */
    uint32_t audio_taken;   /* frames a frontend has collected or that were dropped */

    /* The card's backup memory on the AUXSPI port. */
    NdsSave save;
    uint8_t save_type;     /* NdsSaveType: from the table, or NDS_SAVE_UNKNOWN until the program shows it */
    uint8_t *save_data;    /* save_capacity bytes */
    uint32_t save_capacity;
    uint32_t save_size;    /* the chip's size once known, else 0 */
    bool save_dirty;       /* the memory changed since this was last cleared */

    /* Keys, touch screen, lid. */
    uint16_t keyinput;  /* active-low, as the register reads */
    uint16_t extkeyin;
    uint16_t keycnt[2];
    bool touch_down;
    uint16_t touch_x, touch_y;
    bool lid_closed;

    /* Scheduler. now is the time both CPUs have reached; slice_end is where
     * the slice being run stops, and only ever moves earlier while it runs.
     * cpu_limit is slice_end in each CPU's own cycles, which is what its run
     * loop and its recompiled code compare against. */
    uint64_t now;
    uint64_t slice_end;
    uint64_t cpu_limit[2];
    int running; /* the CPU executing right now: NDS_ARM9, NDS_ARM7 or -1 */
    uint64_t event_at[NDS_EVENT_COUNT];
    uint64_t next_event;

    /* Frame output, 0xAARRGGBB: [0] is the top screen, [1] the bottom. */
    uint32_t framebuffer[2][NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT];
    /* The same picture with its 3D drawn larger, for a display that has the
     * pixels (nds_set_internal_scale); NULL until one is asked for. It is
     * no part of the console: nothing the game can read depends on it. */
    struct NdsFine *fine;

    /* Recompiled code (nds_recomp.h). */
    const struct NdsRcProgram *rc_program;
    uint32_t *rc_index[2];     /* per CPU: hash of entry keys -> first entry with that key, plus 1 */
    uint32_t rc_index_shift[2];
    uint32_t rc_index_mask[2];
    struct NdsRcBlockState *rc_block; /* per block: whether its code is in memory, and as of when */
    struct NdsRcCacheEntry rc_cache[2][NDS_RC_CACHE];
    uint8_t *rc_code_words;    /* one bit per word of the code space: compiled code lies there */
    /* Every write to a code word, and every change of the memory map, takes
     * the next number in one sequence; a page remembers the number of its
     * last such write. A block was checked "as of" the largest number among
     * its pages and the map, and stays checked while that does not move. */
    uint64_t rc_seq;
    uint64_t rc_page_seq[NDS_RC_PAGES];
    uint64_t rc_map_seq;
    NdsRcFunc rc_resume[2];    /* the function that yielded part-way, to be continued */
    uint32_t rc_resume_block[2];
    uint8_t rc_resume_thumb[2];
    /* Native code does not keep the prefetched opcodes: they are read back
     * (without charging the fetches again) before the interpreter runs. */
    bool pipe_stale[2];

    /* Diagnostics. */
    uint64_t instructions[2];
    uint64_t interpreted[2]; /* of those, how many the interpreter ran */
    uint64_t rc_calls[2];    /* calls into recompiled code */
    uint64_t rc_compared;    /* times a block's bytes were compared with memory */
    /* Called before each interpreted instruction when set (tracing). */
    void (*step_hook)(struct NdsMachine *m, int cpu);
    /* Called for the debug output games write to the emulator ports. */
    void (*debug_log)(struct NdsMachine *m, int cpu, const char *text);

    /* Derived from the registers above; rebuilt, not saved. */
    NdsVramMap map;
    uint8_t *wram9;       /* the ARM9's share of the shared WRAM, or NULL */
    uint32_t wram9_mask;
    uint8_t *wram7;       /* the ARM7's share, or NULL (its own WRAM shows through) */
    uint32_t wram7_mask;
    uint32_t itcm_end;    /* ITCM covers [0, itcm_end); 0 = disabled */
    uint32_t dtcm_base;
    uint32_t dtcm_size;   /* 0 = disabled */
    bool itcm_load, dtcm_load; /* load mode: writes reach the TCM, reads do not */
} NdsMachine;

#define NDS9_MACHINE(cpu_ptr) ((NdsMachine *)(void *)(cpu_ptr))
#define NDS7_MACHINE(cpu_ptr) ((NdsMachine *)(void *)((uint8_t *)(cpu_ptr) - offsetof(NdsMachine, arm7)))

/* Each CPU's position in system time. */
static inline uint64_t nds9_now(const NdsMachine *m)
{
    return m->arm9.cycles >> 1;
}

static inline uint64_t nds7_now(const NdsMachine *m)
{
    return m->arm7.cycles;
}

static inline uint64_t nds_cpu_now(const NdsMachine *m, int cpu)
{
    return cpu == NDS_ARM9 ? nds9_now(m) : nds7_now(m);
}

NdsMachine *nds_create(void);
void nds_destroy(NdsMachine *m);

/*
 * Internal resolution: how many times the screens' size, each way, the 3D
 * engine's picture is drawn at for the display: 1 (the console's own
 * picture and nothing more) to NDS_INTERNAL_SCALE_MAX.
 *
 * Above 1 the machine goes on drawing the console's own picture exactly as
 * before, into framebuffer[], and everything a game can see - display
 * capture, the 3D picture as a texture, timing - comes from that. Beside it
 * the 3D engine draws the frame again on a finer grid, and each line of the
 * screen showing it is stacked a second time around those finer pixels:
 * backgrounds and sprites stay the screen's pixels, made larger; polygons
 * get the detail.
 *
 * nds_fine_frame() is the last finished frame of a screen drawn that way,
 * 0xAARRGGBB, *width by *height, or NULL when the screen's picture is the
 * one in framebuffer[]: the scale is 1, or that screen did not show the 3D
 * engine's picture through the whole of the last two frames (a screen of
 * backgrounds and sprites alone; a game that draws 3D on both screens by
 * turns, whose other screen shows a capture).
 */
#define NDS_INTERNAL_SCALE_MAX 4
void nds_set_internal_scale(NdsMachine *m, int scale);
int nds_internal_scale(const NdsMachine *m);
const uint32_t *nds_fine_frame(const NdsMachine *m, int screen, int *width, int *height);

/* ROM data is copied; returns false on a size or header the machine cannot
 * use. */
bool nds_load_rom(NdsMachine *m, const uint8_t *data, size_t size);
/* Replace a BIOS image (the built-in ones are the BSD-licensed replacement
 * BIOS). size may be less than the ROM's size. */
bool nds_load_bios9(NdsMachine *m, const uint8_t *data, size_t size);
bool nds_load_bios7(NdsMachine *m, const uint8_t *data, size_t size);

/* Reset to the state the firmware leaves when it starts the game in the
 * card slot: both binaries loaded, the header and user settings in RAM, the
 * CPUs at their entry points. */
void nds_reset(NdsMachine *m);

/* Run until the next frame completes (the start of VBlank). */
void nds_run_frame(NdsMachine *m);
/* Run until system time reaches `until`. */
void nds_run_until(NdsMachine *m, uint64_t until);
/* One step of the scheduler: both CPUs to the next event, to `limit`, or to
 * the point one of them had to let the other catch up, whichever is first. */
void nds_run_slice(NdsMachine *m, uint64_t limit);

/* Input. pressed is a mask of NDS_KEY_*. Touch coordinates are bottom-screen
 * pixels. The microphone level is a 12-bit unsigned sample, silence at
 * 0x800. */
void nds_set_keys(NdsMachine *m, uint32_t pressed);
void nds_set_touch(NdsMachine *m, bool down, int x, int y);
void nds_set_lid(NdsMachine *m, bool closed);
void nds_set_microphone(NdsMachine *m, uint16_t sample12);

/* Sound: up to max_frames stereo frames (left, right) produced since the
 * last call, oldest first. Returns how many were written to out. The rate is
 * NDS_CLOCK_HZ / NDS_CYCLES_PER_SAMPLE frames a second. */
size_t nds_audio_read(NdsMachine *m, int16_t *out, size_t max_frames);

/* The owner's settings (nickname, language, touch calibration): rebuilds the
 * firmware image. Takes effect at the next nds_reset(). */
void nds_set_user_settings(NdsMachine *m, const NdsUserSettings *settings);

/* What the real-time clock reads at reset (see NdsMachine.rtc_base). The
 * default is 2026-01-01 12:00:00, so that runs are repeatable; a frontend
 * passes the host's local time. Takes effect at the next nds_reset(). */
#define NDS_RTC_DEFAULT_BASE 1767268800
void nds_set_rtc_base(NdsMachine *m, int64_t seconds);

/* The card's save memory, for a frontend to keep in a file (when
 * NdsMachine.save_dirty says it changed). nds_save_memory gives the chip's
 * bytes: none while the kind of chip is still unknown, which it is until the
 * game first writes to a card the table does not list. nds_load_save
 * replaces the contents; it returns false if there is no card, the data is
 * larger than the chip, or the chip is unknown and no chip has that size. */
const uint8_t *nds_save_memory(const NdsMachine *m, uint32_t *size);
bool nds_load_save(NdsMachine *m, const uint8_t *data, size_t size);

/* The firmware flash, which games write (the owner's settings, Wi-Fi
 * set-up): NDS_FIRMWARE_SIZE bytes for a frontend to keep in a file when
 * NdsMachine.firmware_dirty says it changed. nds_load_firmware takes such a
 * file back (or a user's own dump) and takes the owner's settings from it;
 * false if it is not NDS_FIRMWARE_SIZE bytes. Takes effect at the next
 * nds_reset(). */
const uint8_t *nds_firmware_image(const NdsMachine *m);
bool nds_load_firmware(NdsMachine *m, const uint8_t *data, size_t size);

/* Raise interrupt flags on one CPU (IF |= bits). */
void nds_raise_irq(NdsMachine *m, int cpu, uint32_t bits);
/* Recompute a CPU's interrupt line after IME, IE or IF changed. */
void nds_update_irq(NdsMachine *m, int cpu);
/* End the running slice at the running CPU's current position, so the other
 * CPU catches up before it goes on. */
void nds_sync(NdsMachine *m);

/* Side-effect-free reads for debuggers, tracers and tests. */
uint32_t nds9_peek32(NdsMachine *m, uint32_t addr);
uint32_t nds7_peek32(NdsMachine *m, uint32_t addr);
uint8_t nds9_peek8(NdsMachine *m, uint32_t addr);
uint8_t nds7_peek8(NdsMachine *m, uint32_t addr);
/* What an opcode fetch of `size` bytes at `addr` would read, without making
 * one (the ARM9's DTCM never answers opcode fetches). */
uint32_t nds9_peek_code(NdsMachine *m, uint32_t addr, int size);
uint32_t nds7_peek_code(NdsMachine *m, uint32_t addr, int size);
/* Reload a CPU's pipeline from memory at its PC without charging cycles. */
void nds_prime_pipeline(NdsMachine *m, int cpu);

/* What the ARM9 pays for an opcode fetch at an (aligned) address. The bus
 * and recompiled code both use this. */
static inline uint32_t nds9_code_cycles(const NdsMachine *m, uint32_t addr)
{
    if (addr < m->itcm_end && !m->itcm_load) return NDS9_CYCLES_TCM;
    return (addr >> 24) == 0x02u ? NDS9_CYCLES_CODE : NDS9_CYCLES_BUS;
}

/* A write reached RAM at `offset` in the code space (NDS_RC_*): if compiled
 * code lies in that word, whatever was checked against that page must be
 * checked again. Called by the bus for every such write. */
static inline void nds_rc_note_write(NdsMachine *m, uint32_t offset)
{
    if (m->rc_code_words && ((m->rc_code_words[offset >> 5] >> ((offset >> 2) & 7u)) & 1u)) {
        m->rc_page_seq[offset >> 12] = ++m->rc_seq;
    }
}

/* ------------------------------------------------------------------ */
/* Internal entry points shared between runtime files                  */
/* ------------------------------------------------------------------ */

void nds_schedule(NdsMachine *m, int event, uint64_t at);
void nds_process_events(NdsMachine *m);

/* Recompute EXTKEYIN from the keys, the pen and the lid. */
void nds_update_extkeyin(NdsMachine *m);
/* The real-time clock's interrupt line changed level. */
void nds_rtc_line_changed(NdsMachine *m);
/* The clock's time now: rtc_base plus emulated seconds. */
int64_t nds_rtc_now(const NdsMachine *m);

/* Rebuild the derived memory maps after the registers they come from
 * changed (and after loading a state). */
void nds_vram_remap(NdsMachine *m);
void nds_wram_remap(NdsMachine *m);
void nds_tcm_remap(NdsMachine *m);

/* Memory as a device on the ARM7's side reads and writes it (the sound
 * unit's sample fetches and capture stores): RAM only, no time charged, and
 * recompiled code is told about writes. */
uint32_t nds7_device_read32(NdsMachine *m, uint32_t addr);
void nds7_device_write32(NdsMachine *m, uint32_t addr, uint32_t value);

/* Sound (nds_sound.c). */
void nds_sound_reset(NdsMachine *m);
void nds_sound_event(NdsMachine *m);
uint32_t nds_sound_read7(NdsMachine *m, uint32_t addr, int size);
void nds_sound_write7(NdsMachine *m, uint32_t addr, uint32_t value, int size);

/* I/O (nds_io.c). addr is the full address; size is 1, 2 or 4. */
uint32_t nds9_io_read(NdsMachine *m, uint32_t addr, int size);
void nds9_io_write(NdsMachine *m, uint32_t addr, uint32_t value, int size);
uint32_t nds7_io_read(NdsMachine *m, uint32_t addr, int size);
void nds7_io_write(NdsMachine *m, uint32_t addr, uint32_t value, int size);

/* Timers (nds_timer.c). */
void nds_timer_write_control(NdsMachine *m, int cpu, int index, uint16_t value);
void nds_timer_write_reload(NdsMachine *m, int cpu, int index, uint16_t value);
uint16_t nds_timer_read_counter(NdsMachine *m, int cpu, int index);
void nds_timer_overflow_event(NdsMachine *m, int cpu, int index);

/* DMA (nds_dma.c). */
enum {
    NDS_DMA_IMMEDIATE,
    NDS_DMA_VBLANK,
    NDS_DMA_HBLANK,
    NDS_DMA_DISPLAY_START,
    NDS_DMA_MAIN_MEMORY_DISPLAY,
    NDS_DMA_CARD,
    NDS_DMA_GBA_SLOT,
    NDS_DMA_GXFIFO,
    NDS_DMA_WIFI,
};
void nds_dma_write_control(NdsMachine *m, int cpu, int index, uint32_t value);
/* Run the channels waiting on `timing`. True if any of them moved data. */
bool nds_dma_trigger(NdsMachine *m, int cpu, int timing);

/* Video (nds_video.c). The read and write take any ARM9 I/O access and
 * return false if it is not to a video register. */
void nds_video_reset(NdsMachine *m);
bool nds_video_read9(NdsMachine *m, uint32_t addr, int size, uint32_t *value);
bool nds_video_write9(NdsMachine *m, uint32_t addr, uint32_t value, int size);
void nds_video_render_line(NdsMachine *m, int y); /* at the HBlank of visible line y */
void nds_video_line_start(NdsMachine *m, int line); /* VCOUNT has just become `line` */
void nds_video_gx_event(NdsMachine *m);
bool nds_video_gx_irq(const NdsMachine *m); /* the geometry FIFO's interrupt request: a level */
void nds_video_dma_enabled(NdsMachine *m);  /* a geometry-FIFO DMA was switched on */
void nds_video_power_changing(NdsMachine *m); /* before POWCNT1 is written */
void nds_video_power_changed(NdsMachine *m, uint16_t before); /* and after */

/* IPC (nds_ipc.c). */
uint32_t nds_ipc_read(NdsMachine *m, int cpu, uint32_t offset, int size); /* 0x180-0x18B */
void nds_ipc_write(NdsMachine *m, int cpu, uint32_t offset, uint32_t value, int size);
uint32_t nds_ipc_recv(NdsMachine *m, int cpu); /* 0x04100000 */

/* Maths unit (nds_math.c). */
uint32_t nds_math_read(NdsMachine *m, uint32_t offset, int size); /* 0x280-0x2BF */
void nds_math_write(NdsMachine *m, uint32_t offset, uint32_t value, int size);

/* Card (nds_card.c). */
uint32_t nds_card_read(NdsMachine *m, int cpu, uint32_t offset, int size); /* 0x1A0-0x1BB */
void nds_card_write(NdsMachine *m, int cpu, uint32_t offset, uint32_t value, int size);
uint32_t nds_card_data(NdsMachine *m, int cpu); /* 0x04100010 */
void nds_card_event(NdsMachine *m);

/* Boot (nds_boot.c). */
void nds_boot_direct(NdsMachine *m);

#ifdef __cplusplus
}
#endif

#endif /* NDS_H */
