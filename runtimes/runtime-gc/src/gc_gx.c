/*
 * The command FIFO and the vertex decoder.
 *
 * The write-gather pipe drops 32 bytes at a time into a ring buffer in main
 * memory; the graphics processor reads that ring and executes what it
 * finds. This is that reader. It is written so that an incomplete command
 * is not an error: the stream arrives in 32-byte bursts and a draw can
 * easily straddle several, so a command that runs past what has arrived is
 * put back and retried on the next burst.
 *
 * The one thing that cannot be approximated is the size of a vertex. A draw
 * command carries a count, not a length, and everything after it in the
 * stream depends on working the length out from the vertex descriptor and
 * the attribute table. A mistake there does not draw the wrong triangle; it
 * loses the parser's place and every command after it is garbage. That is
 * why gc_gx_vertex_size is public and tested on its own, and why the runner
 * reports commands it could not parse rather than skipping them quietly.
 *
 * The field layouts come from devkitPro/libogc's GX implementation (zlib),
 * which is on this project's reuse list, and from YAGCD. Dolphin's are not
 * copied (docs/GC-PLAN.md, "What we reuse").
 */

#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_gx.h"
#include "gc_efb.h"
#include "gc_gx_draw.h"
#include "gc_gx_tev.h"
#include "gc_gx_texture.h"
#include "gc_gx_thread.h"
#include "gc_memory.h"
#include "gc_perf.h"

/* A display list can call another one. Hardware allows one level; this
 * allows a few and counts anything deeper rather than recursing forever on
 * a list that calls itself. */
#define GX_MAX_LIST_DEPTH 4

/* ------------------------------------------------------------------ */
/* Reading the stream                                                  */

typedef struct GxStream {
  GcMachine *m;
  uint32_t base;  /* physical, where the ring restarts */
  uint32_t limit; /* physical, one past the end of the ring */
  uint32_t ptr;
  uint32_t available; /* bytes still to be consumed */
  int ring;           /* wraps at the limit; a display list does not */
  int overrun;        /* a read asked for more than had arrived */
  const uint8_t *copy; /* when set, ptr indexes these bytes rather than memory */
  const uint8_t *memory; /* main memory as this stream reads it (ring_view) */
} GxStream;

/* gc_machine_ram(m, address, 1), without the call: a byte of main memory,
 * and zero outside it. The decoder reads every vertex a byte at a time. */
static uint8_t ram_byte(const GcMachine *m, uint32_t address) {
  return address < gc_ram_size ? m->mem1[address] : 0u;
}

/* Main memory as the FIFO ring can be read and written through without
 * waiting. The OpenGL path holds the pages an EFB copy will write until the
 * card has the bytes (gc_memory.h), and a game's ring often ends on the page
 * where its display buffer begins: every command written to the ring's last
 * page, and every one read back from it, then waited up to a frame for the
 * card. The ring's own bytes are never a copy's, so they are reached through
 * memory's second view while their page is held (gc_memory_view_for), and
 * the machine's view otherwise. Remembered until a hold changes. */
static struct {
  const uint8_t *mem1;
  long long generation;
  uint32_t base, limit;
  uint8_t *view;
} ring_view_cache;

static uint8_t *ring_view(GcMachine *m, uint32_t base, uint32_t limit) {
  const long long generation = gc_memory_generation;
  if (limit > gc_ram_size || base >= limit) return m->mem1;
  if (ring_view_cache.mem1 != m->mem1 || ring_view_cache.generation != generation ||
      ring_view_cache.base != base || ring_view_cache.limit != limit) {
    uint8_t *const view = gc_memory_view_for(m->mem1, base, limit - base);
    ring_view_cache.mem1 = m->mem1;
    ring_view_cache.generation = generation;
    ring_view_cache.base = base;
    ring_view_cache.limit = limit;
    ring_view_cache.view = view ? view : m->mem1;
  }
  return ring_view_cache.view;
}

static uint8_t stream_u8(GxStream *s) {
  uint8_t value;
  if (!s->available) {
    s->overrun = 1;
    return 0;
  }
  value = s->copy ? s->copy[s->ptr] : (s->ptr < gc_ram_size ? s->memory[s->ptr] : 0u);
  s->ptr++;
  s->available--;
  if (s->ring && s->ptr >= s->limit) s->ptr = s->base;
  return value;
}

static uint16_t stream_u16(GxStream *s) {
  const uint16_t high = stream_u8(s);
  return (uint16_t)((high << 8) | stream_u8(s));
}

static uint32_t stream_u32(GxStream *s) {
  const uint32_t high = stream_u16(s);
  return (high << 16) | stream_u16(s);
}

/* Plain memory, for the arrays an indexed attribute reads from. */
static uint8_t memory_u8(GcMachine *m, uint32_t address) { return ram_byte(m, address); }

static uint16_t memory_u16(GcMachine *m, uint32_t address) {
  return (uint16_t)((memory_u8(m, address) << 8) | memory_u8(m, address + 1));
}

static uint32_t memory_u32(GcMachine *m, uint32_t address) {
  return ((uint32_t)memory_u16(m, address) << 16) | memory_u16(m, address + 2);
}

/* An attribute comes either from the stream or from an array in memory.
 * One reader for both keeps the decoder from being written twice. */
typedef struct GxSource {
  GxStream *stream; /* when set, the attribute is in the command stream */
  GcMachine *m;
  uint32_t address;
} GxSource;

static uint8_t source_u8(GxSource *src) {
  if (src->stream) return stream_u8(src->stream);
  return memory_u8(src->m, src->address++);
}

static uint16_t source_u16(GxSource *src) {
  uint16_t value;
  if (src->stream) return stream_u16(src->stream);
  value = memory_u16(src->m, src->address);
  src->address += 2;
  return value;
}

static uint32_t source_u32(GxSource *src) {
  uint32_t value;
  if (src->stream) return stream_u32(src->stream);
  value = memory_u32(src->m, src->address);
  src->address += 4;
  return value;
}

/* ------------------------------------------------------------------ */
/* The vertex format                                                   */

/* The descriptor says, for each attribute, whether it is absent, in the
 * stream, or an index into an array. */
static unsigned vcd_position(const GcGx *gx) { return (gx->vcd_lo >> 9) & 3u; }
static unsigned vcd_normal(const GcGx *gx) { return (gx->vcd_lo >> 11) & 3u; }
static unsigned vcd_color(const GcGx *gx, unsigned i) {
  return (gx->vcd_lo >> (13u + 2u * i)) & 3u;
}
static unsigned vcd_texcoord(const GcGx *gx, unsigned i) { return (gx->vcd_hi >> (2u * i)) & 3u; }
static unsigned vcd_position_matrix(const GcGx *gx) { return gx->vcd_lo & 1u; }
static unsigned vcd_texture_matrix(const GcGx *gx, unsigned i) {
  return (gx->vcd_lo >> (1u + i)) & 1u;
}

/* How many bytes one component of a format takes, and how many components
 * a coordinate has. Format 4 is a float; everything below it is a fixed
 * point number the attribute table says where to put the point in. */
static unsigned component_bytes(unsigned format) {
  switch (format) {
    case 0: /* u8 */
    case 1: /* s8 */ return 1;
    case 2: /* u16 */
    case 3: /* s16 */ return 2;
    case 4: /* f32 */ return 4;
    default: return 0;
  }
}

