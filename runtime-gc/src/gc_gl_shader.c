/*
 * The whole pixel pipeline as a fragment shader: coverage, interpolation,
 * the texture environment, the texture unit, indirect lookups, fog, the
 * alpha test, blending, logic operations, dither and the framebuffer's
 * own rounding.
 *
 * **It is gc_gx_raster.c, gc_gx_tev.c, gc_gx_texture.c, gc_gx_indirect.c,
 * gc_gx_fog.c, gc_gx_blend.c and gc_efb.c's quantise transcribed**, one
 * expression at a time and in the same types. That is the point and the
 * constraint. The reference rasteriser is what the hardware tests hold to
 * and so what "correct" means, and it is irregular on purpose: a sample
 * point at seven twelfths, barycentric weights in doubles, a weight of 0 to
 * 256 that skips 128, a bilinear blend in single precision rounded by a
 * truncating conversion, a log2 made of float bits. None of that survives
 * being rewritten as "what a GPU would naturally do", and every earlier
 * attempt that let the card rasterise and interpolate agreed only where
 * the numbers happened to be round.
 *
 * So the card does not rasterise the triangle. It rasterises the
 * triangle's bounding box - pixel-aligned, so exactly the pixels the
 * reference's own bounding box holds - and this shader asks the
 * reference's question of each one: the same edge functions in doubles at
 * the same sample point, the same perspective correction, the same
 * rounding of the colour. A triangle's vertices arrive in a buffer texture
 * as the reference's own doubles, bit for bit.
 *
 * Every floating-point variable is `precise`, which forbids the compiler
 * from fusing a multiply-add or reordering a sum - the two things MSVC is
 * also told not to do (/fp:strict). The C code performs these operations
 * left to right in IEEE single and double precision, and so does this.
 *
 * One body, two kinds of program. The pipeline's shape - how many stages,
 * what each combines, which maps and coordinates, the alpha test and the
 * fog function - is read through CFG(i). The general program defines that
 * as a lookup in a uniform block, so one compiled program draws anything;
 * a specialised program defines it as a constant array, and the compiler
 * folds every branch away. The arithmetic that remains is the same
 * arithmetic in the same order, which is what lets the two draw
 * bit-identical pictures (tools/gc/compare_gl.py checks both).
 *
 * What the transcription cannot reach, and how gc_gl.c handles it, is in
 * docs/GC-PLAN.md under "The OpenGL path, exact".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"

#ifdef GC_HAVE_SDL2

#include "gc_gl_internal.h"

/* The vertex program: which pixels get a fragment at all.
 *
 * Not the triangle as the card would rasterise it - the card's sample point
 * and edge rule are not the reference's - but the triangle grown by half a
 * pixel on every side. A pixel whose reference sample point (px + 7/12) is
 * inside the triangle has its centre (px + 1/2) within sqrt(2)/12 of that
 * point, so strictly inside the grown triangle by more than a third of a
 * pixel, and the card rasterises it whatever its own rules and its
 * sub-pixel snapping do. Every fragment then asks the reference's own
 * question (gc_gl_shader.c's body) and the ones outside are discarded; the
 * bounding box the reference would visit is checked there too. The corner
 * of a very sharp angle moves far out; it is capped at 2^16 pixels, where
 * the card's clipping takes over. The growth is worked out in doubles from
 * the record's own doubles.
 *
 * GC_GL_BOXES=1 draws the whole bounding box instead, as the first version
 * did: the same pixels get the same answer, only more of them are asked. */
#define UNIFORM_BLOCK \
    "layout(std140) uniform Draw {\n" \
    "  uvec4 cfg[20];\n" \
    "  ivec4 tev_reg[4];\n" \
    "  ivec4 konst[16];\n" \
    "  uvec4 fog0;\n" \
    "  uvec4 fog1;\n" \
    "  uvec4 fog_table[3];\n" \
    "  uvec4 ind_matrix[3];\n" \
    "  ivec4 tex_size[8];\n" \
    "  ivec4 tex_mode[8];\n" \
    "  ivec4 tex_lod[8];\n" \
    "  ivec4 tex_bias[8];\n" \
    "  uvec4 coord_size[8];\n" \
    "  uvec4 pe[2];\n" \
    "  uvec4 scale_bits[32];\n" \
    "};\n"

/* The fast path's vertex program (gc_gl.h, "The fast path"): the triangle
 * itself, for the card to rasterise and interpolate.
 *
 * The same record, read a vertex at a time. Position goes out with the
 * vertex's own w, so the card's perspective-correct interpolation of the
 * colour and the coordinate sets is the reference's (weights of a / w over
 * their sum), and depth, which is z / w there, stays linear across the
 * screen as the reference's is. A vertex whose 1 / w is zero gets a w so
 * large that it weighs nothing, which is what a zero does in the
 * reference's sum.
 *
 * The card samples a pixel at its centre and the reference a twelfth of a
 * pixel further on (px + 7/12), so the triangle moves back by a twelfth:
 * the card's centre then lands where the reference would have sampled.
 * What still differs is the rule for a sample exactly on an edge.
 *
 * The record's box is the scissor (and the triangle's own bounds, which cut
 * nothing off it). It goes to the card as four clip distances rather than a
 * test in the fragment program, because a fragment that is discarded there
 * may already have written its depth (FAST_EARLY, below). */
#define FAST_VERTEX_MAIN \
    "out vec4 f_colour;\n" \
    "out vec2 f_coord[8];\n" \
    "invariant gl_Position;\n" \
    "void main() {\n" \
    "  int base = gl_InstanceID * 25;\n" \
    "  int i = gl_VertexID % 3;\n" \
    "  uvec4 r1 = texelFetch(tris, base + 1), r2 = texelFetch(tris, base + 2), r3 = texelFetch(tris, base + 3);\n" \
    "  uvec4 r4 = texelFetch(tris, base + 4), r5 = texelFetch(tris, base + 5), r6 = texelFetch(tris, base + 6);\n" \
    "  uvec4 r7 = texelFetch(tris, base + 7), r8 = texelFetch(tris, base + 8);\n" \
    "  double xs[3] = double[3](vdbl(r1.x, r1.y), vdbl(r1.z, r1.w), vdbl(r2.x, r2.y));\n" \
    "  double ys[3] = double[3](vdbl(r2.z, r2.w), vdbl(r3.x, r3.y), vdbl(r3.z, r3.w));\n" \
    "  double zs[3] = double[3](vdbl(r4.x, r4.y), vdbl(r4.z, r4.w), vdbl(r5.x, r5.y));\n" \
    "  double iws[3] = double[3](vdbl(r5.z, r5.w), vdbl(r6.x, r6.y), vdbl(r6.z, r6.w));\n" \
    "  uint cs[3] = uint[3](r7.z, r7.w, r8.x);\n" \
    "  ivec4 box = ivec4(texelFetch(tris, base));\n" \
    "  uint c = cs[i];\n" \
    "  f_colour = vec4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24));\n" \
    "  for (int k = 0; k < 8; k++) {\n" \
    "    uvec4 a = texelFetch(tris, base + 9 + 2 * k), b = texelFetch(tris, base + 10 + 2 * k);\n" \
    "    uvec2 bits = (i == 0) ? a.xy : ((i == 1) ? a.zw : b.xy);\n" \
    "    f_coord[k] = vec2(uintBitsToFloat(bits.x), uintBitsToFloat(bits.y));\n" \
    "  }\n" \
    "  double iw = iws[i];\n" \
    "  float w = (iw > 1e-30lf) ? float(1.0lf / iw) : 1e30;\n" \
    "  double sx = xs[i] - 1.0lf / 12.0lf, sy = ys[i] - 1.0lf / 12.0lf;\n" \
    "  float x = float(sx / 320.0lf - 1.0lf);\n" \
    "  float y = float(sy / 264.0lf - 1.0lf);\n" \
    "  float z = float(zs[i] / 8388608.0lf - 1.0lf);\n" \
    "  gl_Position = vec4(x * w, y * w, z * w, w);\n" \
    "  gl_ClipDistance[0] = float(sx - double(box.x)) * w;\n" \
    "  gl_ClipDistance[1] = float(double(box.z + 1) - sx) * w;\n" \
    "  gl_ClipDistance[2] = float(sy - double(box.y)) * w;\n" \
    "  gl_ClipDistance[3] = float(double(box.w + 1) - sy) * w;\n" \
    "}\n"

