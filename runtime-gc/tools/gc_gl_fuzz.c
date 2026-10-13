/*
 * gc_gl_fuzz: the OpenGL path against the reference rasteriser, on random
 * pipelines.
 *
 * The guest test programs cover what their authors thought to test, and
 * none of them blends, runs a logic operation, writes through a narrow
 * pixel format or builds a pipeline whose pixels depend on each other. This
 * covers everything the pixel pipeline reads by drawing random triangles
 * through random register states - every TEV stage, every texture format,
 * indirect lookups, fog, the alpha test, blending, logic operations,
 * dither, the depth modes and the channel enables - once through
 * gc_gx_raster_one with the OpenGL path detached, once through the OpenGL
 * path, from the same framebuffer, and then compares every pixel of colour
 * and depth, and the combiner registers the triangles leave behind.
 *
 *     gc_gl_fuzz [--iterations N] [--seed S] [--shaders uber|specialised|hybrid]
 *
 * Exit status 0 when every framebuffer matched. Register mismatches are
 * reported separately with whether the path had flagged them (see carry()
 * in gc_gl.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_efb.h"
#include "gc_gl.h"
#include "gc_gx.h"
#include "gc_gx_draw.h"
#include "gc_gx_texture.h"

void gc_gx_raster_one(GcMachine *m, const GcGxPoint points[3]);

static uint64_t rng_state = 1;
static uint32_t next(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return (uint32_t)(rng_state >> 11);
}
static uint32_t below(uint32_t n) { return n ? next() % n : 0; }
static double unit(void) { return (double)next() / 4294967296.0; }
static int chance(unsigned percent) { return below(100) < percent; }

#define TEXTURE_BASE 0x00100000u

static void random_texture(GcMachine *m, unsigned map, uint32_t address) {
  static const unsigned FORMATS[11] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x8, 0x9, 0xa, 0xe};
  const unsigned block = (map & 4u) ? 0xa0u : 0x80u;
  const unsigned n = map & 3u;
  const unsigned width = 1u + below(chance(50) ? 64 : 16);
  const unsigned height = 1u + below(chance(50) ? 64 : 16);
  const unsigned format = FORMATS[below(11)];
  uint32_t i;

  m->gx.bp[block + 8u + n] = (width - 1u) | ((height - 1u) << 10) | (format << 20);
  m->gx.bp[block + 0x14u + n] = address >> 5;
  /* wrap, filters, mip mode, diagonal, bias */
  m->gx.bp[block + n] = next() & 0x1ffffu;
  if (chance(40)) m->gx.bp[block + n] &= ~(0x7fu << 9); /* often no bias */
  /* min and max lod, often a plain range */
  m->gx.bp[block + 4u + n] = chance(50) ? ((below(256) << 8) | below(256)) : (0x80u << 8);
  m->gx.bp[block + 0x18u + n] = below(4) | (below(3) << 10);
  /* Enough bytes for the whole chain of the largest texture, and then some
   * - what follows a texture is part of what a pixel can read. */
  for (i = 0; i < 64u * 64u * 2u + 4096u; i++) m->mem1[address + i] = (uint8_t)next();
}

