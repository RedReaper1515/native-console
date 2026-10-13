/*
 * The state the apploader leaves behind, and the handful of functions the
 * runner answers on the guest's behalf.
 *
 * A GameCube game starts with the machine already configured: BATs mapping
 * main memory, the OS globals in low memory holding the clock speeds and
 * the memory size, and the console's own boot magic in place. There is no
 * IPL here and never will be (docs/GC-PLAN.md, "No copyrighted firmware"),
 * so this writes that state from the documentation instead.
 *
 * The hooks are a different thing and exist only for programs built for
 * the guest by us or by dolphin-emu/hwtests. They are how a test program
 * reports a verdict and how it asks to stop. A real game is never hooked:
 * it has no symbols, and everything it does goes through the hardware.
 */

#include <stdarg.h>
#include <string.h>

#include "gc.h"
#include "gc_bus.h"
#include "gc_disc.h"
#include "wii.h"

/* ------------------------------------------------------------------ */
/* Boot state                                                          */

/* A BAT pair covering `size` bytes of physical memory at an effective
 * address, valid in both supervisor and problem state. */
static void set_bat(uint32_t *pair, uint32_t ea, uint32_t physical, uint32_t size,
                    uint32_t wimg) {
  const uint32_t blocks = size / 0x20000u; /* 128 KB units */
  pair[0] = (ea & 0xfffe0000u) | (((blocks - 1u) & 0x7ffu) << 2) | 2u | 1u;
  pair[1] = (physical & 0xfffe0000u) | (wimg << 3) | 2u; /* PP = read/write */
}

