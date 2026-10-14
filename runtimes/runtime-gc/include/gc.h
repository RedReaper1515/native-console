/*
 * The GameCube machine: memory, address translation, the I/O register file,
 * interrupts and the run loop around the Gekko core.
 *
 * The CPU is the first member, so a GcCpu pointer is a GcMachine pointer and
 * the bus callbacks recover the machine at no cost - the same arrangement
 * the GBA and SNES runtimes use.
 *
 * Physical memory is what the hardware sees; the addresses a game uses are
 * effective addresses that the BATs translate. Nothing here assumes the
 * usual 0x80000000 mapping: the guest sets its own BATs, and a game that
 * maps memory somewhere unusual works because translation is modelled
 * rather than hardcoded.
 */
#ifndef GC_H
#define GC_H

#include <stdio.h>

#include "gc_cpu.h"
#include "gc_pad.h"
#include "gc_efb.h"
#include "gc_gx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_MEM1_SIZE 0x01800000u /* 24 MB of main memory */
/* A Wii has a second, larger block of main memory at its own physical
 * address (wii.h). The machine holds both in one host block, MEM2 straight
 * after MEM1, so that everything that reads guest memory by offset - the
 * graphics stream, textures, the sound processor's DMA - works on either
 * console with one bound: gc_ram_size. */
#define GC_MEM2_SIZE 0x04000000u /* 64 MB, Wii only */
#define GC_PHYS_MEM2 0x10000000u
#define GC_ARAM_SIZE 0x01000000u /* 16 MB of audio memory, reachable only by DMA */
#define GC_EFB_SIZE 0x00200000u  /* 2 MB embedded framebuffer */
#define GC_LOCKED_CACHE_SIZE 0x4000u

/* Physical addresses. The effective addresses a game uses (0x80000000 for
 * cached memory, 0xc0000000 for the uncached alias) are BAT translations of
 * these. */
#define GC_PHYS_MEM1 0x00000000u
#define GC_PHYS_EFB 0x08000000u
#define GC_PHYS_IO 0x0c000000u
#define GC_IO_SIZE 0x00009000u

/* The locked cache is not physical memory: it answers at one effective
 * address range when HID2[LCE] is set. */
#define GC_LOCKED_CACHE_EA 0xe0000000u

/* Processor interface interrupt causes. */
enum {
  GC_INT_ERROR = 0,
  GC_INT_RESET_SWITCH = 1,
  GC_INT_DVD = 2,
  GC_INT_SERIAL = 3,
  GC_INT_EXI = 4,
  GC_INT_AUDIO = 5,
  GC_INT_DSP = 6,
  GC_INT_MEMORY = 7,
  GC_INT_VIDEO = 8,
  GC_INT_PE_TOKEN = 9,
  GC_INT_PE_FINISH = 10,
  GC_INT_COMMAND = 11,
  GC_INT_DEBUG = 12,
  GC_INT_HIGH_SPEED = 13
};

/* Timing, derived rather than rounded. The CPU runs at 486 MHz and the
 * video interface at 27 MHz, exactly eighteen core cycles apart. NTSC is
 * 858 pixels a line at 13.5 MHz, so 1716 VI clocks, and 262.5 lines a
 * field: 450,450 VI clocks, which is 59.94 fields a second and not 60.
 *
 * The difference is one part in a thousand, which sounds like nothing and
 * is not: a game that paces its audio off the retrace drifts a second out
 * of sync every quarter of an hour. tests/gc/programs/gc_timetest.c holds
 * the field rate to 500 parts per million for that reason.
 *
 * A "frame" here is one field, which is what a headless run counts. Only
 * NTSC is modelled; PAL's 50 Hz and the progressive modes are P3's. */
/* A Wii's processor runs half as fast again, 729 MHz, against the same 27 MHz
 * video clock: twenty-seven core cycles to a video clock. Which console this
 * process is running is fixed when the first machine is made (wii.h), and
 * these three say so. They are the process's and not the machine's for the
 * same reason gc_vi_double is: GcMachine's layout is what a save state
 * checks. */