static void random_state(GcMachine *m) {
  GcGx *gx = &m->gx;
  unsigned i, k;
  const unsigned stages = 1u + below(chance(60) ? 4 : 16);

  gx->bp[0x00] = ((stages - 1u) << 10) | (below(4) << 14);
  if (chance(70)) gx->bp[0x00] &= ~(3u << 14); /* mostly no culling */
  for (i = 0; i < 16u; i++) {
    gx->bp[0xc0 + 2u * i] = next() & 0xffffffu;
    gx->bp[0xc1 + 2u * i] = next() & 0xffffffu;
    gx->bp[0x10 + i] = chance(25) ? (next() & 0x1fffffu) : 0u;
  }
  if (chance(50)) {
    /* A pipeline whose pixels are independent of each other is by far the
     * common case in games; make most of them so the card gets to draw. */
    for (i = 0; i < 16u; i++) {
      uint32_t c = gx->bp[0xc0 + 2u * i], a = gx->bp[0xc1 + 2u * i];
      unsigned sel;
      for (k = 0; k < 4u; k++) {
        sel = (c >> (4u * k)) & 0xfu;
        if (sel < 8u) c = (c & ~(0xfu << (4u * k))) | ((8u + below(8)) << (4u * k));
        sel = (a >> (4u + 3u * k)) & 7u;
        if (sel < 4u) a = (a & ~(7u << (4u + 3u * k))) | ((4u + below(4)) << (4u + 3u * k));
      }
      gx->bp[0xc0 + 2u * i] = c;
      gx->bp[0xc1 + 2u * i] = a;
    }
  }
  for (i = 0; i < 8u; i++) {
    gx->bp[0x28 + i] = next() & 0xffffffu;
    gx->bp[0x30 + 2u * i] = below(chance(50) ? 64 : 1024);
    gx->bp[0x31 + 2u * i] = below(chance(50) ? 64 : 1024);
  }
  for (i = 0x06; i <= 0x0e; i++) gx->bp[i] = next() & 0xffffffu;
  gx->bp[0x25] = next() & 0xffffu;
  gx->bp[0x26] = next() & 0xffffu;
  gx->bp[0x27] = next() & 0xffffffu;
  gx->bp[0xf3] = next() & 0xffffffu;
  if (chance(40)) gx->bp[0xf3] = (7u << 16) | (7u << 19); /* always passes */
  {
    static const unsigned SELECT[6] = {0, 2, 4, 5, 6, 7};
    const unsigned select = chance(60) ? 0u : SELECT[below(6)];
    gx->bp[0xee] = next() & 0xfffffu;
    gx->bp[0xef] = next() & 0xffffffu;
    gx->bp[0xf0] = below(32);
    gx->bp[0xf1] = (next() & 0xfffffu) | (below(2) << 20) | (select << 21);
    gx->bp[0xf2] = next() & 0xffffffu;
    gx->bp[0xe8] = next() & 0x7ffu;
    for (i = 0xe9; i <= 0xed; i++) gx->bp[i] = next() & 0xffffffu;
  }
  gx->bp[0x40] = next() & 0x1fu;
  gx->bp[0x41] = next() & 0xffffu;
  if (chance(30)) gx->bp[0x41] &= ~0x0803u; /* plain writes */
  gx->bp[0x42] = next() & 0x1ffu;
  gx->bp[0x43] = below(3) | (below(2) << 6);
  /* The scissor: usually the whole buffer, sometimes a box. */
  if (chance(70)) {
    gx->bp[0x20] = (342u << 12) | 342u;
    gx->bp[0x21] = ((342u + 639u) << 12) | (342u + 527u);
  } else {
    const unsigned l = 342u + below(400), t = 342u + below(300);
    gx->bp[0x20] = (l << 12) | t;
    gx->bp[0x21] = ((l + below(300)) << 12) | (t + below(300));
  }
  gx->bp[0x59] = (171u << 10) | 171u;

  for (i = 0; i < 4u; i++) {
    for (k = 0; k < 4u; k++) {
      gx->tev.reg[i][k] = (int)below(2048) - 1024;
      gx->tev.konstant[i][k] = (int)below(256);
      gx->tev.swap[i][k] = (uint8_t)below(4);
    }
    if (chance(50)) {
      for (k = 0; k < 4u; k++) gx->tev.swap[i][k] = (uint8_t)k;
    }
  }
  for (i = 0; i < 16u; i++) {
    gx->tev.konstant_color[i] = (uint8_t)below(32);
    gx->tev.konstant_alpha[i] = (uint8_t)below(32);
  }
  for (i = 0; i < 8u; i++) random_texture(m, i, TEXTURE_BASE + i * 0x10000u);
  for (i = 0; i < 4096u; i++) gx->tlut.entry[i] = (uint16_t)next();
}

static void random_triangle(GcGxPoint *p) {
  const double cx = 342.0 + unit() * 700.0 - 30.0, cy = 342.0 + unit() * 600.0 - 30.0;
  const double spread = chance(20) ? 700.0 : (chance(50) ? 120.0 : 24.0);
  unsigned v, k, c;
  memset(p, 0, sizeof(GcGxPoint) * 3u);
  for (v = 0; v < 3u; v++) {
    p[v].screen[0] = cx + (unit() - 0.5) * spread;
    p[v].screen[1] = cy + (unit() - 0.5) * spread;
    p[v].screen[2] = chance(20) ? 16777215.0 : unit() * 16777215.0;
    p[v].clip[3] = chance(50) ? 1.0f : (float)(0.25 + unit() * 4.0);
    for (c = 0; c < 4u; c++) p[v].color[0][c] = (uint8_t)next();
    for (k = 0; k < GC_GX_MAX_TEXCOORDS; k++) {
      p[v].texcoord[k][0] = (float)((unit() - 0.5) * (chance(50) ? 2.0 : 16.0));
      p[v].texcoord[k][1] = (float)((unit() - 0.5) * (chance(50) ? 2.0 : 16.0));
    }
  }
  if (chance(30)) {
    /* Two vertices sharing a w, the third not: perspective. */
    p[2].clip[3] = p[0].clip[3] * 3.0f;
  }
}

