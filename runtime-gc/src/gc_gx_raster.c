/*
 * The rasteriser: a triangle in screen coordinates, turned into pixels in
 * the embedded framebuffer.
 *
 * Two things here are not the obvious choice, and both come from
 * dolphin-emu/hwtests' rasterization program, which measured them on a
 * console (docs/gc-gxtest-survey-raster.md).
 *
 * The first is the **sample point**. A pixel is not sampled at its centre:
 * it is sampled just above seven twelfths of the way across. The hardware
 * test pins it to a window about one hundred-thousandth of a pixel wide by
 * sweeping a viewport four hundred-thousandths of a pixel wide across it
 * one float at a time, so this is not a detail that can be approximated -
 * a rasteriser that snaps coverage to a sub-pixel grid cannot produce the
 * answer at all.
 *
 * The second is that **screen coordinates carry an offset of 342** in both
 * axes. The viewport registers have it added in, the scissor box has it
 * added in, and the scissor *offset* register is what takes it out again.
 * So the transform leaves it in and this subtracts twice the scissor
 * offset, which is what the hardware does and what lets a game render in
 * strips by moving that register.
 *
 * Everything about the pixel itself - the texture environment, blending,
 * dithering - is the next part of P3. What is written here is the colour
 * the transform produced, which is what a pass-through configuration
 * gives; a stage doing anything else is counted and reported rather than
 * quietly drawn wrong.
 */

#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_efb.h"
#include "gc_gl.h"
#include "gc_gx_blend.h"
#include "gc_gx_fog.h"
#include "gc_gx_draw.h"
#include "gc_gx_tev.h"
#include "gc_gx_tev_plan.h"
#include "gc_perf.h"
#include "gc_gx_texture.h"
#include "gc_gx_workers.h"
#include "gc_memory.h"

#define BP_GEN_MODE 0x00
#define BP_SCISSOR_TOP_LEFT 0x20
#define BP_SCISSOR_BOTTOM_RIGHT 0x21
#define BP_Z_MODE 0x40
/* The depth texture: a bias, and what to do with it. */
#define BP_Z_TEXTURE_BIAS 0xf4
#define BP_Z_TEXTURE 0xf5
#define BP_SCISSOR_OFFSET 0x59

/* Where in a pixel the hardware takes its sample. Seven twelfths, which
 * the measurement brackets to (0.5833330, 0.5833435). */
#define SAMPLE_POINT (7.0 / 12.0)

/* The scissor box and the coordinate bias share an origin of 342. */
#define SCREEN_ORIGIN 342

typedef struct GcScissor {
  int left, top, right, bottom; /* in framebuffer pixels, inclusive */
} GcScissor;

/* The scissor box, and the offset that turns a screen coordinate into a
 * framebuffer one. Both are in the biased space; this returns the box
 * already unbiased, and the shift separately. */
static void scissor_box(const GcMachine *m, GcScissor *box, int *shift_x, int *shift_y) {
  const uint32_t top_left = m->gx.bp[BP_SCISSOR_TOP_LEFT];
  const uint32_t bottom_right = m->gx.bp[BP_SCISSOR_BOTTOM_RIGHT];
  const uint32_t offset = m->gx.bp[BP_SCISSOR_OFFSET];

  *shift_x = (int)(offset & 0x3ffu) * 2;
  *shift_y = (int)((offset >> 10) & 0x3ffu) * 2;

  box->top = (int)(top_left & 0xfffu) - *shift_y;
  box->left = (int)((top_left >> 12) & 0xfffu) - *shift_x;
  box->bottom = (int)(bottom_right & 0xfffu) - *shift_y;
  box->right = (int)((bottom_right >> 12) & 0xfffu) - *shift_x;

  if (box->left < 0) box->left = 0;
  if (box->top < 0) box->top = 0;
  if (box->right > (int)GC_EFB_WIDTH - 1) box->right = (int)GC_EFB_WIDTH - 1;
  if (box->bottom > (int)GC_EFB_HEIGHT - 1) box->bottom = (int)GC_EFB_HEIGHT - 1;
}

/* The signed area of the triangle, twice over. Its sign is the winding,
 * which is what the cull mode is about. */