extern uint32_t gc_cpu_hz;               /* 486,000,000 or 729,000,000 */
extern uint32_t gc_cycles_per_vi_clock;  /* 18 or 27 */
extern uint32_t gc_ram_size;             /* GC_MEM1_SIZE, plus GC_MEM2_SIZE on a Wii */
#define GC_CPU_HZ gc_cpu_hz
#define GC_CYCLES_PER_VI_CLOCK gc_cycles_per_vi_clock
#define GC_VI_CLOCKS_PER_LINE 1716u
/* The beam position register counts *pixels*, and a pixel is two VI clocks
 * in an interlaced mode: 858 of them to a line, 720 of which are the
 * visible window. */
#define GC_VI_PIXELS_PER_LINE (GC_VI_CLOCKS_PER_LINE / 2u)
/* A frame is two fields, and the position register spans the whole frame -
 * 525 lines for NTSC, which is where the 262.5 lines a field comes from. */
#define GC_VI_LINES_PER_FRAME 525u
#define GC_VI_PIXELS_PER_FRAME (GC_VI_LINES_PER_FRAME * GC_VI_PIXELS_PER_LINE)
#define GC_VI_CLOCKS_PER_FIELD 450450u /* 262.5 lines */
#define GC_CYCLES_PER_FIELD ((uint64_t)GC_VI_CLOCKS_PER_FIELD * GC_CYCLES_PER_VI_CLOCK)

/* Sweep a frame in one field's worth of cycles instead of two, so the
 * display interrupts a game uses as its retrace arrive twice as often.
 *
 * A game that draws on every second retrace then draws every field - sixty
 * pictures a second instead of thirty. Whether that is sixty *correct*
 * pictures a second depends on where the game gets the time it advances
 * by: one that reads the timebase advances by what really elapsed and
 * plays at the right speed, and one that counts a frame an iteration plays
 * at double speed. Nothing here can tell which, so this is a switch a
 * player throws and looks at, not something to turn on by default.
 *
 * It is a host setting and not the machine's: putting it in GcMachine
 * would change that structure's size, and a save state refuses to load
 * into a machine whose layout differs (gc_state.c). States keep working
 * across this.
 *
 * The arithmetic stays exact either way. A frame is 450,450 pixels; two
 * fields are 16,216,200 cycles, which is 36 a pixel, and one field is
 * 8,108,100, which is 18. */
extern int gc_vi_double;
#define GC_VI_FRAME_CYCLES ((uint64_t)GC_CYCLES_PER_FIELD * (gc_vi_double ? 1u : 2u))
#define GC_VI_CYCLES_PER_PIXEL ((uint64_t)GC_CYCLES_PER_VI_CLOCK * (gc_vi_double ? 1u : 2u))

typedef struct GcMachine GcMachine;
struct GcDisc;

/* One I/O register, described rather than implemented. The blocks that need
 * more than storage supply a hook; everything else is a masked read and a
 * masked write, so a register the guest touches cannot silently do nothing
 * without saying so. */
typedef struct GcIoReg {
  uint16_t offset;
  uint8_t width; /* 1, 2 or 4 bytes */
  const char *name;
  uint32_t read_mask;
  uint32_t write_mask;
  uint32_t (*on_read)(GcMachine *m, uint32_t stored);
  void (*on_write)(GcMachine *m, uint32_t value);
} GcIoReg;

typedef struct GcIoBlock {
  const char *name;
  uint32_t base; /* offset from GC_PHYS_IO */
  uint32_t size;
  const GcIoReg *regs;
  unsigned count;
  unsigned storage_base; /* index into GcMachine::io, filled in at reset */
} GcIoBlock;

#define GC_IO_STORAGE 256

/* A save state writes this structure whole (gc_state.c), so a field added
 * here is saved and loaded without further work - unless it is a pointer or
 * belongs to the host rather than the machine (a cache, a setting, a
 * diagnostic), in which case scrub() and keep_host() in gc_state.c must name
 * it, or a loaded state carries another process's value. Add fields; do not
 * reorder them. */
struct GcMachine {
  GcCpu cpu; /* first member: a GcCpu * is a GcMachine * */