static const char *VERTEX_BODY =
    UNIFORM_BLOCK
    "uniform usamplerBuffer tris;\n"
    "#ifndef GC_FAST\n"
    "flat out uvec4 v_rec[25];\n"
    "#endif\n"
    "double vdbl(uint lo, uint hi) { return UNPACK_DOUBLE(uvec2(lo, hi)); }\n"
    "#ifdef GC_FAST\n"
    FAST_VERTEX_MAIN
    "#else\n"
    "void main() {\n"
    "  int base = gl_InstanceID * 25;\n"
    "  uvec4 box = texelFetch(tris, base);\n"
    "  for (int k = 0; k < 25; k++) v_rec[k] = texelFetch(tris, base + k);\n"
    /* A bound on every depth the triangle's pixels will write, for the
     * card's early depth test (see gc_gl_fragment_source): below the
     * smallest vertex depth when the test is "less", above the largest when
     * it is "greater", sixteen steps clear of the rounding either way. */
    "  float EARLY_Z = 0.0;\n"
    "  {\n"
    "    double z0 = vdbl(v_rec[4].x, v_rec[4].y), z1 = vdbl(v_rec[4].z, v_rec[4].w), z2 = vdbl(v_rec[5].x, v_rec[5].y);\n"
    "    uint function = (pe[0].w >> 1) & 7u;\n"
    "    bool less = (pe[0].w & 1u) != 0u && (function == 1u || function == 3u);\n"
    "    bool greater = (pe[0].w & 1u) != 0u && (function == 4u || function == 6u);\n"
    "    double bound = 0.0lf;\n"
    "    if (less) {\n"
    "      double lowest = min(z0, min(z1, z2));\n"
    "      bound = (lowest >= 16.0lf) ? floor(lowest) - 16.0lf : 0.0lf;\n"
    "      if (!(bound <= 16777215.0lf)) bound = (lowest > 16777215.0lf) ? 16777199.0lf : 0.0lf;\n"
    "    } else if (greater) {\n"
    "      double highest = max(z0, max(z1, z2));\n"
    "      bound = (highest <= 16777199.0lf) ? ceil(highest) + 16.0lf : 16777216.0lf;\n"
    "      if (!(bound >= 0.0lf)) bound = (highest < 0.0lf) ? 16.0lf : 16777216.0lf;\n"
    "    }\n"
    "    EARLY_Z = float(bound / 8388608.0lf - 1.0lf);\n"
    "  }\n"
    "#ifdef GC_BOXES\n"
    "  int v = gl_VertexID;\n"
    "  bool right = (v == 1 || v == 3 || v == 4);\n"
    "  bool down = (v == 2 || v == 4 || v == 5);\n"
    "  float x = float(right ? int(box.z) + 1 : int(box.x));\n"
    "  float y = float(down ? int(box.w) + 1 : int(box.y));\n"
    "  gl_Position = vec4(x / 640.0 * 2.0 - 1.0, y / 528.0 * 2.0 - 1.0, EARLY_Z, 1.0);\n"
    "#else\n"
    "  uvec4 r1 = texelFetch(tris, base + 1), r2 = texelFetch(tris, base + 2), r3 = texelFetch(tris, base + 3);\n"
    "  dvec2 p[3];\n"
    "  p[0] = dvec2(vdbl(r1.x, r1.y), vdbl(r2.z, r2.w));\n"
    "  p[1] = dvec2(vdbl(r1.z, r1.w), vdbl(r3.x, r3.y));\n"
    "  p[2] = dvec2(vdbl(r2.x, r2.y), vdbl(r3.z, r3.w));\n"
    "  int i = gl_VertexID % 3;\n"
    "  dvec2 here = p[i], after = p[(i + 1) % 3], before = p[(i + 2) % 3];\n"
    "  double area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);\n"
    "  double s = area > 0.0lf ? 1.0lf : -1.0lf;\n"
    "  dvec2 e1 = here - before, e2 = after - here;\n"
    "  double l1 = length(e1), l2 = length(e2);\n"
    "  dvec2 v = dvec2(0.0lf);\n"
    "  if (l1 > 0.0lf && l2 > 0.0lf) {\n"
    "    dvec2 n1 = s * dvec2(e1.y, -e1.x) / l1;\n"
    "    dvec2 n2 = s * dvec2(e2.y, -e2.x) / l2;\n"
    "    double d = 1.0lf + dot(n1, n2);\n"
    "    dvec2 sum = n1 + n2;\n"
    "    double ls = length(sum);\n"
    "    if (d > 1e-12lf) v = 0.5lf * sum / d;\n"
    "    else if (ls > 0.0lf) v = 65536.0lf * sum / ls;\n"
    "    else v = 65536.0lf * n1;\n"
    "    double lv = length(v);\n"
    "    if (!(lv <= 65536.0lf)) v = (lv > 0.0lf) ? v * (65536.0lf / lv) : dvec2(0.0lf);\n"
    "  }\n"
    "  dvec2 at = here + v;\n"
    "  gl_Position = vec4(float(at.x / 320.0lf - 1.0lf), float(at.y / 264.0lf - 1.0lf), EARLY_Z, 1.0);\n"
    "#endif\n"
    "}\n"
    "#endif\n";

int gc_gl_fast = 0;
int gc_gl_fast_early = 0;
int gc_gl_scale = 1;
int gc_gl_card_filter = 0;
int gc_gl_anisotropy = 1;

static char vertex_text[16384];
const char *gc_gl_vertex_source = vertex_text;

void gc_gl_build_vertex_source(void) {
  /* GC_GL_NONCE: see gc_gl_fragment_source; the vertex shader too, or the
   * driver's cache would still answer for half of every program. */
  snprintf(vertex_text, sizeof(vertex_text), "#version 410 core\n#define UNPACK_DOUBLE %s\n// %s\n%s%s%s",
           gc_gl_unpack_double, getenv("GC_GL_NONCE") ? getenv("GC_GL_NONCE") : "",
           gc_gl_fast ? "#define GC_FAST 1\n" : "", getenv("GC_GL_BOXES") ? "#define GC_BOXES 1\n" : "",
           VERTEX_BODY);
}

/* The body. Written as C string lines; the comments that explain each
 * step are in the C this transcribes, and are not repeated. */
