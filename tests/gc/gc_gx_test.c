/*
 * The graphics FIFO and the vertex decoder.
 *
 * These are layer-one evidence (AGENTS.md): they pin what this
 * implementation intends, from libogc's GX (zlib) and YAGCD. The hardware
 * layer is dolphin-emu/hwtests' gxtest, which runs on the machine; what it
 * cannot do is fail *usefully* when the parser loses its place, because
 * every command after that point is noise. So the cases most worth having
 * here are the ones about staying in step with the stream: how big a vertex
 * is, and what happens when a command is only half delivered.
 */

#include <stdio.h>
#include <string.h>

#include "gc.h"
#include "gc_gx.h"
#include "gc_gx_blend.h"
#include "gc_gx_draw.h"
#include "gc_gx_tev.h"
#include "gc_efb.h"

static int failures;
static const char *current;
static char gc_message[128];

#define CHECK(condition, ...)                                   \
  do {                                                          \
    if (!(condition)) {                                         \
      printf("FAIL [%s] %s:%d: ", current, __FILE__, __LINE__); \
      printf(__VA_ARGS__);                                      \
      printf("\n");                                             \
      failures++;                                               \
    }                                                           \
  } while (0)

#define CHECK_U32(got, want, what)                                 \
  do {                                                             \
    const uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);    \
    CHECK(g_ == w_, "%s = %u, expected %u", what, g_, w_);         \
  } while (0)

/* ------------------------------------------------------------------ */
/* A machine with a FIFO set up in it, and a way to push bytes through   */

#define FIFO_BASE 0x00100000u
#define FIFO_SIZE 0x00001000u

static GcMachine *machine;

/* What the decoder handed back, so a test can look at it. */
static unsigned seen_kind;
static unsigned seen_count;
static GcGxVertex seen[3];

static void remember(GcMachine *m, unsigned kind, const GcGxVertex *vertices, unsigned count) {
  (void)m;
  seen_kind = kind;
  seen_count = count;
  memcpy(seen, vertices, sizeof(seen));
}

static void start(const char *name) {
  current = name;
  gc_machine_reset(machine);
  machine->log = NULL;
  machine->gx.fifo_base = FIFO_BASE;
  machine->gx.fifo_end = FIFO_BASE + FIFO_SIZE - 4u;
  machine->gx.read_ptr = FIFO_BASE;
  machine->gx.write_ptr = FIFO_BASE;
  machine->gx.linked = 1;
  machine->gx.on_primitive = remember;
  seen_kind = seen_count = 0;
  memset(seen, 0, sizeof(seen));
}

/* The write-gather pipe delivers 32 bytes at a time and never fewer, so
 * that is how the tests deliver them: a command that straddles a burst has
 * to be put back, and doing it any other way would not exercise that. */
static uint8_t pending[32];
static unsigned pending_used;

static void push(const void *bytes, unsigned size) {
  const uint8_t *p = (const uint8_t *)bytes;
  unsigned i;
  for (i = 0; i < size; i++) {
    pending[pending_used++] = p[i];
    if (pending_used == sizeof(pending)) {
      gc_gx_fifo_write(machine, pending, sizeof(pending));
      pending_used = 0;
    }
  }
}

static void push_u8(uint8_t value) { push(&value, 1); }

static void push_u16(uint16_t value) {
  const uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
  push(bytes, 2);
}

static void push_u32(uint32_t value) {
  const uint8_t bytes[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
                            (uint8_t)value};
  push(bytes, 4);
}

static void push_f32(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  push_u32(bits);
}

/* Round the burst out with the padding the hardware uses, so everything
 * written so far is delivered. */
static void flush(void) {
  while (pending_used) push_u8(0x00);
}

static void load_cp(uint8_t reg, uint32_t value) {
  push_u8(0x08);
  push_u8(reg);
  push_u32(value);
}

static void write_memory(uint32_t address, const void *bytes, unsigned size) {
  uint8_t *p = gc_machine_ram(machine, address, size);
  if (p) memcpy(p, bytes, size);
}

/* ------------------------------------------------------------------ */

/* The attribute table, built the way libogc builds it, so that a test says
 * what it means rather than a hexadecimal constant. */
static uint32_t vat_a(unsigned pos_elements, unsigned pos_format, unsigned pos_frac,
                      unsigned col0_elements, unsigned col0_format) {
  return ((pos_elements == 3u) ? 1u : 0u) | ((pos_format & 7u) << 1) | ((pos_frac & 0x1fu) << 4) |
         (((col0_elements == 4u) ? 1u : 0u) << 13) | ((col0_format & 7u) << 14);
}

static void test_vertex_size(void) {
  GcGx gx;
  current = "how big a vertex is";
  memset(&gx, 0, sizeof(gx));

  /* Three floats of position and nothing else: the format util.cpp's own
   * warm-up quad uses, minus its colour. */
  gx.vcd_lo = (GC_GX_ATTR_DIRECT << 9);
  gx.vat_a[0] = vat_a(3, 4, 0, 3, 0);
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 12u, "three floats");

  /* With an RGBA8 colour. */
  gx.vcd_lo |= (GC_GX_ATTR_DIRECT << 13);
  gx.vat_a[0] = vat_a(3, 4, 0, 4, 5);
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 16u, "three floats and a colour");

  /* Signed shorts, two of them, and a 565 colour. */
  gx.vat_a[0] = vat_a(2, 3, 5, 3, 0);
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 4u + 2u, "two shorts and a 565 colour");

  /* Indexed attributes are the index's size, whatever the format says. */
  gx.vcd_lo = (GC_GX_ATTR_INDEX16 << 9) | (GC_GX_ATTR_INDEX8 << 13);
  gx.vat_a[0] = vat_a(3, 4, 0, 4, 5);
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 3u, "an index16 position and an index8 colour");

  /* The matrix indices are a byte each and are never indexed. */
  gx.vcd_lo = 1u | (1u << 1) | (GC_GX_ATTR_DIRECT << 9);
  gx.vat_a[0] = vat_a(3, 4, 0, 3, 0);
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 2u + 12u, "two matrix indices and a position");

  /* A normal is three components; with the table's three-index bit and a
   * normal-binormal-tangent set, it is three indices rather than one. */
  gx.vcd_lo = (GC_GX_ATTR_DIRECT << 9) | (GC_GX_ATTR_DIRECT << 11);
  gx.vat_a[0] = vat_a(3, 4, 0, 3, 0) | (1u << 10); /* s8 normals */
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 12u + 3u, "a position and a byte normal");
  gx.vat_a[0] |= (1u << 9); /* normal, binormal and tangent */
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 12u + 9u, "a position and three byte normals");
  gx.vcd_lo = (GC_GX_ATTR_DIRECT << 9) | (GC_GX_ATTR_INDEX8 << 11);
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 12u + 1u, "one index for three normals");
  gx.vat_a[0] |= (1u << 31); /* three indices, one per vector */
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 12u + 3u, "three indices for three normals");

  /* The texture coordinates are spread over three registers, and the
   * fourth one's fraction is in a different register from its format. */
  memset(&gx, 0, sizeof(gx));
  gx.vcd_hi = (GC_GX_ATTR_DIRECT << 8); /* texture coordinate 4 */
  gx.vat_b[0] = (1u << 27) | (4u << 28);  /* two elements, float */
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 8u, "texture coordinate four, in two registers");
  gx.vcd_hi = (GC_GX_ATTR_DIRECT << 14); /* texture coordinate 7 */
  gx.vat_c[0] = (1u << 23) | (3u << 24); /* two elements, s16 */
  CHECK_U32(gc_gx_vertex_size(&gx, 0), 4u, "texture coordinate seven");
}