  uint8_t *mem1;
  uint8_t *aram;
  /* The embedded framebuffer is not memory: it is two planes of pixels
   * behind an address window, and the pixel format says how many bits of
   * each one survive. */
  GcEfb efb;
  /* The disc in the drive, if there is one. It belongs to the caller and
   * outlives a reset, which is why it sits with the memory rather than in
   * the part of this structure a reset clears. */
  struct GcDisc *disc;
  uint8_t locked_cache[GC_LOCKED_CACHE_SIZE];

  /* One slot per described register, in table order. */
  uint32_t io[GC_IO_STORAGE];

  /* Processor interface. */
  uint32_t pi_cause;
  uint32_t pi_mask;

  /* The OpenGL path's state, or null when triangles go to the reference
   * rasteriser. Opaque here so that gc.h does not drag SDL in. */
  void *gl;

  /* What is plugged into the four controller ports. */
  struct GcPad *pads;

  /* The memory card in the first slot, or null for an empty slot. An
   * empty slot is a correct answer: a game asked to save on one says
   * there is no memory card and carries on. Opaque here; see
   * gc_card.h. */
  void *card;

  /* The expansion bus's three interrupt bits, per channel, kept apart
   * from the guest's own copy of the register because they are the
   * machine's to set and the guest's only to acknowledge - and it
   * acknowledges by writing them back *set*, so an ordinary store must
   * not be able to raise one. */
  uint32_t exi_status[3];
  /* What the last immediate write asked each channel's device for: the
   * protocol is a command word written by itself and then a read, so the
   * command has to be remembered between the two. And which device the
   * last chip-select picked (0 for none). Nothing else frames a command -
   * a card's read is an opcode, an address, four bytes of nothing and a
   * transfer, and only the select going away says where it ends. */
  uint32_t exi_command[3];
  uint32_t exi_selected[3];

  /* The audio DMA: where it is reading, how much of the buffer is
   * left, and when the next thirty-two bytes come due. What the guest
   * programmed is in the registers; these are what is being played,
   * which is deliberately not the same thing - a game writes the next
   * buffer's address while the current one is still going. */
  uint32_t aid_address;
  uint32_t aid_blocks_left;
  uint64_t aid_next_block_cycles;
  int aid_running;

  /* And what has been played, waiting for a host that may not be
   * taking it. Allocated the first time a game makes a sound. */
  int16_t *audio_ring;
  unsigned audio_read;
  unsigned audio_write;
  uint64_t audio_overruns;

  /* The audio interface's sample counter: the cycle it was last started
   * or reset at, and what it had counted to by then. A game waits on
   * this to pace itself, so it has to move whether or not any sound is
   * being produced. */
  uint64_t ai_started_cycles;
  uint32_t ai_counted_before;

  /* Video interface: the field counter the run loop advances, and where
   * the beam is inside the current field. */
  uint64_t field;
  uint64_t next_field_cycles;
  /* The instruction count the beam and the clocks were last brought up to.
   * Doing that twice at one count is not harmless: at the end of a frame
   * the beam is held on the last pixel for one instruction, and a second
   * look at the same count would wrap it one instruction early. */
  uint64_t advanced_cycles;
  uint32_t vi_line;   /* 0 .. 524, the line within the *frame* - see advance_video */
  uint32_t vi_column; /* 0 .. 857, the pixel within the line */
  uint32_t vi_pixel;  /* the two together, as one count into the frame */
  /* Where the four display interrupt registers live in the I/O storage,
   * resolved once: finding them by name on every step would cost more than
   * the rest of the video interface put together. */
  uint32_t vi_interrupt_slot[4];
  int vi_interrupt_slots_known;

  /* The other registers the machine touches between every pair of
   * instructions, resolved the same way and for the same reason: the
   * audio interface's control and timing words, and the sound
   * processor's control word. Looking them up by name each time costs
   * more than everything else the machine does between instructions
   * put together, which is not a guess - it is what the profile of a
   * program doing nothing at all says. Indices rather than pointers,
   * so a reset cannot leave a stale one behind. */
  uint32_t ai_control_slot;
  uint32_t ai_timing_slot;
  uint32_t dsp_control_slot;
  int hot_slots_known;

