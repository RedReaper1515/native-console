/*
 * The DS rendering engine: a software scanline rasteriser.
 *
 * nds_gx_render() draws the polygons the last SWAP_BUFFERS handed over, one
 * scanline at a time and in the order the geometry engine left them (opaque
 * first), into gx->frame. Each pixel has a colour, a 24-bit depth and
 * attributes (polygon IDs, fog and edge flags); under edge pixels the pixel
 * they covered is kept as well, which is what anti-aliasing blends with.
 * When a line's polygons are done, the line above gets its final pass: edge
 * marking, fog, anti-aliasing.
 *
 * References. GBATEK's "DS 3D" chapters for the registers, texture formats
 * and blending equations. The edge walker is a port of StrikerX3's
 * nds-interp (see slope_span below), which reproduces hardware captures of
 * every line. The fill rules are the ones polyrastertest (Jaklyy)
 * demonstrates with its hardware recording; tests/nds/test_nds_gx_raster.c
 * runs both. What neither GBATEK nor those captures show (attribute
 * interpolation, depth precision, the two-layer pixel store, anti-aliasing
 * coverage) is as melonDS's software renderer (src/GPU3D_Soft.cpp, GPL-3.0)
 * models the hardware; that source was read for the behaviour, not copied,
 * and such places say "melonDS".
 */
#include <stdlib.h>
#include <string.h>

#include "nds_gx.h"
#include "nds_gx_workers.h"

#define WIDTH NDS_GX_WIDTH
#define HEIGHT NDS_GX_HEIGHT

/* Pixel attributes. */
#define ATTR_EDGE_LEFT 0x00000001u   /* drawn as part of a polygon's left edge */
#define ATTR_EDGE_RIGHT 0x00000002u
#define ATTR_EDGE_TOP 0x00000004u    /* on a polygon's first line */
#define ATTR_EDGE_BOTTOM 0x00000008u /* on its last line */
#define ATTR_EDGE 0x0000000Fu
#define ATTR_BACK 0x00000010u        /* from a polygon facing away */
#define ATTR_TRANSLUCENT 0x00000020u /* a translucent pixel was blended in */
#define ATTR_FOG 0x00000040u
#define ATTR_COVERAGE 0x00001F00u    /* anti-aliasing coverage, 0-31 */
#define ATTR_COVERAGE_SHIFT 8
#define ATTR_OPAQUE_ID 0x003F0000u
#define ATTR_OPAQUE_ID_SHIFT 16
#define ATTR_TRANSLUCENT_ID 0x3F000000u /* where POLYGON_ATTR has the ID */
/* Which side of its polygon an edge pixel is on, for edges that two
 * polygons share (see takes_over): on an x-major edge with the polygon
 * below it, or inside the polygon's first line; the same with the polygon
 * above, or its last line; on a steeper left edge; on a steeper right one. */
#define ATTR_SIDE_UPPER 0x00400000u
#define ATTR_SIDE_LOWER 0x00800000u
#define ATTR_SIDE_LEFT 0x40000000u
#define ATTR_SIDE_RIGHT 0x80000000u

/* A pixel's colour, in nds_gx_line()'s format. */
#define RED(c) ((c) & 0x3Fu)
#define GREEN(c) (((c) >> 6) & 0x3Fu)
#define BLUE(c) (((c) >> 12) & 0x3Fu)
#define ALPHA(c) (((c) >> 18) & 0x1Fu)
#define COLOR(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 6) | ((uint32_t)(b) << 12) | ((uint32_t)(a) << 18))

/* "The 5bit RGB values are internally expanded to 6bit RGB as follows:
 * X=X*2+(X+31)/32" (GBATEK, COLOR). */
static inline uint32_t expand5(uint32_t c)
{
    return c ? c * 2u + 1u : 0u;
}

/* A BGR555 word as three 6-bit channels with no alpha. */
static inline uint32_t color_from_555(uint32_t c)
{
    return COLOR(expand5(c & 31u), expand5((c >> 5) & 31u), expand5((c >> 10) & 31u), 0);
}

/* ------------------------------------------------------------------ */
/* The frame's state                                                   */
/* ------------------------------------------------------------------ */

/* A polygon on the grid of the picture being drawn: where its vertices are,
 * which is topmost and which bottommost, and their lines. For the console's
 * own picture this is what polygon list RAM and vertex RAM hold. For a
 * larger one it is worked out again from the vertices' finer positions,
 * since which vertex is topmost can differ there. */
typedef struct Shape {
    int32_t x[NDS_GX_POLYGON_VERTICES], y[NDS_GX_POLYGON_VERTICES];
    int32_t y_top, y_bottom;
    uint8_t top, bottom;
    bool degenerate;
} Shape;

/* The rows of working pixels the console's own picture needs. */
typedef struct Rows {
    uint32_t depth[4][WIDTH + 2];
    uint32_t attr[4][WIDTH + 2];
    uint32_t border_depth[WIDTH + 2];
    uint32_t border_attr[WIDTH + 2];
    uint32_t under_color[2][WIDTH];
    uint32_t under_depth[2][WIDTH];
    uint32_t under_attr[2][WIDTH];
    uint8_t stencil[2][WIDTH];
} Rows;

typedef struct Render {
    const NdsGx *gx;
    const NdsGxVertex *vertices; /* the rendering bank */
    const NdsGxPolygon *polygons;
    NdsGxVram vram;

    /* The picture: the console's own (scale 1, into gx->frame, with the
     * console's arithmetic to the bit), or one `scale` times as large each
     * way (`fine`: nds_gx_render_scaled). */
    int width, height, scale;
    bool fine;
    uint32_t *frame;          /* height rows of width */
    /* The lines this is drawing, first to one past the last. The whole
     * picture, or a band of a larger one: then the line either side is
     * drawn too, for the depths and polygon IDs edge marking compares
     * with, and its colours go to `spare` and nowhere else. */
    int first, last;
    uint32_t *spare;          /* a row of width */
    unsigned edge_shift;      /* fraction bits of the weight along an edge: 9 on the console */
    unsigned span_shift;      /* ... and along a scanline: 8 */
    const Shape *shapes;      /* fine: one for each polygon of the rendering bank */

    /* the latched registers */
    bool texturing, highlight, blending, antialias, edge_marking, fog, fog_alpha_only;
    bool shared_edges;       /* edge marking without anti-aliasing: see takes_over */
    unsigned alpha_ref;      /* a pixel is drawn if its alpha is greater */
    uint32_t toon[32];
    uint32_t edge_color[8];
    uint32_t fog_color;
    uint32_t fog_offset;
    unsigned fog_shift;
    uint8_t fog_density[34];
    uint32_t clear_color, clear_depth, clear_attr;

    /* Depth and attributes of the visible pixels: the line being drawn and
     * the two above it, with a column either side that keeps the rear
     * plane's values, for edge marking. Each row is width + 2. */
    uint32_t *depth[4];
    uint32_t *attr[4];
    uint32_t *border_depth; /* the lines above and below the screen */
    uint32_t *border_attr;
    /* The pixel under each visible one, for the line being drawn and the
     * one above. Each row is width. */
    uint32_t *under_color[2];
    uint32_t *under_depth[2];
    uint32_t *under_attr[2];
    /* Shadow volumes: bit 0 set where the mask's depth test failed against
     * the visible pixel, bit 1 against the one under it. */
    uint8_t *stencil[2];
    bool mask_before; /* the polygon drawn before this one was a shadow mask */
} Render;

/* Texture memory: 512 KiB in four slots; palette memory: 96 KiB in six.
 * What is not mapped reads as zero. */
static inline uint32_t texture_byte(const Render *r, uint32_t addr)
{
    addr &= 0x7FFFFu;
    const uint8_t *slot = r->vram.texture[addr >> 17];
    return slot ? slot[addr & 0x1FFFFu] : 0u;
}

static inline uint32_t texture_word(const Render *r, uint32_t addr)
{
    return texture_byte(r, addr) | (texture_byte(r, addr + 1u) << 8);
}

static inline uint32_t palette_word(const Render *r, uint32_t addr)
{
    addr &= 0x1FFFEu;
    if (addr >= 0x18000u) return 0;
    const uint8_t *slot = r->vram.palette[addr >> 14];
    return slot ? nds_get_le16(&slot[addr & 0x3FFFu]) : 0u;
}

/* Where line y's colours go. */
static inline uint32_t *color_row(const Render *r, int y)
{
    return y >= r->first && y < r->last ? r->frame + (size_t)y * (size_t)r->width : r->spare;
}

/* ------------------------------------------------------------------ */
/* Edges                                                               */
/* ------------------------------------------------------------------ */

#define ONE (1 << 18) /* edge positions have 18 fraction bits */

/*
 * The pixels of one polygon edge on one scanline: columns *lo to *hi. The
 * edge runs from (x0, y0) down to (x1, y1), y0 <= y < y1.
 *
 * This is a port of the Slope class of nds-interp:
 *
 *   nds-interp/slope.h, https://github.com/StrikerX3/nds-interp,
 *   commit e3acde4c35ac53e628a2548238870a287409733e
 *   MIT License, Copyright (c) 2021 Ivan Roberto de Oliveira
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * What it found on hardware: the x step per line is (1 / dy) * dx, the
 * reciprocal taken first and cut to 18 bits. An edge steeper than 45
 * degrees has one pixel per line, at x0 + (y - y0) * step. A shallower one
 * (x-major) has a run of pixels: it starts at x0 + (y - y0) * step + 0.5
 * and ends at that start, with its low 9 fraction bits dropped, plus
 * step - 1.0; dropping those bits is what leaves a one-pixel gap in some
 * slopes. An edge going left mirrors this from one fraction unit below x0.
 */
