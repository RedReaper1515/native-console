/*
 * The apploader: letting the disc load its own game.
 *
 * Every GameCube disc carries, at offset 0x2440, a small PowerPC program
 * whose job is to load the game. The console's boot ROM copies it into
 * memory and calls it; it reads the DOL and anything else it wants
 * through the drive, puts it where it belongs, and hands back an entry
 * point.
 *
 * Loading the DOL ourselves instead is quicker to write and *almost*
 * works. What it misses is everything else the apploader does, and the
 * one that matters is where memory free for the game begins. The low
 * memory word at 0x80000030 says so, and the SDK's start-up copies it and
 * then clears everything between there and the top of the arena. A game's
 * stack sits immediately above its own program image - Shrek 2's at
 * 0x803A1100, sixty-four kilobytes past the end of its last section - so
 * a value derived from the section table alone puts the stack *inside*
 * the arena, and the game's own start-up code zeroes its own return
 * addresses. It then returns to address zero, which looks nothing like
 * the cause.
 *
 * There is no reading that off the DOL: the stack is a linker's decision
 * and the header does not mention it. The apploader knows because it is
 * the game's own code. So this runs it.
 *
 * The protocol, which is the same on every disc:
 *   - a header at 0x2440: a version string, then the entry point, the
 *     size of the code, and the size of a trailer;
 *   - the code goes to 0x81200000 and is called with pointers to three
 *     function pointers, which it fills in: init, main, close;
 *   - init takes a function to print progress with;
 *   - main is called until it returns zero, each time asking for a number
 *     of bytes from a disc offset to be read to an address;
 *   - close returns the game's entry point.
 */
#include <string.h>

#include "gc.h"
#include "gc_disc.h"
#include "wii.h"

/* Where the boot ROM puts the apploader, and the scratch this needs below
 * it: three function pointers, three arguments for `main`, and one
 * instruction for the apploader to call when it wants to report progress. */
#define GC_APPLOADER_ADDRESS 0x81200000u
#define GC_APPLOADER_SCRATCH 0x811fff00u
#define GC_APPLOADER_REPORT (GC_APPLOADER_SCRATCH + 0x40u)
/* An address no code lives at. A guest call returns to it and the run
 * loop stops, which is how a host asks a guest function a question. */
#define GC_APPLOADER_RETURN 0x81100000u
#define GC_APPLOADER_STACK 0x816fff00u