  /* The DSP: the mailbox handshake its ROM performs, the ARAM DMA the SDK
   * uploads through, and the task protocol a game's own microcode speaks
   * once it is running. What the command lists actually *say* - the voices
   * and the mixing - is still to come. */
  uint32_t dsp_mail_to_cpu;
  uint32_t dsp_mail_to_dsp;
  int dsp_mail_waiting;
  uint32_t dsp_boot_parameter; /* the ROM's upload protocol */
  uint32_t dsp_ucode_address;
  uint32_t dsp_ucode_length;
  /* Mail the DSP owes the CPU, and when it may send it. A microcode
   * that answered instantly would be wrong in a way that hangs games;
   * see gc_io.c. Zero means nothing is owed. */
  uint32_t dsp_mail_queued;
  uint64_t dsp_mail_due_cycles;
  /* The command list the CPU last handed over: its length in
   * halfwords, then its address. */
  uint32_t dsp_list_length;
  /* The sound microcode's own state - the nine mixing buffers and the
   * voice list - allocated the first time a game hands over a command
   * list. Opaque here; see gc_ax.h. */
  void *ax;
  uint32_t dsp_control_previous;
  uint64_t dsp_init_code_clears;

  /* The disc drive: the two registers whose interrupt bits are written
   * back to clear, kept alongside so a hook can tell what changed, and the
   * error the drive is holding for the next 0xe0 command. */
  uint32_t di_status;
  uint32_t di_cover;
  uint32_t disc_error;
  unsigned unknown_disc_commands;

  /* The write-gather pipe: 32 bytes gathered at a time and handed to the
   * command processor. */
  uint8_t gather[32];
  unsigned gather_used;
  unsigned gather_bursts;

  /* Flipper's front end: the command FIFO and the register files behind
   * it. The bytes the pipe gathers end up here. */
  GcGx gx;
  uint32_t pe_control;

  /* High-level hooks installed from the program's symbols. */
  uint32_t osreport_addr;
  uint32_t osreport_size;

  /* Diagnostics. Unknown I/O is counted and reported rather than ignored:
   * a register nobody implemented is the most likely reason a program
   * wanders off. */
  unsigned unknown_io_reads;
  unsigned unknown_io_writes;
  unsigned bad_access;
  int trace_io;
  /* Stop at the first access to an address that is not memory, so that
   * whoever made it can still be asked about it. */
  int stop_on_bad_access;
  /* Where the program counter spends its time, sampled. A game that is
   * stuck is stuck *somewhere*, and without symbols that address is the
   * only thing there is to go on. */
  uint32_t *profile;      /* one counter per 4 bytes of main memory */
  uint32_t profile_every; /* sample once every this many instructions */

  /* Every interpreted instruction, counted by address, or null. Opaque
   * here; see gc_coverage.h. */
  void *coverage;

  /* Stop once this many instructions have run, so that whatever the
   * machine is doing at the time can be asked about. Zero to run on. */
  uint64_t stop_after;

  /* Stop when the program counter reaches this, so that whoever got it
   * there can be asked about it. Zero to run on. */
  uint32_t stop_at;

  /* The last branches taken, oldest first once it has wrapped. A
   * processor keeps a record like this for the same reason: when
   * control ends up somewhere impossible, the only useful question is
   * how it got there, and a stack frame cannot answer it after a jump
   * through a pointer that was never set. */
#define GC_BRANCH_HISTORY 64
  uint32_t branch_from[GC_BRANCH_HISTORY];
  uint32_t branch_to[GC_BRANCH_HISTORY];
  uint32_t branch_repeat[GC_BRANCH_HISTORY];
  unsigned branch_next;
  int keep_branch_history;

  /* An address to report every write to, or zero. */
  uint32_t watch_address;
  FILE *log;

  /* Set when the program asks to stop (SYS_ResetSystem, or a test hook). */
  int finished;
  int exit_code;
};

GcMachine *gc_machine_create(void);
void gc_machine_destroy(GcMachine *m);
void gc_machine_reset(GcMachine *m);

/* Physical memory the machine owns; NULL when the address is not memory. */
uint8_t *gc_machine_ram(GcMachine *m, uint32_t physical, uint32_t size);