static void slope_span(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t y, int32_t *lo, int32_t *hi)
{
    bool negative = x1 < x0;
    int32_t dx = negative ? x0 - x1 : x1 - x0;
    int32_t dy = y1 - y0;
    bool x_major = dx > dy;

    int64_t origin = (int64_t)x0 * ONE;
    if (negative) origin--;
    if (x_major || dx == dy) origin += negative ? -(ONE / 2) : ONE / 2;

    int64_t step = (int64_t)dx * (ONE / dy);
    int64_t travelled = (int64_t)(y - y0) * step;
    int64_t start = negative ? origin - travelled : origin + travelled;
    int64_t end = start;
    if (x_major) {
        if (negative) end = (start | 0x1FF) - step + ONE;
        else end = (start & ~(int64_t)0x1FF) + step - ONE;
    }
    if (negative) {
        *lo = (int32_t)(end >> 18);
        *hi = (int32_t)(start >> 18);
    } else {
        *lo = (int32_t)(start >> 18);
        *hi = (int32_t)(end >> 18);
    }
}

/*
 * The same for a picture drawn larger than the screen. The rules are the
 * console's: where an edge starts, which pixels of a line are its run. The
 * arithmetic is not: the console takes a reciprocal cut to 18 bits and
 * multiplies, and the error of that grows with both of an edge's lengths, to
 * several pixels over an edge four times as long each way. Here the distance
 * travelled is one division with 32 fraction bits, and an x-major run ends
 * where the next line's begins, without the nine dropped bits that leave the
 * console its one-pixel gaps.
 */
static void slope_span_fine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t y, int32_t *lo, int32_t *hi)
{
    const int64_t one = (int64_t)1 << 32;
    bool negative = x1 < x0;
    int64_t dx = negative ? x0 - x1 : x1 - x0;
    int64_t dy = y1 - y0;
    bool x_major = dx > dy;

    int64_t origin = (int64_t)x0 * one;
    if (negative) origin--;
    if (x_major || dx == dy) origin += negative ? -(one / 2) : one / 2;

    int64_t step = dx * one / dy;
    int64_t travelled = (int64_t)(y - y0) * dx * one / dy;
    int64_t start = negative ? origin - travelled : origin + travelled;
    int64_t end = start;
    if (x_major) end = negative ? start - step + one : start + step - one;
    if (negative) {
        *lo = (int32_t)(end >> 32);
        *hi = (int32_t)(start >> 32);
    } else {
        *lo = (int32_t)(start >> 32);
        *hi = (int32_t)(end >> 32);
    }
}

/* ------------------------------------------------------------------ */
/* Interpolation                                                       */
/* ------------------------------------------------------------------ */

/*
 * Attributes between two points, pos / span of the way from the first
 * (melonDS, "Notes on the interpolator"). The hardware does not divide each
 * attribute by W. It works out one perspective-correct weight,
 *
 *   factor = pos * w0 / (pos * w0 + (span - pos) * w1)
 *
 * with 9 fraction bits along an edge and 8 along a scanline, and blends
 * every attribute linearly with it. When both W are the same and their low
 * bits are clear it interpolates linearly and exactly instead, which is the
 * case that matters for 2D drawn with the 3D engine. Depth has its own
 * rule: with W-buffering it follows the weight, with Z-buffering it is
 * linear through a 22-bit reciprocal of the span.
 */
/*
 * A picture drawn larger than the screen (`fine`) keeps the scheme and
 * carries more bits: 16 in the weight either way, where 8 would give a
 * scanline 256 steps of texture however many pixels it has, and depth under
 * Z-buffering is linear without the console's cut-down range.
 */
typedef struct Interp {
    int32_t pos, span;
    uint32_t factor;
    int32_t recip;   /* (1 << 22) / span */
    unsigned shift;  /* fraction bits of factor: 9 or 8 on the console */
    bool edge;       /* along an edge, not a scanline */
    bool fine;
    bool linear;
    bool w_buffer;
} Interp;

static inline int32_t interp(const Interp *in, int32_t a0, int32_t a1)
{
    if (in->span == 0 || a0 == a1) return a0;
    if (!in->linear) {
        if (a0 < a1) return a0 + (int32_t)(((uint64_t)(uint32_t)(a1 - a0) * in->factor) >> in->shift);
        return a1 + (int32_t)(((uint64_t)(uint32_t)(a0 - a1) * ((1u << in->shift) - in->factor)) >> in->shift);
    }
    if (a0 < a1) return a0 + (int32_t)((int64_t)(a1 - a0) * in->pos / in->span);
    return a1 + (int32_t)((int64_t)(a0 - a1) * (in->span - in->pos) / in->span);
}

static inline int32_t interp_depth(const Interp *in, int32_t z0, int32_t z1)
{
    if (in->span == 0 || z0 == z1) return z0;
    if (in->w_buffer) {
        if (z0 < z1) return z0 + (int32_t)(((int64_t)(z1 - z0) * in->factor) >> in->shift);
        return z1 + (int32_t)(((int64_t)(z0 - z1) * ((1u << in->shift) - in->factor)) >> in->shift);
    }
    int32_t base = z0 < z1 ? z0 : z1;
    int32_t range = z0 < z1 ? z1 - z0 : z0 - z1;
    int32_t part = z0 < z1 ? in->pos : in->span - in->pos;
    if (in->fine) return base + (int32_t)((int64_t)range * part / in->span);
    if (in->edge) {
        /* along an edge: the range is first cut to 10 bits */
        unsigned dropped = 0;
        while (range > 0x3FF) {
            range >>= 1;
            dropped++;
        }
        return base + (int32_t)((((int64_t)range * part * in->recip) >> 22) << dropped);
    }
    /* along a scanline: the range loses its low 9 bits */
    return base + (int32_t)(((int64_t)(range >> 9) * part * in->recip) >> 13);
}

/* The weight for a position along an edge. The W values lose their lowest
 * bit here, with the first rounded up in the denominator when only it is
 * odd. */
static void interp_edge(Interp *in, const Render *r, int32_t pos, int32_t span, uint32_t w0, uint32_t w1,
                        bool w_buffer)
{
    in->pos = pos;
    in->span = span;
    in->shift = r->edge_shift;
    in->edge = true;
    in->fine = r->fine;
    in->w_buffer = w_buffer;
    in->recip = span ? (1 << 22) / span : 0;
    in->linear = w0 == w1 && !(w0 & 0x7Eu);
    in->factor = 0;
    if (span != 0 && (!in->linear || w_buffer)) {
        uint32_t near = w0 >> 1, near_d = (w0 + ((w0 & ~w1) & 1u)) >> 1, far_d = w1 >> 1;
        /* On the console (9 bits, a span of at most 192) these fit 32 bits. */
        uint64_t numerator = ((uint64_t)(uint32_t)pos * near) << in->shift;
        uint64_t denominator = (uint64_t)(uint32_t)pos * near_d + (uint64_t)(uint32_t)(span - pos) * far_d;
        in->factor = denominator ? (uint32_t)(numerator / denominator) : 0;
    }
}

/* The weight for a position along a scanline, between the W values the two
 * sides have there. */
static inline void interp_across(Interp *in, int32_t pos, uint32_t w_left, uint32_t w_right)
{
    /* On the console (8 bits, a position below 256) these fit 32 bits. */
    uint64_t numerator = ((uint64_t)(uint32_t)pos * w_left) << in->shift;
    uint64_t denominator = (uint64_t)(uint32_t)pos * w_left + (uint64_t)(uint32_t)(in->span - pos) * w_right;
    in->pos = pos;
    in->factor = denominator ? (uint32_t)(numerator / denominator) : 0;
}

/* ------------------------------------------------------------------ */
/* One side of a polygon on one scanline                               */
/* ------------------------------------------------------------------ */

typedef struct Side {
    unsigned from, to;      /* the edge: which of the polygon's vertices it runs between */
    int32_t lo, hi;         /* its pixels on this line */
    int32_t slope;          /* |dx / dy| with 18 fraction bits; 0 for a vertical edge, exactly 1.0 for a diagonal */
    bool negative, x_major;
    int32_t x_length, y_length;
    int32_t travelled;      /* how far the edge has moved in x, for anti-aliasing */
    Interp along;
    int32_t w, z;           /* at this line */
} Side;

/* A polygon's edges are walked from its top vertex, down one side and down
 * the other. The left side takes the vertices in the order they were sent
 * when the polygon faces the viewer, in reverse when it does not (melonDS).
 * A side moves on to its next edge when the line reaches that edge's start,
 * past any edge with no height, until it stands at the bottom vertex. */
static void find_edge(const NdsGxPolygon *p, const Shape *shape, int y, bool forward, Side *side)
{
    unsigned count = p->count;
    unsigned from = shape->top;
    unsigned to = forward ? (from + 1u) % count : (from + count - 1u) % count;
    while (y >= shape->y[to] && from != shape->bottom) {
        from = to;
        to = forward ? (from + 1u) % count : (from + count - 1u) % count;
    }
    side->from = from;
    side->to = to;
}

/* Everything about an edge at line y. `right` is the side of the polygon
 * the edge is walked as, which is not always the side it is drawn on. */