static void test_direct_draw(void) {
  current = "a draw with its vertices in the stream";
  start(current);

  load_cp(0x50, (GC_GX_ATTR_DIRECT << 9) | (GC_GX_ATTR_DIRECT << 13));
  load_cp(0x70, vat_a(3, 4, 0, 4, 5));

  push_u8(0x90 | 0u); /* triangles, attribute table 0 */
  push_u16(3);
  push_f32(1.0f); push_f32(2.0f); push_f32(3.0f); push_u32(0x11223344u);
  push_f32(4.0f); push_f32(5.0f); push_f32(6.0f); push_u32(0x55667788u);
  push_f32(7.0f); push_f32(8.0f); push_f32(9.0f); push_u32(0x99aabbccu);
  flush();

  CHECK_U32(machine->gx.primitives, 1u, "primitives");
  CHECK_U32(machine->gx.vertices, 3u, "vertices");
  CHECK_U32(machine->gx.unknown_commands, 0u, "commands nobody could parse");
  CHECK_U32(seen_kind, GC_GX_TRIANGLES, "the primitive");
  CHECK_U32(seen_count, 3u, "the vertex count");
  CHECK(seen[0].position[0] == 1.0f && seen[0].position[2] == 3.0f, "the first position");
  CHECK(seen[2].position[0] == 7.0f, "the last position");
  CHECK_U32(seen[1].color[0][0], 0x55u, "the second vertex's red");
  CHECK_U32(seen[1].color[0][3], 0x88u, "the second vertex's alpha");
}

/* A draw is easily bigger than the 32 bytes the pipe delivers at a time, so
 * the parser has to be able to stop half way and pick the command up again
 * when the rest lands. */
static void test_draw_across_bursts(void) {
  current = "a draw split across bursts";
  start(current);

  load_cp(0x50, GC_GX_ATTR_DIRECT << 9);
  load_cp(0x70, vat_a(3, 4, 0, 3, 0));

  /* The draw is thirty-nine bytes: a command, a count and three vertices of
   * twelve. The pipe delivers thirty-two at a time, so the first burst
   * carries the command and most of the second vertex and no more - which
   * is the case worth testing, and it arrives on its own without anything
   * here having to arrange it. */
  push_u8(0x98 | 0u); /* a triangle strip */
  push_u16(3);
  push_f32(1.0f); push_f32(2.0f); push_f32(3.0f);
  push_f32(4.0f); push_f32(5.0f); push_f32(6.0f);
  push_f32(7.0f); push_f32(8.0f);
  CHECK_U32(machine->gx.primitives, 0u, "primitives after the first burst");
  CHECK_U32(machine->gx.unknown_commands, 0u, "commands nobody could parse");

  push_f32(9.0f);
  flush();

  CHECK_U32(machine->gx.primitives, 1u, "primitives");
  CHECK_U32(seen_kind, GC_GX_TRIANGLE_STRIP, "the primitive");
  CHECK(seen[2].position[2] == 9.0f, "the last position arrived");
}

static void test_indexed_draw(void) {
  static const float positions[6] = {10.0f, 11.0f, 12.0f, 20.0f, 21.0f, 22.0f};
  uint8_t big_endian[sizeof(positions)];
  unsigned i;

  current = "a draw whose positions are in an array";
  start(current);

  for (i = 0; i < 6; i++) {
    uint32_t bits;
    memcpy(&bits, &positions[i], sizeof(bits));
    big_endian[i * 4 + 0] = (uint8_t)(bits >> 24);
    big_endian[i * 4 + 1] = (uint8_t)(bits >> 16);
    big_endian[i * 4 + 2] = (uint8_t)(bits >> 8);
    big_endian[i * 4 + 3] = (uint8_t)bits;
  }
  write_memory(0x00200000u, big_endian, sizeof(big_endian));

  load_cp(0xa0, 0x00200000u); /* the position array */
  load_cp(0xb0, 12u);         /* twelve bytes apart */
  load_cp(0x50, GC_GX_ATTR_INDEX8 << 9);
  load_cp(0x70, vat_a(3, 4, 0, 3, 0));

  push_u8(0x90);
  push_u16(3);
  push_u8(1); push_u8(0); push_u8(1);
  flush();

  CHECK_U32(machine->gx.primitives, 1u, "primitives");
  CHECK(seen[0].position[0] == 20.0f, "the first vertex came from entry one");
  CHECK(seen[1].position[0] == 10.0f, "the second came from entry zero");
  CHECK(seen[2].position[2] == 22.0f, "the third's z");
}

static void test_registers(void) {
  current = "the register files";
  start(current);

  push_u8(0x61);
  push_u32(0x28abcdefu); /* a blitting processor register */
  push_u8(0x10);
  push_u32((1u << 16) | 0x1009u); /* two transform registers from 0x1009 */
  push_u32(0x01020304u);
  push_u32(0x05060708u);
  flush();

  CHECK_U32(machine->gx.bp[0x28], 0x00abcdefu, "the blitting processor register");
  CHECK_U32(machine->gx.xf[0x0009], 0x01020304u, "the first transform register");
  CHECK_U32(machine->gx.xf[0x000a], 0x05060708u, "the second");
  CHECK_U32(machine->gx.unknown_commands, 0u, "commands nobody could parse");
}

/* A matrix goes into the transform unit's own memory, as floats, and comes
 * out of an array by index rather than through the stream. */
static void test_indexed_transform_load(void) {
  uint8_t words[16];
  unsigned i;
  current = "a matrix loaded by index";
  start(current);

  for (i = 0; i < 4; i++) {
    const uint32_t value = 0x3f800000u + i; /* 1.0f and the three after it */
    words[i * 4 + 0] = (uint8_t)(value >> 24);
    words[i * 4 + 1] = (uint8_t)(value >> 16);
    words[i * 4 + 2] = (uint8_t)(value >> 8);
    words[i * 4 + 3] = (uint8_t)value;
  }
  /* Entry two of an array whose entries are sixteen bytes apart. */
  write_memory(0x00300000u + 2u * 16u, words, sizeof(words));

  load_cp(0xac, 0x00300000u); /* array twelve: the transform index A */
  load_cp(0xbc, 16u);
  push_u8(0x20);
  push_u32((2u << 16) | (3u << 12) | 0x0010u); /* entry two, four words, at 0x10 */
  flush();

  CHECK_U32(machine->gx.unknown_commands, 0u, "commands nobody could parse");
  {
    uint32_t bits;
    memcpy(&bits, &machine->gx.xf_memory[0x10], sizeof(bits));
    CHECK_U32(bits, 0x3f800000u, "the first word of the matrix");
    memcpy(&bits, &machine->gx.xf_memory[0x13], sizeof(bits));
    CHECK_U32(bits, 0x3f800003u, "the fourth");
  }
}

static void test_display_list(void) {
  uint8_t list[32];
  current = "a display list";
  start(current);

  memset(list, 0, sizeof(list)); /* the hardware pads a list with nops */
  list[0] = 0x61;                /* a blitting processor register */
  list[1] = 0x30;
  list[2] = 0x00;
  list[3] = 0x11;
  list[4] = 0x22;
  write_memory(0x00400000u, list, sizeof(list));

  push_u8(0x40);
  push_u32(0x00400000u);
  push_u32(sizeof(list));
  flush();

  CHECK_U32(machine->gx.display_lists, 1u, "display lists");
  CHECK_U32(machine->gx.bp[0x30], 0x00001122u, "the register the list wrote");
  CHECK_U32(machine->gx.truncated_lists, 0u, "truncated lists");
  CHECK_U32(machine->gx.unknown_commands, 0u, "commands nobody could parse");
}

/* A display list the game records itself, as the SDK's GXBeginDisplayList
 * does it: the processor's FIFO is pointed at a buffer and unlinked from
 * the graphics processor's, the commands are written, and the FIFO is put
 * back. What is written while unlinked is bytes in memory and nothing
 * else; it runs when the list is called, once.
 *
 * Running it as it was written is what this used to do, against whatever
 * vertex format was current, and then again when the list was called. A
 * game that builds its lists at run time (Super Mario Galaxy 2) drew
 * nothing recognisable. */