/* Where a physical address sits in the machine's block of main memory
 * (GcMachine::mem1), or GC_RAM_NONE when it is not main memory. MEM1 is at
 * its own address; a Wii's MEM2 follows it. */
#define GC_RAM_NONE 0xffffffffu
static inline uint32_t gc_ram_offset(uint32_t physical) {
  if (physical < GC_MEM1_SIZE) return physical;
  if (gc_ram_size > GC_MEM1_SIZE && physical - GC_PHYS_MEM2 < GC_MEM2_SIZE) {
    return physical - GC_PHYS_MEM2 + GC_MEM1_SIZE;
  }
  return GC_RAM_NONE;
}

/* The same for an address a hardware register holds - a texture's, a vertex
 * array's, a DMA's. Software writes these with or without the bits that
 * choose a view (0x80000000, 0xc0000000) and the hardware ignores them, so
 * they are dropped here. On a GameCube that leaves 26 bits, as it always
 * did, and what lies past MEM1 is not memory (the result is at or beyond
 * gc_ram_size, which every user checks against). On a Wii bit 28 is the
 * difference between MEM1 and MEM2 and has to survive. */
#define GC_RAM_INVALID 0x0c000000u /* past either console's memory, and safe to add to */
static inline uint32_t gc_ram_from_register(uint32_t value) {
  if (gc_ram_size > GC_MEM1_SIZE) {
    const uint32_t offset = gc_ram_offset(value & 0x1fffffffu);
    return offset == GC_RAM_NONE ? GC_RAM_INVALID : offset;
  }
  return value & 0x03ffffffu;
}

/* And back, for a register that reports a position in memory the machine
 * keeps as an offset (the graphics FIFO's pointers). */
static inline uint32_t gc_ram_to_register(uint32_t offset) {
  if (offset >= GC_MEM1_SIZE && offset < gc_ram_size) return offset - GC_MEM1_SIZE + GC_PHYS_MEM2;
  return offset;
}

/* Main memory by offset, and by an address a register holds; NULL when the
 * bytes asked for are not all memory. */
static inline uint8_t *gc_ram_ptr(GcMachine *m, uint32_t offset, uint32_t size) {
  return (offset < gc_ram_size && size <= gc_ram_size - offset) ? m->mem1 + offset : NULL;
}
static inline uint8_t *gc_ram_at_register(GcMachine *m, uint32_t value, uint32_t size) {
  return gc_ram_ptr(m, gc_ram_from_register(value), size);
}

/* An effective address in one of the fixed views (0x80000000 and 0xc0000000
 * for MEM1, 0x90000000 and 0xd0000000 for a Wii's MEM2) as a physical one:
 * for the loaders, which place things before any game code has run. */
#define GC_VIEW_TO_PHYSICAL(ea) ((ea) & 0x3fffffffu)

/* Raise or clear a processor-interface interrupt. */
void gc_set_interrupt(GcMachine *m, unsigned cause, int raised);

/* The pixel engine's two interrupts, raised from the command stream rather
 * than by the CPU: a game writes a token or a "done" into the stream and
 * waits for the interrupt that arrives when the graphics processor gets
 * there. */
void gc_pe_token(GcMachine *m, uint16_t token, int interrupt);
void gc_pe_finish(GcMachine *m);

/* One instruction's worth of everything that is not the instruction: the
 * hooks and interrupts before it, and its retirement and the video
 * interface after it. gc_machine_step is these two with an interpreted
 * instruction between them; recompiled code puts a call to a handler
 * there instead, and so cannot disagree about the rest. */
int gc_machine_before(GcMachine *m);
void gc_machine_after(GcMachine *m);

/* The same work, done only when it is due - which is how recompiled code
 * calls it. Code that has retired instructions without calling
 * gc_machine_after calls gc_machine_due once cpu.cycles reaches
 * cpu.next_event, in place of gc_machine_before; it catches the machine up,
 * does what `before` does, and sets the next moment to look. See gc.c for
 * why that is the same as calling both every time. gc_machine_sync is the
 * catching-up alone, for code that needs the beam and the clocks current
 * without being about to run an instruction. */
int gc_machine_due(GcMachine *m);
void gc_machine_sync(GcMachine *m);