static void put32(GcMachine *m, uint32_t physical, uint32_t value) {
  uint8_t *p = gc_machine_ram(m, physical, 4);
  if (!p) return;
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

void gc_install_boot_state(GcMachine *m) {
  GcCpu *c = &m->cpu;

  /* A Wii starts from a state of its own (wii.c). */
  if (m->cpu.machine) {
    wii_install_boot_state(m);
    return;
  }

  /* Cached main memory at 0x80000000, the uncached alias at 0xc0000000, and
   * the register block at 0xcc000000. A game may replace these; they are
   * what it finds. */
  set_bat(&c->ibat[0], 0x80000000u, GC_PHYS_MEM1, 0x10000000u, 0u);
  set_bat(&c->dbat[0], 0x80000000u, GC_PHYS_MEM1, 0x10000000u, 0u);
  set_bat(&c->dbat[2], 0xc0000000u, GC_PHYS_MEM1, 0x10000000u, 5u); /* inhibited, guarded */
  set_bat(&c->ibat[2], 0xc0000000u, GC_PHYS_MEM1, 0x10000000u, 5u);
  gc_cpu_forget_fast(c);

  c->msr = GC_MSR_IR | GC_MSR_DR | GC_MSR_FP | GC_MSR_ME;
  /* Paired singles, the quantised load/store forms and the write-gather
   * pipe are all on before a game's first instruction. */
  c->hid2 = GC_HID2_PSE | GC_HID2_LSQE | GC_HID2_WPE;
  c->hid0 = 0x0011c464u; /* caches enabled, as the SDK leaves them */
  c->l2cr = 0xe0000000u;

  /* The OS globals. A program reads the clock speeds out of these to
   * calibrate every timer it has, so getting them wrong makes everything
   * else look subtly broken. */
  put32(m, 0x0000001cu, 0xc2339f3du); /* disc boot magic */
  put32(m, 0x00000020u, 0x0d15ea5eu); /* "boot from a disc" */
  put32(m, 0x00000024u, 0x00000001u); /* version */
  put32(m, 0x00000028u, GC_MEM1_SIZE);
  put32(m, 0x0000002cu, 0x00000003u); /* retail console */
  put32(m, 0x00000030u, 0x00000000u); /* arena low */
  put32(m, 0x00000034u, 0x81700000u); /* arena high */
  put32(m, 0x00000038u, 0x00000000u); /* filesystem table */
  put32(m, 0x0000003cu, 0x00000000u);
  put32(m, 0x000000ccu, 0x00000000u); /* NTSC */
  put32(m, 0x000000f0u, GC_MEM1_SIZE); /* simulated memory size */
  put32(m, 0x000000f8u, 162000000u);  /* bus clock */
  put32(m, 0x000000fcu, GC_CPU_HZ);   /* core clock */

  /* A system call that goes straight back.
   *
   * `sc` is how the SDK ends a cache flush, and a disc's apploader flushes
   * caches before it has installed anything of its own - so on a real
   * console the boot ROM has already left a handler at the vector. With
   * nothing there the processor takes the exception, fetches zero, and
   * stops on an instruction that does not exist, which reads as a broken
   * apploader rather than a missing handler.
   *
   * Just `rfi`: the system call has already done its work by the time the
   * exception is taken, and the return address is the instruction after
   * it. A game replaces this with its own handler on the way up, and the
   * other vectors are deliberately left empty so that a fault nobody
   * expected is loud rather than silently retried for ever. */
  put32(m, 0x00000c00u, 0x4c000064u); /* rfi */
}

/* Where the loaded program ends, and therefore where memory a game may
 * take for itself begins.
 *
 * The apploader works this out and writes it down, and the SDK's allocator
 * reads it as the bottom of the arena. Leaving it at zero does not make
 * the allocator refuse: it makes it hand out addresses derived from zero,
 * and a game builds its heap somewhere that is not memory. Shrek 2 built
 * one at 0x7E000000 and wrote to it, and the only sign was a single bad
 * access a long way from the cause.
 *
 * Rounded up to a cache line, because that is the granularity everything
 * downstream assumes. */
void gc_set_arena_low(GcMachine *m, uint32_t end) {
  if (end < 0x80000000u) end += 0x80000000u;
  put32(m, 0x00000030u, (end + 31u) & ~31u);
}

/* Where the apploader leaves the filesystem. A game reads the pointer out
 * of the globals rather than assuming this, but it has to be somewhere, and
 * this is the end of the area the SDK treats as its own. */
#define GC_FST_ADDRESS 0x81700000u

/* The second boot-info block: where it is on the disc, how big it is, and
 * where the apploader puts it - near the top of main memory, above the
 * arena, so a game's own allocator never hands it out. */
#define GC_BI2_OFFSET 0x440u
#define GC_BI2_SIZE 0x2000u
#define GC_BI2_ADDRESS 0x817fe000u

int gc_attach_disc(GcMachine *m, struct GcDisc *disc) {
  uint8_t *destination;
  m->disc = disc;
  if (!disc) return 0;

  /* A Wii's disc is the apploader's and IOS's business: nothing is placed
   * in memory ahead of them but the disc's id, which the drive would be
   * asked for first. A disc with no key is still a disc in the drive. */
  if (m->cpu.machine) {
    uint8_t header[0x20];
    if (gc_disc_read_raw(disc, 0, header, sizeof(header)) == 0) {
      uint8_t *low = gc_machine_ram(m, 0, sizeof(header));
      if (low) memcpy(low, header, sizeof(header));
    }
    return 0;
  }

  /* The apploader reads the filesystem into memory and records where it
   * put it. DVDOpen walks it from there; without this a game can still
   * read by offset, but not by name. */
  destination = gc_machine_ram(m, GC_VIEW_TO_PHYSICAL(GC_FST_ADDRESS), disc->fst_size);
  if (!destination) return -1;
  if (gc_disc_read(disc, gc_disc_fst_offset(disc), destination, disc->fst_size) != 0) return -1;

  put32(m, 0x00000038u, GC_FST_ADDRESS);
  put32(m, 0x0000003cu, disc->fst_max_size);
  /* The arena ends below the filesystem, so a game's allocator does not
   * hand out the memory the apploader just filled. */
  put32(m, 0x00000034u, GC_FST_ADDRESS);
  /* The disc id, which the drive would otherwise be asked for. */
  {
    uint8_t header[0x20];
    if (gc_disc_read(disc, 0, header, sizeof(header)) == 0) {
      uint8_t *low = gc_machine_ram(m, 0, sizeof(header));
      if (low) memcpy(low, header, sizeof(header));
    }
  }

  /* The second boot-info block, and the pointer to it.
   *
   * BI2 sits at a fixed place on every disc and holds the things a game's
   * start-up asks about before it asks about anything else - the debug
   * flag, the country, the controller specification. The apploader copies
   * it into memory and leaves a pointer at 0x800000F4, and a game
   * dereferences that pointer without checking it, because on a real
   * console it is never null.
   *
   * Leaving it null is not a small omission that shows up as a missing
   * feature. Shrek 2 read offset eight of it within the first field, got
   * an address of eight, and went downhill from there - half a billion
   * instructions later the program counter was in the exception vectors
   * with thirteen million bad accesses behind it. One unset pointer. */
  destination = gc_machine_ram(m, GC_VIEW_TO_PHYSICAL(GC_BI2_ADDRESS), GC_BI2_SIZE);
  if (!destination) return -1;
  if (gc_disc_read(disc, GC_BI2_OFFSET, destination, GC_BI2_SIZE) != 0) {
    /* An image too small to hold one - the synthetic disc the tests use is
     * a few kilobytes - gets a block of zeroes instead. The pointer is
     * still set, because a game that reads through it wants an address it
     * can read, and a disc without a boot-info block is not a disc that
     * cannot be used. */
    memset(destination, 0, GC_BI2_SIZE);
  }
  put32(m, 0x000000f4u, GC_BI2_ADDRESS);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */

typedef struct GcHook {
  const char *name;
  uint32_t address;
} GcHook;

enum { GC_HOOK_OSREPORT, GC_HOOK_RESET, GC_HOOK_WRITE_CONSOLE, GC_HOOK_COUNT };

static GcHook hooks[GC_HOOK_COUNT] = {
    {"OSReport", 0},
    {"SYS_ResetSystem", 0},
    {"__write_console", 0},
};

/* Bumped whenever the set of hooked addresses changes, so that recompiled
 * code knows to look at them again (gc_os_hook_addresses). */
static unsigned hook_generation;

void gc_register_symbol(GcMachine *m, const char *name, uint32_t address, uint32_t size) {
  unsigned i;
  for (i = 0; i < GC_HOOK_COUNT; i++) {
    if (strcmp(hooks[i].name, name) == 0 && size) {
      hooks[i].address = address;
      hook_generation++;
      if (i == GC_HOOK_OSREPORT) {
        m->osreport_addr = address;
        m->osreport_size = size;
      }
    }
  }
}

void gc_clear_symbols(void) {
  unsigned i;
  for (i = 0; i < GC_HOOK_COUNT; i++) hooks[i].address = 0;
  hook_generation++;
}

unsigned gc_os_hook_addresses(uint32_t *out, unsigned max) {
  unsigned i, n = 0;
  for (i = 0; i < GC_HOOK_COUNT; i++) {
    if (hooks[i].address && n < max) out[n++] = hooks[i].address;
  }
  return n;
}

unsigned gc_os_hook_generation(void) { return hook_generation; }

/* Reading a string out of guest memory, with a bound so that a bad pointer
 * cannot walk the whole address space. */
static void read_string(GcMachine *m, uint32_t ea, char *out, size_t max) {
  size_t i;
  for (i = 0; i + 1 < max; i++) {
    const uint8_t ch = gc_bus_read8(&m->cpu, ea + (uint32_t)i);
    if (m->cpu.pending_vector) {
      m->cpu.pending_vector = 0;
      break;
    }
    if (!ch) break;
    out[i] = (char)ch;
  }
  out[i] = 0;
}

/* The PowerPC EABI passes the first eight integer arguments in r3-r10 and
 * the first eight floating-point arguments in f1-f8. OSReport's format
 * string takes r3, so its arguments start at r4. Anything past the
 * registers would be on the stack; a printf with that many arguments is not
 * something a test program does, and it is reported rather than guessed. */
typedef struct GcVarargs {
  GcMachine *m;
  unsigned gpr;   /* next GPR index */
  unsigned fpr;   /* next FPR index */
  int overflowed;
} GcVarargs;

static uint32_t next_word(GcVarargs *va) {
  if (va->gpr > 10) {
    va->overflowed = 1;
    return 0;
  }
  return va->m->cpu.gpr[va->gpr++];
}

static uint64_t next_doubleword(GcVarargs *va) {
  uint32_t high, low;
  if (va->gpr & 1u) va->gpr++; /* a 64-bit argument starts on an even pair */
  high = next_word(va);
  low = next_word(va);
  return ((uint64_t)high << 32) | low;
}

static double next_double(GcVarargs *va) {
  double value;
  uint64_t bits;
  if (va->fpr > 8) {
    va->overflowed = 1;
    return 0.0;
  }
  bits = va->m->cpu.fpr[va->fpr++].ps0;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/* A printf subset big enough for what test programs print. Each conversion
 * is handed to the host's snprintf with the guest's argument, so widths,
 * flags and precision behave the way the program expects. */
static void format_report(GcMachine *m, const char *format, GcVarargs *va, char *out,
                          size_t max) {
  size_t used = 0;
  const char *p = format;

  while (*p && used + 1 < max) {
    char spec[32];
    unsigned length = 0;
    int is_long_long = 0;
    char conversion;

    if (*p != '%') {
      out[used++] = *p++;
      continue;
    }
    spec[length++] = *p++;
    if (*p == '%') {
      out[used++] = '%';
      p++;
      continue;
    }
    /* flags, width, precision */
    while (*p && length + 2 < sizeof(spec) && strchr("-+ #0123456789.*", *p)) {
      if (*p == '*') {
        /* A width taken from an argument: resolve it and write the number
         * into the specifier. */
        const int width = (int)next_word(va);
        length += (unsigned)snprintf(spec + length, sizeof(spec) - length, "%d", width);
        p++;
        continue;
      }
      spec[length++] = *p++;
    }
    while (*p && (*p == 'l' || *p == 'h' || *p == 'z')) {
      if (*p == 'l' && p[1] == 'l') is_long_long = 1;
      p++;
    }
    if (!*p) break;
    conversion = *p++;

    switch (conversion) {
      case 'd':
      case 'i': {
        if (is_long_long) {
          spec[length++] = 'l';
          spec[length++] = 'l';
          spec[length++] = conversion;
          spec[length] = 0;
          used += (size_t)snprintf(out + used, max - used, spec,
                                   (long long)(int64_t)next_doubleword(va));
        } else {
          spec[length++] = conversion;
          spec[length] = 0;
          used += (size_t)snprintf(out + used, max - used, spec, (int)next_word(va));
        }
        break;
      }
      case 'u':
      case 'x':
      case 'X':
      case 'o': {
        if (is_long_long) {
          spec[length++] = 'l';
          spec[length++] = 'l';
          spec[length++] = conversion;
          spec[length] = 0;
          used += (size_t)snprintf(out + used, max - used, spec,
                                   (unsigned long long)next_doubleword(va));
        } else {
          spec[length++] = conversion;
          spec[length] = 0;
          used += (size_t)snprintf(out + used, max - used, spec, (unsigned)next_word(va));
        }
        break;
      }
      case 'c': {
        spec[length++] = 'c';
        spec[length] = 0;
        used += (size_t)snprintf(out + used, max - used, spec, (int)next_word(va));
        break;
      }
      case 'p': {
        used += (size_t)snprintf(out + used, max - used, "%08x", (unsigned)next_word(va));
        break;
      }
      case 's': {
        char text[512];
        read_string(m, next_word(va), text, sizeof(text));
        spec[length++] = 's';
        spec[length] = 0;
        used += (size_t)snprintf(out + used, max - used, spec, text);
        break;
      }
      case 'f':
      case 'g':
      case 'G':
      case 'e':
      case 'E': {
        spec[length++] = conversion;
        spec[length] = 0;
        used += (size_t)snprintf(out + used, max - used, spec, next_double(va));
        break;
      }
      default:
        /* An unknown conversion is printed literally rather than silently
         * swallowing an argument. */
        if (used + 2 < max) {
          out[used++] = '%';
          out[used++] = conversion;
        }
        break;
    }
    if (used >= max) break;
  }
  out[used < max ? used : max - 1] = 0;
}

static void hook_osreport(GcMachine *m) {
  char format[512];
  char text[1024];
  GcVarargs va;
  va.m = m;
  va.gpr = 4;
  va.fpr = 1;
  va.overflowed = 0;

  read_string(m, m->cpu.gpr[3], format, sizeof(format));
  format_report(m, format, &va, text, sizeof(text));
  fputs(text, stdout);
  if (va.overflowed && m->log) {
    fprintf(m->log, "gc: OSReport ran out of argument registers: %s\n", format);
  }
}

static void hook_write_console(GcMachine *m) {
  /* __write_console(fd, buffer, &length) */
  const uint32_t buffer = m->cpu.gpr[4];
  const uint32_t length_ptr = m->cpu.gpr[5];
  uint32_t length = gc_bus_read32(&m->cpu, length_ptr);
  uint32_t i;
  if (m->cpu.pending_vector) {
    m->cpu.pending_vector = 0;
    return;
  }
  if (length > 4096) length = 4096;
  for (i = 0; i < length; i++) {
    const uint8_t ch = gc_bus_read8(&m->cpu, buffer + i);
    if (m->cpu.pending_vector) {
      m->cpu.pending_vector = 0;
      break;
    }
    fputc(ch, stdout);
  }
}

int gc_os_hook(GcMachine *m) {
  const uint32_t pc = m->cpu.pc;
  unsigned i;
  for (i = 0; i < GC_HOOK_COUNT; i++) {
    if (!hooks[i].address || hooks[i].address != pc) continue;
    switch (i) {
      case GC_HOOK_OSREPORT: hook_osreport(m); break;
      case GC_HOOK_WRITE_CONSOLE: hook_write_console(m); break;
      case GC_HOOK_RESET:
        m->finished = 1;
        return 1;
      default: break;
    }
    /* Return to the caller, as the real function would. */
    m->cpu.pc = m->cpu.lr;
    m->cpu.npc = m->cpu.lr;
    return 1;
  }
  return 0;
}