static void test_recorded_display_list(void) {
  const uint32_t buffer = 0x00500000u;
  const uint8_t *in_memory;
  current = "a display list recorded through an unlinked FIFO";
  start(current);

  /* Linked, through the registers this time: both FIFOs are the ring. */
  gc_io_write(machine, 0x3000u + 0x0cu, 4, FIFO_BASE);
  gc_io_write(machine, 0x3000u + 0x10u, 4, FIFO_BASE + FIFO_SIZE - 4u);
  gc_io_write(machine, 0x3000u + 0x14u, 4, FIFO_BASE);
  gc_io_write(machine, 0x0000u + 0x02u, 2, 0x0011u);
  CHECK_U32(machine->gx.linked, GC_GX_READS, "linked and reading");

  /* Record: the processor's FIFO is the buffer, two blocks long, and the
   * link is off. */
  gc_io_write(machine, 0x0000u + 0x02u, 2, 0x0001u);
  gc_io_write(machine, 0x3000u + 0x0cu, 4, buffer);
  gc_io_write(machine, 0x3000u + 0x10u, 4, buffer + 32u);
  gc_io_write(machine, 0x3000u + 0x14u, 4, buffer);
  {
    const uint64_t before = machine->gx.bytes;
    push_u8(0x61);
    push_u32(0x32007788u);
    flush();
    CHECK(machine->gx.bytes == before, "nothing reached the graphics processor");
  }
  CHECK_U32(machine->gx.bp[0x32], 0u, "the register the recorded command names is untouched");
  CHECK(machine->gx.read_ptr == machine->gx.write_ptr && machine->gx.write_ptr == FIFO_BASE,
        "the graphics processor's own FIFO has not moved");
  in_memory = gc_machine_ram(machine, buffer, 32);
  CHECK(in_memory && in_memory[0] == 0x61 && in_memory[1] == 0x32 && in_memory[3] == 0x77 && in_memory[4] == 0x88,
        "the command is in the buffer");
  CHECK_U32(gc_io_read(machine, 0x3000u + 0x14u, 4), buffer + 32u, "the processor's write pointer moved on a block");
  /* And wraps after the block the end register names. */
  push_u32(0u);
  flush();
  CHECK_U32(gc_io_read(machine, 0x3000u + 0x14u, 4), buffer, "and wraps to the base after the last block");

  /* Put back, linked, and call the list. */
  gc_io_write(machine, 0x3000u + 0x0cu, 4, FIFO_BASE);
  gc_io_write(machine, 0x3000u + 0x10u, 4, FIFO_BASE + FIFO_SIZE - 4u);
  gc_io_write(machine, 0x3000u + 0x14u, 4, FIFO_BASE);
  gc_io_write(machine, 0x0000u + 0x02u, 2, 0x0011u);
  push_u8(0x40);
  push_u32(buffer);
  push_u32(32u);
  flush();
  CHECK_U32(machine->gx.display_lists, 1u, "display lists");
  CHECK_U32(machine->gx.bp[0x32], 0x00007788u, "the register, once the list is called");
  CHECK_U32(gc_io_read(machine, 0x3000u + 0x14u, 4), FIFO_BASE + 32u,
            "linked, the processor's write pointer follows the graphics processor's");
  CHECK_U32(machine->gx.unknown_commands, 0u, "commands nobody could parse");
}

/* Register 0xfe masks the one write after it: only the bits it names are
 * taken, and the mask is all ones again afterwards. A library that changes
 * one field of a register this way was heard as writing the whole register,
 * which is how a material's "set the blend factors" turned colour update
 * off for everything Super Mario Galaxy 2 drew with it. */
static void test_write_mask(void) {
  current = "the register write mask";
  start(current);

  /* Colour and alpha update on, by a whole write. */
  push_u8(0x61);
  push_u32(0x41000018u);
  /* Then the blend factors alone: bits 5 to 10, and nothing else. */
  push_u8(0x61);
  push_u32(0xfe0007e0u);
  push_u8(0x61);
  push_u32(0x41000120u);
  flush();
  CHECK_U32(machine->gx.bp[0x41], 0x00000138u, "the factors are written and the update bits are kept");

  /* The mask was for that one write. */
  push_u8(0x61);
  push_u32(0x41000004u);
  flush();
  CHECK_U32(machine->gx.bp[0x41], 0x00000004u, "the next write is whole again");

  /* A mask of nothing changes nothing, and is spent all the same. */
  push_u8(0x61);
  push_u32(0xfe000000u);
  push_u8(0x61);
  push_u32(0x41ffffffu);
  push_u8(0x61);
  push_u32(0x40000017u);
  flush();
  CHECK_U32(machine->gx.bp[0x41], 0x00000004u, "a write under an empty mask leaves the register");
  CHECK_U32(machine->gx.bp[0x40], 0x00000017u, "and the write after it is not masked");

  /* Inside a display list too, and across the call. */
  {
    uint8_t list[32];
    memset(list, 0, sizeof(list));
    list[0] = 0x61;
    list[1] = 0xfe;
    list[2] = 0x00;
    list[3] = 0x00;
    list[4] = 0xff;
    list[5] = 0x61;
    list[6] = 0x30;
    list[7] = 0x12;
    list[8] = 0x34;
    list[9] = 0x56;
    write_memory(0x00400000u, list, sizeof(list));
    push_u8(0x61);
    push_u32(0x30abcdefu);
    push_u8(0x40);
    push_u32(0x00400000u);
    push_u32(sizeof(list));
    flush();
    CHECK_U32(machine->gx.bp[0x30], 0x00abcd56u, "a masked write in a display list");
  }
}

/* The two interrupts a game waits on. Without them every GX_DrawDone waits
 * for ever, which is a hang rather than a wrong picture. */
static void test_pixel_engine_interrupts(void) {
  current = "the pixel engine's interrupts";
  start(current);

  /* Enable both, and clear whatever was pending. */
  gc_io_write(machine, 0x1000u + 0x0au, 2, 0x0fu);
  CHECK_U32(machine->pi_cause & ((1u << GC_INT_PE_TOKEN) | (1u << GC_INT_PE_FINISH)), 0u,
            "nothing is pending to start with");

  push_u8(0x61);
  push_u32(0x48000123u); /* a token, with an interrupt */
  flush();
  CHECK_U32(gc_io_read(machine, 0x1000u + 0x0eu, 2), 0x0123u, "the token the stream set");
  CHECK(machine->pi_cause & (1u << GC_INT_PE_TOKEN), "the token interrupt is raised");

  gc_io_write(machine, 0x1000u + 0x0au, 2, 0x07u); /* write the status bit back */
  CHECK_U32(machine->pi_cause & (1u << GC_INT_PE_TOKEN), 0u, "and clears");

  push_u8(0x61);
  push_u32(0x45000002u); /* done drawing */
  flush();
  CHECK(machine->pi_cause & (1u << GC_INT_PE_FINISH), "the finish interrupt is raised");
}

/* A command nobody knows is not skipped: there is no way to know how long
 * it is, so everything after it would be noise. */
static void test_unknown_command(void) {
  current = "a command nobody knows";
  start(current);

  push_u8(0x7f);
  push_u32(0xdeadbeefu);
  flush();

  CHECK_U32(machine->gx.unknown_commands, 1u, "commands nobody could parse");
  CHECK_U32(machine->gx.primitives, 0u, "primitives");
}

/* The ring wraps, and a command that straddles the wrap has to come out
 * whole. */
static void test_ring_wrap(void) {
  unsigned i;
  current = "a command across the end of the ring";
  start(current);

  /* Leave twelve bytes before the wrap, then write a sixteen-byte
   * register load across it. */
  machine->gx.write_ptr = FIFO_BASE + FIFO_SIZE - 32u;
  machine->gx.read_ptr = machine->gx.write_ptr;
  for (i = 0; i < 5; i++) push_u32(0x00000000u); /* twenty bytes of padding */
  push_u8(0x61);
  push_u32(0x31005566u);
  flush();

  CHECK_U32(machine->gx.bp[0x31], 0x00005566u, "the register written across the wrap");
  CHECK_U32(machine->gx.unknown_commands, 0u, "commands nobody could parse");
  CHECK(machine->gx.read_ptr == machine->gx.write_ptr, "the reader caught up");
}

/* ------------------------------------------------------------------ */
/* The transform unit                                                    */

/* Triangles the assembler produced, so a test can count and inspect them. */
static unsigned triangle_count;
static GcGxPoint last_triangle[3];

static void remember_triangle(GcMachine *m, const GcGxPoint *points) {
  (void)m;
  triangle_count++;
  memcpy(last_triangle, points, sizeof(last_triangle));
}

static void set_xf(unsigned reg, uint32_t value) { machine->gx.xf[reg] = value; }

static void set_xf_float(unsigned reg, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  machine->gx.xf[reg] = bits;
}

/* The viewport GX_SetViewport(0, 0, 640, 528, 0, 1) leaves behind, and the
 * orthographic projection every gxtest program draws with. */