static uint32_t read_word(GcMachine *m, uint32_t address) {
  const uint8_t *p = gc_machine_ram(m, GC_VIEW_TO_PHYSICAL(address), 4);
  if (!p) return 0;
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void write_word(GcMachine *m, uint32_t address, uint32_t value) {
  uint8_t *p = gc_machine_ram(m, GC_VIEW_TO_PHYSICAL(address), 4);
  if (!p) return;
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

/* Call a function in the guest and wait for it to come back.
 *
 * The return address is an address nothing occupies, so "it returned" is
 * simply "the program counter reached it". The instruction budget is there
 * because an apploader that goes wrong must not take the run loop with
 * it - a load that never finishes is a failure to report, not a hang. */
static int call_guest(GcMachine *m, uint32_t function, const uint32_t *arguments,
                      unsigned count, uint32_t *result) {
  const uint64_t budget = 200000000ull;
  const uint64_t started = m->cpu.cycles;
  unsigned i;

  for (i = 0; i < count && i < 8u; i++) m->cpu.gpr[3 + i] = arguments[i];
  m->cpu.pc = function;
  m->cpu.npc = function + 4u;
  m->cpu.lr = GC_APPLOADER_RETURN;
  m->cpu.stopped = GC_STOP_NONE;

  while (m->cpu.pc != GC_APPLOADER_RETURN) {
    if (m->cpu.stopped) {
      if (m->log) {
        fprintf(m->log, "gc: the apploader stopped (%d) at pc %08x on %08x\n",
                (int)m->cpu.stopped, m->cpu.pc, m->cpu.stop_insn);
      }
      return -1;
    }
    if (m->cpu.cycles - started > budget) {
      if (m->log) fprintf(m->log, "gc: the apploader ran on and on, at pc %08x\n", m->cpu.pc);
      return -1;
    }
    gc_machine_step(m);
  }
  if (result) *result = m->cpu.gpr[3];
  return 0;
}

int gc_run_apploader(GcMachine *m, struct GcDisc *disc, uint32_t *entry) {
  uint8_t header[0x20];
  uint32_t code_size, trailer_size, start;
  uint32_t init_function = 0, main_function = 0, close_function = 0;
  uint32_t arguments[3];
  uint8_t *destination;

  if (!disc) return -1;
  if (gc_disc_read(disc, 0x2440u, header, sizeof(header)) != 0) return -1;
  start = ((uint32_t)header[0x10] << 24) | ((uint32_t)header[0x11] << 16) |
          ((uint32_t)header[0x12] << 8) | header[0x13];
  code_size = ((uint32_t)header[0x14] << 24) | ((uint32_t)header[0x15] << 16) |
              ((uint32_t)header[0x16] << 8) | header[0x17];
  trailer_size = ((uint32_t)header[0x18] << 24) | ((uint32_t)header[0x19] << 16) |
                 ((uint32_t)header[0x1a] << 8) | header[0x1b];
  if (!start || !code_size || code_size > 0x00100000u) {
    if (m->log) fprintf(m->log, "gc: this disc's apploader header makes no sense\n");
    return -1;
  }

  destination = gc_machine_ram(m, GC_VIEW_TO_PHYSICAL(GC_APPLOADER_ADDRESS), code_size + trailer_size);
  if (!destination) return -1;
  if (gc_disc_read(disc, 0x2460u, destination, code_size + trailer_size) != 0) return -1;
  if (m->log) {
    fprintf(m->log, "gc: apploader \"%.16s\", %u bytes, entry %08x\n", (const char *)header,
            code_size, start);
  }

  /* One `blr`, for the apploader to call when it wants to say how far it
   * has got. A null pointer here would be called all the same. */
  write_word(m, GC_APPLOADER_REPORT, 0x4e800020u);

  /* A stack. The apploader is ordinary compiled code and its very first
   * instruction pushes a frame; with no stack pointer that is a write to
   * minus eight. High enough to be clear of the game it is about to load,
   * which goes where its own sections say. */
  m->cpu.gpr[1] = GC_APPLOADER_STACK;

  arguments[0] = GC_APPLOADER_SCRATCH;
  arguments[1] = GC_APPLOADER_SCRATCH + 4u;
  arguments[2] = GC_APPLOADER_SCRATCH + 8u;
  if (call_guest(m, start, arguments, 3, NULL) != 0) {
    if (m->log) fprintf(m->log, "gc: the apploader's entry point did not return\n");
    return -1;
  }
  init_function = read_word(m, GC_APPLOADER_SCRATCH);
  main_function = read_word(m, GC_APPLOADER_SCRATCH + 4u);
  close_function = read_word(m, GC_APPLOADER_SCRATCH + 8u);
  if (!init_function || !main_function || !close_function) {
    if (m->log) fprintf(m->log, "gc: the apploader did not hand back its three functions\n");
    return -1;
  }

  arguments[0] = GC_APPLOADER_REPORT;
  if (call_guest(m, init_function, arguments, 1, NULL) != 0) {
    if (m->log) fprintf(m->log, "gc: the apploader would not start\n");
    return -1;
  }

  for (;;) {
    uint32_t more = 0;
    uint32_t address, length, offset;
    arguments[0] = GC_APPLOADER_SCRATCH + 0x10u;
    arguments[1] = GC_APPLOADER_SCRATCH + 0x14u;
    arguments[2] = GC_APPLOADER_SCRATCH + 0x18u;
    if (call_guest(m, main_function, arguments, 3, &more) != 0) {
      if (m->log) fprintf(m->log, "gc: the apploader stopped part way through loading\n");
      return -1;
    }
    if (!more) break;

    address = read_word(m, GC_APPLOADER_SCRATCH + 0x10u);
    length = read_word(m, GC_APPLOADER_SCRATCH + 0x14u);
    offset = read_word(m, GC_APPLOADER_SCRATCH + 0x18u);
    if (!length) continue;
    destination = gc_machine_ram(m, GC_VIEW_TO_PHYSICAL(address), length);
    if (!destination) {
      if (m->log) {
        fprintf(m->log, "gc: the apploader asked for %u bytes at %08x, which is not memory\n",
                length, address);
      }
      return -1;
    }
    /* A Wii's apploader counts the disc in words, as its filesystem does. */
    if (gc_disc_read(disc, (uint64_t)offset << disc->offset_shift, destination, length) != 0) {
      if (m->log) fprintf(m->log, "gc: a read the apploader asked for ran off the disc\n");
      return -1;
    }
  }

  if (call_guest(m, close_function, NULL, 0, entry) != 0) {
    if (m->log) fprintf(m->log, "gc: the apploader would not finish\n");
    return -1;
  }
  if (m->cpu.machine) wii_after_apploader(m);
  if (m->log) {
    fprintf(m->log, "gc: the apploader loaded the game, entry %08x, arena %08x to %08x\n",
            *entry, read_word(m, 0x80000030u), read_word(m, 0x80000034u));
  }
  return 0;
}
