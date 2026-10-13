/*
 * Flipper's front end: the command FIFO, the register files behind it, and
 * the vertices that come out.
 *
 * A GameCube game does not call a graphics API that writes registers. It
 * writes a *byte stream* through the write-gather pipe into a ring buffer
 * in main memory, and the graphics processor reads that ring and does what
 * it says. Everything - a register write, a matrix, a display list, a
 * triangle - is a command in that one stream, and the stream only stays
 * parseable if the vertex format is understood exactly: a draw command says
 * how many vertices follow but not how big they are. That comes from the
 * vertex descriptor and the attribute tables, and getting it wrong does not
 * produce a wrong triangle, it loses the parser's place in the stream for
 * good.
 *
 * So this is the piece to get right first, and the piece with an obvious
 * test: run a program that draws, and count the commands nobody could
 * parse. It has to be zero.
 *
 * Transform, rasterisation and the pixel engine are the rest of P3
 * (docs/GC-PLAN.md); this hands them decoded vertices and a register file.
 */
#ifndef GC_GX_H
#define GC_GX_H

#include <stdint.h>

#include "gc_gx_tev.h"
#include "gc_gx_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcMachine GcMachine;
struct GcGxPoint;

/* The attributes a vertex can carry. Positions and normals are geometry;
 * the two colours and eight texture coordinates are what the pixel stages
 * interpolate. */
enum {
  GC_GX_MAX_TEXCOORDS = 8,
  GC_GX_MAX_VAT = 8,
  GC_GX_MAX_ARRAYS = 16,
  GC_GX_BP_REGS = 256,
  GC_GX_XF_REGS = 0x60,   /* 0x1000 upwards: viewport, projection, lighting */
  GC_GX_XF_MEMORY = 0x800, /* matrices, normals and light data, as floats */
  /* A triangle clipped against two depth planes cannot gain more than one
   * vertex per plane, but the buffer is generous: clipping is where an
   * off-by-one writes past the end. */
  GC_GX_MAX_CLIPPED = 8
};

/* How an attribute reaches the stream. */
enum {
  GC_GX_ATTR_NONE = 0,
  GC_GX_ATTR_DIRECT = 1,
  GC_GX_ATTR_INDEX8 = 2,
  GC_GX_ATTR_INDEX16 = 3
};

/* One vertex, after the stream has been decoded but before anything has
 * been done to it. Positions and texture coordinates are already scaled by
 * the fraction their format carries; colours are already 8 bits a channel. */
typedef struct GcGxVertex {
  float position[3];
  float normal[3][3]; /* normal, binormal, tangent */
  uint8_t color[2][4];
  float texcoord[GC_GX_MAX_TEXCOORDS][2];

  uint8_t position_matrix;                    /* index into the matrix memory */
  uint8_t texture_matrix[GC_GX_MAX_TEXCOORDS];
  uint8_t has_normal;
  uint8_t has_color[2];
  uint8_t texcoord_count;
} GcGxVertex;

/* The primitive a draw command names. The values are the command's own
 * top five bits, so a stream byte maps straight onto one. */
enum {
  GC_GX_QUADS = 0x80,
  GC_GX_QUADS_2 = 0x88, /* undocumented, and used: same as quads */
  GC_GX_TRIANGLES = 0x90,
  GC_GX_TRIANGLE_STRIP = 0x98,
  GC_GX_TRIANGLE_FAN = 0xa0,
  GC_GX_LINES = 0xa8,
  GC_GX_LINE_STRIP = 0xb0,
  GC_GX_POINTS = 0xb8
};

/* GcGx::linked. */
#define GC_GX_READS 1    /* the graphics processor takes commands from its FIFO */
#define GC_GX_UNLINKED 2 /* what the processor writes is not put in that FIFO */