static double doubled_area(const double x[3], const double y[3]) {
  return (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
}

/* Hardware cull values: 0 draws everything, 1 drops back faces, 2 drops
 * front faces, 3 drops the lot. libogc's own table maps its API's FRONT and
 * BACK onto 2 and 1 in that order. */
static int culled(const GcMachine *m, double area) {
  const unsigned mode = (m->gx.bp[BP_GEN_MODE] >> 14) & 3u;
  switch (mode) {
    case 1: return area < 0.0;
    case 2: return area > 0.0;
    case 3: return 1;
    default: return 0;
  }
}

/* The depth test, as the Z mode register describes it. */
static int depth_passes(unsigned function, uint32_t incoming, uint32_t stored) {
  switch (function & 7u) {
    case 0: return 0;                   /* never */
    case 1: return incoming < stored;   /* less */
    case 2: return incoming == stored;  /* equal */
    case 3: return incoming <= stored;  /* less or equal */
    case 4: return incoming > stored;   /* greater */
    case 5: return incoming != stored;  /* not equal */
    case 6: return incoming >= stored;  /* greater or equal */
    default: return 1;                  /* always */
  }
}

static double clamp_to(double value, double low, double high) {
  return value < low ? low : (value > high ? high : value);
}

/* ------------------------------------------------------------------ */
/* Clipping                                                            */

/* The clipper does not use strict single-precision arithmetic. The
 * hardware test settles this: at a clip depth of -1.0000001 against a w of
 * 1 the exact sum is -1.19e-7, which is negative, and the hardware draws
 * the primitive anyway. One bit is the smallest reduction consistent with
 * that measurement - the value is exactly one unit in the last place below
 * -1 - so that is what is assumed rather than a wider guess.
 *
 * **Unverified:** how many bits the hardware really carries. What is
 * measured is only that one unit in the last place vanishes. */
static float reduced(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  bits &= ~1u;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/* How far a vertex is inside a plane. Positive or zero is inside: the far
 * plane is inclusive, and every quad the hardware tests draw sits exactly
 * on it. */
static float plane_distance(const GcGxPoint *p, int plane) {
  const float z = reduced(p->clip[2]);
  const float w = reduced(p->clip[3]);
  return plane ? (z + w) : -z; /* z >= -w, and z <= 0 */
}

/* Is every vertex outside the same plane? Then there is nothing to clip -
 * the primitive is gone, whatever the clip-disable bit says. */
static int trivially_rejected(const GcGxPoint *points, unsigned count) {
  unsigned i, plane;
  for (i = 0; i < count; i++) {
    if (points[i].clip[3] <= 0.0f) continue;
    break;
  }
  if (i == count) return 1; /* all behind the eye */
  for (plane = 0; plane < 2; plane++) {
    unsigned outside = 0;
    for (i = 0; i < count; i++) {
      if (plane_distance(&points[i], (int)plane) < 0.0f) outside++;
    }
    if (outside == count) return 1;
  }
  return 0;
}

/* One vertex a fraction of the way along an edge. Clip space is where the
 * interpolation is linear, so everything is carried across from there and
 * the screen position is recomputed rather than interpolated. */
static void between(const GcGx *gx, const GcGxPoint *a, const GcGxPoint *b, float t,
                    GcGxPoint *out) {
  GcGxViewport viewport;
  unsigned i, c;

  for (i = 0; i < 4; i++) out->clip[i] = a->clip[i] + t * (b->clip[i] - a->clip[i]);
  for (i = 0; i < 2; i++) {
    for (c = 0; c < 4; c++) {
      out->color[i][c] =
          (uint8_t)((float)a->color[i][c] + t * ((float)b->color[i][c] - (float)a->color[i][c]));
    }
  }
  for (i = 0; i < GC_GX_MAX_TEXCOORDS; i++) {
    for (c = 0; c < 3; c++) {
      out->texcoord[i][c] = a->texcoord[i][c] + t * (b->texcoord[i][c] - a->texcoord[i][c]);
    }
  }

  gc_gx_viewport(gx, &viewport);
  if (out->clip[3] != 0.0f) {
    const double inverse = 1.0 / (double)out->clip[3];
    for (i = 0; i < 3; i++) {
      out->screen[i] =
          (double)out->clip[i] * inverse * (double)viewport.scale[i] + (double)viewport.offset[i];
    }
  } else {
    /* A vertex exactly on the eye is a point at infinity and has no screen
     * position. Clipping against both planes leaves one only when z is zero
     * as well, which is a degenerate piece; zero is what it gets, so that
     * what comes out of the clipper never depends on uninitialised memory. */
    for (i = 0; i < 3; i++) out->screen[i] = 0.0;
  }
}

/* A polygon against one plane, keeping the inside. */
static unsigned clip_against(const GcGx *gx, const GcGxPoint *in, unsigned count,
                             GcGxPoint *out, int plane) {
  unsigned produced = 0;
  unsigned i;
  for (i = 0; i < count && produced + 2 <= GC_GX_MAX_CLIPPED; i++) {
    const GcGxPoint *current = &in[i];
    const GcGxPoint *next = &in[(i + 1u) % count];
    const float here = plane_distance(current, plane);
    const float there = plane_distance(next, plane);

    if (here >= 0.0f) out[produced++] = *current;
    if ((here >= 0.0f) != (there >= 0.0f)) {
      const float span = here - there;
      if (span != 0.0f) between(gx, current, next, here / span, &out[produced++]);
    }
  }
  return produced;
}

void gc_gx_raster_one(GcMachine *m, const GcGxPoint points[3]);

/* The most triangles clipping can leave of one: a fan over the polygon. */
#define GC_RASTER_MAX_PIECES (GC_GX_MAX_CLIPPED - 2u)

/* A triangle through trivial rejection and the clipper: the triangles left
 * to rasterise, in the order they are drawn (none, the triangle itself, or
 * a fan). Reads only the transform registers.
 *
 * A triangle with a vertex at or behind the eye - w <= 0 - used to be
 * dropped whole and counted in *unclipped. That is what took the swamp's
 * water away: a large surface that reaches past the camera has one corner
 * behind it, and dropping the whole triangle left the clear colour showing
 * through, flat and untextured, where the water should be.
 *
 * Clipping in homogeneous space needs no separate eye plane. The two planes
 * the hardware has - z <= 0 and z >= -w - together give w >= -z >= 0, so a
 * vertex behind the eye is outside one of them and the clipper removes it
 * along with the part of the triangle that goes with it. What is left is in
 * front of the eye and divides by a positive w.
 *
 * The clip-disable bit (0x1005 bit 0) cannot be honoured for such a
 * triangle: without the clip there is no positive w to divide by. It is
 * clipped anyway, and *unclipped counts that rather than a drop. */
static unsigned clip_triangle(const GcGx *gx, const GcGxPoint points[3],
                              GcGxPoint pieces[GC_RASTER_MAX_PIECES][3], int *unclipped) {
  /* Transform register 0x1005 bit 0 turns clipping off. Trivial rejection
   * is bit 1 and nothing turns it off here, so it always applies. */
  const int clipping_on = (gx->xf[0x05] & 1u) == 0;
  GcGxPoint polygon[GC_GX_MAX_CLIPPED], scratch[GC_GX_MAX_CLIPPED];
  unsigned count = 3, i, made = 0;
  const int crosses_eye =
      points[0].clip[3] <= 0.0f || points[1].clip[3] <= 0.0f || points[2].clip[3] <= 0.0f;

  *unclipped = crosses_eye;
  if (trivially_rejected(points, 3)) return 0;

  /* Nothing crossing a plane means nothing to do - but a triangle that
   * reaches the eye is clipped whatever the registers say, because the
   * rasteriser has no screen position for a vertex it cannot divide. */
  if (!crosses_eye &&
      (!clipping_on ||
       (plane_distance(&points[0], 0) >= 0.0f && plane_distance(&points[1], 0) >= 0.0f &&
        plane_distance(&points[2], 0) >= 0.0f && plane_distance(&points[0], 1) >= 0.0f &&
        plane_distance(&points[1], 1) >= 0.0f && plane_distance(&points[2], 1) >= 0.0f))) {
    for (i = 0; i < 3; i++) pieces[0][i] = points[i];
    return 1;
  }

  for (i = 0; i < 3; i++) polygon[i] = points[i];
  count = clip_against(gx, polygon, count, scratch, 0);
  count = clip_against(gx, scratch, count, polygon, 1);
  if (count < 3) return 0;

  /* A fan over what is left. A clip that only touched vertices already on
   * the plane produces a fan of zero-area triangles, which draw nothing -
   * which is what the hardware does, and what the test expects. */
  for (i = 1; i + 1 < count && made < GC_RASTER_MAX_PIECES; i++) {
    pieces[made][0] = polygon[0];
    pieces[made][1] = polygon[i];
    pieces[made][2] = polygon[i + 1u];
    made++;
  }
  return made;
}

void gc_gx_raster_triangle(GcMachine *m, const GcGxPoint points[3]) {
  GcGxPoint pieces[GC_RASTER_MAX_PIECES][3];
  int unclipped;
  const unsigned count = clip_triangle(&m->gx, points, pieces, &unclipped);
  unsigned i;
  if (unclipped) m->gx.unclipped_triangles++;
  for (i = 0; i < count; i++) gc_gx_raster_one(m, pieces[i]);
}

/* ------------------------------------------------------------------ */
/* The weights at a sample point, remembered for a row                 */

/* Everything the pixel loop works out at one sample point: the edge
 * functions, the screen-space weights, the perspective-correct ones, and the
 * texture coordinates they interpolate.
 *
 * A pixel needs its own point and the points one to the right and one below
 * - the second two for the gradient that picks a mipmap level. Those two are
 * exactly the points the pixel to the right and the pixel below will need
 * as their own, so a row remembers them and each point is worked out once
 * rather than up to three times. The arithmetic at a point is unchanged,
 * expression for expression; only how often it runs is.
 *
 * That the remembered point *is* the neighbour's point needs one fact about
 * doubles: the neighbour is asked for at `(px + 7/12) + 1.0` and visited at
 * `(px + 1) + 7/12`. Those are the same double for every px from 0 to 4095
 * - checked exhaustively - though not for every negative one; px and py are
 * never negative here, because the scissor box is clamped to the
 * framebuffer, and never reach 4096. */
typedef struct GcRasterPoint {
  uint32_t stamp; /* equal to the row's stamp when this point is filled in */
  double w[3];
  double a[3];
  double c[3];
  float coordinate[GC_GX_MAX_TEXCOORDS][2];
} GcRasterPoint;

typedef struct GcRasterRow {
  uint32_t stamp;
  GcRasterPoint point[GC_EFB_WIDTH + 1u];
} GcRasterRow;

/* Everything one thread needs to draw rows of a triangle: its own two
 * remembered rows and the stamps that say what in them is current (a
 * thread's rows mean nothing to another), the combiner registers it
 * computes in, and what it counted. */
typedef struct GcRasterWorker {
  uint32_t stamp;
  GcRasterRow rows[2];
  GcGxTev tev;
  uint64_t pixels;
  long tev_chunk; /* the last chunk in which this thread ran the combiner, or -1 */
  int reg[4][4];  /* the combiner's registers as that chunk left them */
} GcRasterWorker;

static uint32_t next_stamp(GcRasterWorker *worker) {
  if (worker->stamp >= 0xfffffff0u) {
    /* Before the counter could come round to a stamp a stale point still
     * carries, forget every point. */
    unsigned r, i;
    for (r = 0; r < 2u; r++) {
      worker->rows[r].stamp = 0;
      for (i = 0; i <= GC_EFB_WIDTH; i++) worker->rows[r].point[i].stamp = 0;
    }
    worker->stamp = 0;
  }
  return ++worker->stamp;
}

typedef struct GcRasterTriangle {
  double x[3], y[3], z[3], inverse_w[3], area;
  const GcGxPoint *points;
  unsigned coordinates_used;
  unsigned gradients_used; /* the plan's: sets whose neighbours are needed */
} GcRasterTriangle;

/* The edge functions alone, which is all a pixel outside the triangle
 * costs. */
static void edges_at(const GcRasterTriangle *tri, double sample_x, double sample_y, double w[3]) {
  const double *x = tri->x, *y = tri->y;
  w[0] = (x[2] - x[1]) * (sample_y - y[1]) - (y[2] - y[1]) * (sample_x - x[1]);
  w[1] = (x[0] - x[2]) * (sample_y - y[2]) - (y[0] - y[2]) * (sample_x - x[2]);
  w[2] = (x[1] - x[0]) * (sample_y - y[0]) - (y[1] - y[0]) * (sample_x - x[0]);
}

/* The rest of a point, from its edge functions. */
static void finish_point(const GcRasterTriangle *tri, GcRasterPoint *p) {
  const double *inverse_w = tri->inverse_w;
  const double area = tri->area;
  const double a0 = p->w[0] / area, a1 = p->w[1] / area, a2 = p->w[2] / area;
  unsigned used = tri->coordinates_used;
  double c0, c1, c2;

  p->a[0] = a0;
  p->a[1] = a1;
  p->a[2] = a2;

  /* Perspective correction. A barycentric weight is linear in *screen*
   * space, and an attribute is linear in clip space, so interpolating one
   * with the other is only right when every vertex shares a w - which is to
   * say only for an orthographic projection. Dividing each attribute by its
   * own w, interpolating, and dividing by the interpolated reciprocal fixes
   * it. */
  {
    const double sum = a0 * inverse_w[0] + a1 * inverse_w[1] + a2 * inverse_w[2];
    if (sum > 0.0) {
      const double scale = 1.0 / sum;
      c0 = a0 * inverse_w[0] * scale;
      c1 = a1 * inverse_w[1] * scale;
      c2 = a2 * inverse_w[2] * scale;
    } else {
      c0 = a0;
      c1 = a1;
      c2 = a2;
    }
  }
  p->c[0] = c0;
  p->c[1] = c1;
  p->c[2] = c2;

  /* The texture coordinates, one set per generator - only the sets some
   * stage reads. */
  while (used) {
    unsigned channel = 0;
    const GcGxPoint *points = tri->points;
    while (!((used >> channel) & 1u)) channel++;
    used &= used - 1u;
    p->coordinate[channel][0] = (float)(c0 * points[0].texcoord[channel][0] +
                                        c1 * points[1].texcoord[channel][0] +
                                        c2 * points[2].texcoord[channel][0]);
    p->coordinate[channel][1] = (float)(c0 * points[0].texcoord[channel][1] +
                                        c1 * points[1].texcoord[channel][1] +
                                        c2 * points[2].texcoord[channel][1]);
  }
}

/* A point of a remembered row, filled in if it is not already. */
static GcRasterPoint *row_point(const GcRasterTriangle *tri, GcRasterRow *row, int px, int py) {
  GcRasterPoint *p = &row->point[px];
  if (p->stamp != row->stamp) {
    edges_at(tri, (double)px + SAMPLE_POINT, (double)py + SAMPLE_POINT, p->w);
    finish_point(tri, p);
    p->stamp = row->stamp;
  }
  return p;
}

/* ------------------------------------------------------------------ */
/* Where in a row the triangle is                                      */

/* A pixel is inside when all three edge functions pass: none negative for
 * a triangle of positive area, none positive for negative area - written
 * as the pixel loop writes it, so the two agree on every double.
 *
 * Along one row each edge function is monotonic in px. It is
 * `A * (sample_y - y_r) - B * (sample_x - x_r)` with everything but
 * sample_x fixed; sample_x = px + 7/12 rounded, sample_x - x_r, B times
 * that, and A-term minus that are each a monotonic function of the one
 * input that varies, because rounding is monotonic. So the pixels where one
 * edge passes are a run at the start or the end of the row (which, by the
 * sign of B), all three pass on the intersection of three such runs, and
 * that is one run. A binary search finds each end exactly - it asks the same
 * question of the same doubles the pixel loop asks, only of fewer pixels.
 *
 * That needs every value finite, which a bound on the vertices guarantees:
 * with |x|, |y| at most 1e30 no product or difference here can overflow or
 * become NaN. A triangle outside that bound scans every pixel as before. */
static double edge_function(const GcRasterTriangle *tri, unsigned edge, double sample_x,
                            double sample_y) {
  const double *x = tri->x, *y = tri->y;
  switch (edge) {
    case 0: return (x[2] - x[1]) * (sample_y - y[1]) - (y[2] - y[1]) * (sample_x - x[1]);
    case 1: return (x[0] - x[2]) * (sample_y - y[2]) - (y[0] - y[2]) * (sample_x - x[2]);
    default: return (x[1] - x[0]) * (sample_y - y[0]) - (y[1] - y[0]) * (sample_x - x[0]);
  }
}

static int edge_passes(const GcRasterTriangle *tri, unsigned edge, int px, double sample_y) {
  const double w = edge_function(tri, edge, (double)px + SAMPLE_POINT, sample_y);
  return tri->area > 0.0 ? !(w < 0.0) : !(w > 0.0);
}

static int spans_exact(const GcRasterTriangle *tri) {
  unsigned i;
  for (i = 0; i < 3; i++) {
    const double x = tri->x[i] < 0.0 ? -tri->x[i] : tri->x[i];
    const double y = tri->y[i] < 0.0 ? -tri->y[i] : tri->y[i];
    if (!(x <= 1e30) || !(y <= 1e30)) return 0;
  }
  return 1;
}

static void row_span(const GcRasterTriangle *tri, int py, int *lo_io, int *hi_io) {
  static const unsigned from[3] = {1u, 2u, 0u}, to[3] = {2u, 0u, 1u};
  const double sample_y = (double)py + SAMPLE_POINT;
  int lo = *lo_io, hi = *hi_io;
  unsigned edge;
  for (edge = 0; edge < 3u && lo <= hi; edge++) {
    const double slope = tri->y[to[edge]] - tri->y[from[edge]];
    if (slope == 0.0) {
      /* B is a zero: the function does not move along the row. */
      if (!edge_passes(tri, edge, lo, sample_y)) hi = lo - 1;
    } else if ((slope > 0.0) == (tri->area > 0.0)) {
      /* Passes on a run at the start: find its last pixel. */
      int yes = lo, no = hi;
      if (!edge_passes(tri, edge, lo, sample_y)) {
        hi = lo - 1;
      } else if (!edge_passes(tri, edge, hi, sample_y)) {
        while (no - yes > 1) {
          const int mid = yes + (no - yes) / 2;
          if (edge_passes(tri, edge, mid, sample_y)) yes = mid;
          else no = mid;
        }
        hi = yes;
      }
    } else {
      /* Passes on a run at the end: find its first pixel. */
      int no = lo, yes = hi;
      if (!edge_passes(tri, edge, hi, sample_y)) {
        hi = lo - 1;
      } else if (!edge_passes(tri, edge, lo, sample_y)) {
        while (yes - no > 1) {
          const int mid = no + (yes - no) / 2;
          if (edge_passes(tri, edge, mid, sample_y)) yes = mid;
          else no = mid;
        }
        lo = yes;
      }
    }
  }
  *lo_io = lo;
  *hi_io = hi;
}

/* ------------------------------------------------------------------ */
/* Rows                                                                */

typedef struct GcRasterJob {
  GcMachine *m;
  GcRasterTriangle tri;
  GcGxTevPlan *plan;
  int left, right, top, bottom;
  int early_z, z_test, z_write;
  unsigned z_function;
  /* The depth texture (depth_from_texture): 0 for none, 1 to add the
   * texture's value to the pixel's depth, 2 to replace it; which of the
   * texture's channels make the value; and the bias added to it. */
  unsigned z_texture, z_texture_type;
  uint32_t z_texture_bias;
  int spans;
  /* Late depth testing, and a combiner whose registers after a pixel depend
   * on that pixel alone (gc_gx_tev_plan_independent, no indirect stage):
   * then a pixel that fails the depth test is not combined at all. What it
   * would have written is thrown away by the test anyway; what it would
   * have left in the registers matters only if no later pixel of the same
   * rows runs the combiner, and draw_rows runs that one at the end. */
  int skip_hidden;
  /* Split across threads: rows in chunks of chunk_rows, taken in order. */
  int chunk_rows;
  long chunks;
  long next_chunk;
  struct GcRasterWorkers *workers; /* the drawing thread's (gc_gx_raster_one) */
} GcRasterJob;

#if defined(_MSC_VER)
#define GC_RASTER_INLINE static __forceinline
#elif defined(__GNUC__)
#define GC_RASTER_INLINE static inline __attribute__((always_inline))
#else
#define GC_RASTER_INLINE static inline
#endif

/* A point worked out on its own, by the same two steps row_point takes. */
static void point_at(const GcRasterTriangle *tri, int px, int py, GcRasterPoint *p) {
  edges_at(tri, (double)px + SAMPLE_POINT, (double)py + SAMPLE_POINT, p->w);
  finish_point(tri, p);
}

/* The combiner for one pixel, from its point and the points to its right and
 * below (read only when some stage uses a coordinate). */
GC_RASTER_INLINE void combine_point(const GcRasterJob *job, GcGxTev *tev, const GcRasterPoint *centre,
                                    const GcRasterPoint *right_of, const GcRasterPoint *under,
                                    GcGxTevPixel *pixel) {
  const GcRasterTriangle *const tri = &job->tri;
  const GcGxPoint *const points = tri->points;
  float gradient[GC_GX_MAX_TEXCOORDS][4];
  const double c0 = centre->c[0], c1 = centre->c[1], c2 = centre->c[2];
  unsigned channel;
  memset(pixel, 0, sizeof(*pixel));
  for (channel = 0; channel < 4; channel++) {
    const double value = c0 * (double)points[0].color[0][channel] +
                         c1 * (double)points[1].color[0][channel] +
                         c2 * (double)points[2].color[0][channel];
    pixel->raster[channel] = (int)clamp_to(value + 0.5, 0.0, 255.0);
  }
  /* How fast each coordinate is changing across the screen, which is what
   * picks a mipmap level. The hardware gets this from the two-by-two quad it
   * rasterises at once, taking differences with the pixel to the right and
   * the pixel below; this evaluates the same interpolation at those two
   * neighbours, which is the same quantity by a slower route.
   *
   * The neighbours are taken whether or not they are inside the triangle.
   * That is deliberate and it is what the hardware does: a quad straddling
   * an edge still needs a gradient for the pixels that are covered, and the
   * interpolation is defined everywhere on the triangle's plane. Clamping to
   * the covered pixels instead would make the level jump along every
   * silhouette.
   *
   * Only for the sets a texture with a range of levels samples through
   * (gradients_used): the rest are handed over as zero and never read. */
  if (tri->coordinates_used) {
    unsigned used = tri->coordinates_used;
    while (used) {
      channel = 0;
      while (!((used >> channel) & 1u)) channel++;
      used &= used - 1u;
      if ((tri->gradients_used >> channel) & 1u) {
        gradient[channel][0] = right_of->coordinate[channel][0] - centre->coordinate[channel][0];
        gradient[channel][1] = right_of->coordinate[channel][1] - centre->coordinate[channel][1];
        gradient[channel][2] = under->coordinate[channel][0] - centre->coordinate[channel][0];
        gradient[channel][3] = under->coordinate[channel][1] - centre->coordinate[channel][1];
      } else {
        gradient[channel][0] = gradient[channel][1] = gradient[channel][2] = gradient[channel][3] = 0.0f;
      }
    }
  }
  gc_gx_tev_run_planned(job->m, job->plan, tev, (const float(*)[2])centre->coordinate,
                        (const float(*)[4])gradient, pixel);
}

/* A pixel's depth when the depth texture is on.
 *
 * The last texture the combiner sampled is read as a number rather than a
 * colour - eight bits from alpha, sixteen from alpha and red, or twenty-four
 * from red, green and blue - the bias is added, and the result either is the
 * pixel's depth or is added to the depth the triangle gave it. Twenty-four
 * bits are kept.
 *
 * It is how a game clears the depth buffer without a copy: a rectangle over
 * the whole screen, depth test "always", and a small texture whose colour is
 * the depth wanted. Super Mario Galaxy 2 does that at the top of every
 * frame; without this the rectangle wrote its own depth, the nearest there
 * is, and every pixel of the scene behind it failed the depth test.
 *
 * From Dolphin's software renderer, read as a description. **Unverified:**
 * whether the sample is taken before or after the stage's channel swap (it
 * is before, here), and what a last stage with no texture leaves. */
static uint32_t depth_from_texture(const GcRasterJob *job, const GcGxTevPixel *pixel, uint32_t depth) {
  const int *t = pixel->texture;
  uint32_t value = job->z_texture_bias;
  switch (job->z_texture_type) {
    case 0: value += (uint32_t)t[3]; break;
    case 1: value += ((uint32_t)t[3] << 8) | (uint32_t)t[0]; break;
    default: value += ((uint32_t)t[0] << 16) | ((uint32_t)t[1] << 8) | (uint32_t)t[2]; break;
  }
  if (job->z_texture == 1u) value += depth;
  return value & 0x00ffffffu;
}

/* Rows first to last of a triangle, on one thread, with the combiner
 * computing in `tev`. Returns non-zero if the combiner ran for any pixel. */
static int draw_rows(const GcRasterJob *job, GcRasterWorker *worker, GcGxTev *tev, int first,
                     int last) {
  GcMachine *const m = job->m;
  const GcRasterTriangle *const tri = &job->tri;
  const double *const z = tri->z;
  const int early_z = job->early_z, z_test = job->z_test, z_write = job->z_write;
  const unsigned z_function = job->z_function;
  const int skip_hidden = job->skip_hidden;
  GcRasterRow *here = &worker->rows[0], *below = &worker->rows[1];
  int ran = 0, px, py;
  int owed = 0, owed_px = 0, owed_py = 0;

  here->stamp = next_stamp(worker);
  below->stamp = next_stamp(worker);

  for (py = first; py <= last; py++) {
    const double sample_y = (double)py + SAMPLE_POINT;
    int from_x = job->left, to_x = job->right;
    if (py != first) {
      /* The row that was below is this row now. */
      GcRasterRow *swap = here;
      here = below;
      below = swap;
      below->stamp = next_stamp(worker);
    }
    if (job->spans) row_span(tri, py, &from_x, &to_x);
    for (px = from_x; px <= to_x; px++) {
      const double sample_x = (double)px + SAMPLE_POINT;
      GcRasterPoint *centre = &here->point[px];
      /* Barycentric weights, from the same cross products the area came
       * from, so a point exactly on an edge is decided consistently. */
      double wo[3];
      const double *w = wo;
      const size_t at = (size_t)py * GC_EFB_WIDTH + (size_t)px;
      double depth;
      uint32_t incoming;

      if (centre->stamp == here->stamp) {
        w = centre->w;
      } else {
        edges_at(tri, sample_x, sample_y, wo);
      }

      if (tri->area > 0.0) {
        if (w[0] < 0.0 || w[1] < 0.0 || w[2] < 0.0) continue;
      } else {
        if (w[0] > 0.0 || w[1] > 0.0 || w[2] > 0.0) continue;
      }

      if (centre->stamp != here->stamp) {
        centre->w[0] = wo[0];
        centre->w[1] = wo[1];
        centre->w[2] = wo[2];
        finish_point(tri, centre);
        centre->stamp = here->stamp;
      }

      /* Depth is left out of the perspective correction: it has already
       * been divided by w on the way through the viewport, so it *is*
       * linear in screen space and correcting it again would bend it. */
      depth = clamp_to(centre->a[0] * z[0] + centre->a[1] * z[1] + centre->a[2] * z[2], 0.0,
                       16777215.0);
      incoming = (uint32_t)depth;
      /* With early depth testing the test runs here, before the texture
       * stages, and a pixel the alpha test later kills has already written
       * its depth. Without it, both happen after. */
      if (early_z) {
        if (z_test && !depth_passes(z_function, incoming, m->efb.depth[at])) continue;
        if (z_write) m->efb.depth[at] = incoming;
      } else if (skip_hidden && !depth_passes(z_function, incoming, m->efb.depth[at])) {
        /* The late depth test will throw this pixel away whatever the
         * combiner gives it, and nothing the combiner does before then can
         * change the depth it compares against. So only the registers it
         * would leave behind are owed, and only if this turns out to be the
         * last pixel to run it (see skip_hidden in the job). */
        owed = 1;
        owed_px = px;
        owed_py = py;
        continue;
      }

      {
        /* The interpolated vertex colour goes into the combiner, and what
         * comes out goes through the pixel engine's output stage. */
        GcGxTevPixel pixel;
        const GcRasterPoint *right_of = NULL, *under = NULL;
        /* The neighbours are the points the pixel to the right and the
         * pixel below will need as their own (see combine_point). */
        if (tri->gradients_used) {
          right_of = row_point(tri, here, px + 1, py);
          under = row_point(tri, below, px, py + 1);
        }
        combine_point(job, tev, centre, right_of, under, &pixel);
        ran = 1;
        owed = 0;
        if (job->z_texture) incoming = depth_from_texture(job, &pixel, incoming);
        if (!gc_gx_alpha_test(m, pixel.result[3])) continue;
        if (!early_z) {
          if (z_test && !depth_passes(z_function, incoming, m->efb.depth[at])) continue;
          if (z_write) m->efb.depth[at] = incoming;
        }
        /* Fog sits between the last combiner stage and the framebuffer
         * blend, and it never touches alpha - the fog colour register has
         * no alpha field - so it is applied only to a pixel that is going to
         * be written: the alpha test sees the same alpha either side of it,
         * and neither test reads the colour. */
        gc_gx_fog_apply(m, incoming, (uint32_t)px, pixel.result);
        gc_gx_write_pixel(m, (uint32_t)px, (uint32_t)py, pixel.result);
      }
      worker->pixels++;
    }
  }
  if (owed) {
    /* The last pixel that would have run the combiner was one the depth
     * test threw away: run it now, on the same three points, for the
     * registers it leaves. Its colour goes nowhere. */
    GcRasterPoint centre, neighbour[2];
    GcGxTevPixel pixel;
    point_at(tri, owed_px, owed_py, &centre);
    if (tri->gradients_used) {
      point_at(tri, owed_px + 1, owed_py, &neighbour[0]);
      point_at(tri, owed_px, owed_py + 1, &neighbour[1]);
      combine_point(job, tev, &centre, &neighbour[0], &neighbour[1], &pixel);
    } else {
      combine_point(job, tev, &centre, NULL, NULL, &pixel);
    }
    ran = 1;
  }
  return ran;
}

/* ------------------------------------------------------------------ */
/* Rows on several threads                                             */

/* Why the rows of one triangle can be drawn on several threads at once and
 * give the same framebuffer, the same registers and the same count:
 *
 *  - A pixel reads and writes the colour and depth at its own position and
 *    nowhere else: the depth test and write, the blend's destination read,
 *    the colour write. Two pixels of one triangle never share a position.
 *  - Everything else a pixel reads cannot change while a triangle is drawn:
 *    the register file, main memory (textures), the fog and alpha-compare
 *    registers, the triangle's own vertices.
 *  - The one thing a pixel leaves for the next is the combiner's colour
 *    registers. Rows are split only when gc_gx_tev_plan_independent says no
 *    stage reads a register group some stage writes before this pixel has
 *    written it - so each pixel's result, and the registers it leaves, are a
 *    function of that pixel alone. Each thread computes in its own copy, and
 *    afterwards the machine's registers are set to what the last pixel in
 *    drawing order left: the copy of the thread that drew the highest chunk
 *    in which the combiner ran, as that chunk left it. A thread takes chunks
 *    in increasing order, so its last such chunk is its highest.
 *  - Indirect stages are drawn on one thread: the indirect lookup can
 *    count and log an unsupported matrix.
 *  - The memos a pixel uses - the remembered rows here, the compressed
 *    colour memo in gc_gx_texture.c - are a thread's own, and neither
 *    changes a result.
 *  - A helper computes in the caller's floating-point rounding mode
 *    (gc_gx_workers.c).
 *  - The pixel count is a sum. */
#define GC_RASTER_MAX_WORKERS 16u

/* A thread that draws - the machine's, or a graphics thread
 * (gc_gx_thread.h) - has workers of its own: its own (0), and one for each
 * helper that takes a piece of its jobs, whose results it reads after the
 * pool has returned - by when another drawing thread may already have handed
 * the helpers a job of its own. Each is made the first time it is needed. */
typedef struct GcRasterWorkers {
  GcRasterWorker *worker[GC_RASTER_MAX_WORKERS];
} GcRasterWorkers;

static GcRasterWorker *worker_for(GcRasterWorkers *w, unsigned id) {
  if (!w->worker[id]) w->worker[id] = (GcRasterWorker *)calloc(1, sizeof(GcRasterWorker));
  return w->worker[id];
}

static void draw_chunks(void *argument, unsigned id) {
  GcRasterJob *const job = (GcRasterJob *)argument;
  GcRasterWorker *const worker = job->workers->worker[id];
  long chunk;
  worker->tev = job->m->gx.tev;
  while ((chunk = gc_workers_take(&job->next_chunk)) < job->chunks) {
    const int first = job->top + (int)chunk * job->chunk_rows;
    const int last = (first + job->chunk_rows - 1 < job->bottom) ? first + job->chunk_rows - 1 : job->bottom;
    if (draw_rows(job, worker, &worker->tev, first, last)) {
      worker->tev_chunk = chunk;
      memcpy(worker->reg, worker->tev.reg, sizeof(worker->reg));
    }
  }
}

/* Whether this triangle is worth the helpers, and allowed them. */
static unsigned helpers_for(const GcRasterJob *job) {
  const long rows = (long)job->bottom - job->top + 1;
  const long columns = (long)job->right - job->left + 1;
  unsigned helpers, stage, i;
  /* Measured on the storybook title, whose 350 triangles a field are mostly
   * small: splitting everything of two rows and 256 pixels of bounding box
   * or more took it from 28 to 18 ms a field against 8 rows and 4,096, and
   * going lower changed nothing. */
  if (rows < 2 || rows * columns < 256) return 0;
  helpers = gc_workers_count();
  if (!helpers) return 0;
  if (!gc_gx_tev_plan_independent(job->plan)) return 0;
  for (stage = 0; stage < job->plan->stages; stage++) {
    if (job->plan->stage[stage].textured && job->plan->stage[stage].indirect) return 0;
  }
  if (helpers + 1u > GC_RASTER_MAX_WORKERS) helpers = GC_RASTER_MAX_WORKERS - 1u;
  for (i = 0; i <= helpers; i++) {
    if (!worker_for(job->workers, i)) return 0;
  }
  return helpers;
}

/* Whether the rows of a triangle drawn under this plan may be split between
 * threads, and its hidden pixels left uncombined: every pixel's combiner
 * independent of the pixel before it, and no indirect stage. */
static int plan_splittable(const GcGxTevPlan *plan) {
  unsigned stage;
  if (!gc_gx_tev_plan_independent(plan)) return 0;
  for (stage = 0; stage < plan->stages; stage++) {
    if (plan->stage[stage].textured && plan->stage[stage].indirect) return 0;
  }
  return 1;
}

/* Everything the rows of a triangle need, worked out once: its edges, its
 * bounding box in the scissor box, and how its pixels are tested. Returns 0
 * for a triangle that draws nothing (no area, culled, or outside the box).
 * Reads the blitting-processor registers and nothing else. */
static int setup_job(GcMachine *m, const GcGxPoint points[3], GcGxTevPlan *plan, int splittable,
                     GcRasterJob *job) {
  const GcGx *gx = &m->gx;

  GcScissor box;
  int shift_x, shift_y;
  GcRasterTriangle *const tri = &job->tri;
  int left, right, top, bottom;
  const uint32_t z_mode = gx->bp[BP_Z_MODE];
  const double *x = tri->x, *y = tri->y;
  unsigned i;

  scissor_box(m, &box, &shift_x, &shift_y);

  for (i = 0; i < 3; i++) {
    tri->x[i] = points[i].screen[0] - (double)shift_x;
    tri->y[i] = points[i].screen[1] - (double)shift_y;
    tri->z[i] = points[i].screen[2];
    tri->inverse_w[i] = (points[i].clip[3] != 0.0f) ? 1.0 / (double)points[i].clip[3] : 0.0;
  }

  tri->area = doubled_area(tri->x, tri->y);
  if (tri->area == 0.0) return 0;
  if (culled(m, tri->area)) return 0;

  /* The pixels whose sample point could be inside. */
  left = (int)(x[0] < x[1] ? (x[0] < x[2] ? x[0] : x[2]) : (x[1] < x[2] ? x[1] : x[2])) - 1;
  right = (int)(x[0] > x[1] ? (x[0] > x[2] ? x[0] : x[2]) : (x[1] > x[2] ? x[1] : x[2])) + 1;
  top = (int)(y[0] < y[1] ? (y[0] < y[2] ? y[0] : y[2]) : (y[1] < y[2] ? y[1] : y[2])) - 1;
  bottom = (int)(y[0] > y[1] ? (y[0] > y[2] ? y[0] : y[2]) : (y[1] > y[2] ? y[1] : y[2])) + 1;
  if (left < box.left) left = box.left;
  if (top < box.top) top = box.top;
  if (right > box.right) right = box.right;
  if (bottom > box.bottom) bottom = box.bottom;
  if (left > right || top > bottom) return 0;

  tri->points = points;
  tri->coordinates_used = plan->coordinates_used;
  tri->gradients_used = plan->gradients_used;

  job->m = m;
  job->plan = plan;
  job->left = left;
  job->right = right;
  job->top = top;
  job->bottom = bottom;
  /* The depth texture: bits 2 and 3 say add or replace (3 is not a value
   * the hardware has; it is taken as none), bits 0 and 1 the width. A depth
   * that comes out of the combiner cannot be tested before the combiner
   * runs, so the test is late whatever the early bit says, and no pixel is
   * skipped on the strength of the depth the triangle gave it. */
  job->z_texture = (gx->bp[BP_Z_TEXTURE] >> 2) & 3u;
  if (job->z_texture == 3u) job->z_texture = 0u;
  job->z_texture_type = gx->bp[BP_Z_TEXTURE] & 3u;
  job->z_texture_bias = gx->bp[BP_Z_TEXTURE_BIAS] & 0x00ffffffu;
  job->early_z = gc_gx_early_depth_test(m) && !job->z_texture;
  job->z_test = z_mode & 1u;
  job->z_write = (z_mode >> 4) & 1u;
  job->z_function = (z_mode >> 1) & 7u;
  job->skip_hidden = !job->early_z && job->z_test && splittable && !job->z_texture;
  /* A row too short to search is scanned. */
  /* Walk each row's exact span rather than the whole bounding box.
   *
   * This used to be done only for boxes sixteen pixels across or wider, on
   * the reasoning that working the span out costs more than stepping over a
   * few pixels that the edge tests reject. For foliage it is the other way
   * round: the triangles are small and thin, their bounding boxes hold four
   * or five times as many pixels as they cover, and every pixel stepped over
   * still pays for three edge functions. Measured on the swamp, where the
   * software rasteriser misses its budget: the fields over budget fell from
   * 183 to 141 of 877 and the median drawing field from 28.7 to 26.7 ms,
   * with every frame identical.
   *
   * The remaining condition is the one that matters - a span can only be
   * worked out when the coordinates are finite. */
  job->spans = (right - left >= 16) && spans_exact(tri);
  return 1;
}

/* Held triangles (below), and what a drawing thread keeps. */
#define GC_RASTER_HELD_MAX 2048u  /* triangles */
#define GC_RASTER_DRAWS_MAX 256u  /* draws they came from */
#define GC_RASTER_BYTES_MAX (4u << 20)
#define GC_RASTER_MAX_BANDS 256u

/* A draw held back: its vertices as the stream carried them, and the format
 * they were in. */
typedef struct GcRasterDraw {
  unsigned kind;
  unsigned count;
  GcGxLayout layout;
  size_t offset; /* into h->bytes */
} GcRasterDraw;

typedef struct GcRasterHeld {
  unsigned draw;  /* which held draw */
  unsigned local; /* which of its triangles, in assembly order */
  /* Filled in by prepare_held: the pieces clipping left that draw something,
   * each with its job (whose tri.points is point[k]). */
  unsigned pieces;
  int unclipped;
  GcRasterJob job[GC_RASTER_MAX_PIECES];
  GcGxPoint point[GC_RASTER_MAX_PIECES][3];
} GcRasterHeld;

typedef struct GcRasterHeldState {
  GcMachine *m;
  unsigned open;  /* inside gc_gx_raster_begin / gc_gx_raster_end */
  unsigned count; /* triangles */
  unsigned draws;
  /* The plan below was worked out for h->m's registers as they stand
   * (cleared by every flush, which every register write makes), and whether
   * it splits. A plan that does not split is remembered too, so that a run
   * of draws made when they arrive is not prepared twice a triangle. */
  int known;
  int splittable;
  int gl; /* whether the plan above was judged for the OpenGL path */
  GcGxRasterPiece *pieces; /* for the OpenGL path: every piece, set up */
  const GcGxRasterPiece **piece_list;
  size_t pieces_size;
  GcRasterDraw draw[GC_RASTER_DRAWS_MAX];
  uint8_t *bytes;
  size_t bytes_used, bytes_size;
  GcRasterHeld held[GC_RASTER_HELD_MAX];
  GcGxTevPlan plan;
  long next_triangle; /* for prepare_held */
  int top, bottom;    /* the rows any held piece touches */
  uint64_t area;      /* their bounding boxes' pixels, added up */
  /* For one flush: bands of band_rows rows from top, taken in order. */
  int band_rows;
  long bands;
  long next_band;
  long band_last[GC_RASTER_MAX_BANDS]; /* the last piece to run the combiner in a band, or -1 */
  int band_reg[GC_RASTER_MAX_BANDS][4][4];
  GcRasterWorkers workers;
  GcGxTevPlan one_plan; /* gc_gx_raster_one's */
} GcRasterHeldState;

/* Each drawing thread's held triangles and workers. The machine's thread
 * draws when there is no graphics thread, and a graphics thread draws for
 * its machine; a differential's second machine draws on the machine's
 * thread at the same time. Helper jobs are handed the state they work on. */
#if defined(_MSC_VER)
static __declspec(thread) GcRasterHeldState *held_tls;
#else
static __thread GcRasterHeldState *held_tls;
#endif

static GcRasterHeldState *held_state(void) {
  if (!held_tls) {
    held_tls = (GcRasterHeldState *)calloc(1, sizeof(GcRasterHeldState));
    if (!held_tls) abort();
  }
  return held_tls;
}

void gc_gx_raster_forget_thread(void) {
  unsigned i;
  if (!held_tls) return;
  for (i = 0; i < GC_RASTER_MAX_WORKERS; i++) free(held_tls->workers.worker[i]);
  free(held_tls->bytes);
  free(held_tls->pieces);
  free((void *)held_tls->piece_list);
  free(held_tls);
  held_tls = NULL;
}

void gc_gx_raster_one(GcMachine *m, const GcGxPoint points[3]) {
  /* The fast path takes the triangle whole. Everything before this - the
   * transform, the clipper, the trivial rejection - has already run, and
   * everything after it is what the card does instead. */
  if (gc_gl_active(m)) {
    gc_gl_triangle(m, points);
    return;
  }

  GcGx *gx = &m->gx;
  GcRasterJob job;
  GcRasterHeldState *const h = held_state();
  /* The combiner's registers cannot change while a triangle is drawn. */
  GcGxTevPlan *const plan = &h->one_plan;
  unsigned helpers, i;

  gc_gx_tev_prepare(m, plan);
  if (!setup_job(m, points, plan, plan_splittable(plan), &job)) return;
  gc_perf.triangles++; /* the one-at-a-time path, on this thread */
  job.workers = &h->workers;

  helpers = helpers_for(&job);
  if (helpers) {
    const long rows = (long)job.bottom - job.top + 1;
    long per = rows / ((long)(helpers + 1u) * 4);
    uint64_t pixels = 0;
    long best = -1;
    unsigned best_id = 0;
    job.chunk_rows = (int)(per < 2 ? 2 : per);
    job.chunks = (rows + job.chunk_rows - 1) / job.chunk_rows;
    job.next_chunk = 0;
    /* Reset here, not by the helper: one that never joins this job - asleep,
     * or turned away because the job had closed - must not be read as
     * having drawn what it drew for the last one. */
    for (i = 0; i <= helpers; i++) {
      h->workers.worker[i]->pixels = 0;
      h->workers.worker[i]->tev_chunk = -1;
    }
    if (GC_WORKERS_RUN_SHARED(draw_chunks, &job) == 0) {
      for (i = 0; i <= helpers; i++) {
        const GcRasterWorker *worker = h->workers.worker[i];
        pixels += worker->pixels;
        if (worker->tev_chunk > best) {
          best = worker->tev_chunk;
          best_id = i;
        }
      }
      if (best >= 0) memcpy(gx->tev.reg, h->workers.worker[best_id]->reg, sizeof(gx->tev.reg));
      gx->pixels += pixels;
      gc_perf.pixels += pixels;
      return;
    }
  }

  {
    GcRasterWorker *const worker = worker_for(&h->workers, 0);
    worker->pixels = 0;
    (void)draw_rows(&job, worker, &gx->tev, job.top, job.bottom);
    gx->pixels += worker->pixels;
    gc_perf.pixels += worker->pixels;
  }
}

/* ------------------------------------------------------------------ */
/* Triangles held back, and drawn together                             */

/* A 3D scene is thousands of small triangles a field, and splitting the
 * rows of each one across threads costs more in waking and joining than the
 * few dozen pixels in it are worth. So while the command stream runs, a
 * draw whose triangles' rows could have been split is held back as the bytes
 * of its vertices, and held triangles are drawn together: first each is
 * decoded, transformed, clipped and set up (on any thread, since that reads
 * only registers and main memory and writes only the triangle's own record;
 * a triangle's vertices are found from its place in the draw, the way
 * primitive assembly would have picked them), then the framebuffer
 * is cut into bands of rows, each band is drawn by one thread, and a band
 * draws its rows of every held triangle, in the order they arrived. That
 * gives the same framebuffer, registers and counts as drawing each one when
 * it arrived, because:
 *
 *  - The only state pixels share is the colour and depth at a position, and
 *    every position belongs to one band, which sees the triangles covering
 *    it in their order - the order they would have been drawn in.
 *  - Nothing a held triangle reads changes while it is held. The registers
 *    change only by a command in the stream, and the stream draws what is
 *    held before any register write (gc_gx_raster_flush from write_cp,
 *    write_xf and write_bp - the last also being how an EFB copy or a
 *    palette load arrives). Main memory - the textures - changes only when
 *    the processor runs, and the stream draws what is held before it
 *    returns (gc_gx_raster_end).
 *  - So every held triangle has the same combiner plan, worked out once, and
 *    a triangle is held only when that plan is plan_splittable: then the
 *    registers a pixel reads are either written by that pixel first or
 *    written by no stage at all, and so are the same whatever order the bands
 *    run in. The registers the machine is left with are those of the last
 *    triangle to run the combiner, in its highest band that did.
 *  - Decoding a vertex reads its own bytes, the vertex-format registers and
 *    the arrays an indexed attribute points into, none of which can change
 *    while it is held. Transforming and clipping a triangle is arithmetic on
 *    its own vertices and the registers, and a helper computes in the
 *    caller's rounding mode
 *    (gc_gx_workers.c), so it gives the same points on any thread; the
 *    count of triangles dropped for crossing the eye plane is added up
 *    afterwards.
 *  - Each band keeps its own remembered rows and its own copy of the
 *    registers, as for split rows above.
 *
 * A draw that cannot be held - a plan that is not splittable, the OpenGL
 * path, a program watching vertices or triangles through on_primitive or
 * on_triangle, lines and points, or more than the room there is - is drawn
 * when it arrives, after what is held. */

uint8_t *gc_gx_raster_hold_draw(GcMachine *m, unsigned kind, const GcGxLayout *layout, unsigned count,
                                unsigned triangles) {
  const size_t bytes = (size_t)count * layout->size;
  GcRasterDraw *draw;
  unsigned i;
  const int gl = gc_gl_active(m);
  GcRasterHeldState *const h = held_state();
  if (!h->open || m->gx.on_triangle || (!gl && !gc_workers_count())) return NULL;
  if (h->count && h->m != m) gc_gx_raster_flush(h->m);
  if (!h->count && !(h->known && h->m == m && h->gl == gl)) {
    h->m = m;
    gc_gx_tev_prepare(m, &h->plan);
    /* The OpenGL path draws every pixel on its own, so it takes any plan
     * whose pixels do not depend on the pixel before - indirect stages
     * included; the rest it draws on the reference, one triangle at a time
     * as they arrive. */
    h->splittable = gl ? gc_gx_tev_plan_independent(&h->plan) : plan_splittable(&h->plan);
    h->gl = gl;
    h->known = 1;
  }
  if (!h->splittable) return NULL;
  if (triangles > GC_RASTER_HELD_MAX || bytes > GC_RASTER_BYTES_MAX) {
    /* Too big to hold: drawn now, after what is already held. */
    gc_gx_raster_flush(m);
    return NULL;
  }
  if (h->count + triangles > GC_RASTER_HELD_MAX || h->draws == GC_RASTER_DRAWS_MAX ||
      h->bytes_used + bytes > GC_RASTER_BYTES_MAX) {
    gc_gx_raster_flush(m);
    /* The plan and whether it splits are unchanged: no register moved. */
    h->m = m;
    h->known = 1;
  }
  if (h->bytes_used + bytes > h->bytes_size) {
    size_t size = h->bytes_size ? h->bytes_size : 65536u;
    uint8_t *grown;
    while (size < h->bytes_used + bytes) size *= 2u;
    grown = (uint8_t *)realloc(h->bytes, size);
    if (!grown) {
      gc_gx_raster_flush(m);
      return NULL;
    }
    h->bytes = grown;
    h->bytes_size = size;
  }
  draw = &h->draw[h->draws];
  draw->kind = kind;
  draw->count = count;
  draw->layout = *layout;
  draw->offset = h->bytes_used;
  for (i = 0; i < triangles; i++) {
    h->held[h->count + i].draw = h->draws;
    h->held[h->count + i].local = i;
  }
  h->draws++;
  h->count += triangles;
  h->bytes_used += bytes;
  return h->bytes + draw->offset;
}

/* Which three vertices of a draw make its n-th triangle, in the order
 * gc_gx_assemble_vertex hands them to the rasteriser. */
static void triangle_vertices(unsigned kind, unsigned n, unsigned out[3]) {
  switch (kind) {
    case GC_GX_QUADS:
    case GC_GX_QUADS_2: {
      const unsigned first = (n / 2u) * 4u;
      out[0] = first;
      out[1] = first + 1u + (n & 1u);
      out[2] = first + 2u + (n & 1u);
      return;
    }
    case GC_GX_TRIANGLES:
      out[0] = n * 3u;
      out[1] = n * 3u + 1u;
      out[2] = n * 3u + 2u;
      return;
    case GC_GX_TRIANGLE_STRIP:
      /* Every other triangle is wound the other way by swapping its first
       * two vertices. */
      out[0] = (n & 1u) ? n + 1u : n;
      out[1] = (n & 1u) ? n : n + 1u;
      out[2] = n + 2u;
      return;
    default: /* a fan */
      out[0] = 0;
      out[1] = n + 1u;
      out[2] = n + 2u;
      return;
  }
}

/* Decode, transform, clip and set up held triangles, taken eight at a time
 * in any order (one at a time, the shared counter was a cost of its own). */
#define GC_RASTER_PREPARE_RUN 8
static void prepare_held(void *argument, unsigned id) {
  GcRasterHeldState *const h = (GcRasterHeldState *)argument;
  GcMachine *const m = h->m;
  const GcGx *const gx = &m->gx;
  long run, i;
  (void)id;
  while ((run = gc_workers_take(&h->next_triangle)) * GC_RASTER_PREPARE_RUN < (long)h->count) {
    const long end = (run + 1) * GC_RASTER_PREPARE_RUN;
    for (i = run * GC_RASTER_PREPARE_RUN; i < end && i < (long)h->count; i++) {
      GcRasterHeld *const entry = &h->held[i];
      const GcRasterDraw *const draw = &h->draw[entry->draw];
      const uint8_t *const bytes = h->bytes + draw->offset;
      GcGxPoint points[3];
      unsigned which[3], pieces, k;
      triangle_vertices(draw->kind, entry->local, which);
      for (k = 0; k < 3u; k++) {
        GcGxVertex vertex;
        gc_gx_decode_vertex_bytes(m, &draw->layout, bytes + (size_t)which[k] * draw->layout.size, &vertex);
        gc_gx_transform(gx, &vertex, &points[k]);
      }
      pieces = clip_triangle(gx, points, entry->point, &entry->unclipped);
      entry->pieces = 0;
      for (k = 0; k < pieces; k++) {
        if (entry->pieces != k) memcpy(entry->point[entry->pieces], entry->point[k], sizeof(entry->point[k]));
        if (setup_job(m, entry->point[entry->pieces], &h->plan, 1, &entry->job[entry->pieces])) entry->pieces++;
      }
    }
  }
}

static void draw_bands(void *argument, unsigned id) {
  GcRasterHeldState *const h = (GcRasterHeldState *)argument;
  GcRasterWorker *const worker = h->workers.worker[id];
  long band;
  worker->tev = h->m->gx.tev;
  while ((band = gc_workers_take(&h->next_band)) < h->bands) {
    const int band_top = h->top + (int)band * h->band_rows;
    const int band_bottom = band_top + h->band_rows - 1;
    unsigned i, k;
    for (i = 0; i < h->count; i++) {
      const GcRasterHeld *const entry = &h->held[i];
      for (k = 0; k < entry->pieces; k++) {
        const GcRasterJob *const job = &entry->job[k];
        const int first = job->top > band_top ? job->top : band_top;
        const int last = job->bottom < band_bottom ? job->bottom : band_bottom;
        if (first > last) continue;
        if (draw_rows(job, worker, &worker->tev, first, last)) {
          h->band_last[band] = (long)(i * GC_RASTER_MAX_PIECES + k);
          memcpy(h->band_reg[band], worker->tev.reg, sizeof(h->band_reg[band]));
        }
      }
    }
  }
}

void gc_gx_raster_flush(GcMachine *m) {
  GcRasterHeldState *const h = held_state();
  GcGx *gx;
  unsigned i, k, helpers;
  int any = 0;
  (void)m;
  h->known = 0;
  if (!h->count) return;
  gx = &h->m->gx;
  helpers = gc_workers_count();
  if (helpers + 1u > GC_RASTER_MAX_WORKERS) helpers = GC_RASTER_MAX_WORKERS - 1u;
  for (i = 0; i <= helpers; i++) {
    if (!worker_for(&h->workers, i)) helpers = 0;
  }

  /* The triangles themselves: a handful are not worth waking anybody for. */
  h->next_triangle = 0;
  {
    const double started = gc_perf_now_ms();
    if (!helpers || h->count < 8u || GC_WORKERS_RUN_SHARED(prepare_held, h) != 0) prepare_held(h, 0);
    gc_perf.raster_prepare_ms += gc_perf_now_ms() - started;
  }

  h->area = 0;
  for (i = 0; i < h->count; i++) {
    const GcRasterHeld *const entry = &h->held[i];
    if (entry->unclipped) gx->unclipped_triangles++;
    /* Counted here rather than in setup_job: that runs on the helper
     * threads, and one shared increment a triangle cost about five percent
     * of this scene in cache-line traffic. This loop is the flushing
     * thread's alone and walks every piece anyway. */
    gc_perf.triangles += entry->pieces;
    for (k = 0; k < entry->pieces; k++) {
      const GcRasterJob *const job = &entry->job[k];
      if (!any || job->top < h->top) h->top = job->top;
      if (!any || job->bottom > h->bottom) h->bottom = job->bottom;
      any = 1;
      h->area += (uint64_t)(job->right - job->left + 1) * (uint64_t)(job->bottom - job->top + 1);
    }
  }
  if (!any) {
    h->count = 0;
    h->draws = 0;
    h->bytes_used = 0;
    return;
  }

  if (h->gl && gc_gl_active(h->m)) {
    /* The card draws them: every piece, set up, in the order it arrived. */
    size_t n = 0;
    for (i = 0; i < h->count; i++) n += h->held[i].pieces;
    if (n > h->pieces_size) {
      GcGxRasterPiece *grown = (GcGxRasterPiece *)realloc(h->pieces, n * sizeof(*grown));
      const GcGxRasterPiece **list = (const GcGxRasterPiece **)realloc((void *)h->piece_list, n * sizeof(*list));
      if (grown) h->pieces = grown;
      if (list) h->piece_list = list;
      if (grown && list) h->pieces_size = n;
    }
    if (n <= h->pieces_size) {
      n = 0;
      for (i = 0; i < h->count; i++) {
        const GcRasterHeld *const entry = &h->held[i];
        for (k = 0; k < entry->pieces; k++) {
          const GcRasterJob *const job = &entry->job[k];
          GcGxRasterPiece *const piece = &h->pieces[n];
          memcpy(piece->x, job->tri.x, sizeof(piece->x));
          memcpy(piece->y, job->tri.y, sizeof(piece->y));
          memcpy(piece->z, job->tri.z, sizeof(piece->z));
          memcpy(piece->inverse_w, job->tri.inverse_w, sizeof(piece->inverse_w));
          piece->area = job->tri.area;
          piece->left = job->left;
          piece->right = job->right;
          piece->top = job->top;
          piece->bottom = job->bottom;
          piece->points = entry->point[k];
          h->piece_list[n] = piece;
          n++;
        }
      }
      gc_gl_pieces(h->m, h->piece_list, (unsigned)n);
    } else {
      for (i = 0; i < h->count; i++) {
        const GcRasterHeld *const entry = &h->held[i];
        for (k = 0; k < entry->pieces; k++) gc_gl_triangle(h->m, entry->point[k]);
      }
    }
    h->count = 0;
    h->draws = 0;
    h->bytes_used = 0;
    return;
  }

  /* The same threshold as split rows: less than 256 pixels of bounding box,
   * or one row, is not worth waking anybody for. */
  if (helpers && h->area >= 256u && h->bottom > h->top) {
    const long rows = (long)h->bottom - h->top + 1;
    long per = rows / ((long)(helpers + 1u) * 4);
    uint64_t pixels = 0;
    long best = -1, band;
    if (per < 2) per = 2;
    if ((rows + per - 1) / per > (long)GC_RASTER_MAX_BANDS) per = (rows + GC_RASTER_MAX_BANDS - 1) / GC_RASTER_MAX_BANDS;
    h->band_rows = (int)per;
    h->bands = (rows + per - 1) / per;
    h->next_band = 0;
    for (band = 0; band < h->bands; band++) h->band_last[band] = -1;
    for (i = 0; i <= helpers; i++) h->workers.worker[i]->pixels = 0;
    const double rows_started = gc_perf_now_ms();
    if (GC_WORKERS_RUN_SHARED(draw_bands, h) == 0) {
      long best_band = 0;
      gc_perf.raster_rows_ms += gc_perf_now_ms() - rows_started;
      for (i = 0; i <= helpers; i++) pixels += h->workers.worker[i]->pixels;
      for (band = 0; band < h->bands; band++) {
        if (h->band_last[band] >= best) {
          best = h->band_last[band];
          best_band = band;
        }
      }
      if (best >= 0) memcpy(gx->tev.reg, h->band_reg[best_band], sizeof(gx->tev.reg));
      gx->pixels += pixels;
      gc_perf.pixels += pixels;
      h->count = 0;
      h->draws = 0;
      h->bytes_used = 0;
      return;
    }
  }
  /* One thread: each piece whole, in order, exactly as it would have been
   * drawn when it arrived. */
  worker_for(&h->workers, 0)->pixels = 0;
  for (i = 0; i < h->count; i++) {
    const GcRasterHeld *const entry = &h->held[i];
    for (k = 0; k < entry->pieces; k++) {
      const GcRasterJob *const job = &entry->job[k];
      (void)draw_rows(job, worker_for(&h->workers, 0), &gx->tev, job->top, job->bottom);
    }
  }
  gx->pixels += worker_for(&h->workers, 0)->pixels;
  h->count = 0;
  h->draws = 0;
  h->bytes_used = 0;
}

void gc_gx_raster_begin(GcMachine *m) {
  GcRasterHeldState *const h = held_state();
  if (h->count && h->m != m) gc_gx_raster_flush(h->m);
  h->open++;
}

void gc_gx_raster_end(GcMachine *m) {
  GcRasterHeldState *const h = held_state();
  gc_gx_raster_flush(m);
  if (h->open) h->open--;
}
