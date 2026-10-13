/*
 * The Wii's half of the machine, driven from outside: memory, Broadway's
 * extra BATs, Hollywood's registers, the mailbox's handshake and its
 * interrupt. No guest code runs - the bus is called directly and time is
 * moved by hand - so a failure here names a register rather than a symptom.
 */
#include <stdio.h>
#include <string.h>

#include "gc.h"
#include "gc_bus.h"
#include "wii.h"

static int failures;
static int checks;

#define CHECK(condition, ...)                \
  do {                                       \
    checks++;                                \
    if (!(condition)) {                      \
      failures++;                            \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                   \
      printf("\n");                          \
    }                                        \
  } while (0)

/* Hollywood through the uncached view, as software reaches it. */
#define HW(offset) (0xcd000000u + (offset))
#define HW_MIRROR(offset) (0xcd800000u + (offset))

static void advance(GcMachine *m, uint64_t cycles) {
  m->cpu.cycles += cycles;
  wii_machine_advance(m);
}

static GcMachine *fresh(void) {
  GcMachine *m = gc_machine_create();
  if (m) {
    m->log = NULL;
    gc_install_boot_state(m);
  }
  return m;
}

static void test_memory(void) {
  GcMachine *m = fresh();
  GcCpu *c = &m->cpu;

  CHECK(gc_ram_size == GC_MEM1_SIZE + GC_MEM2_SIZE, "a Wii has %u bytes of memory", (unsigned)gc_ram_size);
  CHECK(gc_machine_ram(m, 0, 4) == m->mem1, "MEM1 starts the block");
  CHECK(gc_machine_ram(m, GC_PHYS_MEM2, 4) == m->mem1 + GC_MEM1_SIZE, "MEM2 follows it");
  CHECK(gc_machine_ram(m, GC_MEM1_SIZE - 2u, 4) == NULL, "an access may not run off the end of MEM1");
  CHECK(gc_machine_ram(m, GC_MEM1_SIZE, 4) == NULL, "nothing lies between the blocks");
  CHECK(gc_machine_ram(m, 0x0fffffffu, 4) == NULL, "nor just below MEM2");
  CHECK(gc_machine_ram(m, GC_PHYS_MEM2 + GC_MEM2_SIZE - 4u, 4) != NULL, "MEM2's last word is memory");
  CHECK(gc_machine_ram(m, GC_PHYS_MEM2 + GC_MEM2_SIZE - 2u, 4) == NULL, "and the word after is not");

  /* Register values: with and without the bits that choose a view. */
  CHECK(gc_ram_from_register(0x00123456u) == 0x00123456u, "a MEM1 address is its own offset");
  CHECK(gc_ram_from_register(0x80123456u) == 0x00123456u, "with the cached view's bit");
  CHECK(gc_ram_from_register(0x10123456u) == GC_MEM1_SIZE + 0x00123456u, "a MEM2 address");
  CHECK(gc_ram_from_register(0x90123456u) == GC_MEM1_SIZE + 0x00123456u, "with the cached view's bit");
  CHECK(gc_ram_from_register(0xd0123456u) == GC_MEM1_SIZE + 0x00123456u, "and the uncached one's");
  CHECK(gc_ram_from_register(0x02000000u) >= gc_ram_size, "past MEM1 is not memory");
  CHECK(gc_ram_to_register(GC_MEM1_SIZE + 0x40u) == GC_PHYS_MEM2 + 0x40u, "an offset back to an address");
  CHECK(gc_ram_to_register(0x40u) == 0x40u, "and MEM1's");

  /* Through the bus: both views of both blocks, the long way and by the
   * shortcut (the second access to a piece takes it). */
  gc_bus_write32(c, 0x90200000u, 0x11223344u);
  CHECK(gc_bus_read32(c, 0xd0200000u) == 0x11223344u, "MEM2 reads back through the uncached view");
  CHECK(gc_bus_fast_read32(c, 0x90200000u) == 0x11223344u, "and by the shortcut");
  CHECK(m->mem1[GC_MEM1_SIZE + 0x00200000u] == 0x11, "big-endian, in the host block after MEM1");
  gc_bus_fast_write32(c, 0x93fffffcu, 0xa1b2c3d4u);
  CHECK(gc_bus_read32(c, 0xd3fffffcu) == 0xa1b2c3d4u, "MEM2's last word");
  gc_bus_write32(c, 0x80200000u, 0x55667788u);
  CHECK(gc_bus_read32(c, 0x90200000u) == 0x11223344u, "MEM1 at the same offset is other memory");
  CHECK(m->bad_access == 0, "none of that was a bad access (%u)", m->bad_access);

  /* The second four BAT pairs exist only while HID4 says so. */
  {
    uint32_t hid4 = 0;
    CHECK(gc_bus_spr_read(c, 1011, &hid4) && (hid4 & WII_HID4_SBE), "HID4 has the extra BATs on (%08x)", hid4);
    gc_bus_spr_write(c, 1011, hid4 & ~WII_HID4_SBE);
    (void)gc_bus_read32(c, 0x90200000u);
    CHECK(m->bad_access == 1 && c->pending_vector == GC_VEC_DSI, "with them off, MEM2's view is unmapped");
    c->pending_vector = 0;
    gc_bus_spr_write(c, 1011, hid4);
    CHECK(gc_bus_read32(c, 0x90200000u) == 0x11223344u, "and back on");
    /* DBAT4U is SPR 568; clearing its valid bits unmaps the cached view
     * and leaves the uncached one, which is DBAT5. */
    {
      uint32_t upper = 0;
      gc_bus_spr_read(c, 568, &upper);
      gc_bus_spr_write(c, 568, upper & ~3u);
      (void)gc_bus_read32(c, 0x90200000u);
      CHECK(c->pending_vector == GC_VEC_DSI, "DBAT4 turned off unmaps 0x90000000");
      c->pending_vector = 0;
      CHECK(gc_bus_read32(c, 0xd0200000u) == 0x11223344u, "0xd0000000 is another pair");
      gc_bus_spr_write(c, 568, upper);
    }
  }
  gc_machine_destroy(m);
}