static const char *BODY =
    "#ifdef GC_FAST\n"
    /* Two outputs to one buffer: the source's term of the blend, and
     * beside it what the destination's factor reads (main's last block). */
    "layout(location = 0, index = 0) out vec4 out_color;\n"
    "layout(location = 0, index = 1) out vec4 out_blend;\n"
    "in vec4 f_colour;\n"
    "in vec2 f_coord[8];\n"
    "#else\n"
    "layout(location = 0) out vec4 out_color;\n"
    "flat in uvec4 v_rec[25];\n"
    "#endif\n"
    "#ifdef GC_CARD_FILTER\n"
    /* The fast path's textures are the card's to filter: eight bits a
     * channel as fractions, each map bound twice with a sampler apiece -
     * one that magnifies and one that minifies (gc_gl_render.c,
     * apply_fast_samplers). */
    "uniform sampler2D maps[8];\n"
    "uniform sampler2D maps_min[8];\n"
    "#else\n"
    "uniform usampler2D maps[8];\n"
    "#endif\n"
    "uniform sampler2D destination;\n"
    UNIFORM_BLOCK

    "const int INT_MIN = -2147483647 - 1;\n"
    "\n"
    /* ---- the triangle ---- */
    "double dbl(uint lo, uint hi) { return UNPACK_DOUBLE(uvec2(lo, hi)); }\n"
    "uint USED;\n"
    "#ifndef GC_FAST\n"
    "uvec4 record(int t) { return v_rec[t]; }\n"
    "double X0, X1, X2, Y0, Y1, Y2, Z0, Z1, Z2, IW0, IW1, IW2, AREA;\n"
    "ivec4 C0, C1, C2;\n"
    "vec2 TA[8], TB[8], TC[8];\n"
    "ivec4 unpack_colour(uint v) {\n"
    "  return ivec4(int(v & 255u), int((v >> 8) & 255u), int((v >> 16) & 255u), int(v >> 24));\n"
    "}\n"
    "#endif\n"
    "uint used_coordinates() {\n"
    "  uint used = 0u;\n"
    "  uint stages = CFG(0u);\n"
    "  for (uint s = 0u; s < stages; s++) {\n"
    "    uint order = CFG(3u + 4u * s);\n"
    "    if (((order >> 6) & 1u) == 0u) continue;\n"
    "    used |= 1u << ((order >> 3) & 7u);\n"
    "    uint command = CFG(4u + 4u * s);\n"
    "    if (((command >> 9) & 3u) != 0u) {\n"
    "      used |= 1u << ((CFG(67u) >> ((command & 3u) * 6u + 3u)) & 7u);\n"
    "    }\n"
    "  }\n"
    "  return used;\n"
    "}\n"
    "#ifndef GC_FAST\n"
    "void load() {\n"
    "  uvec4 r1 = record(1), r2 = record(2), r3 = record(3), r4 = record(4);\n"
    "  uvec4 r5 = record(5), r6 = record(6), r7 = record(7), r8 = record(8);\n"
    "  X0 = dbl(r1.x, r1.y); X1 = dbl(r1.z, r1.w); X2 = dbl(r2.x, r2.y); Y0 = dbl(r2.z, r2.w);\n"
    "  Y1 = dbl(r3.x, r3.y); Y2 = dbl(r3.z, r3.w); Z0 = dbl(r4.x, r4.y); Z1 = dbl(r4.z, r4.w);\n"
    "  Z2 = dbl(r5.x, r5.y); IW0 = dbl(r5.z, r5.w); IW1 = dbl(r6.x, r6.y); IW2 = dbl(r6.z, r6.w);\n"
    "  AREA = dbl(r7.x, r7.y);\n"
    "  C0 = unpack_colour(r7.z); C1 = unpack_colour(r7.w); C2 = unpack_colour(r8.x);\n"
    "  for (int k = 0; k < 8; k++) {\n"
    "    if (((USED >> uint(k)) & 1u) == 0u) continue;\n"
    "    uvec4 a = record(9 + 2 * k), b = record(10 + 2 * k);\n"
    "    TA[k] = vec2(uintBitsToFloat(a.x), uintBitsToFloat(a.y));\n"
    "    TB[k] = vec2(uintBitsToFloat(a.z), uintBitsToFloat(a.w));\n"
    "    TC[k] = vec2(uintBitsToFloat(b.x), uintBitsToFloat(b.y));\n"
    "  }\n"
    "}\n"
    "\n"
    /* ---- gc_gx_raster.c: edges_at and finish_point ---- */
    "void edges_at(double sx, double sy, out double w0, out double w1, out double w2) {\n"
    "  precise double e0 = (X2 - X1) * (sy - Y1) - (Y2 - Y1) * (sx - X1);\n"
    "  precise double e1 = (X0 - X2) * (sy - Y2) - (Y0 - Y2) * (sx - X2);\n"
    "  precise double e2 = (X1 - X0) * (sy - Y0) - (Y1 - Y0) * (sx - X0);\n"
    "  w0 = e0; w1 = e1; w2 = e2;\n"
    "}\n"
    "struct Point { double a0; double a1; double a2; double c0; double c1; double c2; vec2 coord[8]; };\n"
    "void finish(double w0, double w1, double w2, uint sets, out Point p) {\n"
    "  precise double a0 = w0 / AREA;\n"
    "  precise double a1 = w1 / AREA;\n"
    "  precise double a2 = w2 / AREA;\n"
    "  precise double sum = a0 * IW0 + a1 * IW1 + a2 * IW2;\n"
    "  precise double c0, c1, c2;\n"
    "  if (sum > 0.0lf) {\n"
    "    precise double scale = 1.0lf / sum;\n"
    "    c0 = a0 * IW0 * scale;\n"
    "    c1 = a1 * IW1 * scale;\n"
    "    c2 = a2 * IW2 * scale;\n"
    "  } else {\n"
    "    c0 = a0; c1 = a1; c2 = a2;\n"
    "  }\n"
    "  p.a0 = a0; p.a1 = a1; p.a2 = a2; p.c0 = c0; p.c1 = c1; p.c2 = c2;\n"
    "  for (int k = 0; k < 8; k++) {\n"
    "    if (((sets >> uint(k)) & 1u) == 0u) { p.coord[k] = vec2(0.0); continue; }\n"
    "    precise double s = c0 * double(TA[k].x) + c1 * double(TB[k].x) + c2 * double(TC[k].x);\n"
    "    precise double t = c0 * double(TA[k].y) + c1 * double(TB[k].y) + c2 * double(TC[k].y);\n"
    "    p.coord[k] = vec2(float(s), float(t));\n"
    "  }\n"
    "}\n"
    "#endif\n"
    "\n"
    /* ---- gc_gx_texture.c ---- */
    /* C's remainder, which truncates, for any int; exact in doubles. The
     * fast path does it in single precision, which is exact for a texel
     * coordinate (anything under 2^24) and a great deal quicker on a card:
     * a pixel that wraps a texture runs this for every tap. */
    "#ifdef GC_FAST\n"
    "int cmod(int v, int n) {\n"
    "  int r = v - n * int(trunc(float(v) / float(n)));\n"
    "  if (r >= n) r -= n;\n"
    "  if (r <= -n) r += n;\n"
    "  return r;\n"
    "}\n"
    "#else\n"
    "int cmod(int v, int n) {\n"
    "  precise double dv = double(v);\n"
    "  precise double dn = double(n);\n"
    "  precise double r = dv - dn * trunc(dv / dn);\n"
    "  return int(r);\n"
    "}\n"
    "#endif\n"
    "int wrap_texel(int value, int size, int mode) {\n"
    "  if (size <= 0) return 0;\n"
    "  if (value >= 0 && value < size) return value;\n"
    "  if (mode == 1) { int w = cmod(value, size); return (w < 0) ? w + size : w; }\n"
    "  if (mode == 2) {\n"
    "    int period = size * 2;\n"
    "    int w = cmod(value, period);\n"
    "    if (w < 0) w += period;\n"
    "    return (w < size) ? w : period - 1 - w;\n"
    "  }\n"
    "  return (value < 0) ? 0 : size - 1;\n"
    "}\n"
    /* (int)value, then floor: the conversion of a value it cannot represent
     * - NaN included - is INT_MIN on the host, and stays there. */
    "int floor_to_int(float value) {\n"
    "  int t = (abs(value) < 2147483648.0) ? int(value) : INT_MIN;\n"
    "  if (float(t) > value && t != INT_MIN) return t - 1;\n"
    "  return t;\n"
    "}\n"
    "#ifdef GC_CARD_FILTER\n"
    /* The fast path's texture unit: the level of detail is still worked out
     * here, the reference's way, and so is which level and which of the two
     * filters that means; the card then wraps, fetches and filters. Its
     * weights are its own (eight bits of fraction), so a filtered texel can
     * be a level out.
     *
     * Which filter is this program's choice and not the card's: the
     * reference magnifies when the level of detail is zero or less and
     * minifies above, while a card decides by the level it is asked for,
     * and those differ whenever a minified pixel lands on level 0. So the
     * map is bound twice, with a sampler that only magnifies and one that
     * only minifies, and the level is passed whole unless the two levels
     * either side are to be blended. */
    "ivec4 as_bytes(vec4 c) { return ivec4(c * 255.0 + 0.5); }\n"
    "ivec4 sample_with_lod(uint map, float s, float t, int lod) {\n"
    "  int base = (lod <= 0) ? (tex_lod[map].y >> 4) : (lod >> 4);\n"
    "  int fraction = (lod <= 0) ? 0 : (lod & 15);\n"
    "  int mip = tex_lod[map].x;\n"
    "  bool blend = lod > 0 && mip != 0 && mip != 1;\n"
    "  int level = base + ((lod > 0 && mip == 1 && fraction >= 8) ? 1 : 0);\n"
    "  if (tex_size[map].w != 0) {\n"
    /* A one-texel texture with a second level: both texels are side by
     * side in level 0, and no filter is asked to tell them apart. */
    "    ivec4 a = as_bytes(texelFetch(maps[map], ivec2(min(level, 1), 0), 0));\n"
    "    if (!blend) return a;\n"
    "    ivec4 b = as_bytes(texelFetch(maps[map], ivec2(min(level + 1, 1), 0), 0));\n"
    "    return (a * (16 - fraction) + b * fraction) >> 4;\n"
    "  }\n"
    "  float wanted = blend ? float(lod) * 0.0625 : float(level);\n"
    "  if (lod <= 0) return as_bytes(textureLod(maps[map], vec2(s, t), wanted));\n"
    "  return as_bytes(textureLod(maps_min[map], vec2(s, t), wanted));\n"
    "}\n"
    "#else\n"
    "ivec4 bound_texel(uint map, int level, int s, int t) {\n"
    "  if (tex_size[map].w != 0) return ivec4(texelFetch(maps[map], ivec2(min(level, 1), 0), 0));\n"
    "  int lw = max(tex_size[map].x >> level, 1);\n"
    "  int lh = max(tex_size[map].y >> level, 1);\n"
    "  int ws = wrap_texel(s, lw, tex_mode[map].x);\n"
    "  int wt = wrap_texel(t, lh, tex_mode[map].y);\n"
    "  return ivec4(texelFetch(maps[map], ivec2(ws, wt), min(level, tex_size[map].z)));\n"
    "}\n"
    "float byte_of(float v) {\n"
    "  float a = (v > 0.0) ? v : 0.0;\n"
    "  return (a < 255.0) ? a : 255.0;\n"
    "}\n"
    "ivec4 sample_level(uint map, int level, float s, float t, bool linear) {\n"
    "  int lw = max(tex_size[map].x >> level, 1);\n"
    "  int lh = max(tex_size[map].y >> level, 1);\n"
    "  int s0, t0;\n"
    "  precise float ws = 0.0;\n"
    "  precise float wt = 0.0;\n"
    "  if (!linear) {\n"
    "    precise float ss = s * float(lw);\n"
    "    precise float tt = t * float(lh);\n"
    "    s0 = floor_to_int(ss);\n"
    "    t0 = floor_to_int(tt);\n"
    "  } else {\n"
    "    precise float fs = s * float(lw) - 0.5;\n"
    "    precise float ft = t * float(lh) - 0.5;\n"
    "    s0 = floor_to_int(fs);\n"
    "    t0 = floor_to_int(ft);\n"
    "    ws = fs - float(s0);\n"
    "    wt = ft - float(t0);\n"
    "  }\n"
    /* bound_texel's four taps, with what they share worked out once: the
     * level, the wrap of each column and each row. The same texels. */
    "  vec4 tap[4];\n"
    "  if (tex_size[map].w != 0) {\n"
    "    ivec4 one = ivec4(texelFetch(maps[map], ivec2(min(level, 1), 0), 0));\n"
    "    if (!linear) return one;\n"
    "    tap[0] = vec4(one); tap[1] = tap[0]; tap[2] = tap[0]; tap[3] = tap[0];\n"
    "  } else {\n"
    "    int fetch_level = min(level, tex_size[map].z);\n"
    "    int x0 = wrap_texel(s0, lw, tex_mode[map].x);\n"
    "    int y0 = wrap_texel(t0, lh, tex_mode[map].y);\n"
    "    if (!linear) return ivec4(texelFetch(maps[map], ivec2(x0, y0), fetch_level));\n"
    "    int x1 = wrap_texel(s0 + 1, lw, tex_mode[map].x);\n"
    "    int y1 = wrap_texel(t0 + 1, lh, tex_mode[map].y);\n"
    "    tap[0] = vec4(texelFetch(maps[map], ivec2(x0, y0), fetch_level));\n"
    "    tap[1] = vec4(texelFetch(maps[map], ivec2(x1, y0), fetch_level));\n"
    "    tap[2] = vec4(texelFetch(maps[map], ivec2(x0, y1), fetch_level));\n"
    "    tap[3] = vec4(texelFetch(maps[map], ivec2(x1, y1), fetch_level));\n"
    "  }\n"
    "  precise vec4 top = tap[0] + ws * (tap[1] - tap[0]);\n"
    "  precise vec4 bottom = tap[2] + ws * (tap[3] - tap[2]);\n"
    "  precise vec4 value = (top + wt * (bottom - top)) + 0.5;\n"
    "  return ivec4(int(byte_of(value.x)), int(byte_of(value.y)), int(byte_of(value.z)),\n"
    "               int(byte_of(value.w)));\n"
    "}\n"
    "ivec4 sample_with_lod(uint map, float s, float t, int lod) {\n"
    "  int first;\n"
    "  int count = 1;\n"
    "  int fraction = 0;\n"
    "  bool linear;\n"
    "  if (lod <= 0) {\n"
    "    first = tex_lod[map].y >> 4;\n"
    "    linear = tex_mode[map].z != 0;\n"
    "  } else {\n"
    "    int base = lod >> 4;\n"
    "    fraction = lod & 15;\n"
    "    linear = tex_mode[map].w != 0;\n"
    "    if (tex_lod[map].x == 0) first = base;\n"
    "    else if (tex_lod[map].x == 1) first = base + ((fraction >= 8) ? 1 : 0);\n"
    "    else { first = base; count = 2; }\n"
    "  }\n"
    "  ivec4 got[2];\n"
    "  for (int i = 0; i < count; i++) got[i] = sample_level(map, first + i, s, t, linear);\n"
    "  if (count == 1) return got[0];\n"
    "  return (got[0] * (16 - fraction) + got[1] * fraction) >> 4;\n"
    "}\n"
    "#endif\n"
    "int fixed_log2(float value) {\n"
    "  if (!(value > 0.0)) return -32767;\n"
    "  return int((floatBitsToUint(value) >> 19u) & 0x1fffu) - 127 * 16;\n"
    "}\n"
    "float magnitude(float v) { return uintBitsToFloat(floatBitsToUint(v) & 0x7fffffffu); }\n"
    "ivec4 sample_bound(uint map, float s, float t, vec4 g) {\n"
    "  precise float du_dx = g.x * float(tex_size[map].x);\n"
    "  precise float dv_dx = g.y * float(tex_size[map].y);\n"
    "  precise float du_dy = g.z * float(tex_size[map].x);\n"
    "  precise float dv_dy = g.w * float(tex_size[map].y);\n"
    "  float su = magnitude(du_dx), sv = magnitude(dv_dx);\n"
    "  float tu = magnitude(du_dy), tv = magnitude(dv_dy);\n"
    "  bool diagonal = tex_lod[map].w != 0;\n"
    "  precise float s_delta = diagonal ? (su + tu) : ((su > tu) ? su : tu);\n"
    "  precise float t_delta = diagonal ? (sv + tv) : ((sv > tv) ? sv : tv);\n"
    "  float delta = (s_delta > t_delta) ? s_delta : t_delta;\n"
    "  int lod = fixed_log2(delta);\n"
    "  lod += tex_bias[map].x >> 1;\n"
    "  if (lod > tex_lod[map].z) lod = tex_lod[map].z;\n"
    "  if (lod < tex_lod[map].y) lod = tex_lod[map].y;\n"
    "#ifdef GC_ANISOTROPY\n"
    /* The card's anisotropic filtering, at the count the player chose: a
     * minified, linearly filtered, mipmapped texture is given to the card
     * with the gradients themselves, and the card takes several samples
     * along the longer one and chooses the level from the shorter - where
     * the level of detail above is chosen from the longer, and blurs a
     * surface seen at a slant along both. The bias goes in as a scale on
     * the gradients: a level is a doubling.
     *
     * This is not the console's own setting (two or four filter cycles, a
     * map's bits 20:19), which the reference draws as one and nothing here
     * reads (docs/gc-mipmap-lod.md, 3.6): it is for every such texture,
     * whatever the game asked for. */
    "  if (lod > 0 && tex_mode[map].w != 0 && tex_lod[map].x != 0 && tex_size[map].w == 0) {\n"
    "    float widen = exp2(float(tex_bias[map].x >> 1) * 0.0625);\n"
    "    return as_bytes(textureGrad(maps_min[map], vec2(s, t), g.xy * widen, g.zw * widen));\n"
    "  }\n"
    "#endif\n"
    "  return sample_with_lod(map, s, t, lod);\n"
    "}\n"
    /* gc_gx_texcoord_scale, worked out on the CPU: a single-precision
     * division on this card (NVIDIA, 616.64) is not always the correctly
     * rounded one - it came out one unit in the last place high for 26/11 -
     * so the shader is handed the quotient's bits instead of dividing. */
    "vec2 coord_scale(uint coordinate, uint map) {\n"
    "  uint at = coordinate * 16u + map * 2u;\n"
    "  return vec2(uintBitsToFloat(scale_bits[at >> 2][at & 3u]),\n"
    "              uintBitsToFloat(scale_bits[(at + 1u) >> 2][(at + 1u) & 3u]));\n"
    "}\n"
    "\n"
    /* ---- gc_gx_indirect.c ---- */
    "float wrap_coordinate(float value, uint mode, int size) {\n"
    "  if (mode == 0u || size == 0) return value;\n"
    "  if (mode >= 6u) return 0.0;\n"
    "  precise double modulus = double(256u >> (mode - 1u));\n"
    "  precise double x = double(value) * double(size);\n"
    "  precise double texels = x - modulus * trunc(x / modulus);\n"
    "  if (texels < 0.0lf) texels += modulus;\n"
    "  precise double r = texels / double(size);\n"
    "  return float(r);\n"
    "}\n"
    "double element(uint bits) {\n"
    "  int signed_bits = int(bits & 0x7ffu) - int((bits & 0x400u) << 1);\n"
    "  return double(signed_bits) / 1024.0lf;\n"
    "}\n"
    "double power_of_two(int e) {\n"
    "  if (e >= 0) return double(1u << uint(min(e, 30))) * double(1u << uint(max(e - 30, 0)));\n"
    "  return 1.0lf / double(1u << uint(-e));\n"
    "}\n"
    "void indirect_source(uint command, vec2 coord[8], out uint imap, out float is, out float it) {\n"
    "  uint stage = command & 3u;\n"
    "  uint order = CFG(67u);\n"
    "  imap = (order >> (stage * 6u)) & 7u;\n"
    "  uint icoord = (order >> (stage * 6u + 3u)) & 7u;\n"
    "  uint scale_reg = CFG(65u + (stage >> 1));\n"
    "  uint scale_s = (scale_reg >> ((stage & 1u) * 8u)) & 15u;\n"
    "  uint scale_t = (scale_reg >> ((stage & 1u) * 8u + 4u)) & 15u;\n"
    "  vec2 to_map = coord_scale(icoord, imap);\n"
    "  precise float s = coord[icoord].x * to_map.x / float(1u << scale_s);\n"
    "  precise float t = coord[icoord].y * to_map.y / float(1u << scale_t);\n"
    "  is = s;\n"
    "  it = t;\n"
    "}\n"
    "vec2 indirect_offset(uint map, uint command, ivec4 texel, vec2 o) {\n"
    "  uint index = (command >> 9) & 3u;\n"
    "  uint format = (command >> 2) & 3u;\n"
    "  uint bias_select = (command >> 4) & 7u;\n"
    "  int shift = (format == 0u) ? 0 : int(format) + 2;\n"
    "  double bias = (format == 0u) ? -128.0lf : 1.0lf;\n"
    "  precise double k0 = double(texel.x >> shift) + (((bias_select & 1u) != 0u) ? bias : 0.0lf);\n"
    "  precise double k1 = double(texel.y >> shift) + (((bias_select & 2u) != 0u) ? bias : 0.0lf);\n"
    "  precise double k2 = double(texel.z >> shift) + (((bias_select & 4u) != 0u) ? bias : 0.0lf);\n"
    "  uint base = ((index - 1u) & 3u) * 3u;\n"
    "  uint ra = ind_matrix[base >> 2][base & 3u];\n"
    "  uint rb = ind_matrix[(base + 1u) >> 2][(base + 1u) & 3u];\n"
    "  uint rc = ind_matrix[(base + 2u) >> 2][(base + 2u) & 3u];\n"
    "  uint raw = ((ra >> 22) & 3u) | (((rb >> 22) & 3u) << 2) | (((rc >> 22) & 3u) << 4);\n"
    "  double scale = power_of_two(int(raw) - 17);\n"
    "  precise double o0 = element(ra) * k0 + element(rb) * k1 + element(rc) * k2;\n"
    "  precise double o1 = element(ra >> 11) * k0 + element(rb >> 11) * k1 + element(rc >> 11) * k2;\n"
    "  precise double m0 = o0 * scale;\n"
    "  precise double m1 = o1 * scale;\n"
    "  precise double x = double(o.x) + m0 / double(tex_size[map].x);\n"
    "  precise double y = double(o.y) + m1 / double(tex_size[map].y);\n"
    "  return vec2(float(x), float(y));\n"
    "}\n"
    "\n"
    /* ---- gc_gx_tev.c ---- */
    "ivec4 swapped(ivec4 v, uint table) {\n"
    "  uint t = CFG(68u) >> (table * 8u);\n"
    "  return ivec4(v[int(t & 3u)], v[int((t >> 2) & 3u)], v[int((t >> 4) & 3u)], v[int((t >> 6) & 3u)]);\n"
    "}\n"
    "ivec3 colour_input(uint sel, ivec4 reg[4], ivec4 tex, ivec4 ras, ivec4 k) {\n"
    "  if (sel < 8u) return ((sel & 1u) != 0u) ? ivec3(reg[sel >> 1].a) : reg[sel >> 1].rgb;\n"
    "  if (sel == 8u) return tex.rgb;\n"
    "  if (sel == 9u) return ivec3(tex.a);\n"
    "  if (sel == 10u) return ras.rgb;\n"
    "  if (sel == 11u) return ivec3(ras.a);\n"
    "  if (sel == 12u) return ivec3(255);\n"
    "  if (sel == 13u) return ivec3(128);\n"
    "  if (sel == 14u) return k.rgb;\n"
    "  return ivec3(0);\n"
    "}\n"
    "int alpha_input(uint sel, ivec4 reg[4], ivec4 tex, ivec4 ras, ivec4 k) {\n"
    "  if (sel < 4u) return reg[sel].a;\n"
    "  if (sel == 4u) return tex.a;\n"
    "  if (sel == 5u) return ras.a;\n"
    "  if (sel == 6u) return k.a;\n"
    "  return 0;\n"
    "}\n"
    "int combine(int a, int b, int c, int d, uint bias, bool subtract, uint scale, bool clamped) {\n"
    "  int left = (scale == 3u) ? 0 : int(scale);\n"
    "  int right = (scale == 3u) ? 1 : 0;\n"
    "  int rounding = (scale == 3u) ? 0 : (subtract ? 127 : 128);\n"
    "  int bias_term = (bias == 1u) ? 128 : ((bias == 2u) ? -128 : 0);\n"
    "  a &= 255; b &= 255; c &= 255;\n"
    "  int weight = c + (c >> 7);\n"
    "  int interpolated = ((a * (256 - weight) + b * weight) << left) + rounding;\n"
    "  interpolated >>= 8;\n"
    "  int result = (d << left) + (subtract ? -interpolated : interpolated) + (bias_term << left);\n"
    "  result >>= right;\n"
    "  return clamped ? clamp(result, 0, 255) : clamp(result, -1024, 1023);\n"
    "}\n"
    "int compare(int a, int b, int c, int d, uint mode, bool equal) {\n"
    "  int l = a, r = b;\n"
    "  if (mode == 1u) { l = a & 255; r = b & 255; }\n"
    "  if (equal) return d + ((l == r) ? c : 0);\n"
    "  return d + ((l > r) ? c : 0);\n"
    "}\n"
    "\n"
    /* ---- gc_gx_fog.c ---- */
    "double fog_float(uint bits) { return double(uintBitsToFloat((bits & 0xfffffu) << 12)); }\n"
    /* 2^y. The reference calls the C library's pow; this is a series in
     * doubles, good to an ulp or two, which changes a rounded colour only
     * when the blend lands within that of a half. */
    "double pow2(double y) {\n"
    "  if (y == 0.0lf) return 1.0lf;\n"
    "  precise double n = floor(y);\n"
    "  precise double r = (y - n) * dbl(LN2_LO, LN2_HI);\n"
    "  precise double term = 1.0lf;\n"
    "  precise double sum = 1.0lf;\n"
    "  for (int i = 1; i < 17; i++) { term = term * r / double(i); sum = sum + term; }\n"
    "  int e = int(n);\n"
    "  if (e >= 0) return sum * double(1u << uint(min(e, 30)));\n"
    "  if (e < -30) return 0.0lf;\n"
    "  return sum / double(1u << uint(-e));\n"
    "}\n"
    /* The fast path's fog, in single precision: the same steps, with the
     * card's own exp2 where the reference has a series in doubles. */
    "#ifdef GC_FAST\n"
    "float fog_single(uint bits) { return uintBitsToFloat((bits & 0xfffffu) << 12); }\n"
    "float fog_factor_fast(uint depth, uint x) {\n"
    "  uint shape = CFG(70u);\n"
    "  uint select = shape & 7u;\n"
    "  float a = fog_single(fog0.x);\n"
    "  float c = fog_single(fog0.w & 0xfffffu);\n"
    "  float range;\n"
    "  if (((shape >> 3) & 1u) != 0u) {\n"
    "    range = a * (float(depth & 0xffffffu) / 16777215.0);\n"
    "  } else {\n"
    "    int denominator = int(fog0.y & 0xffffffu) - int((depth & 0xffffffu) >> (fog0.z & 31u));\n"
    "    if (denominator == 0) range = (a >= 0.0) ? 1e30 : -1e30;\n"
    "    else range = (a * 16777216.0) / float(denominator);\n"
    "  }\n"
    "  if (((fog1.y >> 10) & 1u) != 0u) {\n"
    "    uint centre = fog1.y & 0x3ffu;\n"
    "    uint dist = (x + 342u > centre) ? (x + 342u - centre) : (centre - x - 342u);\n"
    "    uint index = dist >> 5;\n"
    "    if (index >= 10u) index = 9u;\n"
    "    range = range * (float(fog_table[index >> 2][index & 3u]) / 256.0);\n"
    "  }\n"
    "  float z = clamp(range - c, 0.0, 1.0);\n"
    "  if (select == 2u) return z;\n"
    "  if (select == 4u) return 1.0 - exp2(-8.0 * z);\n"
    "  if (select == 5u) return 1.0 - exp2(-8.0 * z * z);\n"
    "  if (select == 6u) return exp2(-8.0 * (1.0 - z));\n"
    "  if (select == 7u) return exp2(-8.0 * (1.0 - z) * (1.0 - z));\n"
    "  return 0.0;\n"
    "}\n"
    "#endif\n"
    "double fog_factor(uint depth, uint x) {\n"
    "  uint shape = CFG(70u);\n"
    "  uint select = shape & 7u;\n"
    "  precise double a = fog_float(fog0.x);\n"
    "  precise double c = fog_float(fog0.w & 0xfffffu);\n"
    "  precise double range;\n"
    "  if (((shape >> 3) & 1u) != 0u) {\n"
    "    range = a * (double(depth & 0xffffffu) / 16777215.0lf);\n"
    "  } else {\n"
    "    int denominator = int(fog0.y & 0xffffffu) - int((depth & 0xffffffu) >> (fog0.z & 31u));\n"
    "    if (denominator == 0) {\n"
    "      range = (a >= 0.0lf) ? 1e30lf : -1e30lf;\n"
    "    } else {\n"
    "      range = (a * 16777216.0lf) / double(denominator);\n"
    "    }\n"
    "  }\n"
    "  if (((fog1.y >> 10) & 1u) != 0u) {\n"
    "    uint centre = fog1.y & 0x3ffu;\n"
    "    uint dist = (x + 342u > centre) ? (x + 342u - centre) : (centre - x - 342u);\n"
    "    uint index = dist >> 5;\n"
    "    if (index >= 10u) index = 9u;\n"
    "    range = range * (double(fog_table[index >> 2][index & 3u]) / 256.0lf);\n"
    "  }\n"
    "  precise double z = range - c;\n"
    "  if (z < 0.0lf) z = 0.0lf;\n"
    "  if (z > 1.0lf) z = 1.0lf;\n"
    "  precise double f;\n"
    "  if (select == 2u) f = z;\n"
    "  else if (select == 4u) f = 1.0lf - pow2(-8.0lf * z);\n"
    "  else if (select == 5u) f = 1.0lf - pow2(-8.0lf * z * z);\n"
    "  else if (select == 6u) f = pow2(-8.0lf * (1.0lf - z));\n"
    "  else if (select == 7u) f = pow2(-8.0lf * (1.0lf - z) * (1.0lf - z));\n"
    "  else f = 0.0lf;\n"
    "  return f;\n"
    "}\n"
    "\n"
    /* ---- gc_gx_blend.c ---- */
    "bool compares(uint function, int a, int b) {\n"
    "  if (function == 0u) return false;\n"
    "  if (function == 1u) return a < b;\n"
    "  if (function == 2u) return a == b;\n"
    "  if (function == 3u) return a <= b;\n"
    "  if (function == 4u) return a > b;\n"
    "  if (function == 5u) return a != b;\n"
    "  if (function == 6u) return a >= b;\n"
    "  return true;\n"
    "}\n"
    "int blend_factor(uint factor, int channel, ivec4 source, ivec4 destination, bool for_source) {\n"
    "  ivec4 other = for_source ? destination : source;\n"
    "  if (factor == 0u) return 0;\n"
    "  if (factor == 1u) return 255;\n"
    "  if (factor == 2u) return other[channel];\n"
    "  if (factor == 3u) return 255 - other[channel];\n"
    "  if (factor == 4u) return source.a;\n"
    "  if (factor == 5u) return 255 - source.a;\n"
    "  if (factor == 6u) return destination.a;\n"
    "  return 255 - destination.a;\n"
    "}\n"
    "int logic_op(uint op, int source, int destination) {\n"
    "  int s = source & 255, d = destination & 255;\n"
    "  if (op == 0u) return 0;\n"
    "  if (op == 1u) return s & d;\n"
    "  if (op == 2u) return s & ~d;\n"
    "  if (op == 3u) return s;\n"
    "  if (op == 4u) return ~s & d;\n"
    "  if (op == 5u) return d;\n"
    "  if (op == 6u) return s ^ d;\n"
    "  if (op == 7u) return s | d;\n"
    "  if (op == 8u) return ~(s | d);\n"
    "  if (op == 9u) return ~(s ^ d);\n"
    "  if (op == 10u) return ~d;\n"
    "  if (op == 11u) return s | ~d;\n"
    "  if (op == 12u) return ~s;\n"
    "  if (op == 13u) return ~s | d;\n"
    "  if (op == 14u) return ~(s & d);\n"
    "  return 255;\n"
    "}\n"
    "const int DITHER[4] = int[4](0, 2, 3, 1);\n"
    /* ---- gc_efb.c: quantise ---- */
    "uint six_bit(uint v) { return (v & 0xfcu) | ((v & 0xc0u) >> 6); }\n"
    "uint five_bit(uint v) { return (v & 0xf8u) | ((v & 0xe0u) >> 5); }\n"
    "uint quantise(uint format, uint argb) {\n"
    "  uint a = (argb >> 24) & 255u, r = (argb >> 16) & 255u, g = (argb >> 8) & 255u, b = argb & 255u;\n"
    "  if (format == 1u) return (six_bit(a) << 24) | (six_bit(r) << 16) | (six_bit(g) << 8) | six_bit(b);\n"
    "  if (format == 2u) return 0xff000000u | (five_bit(r) << 16) | (six_bit(g) << 8) | five_bit(b);\n"
    "  return 0xff000000u | (argb & 0x00ffffffu);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  int px = int(gl_FragCoord.x);\n"
    "  int py = int(gl_FragCoord.y);\n"
    "#ifdef GC_FAST\n"
    /* The fast path: the card has rasterised the triangle, clipped it to
     * the scissor and interpolated what the vertex program handed it, so
     * there is no coverage to work out here and no neighbour to visit for a
     * gradient. At a scale the card draws GC_SCALE pixels each way for
     * every one of the console's, and px and py stay the console's: the fog
     * table is indexed by them. The gradients are per pixel drawn, so a
     * texture's level of detail follows the size it is drawn at. */
    "  px /= GC_SCALE;\n"
    "  py /= GC_SCALE;\n"
    "  USED = used_coordinates();\n"
    "  uint GRAD = USED & pe[1].y;\n"
    "  vec2 COORD[8];\n"
    "  vec4 gradient[8];\n"
    "  for (int k = 0; k < 8; k++) {\n"
    "    vec4 g = vec4(dFdx(f_coord[k]), dFdy(f_coord[k]));\n"
    "    COORD[k] = f_coord[k];\n"
    "    gradient[k] = (((GRAD >> uint(k)) & 1u) != 0u) ? g : vec4(0.0);\n"
    "  }\n"
    "  uint incoming = uint(clamp(gl_FragCoord.z * 16777216.0, 0.0, 16777215.0));\n"
    "  ivec4 raster = ivec4(clamp(f_colour + 0.5, 0.0, 255.0));\n"
    "#else\n"
    "  {\n"
    "    uvec4 box = record(0);\n"
    "    if (px < int(box.x) || px > int(box.z) || py < int(box.y) || py > int(box.w)) discard;\n"
    "  }\n"
    "  USED = used_coordinates();\n"
    "  load();\n"
    "  double sample_point = dbl(SAMPLE_LO, SAMPLE_HI);\n"
    "  precise double sx = double(px) + sample_point;\n"
    "  precise double sy = double(py) + sample_point;\n"
    "  double w0, w1, w2;\n"
    "  edges_at(sx, sy, w0, w1, w2);\n"
    "  if (AREA > 0.0lf) {\n"
    "    if (w0 < 0.0lf || w1 < 0.0lf || w2 < 0.0lf) discard;\n"
    "  } else {\n"
    "    if (w0 > 0.0lf || w1 > 0.0lf || w2 > 0.0lf) discard;\n"
    "  }\n"
    "  Point centre;\n"
    "  finish(w0, w1, w2, USED, centre);\n"
    "  precise double depth = centre.a0 * Z0 + centre.a1 * Z1 + centre.a2 * Z2;\n"
    "  if (depth < 0.0lf) depth = 0.0lf; else if (depth > 16777215.0lf) depth = 16777215.0lf;\n"
    "  uint incoming = (depth == depth) ? uint(depth) : 0u;\n"
    "  gl_FragDepth = float(incoming) / 16777216.0;\n"
    "  ivec4 raster;\n"
    "  for (int ch = 0; ch < 4; ch++) {\n"
    "    precise double value = centre.c0 * double(C0[ch]) + centre.c1 * double(C1[ch]) +\n"
    "                           centre.c2 * double(C2[ch]);\n"
    "    precise double r = value + 0.5lf;\n"
    "    if (r < 0.0lf) r = 0.0lf; else if (r > 255.0lf) r = 255.0lf;\n"
    "    raster[ch] = (r == r) ? int(r) : INT_MIN;\n"
    "  }\n"
    "  vec4 gradient[8];\n"
    "  for (int k = 0; k < 8; k++) gradient[k] = vec4(0.0);\n"
    /* Only the sets a texture with a range of levels samples through need
     * the neighbours (the plan's gradients_used, handed over in pe[1].y);
     * the rest are zero, which the reference hands over too. */
    "  uint GRAD = USED & pe[1].y;\n"
    "  if (GRAD != 0u) {\n"
    "    Point right_of, under;\n"
    "    double r0, r1, r2;\n"
    "    precise double rx = double(px + 1) + sample_point;\n"
    "    precise double uy = double(py + 1) + sample_point;\n"
    "    edges_at(rx, sy, r0, r1, r2);\n"
    "    finish(r0, r1, r2, GRAD, right_of);\n"
    "    edges_at(sx, uy, r0, r1, r2);\n"
    "    finish(r0, r1, r2, GRAD, under);\n"
    "    for (int k = 0; k < 8; k++) {\n"
    "      if (((GRAD >> uint(k)) & 1u) == 0u) continue;\n"
    "      precise vec4 g = vec4(right_of.coord[k].x - centre.coord[k].x,\n"
    "                            right_of.coord[k].y - centre.coord[k].y,\n"
    "                            under.coord[k].x - centre.coord[k].x,\n"
    "                            under.coord[k].y - centre.coord[k].y);\n"
    "      gradient[k] = g;\n"
    "    }\n"
    "  }\n"
    "#define COORD centre.coord\n"
    "#endif\n"
    /* The combiner. */
    "  ivec4 reg[4];\n"
    "  reg[0] = tev_reg[0]; reg[1] = tev_reg[1]; reg[2] = tev_reg[2]; reg[3] = tev_reg[3];\n"
    "  ivec4 texel = ivec4(0);\n"
    "  uint stages = CFG(0u);\n"
    "  for (uint stage = 0u; stage < stages; stage++) {\n"
    "    uint cenv = CFG(1u + 4u * stage);\n"
    "    uint aenv = CFG(2u + 4u * stage);\n"
    "    uint order = CFG(3u + 4u * stage);\n"
    "    uint command = CFG(4u + 4u * stage);\n"
    "    if (((order >> 6) & 1u) != 0u) {\n"
    "      uint map = order & 7u;\n"
    "      uint n = (order >> 3) & 7u;\n"
    "      vec2 scale = coord_scale(n, map);\n"
    "      precise float nx = COORD[n].x * scale.x;\n"
    "      precise float ny = COORD[n].y * scale.y;\n"
    "      precise float g0 = gradient[n].x * scale.x;\n"
    "      precise float g1 = gradient[n].y * scale.y;\n"
    "      precise float g2 = gradient[n].z * scale.x;\n"
    "      precise float g3 = gradient[n].w * scale.y;\n"
    "      vec2 at = vec2(nx, ny);\n"
    "      bool indirect = ((command >> 9) & 3u) != 0u;\n"
    "      bool offset = indirect && ((command >> 11) & 3u) == 0u;\n"
    "      if (indirect) {\n"
    "        at = vec2(wrap_coordinate(nx, (command >> 13) & 7u, tex_size[map].x),\n"
    "                  wrap_coordinate(ny, (command >> 16) & 7u, tex_size[map].y));\n"
    "      }\n"
    "      for (int pass = offset ? 0 : 1; pass < 2; pass++) {\n"
    "        uint lookup = map;\n"
    "        float ls = at.x, lt = at.y;\n"
    "        vec4 lg = vec4(g0, g1, g2, g3);\n"
    "        if (pass == 0) {\n"
    "          indirect_source(command, COORD, lookup, ls, lt);\n"
    "          lg = vec4(0.0);\n"
    "        }\n"
    "        ivec4 got = sample_bound(lookup, ls, lt, lg);\n"
    "        if (pass == 0) at = indirect_offset(map, command, got, at);\n"
    "        else texel = got;\n"
    "      }\n"
    "    }\n"
    "    ivec4 ras = swapped(raster, aenv & 3u);\n"
    "    ivec4 tex = swapped(texel, (aenv >> 2) & 3u);\n"
    "    ivec4 k = konst[stage];\n"
    "    ivec3 a = colour_input((cenv >> 12) & 15u, reg, tex, ras, k);\n"
    "    ivec3 b = colour_input((cenv >> 8) & 15u, reg, tex, ras, k);\n"
    "    ivec3 c = colour_input((cenv >> 4) & 15u, reg, tex, ras, k);\n"
    "    ivec3 d = colour_input(cenv & 15u, reg, tex, ras, k);\n"
    "    {\n"
    "      uint bias = (cenv >> 16) & 3u;\n"
    "      bool subtract = ((cenv >> 18) & 1u) != 0u;\n"
    "      bool clamped = ((cenv >> 19) & 1u) != 0u;\n"
    "      uint scale = (cenv >> 20) & 3u;\n"
    "      ivec3 result;\n"
    "      for (int ch = 0; ch < 3; ch++) {\n"
    "        result[ch] = (bias == 3u) ? compare(a[ch], b[ch], c[ch], d[ch], scale, subtract)\n"
    "                                  : combine(a[ch], b[ch], c[ch], d[ch], bias, subtract, scale, clamped);\n"
    "      }\n"
    "      reg[(cenv >> 22) & 3u].rgb = result;\n"
    "    }\n"
    "    {\n"
    "      uint bias = (aenv >> 16) & 3u;\n"
    "      bool subtract = ((aenv >> 18) & 1u) != 0u;\n"
    "      bool clamped = ((aenv >> 19) & 1u) != 0u;\n"
    "      uint scale = (aenv >> 20) & 3u;\n"
    "      int aa = alpha_input((aenv >> 13) & 7u, reg, tex, ras, k);\n"
    "      int ab = alpha_input((aenv >> 10) & 7u, reg, tex, ras, k);\n"
    "      int ac = alpha_input((aenv >> 7) & 7u, reg, tex, ras, k);\n"
    "      int ad = alpha_input((aenv >> 4) & 7u, reg, tex, ras, k);\n"
    "      reg[(aenv >> 22) & 3u].a = (bias == 3u) ? compare(aa, ab, ac, ad, scale, subtract)\n"
    "                                              : combine(aa, ab, ac, ad, bias, subtract, scale, clamped);\n"
    "    }\n"
    "  }\n"
    /* The depth texture (gc_gx_raster.c, depth_from_texture): the last
     * texel sampled, read as a number, with the bias, is the pixel's depth
     * or is added to it. The fast path's program writes a depth only when
     * its pipeline has one (GC_ZTEX); its general program cannot, and until
     * the pipeline's own is ready such a pixel keeps the triangle's depth. */
    "  uint ZT = (pe[1].z >> 2) & 3u;\n"
    "#if !defined(GC_FAST) || defined(GC_ZTEX)\n"
    "  if (ZT == 1u || ZT == 2u) {\n"
    "    uint zt = pe[1].w;\n"
    "    uint width = pe[1].z & 3u;\n"
    "    if (width == 0u) zt += uint(texel.a);\n"
    "    else if (width == 1u) zt += (uint(texel.a) << 8) | uint(texel.r);\n"
    "    else zt += (uint(texel.r) << 16) | (uint(texel.g) << 8) | uint(texel.b);\n"
    "    if (ZT == 1u) zt += incoming;\n"
    "    incoming = zt & 0x00ffffffu;\n"
    "    gl_FragDepth = float(incoming) / 16777216.0;\n"
    "  }\n"
    "#ifdef GC_FAST\n"
    "  else gl_FragDepth = gl_FragCoord.z;\n"
    "#endif\n"
    "#endif\n"
    "  ivec4 result = reg[0] & 255;\n"
    /* Fog. */
    "  if ((CFG(70u) & 7u) != 0u) {\n"
    "    uint colour = fog1.x;\n"
    "    ivec3 fog = ivec3(int((colour >> 16) & 255u), int((colour >> 8) & 255u), int(colour & 255u));\n"
    "#ifdef GC_FAST\n"
    "    float ff = fog_factor_fast(incoming, uint(px));\n"
    "    result.rgb = clamp(ivec3(vec3(result.rgb) * (1.0 - ff) + vec3(fog) * ff + 0.5), 0, 255);\n"
    "#else\n"
    "    precise double f = fog_factor(incoming, uint(px));\n"
    "    for (int ch = 0; ch < 3; ch++) {\n"
    "      precise double mixed = double(result[ch]) * (1.0lf - f) + double(fog[ch]) * f;\n"
    "      precise double v = mixed + 0.5lf;\n"
    "      int value = (abs(v) < 2147483648.0lf) ? int(v) : INT_MIN;\n"
    "      result[ch] = (value < 0) ? 0 : ((value > 255) ? 255 : value);\n"
    "    }\n"
    "#endif\n"
    "  }\n"
    /* The alpha test. */
    "  {\n"
    "    uint compare_reg = CFG(69u);\n"
    "    bool first = compares((compare_reg >> 16) & 7u, result.a, int(compare_reg & 255u));\n"
    "    bool second = compares((compare_reg >> 19) & 7u, result.a, int((compare_reg >> 8) & 255u));\n"
    "    uint logic = (compare_reg >> 22) & 3u;\n"
    "    bool pass = (logic == 0u) ? (first && second)\n"
    "              : (logic == 1u) ? (first || second)\n"
    "              : (logic == 2u) ? (first != second) : (first == second);\n"
    "    if (!pass) {\n"
    "#ifndef GC_FAST\n"
    /* Early depth has already written this pixel's depth; the colour
     * stays what it was, which only the destination copy knows. */
    "      if (((pe[0].z >> 6) & 1u) != 0u && ((pe[0].w >> 4) & 1u) != 0u && pe[1].x != 0u &&\n"
    "          ZT != 1u && ZT != 2u) {\n"
    "        out_color = texelFetch(destination, ivec2(px, py), 0);\n"
    "        return;\n"
    "      }\n"
    "#endif\n"
    "      discard;\n"
    "    }\n"
    "  }\n"
    "#ifdef GC_FAST\n"
    /* The fast path's pixel write. The card blends, subtracts or applies
     * the logic operation (gc_gl_render.c, apply_fast_state), and this hands
     * it the two things it combines.
     *
     * The reference blends as (s * f + d * g) >> 8, a factor being 0 to 255
     * with its top bit added (gc_gx_blend.c); the card computes s * F + d * G
     * in fractions of 255 and rounds to nearest. So the source's term is
     * worked out here, s * f / 256, and goes out to be added as it is; and
     * the second output carries what the destination's factor reads,
     * already over 256.
     *
     * The card keeps eight bits of each (measured: an NVIDIA card rounds the
     * source's term to a whole level before it adds), so this is exact only
     * where nothing is left for the card to round: with nothing added
     * (destination factor zero) or the destination read whole (factor one),
     * the term is floored here and the sum is of whole numbers. Everywhere
     * else half a level comes off the term, which is what the reference's
     * floor takes off on average. A source factor that names the
     * destination is the card's to multiply by.
     *
     * No dither and no rounding to a six- or five-bit pixel format: the
     * buffer keeps eight bits, and the format decides only whether there is
     * an alpha to write. Against the reference that leaves under a level on
     * average (docs/GC-PLAN.md, "The fast path"). */
    "#define FACTOR(f) ((f) + step(127.5, (f)))\n"
    "  {\n"
    "    uint fmode = pe[0].x;\n"
    "    uint fformat = pe[0].z & 7u;\n"
    "    bool constant_alpha = ((pe[0].y >> 8) & 1u) != 0u;\n"
    "    vec4 source = vec4(result);\n"
    "    float alpha = float(result.a);\n"
    "    vec4 term = source;\n"
    "    vec4 reads = vec4(0.0);\n"
    "    if ((fmode & 1u) != 0u && ((fmode >> 11) & 1u) == 0u) {\n"
    "      uint sf = (fmode >> 8) & 7u;\n"
    "      uint df = (fmode >> 5) & 7u;\n"
    "      bool own = sf == 1u || sf == 4u || sf == 5u;\n"
    "      if (sf == 0u) term = vec4(0.0);\n"
    "      else if (sf == 4u) term = source * (FACTOR(alpha) / 256.0);\n"
    "      else if (sf == 5u) term = source * (FACTOR(255.0 - alpha) / 256.0);\n"
    "      if (own && df <= 1u) term = floor(term);\n"
    "      else if (own) term -= 0.5;\n"
    "      if (df == 1u) reads = vec4(1.0);\n"
    "      else if (df == 2u) reads = FACTOR(source) / 256.0;\n"
    "      else if (df == 3u) reads = 1.0 - FACTOR(255.0 - source) / 256.0;\n"
    "      else if (df == 4u) reads = vec4(FACTOR(alpha) / 256.0);\n"
    "      else if (df == 5u) reads = vec4(1.0 - FACTOR(255.0 - alpha) / 256.0);\n"
    "    }\n"
    "    if (constant_alpha) term.a = float(pe[0].y & 255u);\n"
    "    else if (fformat != 1u) term.a = 255.0;\n"
    "    out_color = term / 255.0;\n"
    "    out_blend = reads;\n"
    "  }\n"
    "#else\n"
    /* gc_gx_write_pixel. */
    "  uint mode = pe[0].x;\n"
    "  ivec4 was = ivec4(0);\n"
    "  if (pe[1].x != 0u) was = ivec4(round(texelFetch(destination, ivec2(px, py), 0) * 255.0));\n"
    "  ivec4 o;\n"
    "  if (((mode >> 11) & 1u) != 0u) {\n"
    "    o = clamp(was - result, 0, 255);\n"
    "  } else if ((mode & 1u) != 0u) {\n"
    "    uint destination_factor = (mode >> 5) & 7u;\n"
    "    uint source_factor = (mode >> 8) & 7u;\n"
    "    for (int ch = 0; ch < 4; ch++) {\n"
    "      int fs = blend_factor(source_factor, ch, result, was, true);\n"
    "      int fd = blend_factor(destination_factor, ch, result, was, false);\n"
    "      int s = result[ch] * (fs + (fs >> 7));\n"
    "      int d = was[ch] * (fd + (fd >> 7));\n"
    "      o[ch] = clamp((s + d) >> 8, 0, 255);\n"
    "    }\n"
    "  } else if (((mode >> 1) & 1u) != 0u) {\n"
    "    uint op = (mode >> 12) & 15u;\n"
    "    for (int ch = 0; ch < 4; ch++) o[ch] = logic_op(op, result[ch], was[ch]);\n"
    "  } else {\n"
    "    o = result;\n"
    "  }\n"
    "  uint format = pe[0].z & 7u;\n"
    "  if (((mode >> 2) & 1u) != 0u && format == 1u) {\n"
    "    int m = DITHER[(py & 1) * 2 + (px & 1)];\n"
    "    for (int ch = 0; ch < 3; ch++) o[ch] = ((o[ch] - (o[ch] >> 6)) + m) & 0xfc;\n"
    "  }\n"
    "  if (((pe[0].y >> 8) & 1u) != 0u) o.a = int(pe[0].y & 255u);\n"
    "  if (((mode >> 3) & 1u) == 0u) o.rgb = was.rgb;\n"
    "  if (((mode >> 4) & 1u) == 0u) o.a = was.a;\n"
    "  uint packed_colour = (uint(o.a) << 24) | (uint(o.r) << 16) | (uint(o.g) << 8) | uint(o.b);\n"
    "  uint q = quantise(format, packed_colour);\n"
    "  out_color = vec4(float((q >> 16) & 255u), float((q >> 8) & 255u), float(q & 255u),\n"
    "                   float(q >> 24)) / 255.0;\n"
    "#endif\n"
    "}\n";