static void standard_camera(void) {
  unsigned i;
  set_xf_float(0x1a, 320.0f);       /* width / 2 */
  set_xf_float(0x1b, -264.0f);      /* -height / 2 */
  set_xf_float(0x1c, 16777215.0f);  /* far - near, in depth units */
  set_xf_float(0x1d, 662.0f);       /* 0 + 320 + 342 */
  set_xf_float(0x1e, 606.0f);       /* 0 + 264 + 342 */
  set_xf_float(0x1f, 16777215.0f);  /* far */

  set_xf_float(0x20, 1.0f);
  set_xf_float(0x21, 0.0f);
  set_xf_float(0x22, 1.0f);
  set_xf_float(0x23, 0.0f);
  set_xf_float(0x24, -1.0f);
  set_xf_float(0x25, 0.0f);
  set_xf(0x26, 1u); /* orthographic */

  /* An identity position matrix at index zero. */
  for (i = 0; i < 12; i++) machine->gx.xf_memory[i] = 0.0f;
  machine->gx.xf_memory[0] = 1.0f;
  machine->gx.xf_memory[5] = 1.0f;
  machine->gx.xf_memory[10] = 1.0f;

  /* Both channels take their colour from the vertex. */
  set_xf(0x0e, 1u);
  set_xf(0x0f, 1u);
  set_xf(0x10, 1u);
  set_xf(0x11, 1u);
}

static int close_to(double got, double want) {
  const double difference = got - want;
  return difference > -0.001 && difference < 0.001;
}

static void test_transform(void) {
  GcGxVertex v;
  GcGxPoint out;

  current = "the transform unit";
  start(current);
  standard_camera();

  /* The bottom left of the screen, and the top right. Screen coordinates
   * keep the hardware's origin of 342 in both axes - the viewport
   * registers put it there and the scissor offset register takes it out,
   * which is the rasteriser's job - so the left edge of the framebuffer is
   * 342 here and not zero. */
  memset(&v, 0, sizeof(v));
  v.position[0] = -1.0f;
  v.position[1] = -1.0f;
  v.position[2] = 1.0f;
  gc_gx_transform(&machine->gx, &v, &out);
  CHECK(close_to(out.screen[0], 342.0f), "the left edge, not %f", out.screen[0]);
  CHECK(close_to(out.screen[1], 342.0f + 528.0f), "the bottom, not %f", out.screen[1]);
  CHECK(close_to(out.screen[2], 0.0f), "depth at the near plane is 0, not %f", out.screen[2]);

  v.position[0] = 1.0f;
  v.position[1] = 1.0f;
  v.position[2] = -1.0f;
  gc_gx_transform(&machine->gx, &v, &out);
  CHECK(close_to(out.screen[0], 342.0f + 640.0f), "the right edge, not %f", out.screen[0]);
  CHECK(close_to(out.screen[1], 342.0f), "the top, not %f", out.screen[1]);
  CHECK(close_to(out.screen[2], 2.0f * 16777215.0f), "depth at the far plane, not %f",
        out.screen[2]);

  /* A position matrix that moves the world moves the picture. */
  machine->gx.xf_memory[3] = 0.5f; /* translate x */
  v.position[0] = 0.0f;
  v.position[1] = 0.0f;
  gc_gx_transform(&machine->gx, &v, &out);
  CHECK(close_to(out.screen[0], 342.0f + 480.0f), "a translated x, not %f", out.screen[0]);
  machine->gx.xf_memory[3] = 0.0f;
}

static void test_perspective(void) {
  GcGxVertex v;
  GcGxPoint out;

  current = "a perspective projection";
  start(current);
  standard_camera();

  /* A frustum: x and y pass through, z maps through the near and far
   * planes, and w is the negated view depth. */
  set_xf_float(0x20, 2.0f);
  set_xf_float(0x21, 0.0f);
  set_xf_float(0x22, 2.0f);
  set_xf_float(0x23, 0.0f);
  set_xf_float(0x24, -1.0f);
  set_xf_float(0x25, -1.0f);
  set_xf(0x26, 0u); /* perspective */

  memset(&v, 0, sizeof(v));
  v.position[0] = 1.0f;
  v.position[1] = 1.0f;
  v.position[2] = -2.0f; /* two units in front of the eye */
  gc_gx_transform(&machine->gx, &v, &out);

  CHECK(close_to(out.clip[3], 2.0f), "w is the negated depth, not %f", out.clip[3]);
  CHECK(close_to(out.clip[0], 2.0f), "clip x, not %f", out.clip[0]);
  /* The divide halves it, so the point lands halfway out to the edge. */
  CHECK(close_to(out.screen[0], 1.0f * 320.0f + 662.0f), "screen x, not %f", out.screen[0]);
}

/* Which matrix a vertex uses: its own when the descriptor gave it one, and
 * the register file's otherwise. */
static void test_matrix_index(void) {
  GcGxVertex v;
  current = "the matrix index";
  start(current);

  memset(&v, 0, sizeof(v));
  v.position_matrix = 21;
  v.texture_matrix[0] = 33;

  machine->gx.vcd_lo = 0; /* neither is in the vertex */
  machine->gx.xf[0x18] = 6u | (30u << 6);
  machine->gx.xf[0x19] = 45u;
  CHECK_U32(gc_gx_position_matrix(&machine->gx, &v), 6u, "the position matrix from the registers");
  CHECK_U32(gc_gx_texture_matrix(&machine->gx, &v, 0), 30u, "texture matrix zero");
  CHECK_U32(gc_gx_texture_matrix(&machine->gx, &v, 4), 45u, "texture matrix four");

  machine->gx.vcd_lo = 1u | 2u; /* both are */
  CHECK_U32(gc_gx_position_matrix(&machine->gx, &v), 21u, "the position matrix from the vertex");
  CHECK_U32(gc_gx_texture_matrix(&machine->gx, &v, 0), 33u, "texture matrix zero from the vertex");
}

/* Quads become two triangles, a strip alternates its winding and a fan
 * keeps its first vertex. */
static void test_assembly(void) {
  unsigned i;
  current = "assembling triangles";
  start(current);
  standard_camera();
  machine->gx.on_triangle = remember_triangle;
  triangle_count = 0;

  load_cp(0x50, GC_GX_ATTR_DIRECT << 9);
  load_cp(0x70, vat_a(3, 4, 0, 3, 0));

  push_u8(0x80); /* four vertices, one quad */
  push_u16(4);
  for (i = 0; i < 4; i++) {
    push_f32((float)i);
    push_f32(0.0f);
    push_f32(0.0f);
  }
  flush();
  CHECK_U32(triangle_count, 2u, "a quad is two triangles");
  CHECK_U32(machine->gx.triangles, 2u, "the counter agrees");

  triangle_count = 0;
  push_u8(0x98); /* five vertices in a strip: three triangles */
  push_u16(5);
  for (i = 0; i < 5; i++) {
    push_f32((float)i);
    push_f32(0.0f);
    push_f32(0.0f);
  }
  flush();
  CHECK_U32(triangle_count, 3u, "a five-vertex strip is three triangles");
  /* The third triangle of a strip is wound the way the first was, and the
   * second the other way: its first two vertices come out swapped. */
  CHECK(close_to(last_triangle[0].clip[0] < last_triangle[1].clip[0] ? 1.0f : 0.0f, 1.0f),
        "the last triangle of the strip is wound forwards");

  triangle_count = 0;
  push_u8(0xa0); /* five vertices in a fan: three triangles */
  push_u16(5);
  for (i = 0; i < 5; i++) {
    push_f32((float)i);
    push_f32(0.0f);
    push_f32(0.0f);
  }
  flush();
  CHECK_U32(triangle_count, 3u, "a five-vertex fan is three triangles");
  CHECK(close_to(last_triangle[0].clip[0], -1.0f * 0.0f), "a fan keeps its first vertex");

  machine->gx.on_triangle = NULL;
}

/* The colour a channel produces: the vertex's, or the register's. */
static void test_channels(void) {
  GcGxVertex v;
  GcGxPoint out;
  current = "the colour channels";
  start(current);
  standard_camera();

  memset(&v, 0, sizeof(v));
  v.has_color[0] = 1;
  v.color[0][0] = 0x11;
  v.color[0][1] = 0x22;
  v.color[0][2] = 0x33;
  v.color[0][3] = 0x44;

  gc_gx_transform(&machine->gx, &v, &out);
  CHECK_U32(out.color[0][0], 0x11u, "red from the vertex");
  CHECK_U32(out.color[0][3], 0x44u, "alpha from the vertex");

  /* With the channel taking its material from the register instead. */
  set_xf(0x0e, 0u);
  set_xf(0x10, 0u);
  machine->gx.xf[0x0c] = 0xaabbccddu;
  gc_gx_transform(&machine->gx, &v, &out);
  CHECK_U32(out.color[0][0], 0xaau, "red from the register");
  CHECK_U32(out.color[0][3], 0xddu, "alpha from the register");
}