static void test_registers(void) {
  GcMachine *m = fresh();
  GcCpu *c = &m->cpu;

  CHECK(gc_bus_read32(c, HW(0x214)) == 0x11u, "Hollywood's revision");
  CHECK(gc_bus_read32(c, HW_MIRROR(0x214)) == 0x11u, "and again at the mirror");
  gc_bus_write32(c, HW(0x0c0), 0x00000020u);
  CHECK(gc_bus_read32(c, HW_MIRROR(0x0c0)) == 0x20u, "a register written at one address reads at the other");
  /* Part of a register. */
  gc_bus_write8(c, HW(0x0c3), 0xffu);
  CHECK(gc_bus_read32(c, HW(0x0c0)) == 0xffu, "a byte write replaces the byte it covers (%08x)",
        gc_bus_read32(c, HW(0x0c0)));
  CHECK(gc_bus_read16(c, HW(0x0c2)) == 0x00ffu, "and a halfword read returns its half");

  /* The pins: an output reads back; an input with nothing driving it is
   * low, which is the video encoder's acknowledge. */
  gc_bus_write32(c, HW(0x0c4), WII_GPIO_SLOT_LED | WII_GPIO_AVE_SCL);
  gc_bus_write32(c, HW(0x0c0), WII_GPIO_SLOT_LED | WII_GPIO_AVE_SDA);
  CHECK(gc_bus_read32(c, HW(0x0c8)) == WII_GPIO_SLOT_LED, "pins read %08x", gc_bus_read32(c, HW(0x0c8)));

  /* The timer: one count every 384 cycles. */
  {
    const uint32_t before = gc_bus_read32(c, HW(0x010));
    m->cpu.cycles += 384u * 1000u;
    CHECK(gc_bus_read32(c, HW(0x010)) - before == 1000u, "the timer counts the bus clock over 128");
  }

  /* The processor may not see the key memory or the bus protection. */
  CHECK(gc_bus_read32(c, HW(0x064)) == 0u, "AHBPROT reads as no access");
  CHECK(gc_bus_read32(c, HW(0x1f0)) == 0u, "the key memory reads as nothing");

  /* The disc drive, controller ports, expansion bus and audio interface
   * are the GameCube's registers at a second address. */
  gc_bus_write32(c, 0xcd006c04u, 0x00001234u);
  CHECK(gc_bus_read32(c, 0xcc006c04u) == gc_bus_read32(c, 0xcd006c04u), "the audio interface's volume, both ways");
  CHECK(m->unknown_io_reads == 0 && m->unknown_io_writes == 0, "nothing above was undescribed (%u, %u)",
        m->unknown_io_reads, m->unknown_io_writes);
  gc_machine_destroy(m);
}