/* The name of the function that turns two words into a double. The
 * specification calls it unpackDouble2x32 and packDouble2x32 goes the other
 * way; NVIDIA's compiler (616.64 here) has the two declared the wrong way
 * round, so gc_gl_programs.c asks the driver which one it accepts before
 * the first program is built. */
const char *gc_gl_unpack_double = "unpackDouble2x32";

static void split_double(double value, uint32_t *lo, uint32_t *hi) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  *lo = (uint32_t)bits;
  *hi = (uint32_t)(bits >> 32);
}

/* The text for a pipeline, or the general program's when `shape` is null.
 * `early` is the fast path's "depth before texture": a pipeline's own
 * shape[72], or which of the two general programs this is. */
static int fragment_source(const uint32_t *shape, int early, char *buffer, size_t size) {
  uint32_t sample_lo, sample_hi, ln2_lo, ln2_hi;
  size_t used;
  int written;
  unsigned i;

  { const double seven = 7.0, twelve = 12.0; split_double(seven / twelve, &sample_lo, &sample_hi); }
  split_double(0.69314718055994530942, &ln2_lo, &ln2_hi);

  written = snprintf(buffer, size,
                     "#version 410 core\n#define UNPACK_DOUBLE %s\n"
                     "#define SAMPLE_LO 0x%08xu\n#define SAMPLE_HI 0x%08xu\n"
                     "#define LN2_LO 0x%08xu\n#define LN2_HI 0x%08xu\n",
                     gc_gl_unpack_double, sample_lo, sample_hi, ln2_lo, ln2_hi);
  if (written < 0 || (size_t)written >= size) return -1;
  used = (size_t)written;

  if (gc_gl_fast) {
    /* The card interpolates depth itself and tests it when it likes - or,
     * for a pipeline with the console's "depth before texture" set
     * (shape[72], FAST_EARLY), before the program runs, so that a pixel the
     * alpha test then discards has still written its depth, as it has on
     * the console. It is a declaration and not a branch, so the general
     * program cannot choose it per pipeline: there are two general programs
     * instead, one with it and one without (gc_gl_general_source). */
    written = snprintf(buffer + used, size - used, "#define GC_FAST 1\n#define GC_SCALE %d\n%s%s%s", gc_gl_scale,
                       gc_gl_card_filter ? (gc_gl_anisotropy > 1 ? "#define GC_CARD_FILTER 1\n#define GC_ANISOTROPY 1\n"
                                                                    : "#define GC_CARD_FILTER 1\n")
                                         : "",
                       early ? "#extension GL_ARB_shader_image_load_store : enable\nlayout(early_fragment_tests) in;\n"
                             : "",
                       /* A pipeline with the depth texture writes its depth
                        * (and is never one that tests depth early). */
                       (shape && shape[73] != 0u) ? "#define GC_ZTEX 1\n" : "");
    if (written < 0 || (size_t)written >= size - used) return -1;
    used += (size_t)written;
  } else if (shape && shape[71] != 0u) {
    /* The depth this pixel writes is never below (1) or above (2) the depth
     * the card rasterised for the triangle (the vertex stage's EARLY_Z), so
     * the card may run its depth test before this program: a pixel that
     * fails it there fails it here too, and draws nothing either way. */
    written = snprintf(buffer + used, size - used,
                       "#extension GL_ARB_conservative_depth : enable\nlayout(%s) out float gl_FragDepth;\n",
                       shape[71] == 1u ? "depth_greater" : "depth_less");
    if (written < 0 || (size_t)written >= size - used) return -1;
    used += (size_t)written;
  }
  if (!shape) {
    written = snprintf(buffer + used, size - used, "#define CFG(i) cfg[(i) >> 2u][(i) & 3u]\n");
  } else {
    written = snprintf(buffer + used, size - used, "const uint K[%u] = uint[%u](", GC_GL_SHAPE_WORDS,
                       GC_GL_SHAPE_WORDS);
    if (written < 0 || (size_t)written >= size - used) return -1;
    used += (size_t)written;
    for (i = 0; i < GC_GL_SHAPE_WORDS; i++) {
      written = snprintf(buffer + used, size - used, "%s0x%xu", i ? "," : "", shape[i]);
      if (written < 0 || (size_t)written >= size - used) return -1;
      used += (size_t)written;
    }
    written = snprintf(buffer + used, size - used, ");\n#define CFG(i) K[i]\n");
  }
  if (written < 0 || (size_t)written >= size - used) return -1;
  used += (size_t)written;

  if (getenv("GC_GL_NONCE")) {
    /* A different text each run, so a driver's own cache of compiled
     * shaders cannot hide what compiling costs when that is measured. */
    written = snprintf(buffer + used, size - used, "// %s\n", getenv("GC_GL_NONCE"));
    if (written < 0 || (size_t)written >= size - used) return -1;
    used += (size_t)written;
  }
  if (getenv("GC_GL_BODY")) {
    /* For working on the shader: the body from a file instead. */
    FILE *f = fopen(getenv("GC_GL_BODY"), "rb");
    if (f) {
      const size_t got = fread(buffer + used, 1, size - used - 1u, f);
      fclose(f);
      buffer[used + got] = 0;
      return 0;
    }
  }
  written = snprintf(buffer + used, size - used, "%s", BODY);
  if (written < 0 || (size_t)written >= size - used) return -1;
  return 0;
}

int gc_gl_fragment_source(const uint32_t *shape, char *buffer, size_t size) {
  return fragment_source(shape, shape && shape[72] != 0u, buffer, size);
}

int gc_gl_general_source(int early, char *buffer, size_t size) {
  return fragment_source(NULL, gc_gl_fast && gc_gl_fast_early && early, buffer, size);
}

#endif /* GC_HAVE_SDL2 */
