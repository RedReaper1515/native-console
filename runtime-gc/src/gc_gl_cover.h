/*
 * Which pixels a triangle covers, asked exactly as gc_gx_raster.c asks it,
 * for the OpenGL path's two threads: the machine's, which works out the
 * combiner registers a triangle leaves (carry, gc_gl.c), and the render
 * thread's, which cuts batches that read the framebuffer into layers
 * (gc_gl_render.c). Static functions, included by both.
 */
#ifndef GC_GL_COVER_H
#define GC_GL_COVER_H

#include <stdint.h>

#include "gc_efb.h"

typedef struct GcGlBox {
  int left, top, right, bottom;
} GcGlBox;

/* A triangle as the reference rasteriser sets it up. */
typedef struct GcGlTri {
  double x[3], y[3], z[3], inverse_w[3], area;
  GcGlBox box;
} GcGlTri;

/* Which pixels of a row a triangle covers, asked exactly as gc_gx_raster.c's
 * row_span asks it - the same edge functions on the same doubles at the same
 * sample point, and a binary search for each end of the run, which is one
 * run because each edge function is monotonic along a row. */
#define SAMPLE_POINT (7.0 / 12.0)

static double edge_function(const GcGlTri *t, unsigned edge, double sample_x, double sample_y) {
  const double *x = t->x, *y = t->y;
  switch (edge) {
    case 0: return (x[2] - x[1]) * (sample_y - y[1]) - (y[2] - y[1]) * (sample_x - x[1]);
    case 1: return (x[0] - x[2]) * (sample_y - y[2]) - (y[0] - y[2]) * (sample_x - x[2]);
    default: return (x[1] - x[0]) * (sample_y - y[0]) - (y[1] - y[0]) * (sample_x - x[0]);
  }
}

static int edge_passes(const GcGlTri *t, unsigned edge, int px, double sample_y) {
  const double w = edge_function(t, edge, (double)px + SAMPLE_POINT, sample_y);
  return t->area > 0.0 ? !(w < 0.0) : !(w > 0.0);
}

static int spans_exact(const GcGlTri *t) {
  unsigned i;
  for (i = 0; i < 3; i++) {
    const double x = t->x[i] < 0.0 ? -t->x[i] : t->x[i];
    const double y = t->y[i] < 0.0 ? -t->y[i] : t->y[i];
    if (!(x <= 1e30) || !(y <= 1e30)) return 0;
  }
  return 1;
}

/* The last pixel that passes in [yes, no], given that yes passes and no does
 * not (passes_high: the run is at the start) - or the first, given that yes
 * passes and no does not with no < yes. The search starts at a guess (the
 * row above's answer, which the answer is near) and gallops out from it
 * before halving, which asks the same monotonic question of fewer pixels
 * and so finds the same pixel. */
#define NO_HINT (-0x7fffffff)
static int boundary(const GcGlTri *t, unsigned edge, double sample_y, int yes, int no, int hint) {
  const int up = no > yes; /* the passing run is below `no` */
  if (hint != NO_HINT && (up ? (hint > yes && hint < no) : (hint < yes && hint > no))) {
    int step = 1;
    if (edge_passes(t, edge, hint, sample_y)) {
      yes = hint;
      for (;;) {
        const int at = up ? yes + step : yes - step;
        if (up ? at >= no : at <= no) break;
        if (edge_passes(t, edge, at, sample_y)) {
          yes = at;
          step *= 2;
        } else {
          no = at;
          break;
        }
      }
    } else {
      no = hint;
      for (;;) {
        const int at = up ? no - step : no + step;
        if (up ? at <= yes : at >= yes) break;
        if (edge_passes(t, edge, at, sample_y)) {
          yes = at;
          break;
        }
        no = at;
        step *= 2;
      }
    }
  }
  if (up) {
    while (no - yes > 1) {
      const int mid = yes + (no - yes) / 2;
      if (edge_passes(t, edge, mid, sample_y)) yes = mid;
      else no = mid;
    }
  } else {
    while (yes - no > 1) {
      const int mid = no + (yes - no) / 2;
      if (edge_passes(t, edge, mid, sample_y)) yes = mid;
      else no = mid;
    }
  }
  return yes;
}

static void row_span(const GcGlTri *t, int py, int *lo_io, int *hi_io, int hint_lo, int hint_hi) {
  static const unsigned from[3] = {1u, 2u, 0u}, to[3] = {2u, 0u, 1u};
  const double sample_y = (double)py + SAMPLE_POINT;
  int lo = *lo_io, hi = *hi_io;
  unsigned edge;
  for (edge = 0; edge < 3u && lo <= hi; edge++) {
    const double slope = t->y[to[edge]] - t->y[from[edge]];
    if (slope == 0.0) {
      if (!edge_passes(t, edge, lo, sample_y)) hi = lo - 1;
    } else if ((slope > 0.0) == (t->area > 0.0)) {
      /* Passes on a run at the start: find its last pixel. */
      if (!edge_passes(t, edge, lo, sample_y)) {
        hi = lo - 1;
      } else if (!edge_passes(t, edge, hi, sample_y)) {
        hi = boundary(t, edge, sample_y, lo, hi, hint_hi);
      }
    } else {
      /* Passes on a run at the end: find its first pixel. */
      if (!edge_passes(t, edge, hi, sample_y)) {
        hi = lo - 1;
      } else if (!edge_passes(t, edge, lo, sample_y)) {
        lo = boundary(t, edge, sample_y, hi, lo, hint_lo);
      }
    }
  }
  *lo_io = lo;
  *hi_io = hi;
}

static int pixel_covered(const GcGlTri *t, int px, int py) {
  const double sample_x = (double)px + SAMPLE_POINT, sample_y = (double)py + SAMPLE_POINT;
  const double w0 = edge_function(t, 0, sample_x, sample_y);
  const double w1 = edge_function(t, 1, sample_x, sample_y);
  const double w2 = edge_function(t, 2, sample_x, sample_y);
  if (t->area > 0.0) return !(w0 < 0.0 || w1 < 0.0 || w2 < 0.0);
  return !(w0 > 0.0 || w1 > 0.0 || w2 > 0.0);
}

/* The first and last covered pixel of row py (lo > hi when there is none).
 * Exact at both ends; a triangle too large for the search is scanned, and
 * then the pixels between the two ends may not all be covered, which the
 * callers allow for. */
static void covered_run(const GcGlTri *t, int py, int *lo, int *hi, int hint_lo, int hint_hi) {
  *lo = t->box.left;
  *hi = t->box.right;
  if (spans_exact(t)) {
    row_span(t, py, lo, hi, hint_lo, hint_hi);
    return;
  }
  while (*lo <= *hi && !pixel_covered(t, *lo, py)) (*lo)++;
  while (*hi >= *lo && !pixel_covered(t, *hi, py)) (*hi)--;
}

#endif /* GC_GL_COVER_H */