/* A spot light with no cone - a0 = 1, a1 = a2 = 0, which is what a game
 * that never calls GX_InitLightSpot leaves - lights whatever its direction
 * words hold, because nothing multiplies them into the answer. Super Mario
 * Galaxy 2 leaves them uninitialised, and when they were not a number the
 * host's arithmetic made the whole channel black (its tulips). And a cosine
 * below zero is zero before the quadratic sees it. */
static void light_words(unsigned light, uint32_t colour, const float cosine[3], const float position[3],
                        const float direction[3]) {
  float *const words = &machine->gx.xf_memory[0x600u + 16u * light];
  unsigned i;
  memcpy(&words[3], &colour, sizeof(colour));
  for (i = 0; i < 3; i++) {
    words[4 + i] = cosine[i];
    words[7 + i] = i == 0 ? 1.0f : 0.0f; /* no falloff with distance */
    words[10 + i] = position[i];
    words[13 + i] = direction[i];
  }
}

static void test_spot_light_direction(void) {
  static const float no_cone[3] = {1.0f, 0.0f, 0.0f};
  static const float linear[3] = {0.0f, 1.0f, 0.0f};
  static const float above[3] = {0.0f, 0.0f, 1000.0f};
  static const float towards[3] = {0.0f, 0.0f, 1.0f}, away[3] = {0.0f, 0.0f, -1.0f};
  float not_a_number[3];
  const uint32_t nan_bits = 0xffc12345u;
  GcGxVertex v;
  GcGxPoint out;
  unsigned i;
  current = "a spot light's direction";
  start(current);
  standard_camera();
  for (i = 0; i < 3; i++) memcpy(&not_a_number[i], &nan_bits, sizeof(nan_bits));

  memset(&v, 0, sizeof(v));
  v.has_normal = 1;
  v.normal[0][2] = 1.0f; /* facing the light */
  /* An identity normal matrix at index zero. */
  for (i = 0; i < 9; i++) machine->gx.xf_memory[0x400 + i] = (i % 4u == 0u) ? 1.0f : 0.0f;

  /* Channel 0: material and ambient from the registers, lit by light 0,
   * diffuse clamped, spot attenuation. */
  set_xf(0x0e, (1u << 1) | (1u << 2) | (2u << 7) | (3u << 9));
  set_xf(0x10, 0u);
  machine->gx.xf[0x0a] = 0x20202000u; /* ambient */
  machine->gx.xf[0x0c] = 0xffffffffu; /* material */

  light_words(0, 0x80808000u, no_cone, above, not_a_number);
  gc_gx_transform(&machine->gx, &v, &out);
  CHECK_U32(out.color[0][0], 0xa0u, "no cone, a direction that is not a number: ambient plus the light");

  light_words(0, 0x80808000u, linear, above, towards);
  gc_gx_transform(&machine->gx, &v, &out);
  CHECK_U32(out.color[0][0], 0xa0u, "a cone the vertex is in the middle of");

  light_words(0, 0x80808000u, linear, above, away);
  gc_gx_transform(&machine->gx, &v, &out);
  /* White times an ambient of 0x20 is 0x1f: the product is truncated (modulate). */
  CHECK_U32(out.color[0][0], 0x1fu, "behind the light: the cosine is zero, and so is a1 times it");

  light_words(0, 0x80808000u, linear, above, not_a_number);
  gc_gx_transform(&machine->gx, &v, &out);
  CHECK_U32(out.color[0][0], 0x1fu, "a cone and no direction: nothing from the light, and the ambient kept");
}

/* A coordinate generator's position and normal are the vertex's own: the
 * texture matrix is the only matrix they go through. Here the position
 * matrix doubles and moves the vertex and the normal matrix triples its
 * normal (which lighting then brings to unit length), and neither shows in
 * a coordinate generated through an identity texture matrix. Fed the
 * transformed ones, a skinned model's reflection breaks along its bones
 * (Super Mario Galaxy 2's Luma). */
static void test_texgen_source(void) {
  GcGxVertex v;
  GcGxPoint out;
  float *const memory = machine->gx.xf_memory;
  unsigned i;
  current = "a generator's source";
  start(current);
  standard_camera();

  /* Position matrix zero: twice the vertex, moved. Its normal matrix: three
   * times the normal. */
  memory[0] = 2.0f, memory[3] = 10.0f;
  memory[5] = 2.0f, memory[7] = 20.0f;
  memory[10] = 2.0f, memory[11] = -30.0f;
  for (i = 0; i < 9; i++) memory[0x400 + i] = (i % 4u == 0u) ? 3.0f : 0.0f;
  /* Texture matrix 30: the identity. Texture matrix 33: its first row
   * takes the third component. */
  for (i = 0; i < 24; i++) memory[30 * 4 + i] = 0.0f;
  memory[30 * 4 + 0] = 1.0f;
  memory[30 * 4 + 5] = 1.0f;
  memory[30 * 4 + 10] = 1.0f;
  memory[33 * 4 + 2] = 1.0f;
  memory[33 * 4 + 5] = 1.0f;

  machine->gx.vcd_lo = 0u;
  set_xf(0x18, 0u | (30u << 6) | (30u << 12) | (33u << 18) | (33u << 24));
  set_xf(0x12, 0u); /* no second transform */
  set_xf(0x3f, 4u);
  set_xf(0x40, (1u << 2) | (0u << 7)); /* the position, three components */
  set_xf(0x41, (1u << 2) | (1u << 7)); /* the normal, three components */
  set_xf(0x42, (1u << 2) | (0u << 7)); /* the position's third component */
  set_xf(0x43, (0u << 2) | (0u << 7)); /* the position as two components and a one */

  memset(&v, 0, sizeof(v));
  v.position[0] = 1.0f, v.position[1] = 2.0f, v.position[2] = 3.0f;
  v.has_normal = 1;
  v.normal[0][1] = 0.5f;
  gc_gx_transform(&machine->gx, &v, &out);

  CHECK(close_to(out.texcoord[0][0], 1.0) && close_to(out.texcoord[0][1], 2.0),
        "the position as the vertex has it, not after its matrix: %g %g", (double)out.texcoord[0][0],
        (double)out.texcoord[0][1]);
  CHECK(close_to(out.texcoord[1][0], 0.0) && close_to(out.texcoord[1][1], 0.5),
        "the normal as the vertex has it, not turned and not unit length: %g %g", (double)out.texcoord[1][0],
        (double)out.texcoord[1][1]);
  CHECK(close_to(out.texcoord[2][0], 3.0), "the third component: %g", (double)out.texcoord[2][0]);
  CHECK(close_to(out.texcoord[3][0], 1.0), "a two-component input has one for its third: %g",
        (double)out.texcoord[3][0]);
}

/* ------------------------------------------------------------------ */
/* The texture environment                                               */

/* One stage, built the way the registers pack it. The four worked examples
 * in docs/gc-tev.md are stepped through here with every intermediate the
 * document shows, because the arithmetic is the kind that looks right and
 * is off by one. */
static uint32_t colour_env(unsigned a, unsigned b, unsigned c, unsigned d, unsigned bias,
                           unsigned subtract, unsigned clamp, unsigned scale, unsigned dest) {
  return (d & 0xfu) | ((c & 0xfu) << 4) | ((b & 0xfu) << 8) | ((a & 0xfu) << 12) |
         ((bias & 3u) << 16) | ((subtract & 1u) << 18) | ((clamp & 1u) << 19) |
         ((scale & 3u) << 20) | ((dest & 3u) << 22);
}

/* The selectors the examples use. */
enum {
  TEV_PREV_COLOR = 0,
  TEV_COLOR0 = 2,
  TEV_TEX_COLOR = 8,
  TEV_RAS_COLOR = 10,
  TEV_ONE = 12,
  TEV_HALF = 13,
  TEV_ZERO = 15
};

static void one_stage(uint32_t env) {
  machine->gx.bp[0x00] = 0u; /* one stage */
  machine->gx.bp[0xc0] = env;
  /* An alpha combiner that passes the previous alpha through unchanged. */
  machine->gx.bp[0xc1] = (0u << 4) | (7u << 7) | (7u << 10) | (7u << 13);
}