static void setup_side(Side *s, const Render *r, const NdsGxPolygon *p, const Shape *shape, int y, bool right,
                       bool w_buffer)
{
    int32_t x0 = shape->x[s->from], y0 = shape->y[s->from], x1 = shape->x[s->to], y1 = shape->y[s->to];
    int32_t dy = y1 - y0;

    s->negative = x1 < x0;
    s->x_length = x1 == x0 ? 1 : (s->negative ? x0 - x1 : x1 - x0);
    s->y_length = dy;
    int32_t dx = s->negative ? x0 - x1 : x1 - x0;
    if (dy <= 0 || y < y0) {
        /* Not a real edge: a side that has run out. Stay on its vertex. */
        s->slope = 0;
        s->x_major = false;
        s->negative = false;
        s->lo = s->hi = x0;
        s->travelled = 0;
        interp_edge(&s->along, r, 0, 0, p->w[s->from], p->w[s->from], w_buffer);
        s->w = p->w[s->from];
        s->z = (int32_t)p->depth[s->from];
        return;
    }
    s->slope = dx == dy ? ONE : dx * (ONE / dy);
    s->x_major = dx > dy;
    if (r->fine) slope_span_fine(x0, y0, x1, y1, y, &s->lo, &s->hi);
    else slope_span(x0, y0, x1, y1, y, &s->lo, &s->hi);

    /* How far along its run of pixels an edge is, as anti-aliasing counts
     * it (melonDS): an x-major edge starts half a pixel in, and an edge is
     * measured to the end of its run on the side it leans to. */
    int32_t start;
    if (s->x_major) {
        if (right) start = s->negative ? ONE + ONE / 2 : s->slope - ONE / 2;
        else start = s->negative ? s->slope + ONE / 2 : ONE / 2;
    } else {
        start = s->slope != 0 && s->negative ? ONE : 0;
    }
    s->travelled = start + (y - y0) * s->slope;

    /* Attributes along a diagonal or x-major edge are taken one line
     * further on when the edge leans away from the polygon (melonDS). */
    int32_t ahead = s->slope >= ONE && (right != s->negative) ? 1 : 0;
    interp_edge(&s->along, r, y - y0 + ahead, dy, p->w[s->from], p->w[s->to], w_buffer);
    s->w = interp(&s->along, p->w[s->from], p->w[s->to]);
    s->z = interp_depth(&s->along, (int32_t)p->depth[s->from], (int32_t)p->depth[s->to]);
}

/* ------------------------------------------------------------------ */
/* Textures                                                            */
/* ------------------------------------------------------------------ */

/* One texture coordinate to a texel column or row: TEXIMAGE_PARAM's repeat
 * and flip bits ("Flip each 2nd Texture (requires Repeat)"), or clamping. */
static inline int32_t wrap(int32_t c, int32_t size, bool repeat, bool flip)
{
    if (repeat) {
        if (flip && (c & size)) return (size - 1) - (c & (size - 1));
        return c & (size - 1);
    }
    if (c < 0) return 0;
    return c >= size ? size - 1 : c;
}

/* A texel: BGR555 in *color and a 5-bit alpha in *alpha (GBATEK "DS 3D
 * Texture Formats"). s and t are in texels. */
