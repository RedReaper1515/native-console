/*
 * The Wii's half of the machine: choosing the console, Hollywood's own
 * registers, its interrupt controller, Broadway's extra registers, and the
 * state a game finds the machine in (wii.h).
 *
 * The references are WiiBrew's hardware pages (Hollywood/Registers,
 * Hollywood/IRQs, Hardware/IPC, Memory map) and libogc, which is what a
 * program that uses these registers looks like. Nothing here comes from a
 * console's firmware.
 */

#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_disc.h"
#include "wii.h"
#include "wii_nand.h"

/* ------------------------------------------------------------------ */
/* Which console                                                       */

static int console_is_wii;

void gc_console_select(int wii) {
  console_is_wii = wii != 0;
  gc_cpu_hz = wii ? WII_CPU_HZ : 486000000u;
  gc_cycles_per_vi_clock = wii ? WII_CYCLES_PER_VI_CLOCK : 18u;
  gc_ram_size = wii ? GC_MEM1_SIZE + GC_MEM2_SIZE : GC_MEM1_SIZE;
}

int gc_console_is_wii(void) { return console_is_wii; }

int wii_machine_create(GcMachine *m) {
  WiiState *w;
  if (!console_is_wii) return 0;
  w = (WiiState *)calloc(1, sizeof(WiiState));
  if (!w) return -1;
  m->cpu.machine = w;
  return 0;
}

void wii_machine_destroy(GcMachine *m) {
  WiiState *w = wii_state(m);
  if (!w) return;
  /* The flash is written back, if it has a file and has changed. */
  wii_nand_detach(m);
  free(w);
  m->cpu.machine = NULL;
}

/* A reset clears the machine and keeps the host's side of it. */
void wii_machine_reset(GcMachine *m) {
  WiiState *w = wii_state(m);
  WiiHost host;
  if (!w) return;
  host = w->host;
  memset(w, 0, sizeof(*w));
  w->host = host;
  /* Remotes that are switched off, lying flat. */
  {
    unsigned i;
    for (i = 0; i < WII_REMOTES; i++) wii_remote_rest(&w->remote_input[i], 0);
  }
  /* The top bit of HID4 always reads as one. */
  w->hid4 = 0x80000000u;
  m->cpu.pvr = 0x00087102u; /* Broadway */
  wii_ios_reset(m);
}

/* ------------------------------------------------------------------ */
/* The remotes' input                                                  */

void wii_remote_rest(WiiRemoteInput *input, int present) {
  memset(input, 0, sizeof(*input));
  input->present = present ? 1u : 0u;
  input->accel_z = 1000;
  input->nunchuk_accel_z = 1000;
}

void wii_remote_set(GcMachine *m, unsigned index, const WiiRemoteInput *input) {
  WiiState *w = wii_state(m);
  if (!w || index >= WII_REMOTES || !input) return;
  w->remote_input[index] = *input;
  /* A remote that reports only when something changes has something to
   * report. */
  wii_bt_input_changed(m);
}