static void test_tev_modulate(void) {
  GcGxTevPixel pixel;
  current = "the combiner: a plain modulate";
  start(current);

  /* a = zero, b = texture, c = rasterised, d = zero; add, no bias, no
   * scale, clamped. Texture red 200 against rasterised red 128. */
  one_stage(colour_env(TEV_ZERO, TEV_TEX_COLOR, TEV_RAS_COLOR, TEV_ZERO, 0, 0, 1, 0, 0));
  memset(&pixel, 0, sizeof(pixel));
  pixel.texture[0] = 200;
  pixel.raster[0] = 128;
  gc_gx_tev_run(machine, &pixel);

  /* Not 100: the weight is 129/256 rather than 128/255, and the rounding
   * bias adds 128 before the divide. */
  CHECK_U32(pixel.result[0], 101u, "200 modulated by 128");
}

static void test_tev_subtract(void) {
  GcGxTevPixel pixel;
  current = "the combiner: subtract, doubled, biased down";
  start(current);

  /* a = 32 and b = 200 come from the texture and its alpha; c = 64 from
   * the rasterised colour; d = 100 from a register, at full width. */
  one_stage(colour_env(TEV_TEX_COLOR, TEV_PREV_COLOR, TEV_RAS_COLOR, TEV_COLOR0, 2, 1, 0, 1, 1));
  memset(&pixel, 0, sizeof(pixel));
  pixel.texture[0] = 32;
  machine->gx.tev.reg[0][0] = 200; /* previous */
  machine->gx.tev.reg[1][0] = 100; /* register zero, read as d */
  pixel.raster[0] = 64;
  gc_gx_tev_run(machine, &pixel);

  /* The stage writes register one, unclamped, so the value survives
   * negative: d doubled to 200, minus an interpolation of 148, minus a
   * bias of 256. */
  CHECK(machine->gx.tev.reg[1][0] == -204, "the unclamped result is -204, not %d",
        machine->gx.tev.reg[1][0]);
}

static void test_tev_divide(void) {
  GcGxTevPixel pixel;
  current = "the combiner: halved, and negative";
  start(current);

  one_stage(colour_env(TEV_ZERO, TEV_ONE, TEV_ONE, TEV_COLOR0, 2, 0, 0, 3, 1));
  memset(&pixel, 0, sizeof(pixel));
  machine->gx.tev.reg[1][0] = -300;
  gc_gx_tev_run(machine, &pixel);

  /* Three things at once: dividing by two suppresses the rounding bias,
   * the bias term is not scaled because there is no left shift, and the
   * final halving is an arithmetic shift - so -173 becomes -87 and not
   * the -86 that dividing in C would give. */
  CHECK(machine->gx.tev.reg[1][0] == -87, "the halved result is -87, not %d",
        machine->gx.tev.reg[1][0]);
}

static void test_tev_half(void) {
  GcGxTevPixel pixel;
  current = "the combiner: half is not a half";
  start(current);

  /* Interpolating from 255 to 0 by "half", and then the other way round.
   * The weight is 129 of 256, so the two are not the same. */
  one_stage(colour_env(TEV_RAS_COLOR, TEV_ZERO, TEV_HALF, TEV_ZERO, 0, 0, 1, 0, 0));
  memset(&pixel, 0, sizeof(pixel));
  pixel.raster[0] = 255;
  gc_gx_tev_run(machine, &pixel);
  CHECK_U32(pixel.result[0], 127u, "255 towards 0 by half");

  one_stage(colour_env(TEV_ZERO, TEV_RAS_COLOR, TEV_HALF, TEV_ZERO, 0, 0, 1, 0, 0));
  memset(&pixel, 0, sizeof(pixel));
  pixel.raster[0] = 255;
  gc_gx_tev_run(machine, &pixel);
  CHECK_U32(pixel.result[0], 128u, "0 towards 255 by half");
}

/* The konstant table's fractions, which are round-half-up and therefore
 * step 255, 223, 191, 159, 128, 96, 64, 32 - one gap of 31 among sevens
 * of 32. */
static void test_tev_konstants(void) {
  static const unsigned expected[8] = {255, 223, 191, 159, 128, 96, 64, 32};
  unsigned n;
  current = "the combiner: the konstant fractions";
  start(current);

  for (n = 0; n < 8; n++) {
    GcGxTevPixel pixel;
    one_stage(colour_env(TEV_ZERO, TEV_ZERO, TEV_ZERO, 14u, 0, 0, 1, 0, 0));
    /* The konstant selector for stage zero lives in the low half of the
     * first select register. */
    gc_gx_tev_write(machine, 0xf6, (uint32_t)(n << 4));
    memset(&pixel, 0, sizeof(pixel));
    gc_gx_tev_run(machine, &pixel);
    snprintf(gc_message, sizeof(gc_message), "konstant %u", n);
    CHECK_U32(pixel.result[0], expected[n], gc_message);
  }
}

/* The sixteen single-channel konstant selections, 16 to 31. They are
 * channel-major and register-minor - the register is the low two bits and
 * the channel the next two - so 16 is k0.r, 17 is k1.r and 28 is k0.a
 * (docs/gc-tev.md 6.4). Reading the register from the high bits instead
 * gives a different register's different channel for every selector but
 * two, and both the colour and the alpha side are wrong together, so this
 * walks all sixteen on both with a different value in every channel of
 * every register - the arrangement the hardware test uses. */
static void test_tev_konstant_channels(void) {
  unsigned i, n;
  current = "the combiner: the konstant channel selections";
  start(current);

  for (i = 0; i < 4; i++) {
    unsigned channel;
    for (channel = 0; channel < 4; channel++) {
      machine->gx.tev.konstant[i][channel] = (int)(10u + 10u * channel + 50u * i);
    }
  }

  for (n = 0; n < 16u; n++) {
    const unsigned selector = 16u + n;
    const unsigned reg = n & 3u, channel = n >> 2;
    const unsigned expected = 10u + 10u * channel + 50u * reg;
    GcGxTevPixel pixel;

    /* Colour: input d is the konstant, every channel taking the one the
     * selector names. */
    one_stage(colour_env(TEV_ZERO, TEV_ZERO, TEV_ZERO, 14u, 0, 0, 1, 0, 0));
    gc_gx_tev_write(machine, 0xf6, (uint32_t)(selector << 4));
    memset(&pixel, 0, sizeof(pixel));
    gc_gx_tev_run(machine, &pixel);
    snprintf(gc_message, sizeof(gc_message), "colour selector %u is k%u channel %u", selector, reg,
             channel);
    CHECK_U32(pixel.result[0], expected, gc_message);
    CHECK_U32(pixel.result[2], expected, gc_message);

    /* Alpha: input d is the konstant, selector 6. */
    one_stage(colour_env(TEV_ZERO, TEV_ZERO, TEV_ZERO, TEV_ZERO, 0, 0, 1, 0, 0));
    machine->gx.bp[0xc1] = (6u << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19);
    gc_gx_tev_write(machine, 0xf6, (uint32_t)(selector << 9));
    memset(&pixel, 0, sizeof(pixel));
    gc_gx_tev_run(machine, &pixel);
    snprintf(gc_message, sizeof(gc_message), "alpha selector %u is k%u channel %u", selector, reg,
             channel);
    CHECK_U32(pixel.result[3], expected, gc_message);
  }
}

/* The registers a stage writes are eleven bits, signed, and the pixel
 * engine takes the low eight of whatever survives - a wrap, not a clamp. */
static void test_tev_registers(void) {
  GcGxTevPixel pixel;
  current = "the combiner: eleven bits in, eight out";
  start(current);

  /* Eleven bits, signed: 0x3ff is the largest positive and 0x700 is
   * negative. */
  gc_gx_tev_write(machine, 0xe2, (uint32_t)((0x700u << 12) | 0x3ffu)); /* register one */
  CHECK(machine->gx.tev.reg[1][0] == 1023, "eleven bits of red, not %d",
        machine->gx.tev.reg[1][0]);
  CHECK(machine->gx.tev.reg[1][3] == -256, "a negative alpha, not %d", machine->gx.tev.reg[1][3]);

  /* A konstant register shares the number and is told apart by bit 23. */
  gc_gx_tev_write(machine, 0xe2, 0x00800000u | 0x123u);
  CHECK(machine->gx.tev.konstant[1][0] == 0x123, "the konstant register, not %d",
        machine->gx.tev.konstant[1][0]);
  CHECK(machine->gx.tev.reg[1][0] == 1023, "and the colour register is untouched, not %d",
        machine->gx.tev.reg[1][0]);

  /* Pass a negative through to the pixel engine: -128 arrives as 128. */
  one_stage(colour_env(TEV_ZERO, TEV_ZERO, TEV_ZERO, TEV_COLOR0, 0, 0, 0, 0, 0));
  machine->gx.tev.reg[1][0] = -128;
  memset(&pixel, 0, sizeof(pixel));
  gc_gx_tev_run(machine, &pixel);
  CHECK_U32(pixel.result[0], 128u, "a negative narrows by wrapping");
}