/* A request block in MEM2, where a game's are. */
#define BLOCK 0x13300000u
#define PATH (BLOCK + 0x40u)

static void put_request(GcMachine *m, uint32_t at, uint32_t type, uint32_t fd, uint32_t a0, uint32_t a1) {
  uint8_t *block = gc_machine_ram(m, at, 0x40);
  memset(block, 0, 0x40);
  wii_put32(block, type);
  wii_put32(block + 8, fd);
  wii_put32(block + 0x0c, a0);
  wii_put32(block + 0x10, a1);
}

static uint32_t ctrl(GcMachine *m) { return gc_bus_read32(&m->cpu, HW(0x004)); }

static void test_mailbox(void) {
  GcMachine *m = fresh();
  GcCpu *c = &m->cpu;
  uint8_t *block = gc_machine_ram(m, BLOCK, 0x80);
  int32_t fd;

  /* Unmask the line, as a game's interrupt set-up does, and enable both of
   * the mailbox's interrupts. */
  m->pi_mask = 1u << GC_INT_HOLLYWOOD;
  gc_bus_write32(c, HW(0x004), 0x38u);
  CHECK(ctrl(m) == 0x38u, "the control register holds the enables and X2 (%02x)", ctrl(m));

  strcpy((char *)block + 0x40, "/dev/es");
  put_request(m, BLOCK, WII_IOS_OPEN, 0, PATH, 0);
  gc_bus_write32(c, HW(0x000), BLOCK);
  gc_bus_write32(c, HW(0x004), (ctrl(m) & 0x30u) | 1u);
  CHECK((ctrl(m) & 0x06u) == 0, "nothing is flagged the moment a request is handed over");
  CHECK(!(m->pi_cause & (1u << GC_INT_HOLLYWOOD)), "and nothing interrupts");

  advance(m, WII_IOS_ACK_CYCLES - 1u);
  CHECK((ctrl(m) & 0x02u) == 0, "not acknowledged early");
  advance(m, 1u);
  CHECK((ctrl(m) & 0x06u) == 0x02u, "acknowledged (%02x)", ctrl(m));
  CHECK(m->pi_cause & (1u << GC_INT_HOLLYWOOD), "with an interrupt");
  CHECK(gc_bus_read32(c, HW(0x030)) & (1u << WII_IRQ_IPC_BROADWAY), "whose cause is the mailbox");

  /* The answer is ready long before the acknowledgement is dealt with, and
   * must wait for it. */
  advance(m, WII_IOS_REPLY_CYCLES * 2u);
  CHECK((ctrl(m) & 0x06u) == 0x02u, "the answer waits behind the acknowledgement (%02x)", ctrl(m));
  CHECK(wii_be32(block) == WII_IOS_OPEN, "and the block is untouched meanwhile");

  /* Clearing the flag alone is not enough: the cause is still set. */
  gc_bus_write32(c, HW(0x004), (ctrl(m) & 0x30u) | 0x02u);
  CHECK((ctrl(m) & 0x06u) == 0, "the flag clears when written");
  CHECK(wii_be32(block) == WII_IOS_OPEN, "the answer still waits for the cause");
  gc_bus_write32(c, HW(0x030), 1u << WII_IRQ_IPC_BROADWAY);
  CHECK((ctrl(m) & 0x06u) == 0x04u, "then it arrives (%02x)", ctrl(m));
  CHECK(gc_bus_read32(c, HW(0x008)) == BLOCK, "naming the block");
  CHECK(wii_be32(block) == WII_IOS_REPLY, "which is marked answered");
  CHECK(wii_be32(block + 8) == WII_IOS_OPEN, "with the request's type where the descriptor was");
  fd = (int32_t)wii_be32(block + 4);
  CHECK(fd >= 0, "and the descriptor as its result (%d)", (int)fd);
  CHECK(m->pi_cause & (1u << GC_INT_HOLLYWOOD), "with an interrupt of its own");

  gc_bus_write32(c, HW(0x004), (ctrl(m) & 0x30u) | 0x04u);
  gc_bus_write32(c, HW(0x030), 1u << WII_IRQ_IPC_BROADWAY);
  gc_bus_write32(c, HW(0x004), (ctrl(m) & 0x30u) | 0x08u);
  CHECK(!(m->pi_cause & (1u << GC_INT_HOLLYWOOD)), "the line drops once both are acknowledged");

  /* With the mailbox's interrupts off the flags still rise; nothing
   * interrupts. */
  gc_bus_write32(c, HW(0x004), 0u);
  put_request(m, BLOCK, WII_IOS_CLOSE, (uint32_t)fd, 0, 0);
  gc_bus_write32(c, HW(0x000), BLOCK);
  gc_bus_write32(c, HW(0x004), 1u);
  advance(m, WII_IOS_ACK_CYCLES);
  CHECK((ctrl(m) & 0x06u) == 0x02u && !(m->pi_cause & (1u << GC_INT_HOLLYWOOD)), "an acknowledgement without an interrupt");
  gc_bus_write32(c, HW(0x004), 0x02u);
  advance(m, WII_IOS_REPLY_CYCLES);
  CHECK((ctrl(m) & 0x06u) == 0x04u && wii_be32(block + 4) == 0u, "the close is answered with zero");
  gc_bus_write32(c, HW(0x004), 0x04u);

  /* A descriptor that is not open, and a block that is not memory. */
  put_request(m, BLOCK, WII_IOS_CLOSE, (uint32_t)fd, 0, 0);
  gc_bus_write32(c, HW(0x000), BLOCK);
  gc_bus_write32(c, HW(0x004), 1u);
  advance(m, WII_IOS_ACK_CYCLES);
  gc_bus_write32(c, HW(0x004), 0x02u);
  advance(m, WII_IOS_REPLY_CYCLES);
  CHECK((int32_t)wii_be32(block + 4) == WII_IPC_EINVAL, "closing it again is an error (%d)",
        (int)(int32_t)wii_be32(block + 4));
  gc_bus_write32(c, HW(0x004), 0x04u);

  /* The event the machine schedules for is the acknowledgement's. */
  put_request(m, BLOCK, WII_IOS_OPEN, 0, PATH, 0);
  gc_bus_write32(c, HW(0x000), BLOCK);
  gc_bus_write32(c, HW(0x004), 1u);
  CHECK(wii_machine_next_event(m, ~0ull) == m->cpu.cycles + WII_IOS_ACK_CYCLES, "the next event is the acknowledgement");
  advance(m, WII_IOS_ACK_CYCLES);
  CHECK(wii_machine_next_event(m, ~0ull) == ~0ull, "and nothing is scheduled while a flag is up");

  CHECK(wii_state(m)->ios.unknown_commands == 0 && wii_state(m)->ios.unknown_devices == 0, "nothing was unknown");
  gc_machine_destroy(m);
}

int main(void) {
  gc_console_select(1);
  test_memory();
  test_registers();
  test_mailbox();
  printf("wii_machine_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