static unsigned color_bytes(unsigned format) {
  switch (format) {
    case 0: return 2; /* rgb565 */
    case 1: return 3; /* rgb888 */
    case 2: return 4; /* rgb888x */
    case 3: return 2; /* rgba4444 */
    case 4: return 3; /* rgba6666 */
    case 5: return 4; /* rgba8888 */
    default: return 0;
  }
}

/* The attribute table is three registers wide, and the texture coordinates
 * are spread across all three in an order that has to be looked up rather
 * than computed. */
typedef GcGxFormat GxFormat;

static GxFormat vat_position(const GcGx *gx, unsigned vat) {
  const uint32_t a = gx->vat_a[vat & 7u];
  GxFormat f;
  f.elements = (a & 1u) ? 3u : 2u;
  f.format = (a >> 1) & 7u;
  f.fraction = (a >> 4) & 0x1fu;
  return f;
}

static GxFormat vat_normal(const GcGx *gx, unsigned vat) {
  const uint32_t a = gx->vat_a[vat & 7u];
  GxFormat f;
  /* One normal, or a normal with its binormal and tangent. */
  f.elements = ((a >> 9) & 1u) ? 9u : 3u;
  f.format = (a >> 10) & 7u;
  /* Normals do not take their scale from the table: the format fixes it,
   * so that a signed byte spans -1 to 1. */
  f.fraction = (f.format == 1u) ? 6u : (f.format == 3u) ? 14u : 0u;
  return f;
}

static GxFormat vat_color(const GcGx *gx, unsigned vat, unsigned which) {
  const uint32_t a = gx->vat_a[vat & 7u];
  GxFormat f;
  f.elements = ((a >> (13u + 4u * which)) & 1u) ? 4u : 3u;
  f.format = (a >> (14u + 4u * which)) & 7u;
  f.fraction = 0;
  return f;
}

static GxFormat vat_texcoord(const GcGx *gx, unsigned vat, unsigned i) {
  const uint32_t a = gx->vat_a[vat & 7u];
  const uint32_t b = gx->vat_b[vat & 7u];
  const uint32_t c = gx->vat_c[vat & 7u];
  GxFormat f;
  f.elements = 1;
  f.format = 0;
  f.fraction = 0;
  switch (i) {
    case 0:
      f.elements = ((a >> 21) & 1u) ? 2u : 1u;
      f.format = (a >> 22) & 7u;
      f.fraction = (a >> 25) & 0x1fu;
      break;
    case 1:
      f.elements = (b & 1u) ? 2u : 1u;
      f.format = (b >> 1) & 7u;
      f.fraction = (b >> 4) & 0x1fu;
      break;
    case 2:
      f.elements = ((b >> 9) & 1u) ? 2u : 1u;
      f.format = (b >> 10) & 7u;
      f.fraction = (b >> 13) & 0x1fu;
      break;
    case 3:
      f.elements = ((b >> 18) & 1u) ? 2u : 1u;
      f.format = (b >> 19) & 7u;
      f.fraction = (b >> 22) & 0x1fu;
      break;
    case 4:
      /* The only field split across two registers. */
      f.elements = ((b >> 27) & 1u) ? 2u : 1u;
      f.format = (b >> 28) & 7u;
      f.fraction = c & 0x1fu;
      break;
    case 5:
      f.elements = ((c >> 5) & 1u) ? 2u : 1u;
      f.format = (c >> 6) & 7u;
      f.fraction = (c >> 9) & 0x1fu;
      break;
    case 6:
      f.elements = ((c >> 14) & 1u) ? 2u : 1u;
      f.format = (c >> 15) & 7u;
      f.fraction = (c >> 18) & 0x1fu;
      break;
    default:
      f.elements = ((c >> 23) & 1u) ? 2u : 1u;
      f.format = (c >> 24) & 7u;
      f.fraction = (c >> 27) & 0x1fu;
      break;
  }
  return f;
}

/* Three normals are three indices, not one, when the table says so. */
static unsigned normal_index_count(const GcGx *gx, unsigned vat) {
  const uint32_t a = gx->vat_a[vat & 7u];
  const int three = (a >> 31) & 1;
  const int nbt = (a >> 9) & 1;
  return (three && nbt) ? 3u : 1u;
}

static unsigned attribute_bytes(unsigned kind, unsigned direct_bytes, unsigned indices) {
  switch (kind) {
    case GC_GX_ATTR_DIRECT: return direct_bytes;
    case GC_GX_ATTR_INDEX8: return indices;
    case GC_GX_ATTR_INDEX16: return 2u * indices;
    default: return 0;
  }
}

unsigned gc_gx_vertex_size(const GcGx *gx, unsigned vat) {
  unsigned size = 0;
  unsigned i;

  /* The matrix indices are one byte each and are never indexed. */
  if (vcd_position_matrix(gx)) size++;
  for (i = 0; i < GC_GX_MAX_TEXCOORDS; i++) {
    if (vcd_texture_matrix(gx, i)) size++;
  }

  {
    const GxFormat f = vat_position(gx, vat);
    size += attribute_bytes(vcd_position(gx), f.elements * component_bytes(f.format), 1);
  }
  {
    const GxFormat f = vat_normal(gx, vat);
    size += attribute_bytes(vcd_normal(gx), f.elements * component_bytes(f.format),
                            normal_index_count(gx, vat));
  }
  for (i = 0; i < 2; i++) {
    const GxFormat f = vat_color(gx, vat, i);
    size += attribute_bytes(vcd_color(gx, i), color_bytes(f.format), 1);
  }
  for (i = 0; i < GC_GX_MAX_TEXCOORDS; i++) {
    const GxFormat f = vat_texcoord(gx, vat, i);
    size += attribute_bytes(vcd_texcoord(gx, i), f.elements * component_bytes(f.format), 1);
  }
  return size;
}

/* ------------------------------------------------------------------ */
/* Decoding one vertex                                                 */

/* A fixed-point component, scaled by where its format puts the point.
 * Bytes are the exception: they are only scaled when the attribute table
 * asks for it, which is what "byte dequant" means. */
static float read_scaled(GxSource *src, unsigned format, unsigned fraction, int byte_dequant) {
  switch (format) {
    case 0: {
      const float value = (float)source_u8(src);
      return byte_dequant ? value / (float)(1u << fraction) : value;
    }
    case 1: {
      const float value = (float)(int8_t)source_u8(src);
      return byte_dequant ? value / (float)(1u << fraction) : value;
    }
    case 2: return (float)source_u16(src) / (float)(1u << fraction);
    case 3: return (float)(int16_t)source_u16(src) / (float)(1u << fraction);
    case 4: {
      const uint32_t bits = source_u32(src);
      float value;
      memcpy(&value, &bits, sizeof(value));
      return value;
    }
    default: return 0.0f;
  }
}

/* Five and six bit channels are widened by repeating their top bits, so
 * that the largest value becomes 255 rather than 248. */
static uint8_t widen(unsigned value, unsigned bits) {
  const unsigned shifted = value << (8u - bits);
  return (uint8_t)(shifted | (shifted >> bits));
}