/* ------------------------------------------------------------------ */
/* Perspective correction                                                */

/* Enough register state for a triangle to reach the framebuffer: the
 * scissor over the whole of it, the offset that takes the 342 back out,
 * both write enables, an alpha test that always passes, no depth test, and
 * one texture environment stage that passes the rasterised colour on. */
static void ready_to_draw(void) {
  machine->gx.bp[0x20] = (342u << 12) | 342u;
  machine->gx.bp[0x21] = ((342u + 639u) << 12) | (342u + 527u);
  machine->gx.bp[0x59] = (171u << 10) | 171u;
  machine->gx.bp[0x41] = (1u << 3) | (1u << 4);
  machine->gx.bp[0xf3] = (7u << 16) | (7u << 19);
  machine->gx.bp[0x40] = 0;
  machine->gx.bp[0x00] = 0;
  machine->gx.bp[0x43] = 0; /* eight bits a channel, so no dither and no rounding */
  one_stage(colour_env(TEV_ZERO, TEV_ZERO, TEV_ZERO, TEV_RAS_COLOR, 0, 0, 1, 0, 0));
  machine->gx.bp[0xc1] = (0u << 4) | (7u << 7) | (7u << 10) | (5u << 13);
}

/* A vertex already through the transform, so the rasteriser can be handed
 * a triangle with whatever depths this test wants. */
static void point_at(GcGxPoint *p, double x, double y, float w, unsigned red) {
  memset(p, 0, sizeof(*p));
  p->clip[2] = -w; /* on the far plane, which is inside */
  p->clip[3] = w;
  p->screen[0] = 342.0 + x;
  p->screen[1] = 342.0 + y;
  p->screen[2] = 0.0;
  p->color[0][0] = (uint8_t)red;
  p->color[0][3] = 255;
}

/* An attribute is linear in clip space and a barycentric weight is linear
 * in screen space, so interpolating one with the other is only right when
 * every vertex shares a w. This triangle does not: one corner is three
 * times as far away as the others, which pulls the halfway colour from 129
 * down to 65. */
static void test_perspective_correct(void) {
  GcGxPoint triangle[3];
  uint32_t got;
  int red;

  current = "perspective-correct interpolation";
  start(current);
  ready_to_draw();

  point_at(&triangle[0], 0.0, 0.0, 1.0f, 0);
  point_at(&triangle[1], 100.0, 0.0, 3.0f, 255);
  point_at(&triangle[2], 0.0, 100.0, 1.0f, 0);
  gc_gx_raster_triangle(machine, triangle);

  got = gc_efb_get_color(machine, 50, 0);
  red = (int)((got >> 16) & 0xffu);
  CHECK(red > 60 && red < 70, "halfway along the edge is about 65, not %d", red);
  CHECK(red < 100, "and nothing like the 129 that linear interpolation gives");

  /* With every w equal there is nothing to correct, and the same triangle
   * interpolates linearly. */
  start(current);
  ready_to_draw();
  point_at(&triangle[0], 0.0, 0.0, 1.0f, 0);
  point_at(&triangle[1], 100.0, 0.0, 1.0f, 255);
  point_at(&triangle[2], 0.0, 100.0, 1.0f, 0);
  gc_gx_raster_triangle(machine, triangle);

  got = gc_efb_get_color(machine, 50, 0);
  red = (int)((got >> 16) & 0xffu);
  CHECK(red > 124 && red < 134, "with equal depths it is about 129, not %d", red);
}

/* The depth texture: a pixel's depth taken from what the combiner's texture
 * gave, instead of (or on top of) what the triangle gave. A game clears its
 * depth buffer with it - a rectangle, "always", and a texture the colour of
 * the depth it wants - and one that is not heard writes the rectangle's own
 * depth, which is the nearest there is, over the whole screen.
 *
 * Without a texture bound the sample is nothing, so what is tested here is
 * the bias, the two operations, the twenty-four bits kept, and that the
 * depth test uses the result. Which channels make the value is the
 * rasteriser's reading of the reference and has no test of its own. */
static void test_depth_texture(void) {
  GcGxPoint triangle[3];
  const size_t at = (size_t)10 * GC_EFB_WIDTH + 10u;
  unsigned i;

  current = "the depth texture";
  start(current);
  ready_to_draw();
  machine->gx.bp[0x40] = 1u | (7u << 1) | (1u << 4); /* test, always, write */

  /* The triangle's own depth is 1000 everywhere. */
  point_at(&triangle[0], 0.0, 0.0, 1.0f, 200);
  point_at(&triangle[1], 100.0, 0.0, 1.0f, 200);
  point_at(&triangle[2], 0.0, 100.0, 1.0f, 200);
  for (i = 0; i < 3u; i++) triangle[i].screen[2] = 1000.0;

  machine->efb.depth[at] = 0x00ffffffu;
  gc_gx_raster_triangle(machine, triangle);
  CHECK_U32(machine->efb.depth[at], 1000u, "with no depth texture the triangle's depth is written");

  /* Replace: the bias alone, the texture being nothing. */
  machine->gx.bp[0xf4] = 0x00123456u;
  machine->gx.bp[0xf5] = (2u << 2) | 2u;
  gc_gx_raster_triangle(machine, triangle);
  CHECK_U32(machine->efb.depth[at], 0x00123456u, "replace: the depth is the texture's value and the bias");

  /* Add: on top of the triangle's. */
  machine->gx.bp[0xf5] = (1u << 2) | 2u;
  gc_gx_raster_triangle(machine, triangle);
  CHECK_U32(machine->efb.depth[at], 0x00123456u + 1000u, "add: the triangle's depth as well");

  /* Twenty-four bits are kept. */
  machine->gx.bp[0xf4] = 0x00fffff0u;
  gc_gx_raster_triangle(machine, triangle);
  CHECK_U32(machine->efb.depth[at], (0x00fffff0u + 1000u) & 0x00ffffffu, "the sum wraps at twenty-four bits");

  /* And it is the depth the test sees: "less" against what is there. */
  machine->gx.bp[0x40] = 1u | (1u << 1) | (1u << 4);
  machine->gx.bp[0xf4] = 0x00000800u;
  machine->gx.bp[0xf5] = (2u << 2) | 2u;
  machine->efb.depth[at] = 0x00000700u;
  gc_efb_set_color(machine, 10, 10, 0);
  gc_gx_raster_triangle(machine, triangle);
  CHECK_U32(machine->efb.depth[at], 0x00000700u, "a pixel whose texture depth is behind what is there is not written");
  CHECK_U32(gc_efb_get_color(machine, 10, 10) & 0x00ffffffu, 0u, "and leaves no colour");
  machine->efb.depth[at] = 0x00000900u;
  gc_gx_raster_triangle(machine, triangle);
  CHECK_U32(machine->efb.depth[at], 0x00000800u, "one in front of it is");
  CHECK((gc_efb_get_color(machine, 10, 10) & 0x00ff0000u) != 0u, "with its colour");

  machine->gx.bp[0xf4] = 0;
  machine->gx.bp[0xf5] = 0;
}

/* ------------------------------------------------------------------ */
/* A halved copy                                                        */

/* A copy with the halving bit writes a picture half as wide and half as
 * tall, each pixel the mean of a block of four. It halved only the width
 * once: twice the rows the game had made room for, over whatever came after
 * the texture - which is how Super Mario Galaxy 2's pause screen, copying
 * 320 by 228 into a 160 by 114 texture, took the game down. */