typedef struct GcGx {
  /* --- the command processor's view of the ring --- */
  uint32_t fifo_base;
  uint32_t fifo_end;  /* the last word, as the hardware registers hold it */
  uint32_t read_ptr;
  uint32_t write_ptr;
  /* GC_GX_READS and GC_GX_UNLINKED. The second is the negative so that the
   * values this held when it was one flag (0, and 1 for "linked and
   * reading") still mean what they meant, in save states and in tests. */
  int linked;

  /* --- the register files --- */
  uint32_t bp[GC_GX_BP_REGS];
  uint32_t xf[GC_GX_XF_REGS];
  float xf_memory[GC_GX_XF_MEMORY];

  /* --- the vertex format, which lives in CP registers --- */
  uint32_t perf_metric;    /* CP 0x20: which counter the hardware keeps, and nothing reads */
  uint32_t matrix_index_a; /* CP 0x30: the position and first five texture matrices */
  uint32_t matrix_index_b; /* CP 0x40: the other three */
  uint32_t vcd_lo;         /* CP 0x50 */
  uint32_t vcd_hi;         /* CP 0x60 */
  uint32_t vat_a[GC_GX_MAX_VAT]; /* CP 0x70 + n */
  uint32_t vat_b[GC_GX_MAX_VAT]; /* CP 0x80 + n */
  uint32_t vat_c[GC_GX_MAX_VAT]; /* CP 0x90 + n */
  uint32_t array_base[GC_GX_MAX_ARRAYS];   /* CP 0xa0 + n */
  uint32_t array_stride[GC_GX_MAX_ARRAYS]; /* CP 0xb0 + n */

  /* Dynamic indirect matrices, whose coefficients no source states.
   * Counted rather than guessed at, so a game leaning on one is
   * visible instead of quietly wrong. */
  uint32_t unsupported_indirect;

  /* --- the texture environment, and the palettes it samples through --- */
  GcGxTev tev;
  GcGxTlut tlut;
  uint32_t tlut_source;

  /* --- assembling triangles out of the vertices as they arrive --- */
  GcGxVertex assembly[4];
  unsigned assembly_kind;
  unsigned assembly_count;
  unsigned assembly_parity; /* a strip alternates its winding */

  /* --- what has happened, for the runner and the tests to report --- */
  uint64_t commands;
  uint64_t primitives;
  uint64_t vertices;
  uint64_t triangles;
  uint64_t display_lists;
  uint64_t bytes;
  unsigned unknown_commands;
  unsigned truncated_lists;
  unsigned display_list_depth;
  unsigned unsupported_primitives;
  /* Vertices by primitive kind, indexed by (command - 0x80) >> 3. Points
   * and lines are counted and not drawn, and a count is what lets a run
   * say so rather than lose them in silence. */
  uint64_t kind_vertices[8];
  unsigned unclipped_triangles; /* crossing the eye plane, and so clipped whatever 0x1005 says */
  uint64_t pixels;

  /* Where the vertices go, before assembly and after it. The rasteriser
   * takes the second; the first is how the tests watch the decoder. */
  void (*on_primitive)(GcMachine *m, unsigned kind, const GcGxVertex *vertices, unsigned count);
  void (*on_triangle)(GcMachine *m, const struct GcGxPoint *points);
} GcGx;

void gc_gx_reset(GcGx *gx);

/* 32 bytes out of the write-gather pipe, aimed at the FIFO port. They go
 * into the ring at the CPU's write pointer, and then the graphics processor
 * reads as far as it can. */
void gc_gx_fifo_write(GcMachine *m, const uint8_t *bytes, unsigned size);
/* The processor's own FIFO registers (gc_io.c): a burst that is not for the
 * graphics processor. */
void gc_pi_fifo_store(GcMachine *m, const uint8_t *bytes, unsigned size);
uint64_t gc_pi_fifo_recorded(void); /* bytes stored that way, by any machine in the process */

/* Parse as much of the ring as is complete. Called after every write; safe
 * to call at any time. */
void gc_gx_run(GcMachine *m);

/* How many bytes one vertex of this format occupies in the stream. Exposed
 * because it is the thing most worth testing on its own: every other part
 * of the parser depends on it. */
unsigned gc_gx_vertex_size(const GcGx *gx, unsigned vat);

/* The command stream's drawing on a thread of its own, with the machine
 * observably unchanged (docs/GC-PLAN.md, "The graphics thread"). Start
 * returns 1 when the thread runs and 0 when the machine stays on the single
 * thread: GC_GX_THREAD=0 in the environment, a host without main memory's
 * second view (only Windows has one so far), or a machine whose tests watch
 * vertices. gc_machine_destroy stops it. */
int gc_gx_thread_start(GcMachine *m);
void gc_gx_thread_stop(GcMachine *m);
int gc_gx_thread_active(const GcMachine *m);
/* Everything the thread was given, drawn, and what it computed (the
 * transform and combiner registers, the palettes, the pixel count) back in
 * the machine: before reading those from outside. */
void gc_gx_thread_sync(GcMachine *m);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_H */