static void read_color(GxSource *src, unsigned format, uint8_t out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0xff;
  switch (format) {
    case 0: { /* rgb565 */
      const uint16_t v = source_u16(src);
      out[0] = widen((v >> 11) & 0x1fu, 5);
      out[1] = widen((v >> 5) & 0x3fu, 6);
      out[2] = widen(v & 0x1fu, 5);
      break;
    }
    case 1: /* rgb888 */
      out[0] = source_u8(src);
      out[1] = source_u8(src);
      out[2] = source_u8(src);
      break;
    case 2: /* rgb888x */
      out[0] = source_u8(src);
      out[1] = source_u8(src);
      out[2] = source_u8(src);
      (void)source_u8(src);
      break;
    case 3: { /* rgba4444 */
      const uint16_t v = source_u16(src);
      out[0] = widen((v >> 12) & 0xfu, 4);
      out[1] = widen((v >> 8) & 0xfu, 4);
      out[2] = widen((v >> 4) & 0xfu, 4);
      out[3] = widen(v & 0xfu, 4);
      break;
    }
    case 4: { /* rgba6666, three bytes of six-bit channels */
      const uint32_t v = ((uint32_t)source_u8(src) << 16) | ((uint32_t)source_u8(src) << 8) |
                         source_u8(src);
      out[0] = widen((v >> 18) & 0x3fu, 6);
      out[1] = widen((v >> 12) & 0x3fu, 6);
      out[2] = widen((v >> 6) & 0x3fu, 6);
      out[3] = widen(v & 0x3fu, 6);
      break;
    }
    case 5: /* rgba8888 */
      out[0] = source_u8(src);
      out[1] = source_u8(src);
      out[2] = source_u8(src);
      out[3] = source_u8(src);
      break;
    default: break;
  }
}

/* The array an indexed attribute reads from, and the address of one entry
 * in it. Arrays 0 to 11 are the vertex attributes in descriptor order. */
static uint32_t array_entry(const GcGx *gx, unsigned array, uint32_t index) {
  return gx->array_base[array & 15u] + index * gx->array_stride[array & 15u];
}

/* Where this attribute's bytes are: in the stream, or in its array. */
static int open_source(GcMachine *m, GxStream *s, const GcGx *gx, unsigned kind, unsigned array,
                       GxSource *out) {
  out->m = m;
  out->stream = NULL;
  out->address = 0;
  switch (kind) {
    case GC_GX_ATTR_NONE: return 0;
    case GC_GX_ATTR_DIRECT: out->stream = s; return 1;
    case GC_GX_ATTR_INDEX8: out->address = array_entry(gx, array, stream_u8(s)); return 1;
    default: out->address = array_entry(gx, array, stream_u16(s)); return 1;
  }
}

/* What decode_vertex reads out of the descriptor and the attribute table,
 * which cannot change during a draw: worked out once a draw rather than once
 * a vertex. */
typedef GcGxLayout GxLayout;

static void layout_of(const GcGx *gx, unsigned vat, GxLayout *l) {
  unsigned i;
  l->vat = vat;
  l->size = gc_gx_vertex_size(gx, vat);
  l->byte_dequant = (gx->vat_a[vat & 7u] >> 30) & 1;
  l->position_matrix = vcd_position_matrix(gx);
  l->texture_matrix = 0;
  for (i = 0; i < GC_GX_MAX_TEXCOORDS; i++) {
    if (vcd_texture_matrix(gx, i)) l->texture_matrix |= 1u << i;
  }
  l->position_kind = vcd_position(gx);
  l->position = vat_position(gx, vat);
  l->normal_kind = vcd_normal(gx);
  l->normal = vat_normal(gx, vat);
  l->normal_indices = normal_index_count(gx, vat);
  for (i = 0; i < 2; i++) {
    l->color_kind[i] = vcd_color(gx, i);
    l->color[i] = vat_color(gx, vat, i);
  }
  for (i = 0; i < GC_GX_MAX_TEXCOORDS; i++) {
    l->texcoord_kind[i] = vcd_texcoord(gx, i);
    l->texcoord[i] = vat_texcoord(gx, vat, i);
  }
}