int main(int argc, char **argv) {
  unsigned iterations = 200, iteration;
  long only = -1;
  const char *shaders = NULL;
  GcGlOptions options;
  GcMachine *m;
  uint32_t *reference_color, *reference_depth, *start_color, *start_depth;
  unsigned colour_failures = 0, depth_failures = 0, register_failures = 0;
  int i;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--iterations") && i + 1 < argc) iterations = (unsigned)strtoul(argv[++i], NULL, 10);
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) rng_state = strtoull(argv[++i], NULL, 10) * 2654435761u + 1u;
    else if (!strcmp(argv[i], "--shaders") && i + 1 < argc) shaders = argv[++i];
    else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = strtol(argv[++i], NULL, 10);
  }

  m = gc_machine_create();
  if (!m) return 1;
  memset(&options, 0, sizeof(options));
  options.shaders = shaders;
  options.cache_dir = "off";
  if (gc_gl_start_with(m, &options) != 0) {
    fprintf(stderr, "gc_gl_fuzz: no OpenGL path\n");
    return 1;
  }
  reference_color = (uint32_t *)malloc(sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);
  reference_depth = (uint32_t *)malloc(sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);
  start_color = (uint32_t *)malloc(sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);
  start_depth = (uint32_t *)malloc(sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);

  for (iteration = 0; iteration < iterations; iteration++) {
    const unsigned triangles = 1u + below(6);
    GcGxPoint points[8][3];
    int reference_reg[4][4];
    void *gl;
    size_t p, colour_bad = 0, depth_bad = 0, first_bad = (size_t)-1;
    unsigned t;
    FILE *log = m->log;

    m->log = NULL; /* the reference's unsupported-feature notes, once each */
    random_state(m);
    for (p = 0; p < (size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT; p++) {
      const uint32_t c = next();
      /* A framebuffer the format could have produced. */
      start_color[p] = chance(50) ? (c | 0xff000000u) : c;
      start_depth[p] = next() & 0x00ffffffu;
    }
    for (t = 0; t < triangles; t++) random_triangle(points[t]);
    if (only >= 0 && (long)iteration != only) {
      m->log = log;
      continue;
    }
    if (only >= 0 && getenv("GC_GL_FUZZ_SET")) {
      /* reg=value,... over the random state, for narrowing a failure down. */
      const char *at = getenv("GC_GL_FUZZ_SET");
      while (*at) {
        char *end;
        const unsigned long reg = strtoul(at, &end, 16);
        if (*end != '=') break;
        m->gx.bp[reg & 0xff] = (uint32_t)strtoul(end + 1, &end, 16);
        at = (*end == ',') ? end + 1 : end;
        if (!*end) break;
      }
    }
    if (only >= 0) {
      for (t = 0; t < triangles; t++) {
        unsigned v;
        fprintf(stderr, "triangle %u:", t);
        for (v = 0; v < 3u; v++) {
          fprintf(stderr, " (%.6f %.6f %.1f w %g)", points[t][v].screen[0] - 342.0, points[t][v].screen[1] - 342.0,
                  points[t][v].screen[2], points[t][v].clip[3]);
        }
        fprintf(stderr, "\n");
      }
      for (t = 0; t < 16u; t++) {
        fprintf(stderr, "stage %u: colour %06x alpha %06x command %06x\n", t, m->gx.bp[0xc0 + 2u * t],
                m->gx.bp[0xc1 + 2u * t], m->gx.bp[0x10 + t]);
      }
    }

    /* The reference. */
    gl = m->gl;
    m->gl = NULL;
    memcpy(m->efb.color, start_color, sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);
    memcpy(m->efb.depth, start_depth, sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);
    {
      int saved[4][4];
      memcpy(saved, m->gx.tev.reg, sizeof(saved));
      for (t = 0; t < triangles; t++) gc_gx_raster_one(m, points[t]);
      memcpy(reference_reg, m->gx.tev.reg, sizeof(reference_reg));
      memcpy(m->gx.tev.reg, saved, sizeof(saved));
    }
    memcpy(reference_color, m->efb.color, sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);
    memcpy(reference_depth, m->efb.depth, sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);

    /* The card, from the same framebuffer. */
    m->gl = gl;
    memcpy(m->efb.color, start_color, sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);
    memcpy(m->efb.depth, start_depth, sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);
    gc_gl_invalidate(m);
    for (t = 0; t < triangles; t++) gc_gl_triangle(m, points[t]);
    gc_gl_sync_to_memory(m);
    m->log = log;

    for (p = 0; p < (size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT; p++) {
      if (m->efb.color[p] != reference_color[p]) {
        if (first_bad == (size_t)-1) first_bad = p;
        colour_bad++;
      }
      if (m->efb.depth[p] != reference_depth[p]) {
        if (first_bad == (size_t)-1) first_bad = p;
        depth_bad++;
      }
    }
    if (colour_bad || depth_bad) {
      const size_t x = first_bad % GC_EFB_WIDTH, y = first_bad / GC_EFB_WIDTH;
      if (colour_bad) colour_failures++;
      if (depth_bad) depth_failures++;
      fprintf(stderr,
              "gc_gl_fuzz: iteration %u: %zu colour and %zu depth pixels differ; first at %zu,%zu: "
              "colour %08x wanted %08x, depth %06x wanted %06x (blend %04x pe %02x z %02x dst %03x "
              "alpha %06x stages %u)\n",
              iteration, colour_bad, depth_bad, x, y, m->efb.color[first_bad], reference_color[first_bad],
              m->efb.depth[first_bad], reference_depth[first_bad], m->gx.bp[0x41], m->gx.bp[0x43],
              m->gx.bp[0x40], m->gx.bp[0x42], m->gx.bp[0xf3], ((m->gx.bp[0x00] >> 10) & 0xfu) + 1u);
    }
    if (only >= 0 && getenv("GC_GL_FUZZ_PIXEL")) {
      unsigned fx = 0, fy = 0;
      if (sscanf(getenv("GC_GL_FUZZ_PIXEL"), "%u,%u", &fx, &fy) == 2) first_bad = (size_t)fy * GC_EFB_WIDTH + fx;
    }
    if (only >= 0 && first_bad != (size_t)-1 && getenv("GC_GL_FUZZ_PROBE")) {
      /* What the reference's first stage saw at the first bad pixel, for
       * holding against a shader that writes the same things out. */
      const int px = (int)(first_bad % GC_EFB_WIDTH), py = (int)(first_bad / GC_EFB_WIDTH);
      const unsigned which = (unsigned)atoi(getenv("GC_GL_FUZZ_PROBE"));
      const GcGxPoint *q = points[which];
      double x[3], y[3], w[3], iw[3], area, a[3], c[3], sum;
      const double sp = 7.0 / 12.0;
      unsigned v, ch;
      for (v = 0; v < 3u; v++) {
        x[v] = q[v].screen[0] - 342.0;
        y[v] = q[v].screen[1] - 342.0;
        iw[v] = 1.0 / (double)q[v].clip[3];
      }
      area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
      {
        float coord[3][8][2];
        int k;
        for (k = 0; k < 3; k++) {
          const double sx = (double)(px + (k == 1)) + sp, sy = (double)(py + (k == 2)) + sp;
          w[0] = (x[2] - x[1]) * (sy - y[1]) - (y[2] - y[1]) * (sx - x[1]);
          w[1] = (x[0] - x[2]) * (sy - y[2]) - (y[0] - y[2]) * (sx - x[2]);
          w[2] = (x[1] - x[0]) * (sy - y[0]) - (y[1] - y[0]) * (sx - x[0]);
          for (v = 0; v < 3u; v++) a[v] = w[v] / area;
          sum = a[0] * iw[0] + a[1] * iw[1] + a[2] * iw[2];
          for (v = 0; v < 3u; v++) c[v] = sum > 0.0 ? a[v] * iw[v] * (1.0 / sum) : a[v];
          for (ch = 0; ch < 8u; ch++) {
            coord[k][ch][0] = (float)(c[0] * q[0].texcoord[ch][0] + c[1] * q[1].texcoord[ch][0] + c[2] * q[2].texcoord[ch][0]);
            coord[k][ch][1] = (float)(c[0] * q[0].texcoord[ch][1] + c[1] * q[1].texcoord[ch][1] + c[2] * q[2].texcoord[ch][1]);
          }
          if (k == 0) {
            fprintf(stderr, "probe %d,%d raster", px, py);
            for (ch = 0; ch < 4u; ch++) {
              const double value = c[0] * q[0].color[0][ch] + c[1] * q[1].color[0][ch] + c[2] * q[2].color[0][ch] + 0.5;
              fprintf(stderr, " %d", (int)(value < 0 ? 0 : value > 255 ? 255 : value));
            }
            fprintf(stderr, "\n");
          }
        }
        {
          const unsigned probe_stage = getenv("GC_GL_FUZZ_STAGE") ? (unsigned)atoi(getenv("GC_GL_FUZZ_STAGE")) : 0u;
          const uint32_t order = ((probe_stage & 1u) ? (m->gx.bp[0x28 + (probe_stage >> 1)] >> 12)
                                                     : m->gx.bp[0x28 + (probe_stage >> 1)]) & 0xfffu;
          const unsigned map = order & 7u, n = (order >> 3) & 7u;
          float scale[2], gradient[4];
          uint8_t texel[4];
          gc_gx_texcoord_scale(m, n, map, scale);
          gradient[0] = (coord[1][n][0] - coord[0][n][0]) * scale[0];
          gradient[1] = (coord[1][n][1] - coord[0][n][1]) * scale[1];
          gradient[2] = (coord[2][n][0] - coord[0][n][0]) * scale[0];
          gradient[3] = (coord[2][n][1] - coord[0][n][1]) * scale[1];
          const float s_value = coord[0][n][0] * scale[0];
          uint32_t s_bits;
          memcpy(&s_bits, &s_value, 4);
          gc_gx_texture_sample_gradient(m, map, coord[0][n][0] * scale[0], getenv("GC_GL_FUZZ_T0") ? 0.0f : coord[0][n][1] * scale[1], gradient, texel);
          fprintf(stderr, "probe order %03x map %u coord %u s %.9g (%08x) texel %u %u %u %u gradient %g %g %g %g\n",
                  order, map, n, coord[0][n][0] * scale[0], s_bits, texel[0], texel[1], texel[2], texel[3],
                  gradient[0], gradient[1], gradient[2], gradient[3]);
          {
            uint32_t cb, sb;
            memcpy(&cb, &coord[0][n][0], 4);
            memcpy(&sb, &scale[0], 4);
            fprintf(stderr, "probe coord bits %08x scale bits %08x\n", cb, sb);
          }
          fprintf(stderr, "probe gl pixel %08x\n", m->efb.color[first_bad]);
          {
            GcGxTexture tex;
            float su, sv, tu, tv, sd, td, delta;
            uint32_t bits;
            int lod;
            gc_gx_texture_read(m, map, &tex);
            su = gradient[0] * (float)tex.width;
            sv = gradient[1] * (float)tex.height;
            tu = gradient[2] * (float)tex.width;
            tv = gradient[3] * (float)tex.height;
            su = su < 0 ? -su : su;
            sv = sv < 0 ? -sv : sv;
            tu = tu < 0 ? -tu : tu;
            tv = tv < 0 ? -tv : tv;
            sd = tex.diagonal_lod ? su + tu : (su > tu ? su : tu);
            td = tex.diagonal_lod ? sv + tv : (sv > tv ? sv : tv);
            delta = sd > td ? sd : td;
            memcpy(&bits, &delta, 4);
            lod = delta > 0.0f ? (int)((bits >> 19) & 0x1fffu) - 127 * 16 : -0x7fff;
            lod += tex.lod_bias >> 1;
            if (lod > (int)tex.max_lod) lod = (int)tex.max_lod;
            if (lod < (int)tex.min_lod) lod = (int)tex.min_lod;
            fprintf(stderr,
                    "probe texture %ux%u format %x wrap %u %u mag %u min %u mip %u lod %u..%u diagonal %u bias %d -> lod %d\n",
                    tex.width, tex.height, tex.format, tex.wrap_s, tex.wrap_t, tex.magnify_linear, tex.minify_linear,
                    tex.mip_mode, tex.min_lod, tex.max_lod, tex.diagonal_lod, tex.lod_bias, lod);
          }
        }
      }
    }
    if (memcmp(reference_reg, m->gx.tev.reg, sizeof(reference_reg)) != 0) {
      register_failures++;
      fprintf(stderr, "gc_gl_fuzz: iteration %u: the combiner registers differ (early depth %u, test %u)\n",
              iteration, (m->gx.bp[0x43] >> 6) & 1u, m->gx.bp[0x40] & 1u);
    }
  }

  fprintf(stderr, "gc_gl_fuzz: %u iterations: %u with colour differences, %u with depth differences, "
                  "%u with register differences\n",
          iterations, colour_failures, depth_failures, register_failures);
  gc_machine_destroy(m);
  free(reference_color);
  free(reference_depth);
  free(start_color);
  free(start_depth);
  return (colour_failures || depth_failures) ? 1 : 0;
}
