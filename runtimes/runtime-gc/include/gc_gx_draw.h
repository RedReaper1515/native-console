/*
 * The transform unit: a vertex as the stream held it, turned into a point
 * on the screen.
 *
 * Four steps, in the order the hardware does them:
 *
 *   1. the position matrix, three rows of four, out of the transform
 *      unit's own memory at the index the vertex or the register file gives
 *   2. the projection, which is six numbers and a flag rather than a 4x4,
 *      because the hardware only keeps the entries that can be non-zero
 *   3. the perspective divide
 *   4. the viewport, which carries a fixed offset of 342 in both axes -
 *      the hardware's own origin, subtracted again on the way out
 *
 * Colours come from the channel control registers: either the vertex's own
 * or a register, and either lit or not. Lighting is not done here yet; an
 * unlit channel is the common case and the one every gxtest program uses.
 *
 * The output is in screen coordinates with a 24-bit depth, which is what
 * the rasteriser wants.
 */
#ifndef GC_GX_DRAW_H
#define GC_GX_DRAW_H

#include "gc_gx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A vertex after the transform unit. `clip` is kept because clipping
 * happens before the divide and needs it. */
typedef struct GcGxPoint {
  float clip[4];    /* x, y, z, w, as the transform unit computed them */
  /* Pixels, and 0 to 2^24-1 of depth. These are doubles because single
   * precision is not enough: the hardware test sweeps a viewport four
   * hundred-thousandths of a pixel wide, and near 342 a float's steps are
   * three hundred-thousandths - so the sliver would land on the wrong side
   * of the sample point purely from rounding the viewport transform.
   * Doing that one multiply-add wider reproduces every measured point. */
  double screen[3];
  float texcoord[GC_GX_MAX_TEXCOORDS][3];
  uint8_t color[2][4];
} GcGxPoint;

/* The viewport, as the six registers hold it. */
typedef struct GcGxViewport {
  float scale[3];
  float offset[3];
} GcGxViewport;

void gc_gx_viewport(const GcGx *gx, GcGxViewport *out);

/* One vertex through the whole transform. */
void gc_gx_transform(const GcGx *gx, const GcGxVertex *in, GcGxPoint *out);

/* The matrix index a vertex uses: its own if the descriptor gave it one,
 * and the register file's otherwise. */
unsigned gc_gx_position_matrix(const GcGx *gx, const GcGxVertex *v);
unsigned gc_gx_texture_matrix(const GcGx *gx, const GcGxVertex *v, unsigned coordinate);

/* One triangle into the framebuffer. */
void gc_gx_raster_triangle(GcMachine *m, const GcGxPoint points[3]);

/* One triangle that has already been through the eye-plane check, the
 * trivial rejection and the clipper: the reference rasteriser from set-up
 * on (or the OpenGL path, when it is active). The OpenGL path also calls it
 * with the path switched off, for a pipeline whose pixels it cannot draw. */
void gc_gx_raster_one(GcMachine *m, const GcGxPoint points[3]);

/* A clipped triangle set up exactly as gc_gx_raster_one sets it up: the
 * points less the scissor offset in doubles, the reciprocal of each w, the
 * doubled signed area, and the bounding box it visits inside the scissor
 * box (inclusive). What the rasteriser hands the OpenGL path for triangles
 * it held back (gc_gl_pieces). */
typedef struct GcGxRasterPiece {
  double x[3], y[3], z[3], inverse_w[3], area;
  int left, right, top, bottom;
  const GcGxPoint *points;
} GcGxRasterPiece;

/* Between begin and end, a draw whose triangles the rasteriser can draw on
 * several threads may be held back, as the bytes it arrived as, and decoded,
 * transformed and drawn later together with others (gc_gx_raster_hold_draw
 * below). The command stream opens this around what it runs, draws what is
 * held (flush) before any register write, and end draws the rest. Outside,
 * every triangle is drawn when it arrives. */
void gc_gx_raster_begin(GcMachine *m);
void gc_gx_raster_flush(GcMachine *m);
void gc_gx_raster_end(GcMachine *m);
/* Each thread that draws keeps what it holds back, and its workers, to
 * itself; a thread that stops drawing (a graphics thread, gc_gx_thread.h)
 * lets them go. Nothing may be held. */
void gc_gx_raster_forget_thread(void);

/* A vertex format as the descriptor and attribute table give it, worked out
 * once for a draw (gc_gx.c). */
typedef struct GcGxFormat {
  unsigned elements;
  unsigned format;
  unsigned fraction;
} GcGxFormat;

typedef struct GcGxLayout {
  unsigned vat;
  unsigned size; /* bytes a vertex */
  int byte_dequant;
  unsigned position_matrix;
  unsigned texture_matrix; /* bit i: coordinate i carries a matrix index */
  unsigned position_kind, normal_kind, color_kind[2], texcoord_kind[GC_GX_MAX_TEXCOORDS];
  GcGxFormat position, normal, color[2], texcoord[GC_GX_MAX_TEXCOORDS];
  unsigned normal_indices;
} GcGxLayout;

/* One vertex of a draw, decoded from a copy of its bytes - exactly what the
 * stream would have given, so long as the vertex-format registers and the
 * arrays an indexed attribute reads are as they were when the draw arrived. */
void gc_gx_decode_vertex_bytes(GcMachine *m, const GcGxLayout *layout, const uint8_t *bytes,
                               GcGxVertex *out);

/* A whole draw held back (see gc_gx_raster.c): room for count * size bytes,
 * which the caller fills with the draw's vertices, or NULL when the draw is to
 * be drawn now. `triangles` is how many it makes. */
uint8_t *gc_gx_raster_hold_draw(GcMachine *m, unsigned kind, const GcGxLayout *layout, unsigned count,
                                unsigned triangles);

/* Primitive assembly: vertices arrive one at a time and triangles come out.
 * Quads become two triangles, strips alternate their winding, and fans keep
 * their first vertex. */
void gc_gx_assemble_begin(GcGx *gx, unsigned kind);
void gc_gx_assemble_vertex(GcMachine *m, const GcGxVertex *v);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_DRAW_H */