static void decode_vertex(GcMachine *m, GxStream *s, const GcGx *gx, const GxLayout *l,
                          GcGxVertex *v) {
  const int byte_dequant = l->byte_dequant;
  GxSource src;
  unsigned i, c;

  memset(v, 0, sizeof(*v));

  if (l->position_matrix) v->position_matrix = (uint8_t)(stream_u8(s) & 0x3fu);
  for (i = 0; i < GC_GX_MAX_TEXCOORDS; i++) {
    if ((l->texture_matrix >> i) & 1u) v->texture_matrix[i] = (uint8_t)(stream_u8(s) & 0x3fu);
  }

  if (open_source(m, s, gx, l->position_kind, 0, &src)) {
    const GxFormat f = l->position;
    for (c = 0; c < f.elements; c++) {
      v->position[c] = read_scaled(&src, f.format, f.fraction, byte_dequant);
    }
  }

  if (l->normal_kind != GC_GX_ATTR_NONE) {
    const GxFormat f = l->normal;
    const unsigned sets = f.elements / 3u;
    const unsigned indices = l->normal_indices;
    v->has_normal = 1;
    if (indices == 3u && l->normal_kind != GC_GX_ATTR_DIRECT) {
      /* Three separate indices, one for each of the three vectors. */
      for (i = 0; i < 3; i++) {
        if (!open_source(m, s, gx, l->normal_kind, 1, &src)) break;
        for (c = 0; c < 3; c++) v->normal[i][c] = read_scaled(&src, f.format, f.fraction, 1);
      }
    } else if (open_source(m, s, gx, l->normal_kind, 1, &src)) {
      for (i = 0; i < sets; i++) {
        for (c = 0; c < 3; c++) v->normal[i][c] = read_scaled(&src, f.format, f.fraction, 1);
      }
    }
  }

  for (i = 0; i < 2; i++) {
    if (open_source(m, s, gx, l->color_kind[i], 2u + i, &src)) {
      read_color(&src, l->color[i].format, v->color[i]);
      v->has_color[i] = 1;
    }
  }

  for (i = 0; i < GC_GX_MAX_TEXCOORDS; i++) {
    if (open_source(m, s, gx, l->texcoord_kind[i], 4u + i, &src)) {
      const GxFormat f = l->texcoord[i];
      for (c = 0; c < f.elements; c++) {
        v->texcoord[i][c] = read_scaled(&src, f.format, f.fraction, byte_dequant);
      }
      v->texcoord_count = (uint8_t)(i + 1u);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Register writes                                                     */

static void write_cp(GcMachine *m, unsigned reg, uint32_t value) {
  GcGx *gx = &m->gx;
  /* Held triangles are transformed under the registers they arrived with. */
  gc_gx_raster_flush(m);
  switch (reg & 0xf0u) {
    case 0x20: gx->perf_metric = value; return; /* a counter selector */
    case 0x30: gx->matrix_index_a = value; return;
    case 0x40: gx->matrix_index_b = value; return;
    case 0x50: gx->vcd_lo = value; return;
    case 0x60: gx->vcd_hi = value; return;
    case 0x70: gx->vat_a[reg & 7u] = value; return;
    case 0x80: gx->vat_b[reg & 7u] = value; return;
    case 0x90: gx->vat_c[reg & 7u] = value; return;
    case 0xa0: gx->array_base[reg & 15u] = gc_ram_from_register(value); return; /* kept as an offset */
    case 0xb0: gx->array_stride[reg & 15u] = value & 0xffu; return;
    default: break;
  }
  /* Matrix index and vertex format are all a stream can set; anything else
   * is a register only the CPU writes. */
  gx->unknown_commands++;
  if (m->log && gx->unknown_commands <= 16) {
    fprintf(m->log, "gx: stream wrote CP register %02x = %08x\n", reg, value);
  }
}

static void write_xf(GcMachine *m, unsigned address, uint32_t value) {
  GcGx *gx = &m->gx;
  gc_gx_raster_flush(m);
  if (address < GC_GX_XF_MEMORY) {
    memcpy(&gx->xf_memory[address], &value, sizeof(value));
    return;
  }
  if (address >= 0x1000u && address < 0x1000u + GC_GX_XF_REGS) {
    gx->xf[address - 0x1000u] = value;
    return;
  }
  gx->unknown_commands++;
  if (m->log && gx->unknown_commands <= 16) {
    fprintf(m->log, "gx: stream wrote XF address %04x = %08x\n", address, value);
  }
}

/* The pixel engine's two interrupts are raised from the command stream, not
 * by the CPU: a game writes a token or a "done" into the stream and waits
 * for the interrupt that arrives when the graphics processor reaches it.
 * Without these, every GX_DrawDone waits for ever. */
/* A write to a blitting-processor register, through the write mask.
 *
 * Register 0xfe is a mask for the write that follows it: only the bits set
 * in it are taken from that write, and the register's other bits stay as
 * they were. After that one write the mask is all ones again. It is how a
 * library changes one field of a register it does not keep a copy of: the
 * sound of it is "set the blend factors, leave colour update alone". A game
 * built on Nintendo's own engine does that in the display lists of every
 * material; taken as whole-register writes they turned colour and alpha
 * update off for nearly everything Super Mario Galaxy 2 draws in three
 * dimensions.
 *
 * What is kept in slot 0xfe is the complement - the bits *not* to take - so
 * that a machine that has never been told a mask, whose registers are all
 * zero, takes every bit, as it must.
 *
 * Returns the register's value after the write. */
static uint32_t bp_store(GcGx *gx, unsigned reg, uint32_t payload) {
  uint32_t keep;
  if (reg == 0xfeu) {
    gx->bp[0xfe] = ~payload & 0x00ffffffu;
    return payload;
  }
  keep = gx->bp[0xfe];
  gx->bp[0xfe] = 0;
  gx->bp[reg] = (gx->bp[reg] & keep) | (payload & ~keep & 0x00ffffffu);
  return gx->bp[reg];
}

static void write_bp(GcMachine *m, uint32_t value) {
  GcGx *gx = &m->gx;
  const unsigned reg = value >> 24;
  uint32_t payload;

  /* Held triangles are drawn under the registers they arrived with. */
  gc_gx_raster_flush(m);
  payload = bp_store(gx, reg, value & 0x00ffffffu);
  if (reg == 0xfeu) return;
  if (gc_gx_tev_write(m, reg, payload)) return;
  if (gc_gx_tlut_write(m, reg, payload)) return;
  switch (reg) {
    case 0x52: /* copy what is in the framebuffer out to memory */
      gc_efb_copy(m, payload);
      break;
    case 0x45: /* draw done */
      /* On the graphics thread the machine's thread has raised these
       * already, when it read the command (scan_bp). */
      if ((payload & 2u) != 0 && !gc_gx_thread_on_thread()) gc_pe_finish(m);
      break;
    case 0x47: /* a token, with no interrupt */
      if (!gc_gx_thread_on_thread()) gc_pe_token(m, (uint16_t)payload, 0);
      break;
    case 0x48: /* a token, with one */
      if (!gc_gx_thread_on_thread()) gc_pe_token(m, (uint16_t)payload, 1);
      break;
    default: break;
  }
}

/* ------------------------------------------------------------------ */
/* The command loop                                                    */

static void run_stream(GcMachine *m, GxStream *s, unsigned depth);

static void draw(GcMachine *m, GxStream *s, unsigned command) {
  GcGx *gx = &m->gx;
  const unsigned vat = command & 7u;
  const unsigned kind = command & 0xf8u;
  const unsigned count = stream_u16(s);
  const unsigned size = gc_gx_vertex_size(gx, vat);
  GcGxVertex vertices[3];
  GxLayout layout;
  unsigned i;

  if (s->overrun) return;
  /* A format with no position is not something to guess at: the stream
   * cannot be advanced past it, so the parser says so and stops. */
  if (!size && count) {
    s->overrun = 1;
    gx->unknown_commands++;
    if (m->log && gx->unknown_commands <= 16) {
      fprintf(m->log, "gx: a draw of %u vertices with no format, at %08x\n", count, s->ptr);
    }
    return;
  }
  if ((uint32_t)count * size > s->available) {
    s->overrun = 1; /* the rest has not arrived yet */
    return;
  }

  layout_of(gx, vat, &layout);
  gx->kind_vertices[(kind - 0x80u) >> 3] += count;
  if (!gx->on_primitive) {
    /* How many triangles primitive assembly makes of this many vertices. */
    unsigned triangles = 0;
    switch (kind) {
      case GC_GX_QUADS:
      case GC_GX_QUADS_2: triangles = (count / 4u) * 2u; break;
      case GC_GX_TRIANGLES: triangles = count / 3u; break;
      case GC_GX_TRIANGLE_STRIP:
      case GC_GX_TRIANGLE_FAN: triangles = count >= 3u ? count - 2u : 0u; break;
      default: break;
    }
    if (triangles) {
      uint8_t *const bytes = gc_gx_raster_hold_draw(m, kind, &layout, count, triangles);
      if (bytes) {
        const uint32_t total = (uint32_t)count * size;
        uint32_t k;
        if (s->copy && !s->ring) {
          memcpy(bytes, s->copy + s->ptr, total);
          s->ptr += total;
          s->available -= total;
        } else {
          for (k = 0; k < total; k++) bytes[k] = stream_u8(s);
        }
        gx->triangles += triangles;
        gx->vertices += count;
        gx->primitives++;
        return;
      }
    }
  }
  gc_gx_assemble_begin(gx, kind);
  for (i = 0; i < count; i++) {
    GcGxVertex *vertex = &vertices[i < 3u ? i : 2u];
    decode_vertex(m, s, gx, &layout, vertex);
    if (s->overrun) return;
    gc_gx_assemble_vertex(m, vertex);
  }
  gx->vertices += count;
  gx->primitives++;
  /* The first three vertices, before anything was done to them: what the
   * decoder's own tests look at. */
  if (gx->on_primitive) gx->on_primitive(m, kind, vertices, count);
}

void gc_gx_decode_vertex_bytes(GcMachine *m, const GcGxLayout *layout, const uint8_t *bytes,
                               GcGxVertex *out) {
  GxStream s;
  s.m = m;
  s.base = 0;
  s.limit = layout->size;
  s.ptr = 0;
  s.available = layout->size;
  s.ring = 0;
  s.overrun = 0;
  s.copy = bytes;
  s.memory = m->mem1;
  decode_vertex(m, &s, &m->gx, layout, out);
}

static void call_display_list(GcMachine *m, GxStream *s, unsigned depth) {
  GcGx *gx = &m->gx;
  const uint32_t address = gc_ram_from_register(stream_u32(s)) & ~31u;
  const uint32_t size = stream_u32(s) & 0x03ffffe0u;
  GxStream list;

  if (s->overrun) return;
  if (depth >= GX_MAX_LIST_DEPTH) {
    gx->truncated_lists++;
    return;
  }
  gx->display_lists++;
  gx->display_list_depth = depth + 1u;

  list.m = m;
  list.base = address;
  list.limit = address + size;
  list.ptr = address;
  list.available = size;
  list.ring = 0;
  list.overrun = 0;
  list.copy = NULL;
  list.memory = m->mem1;
  run_stream(m, &list, depth + 1u);

  /* A list whose last command runs past its end is a list that was built
   * wrong, and worth saying so: the hardware pads with NOPs to 32 bytes. */
  if (list.overrun) gx->truncated_lists++;
}

static void run_stream(GcMachine *m, GxStream *s, unsigned depth) {
  GcGx *gx = &m->gx;

  while (s->available && !s->overrun) {
    /* A copy asked for inside the stream can change what may be read how. */
    if (s->ring) s->memory = ring_view(m, s->base, s->limit);
    const uint32_t start_ptr = s->ptr;
    const uint32_t start_available = s->available;
    const unsigned command = stream_u8(s);

    if (command == 0x00u) continue; /* padding, and most of a flush */

    switch (command) {
      case 0x08: { /* a command processor register */
        const unsigned reg = stream_u8(s);
        const uint32_t value = stream_u32(s);
        if (!s->overrun) write_cp(m, reg, value);
        break;
      }
      case 0x10: { /* one or more transform registers */
        const uint32_t header = stream_u32(s);
        const unsigned count = ((header >> 16) & 0xfu) + 1u;
        const unsigned address = header & 0xffffu;
        unsigned i;
        if (!s->overrun && count * 4u > s->available) {
          s->overrun = 1;
          break;
        }
        for (i = 0; i < count && !s->overrun; i++) {
          const uint32_t value = stream_u32(s);
          if (!s->overrun) write_xf(m, address + i, value);
        }
        break;
      }
      case 0x20:
      case 0x28:
      case 0x30:
      case 0x38: { /* transform data, indexed out of an array */
        const uint32_t header = stream_u32(s);
        const unsigned array = 12u + ((command - 0x20u) >> 3);
        const unsigned address = header & 0xfffu;
        const unsigned count = ((header >> 12) & 0xfu) + 1u;
        const uint32_t index = header >> 16;
        unsigned i;
        if (s->overrun) break;
        for (i = 0; i < count; i++) {
          write_xf(m, address + i, memory_u32(m, array_entry(gx, array, index) + 4u * i));
        }
        break;
      }
      case 0x40: call_display_list(m, s, depth); break;
      case 0x44: break; /* a metric the hardware counts and nothing reads */
      case 0x48: break; /* invalidate the vertex cache */
      case 0x61: {      /* a blitting processor register */
        const uint32_t value = stream_u32(s);
        if (!s->overrun) write_bp(m, value);
        break;
      }
      default:
        if (command >= 0x80u && command < 0xc0u) {
          draw(m, s, command);
          break;
        }
        gx->unknown_commands++;
        if (m->log && gx->unknown_commands <= 16) {
          fprintf(m->log, "gx: command %02x at %08x is not one this knows\n", command, start_ptr);
        }
        /* Carrying on from here would be reading noise as commands. The
         * rest of what has arrived is dropped, and the count says so. */
        s->available = 0;
        return;
    }

    if (s->overrun) {
      /* Put it back: the rest of this command is still on its way. */
      s->ptr = start_ptr;
      s->available = start_available;
      return;
    }
    gx->commands++;
  }
}

/* ------------------------------------------------------------------ */
/* The stream on the machine's thread, for the graphics thread          */

/* With a graphics thread (gc_gx_thread.h), gc_gx_run walks the stream with
 * run_stream's own control flow - the same bytes consumed, the same
 * commands put back when they have not all arrived, the same counts - and
 * does here only what the processor can see or what must be read now: the
 * registers that decide a command's length, the finish and token
 * interrupts, an EFB copy's preparation and holds, and the counters. Every
 * complete command goes on the queue, in order. What differs from what the
 * queue carries:
 *
 *  - a display list call is replaced by the list's own commands, copied out
 *    of memory as they are walked (so nothing can change them before the
 *    graphics thread runs them);
 *  - a matrix load out of an array (0x20-0x38) becomes a plain transform
 *    register write of the values read now;
 *  - a palette load carries the entries it copies, read now;
 *  - an EFB copy is prepared here and handed over as prepared;
 *  - a draw is preceded by the memory it will read - the reach of its
 *    indexed arrays, and the textures its pipeline samples - sent into the
 *    graphics thread's own copy of memory as it is now. */

static void scan_stream(GcMachine *m, GcGxThread *t, GxStream *s, unsigned depth);


/* The bytes of the command that started at start_ptr and has been read up
 * to s->ptr, onto the queue. */
static void emit_command(GcMachine *m, GcGxThread *t, const GxStream *s, uint32_t start_ptr, uint32_t length) {
  uint8_t *out;
  uint32_t i, at = start_ptr;
  if (!length) return;
  out = gc_gx_thread_commands(t, m, length);
  if (!s->ring && at < gc_ram_size && length <= gc_ram_size - at) {
    memcpy(out, s->memory + at, length);
    return;
  }
  for (i = 0; i < length; i++) {
    out[i] = at < gc_ram_size ? s->memory[at] : 0u;
    at++;
    if (s->ring && at >= s->limit) at = s->base;
  }
}

static void scan_cp(GcMachine *m, unsigned reg, uint32_t value) {
  GcGx *gx = &m->gx;
  switch (reg & 0xf0u) {
    case 0x20: gx->perf_metric = value; return;
    case 0x30: gx->matrix_index_a = value; return;
    case 0x40: gx->matrix_index_b = value; return;
    case 0x50: gx->vcd_lo = value; return;
    case 0x60: gx->vcd_hi = value; return;
    case 0x70: gx->vat_a[reg & 7u] = value; return;
    case 0x80: gx->vat_b[reg & 7u] = value; return;
    case 0x90: gx->vat_c[reg & 7u] = value; return;
    case 0xa0: gx->array_base[reg & 15u] = gc_ram_from_register(value); return; /* kept as an offset */
    case 0xb0: gx->array_stride[reg & 15u] = value & 0xffu; return;
    default: break;
  }
  gx->unknown_commands++;
  if (m->log && gx->unknown_commands <= 16) {
    fprintf(m->log, "gx: stream wrote CP register %02x = %08x\n", reg, value);
  }
}

/* A blitting-processor write: stored, and whatever of it the processor can
 * see done now. Returns 1 when the command went onto the queue here. */
static int scan_bp(GcMachine *m, GcGxThread *t, uint32_t value) {
  GcGx *gx = &m->gx;
  const unsigned reg = value >> 24;
  const uint32_t payload = bp_store(gx, reg, value & 0x00ffffffu);
  gc_gx_thread_textures_changed(t);
  switch (reg) {
    case 0x45:
      if ((payload & 2u) != 0) gc_pe_finish(m);
      return 0;
    case 0x47: gc_pe_token(m, (uint16_t)payload, 0); return 0;
    case 0x48: gc_pe_token(m, (uint16_t)payload, 1); return 0;
    case 0x52: {
      GcEfbCopyJob job;
      if (!gc_efb_copy_prepare(m, payload, &job)) {
        gc_gx_thread_copy(t, m, payload, NULL, 0, 0, 0);
      } else {
        uint32_t first, end;
        uint64_t tag = 0;
        gc_efb_copy_bytes(&job, &first, &end);
        if (first > gc_ram_size) first = gc_ram_size;
        if (end > gc_ram_size) end = gc_ram_size;
        if (end < first) end = first;
        if (end > first) {
          tag = gc_memory_next_tag();
          gc_memory_hold_tagged(m->mem1, first, end - first, tag);
        }
        gc_gx_thread_copy(t, m, payload, &job, tag, first, end);
      }
      return 1;
    }
    case 0x65: {
      /* The load reads its entries from memory now, as gc_gx_tlut_write
       * would. */
      const uint32_t source = gc_ram_from_register((gx->bp[0x64] & 0x00ffffffu) << 5);
      const uint32_t offset = (payload & 0x3ffu) * GC_GX_TLUT_UNIT_ENTRIES;
      const uint32_t count = (payload >> 10) & 0x3ffu;
      uint32_t entries = count * 16u, i, n = 0;
      static uint16_t values[16384];
      if (!entries) entries = 16u;
      for (i = 0; i < entries && offset + i < GC_GX_TLUT_ENTRIES; i++) {
        values[n++] = memory_u16(m, source + i * 2u);
      }
      gc_gx_thread_palette(t, m, payload, values, n);
      return 1;
    }
    default: return 0;
  }
}

/* An indexed attribute of a vertex format: where its index is in a vertex,
 * how wide, which array, and how many bytes one index reads. */
typedef struct GxIndexed {
  unsigned offset, width, array, element;
} GxIndexed;

static unsigned indexed_attributes(const GxLayout *l, GxIndexed out[16]) {
  unsigned offset = 0, n = 0, i;
  offset += l->position_matrix ? 1u : 0u;
  for (i = 0; i < GC_GX_MAX_TEXCOORDS; i++) offset += (l->texture_matrix >> i) & 1u;
#define ATTRIBUTE(kind, direct, indices, array_number, element_bytes)                 \
  do {                                                                              \
    const unsigned k_ = (kind);                                                     \
    if (k_ == GC_GX_ATTR_DIRECT) {                                                  \
      offset += (direct);                                                           \
    } else if (k_ == GC_GX_ATTR_INDEX8 || k_ == GC_GX_ATTR_INDEX16) {               \
      const unsigned w_ = k_ == GC_GX_ATTR_INDEX8 ? 1u : 2u;                         \
      unsigned j_;                                                                  \
      for (j_ = 0; j_ < (indices); j_++) {                                          \
        out[n].offset = offset;                                                     \
        out[n].width = w_;                                                          \
        out[n].array = (array_number);                                              \
        out[n].element = (element_bytes);                                           \
        if (out[n].element) n++;                                                    \
        offset += w_;                                                               \
      }                                                                             \
    }                                                                               \
  } while (0)
  ATTRIBUTE(l->position_kind, l->position.elements * component_bytes(l->position.format), 1u, 0u,
            l->position.elements * component_bytes(l->position.format));
  {
    const unsigned three = l->normal_indices == 3u && l->normal_kind != GC_GX_ATTR_DIRECT;
    const unsigned sets = l->normal.elements / 3u;
    ATTRIBUTE(l->normal_kind, l->normal.elements * component_bytes(l->normal.format), l->normal_indices, 1u,
              (three ? 3u : sets * 3u) * component_bytes(l->normal.format));
  }
  for (i = 0; i < 2; i++) {
    ATTRIBUTE(l->color_kind[i], color_bytes(l->color[i].format), 1u, 2u + i, color_bytes(l->color[i].format));
  }
  for (i = 0; i < GC_GX_MAX_TEXCOORDS; i++) {
    ATTRIBUTE(l->texcoord_kind[i], l->texcoord[i].elements * component_bytes(l->texcoord[i].format), 1u, 4u + i,
              l->texcoord[i].elements * component_bytes(l->texcoord[i].format));
  }
#undef ATTRIBUTE
  return n;
}

/* What one index range of an array reads, as main memory offsets: the
 * decoder's addresses are 32-bit sums that can wrap, and a byte outside main
 * memory reads as zero, so the range is cut to main memory at both places a
 * wrapped sum could put it. */
static void reads_of_range(GcMachine *m, GcGxThread *t, uint32_t base, uint32_t stride, uint32_t lowest,
                           uint32_t highest, uint32_t element) {
  const uint64_t first = (uint64_t)base + (uint64_t)lowest * stride;
  const uint64_t end = (uint64_t)base + (uint64_t)highest * stride + element;
  unsigned wrap;
  for (wrap = 0; wrap < 2u; wrap++) {
    const uint64_t shift = (uint64_t)wrap << 32;
    uint64_t a = first > shift ? first - shift : 0u, b;
    if (end <= shift) break;
    b = end - shift;
    if (first > shift + 0xffffffffull) continue;
    if (a >= gc_ram_size) continue;
    if (b > gc_ram_size) b = gc_ram_size;
    if (b > a) gc_gx_thread_send(t, m, (uint32_t)a, (uint32_t)(b - a));
  }
}

/* Which maps the pipeline samples - each textured stage's, and the indirect
 * map a textured stage's indirect command names - as gc_gl.c works it out. */
static unsigned maps_sampled(const GcGx *gx) {
  const unsigned stages = ((gx->bp[0x00] >> 10) & 0xfu) + 1u;
  unsigned used = 0, stage;
  for (stage = 0; stage < stages && stage < 16u; stage++) {
    const uint32_t reg = gx->bp[0x28 + (stage >> 1)];
    const uint32_t order = (stage & 1u) ? (reg >> 12) : reg;
    const uint32_t command = gx->bp[0x10 + stage];
    if (!((order >> 6) & 1u)) continue;
    used |= 1u << (order & 7u);
    if (((command >> 9) & 3u) != 0u) used |= 1u << ((gx->bp[0x27] >> ((command & 3u) * 6u)) & 7u);
  }
  return used;
}

static void scan_draw(GcMachine *m, GcGxThread *t, GxStream *s, uint32_t start_ptr, unsigned command) {
  GcGx *gx = &m->gx;
  const unsigned vat = command & 7u;
  const unsigned kind = command & 0xf8u;
  const unsigned count = stream_u16(s);
  unsigned size;
  const GxIndexed *indexed;
  unsigned attributes, i;
  uint8_t *out;
  uint32_t total;
  /* The format, its vertex's size and its indexed attributes, worked out
   * again only when the descriptor or this format's attribute table changed:
   * a field has thousands of draws and a handful of formats. */
  static struct {
    int valid;
    uint32_t vcd_lo, vcd_hi, a, b, c;
    unsigned size;
    GxLayout layout;
    GxIndexed indexed[16];
    unsigned attributes;
  } formats[GC_GX_MAX_VAT];

  if (s->overrun) return;
  {
    const uint32_t a = gx->vat_a[vat], b = gx->vat_b[vat], c = gx->vat_c[vat];
    if (!formats[vat].valid || formats[vat].vcd_lo != gx->vcd_lo || formats[vat].vcd_hi != gx->vcd_hi ||
        formats[vat].a != a || formats[vat].b != b || formats[vat].c != c) {
      formats[vat].size = gc_gx_vertex_size(gx, vat);
      layout_of(gx, vat, &formats[vat].layout);
      formats[vat].attributes = indexed_attributes(&formats[vat].layout, formats[vat].indexed);
      formats[vat].vcd_lo = gx->vcd_lo;
      formats[vat].vcd_hi = gx->vcd_hi;
      formats[vat].a = a;
      formats[vat].b = b;
      formats[vat].c = c;
      formats[vat].valid = 1;
    }
    size = formats[vat].size;
  }
  if (!size && count) {
    s->overrun = 1;
    gx->unknown_commands++;
    if (m->log && gx->unknown_commands <= 16) {
      fprintf(m->log, "gx: a draw of %u vertices with no format, at %08x\n", count, s->ptr);
    }
    return;
  }
  if ((uint32_t)count * size > s->available) {
    s->overrun = 1;
    return;
  }
  total = (uint32_t)count * size;
  /* Counted here as well as in draw(): with a graphics thread the draw
   * happens on a machine of its own, whose counters nothing reports. */
  gx->kind_vertices[(kind - 0x80u) >> 3] += count;

  if (!gx->on_primitive) {
    unsigned triangles = 0;
    switch (kind) {
      case GC_GX_QUADS:
      case GC_GX_QUADS_2: triangles = (count / 4u) * 2u; break;
      case GC_GX_TRIANGLES: triangles = count / 3u; break;
      case GC_GX_TRIANGLE_STRIP:
      case GC_GX_TRIANGLE_FAN: triangles = count >= 3u ? count - 2u : 0u; break;
      default: break;
    }
    gx->triangles += triangles;
  }
  gx->vertices += count;
  gx->primitives++;

  /* The textures the pipeline samples, when the registers or the burst
   * changed since they were last worked out. */
  if (gc_gx_thread_textures_due(t)) {
    const unsigned maps = maps_sampled(gx);
    unsigned map;
    for (map = 0; map < 8u; map++) {
      GcGxTexture texture;
      uint32_t first, bytes;
      if (!((maps >> map) & 1u)) continue;
      gc_gx_texture_read(m, map, &texture);
      gc_gx_texture_span(&texture, &first, &bytes);
      if (bytes) gc_gx_thread_send_texture(t, m, first, bytes);
    }
  }

  attributes = count ? formats[vat].attributes : 0u;
  indexed = formats[vat].indexed;
  if (attributes) {
    /* The index ranges, from the draw's own bytes. */
    uint32_t lowest[16], highest[16];
    uint32_t v;
    const uint8_t *bytes;
    uint8_t scratch_small[1];
    uint8_t *copy = NULL;
    if (!s->ring && s->ptr < gc_ram_size && total <= gc_ram_size - s->ptr) {
      bytes = s->memory + s->ptr;
    } else {
      uint32_t at = s->ptr, k;
      copy = (uint8_t *)malloc(total ? total : 1u);
      if (!copy) {
        bytes = scratch_small;
        attributes = 0;
      } else {
        for (k = 0; k < total; k++) {
          copy[k] = at < gc_ram_size ? s->memory[at] : 0u;
          at++;
          if (s->ring && at >= s->limit) at = s->base;
        }
        bytes = copy;
      }
    }
    for (i = 0; i < attributes; i++) {
      /* One attribute down the vertices at a time. */
      const size_t offset = indexed[i].offset;
      uint32_t low = 0xffffffffu, high = 0;
      if (indexed[i].width == 1u) {
        for (v = 0; v < count; v++) {
          const uint32_t index = bytes[(size_t)v * size + offset];
          if (index < low) low = index;
          if (index > high) high = index;
        }
      } else {
        for (v = 0; v < count; v++) {
          const uint8_t *const p = bytes + (size_t)v * size + offset;
          const uint32_t index = (uint32_t)((p[0] << 8) | p[1]);
          if (index < low) low = index;
          if (index > high) high = index;
        }
      }
      lowest[i] = low;
      highest[i] = high;
    }
    for (i = 0; i < attributes; i++) {
      const unsigned array = indexed[i].array & 15u;
      reads_of_range(m, t, gx->array_base[array], gx->array_stride[array], lowest[i], highest[i],
                     indexed[i].element);
    }
    free(copy);
  }

  /* The command itself: header, count, vertices. */
  out = gc_gx_thread_commands(t, m, 3u + total);
  out[0] = (uint8_t)command;
  out[1] = (uint8_t)(count >> 8);
  out[2] = (uint8_t)count;
  if (!s->ring && s->ptr < gc_ram_size && total <= gc_ram_size - s->ptr) {
    memcpy(out + 3, s->memory + s->ptr, total);
  } else {
    uint32_t at = s->ptr, k;
    for (k = 0; k < total; k++) {
      out[3 + k] = at < gc_ram_size ? s->memory[at] : 0u;
      at++;
      if (s->ring && at >= s->limit) at = s->base;
    }
  }
  (void)start_ptr;
  /* Past the vertices. */
  s->ptr += total;
  if (s->ring) {
    while (s->ptr >= s->limit) s->ptr = s->base + (s->ptr - s->limit);
  }
  s->available -= total;
}

static void scan_display_list(GcMachine *m, GcGxThread *t, GxStream *s, unsigned depth) {
  GcGx *gx = &m->gx;
  const uint32_t address = gc_ram_from_register(stream_u32(s)) & ~31u;
  const uint32_t size = stream_u32(s) & 0x03ffffe0u;
  GxStream list;

  if (s->overrun) return;
  if (depth >= GX_MAX_LIST_DEPTH) {
    gx->truncated_lists++;
    return;
  }
  gx->display_lists++;
  gx->display_list_depth = depth + 1u;

  list.m = m;
  list.base = address;
  list.limit = address + size;
  list.ptr = address;
  list.available = size;
  list.ring = 0;
  list.overrun = 0;
  list.copy = NULL;
  list.memory = m->mem1;
  scan_stream(m, t, &list, depth + 1u);
  if (list.overrun) gx->truncated_lists++;
}

static void scan_stream(GcMachine *m, GcGxThread *t, GxStream *s, unsigned depth) {
  GcGx *gx = &m->gx;

  while (s->available && !s->overrun) {
    if (s->ring) s->memory = ring_view(m, s->base, s->limit);
    const uint32_t start_ptr = s->ptr;
    const uint32_t start_available = s->available;
    const unsigned command = stream_u8(s);
    int queued = 0;

    if (command == 0x00u) continue;

    switch (command) {
      case 0x08: {
        const unsigned reg = stream_u8(s);
        const uint32_t value = stream_u32(s);
        if (!s->overrun) scan_cp(m, reg, value);
        break;
      }
      case 0x10: {
        const uint32_t header = stream_u32(s);
        const unsigned count = ((header >> 16) & 0xfu) + 1u;
        unsigned i;
        if (!s->overrun && count * 4u > s->available) {
          s->overrun = 1;
          break;
        }
        for (i = 0; i < count && !s->overrun; i++) (void)stream_u32(s);
        break;
      }
      case 0x20:
      case 0x28:
      case 0x30:
      case 0x38: {
        const uint32_t header = stream_u32(s);
        const unsigned array = 12u + ((command - 0x20u) >> 3);
        const unsigned address = header & 0xfffu;
        const unsigned count = ((header >> 12) & 0xfu) + 1u;
        const uint32_t index = header >> 16;
        uint8_t *out;
        unsigned i;
        if (s->overrun) break;
        /* The matrix as memory holds it now, as a plain register write. */
        out = gc_gx_thread_commands(t, m, 5u + 4u * count);
        out[0] = 0x10;
        out[1] = 0;
        out[2] = (uint8_t)((count - 1u) & 0xfu);
        out[3] = (uint8_t)(address >> 8);
        out[4] = (uint8_t)address;
        for (i = 0; i < count; i++) {
          const uint32_t value = memory_u32(m, array_entry(gx, array, index) + 4u * i);
          out[5 + 4 * i] = (uint8_t)(value >> 24);
          out[6 + 4 * i] = (uint8_t)(value >> 16);
          out[7 + 4 * i] = (uint8_t)(value >> 8);
          out[8 + 4 * i] = (uint8_t)value;
        }
        queued = 1;
        break;
      }
      case 0x40:
        scan_display_list(m, t, s, depth);
        queued = 1;
        break;
      case 0x44: queued = 1; break;
      case 0x48: queued = 1; break;
      case 0x61: {
        const uint32_t value = stream_u32(s);
        if (!s->overrun) queued = scan_bp(m, t, value);
        break;
      }
      default:
        if (command >= 0x80u && command < 0xc0u) {
          scan_draw(m, t, s, start_ptr, command);
          queued = 1;
          break;
        }
        gx->unknown_commands++;
        if (m->log && gx->unknown_commands <= 16) {
          fprintf(m->log, "gx: command %02x at %08x is not one this knows\n", command, start_ptr);
        }
        s->available = 0;
        return;
    }

    if (s->overrun) {
      s->ptr = start_ptr;
      s->available = start_available;
      return;
    }
    if (!queued) emit_command(m, t, s, start_ptr, start_available - s->available);
    gx->commands++;
  }
}

void gc_gx_exec_commands(GcMachine *shadow, const uint8_t *bytes, size_t size) {
  GxStream s;
  s.m = shadow;
  s.base = 0;
  s.limit = (uint32_t)size;
  s.ptr = 0;
  s.available = (uint32_t)size;
  s.ring = 0;
  s.overrun = 0;
  s.copy = bytes;
  s.memory = shadow->mem1;
  run_stream(shadow, &s, 0);
}

/* ------------------------------------------------------------------ */
/* The ring                                                            */

void gc_gx_reset(GcGx *gx) {
  void (*on_primitive)(GcMachine *, unsigned, const GcGxVertex *, unsigned) = gx->on_primitive;
  void (*on_triangle)(GcMachine *, const struct GcGxPoint *) = gx->on_triangle;
  memset(gx, 0, sizeof(*gx));
  gx->on_primitive = on_primitive;
  gx->on_triangle = on_triangle;
  gc_gx_tev_reset(&gx->tev);
}

/* The ring wraps at the 32-byte block after the last word the register
 * holds, which is where the hardware's own pointer comparison puts it. */
static uint32_t ring_limit(const GcGx *gx) { return (gx->fifo_end & ~31u) + 32u; }

void gc_gx_fifo_write(GcMachine *m, const uint8_t *bytes, unsigned size) {
  GcGx *gx = &m->gx;
  const uint32_t limit = ring_limit(gx);
  unsigned i;

  /* Not linked: the processor is writing to a FIFO of its own - a display
   * list being recorded - and none of this is the graphics processor's to
   * run. Running it anyway executes every recorded command once on the spot,
   * against whatever vertex format happens to be set, and then again when
   * the list is called. */
  if (gx->linked & GC_GX_UNLINKED) {
    gc_pi_fifo_store(m, bytes, size);
    return;
  }

  if (gx->fifo_base >= limit) {
    /* Nowhere to put it. A game that gets here has not set the FIFO up,
     * and the bytes are counted rather than written over memory. */
    gx->bytes += size;
    return;
  }
  if (gx->write_ptr < gx->fifo_base || gx->write_ptr >= limit) gx->write_ptr = gx->fifo_base;

  if (limit <= gc_ram_size) {
    uint8_t *const memory = ring_view(m, gx->fifo_base, limit);
    for (i = 0; i < size; i++) {
      memory[gx->write_ptr] = bytes[i];
      gx->write_ptr++;
      if (gx->write_ptr >= limit) gx->write_ptr = gx->fifo_base;
    }
  } else {
    for (i = 0; i < size; i++) {
      uint8_t *p = gc_ram_ptr(m, gx->write_ptr, 1); /* the pointer is an offset */
      if (p) *p = bytes[i];
      gx->write_ptr++;
      if (gx->write_ptr >= limit) gx->write_ptr = gx->fifo_base;
    }
  }
  gx->bytes += size;
  gc_gx_run(m);
}

void gc_gx_run(GcMachine *m) {
  GcGx *gx = &m->gx;
  const uint32_t limit = ring_limit(gx);
  GxStream s;
  uint32_t span;

  if (!(gx->linked & GC_GX_READS) || gx->fifo_base >= limit) return;
  if (gx->read_ptr < gx->fifo_base || gx->read_ptr >= limit) gx->read_ptr = gx->fifo_base;

  span = limit - gx->fifo_base;
  s.m = m;
  s.base = gx->fifo_base;
  s.limit = limit;
  s.ptr = gx->read_ptr;
  s.available = (gx->write_ptr + span - gx->read_ptr) % span;
  s.ring = 1;
  s.overrun = 0;
  s.copy = NULL;
  s.memory = m->mem1;
  if (!s.available) return;

  {
    GcGxThread *const t = gc_gx_thread_of(m);
    if (t) {
      /* The graphics thread draws; this thread walks the stream. */
      const uint64_t started = gc_perf_ticks();
      gc_gx_thread_begin_run(t, m);
      scan_stream(m, t, &s, 0);
      gc_gx_thread_end_run(t);
      gc_perf.gx_ms += (double)(gc_perf_ticks() - started) * gc_perf_ms_per_tick;
      gx->read_ptr = s.ptr;
      return;
    }
  }

  /* Nothing the processor can see is left held when this returns: the
   * textures a held triangle samples are in main memory, which only the
   * processor changes. */
  {
    const double started = gc_perf_now_ms();
    gc_gx_raster_begin(m);
    run_stream(m, &s, 0);
    gc_gx_raster_end(m);
    gc_perf.gx_ms += gc_perf_now_ms() - started;
  }
  gx->read_ptr = s.ptr;
}