const WiiRemoteInput *wii_remote_get(const GcMachine *m, unsigned index) {
  const WiiState *w = wii_state(m);
  if (!w || index >= WII_REMOTES) return NULL;
  return &w->remote_input[index];
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */

/* Hollywood's controller ORs its own causes, masked, into one line of the
 * GameCube's. It is a level like every other line here: it stays up for as
 * long as an unmasked cause is set. */
static void update_line(GcMachine *m) {
  WiiState *w = wii_state(m);
  gc_set_interrupt(m, GC_INT_HOLLYWOOD, (w->irq_flags & w->irq_mask) != 0);
}

void wii_set_irq(GcMachine *m, unsigned irq, int raised) {
  WiiState *w = wii_state(m);
  if (!w) return;
  if (raised) {
    w->irq_flags |= 1u << irq;
  } else {
    w->irq_flags &= ~(1u << irq);
  }
  update_line(m);
}

/* The mailbox's cause follows its two flags: set while either is up and
 * allowed to interrupt. A game acknowledges by clearing the flag in the
 * mailbox and then the cause in the controller, in that order, so the cause
 * is only ever *set* from here - cleared by the write that clears it - and
 * comes straight back if the flag was left up. */
void wii_ipc_update(GcMachine *m) {
  WiiState *w = wii_state(m);
  if (!w) return;
  if ((w->ipc_y1 && w->ipc_iy1) || (w->ipc_y2 && w->ipc_iy2)) {
    w->irq_flags |= 1u << WII_IRQ_IPC_BROADWAY;
  }
  update_line(m);
}

/* ------------------------------------------------------------------ */
/* Hollywood's registers                                               */

enum {
  HW_IPC_PPCMSG = 0x000,
  HW_IPC_PPCCTRL = 0x004,
  HW_IPC_ARMMSG = 0x008,
  HW_IPC_ARMCTRL = 0x00c,
  HW_TIMER = 0x010,
  HW_ALARM = 0x014,
  HW_PPCIRQFLAG = 0x030,
  HW_PPCIRQMASK = 0x034,
  HW_ARMIRQFLAG = 0x038,
  HW_ARMIRQMASK = 0x03c,
  HW_SRNPROT = 0x060,
  HW_AHBPROT = 0x064,
  HW_AIPPROT = 0x070,
  HW_GPIOB_OUT = 0x0c0,
  HW_GPIOB_DIR = 0x0c4,
  HW_GPIOB_IN = 0x0c8,
  HW_GPIOB_INTLVL = 0x0cc,
  HW_GPIOB_INTFLAG = 0x0d0,
  HW_GPIOB_INTMASK = 0x0d4,
  HW_GPIOB_STRAPS = 0x0d8,
  HW_GPIO_ENABLE = 0x0dc,
  HW_GPIO_OUT = 0x0e0,
  HW_GPIO_DIR = 0x0e4,
  HW_GPIO_IN = 0x0e8,
  HW_GPIO_INTLVL = 0x0ec,
  HW_GPIO_INTFLAG = 0x0f0,
  HW_GPIO_INTMASK = 0x0f4,
  HW_GPIO_STRAPS = 0x0f8,
  HW_GPIO_OWNER = 0x0fc,
  HW_COMPAT = 0x180,
  HW_SPARE0 = 0x188,
  HW_BOOT0 = 0x18c,
  HW_CLOCKS = 0x190,
  HW_RESETS = 0x194,
  HW_OTPCMD = 0x1ec,
  HW_OTPDATA = 0x1f0,
  HW_VERSION = 0x214
};

/* The timer counts at the bus clock divided by 128, 1.898 MHz: one count
 * every 384 processor cycles. */
#define WII_TIMER_CYCLES 384u

int wii_io_claims(uint32_t physical) { return (physical & 0xff000000u) == WII_PHYS_HOLLYWOOD; }

/* A register that only stores, and is known to. An offset that is none of
 * these and has no behaviour below is counted, like any register of the
 * GameCube's nobody has described. */
static int stores(uint32_t offset) {
  switch (offset) {
    case HW_IPC_ARMCTRL:
    case HW_ALARM:
    case HW_ARMIRQFLAG:
    case HW_ARMIRQMASK:
    case HW_SRNPROT:
    case HW_AIPPROT:
    case HW_GPIOB_OUT:
    case HW_GPIOB_DIR:
    case HW_GPIOB_INTLVL:
    case HW_GPIOB_INTFLAG:
    case HW_GPIOB_INTMASK:
    case HW_GPIOB_STRAPS:
    case HW_GPIO_ENABLE:
    case HW_GPIO_OUT:
    case HW_GPIO_DIR:
    case HW_GPIO_INTLVL:
    case HW_GPIO_INTFLAG:
    case HW_GPIO_INTMASK:
    case HW_GPIO_STRAPS:
    case HW_GPIO_OWNER:
    case HW_COMPAT:
    case HW_SPARE0:
    case HW_BOOT0:
    case HW_CLOCKS:
    case HW_RESETS:
      return 1;
    default:
      /* The PLL and pad-strength registers: written once at start-up by
       * code that never reads anything back that matters. */
      return offset >= 0x1b0u && offset <= 0x1e8u;
  }
}

/* What the pins read as. An output pin reads back what is driven onto it;
 * an input reads what is outside, and what is outside is: a disc in the
 * slot, and a video encoder that pulls its data line low to acknowledge
 * every byte (so the line reads zero whenever the processor lets go of
 * it, which is all libogc's bit-banged serial bus asks). */
static uint32_t gpio_in(GcMachine *m, uint32_t out, uint32_t direction) {
  uint32_t value = out & direction;
  if (m->disc) value |= WII_GPIO_SLOT_IN & ~direction;
  return value;
}

static uint32_t hollywood_read(GcMachine *m, uint32_t offset) {
  WiiState *w = wii_state(m);
  switch (offset) {
    case HW_IPC_PPCMSG: return w->ipc_ppc_message;
    case HW_IPC_PPCCTRL:
      return (uint32_t)w->ipc_x1 | ((uint32_t)w->ipc_y2 << 1) | ((uint32_t)w->ipc_y1 << 2) |
             ((uint32_t)w->ipc_x2 << 3) | ((uint32_t)w->ipc_iy1 << 4) | ((uint32_t)w->ipc_iy2 << 5);
    case HW_IPC_ARMMSG: return w->ipc_arm_message;
    case HW_TIMER:
      return w->timer_value + (uint32_t)((m->cpu.cycles - w->timer_cycles) / WII_TIMER_CYCLES);
    case HW_PPCIRQFLAG: return w->irq_flags;
    case HW_PPCIRQMASK: return w->irq_mask;
    /* The processor has no access to the bus the second processor's
     * devices sit on, which is how a game that came off a disc finds it. */
    case HW_AHBPROT: return 0;
    case HW_GPIOB_IN:
      return gpio_in(m, w->hollywood[HW_GPIOB_OUT / 4u], w->hollywood[HW_GPIOB_DIR / 4u]);
    case HW_GPIO_IN:
      return gpio_in(m, w->hollywood[HW_GPIO_OUT / 4u], w->hollywood[HW_GPIO_DIR / 4u]);
    /* The one-time-programmable memory holds the console's keys, and the
     * processor is not allowed to read it. */
    case HW_OTPCMD:
    case HW_OTPDATA: return 0;
    case HW_VERSION: return 0x00000011u;
    default: break;
  }
  if (!stores(offset)) {
    m->unknown_io_reads++;
    if (m->log && m->unknown_io_reads <= 16) {
      fprintf(m->log, "wii: read of undescribed Hollywood register +%03x, pc %08x\n", offset, m->cpu.pc);
    }
  }
  return w->hollywood[offset / 4u];
}

static void hollywood_write(GcMachine *m, uint32_t offset, uint32_t value) {
  WiiState *w = wii_state(m);
  switch (offset) {
    case HW_IPC_PPCMSG:
      w->ipc_ppc_message = value;
      return;
    case HW_IPC_PPCCTRL:
      /* X1 hands over a request and X2 says the last answer has been dealt
       * with; a one written to Y1 or Y2 acknowledges it; the top two are
       * the interrupt enables. */
      w->ipc_x1 = (uint8_t)(value & 1u);
      w->ipc_x2 = (uint8_t)((value >> 3) & 1u);
      if (value & 2u) w->ipc_y2 = 0;
      if (value & 4u) w->ipc_y1 = 0;
      w->ipc_iy1 = (uint8_t)((value >> 4) & 1u);
      w->ipc_iy2 = (uint8_t)((value >> 5) & 1u);
      if (w->ipc_x1) {
        w->ipc_x1 = 0;
        wii_ios_request(m, w->ipc_ppc_message);
      }
      wii_ios_advance(m);
      wii_ipc_update(m);
      return;
    case HW_IPC_ARMMSG: return; /* Starlet's to write */
    case HW_TIMER:
      w->timer_value = value;
      w->timer_cycles = m->cpu.cycles;
      return;
    case HW_PPCIRQFLAG:
      /* Written back to clear. The mailbox's cause returns at once if its
       * flag is still up, and whatever was waiting for the processor to
       * finish with the last answer can now be delivered. */
      w->irq_flags &= ~value;
      wii_ios_advance(m);
      wii_ipc_update(m);
      return;
    case HW_PPCIRQMASK:
      w->irq_mask = value;
      wii_ipc_update(m);
      return;
    case HW_AHBPROT:
    case HW_OTPCMD:
    case HW_OTPDATA:
    case HW_VERSION:
    case HW_GPIOB_IN:
    case HW_GPIO_IN: return;
    default: break;
  }
  if (!stores(offset)) {
    m->unknown_io_writes++;
    if (m->log && m->unknown_io_writes <= 16) {
      fprintf(m->log, "wii: write of %08x to undescribed Hollywood register +%03x, pc %08x\n", value,
              offset, m->cpu.pc);
    }
  }
  w->hollywood[offset / 4u] = value;
}

/* The block answers at 0x0d000000 and again at 0x0d800000. Inside it, the
 * disc drive, the controller ports, the expansion bus and the audio
 * interface sit where they do in the GameCube's block, and are the same
 * registers; Hollywood's own are words, and an access to part of one reads
 * or replaces the bytes it covers. */
static uint32_t block_offset(uint32_t physical) { return physical & 0x007fffffu; }

uint32_t wii_io_read(GcMachine *m, uint32_t physical, unsigned size) {
  const uint32_t offset = block_offset(physical);
  if (offset >= 0x6000u && offset < 0x7000u) return gc_io_read(m, offset, size);
  gc_machine_sync(m);
  m->cpu.next_event = 0;
  if (offset < WII_HOLLYWOOD_SIZE && size <= 4u) {
    const uint32_t word = hollywood_read(m, offset & ~3u);
    const unsigned within = offset & 3u;
    if (m->trace_io && m->log) fprintf(m->log, "wii: hollywood +%03x -> %08x\n", offset & ~3u, word);
    if (size == 4u) return word;
    return (word >> (8u * (4u - within - size))) & ((1u << (8u * size)) - 1u);
  }
  m->unknown_io_reads++;
  if (m->log && m->unknown_io_reads <= 16) {
    fprintf(m->log, "wii: read of unmapped I/O %08x (%u bytes), pc %08x\n", physical, size, m->cpu.pc);
  }
  return 0;
}

void wii_io_write(GcMachine *m, uint32_t physical, unsigned size, uint32_t value) {
  const uint32_t offset = block_offset(physical);
  if (offset >= 0x6000u && offset < 0x7000u) {
    gc_io_write(m, offset, size, value);
    return;
  }
  gc_machine_sync(m);
  m->cpu.next_event = 0;
  if (offset < WII_HOLLYWOOD_SIZE && size <= 4u) {
    uint32_t word = value;
    if (size < 4u) {
      const unsigned shift = 8u * (4u - (offset & 3u) - size);
      const uint32_t mask = ((1u << (8u * size)) - 1u) << shift;
      word = (hollywood_read(m, offset & ~3u) & ~mask) | ((value << shift) & mask);
    }
    if (m->trace_io && m->log) fprintf(m->log, "wii: hollywood +%03x <- %08x\n", offset & ~3u, word);
    hollywood_write(m, offset & ~3u, word);
    return;
  }
  m->unknown_io_writes++;
  if (m->log && m->unknown_io_writes <= 16) {
    fprintf(m->log, "wii: write of %08x to unmapped I/O %08x (%u bytes), pc %08x\n", value, physical,
            size, m->cpu.pc);
  }
}

/* ------------------------------------------------------------------ */
/* The machine's clockwork                                             */

void wii_machine_advance(GcMachine *m) {
  wii_ios_advance(m);
  wii_ipc_update(m);
}

void wii_machine_field(GcMachine *m) { wii_ios_field(m); }

uint64_t wii_machine_next_event(GcMachine *m, uint64_t next) { return wii_ios_next_event(m, next); }

/* ------------------------------------------------------------------ */
/* Broadway                                                            */

enum { WII_SPR_IBAT4U = 560, WII_SPR_DBAT4U = 568, WII_SPR_HID4 = 1011 };

int wii_spr_write(GcMachine *m, uint32_t spr, uint32_t value) {
  WiiState *w = wii_state(m);
  if (!w) return 0;
  if (spr == WII_SPR_HID4) {
    w->hid4 = value | 0x80000000u;
    gc_cpu_forget_fast(&m->cpu); /* four BAT pairs may have come or gone */
    return 1;
  }
  if (spr >= WII_SPR_IBAT4U && spr < WII_SPR_IBAT4U + 8u) {
    w->ibat_high[spr - WII_SPR_IBAT4U] = value;
    return 1;
  }
  if (spr >= WII_SPR_DBAT4U && spr < WII_SPR_DBAT4U + 8u) {
    w->dbat_high[spr - WII_SPR_DBAT4U] = value;
    gc_cpu_forget_fast(&m->cpu);
    return 1;
  }
  return 0;
}

int wii_spr_read(GcMachine *m, uint32_t spr, uint32_t *value) {
  const WiiState *w = wii_state(m);
  if (!w) return 0;
  if (spr == WII_SPR_HID4) {
    *value = w->hid4;
    return 1;
  }
  if (spr >= WII_SPR_IBAT4U && spr < WII_SPR_IBAT4U + 8u) {
    *value = w->ibat_high[spr - WII_SPR_IBAT4U];
    return 1;
  }
  if (spr >= WII_SPR_DBAT4U && spr < WII_SPR_DBAT4U + 8u) {
    *value = w->dbat_high[spr - WII_SPR_DBAT4U];
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* The state a game starts in                                          */

static void set_bat(uint32_t *pair, uint32_t ea, uint32_t physical, uint32_t size, uint32_t wimg) {
  const uint32_t blocks = size / 0x20000u;
  pair[0] = (ea & 0xfffe0000u) | (((blocks - 1u) & 0x7ffu) << 2) | 2u | 1u;
  pair[1] = (physical & 0xfffe0000u) | (wimg << 3) | 2u;
}

static void put32(GcMachine *m, uint32_t physical, uint32_t value) {
  uint8_t *p = gc_machine_ram(m, physical, 4);
  if (p) wii_put32(p, value);
}

static void put16(GcMachine *m, uint32_t physical, uint16_t value) {
  uint8_t *p = gc_machine_ram(m, physical, 2);
  if (p) wii_put16(p, value);
}

/* The version of IOS a game finds running when nothing says which it asked
 * for: IOS58, a late one that homebrew expects. A disc names its own (the
 * title metadata's "system version"), and wii_after_apploader uses that. */
#define WII_DEFAULT_IOS 58u

/* What each IOS says of itself and where it ends a game's share of MEM2.
 *
 * IOS writes its number and revision, its build date and the memory layout
 * into low memory before a title starts, and the layout is not the same for
 * all of them: the early ones keep the last twelve megabytes of MEM2, IOS28
 * eight, and everything from IOS30 on ten. A game sizes its heaps from what
 * it finds there. Super Mario Galaxy 2 asks for IOS56 and fills the heap it
 * keeps its permanent archives in to within a megabyte or two; told the
 * early layout it had two megabytes less than it was written for and ran
 * out while loading.
 *
 * The figures are each IOS's own, as the Dolphin project read them out of
 * the IOS binaries and tabulated them (Source/Core/Core/IOS/VersionInfo.cpp);
 * docs/WII-PLAN.md records that. The last 0x20000 bytes below the line are
 * the buffers requests are passed in. */
static const struct {
  uint16_t ios;
  uint32_t version; /* (number << 16) | revision */
  uint32_t date;
  uint32_t mem2_end; /* the end of what the processor may touch */
} IOS_VALUES[] = {
    {4, 0x00040003u, 0x00081006u, 0x93400000u},  {9, 0x0009040au, 0x00030110u, 0x93400000u},
    {11, 0x000b000au, 0x00102506u, 0x93400000u}, {12, 0x000c020eu, 0x00030110u, 0x93400000u},
    {13, 0x000d0408u, 0x00030110u, 0x93400000u}, {14, 0x000e0408u, 0x00030110u, 0x93400000u},
    {15, 0x000f0408u, 0x00030110u, 0x93400000u}, {17, 0x00110408u, 0x00030110u, 0x93400000u},
    {20, 0x0014000cu, 0x00102506u, 0x93400000u}, {21, 0x0015040fu, 0x00030110u, 0x93400000u},
    {22, 0x0016050eu, 0x00030110u, 0x93400000u}, {28, 0x001c070fu, 0x00030110u, 0x93800000u},
    {30, 0x001e0a10u, 0x00040308u, 0x93600000u}, {31, 0x001f0e18u, 0x00030110u, 0x93600000u},
    {33, 0x00210e18u, 0x00030110u, 0x93600000u}, {34, 0x00220e18u, 0x00030110u, 0x93600000u},
    {35, 0x00230e18u, 0x00030110u, 0x93600000u}, {36, 0x00240e18u, 0x00030110u, 0x93600000u},
    {37, 0x0025161fu, 0x00030110u, 0x93600000u}, {38, 0x0026101cu, 0x00030110u, 0x93600000u},
    {40, 0x00280911u, 0x00022308u, 0x93600000u}, {41, 0x00290e17u, 0x00030110u, 0x93600000u},
    {43, 0x002b0e17u, 0x00030110u, 0x93600000u}, {45, 0x002d0e17u, 0x00030110u, 0x93600000u},
    {46, 0x002e0e17u, 0x00030110u, 0x93600000u}, {48, 0x0030101cu, 0x00030110u, 0x93600000u},
    {50, 0x00321319u, 0x00101008u, 0x93600000u}, {51, 0x00331219u, 0x00071108u, 0x93600000u},
    {52, 0x0034161du, 0x00101008u, 0x93600000u}, {53, 0x0035161fu, 0x00030110u, 0x93600000u},
    {55, 0x0037161fu, 0x00030110u, 0x93600000u}, {56, 0x0038161eu, 0x00030110u, 0x93600000u},
    {57, 0x0039171fu, 0x00030110u, 0x93600000u}, {58, 0x003a1820u, 0x00030110u, 0x93600000u},
    {59, 0x003b2421u, 0x00101811u, 0x93600000u}, {60, 0x003c181eu, 0x00112408u, 0x93600000u},
    {61, 0x003d161eu, 0x00030110u, 0x93600000u}, {62, 0x003e191eu, 0x00022712u, 0x93600000u},
    {70, 0x00461a1fu, 0x00060309u, 0x93600000u}, {80, 0x00501b20u, 0x00030310u, 0x93600000u},
};

/* Starts `ios` as far as low memory shows: the IOS's own figures, or for
 * one that is not in the table the newest layout under its own number. */
static void wii_ios_started(GcMachine *m, uint32_t ios) {
  WiiState *w = wii_state(m);
  uint32_t version = (ios << 16) | 0x1820u, date = 0x00030110u, mem2_end = 0x93600000u;
  unsigned i;
  for (i = 0; i < sizeof(IOS_VALUES) / sizeof(IOS_VALUES[0]); i++) {
    if (IOS_VALUES[i].ios != ios) continue;
    version = IOS_VALUES[i].version;
    date = IOS_VALUES[i].date;
    mem2_end = IOS_VALUES[i].mem2_end;
    break;
  }
  put32(m, 0x00003120u, mem2_end);             /* the end of what the processor may touch */
  put32(m, 0x00003128u, mem2_end - 0x20000u);  /* a game's MEM2 ends */
  put32(m, 0x00003130u, mem2_end - 0x20000u);  /* the request buffers start */
  put32(m, 0x00003134u, mem2_end);             /* and end */
  put32(m, 0x00003140u, version);
  put32(m, 0x00003144u, date);
  put32(m, 0x00003148u, mem2_end);             /* IOS's own memory starts */
  put32(m, 0x0000314cu, mem2_end + 0x20000u);  /* and ends */
  w->ios.version = version;
}

void wii_install_boot_state(GcMachine *m) {
  GcCpu *c = &m->cpu;
  WiiState *w = wii_state(m);
  if (!w) return;

  /* Both blocks of memory, each cached at 0x8/0x9 and uncached at 0xc/0xd;
   * the register blocks fall inside the uncached view of MEM1's quarter of
   * the address space. MEM2 is on the second four pairs, which is where
   * the system software puts it. A game replaces all of these. */
  set_bat(&c->ibat[0], 0x80000000u, GC_PHYS_MEM1, 0x10000000u, 0u);
  set_bat(&c->dbat[0], 0x80000000u, GC_PHYS_MEM1, 0x10000000u, 0u);
  set_bat(&c->dbat[2], 0xc0000000u, GC_PHYS_MEM1, 0x10000000u, 5u);
  set_bat(&c->ibat[2], 0xc0000000u, GC_PHYS_MEM1, 0x10000000u, 5u);
  set_bat(&w->ibat_high[0], 0x90000000u, GC_PHYS_MEM2, 0x10000000u, 0u);
  set_bat(&w->dbat_high[0], 0x90000000u, GC_PHYS_MEM2, 0x10000000u, 0u);
  set_bat(&w->dbat_high[2], 0xd0000000u, GC_PHYS_MEM2, 0x10000000u, 5u);
  w->hid4 = 0x80000000u | WII_HID4_SBE;
  gc_cpu_forget_fast(c);

  /* IOS leaves the mailbox's interrupt enabled in Hollywood's controller;
   * a game only ever unmasks the line it arrives on. */
  w->irq_mask = 1u << WII_IRQ_IPC_BROADWAY;

  c->msr = GC_MSR_IR | GC_MSR_DR | GC_MSR_FP | GC_MSR_ME;
  c->hid2 = GC_HID2_PSE | GC_HID2_LSQE | GC_HID2_WPE;
  c->hid0 = 0x0011c464u;
  c->l2cr = 0xe0000000u;

  /* The GameCube's globals, with a Wii's figures. */
  put32(m, 0x00000018u, 0x5d1c9ea3u); /* Wii disc magic */
  put32(m, 0x00000020u, 0x0d15ea5eu); /* "booted normally" */
  put32(m, 0x00000024u, 0x00000001u);
  put32(m, 0x00000028u, GC_MEM1_SIZE);
  put32(m, 0x0000002cu, 0x00000023u); /* a retail board */
  put32(m, 0x00000030u, 0x00000000u); /* arena low: the apploader's */
  put32(m, 0x00000034u, 0x817fec60u); /* arena high */
  put32(m, 0x000000ccu, 0x00000000u); /* NTSC */
  put32(m, 0x000000ecu, 0x81800000u);
  put32(m, 0x000000f0u, GC_MEM1_SIZE);
  put32(m, 0x000000f8u, WII_BUS_HZ);
  put32(m, 0x000000fcu, WII_CPU_HZ);

  /* And the Wii's own, from 0x80003100: how much memory there is of each
   * kind, where a game's share of MEM2 ends and the second processor's
   * begins, and which IOS is running. The last megabytes of MEM2 are not a
   * game's: IOS lives there, and the buffers requests are passed in sit
   * just below it. */
  put32(m, 0x00003100u, GC_MEM1_SIZE);  /* MEM1, physical */
  put32(m, 0x00003104u, GC_MEM1_SIZE);  /* MEM1, simulated */
  put32(m, 0x00003108u, 0x81800000u);   /* MEM1 end */
  put32(m, 0x0000310cu, 0x00000000u);   /* MEM1 arena start: the apploader's */
  put32(m, 0x00003110u, 0x00000000u);   /* MEM1 arena end */
  put32(m, 0x00003118u, GC_MEM2_SIZE);  /* MEM2, physical */
  put32(m, 0x0000311cu, GC_MEM2_SIZE);  /* MEM2, simulated */
  put32(m, 0x00003124u, 0x90000800u);   /* a game's MEM2 starts */
  put32(m, 0x00003138u, 0x00000011u);   /* Hollywood's revision */
  put32(m, 0x00003158u, 0x0000ff16u);   /* the memory's maker */
  put32(m, 0x0000315cu, 0x80000000u);   /* booted by the system menu's loader */
  put16(m, 0x000030e6u, 0x8201u);       /* console type, as the SDK reads it */
  wii_ios_started(m, WII_DEFAULT_IOS);  /* where MEM2 ends, and which IOS says so */

  put32(m, 0x00000c00u, 0x4c000064u); /* rfi: see gc_install_boot_state */
}

/* What the system menu fills in once the apploader has finished: the game
 * the disc holds and the kind of title it is. */
void wii_after_apploader(GcMachine *m) {
  WiiState *w = wii_state(m);
  uint8_t *low = gc_machine_ram(m, 0, 0x20);
  uint8_t *id = gc_machine_ram(m, 0x00003180u, 4);
  /* The IOS the title asks for is the one the menu started for it, and the
   * menu read the game through an open partition and left it open. */
  const uint32_t ios = wii_es_disc_ios(m);
  if (ios) wii_ios_started(m, ios >> 16);
  w->di.partition_open = 1;
  if (low && id) memcpy(id, low, 4);
  put32(m, 0x00003184u, 0x80000000u); /* a disc title */
  put32(m, 0x00003188u, wii_state(m)->ios.version); /* the IOS the title asked for */
}