static void test_halved_copy(void) {
  enum { DESTINATION = 0x00200000, GUARD = 512 };
  GcEfbCopyJob job;
  uint8_t *out = gc_machine_ram(machine, DESTINATION, GUARD);
  uint32_t first = 0, end = 0, x, y;
  const uint32_t trigger = 0x000200u | (8u << 3) | 3u; /* halved, RGB565, both clamps */
  unsigned i, untouched = 0;
  current = "a halved copy";
  start(current);
  CHECK(out != NULL, "memory for the copy");
  if (!out) return;

  /* Sixteen by eight pixels, red a different amount in each of a block's
   * four corners so that neither a row nor a column on its own gives the
   * mean: 0 and 64 above 128 and 192. */
  for (y = 0; y < 8u; y++) {
    for (x = 0; x < 16u; x++) {
      const uint32_t red = ((y & 1u) ? 128u : 0u) + ((x & 1u) ? 64u : 0u);
      gc_efb_set_color(machine, x, y, 0xff000000u | (red << 16));
    }
  }
  memset(out, 0xee, GUARD);
  machine->gx.bp[0x49] = 0;                         /* from the top left corner */
  machine->gx.bp[0x4a] = (7u << 10) | 15u;          /* sixteen by eight */
  machine->gx.bp[0x4b] = (uint32_t)DESTINATION >> 5;
  machine->gx.bp[0x4d] = 2u;                        /* two tiles a row: eight pixels */
  machine->gx.bp[0x53] = (21u << 12) | (22u << 18); /* the filter that leaves a row alone */
  machine->gx.bp[0x54] = 21u;

  CHECK(gc_efb_copy_prepare(machine, trigger, &job) != 0, "the copy is one this writes");
  CHECK_U32(job.out_width, 8u, "half as wide");
  CHECK_U32(job.out_rows, 4u, "and half as tall");
  gc_efb_copy_bytes(&job, &first, &end);
  CHECK_U32(end - first, 64u, "two tiles: eight by four pixels of two bytes");

  gc_efb_copy(machine, trigger);
  /* The first pixel: (0 + 64 + 128 + 192) / 4 is 96, twelve in five bits. */
  CHECK_U32(((uint32_t)out[0] << 8) | out[1], 12u << 11, "a pixel is the mean of its four");
  for (i = 64; i < GUARD; i++) untouched += out[i] == 0xee;
  CHECK_U32(untouched, (unsigned)GUARD - 64u, "nothing is written past the two tiles");

  machine->gx.bp[0x53] = 0;
  machine->gx.bp[0x54] = 0;
}

/* ------------------------------------------------------------------ */
/* The blender and the dither                                           */

/* One pixel written over a known one, under the registers as they stand. */
static uint32_t written_over(unsigned x, unsigned y, uint32_t was, unsigned r, unsigned g, unsigned b,
                             unsigned a) {
  uint8_t source[4];
  source[0] = (uint8_t)r;
  source[1] = (uint8_t)g;
  source[2] = (uint8_t)b;
  source[3] = (uint8_t)a;
  gc_efb_set_color(machine, x, y, was);
  gc_gx_write_pixel(machine, x, y, source);
  return gc_efb_get_color(machine, x, y) & 0x00ffffffu;
}

#define BLEND_ON (1u | (1u << 3) | (1u << 4))
#define BLEND_FACTORS(source, destination) (((source) << 8) | ((destination) << 5))
#define DITHER_ON (1u << 2)

/* A factor of "one" is one, and a layer that adds nothing changes nothing:
 * with the factors taken as 0 to 255 every blended layer cost the
 * destination a level, and in the six-bit format the dither that followed
 * cost half the pixels four more (gc_gx_blend.c). */
static void test_blend_and_dither(void) {
  const uint32_t saved_mode = machine->gx.bp[0x41], saved_format = machine->gx.bp[0x43];
  const uint32_t saved_alpha = machine->gx.bp[0x42];
  uint32_t got;
  unsigned layer, x, y;

  machine->gx.bp[0x42] = 0;

  /* Eight bits a channel. */
  machine->gx.bp[0x43] = 0;
  machine->gx.bp[0x41] = BLEND_ON | BLEND_FACTORS(1u, 1u);
  CHECK_U32(written_over(8, 8, 0xff283c50u, 20, 30, 40, 255), 0x003c5a78u, "one plus one is the sum");
  CHECK_U32(written_over(8, 8, 0xffc8c8c8u, 100, 100, 100, 255), 0x00ffffffu, "... and stops at 255");

  machine->gx.bp[0x41] = BLEND_ON | BLEND_FACTORS(4u, 5u);
  CHECK_U32(written_over(8, 8, 0xff283c50u, 200, 200, 200, 0), 0x00283c50u,
            "a transparent pixel leaves the destination as it was");
  CHECK_U32(written_over(8, 8, 0xff283c50u, 200, 201, 202, 255), 0x00c8c9cau,
            "an opaque one replaces it exactly");
  /* Alpha 128 is a factor of 129, and 127 the other way one of 127. */
  CHECK_U32(written_over(8, 8, 0xff646464u, 200, 200, 200, 128), 0x00969696u,
            "a half blend: (200 * 129 + 100 * 127) >> 8 is 150");

  /* Six bits a channel, dither on, nothing blended. 134 is a six-bit value
   * as it is stored (33 widened), so it comes back from every place in the
   * matrix; 133 is not, and comes back as 130 once and 134 three times,
   * which is 133 on average. */
  machine->gx.bp[0x43] = 1;
  machine->gx.bp[0x41] = (1u << 3) | (1u << 4) | DITHER_ON;
  for (y = 0; y < 2u; y++) {
    for (x = 0; x < 2u; x++) {
      got = written_over(8 + x, 8 + y, 0, 134, 134, 134, 255);
      CHECK_U32(got, 0x00868686u, "a six-bit value passes the dither unchanged");
      got = written_over(8 + x, 8 + y, 0, 133, 133, 133, 255);
      CHECK_U32(got, (x == 0u && y == 0u) ? 0x00828282u : 0x00868686u, "a value between two steps is dithered");
    }
  }

  /* ... and fifty transparent layers later the pixel under them is still
   * what it was, at every place in the matrix. */
  machine->gx.bp[0x41] = BLEND_ON | BLEND_FACTORS(4u, 5u) | DITHER_ON;
  for (y = 0; y < 2u; y++) {
    for (x = 0; x < 2u; x++) {
      uint8_t clear[4] = {255, 255, 255, 0};
      gc_efb_set_color(machine, 8 + x, 8 + y, 0xff283c50u);
      got = gc_efb_get_color(machine, 8 + x, 8 + y) & 0x00ffffffu;
      for (layer = 0; layer < 50u; layer++) gc_gx_write_pixel(machine, 8 + x, 8 + y, clear);
      CHECK_U32(gc_efb_get_color(machine, 8 + x, 8 + y) & 0x00ffffffu, got,
                "blending nothing over a pixel fifty times leaves it alone");
    }
  }

  /* Five-six-five is rounded and not dithered. */
  machine->gx.bp[0x43] = 2;
  machine->gx.bp[0x41] = (1u << 3) | (1u << 4) | DITHER_ON;
  for (y = 0; y < 2u; y++) {
    for (x = 0; x < 2u; x++) {
      CHECK_U32(written_over(8 + x, 8 + y, 0, 133, 133, 133, 255), 0x00848684u,
                "five-six-five has no dither");
    }
  }

  machine->gx.bp[0x41] = saved_mode;
  machine->gx.bp[0x42] = saved_alpha;
  machine->gx.bp[0x43] = saved_format;
}

int main(void) {
  machine = gc_machine_create();
  if (!machine) {
    printf("gc_gx_test: the machine would not start\n");
    return 1;
  }
  machine->log = NULL;

  test_vertex_size();
  test_direct_draw();
  test_draw_across_bursts();
  test_indexed_draw();
  test_registers();
  test_indexed_transform_load();
  test_display_list();
  test_recorded_display_list();
  test_write_mask();
  test_pixel_engine_interrupts();
  test_unknown_command();
  test_ring_wrap();
  test_transform();
  test_perspective();
  test_matrix_index();
  test_assembly();
  test_channels();
  test_spot_light_direction();
  test_texgen_source();
  test_tev_modulate();
  test_tev_subtract();
  test_tev_divide();
  test_tev_half();
  test_tev_konstants();
  test_tev_konstant_channels();
  test_tev_registers();
  test_perspective_correct();
  test_depth_texture();
  test_halved_copy();
  test_blend_and_dither();

  gc_machine_destroy(machine);
  printf("gc_gx_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