/* Instructions counted by gc_machine_idle_until and not run, since the
 * program started: what a game had left of its fields, for the field log
 * (gc_perf.h). Not part of the machine, and in no state. */
extern uint64_t gc_idle_cycles;

/* A wait loop of `count` instructions has just run once from its first
 * instruction back to it, with nothing due (every instruction's count was
 * below `event`, which is still cpu.next_event) and with no effect a second
 * run would not repeat exactly. Every further iteration that finishes below
 * `event` would find nothing due and change nothing but the count, so they
 * are counted instead of run, and the instructions that reach `event` run
 * as they would have. gc.c has the whole argument. */
static inline void gc_machine_idle_until(GcCpu *c, uint64_t event, uint32_t count) {
  if (event > c->cycles) {
    const uint64_t counted = ((event - c->cycles) / count) * count;
    c->cycles += counted;
    gc_idle_cycles += counted;
  }
}

/* The addresses the runtime answers calls to itself (OSReport and the
 * like), so that recompiled code can leave the blocks that contain them to
 * the interpreter, which checks for them before every instruction. Returns
 * how many were written; the generation changes whenever the set does. */
unsigned gc_os_hook_addresses(uint32_t *out, unsigned max);
unsigned gc_os_hook_generation(void);

/* Run until the field counter reaches `until_field` or the program stops. */
void gc_machine_run(GcMachine *m, uint64_t until_field);

/* One step of the machine: the CPU, then anything due. */
void gc_machine_step(GcMachine *m);

/* The I/O register file. */
uint32_t gc_io_read(GcMachine *m, uint32_t offset, unsigned size);
void gc_io_write(GcMachine *m, uint32_t offset, unsigned size, uint32_t value);
void gc_io_reset(GcMachine *m);

/* The frame the video interface is scanning out, decoded from its own
 * registers: each field's buffer, the bytes from one line of a field to the
 * next, and the frame's size in pixels. Returns 0, or -1 while the
 * registers do not describe a frame. */
typedef struct GcViDisplay {
  uint32_t top;
  uint32_t bottom;
  uint32_t field_stride;
  uint32_t width;
  uint32_t height;
} GcViDisplay;
int gc_vi_display(GcMachine *m, GcViDisplay *out);

/* Load an executable into guest memory. Returns 0 on success. Both fill in
 * the entry point and, where the file has them, the symbols the runner
 * hooks. */
int gc_load_elf(GcMachine *m, const void *data, size_t size, uint32_t *entry);
int gc_load_dol(GcMachine *m, const void *data, size_t size, uint32_t *entry);
/* An ELF's symbols without its contents: the hooks for a program that was
 * loaded some other way (off a disc, by its apploader). Returns 0. */
int gc_elf_symbols(GcMachine *m, const void *data, size_t size);

/* The low-memory globals the apploader leaves behind, and the BAT and MSR
 * state a program expects at its entry point. */
void gc_install_boot_state(GcMachine *m);

/* Let the disc load its own game, the way the console does: its apploader
 * is a small PowerPC program that reads the executable through the drive
 * and sets up the memory the game expects to find. Returns 0 and the entry
 * point, or -1 with a reason on the log. */
int gc_run_apploader(GcMachine *m, struct GcDisc *disc, uint32_t *entry);

/* Undo the wreckage of a boot attempt that failed, so the next one
 * starts from a machine that works. */
void gc_machine_reset_after_failed_boot(GcMachine *m);

/* The bottom of the arena: where the loaded program ends and the memory a
 * game may take for itself begins. The loaders call this. */
void gc_set_arena_low(GcMachine *m, uint32_t end);

/* Puts a disc in the drive and does what the apploader does with it: the
 * filesystem into memory, and the low-memory globals that say where it is.
 * The disc belongs to the caller. Returns 0, or -1 if the filesystem will
 * not fit where the globals say it goes. */
int gc_attach_disc(GcMachine *m, struct GcDisc *disc);

/* OSReport, emulated at the call. Returns non-zero when the address was the
 * hooked one and the call has been handled. */
int gc_os_hook(GcMachine *m);

#ifdef __cplusplus
}
#endif

#endif /* GC_H */