static void texel(const Render *r, uint32_t param, uint32_t palette, int32_t s, int32_t t, uint32_t *color,
                  uint32_t *alpha)
{
    uint32_t addr = (param & 0xFFFFu) << 3;
    int32_t width = 8 << ((param >> 20) & 7u), height = 8 << ((param >> 23) & 7u);
    s = wrap(s, width, (param & 0x10000u) != 0, (param & 0x40000u) != 0);
    t = wrap(t, height, (param & 0x20000u) != 0, (param & 0x80000u) != 0);
    uint32_t index = (uint32_t)(t * width + s);
    /* Bit 29: "Color 0 of 4/16/256-Color Palettes (0=Displayed, 1=Made
     * Transparent)". */
    uint32_t alpha0 = (param & 0x20000000u) ? 0u : 31u;
    uint32_t pixel;

    switch ((param >> 26) & 7u) {
    case 1: /* A3I5: "Alpha=(Alpha*4)+(Alpha/2)" */
        pixel = texture_byte(r, addr + index);
        *color = palette_word(r, (palette << 4) + ((pixel & 31u) << 1));
        *alpha = ((pixel >> 5) << 2) + (pixel >> 6);
        break;
    case 2: /* 4 colours, 2 bits each; the palette base is in 8-byte steps */
        pixel = (texture_byte(r, addr + (index >> 2)) >> ((index & 3u) << 1)) & 3u;
        *color = palette_word(r, (palette << 3) + (pixel << 1));
        *alpha = pixel ? 31u : alpha0;
        break;
    case 3: /* 16 colours */
        pixel = (texture_byte(r, addr + (index >> 1)) >> ((index & 1u) << 2)) & 15u;
        *color = palette_word(r, (palette << 4) + (pixel << 1));
        *alpha = pixel ? 31u : alpha0;
        break;
    case 4: /* 256 colours */
        pixel = texture_byte(r, addr + index);
        *color = palette_word(r, (palette << 4) + (pixel << 1));
        *alpha = pixel ? 31u : alpha0;
        break;
    case 5: { /* 4x4 texel blocks: 2 bits per texel in slot 0 or 2, a palette word per block in slot 1 */
        addr = (addr + (uint32_t)((t & 0x3FC) * (width >> 2) + (s & 0x3FC) + (t & 3))) & 0x7FFFFu;
        /* "slot1_addr = slot0_addr / 2", "slot2_addr / 2 + 10000h". Texels
         * addressed in slot 1 itself read as 0 (melonDS). */
        uint32_t info_addr = 0x20000u + ((addr & 0x1FFFCu) >> 1) + (addr >= 0x40000u ? 0x10000u : 0u);
        pixel = addr >= 0x20000u && addr < 0x40000u ? 0u : (texture_byte(r, addr) >> ((s & 3) << 1)) & 3u;
        uint32_t info = texture_word(r, info_addr);
        uint32_t entry = (palette << 4) + ((info & 0x3FFFu) << 2);
        unsigned mode = info >> 14;
        *alpha = 31;
        if (pixel < 2 || (pixel == 2 && !(mode & 1u)) || (pixel == 3 && mode == 2)) {
            *color = palette_word(r, entry + (pixel << 1));
        } else if (mode & 1u && (pixel == 2 || mode == 3)) {
            /* Mode 1: texel 2 is (Color0+Color1)/2. Mode 3: texels 2 and 3
             * are (Color0*5+Color1*3)/8 and (Color0*3+Color1*5)/8. */
            uint32_t c0 = palette_word(r, entry), c1 = palette_word(r, entry + 2u);
            uint32_t f0 = mode == 1 ? 4u : (pixel == 2 ? 5u : 3u), f1 = 8u - f0;
            *color = 0;
            for (unsigned shift = 0; shift < 15; shift += 5)
                *color |= ((((c0 >> shift) & 31u) * f0 + ((c1 >> shift) & 31u) * f1) >> 3) << shift;
        } else {
            *color = 0; /* texel 3 in modes 0 and 1: transparent */
            *alpha = 0;
        }
        break;
    }
    case 6: /* A5I3 */
        pixel = texture_byte(r, addr + index);
        *color = palette_word(r, (palette << 4) + ((pixel & 7u) << 1));
        *alpha = pixel >> 3;
        break;
    case 7: /* direct colour; bit 15 is the alpha */
        *color = texture_word(r, addr + (index << 1));
        *alpha = (*color & 0x8000u) ? 31u : 0u;
        break;
    default:
        *color = 0;
        *alpha = 0;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* A polygon on a scanline                                             */
/* ------------------------------------------------------------------ */

typedef struct Scan {
    const NdsGxPolygon *polygon;
    int y;
    int32_t x_start, x_end;     /* the polygon's pixels on this line */
    Interp across;              /* its pos and factor are set per pixel */
    uint32_t w_left, w_right;
    int32_t z_left, z_right;
    int32_t color_left[3], color_right[3]; /* 9 bits a channel */
    int32_t tex_left[2], tex_right[2];
    /* from the polygon's attributes */
    uint32_t pixel_attr;        /* what an opaque pixel of it puts in the attribute buffer, less the edge bits */
    unsigned alpha, mode;
    bool wireframe, textured, shadow, new_depth, depth_equal, front, w_buffer;
} Scan;

/* Depth test (GBATEK "Depth Test"; POLYGON_ATTR bit 14). "Equal" has a
 * tolerance: +-0x200 with Z-buffering, +-0xFF with W-buffering (melonDS;
 * GBATEK has 0x200 for both). "Less" becomes "less or equal" for a pixel of
 * a front-facing polygon over an opaque pixel of a back-facing one
 * (melonDS). */
static inline bool depth_test(const Scan *q, int32_t z, uint32_t behind_depth, uint32_t behind_attr)
{
    int32_t behind = (int32_t)behind_depth;
    if (q->depth_equal) {
        int32_t margin = q->w_buffer ? 0xFF : 0x200;
        return z >= behind - margin && z <= behind + margin;
    }
    if (q->front && (behind_attr & (ATTR_TRANSLUCENT | ATTR_BACK)) == ATTR_BACK) return z <= behind;
    return z < behind;
}

/*
 * Shared edges under edge marking (polyrastertest scenes 38-44, "The Curse
 * of Edge Marking"; the hardware's reasons are not known).
 *
 * With the normal fill rules two polygons that share an edge never both
 * draw it: the edge's pixels belong to the polygon below it or to its
 * right. Edge marking makes every polygon draw all its edges, and then the
 * hardware settles the shared pixels the same way without regard to depth:
 * a pixel of a polygon's upper edge (or of its first line) is drawn over a
 * pixel of another polygon's lower edge (or of the inside of its last
 * line), and a left edge pixel over a right edge pixel, provided both
 * polygons have the same ID and are opaque. In scene 43 a red polygon far
 * behind a white one shows through the white one's last line exactly where
 * the red one's upper edge runs. The scenes only have the lower or right
 * polygon drawn first; what the opposite order does is not known, and is
 * left to the depth test. The polygon IDs being different, anti-aliasing
 * being on, or the new pixel being translucent each turn this off (the
 * scenes' author, in the comments of the later tests.h).
 */
static inline bool takes_over(uint32_t mine, uint32_t theirs)
{
    if ((theirs & ATTR_TRANSLUCENT) || ((mine ^ theirs) & ATTR_OPAQUE_ID)) return false;
    return ((mine & ATTR_SIDE_UPPER) && (theirs & ATTR_SIDE_LOWER)) ||
           ((mine & ATTR_SIDE_LEFT) && (theirs & ATTR_SIDE_RIGHT));
}

/* The colour and alpha of one pixel of the polygon (GBATEK "DS 3D Texture
 * Blending"). vr, vg, vb: the vertex colour, 6 bits a channel; s, t: the
 * texture coordinates. */
static uint32_t shade(const Render *r, const Scan *q, uint32_t vr, uint32_t vg, uint32_t vb, int32_t s, int32_t t)
{
    uint32_t red, green, blue, alpha = q->alpha;
    uint32_t toon = r->toon[vr >> 1];

    /* Polygon mode 2: "the red component of the Vertex Color is mis-used
     * as an index in the Shading Table". Toon shading uses the table's
     * colour in place of the vertex colour; highlight shading uses the red
     * component for all three and adds the table's colour afterwards. */
    if (q->mode == 2) {
        if (r->highlight) {
            vg = vr;
            vb = vr;
        } else {
            vr = RED(toon);
            vg = GREEN(toon);
            vb = BLUE(toon);
        }
    }

    if (q->textured) {
        uint32_t color, texel_alpha;
        texel(r, q->polygon->tex_param, q->polygon->palette, s >> 4, t >> 4, &color, &texel_alpha);
        uint32_t tr = expand5(color & 31u), tg = expand5((color >> 5) & 31u), tb = expand5((color >> 10) & 31u);
        if (q->mode & 1u) {
            /* Decal (and shadow polygons): the texel's alpha mixes texel
             * and vertex colour; the polygon's alpha is the pixel's. With
             * 5-bit weights, as melonDS has it. */
            if (texel_alpha == 0) {
                red = vr;
                green = vg;
                blue = vb;
            } else if (texel_alpha == 31) {
                red = tr;
                green = tg;
                blue = tb;
            } else {
                red = (tr * texel_alpha + vr * (31u - texel_alpha)) >> 5;
                green = (tg * texel_alpha + vg * (31u - texel_alpha)) >> 5;
                blue = (tb * texel_alpha + vb * (31u - texel_alpha)) >> 5;
            }
        } else {
            /* Modulation: "R = ((Rt+1)*(Rv+1)-1)/64", and the same for
             * alpha in 5 bits. */
            red = ((tr + 1u) * (vr + 1u) - 1u) >> 6;
            green = ((tg + 1u) * (vg + 1u) - 1u) >> 6;
            blue = ((tb + 1u) * (vb + 1u) - 1u) >> 6;
            alpha = ((texel_alpha + 1u) * (alpha + 1u) - 1u) >> 5;
        }
    } else {
        red = vr;
        green = vg;
        blue = vb;
    }

    if (q->mode == 2 && r->highlight) {
        red += RED(toon);
        green += GREEN(toon);
        blue += BLUE(toon);
        if (red > 63) red = 63;
        if (green > 63) green = 63;
        if (blue > 63) blue = 63;
    }
    /* "only the lines at the polygon edges are rendered, using a fixed
     * Alpha value of 31" */
    if (q->wireframe) alpha = 31;
    return COLOR(red, green, blue, alpha);
}

/* Blend a translucent pixel into one layer (GBATEK "Alpha-Blending (Polygon
 * vs FrameBuffer)"):
 *
 *   FrameBuf[R] = (Poly[R]*(Poly[A]+1) + FrameBuf[R]*(31-(Poly[A])) / 32
 *   FrameBuf[A] = max(Poly[A],FrameBuf[A])
 *
 * bypassed, keeping the polygon's colour, when blending is off in DISP3DCNT
 * or the pixel behind is fully transparent. A translucent pixel is not
 * drawn over one already drawn with the same translucent polygon ID; its
 * fog flag is ANDed with the one there ("DS 3D Shadow Polygons", "Polygon
 * ID and Fog Enable"); it sets the depth only if POLYGON_ATTR bit 11 says
 * so. A shadow is not drawn over a pixel of its own ID either, opaque or
 * translucent. */
static void blend(const Render *r, const Scan *q, uint32_t source, int32_t z, uint32_t *color, uint32_t *depth,
                  uint32_t *attr)
{
    uint32_t behind = *attr;
    uint32_t id = q->polygon->attr & ATTR_TRANSLUCENT_ID;
    if (behind & ATTR_TRANSLUCENT) {
        if ((behind & ATTR_TRANSLUCENT_ID) == id) return;
    } else if (q->shadow) {
        if (((behind & ATTR_OPAQUE_ID) >> ATTR_OPAQUE_ID_SHIFT) == (id >> 24)) return;
    }

    uint32_t old = *color, alpha = ALPHA(source);
    uint32_t result = source;
    if (ALPHA(old) != 0) {
        uint32_t red = RED(source), green = GREEN(source), blue = BLUE(source);
        if (r->blending) {
            red = (red * (alpha + 1u) + RED(old) * (31u - alpha)) >> 5;
            green = (green * (alpha + 1u) + GREEN(old) * (31u - alpha)) >> 5;
            blue = (blue * (alpha + 1u) + BLUE(old) * (31u - alpha)) >> 5;
        }
        result = COLOR(red, green, blue, alpha > ALPHA(old) ? alpha : ALPHA(old));
    }
    *color = result;
    if (q->new_depth) *depth = (uint32_t)z;

    uint32_t now = (behind & (ATTR_EDGE | ATTR_COVERAGE | ATTR_OPAQUE_ID)) | ATTR_TRANSLUCENT | id;
    if (!q->front) now |= ATTR_BACK;
    if ((q->pixel_attr & ATTR_FOG) && (behind & ATTR_FOG)) now |= ATTR_FOG;
    *attr = now;
}

/* How much of a pixel an edge covers, for anti-aliasing: a fixed 5-bit
 * value for a steep edge, or for an x-major one a 10-bit value that changes
 * by `step` with each pixel of the edge's run (melonDS). */
typedef struct Coverage {
    bool ramp;
    int32_t value, step;
} Coverage;

static void edge_coverage(const Side *s, bool right, bool swapped, int32_t length, Coverage *c)
{
    c->ramp = false;
    c->step = 0;
    if (s->y_length <= 0) {
        c->value = swapped ? 0 : 31;
    } else if (s->x_major) {
        int32_t run = s->travelled >> 18;
        if (s->negative) run = s->x_length - run;
        if (right) run = run - length + 1;
        c->ramp = true;
        c->value = (int32_t)((uint32_t)((run * 1024 + 0x1FF) * s->y_length / s->x_length) & 0x3FFu);
        if (c->value == 0x3FF) c->value = 0;
        c->step = (s->y_length * 1024 / s->x_length) & 0x3FF;
    } else if (s->slope == 0) {
        /* "for some reason vertical edges' aa values are inverted too when
         * the edges are swapped" (melonDS; polyrastertest's "Swapped
         * Vertical Edge Glitch" scenes) */
        c->value = swapped ? 0 : 31;
    } else {
        int32_t cover = ((s->travelled >> 9) + (s->slope >> 10)) >> 4;
        if ((cover >> 5) != (s->travelled >> 18)) cover = 31;
        cover &= 31;
        if ((right != s->negative) == swapped) cover = 31 - cover;
        c->value = cover;
    }
}

/* Draw the polygon's pixels from column `from` up to but not including
 * `to`. `edge` is which of the polygon's edges they are part of;
 * `coverage` and `right` describe that edge for anti-aliasing. */
static void draw_pixels(Render *r, Scan *q, int32_t from, int32_t to, uint32_t edge, const Coverage *coverage,
                        bool right)
{
    int y = q->y;
    uint32_t *top_color = color_row(r, y);
    uint32_t *top_depth = &r->depth[y & 3][1], *top_attr = &r->attr[y & 3][1];
    uint32_t *under_color = r->under_color[y & 1], *under_depth = r->under_depth[y & 1];
    uint32_t *under_attr = r->under_attr[y & 1];
    const uint8_t *stencil = r->stencil[y & 1];
    Interp *in = &q->across;
    /* The weight along the line is a division a pixel. Depth needs it only
     * under W-buffering; colour and texture need it unless the line is
     * linear, and then only for a pixel that passes the depth test. */
    bool weighted = in->span != 0 && (!in->linear || in->w_buffer);
    bool weight_first = weighted && in->w_buffer, weight_late = weighted && !in->w_buffer;
    int32_t cover = coverage ? coverage->value : 0;

    for (int32_t x = from; x < to; x++) {
        bool under = false;         /* drawing to the pixel underneath */
        bool has_under = true;      /* the visible pixel is an edge with a pixel under it */
        if (q->shadow) {
            /* "drawn ... only if the Stencil Buffer bits are zero" in
             * GBATEK's telling, where the mask sets bits by being drawn;
             * here the mask's bits mark where it was hidden (melonDS). */
            unsigned bits = stencil[x];
            if (!bits) continue;
            if (!(bits & 1u)) under = true;
            if (!(bits & 2u)) has_under = false;
        }

        in->pos = x - q->x_start;
        if (weight_first) interp_across(in, in->pos, q->w_left, q->w_right);
        int32_t z = interp_depth(in, q->z_left, q->z_right);

        /* The pixel's own coverage is consumed whether or not it is drawn. */
        int32_t covered = 31;
        if (coverage) {
            if (coverage->ramp) {
                covered = right ? 31 - (cover >> 5) : cover >> 5;
                if (covered > 31) covered = 31;
                if (covered < 0) covered = 0;
                cover += coverage->step;
            } else {
                covered = coverage->value;
            }
        }

        uint32_t behind = under ? under_attr[x] : top_attr[x];
        if (!has_under) behind &= ~ATTR_EDGE;
        bool taking_over = false;
        if (!depth_test(q, z, under ? under_depth[x] : top_depth[x], behind)) {
            if (!under && r->shared_edges && takes_over(q->pixel_attr | edge, behind)) {
                /* The pixel is the other polygon's along a shared edge. */
                taking_over = true;
            } else {
                /* A pixel hidden by an edge pixel may still show through
                 * it: it is tested against the pixel under the edge, and
                 * drawn there. */
                if (under || !(behind & ATTR_EDGE)) continue;
                under = true;
                behind = under_attr[x];
                if (!depth_test(q, z, under_depth[x], behind)) continue;
            }
        }

        if (weight_late) interp_across(in, in->pos, q->w_left, q->w_right);
        uint32_t vr = (uint32_t)interp(in, q->color_left[0], q->color_right[0]) >> 3;
        uint32_t vg = (uint32_t)interp(in, q->color_left[1], q->color_right[1]) >> 3;
        uint32_t vb = (uint32_t)interp(in, q->color_left[2], q->color_right[2]) >> 3;
        int32_t s = 0, t = 0;
        if (q->textured) {
            s = (int16_t)interp(in, q->tex_left[0], q->tex_right[0]);
            t = (int16_t)interp(in, q->tex_left[1], q->tex_right[1]);
        }
        uint32_t color = shade(r, q, vr, vg, vb, s, t);
        uint32_t alpha = ALPHA(color);
        /* "pixels are rendered only if their Alpha value is GREATER than
         * ALPHA_TEST_REF", or than zero with the test off. */
        if (alpha <= r->alpha_ref) continue;

        if (alpha == 31) {
            uint32_t attr = q->pixel_attr | edge;
            if (r->antialias && (edge & ATTR_EDGE)) {
                attr |= (uint32_t)covered << ATTR_COVERAGE_SHIFT;
                /* An edge pixel keeps what it covers underneath it. */
                if (!under) {
                    under_color[x] = top_color[x];
                    under_depth[x] = top_depth[x];
                    under_attr[x] = top_attr[x];
                }
            }
            if (under) {
                under_color[x] = color;
                under_depth[x] = (uint32_t)z;
                under_attr[x] = attr;
            } else {
                top_color[x] = color;
                top_depth[x] = (uint32_t)z;
                top_attr[x] = attr;
            }
        } else if (taking_over) {
            continue; /* only opaque pixels take over */
        } else if (under) {
            blend(r, q, color, z, &under_color[x], &under_depth[x], &under_attr[x]);
        } else {
            blend(r, q, color, z, &top_color[x], &top_depth[x], &top_attr[x]);
            /* Under an edge pixel, the pixel it covers is blended too. */
            if (behind & ATTR_EDGE) blend(r, q, color, z, &under_color[x], &under_depth[x], &under_attr[x]);
        }
    }
}

/* A shadow mask's pixels: nothing is drawn; the stencil gets a bit wherever
 * the mask is behind what is there (melonDS). */
static void mask_pixels(Render *r, Scan *q, int32_t from, int32_t to)
{
    int y = q->y;
    const uint32_t *top_depth = &r->depth[y & 3][1], *top_attr = &r->attr[y & 3][1];
    uint8_t *stencil = r->stencil[y & 1];
    Interp *in = &q->across;
    /* Only depth is wanted here, and it follows the weight only under
     * W-buffering. */
    bool weighted = in->span != 0 && in->w_buffer;

    for (int32_t x = from; x < to; x++) {
        in->pos = x - q->x_start;
        if (weighted) interp_across(in, in->pos, q->w_left, q->w_right);
        int32_t z = interp_depth(in, q->z_left, q->z_right);
        if (!depth_test(q, z, top_depth[x], top_attr[x])) stencil[x] = 1;
        if ((top_attr[x] & ATTR_EDGE) && !depth_test(q, z, r->under_depth[y & 1][x], r->under_attr[y & 1][x]))
            stencil[x] |= 2;
    }
}

/* The 9-bit colour a vertex is drawn with: 5 bits with the low four set,
 * unless it is zero (melonDS). */
static inline int32_t vertex_color(const NdsGxVertex *v, int channel)
{
    return v->color[channel] ? (v->color[channel] << 4) + 15 : 0;
}

/* Draw one polygon's part of line y. */
static void draw_polygon_line(Render *r, const NdsGxPolygon *p, const Shape *shape, int y)
{
    const NdsGxVertex *v[NDS_GX_POLYGON_VERTICES];
    for (unsigned i = 0; i < p->count; i++) v[i] = &r->vertices[p->vertex[i]];

    bool front = (p->flags & NDS_GX_POLY_FRONT) != 0;
    bool w_buffer = (p->flags & NDS_GX_POLY_W_BUFFER) != 0;
    bool flat = shape->y_top == shape->y_bottom;
    Side left, right;
    memset(&left, 0, sizeof left);
    memset(&right, 0, sizeof right);

    if (flat) {
        /* A polygon with no height is one line, between the leftmost and
         * rightmost of its first, second and last vertices: the hardware
         * does not look at the others (melonDS; polyrastertest's
         * "Horizontal Line Polygons" scenes). */
        unsigned lo = 0, hi = 0;
        unsigned candidates[2] = {1, p->count - 1u};
        for (unsigned i = 0; i < 2; i++) {
            unsigned c = candidates[i];
            if (shape->x[c] < shape->x[lo]) lo = c;
            if (shape->x[c] > shape->x[hi]) hi = c;
        }
        left.from = left.to = lo;
        right.from = right.to = hi;
    } else {
        find_edge(p, shape, y, front, &left);
        find_edge(p, shape, y, !front, &right);
    }
    setup_side(&left, r, p, shape, y, false, w_buffer);
    setup_side(&right, r, p, shape, y, true, w_buffer);

    int32_t x_start = left.lo, x_end = right.hi;
    /* A vertical right edge is one pixel to the left of where its vertices
     * are, unless the polygon is no wider than that pixel and its left edge
     * is vertical too, or it stands at the screen's left edge
     * (polyrastertest's "Vertical Right Edge Shift" scenes). */
    if (right.slope == 0 && (left.slope != 0 || x_start != x_end) && x_end != 0) x_end--;

    /* When the sides cross, the polygon is drawn between them the other way
     * round, and the hardware's idea of an edge's run of pixels breaks
     * down: each edge counts as one pixel. */
    bool swapped = x_start > x_end;
    const Side *l = &left, *rt = &right;
    int32_t left_length, right_length;
    if (swapped) {
        l = &right;
        rt = &left;
        int32_t exchange = x_start;
        x_start = x_end;
        x_end = exchange;
        left_length = 1;
        right_length = 1;
    } else {
        left_length = left.hi - left.lo + 1;
        right_length = right.hi - right.lo + 1;
    }

    uint32_t poly_alpha = (p->attr >> 16) & 31u;
    bool wireframe = poly_alpha == 0;
    bool last_line = y == shape->y_bottom - 1;
    /* The x-major edges of a polygon that ends in a flat bottom are filled
     * on its last line (polyrastertest's "Trapezoids"). */
    bool flat_bottom = last_line && shape->x[l->to] != shape->x[rt->to];

    /*
     * Fill rules: which of the two edges' pixels are drawn (polyrastertest,
     * "Fill Rules"). Edges belong to the polygon on their right or below:
     * a left edge is drawn unless it is x-major going right, and a right
     * edge only if it is x-major going right, or vertical. Everything is
     * drawn when anti-aliasing or edge marking is on, for translucent
     * polygons while blending is on, and for wireframes.
     */
    bool fill_left, fill_right;
    if (r->antialias || r->edge_marking || wireframe || (poly_alpha < 31 && r->blending)) {
        fill_left = true;
        fill_right = true;
    } else if (swapped) {
        fill_left = l->negative || !l->x_major || (flat_bottom && l->x_major);
        fill_right = (!rt->negative && rt->x_major) || (!(rt->negative && rt->x_major) && l->slope == 0) ||
                     (flat_bottom && rt->x_major);
    } else {
        /* A line: both sides are on the same edge, between the same two
         * points. Its pixels are drawn although they are all part of the
         * left edge's run. Two different edges that happen to cover the same
         * pixels are not a line (polyrastertest scenes 24-29, "Line Polygon
         * Exception"). */
        bool line = shape->x[l->from] == shape->x[rt->from] && shape->y[l->from] == shape->y[rt->from] &&
                    shape->x[l->to] == shape->x[rt->to] && shape->y[l->to] == shape->y[rt->to];
        fill_left = l->negative || !l->x_major || (flat_bottom && l->x_major) || line;
        fill_right = (!rt->negative && rt->x_major) || rt->slope == 0 || (flat_bottom && rt->x_major);
    }

    Scan q;
    memset(&q, 0, sizeof q);
    q.polygon = p;
    q.y = y;
    q.x_start = x_start;
    q.x_end = x_end;
    q.alpha = poly_alpha;
    q.mode = (p->attr >> 4) & 3u;
    q.wireframe = wireframe;
    q.textured = r->texturing && ((p->tex_param >> 26) & 7u) != 0;
    q.shadow = (p->flags & NDS_GX_POLY_SHADOW) != 0;
    q.new_depth = (p->attr & 0x800u) != 0;
    q.depth_equal = (p->attr & 0x4000u) != 0;
    q.front = front;
    q.w_buffer = w_buffer;
    q.pixel_attr = ((p->attr >> 24) & 63u) << ATTR_OPAQUE_ID_SHIFT;
    if (p->attr & 0x8000u) q.pixel_attr |= ATTR_FOG;
    if (!front) q.pixel_attr |= ATTR_BACK;

    q.w_left = (uint32_t)l->w;
    q.w_right = (uint32_t)rt->w;
    q.z_left = l->z;
    q.z_right = rt->z;
    for (int c = 0; c < 3; c++) {
        q.color_left[c] = interp(&l->along, vertex_color(v[l->from], c), vertex_color(v[l->to], c));
        q.color_right[c] = interp(&rt->along, vertex_color(v[rt->from], c), vertex_color(v[rt->to], c));
    }
    for (int c = 0; c < 2; c++) {
        q.tex_left[c] = interp(&l->along, v[l->from]->tex[c], v[l->to]->tex[c]);
        q.tex_right[c] = interp(&rt->along, v[rt->from]->tex[c], v[rt->to]->tex[c]);
    }
    q.across.span = x_end + 1 - x_start;
    q.across.shift = r->span_shift;
    q.across.fine = r->fine;
    q.across.w_buffer = w_buffer;
    q.across.recip = (1 << 22) / q.across.span;
    q.across.linear = q.w_left == q.w_right && !(q.w_left & 0x7Fu);

    /* Coverage is worked out for each edge as the side it was walked as,
     * and used on the side it is drawn on. */
    Coverage cover_walked[2];
    edge_coverage(&left, false, swapped, left.hi - left.lo + 1, &cover_walked[0]);
    edge_coverage(&right, true, swapped, right.hi - right.lo + 1, &cover_walked[1]);
    const Coverage *cover_left = &cover_walked[swapped ? 1 : 0], *cover_right = &cover_walked[swapped ? 0 : 1];

    uint32_t line_edge = y == shape->y_top ? ATTR_EDGE_TOP : (last_line ? ATTR_EDGE_BOTTOM : 0u);
    bool mask = (p->flags & NDS_GX_POLY_SHADOW_MASK) != 0;
    if (mask) {
        /* The stencil starts afresh with the first mask after anything
         * else. */
        if (!r->mask_before) memset(r->stencil[y & 1], 0, (size_t)r->width);
        /* The alpha test, on the polygon's alpha alone. */
        if ((wireframe ? 31u : poly_alpha) <= r->alpha_ref) {
            r->mask_before = true;
            return;
        }
    }
    r->mask_before = mask;

    /* Which side of the polygon each part is on: an x-major edge is its
     * upper or lower side, a steeper one its left or right. */
    uint32_t left_side = l->x_major ? (l->negative ? ATTR_SIDE_UPPER : ATTR_SIDE_LOWER) : ATTR_SIDE_LEFT;
    uint32_t right_side = rt->x_major ? (rt->negative ? ATTR_SIDE_LOWER : ATTR_SIDE_UPPER) : ATTR_SIDE_RIGHT;
    uint32_t inside_side = line_edge == ATTR_EDGE_TOP ? ATTR_SIDE_UPPER : (line_edge ? ATTR_SIDE_LOWER : 0u);

    /* The left edge's run, the inside, the right edge's run. */
    int32_t limit = x_end + 1 < r->width ? x_end + 1 : r->width;
    int32_t x = x_start;
    int32_t stop = x_start + left_length < limit ? x_start + left_length : limit;
    if (fill_left && x < stop) {
        if (mask) mask_pixels(r, &q, x, stop);
        else draw_pixels(r, &q, x, stop, line_edge | ATTR_EDGE_LEFT | left_side, cover_left, false);
    }
    if (x < stop) x = stop;

    stop = x_end - right_length + 1 < limit ? x_end - right_length + 1 : limit;
    /* "The interior of Wire-frame polygons is transparent": only their
     * first and last lines are drawn right across. */
    if (!(wireframe && !line_edge) && x < stop) {
        if (mask) mask_pixels(r, &q, x, stop);
        else draw_pixels(r, &q, x, stop, line_edge | inside_side, NULL, false);
    }
    if (x < stop) x = stop;

    if (fill_right && x < limit) {
        if (mask) mask_pixels(r, &q, x, limit);
        else draw_pixels(r, &q, x, limit, line_edge | ATTR_EDGE_RIGHT | right_side, cover_right, true);
    }
}

/* ------------------------------------------------------------------ */
/* The rear plane and the final pass                                   */
/* ------------------------------------------------------------------ */

/* "The 15bit Depth is expanded to 24bit as X=(X*200h)+((X+1)/8000h)*1FFh"
 * says GBATEK; hardware has the low 9 bits set for every value (melonDS). */
static inline uint32_t clear_depth(uint32_t value)
{
    return (value & 0x7FFFu) * 0x200u + 0x1FFu;
}

/* Start a line from the rear plane (GBATEK "DS 3D Rear-Plane"): one colour,
 * depth and fog flag for the whole screen, or with DISP3DCNT bit 14 a
 * 256x256 bitmap of colours in texture slot 2 and of depths in slot 3,
 * scrolled by CLRIMAGE_OFFSET. The polygon ID comes from CLEAR_COLOR either
 * way. */
static void clear_line(Render *r, int y)
{
    const int width = r->width;
    uint32_t *color = color_row(r, y);
    uint32_t *depth = r->depth[y & 3], *attr = r->attr[y & 3];
    depth[0] = depth[width + 1] = r->clear_depth;
    attr[0] = attr[width + 1] = r->clear_attr & ATTR_OPAQUE_ID;

    if (r->gx->latched_disp3dcnt & 0x4000u) {
        /* The bitmap has a pixel for each of the screen's: a larger picture
         * repeats them. */
        const uint8_t *io = r->gx->latched_io;
        const uint32_t scale = (uint32_t)r->scale;
        uint32_t x_offset = io[0x36], y_offset = (io[0x37] + (uint32_t)y / scale) & 0xFFu;
        for (int x = 0; x < width; x++) {
            uint32_t at = (y_offset << 9) + (((x_offset + (uint32_t)x / scale) & 0xFFu) << 1);
            uint32_t c = texture_word(r, 0x40000u + at), d = texture_word(r, 0x60000u + at);
            color[x] = color_from_555(c) | ((c & 0x8000u) ? 31u << 18 : 0u);
            depth[x + 1] = clear_depth(d);
            attr[x + 1] = (r->clear_attr & ATTR_OPAQUE_ID) | ((d & 0x8000u) ? ATTR_FOG : 0u);
        }
    } else {
        for (int x = 0; x < width; x++) {
            color[x] = r->clear_color;
            depth[x + 1] = r->clear_depth;
            attr[x + 1] = r->clear_attr;
        }
    }
    memcpy(r->under_color[y & 1], color, (size_t)width * sizeof(uint32_t));
    memcpy(r->under_depth[y & 1], &depth[1], (size_t)width * sizeof(uint32_t));
    memcpy(r->under_attr[y & 1], &attr[1], (size_t)width * sizeof(uint32_t));
}

/* Fog density at a depth (GBATEK FOG_OFFSET, FOG_TABLE): 32 densities at
 * depth boundaries FOG_OFFSET + FOG_STEP * (n + 1), interpolated between,
 * the first used up to the first boundary and the last past the last. In
 * 24-bit depth the offset is times 0x200 and a step is 0x80000 >> FOG_SHIFT
 * (melonDS); a big shift overflows 32 bits on hardware as it does here.
 * "Density=127 is handled as if Density=128." */
static uint32_t fog_density(const Render *r, uint32_t z)
{
    uint32_t index = 0, part = 0;
    if (z >= r->fog_offset) {
        z = ((z - r->fog_offset) >> 2) << r->fog_shift;
        index = z >> 17;
        part = z & 0x1FFFFu;
        if (index >= 32) {
            index = 32;
            part = 0;
        }
    }
    uint32_t density = (r->fog_density[index] * (0x20000u - part) + r->fog_density[index + 1] * part) >> 17;
    return density >= 127 ? 128 : density;
}

/* "FrameBuffer[R] = (FogColor[R]*Density + FrameBuffer[R]*(128-Density)) /
 * 128", and with DISP3DCNT bit 6 set only the alpha. */
static uint32_t fog_blend(const Render *r, uint32_t color, uint32_t z)
{
    uint32_t density = fog_density(r, z), keep = 128u - density;
    uint32_t red = RED(color), green = GREEN(color), blue = BLUE(color);
    if (!r->fog_alpha_only) {
        red = (RED(r->fog_color) * density + red * keep) >> 7;
        green = (GREEN(r->fog_color) * density + green * keep) >> 7;
        blue = (BLUE(r->fog_color) * density + blue * keep) >> 7;
    }
    return COLOR(red, green, blue, (ALPHA(r->fog_color) * density + ALPHA(color) * keep) >> 7);
}

/* The final pass over a line, once the line below it has been drawn. */
static void finish_line(Render *r, int y)
{
    const int width = r->width;
    uint32_t *color = color_row(r, y);
    uint32_t *depth = &r->depth[y & 3][1], *attr = &r->attr[y & 3][1];
    const uint32_t *under_color = r->under_color[y & 1], *under_depth = r->under_depth[y & 1];
    const uint32_t *under_attr = r->under_attr[y & 1];

    if (r->edge_marking) {
        /* GBATEK EDGE_COLOR: an edge pixel takes its polygon ID's edge
         * colour when "at least one of the four surrounding pixels (up,
         * down, left, right) [has a] different polygon_id ..., and, the edge
         * depth [is] LESS than the depth of that surrounding pixel". Past
         * the screen's borders is the rear plane. */
        const uint32_t *up_depth = y > 0 ? &r->depth[(y - 1) & 3][1] : &r->border_depth[1];
        const uint32_t *up_attr = y > 0 ? &r->attr[(y - 1) & 3][1] : &r->border_attr[1];
        const uint32_t *down_depth = y < r->height - 1 ? &r->depth[(y + 1) & 3][1] : &r->border_depth[1];
        const uint32_t *down_attr = y < r->height - 1 ? &r->attr[(y + 1) & 3][1] : &r->border_attr[1];
        for (int x = 0; x < width; x++) {
            if (!(attr[x] & ATTR_EDGE)) continue;
            uint32_t id = attr[x] & ATTR_OPAQUE_ID, z = depth[x];
            if (((attr[x - 1] & ATTR_OPAQUE_ID) != id && z < depth[x - 1]) ||
                ((attr[x + 1] & ATTR_OPAQUE_ID) != id && z < depth[x + 1]) ||
                ((up_attr[x] & ATTR_OPAQUE_ID) != id && z < up_depth[x]) ||
                ((down_attr[x] & ATTR_OPAQUE_ID) != id && z < down_depth[x])) {
                color[x] = (color[x] & (31u << 18)) | r->edge_color[id >> (ATTR_OPAQUE_ID_SHIFT + 3)];
                /* A marked edge is half covered as far as anti-aliasing
                 * goes (melonDS; GBATEK: "making the edges of Edge-Marked
                 * polygons translucent (with alpha=16 or so?)"). */
                attr[x] = (attr[x] & ~ATTR_COVERAGE) | (16u << ATTR_COVERAGE_SHIFT);
            }
        }
    }

    if (r->fog) {
        /* Fog goes on the pixels whose fog flag is set, and on the pixel
         * under an edge by its own flag and depth. */
        for (int x = 0; x < width; x++) {
            if (attr[x] & ATTR_FOG) color[x] = fog_blend(r, color[x], depth[x]);
        }
        if (r->antialias) {
            uint32_t *fogged = r->under_color[y & 1];
            for (int x = 0; x < width; x++) {
                if ((attr[x] & ATTR_EDGE) && (under_attr[x] & ATTR_FOG))
                    fogged[x] = fog_blend(r, fogged[x], under_depth[x]);
            }
        }
    }

    if (r->antialias) {
        /* GBATEK "Anti-Aliasing": "the edges of opaque polygons will be
         * anti-aliased". An edge pixel is mixed with the pixel under it by
         * its coverage; the colour only if there is something under it, the
         * alpha always (melonDS). */
        for (int x = 0; x < width; x++) {
            if (!(attr[x] & ATTR_EDGE)) continue;
            uint32_t cover = (attr[x] & ATTR_COVERAGE) >> ATTR_COVERAGE_SHIFT;
            if (cover == 31) continue;
            if (cover == 0) {
                color[x] = under_color[x];
                continue;
            }
            uint32_t top = color[x], bottom = under_color[x];
            uint32_t red = RED(top), green = GREEN(top), blue = BLUE(top);
            cover++;
            if (ALPHA(bottom) > 0) {
                red = (red * cover + RED(bottom) * (32u - cover)) >> 5;
                green = (green * cover + GREEN(bottom) * (32u - cover)) >> 5;
                blue = (blue * cover + BLUE(bottom) * (32u - cover)) >> 5;
            }
            color[x] = COLOR(red, green, blue, (ALPHA(top) * cover + ALPHA(bottom) * (32u - cover)) >> 5);
        }
    }
}

/* The registers as the last VBlank left them. The caller has said which
 * picture this is (width, height, scale, fine, frame) and given the rows. */
static void setup(Render *r, const NdsGx *gx, const NdsGxVram *vram)
{
    const uint8_t *io = gx->latched_io; /* 0x04000320 on */
    uint16_t control = gx->latched_disp3dcnt;

    r->gx = gx;
    r->vertices = gx->vertices[gx->geometry_bank ^ 1];
    r->polygons = gx->polygons[gx->geometry_bank ^ 1];
    if (vram) r->vram = *vram;
    r->edge_shift = r->fine ? 16u : 9u;
    r->span_shift = r->fine ? 16u : 8u;

    r->texturing = (control & 0x01u) != 0;
    r->highlight = (control & 0x02u) != 0;
    r->blending = (control & 0x08u) != 0;
    r->antialias = (control & 0x10u) != 0;
    r->edge_marking = (control & 0x20u) != 0;
    r->shared_edges = r->edge_marking && !r->antialias;
    r->fog_alpha_only = (control & 0x40u) != 0;
    r->fog = (control & 0x80u) != 0;
    r->fog_shift = (control >> 8) & 15u;
    /* ALPHA_TEST_REF counts only while DISP3DCNT bit 2 is set. */
    r->alpha_ref = (control & 0x04u) ? io[0x20] & 31u : 0u;

    for (unsigned i = 0; i < 8; i++) r->edge_color[i] = color_from_555(nds_get_le16(&io[0x10 + 2 * i]));
    for (unsigned i = 0; i < 32; i++) r->toon[i] = color_from_555(nds_get_le16(&io[0x60 + 2 * i]));

    uint32_t fog = nds_get_le32(&io[0x38]);
    r->fog_color = color_from_555(fog) | (((fog >> 16) & 31u) << 18);
    r->fog_offset = (nds_get_le16(&io[0x3C]) & 0x7FFFu) * 0x200u;
    r->fog_density[0] = io[0x40] & 0x7Fu;
    for (unsigned i = 0; i < 32; i++) r->fog_density[i + 1] = io[0x40 + i] & 0x7Fu;
    r->fog_density[33] = io[0x5F] & 0x7Fu;

    /* CLEAR_COLOR: colour, fog flag (bit 15), alpha (bits 16-20), polygon
     * ID (bits 24-29). */
    uint32_t clear = nds_get_le32(&io[0x30]);
    r->clear_color = color_from_555(clear) | (((clear >> 16) & 31u) << 18);
    r->clear_attr = (((clear >> 24) & 63u) << ATTR_OPAQUE_ID_SHIFT) | ((clear & 0x8000u) ? ATTR_FOG : 0u);
    r->clear_depth = clear_depth(nds_get_le16(&io[0x34]));
    for (int x = 0; x < r->width + 2; x++) {
        r->border_depth[x] = r->clear_depth;
        r->border_attr[x] = r->clear_attr & ATTR_OPAQUE_ID;
    }
}

/* A polygon as polygon list RAM and vertex RAM have it: the console's own
 * picture. */
static void shape_from_ram(const Render *r, const NdsGxPolygon *p, Shape *shape)
{
    for (unsigned i = 0; i < p->count; i++) {
        const NdsGxVertex *v = &r->vertices[p->vertex[i]];
        shape->x[i] = v->sx;
        shape->y[i] = v->sy;
    }
    shape->y_top = p->y_top;
    shape->y_bottom = p->y_bottom;
    shape->top = p->top;
    shape->bottom = p->bottom;
    shape->degenerate = (p->flags & NDS_GX_POLY_DEGENERATE) != 0;
}

/* The same polygon on a grid `scale` times as fine, its vertices where the
 * viewport division put them and not at the corner of their pixel. Top and
 * bottom are chosen again by the rules submit_polygon() has for the screen:
 * two vertices on one screen line need not be on one line here. */
static void shape_scaled(const Render *r, const NdsGxPolygon *p, Shape *shape)
{
    const int32_t scale = r->scale;
    int32_t y_top = INT32_MAX, y_bottom = 0, x_bottom = 0;
    shape->top = shape->bottom = 0;
    for (unsigned i = 0; i < p->count; i++) {
        const NdsGxVertex *v = &r->vertices[p->vertex[i]];
        const int32_t x = v->sx * scale + ((v->fx * scale) >> 8);
        const int32_t y = v->sy * scale + ((v->fy * scale) >> 8);
        shape->x[i] = x;
        shape->y[i] = y;
        if (y < y_top) {
            y_top = y;
            shape->top = (uint8_t)i;
        }
        if (y > y_bottom || (y == y_bottom && x > x_bottom)) {
            y_bottom = y;
            x_bottom = x;
            shape->bottom = (uint8_t)i;
        }
    }
    shape->y_top = y_top;
    shape->y_bottom = y_bottom;
    shape->degenerate = (p->flags & NDS_GX_POLY_DEGENERATE) != 0 || y_bottom > r->height;
}

/*
 * Lines r->first to r->last - 1 of the picture: for each, the rear plane,
 * the polygons that cross it in their order, and the final pass over the
 * line above.
 *
 * A band that is not the whole picture also draws the line before its first
 * and the line after its last, without the final pass: the final pass of a
 * line looks at the depths and polygon IDs of the lines either side, and
 * those do not change in their own final pass. So bands can be drawn in any
 * order, at once, and the picture is the one a single pass would draw.
 *
 * One thing does run from line to line in the console's picture: the shadow
 * stencil is cleared by the first mask after anything else, and "anything
 * else" includes the line before, so a line can inherit the stencil of the
 * line two above. A larger picture does not have that: there every line
 * starts with a clear stencil and no mask before it, so that a line depends
 * on no line before it and a band on no other band.
 */
static void draw_lines(Render *r)
{
    const NdsGx *gx = r->gx;
    unsigned count = gx->render_count;
    const int from = r->first > 0 ? r->first - 1 : 0;
    const int to = r->last < r->height ? r->last + 1 : r->height;
    r->mask_before = false;
    for (int y = from; y < to; y++) {
        if (r->fine) {
            r->mask_before = false;
            memset(r->stencil[y & 1], 0, (size_t)r->width);
        }
        clear_line(r, y);
        for (unsigned i = 0; i < count; i++) {
            const unsigned index = gx->render_order[i];
            const NdsGxPolygon *p = &r->polygons[index];
            Shape own;
            const Shape *shape = r->shapes ? &r->shapes[index] : &own;
            if (!r->shapes) {
                /* Line and flags first: most polygons miss most lines. */
                if (p->flags & NDS_GX_POLY_DEGENERATE) continue;
                if (y < p->y_top || (y >= p->y_bottom && !(y == p->y_top && p->y_bottom == p->y_top))) continue;
                shape_from_ram(r, p, &own);
            }
            if (shape->degenerate) continue;
            /* A polygon covers the lines from its top vertex down to, but
             * not including, its bottom vertex; one of no height, that
             * single line. */
            if (y < shape->y_top || (y >= shape->y_bottom && !(y == shape->y_top && shape->y_bottom == shape->y_top)))
                continue;
            draw_polygon_line(r, p, shape, y);
        }
        if (y > r->first && y - 1 < r->last) finish_line(r, y - 1);
    }
    if (to == r->height && r->last == r->height) finish_line(r, r->height - 1);
}

void nds_gx_render(NdsGx *gx, const NdsGxVram *vram)
{
    Render render;
    Rows rows;
    Render *r = &render;
    memset(r, 0, sizeof *r);
    memset(&rows, 0, sizeof rows);
    r->width = WIDTH;
    r->height = HEIGHT;
    r->scale = 1;
    r->frame = &gx->frame[0][0];
    r->first = 0;
    r->last = HEIGHT;
    for (int i = 0; i < 4; i++) {
        r->depth[i] = rows.depth[i];
        r->attr[i] = rows.attr[i];
    }
    r->border_depth = rows.border_depth;
    r->border_attr = rows.border_attr;
    for (int i = 0; i < 2; i++) {
        r->under_color[i] = rows.under_color[i];
        r->under_depth[i] = rows.under_depth[i];
        r->under_attr[i] = rows.under_attr[i];
        r->stencil[i] = rows.stencil[i];
    }
    setup(r, gx, vram);
    draw_lines(r);
}

/* A larger picture's lines in bands, shared between threads. */
#define BAND_LINES 24

typedef struct BandJob {
    const Render *base;   /* everything but the rows, and the lines */
    uint32_t *rows;       /* each thread's working rows, */
    uint8_t *stencil;     /* ... its stencil rows, */
    size_t row_words;     /* ... and how many words of rows a thread has */
    long next;            /* the next band to hand out */
    long bands;
    /* nds_gx_render_both(): the console's own picture, which the calling
     * thread draws before it takes a band. It writes gx->frame and nothing
     * else, and nothing here reads that. */
    NdsGx *own;
    const NdsGxVram *own_vram;
} BandJob;

/* A thread's rows, cut out of its block: as Rows has them for the screen,
 * and the spare row of colours. */
static void rows_for_thread(Render *r, const BandJob *job, unsigned thread)
{
    const size_t width = (size_t)r->width;
    uint32_t *next = job->rows + (size_t)thread * job->row_words;
    for (int i = 0; i < 4; i++) {
        r->depth[i] = next;
        next += width + 2u;
        r->attr[i] = next;
        next += width + 2u;
    }
    r->border_depth = next;
    next += width + 2u;
    r->border_attr = next;
    next += width + 2u;
    for (int i = 0; i < 2; i++) {
        r->under_color[i] = next;
        next += width;
        r->under_depth[i] = next;
        next += width;
        r->under_attr[i] = next;
        next += width;
        r->stencil[i] = job->stencil + ((size_t)thread * 2u + (size_t)i) * width;
    }
    r->spare = next;
}

static void draw_bands(void *opaque, unsigned thread)
{
    BandJob *job = (BandJob *)opaque;
    if (thread == 0 && job->own) nds_gx_render(job->own, job->own_vram);
    Render render = *job->base;
    Render *r = &render;
    rows_for_thread(r, job, thread);
    /* The border rows are the rear plane's, the same for every thread. */
    for (int x = 0; x < r->width + 2; x++) {
        r->border_depth[x] = r->clear_depth;
        r->border_attr[x] = r->clear_attr & ATTR_OPAQUE_ID;
    }
    for (;;) {
        const long band = nds_gx_workers_take(&job->next);
        if (band >= job->bands) return;
        r->first = (int)band * BAND_LINES;
        r->last = r->first + BAND_LINES < r->height ? r->first + BAND_LINES : r->height;
        draw_lines(r);
    }
}

/* The larger picture; and, with `own`, the console's as well (which is then
 * drawn whatever becomes of the larger one). */
static bool render_scaled(const NdsGx *gx, NdsGx *own, const NdsGxVram *vram, int scale, uint32_t *frame)
{
    if (!gx || !frame || scale < 1 || scale > NDS_GX_SCALE_MAX) {
        if (own) nds_gx_render(own, vram);
        return false;
    }
    const size_t width = (size_t)WIDTH * (size_t)scale;
    const int height = HEIGHT * scale;
    const long bands = (height + BAND_LINES - 1) / BAND_LINES;
    unsigned threads = nds_gx_workers_threads();
    if ((long)threads > bands) threads = (unsigned)bands;
    /* For each thread the rows of working pixels, as Rows has them for the
     * screen, and a spare row of colours; and a shape for every polygon the
     * rendering bank can hold. */
    const size_t row_words = 10u * (width + 2u) + 7u * width;
    uint32_t *rows = (uint32_t *)calloc((size_t)threads * row_words, sizeof(uint32_t));
    uint8_t *stencil = (uint8_t *)calloc((size_t)threads * 2u * width, 1u);
    Shape *shapes = (Shape *)calloc(NDS_GX_MAX_POLYGONS, sizeof(Shape));
    if (!rows || !stencil || !shapes) {
        free(rows);
        free(stencil);
        free(shapes);
        if (own) nds_gx_render(own, vram);
        return false;
    }

    Render render;
    Render *r = &render;
    memset(r, 0, sizeof *r);
    r->width = (int)width;
    r->height = height;
    r->scale = scale;
    r->fine = true;
    r->frame = frame;

    BandJob job;
    memset(&job, 0, sizeof job);
    job.base = r;
    job.rows = rows;
    job.stencil = stencil;
    job.row_words = row_words;
    job.bands = bands;
    job.own = own;
    job.own_vram = vram;

    /* setup() fills the border rows it is given; each thread fills its own
     * again (draw_bands). */
    rows_for_thread(r, &job, 0);
    setup(r, gx, vram);
    for (unsigned i = 0; i < gx->render_count; i++) {
        const unsigned index = gx->render_order[i];
        shape_scaled(r, &r->polygons[index], &shapes[index]);
    }
    r->shapes = shapes;

    if (threads < 2 || nds_gx_workers_run(draw_bands, &job, threads - 1u, 1) != 0) draw_bands(&job, 0);

    free(rows);
    free(stencil);
    free(shapes);
    return true;
}

bool nds_gx_render_scaled(const NdsGx *gx, const NdsGxVram *vram, int scale, uint32_t *frame)
{
    return render_scaled(gx, NULL, vram, scale, frame);
}

bool nds_gx_render_both(NdsGx *gx, const NdsGxVram *vram, int scale, uint32_t *frame)
{
    if (!gx) return false;
    return render_scaled(gx, gx, vram, scale, frame);
}

const uint32_t *nds_gx_line(const NdsGx *gx, int y)
{
    if (y < 0) y = 0;
    if (y >= HEIGHT) y = HEIGHT - 1;
    return gx->frame[y];
}
