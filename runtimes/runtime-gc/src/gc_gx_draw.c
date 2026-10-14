/*
 * The transform unit, and the assembly of primitives from the vertices the
 * stream delivers.
 *
 * The register layouts are libogc's (zlib, on the reuse list): the viewport
 * is six floats at transform register 0x101a and the projection is seven
 * words at 0x1020, six of them numbers and the seventh a flag saying which
 * six they are. The 342 in the viewport's offsets is the hardware's own
 * origin - libogc puts it there and the hardware takes it out again - so it
 * is subtracted here rather than pretended away.
 */

#include <math.h>
#include <string.h>

#include "gc.h"
#include "gc_gx_draw.h"

/* The transform unit's registers, at 0x1000 upwards. */
#define XF_MATRIX_INDEX_A 0x0018
#define XF_MATRIX_INDEX_B 0x0019
#define XF_VIEWPORT 0x001a
#define XF_PROJECTION 0x0020
#define XF_NUM_CHANNELS 0x0009
#define XF_AMBIENT_COLOR 0x000a
#define XF_MATERIAL_COLOR 0x000c
#define XF_COLOR_CHANNEL 0x000e
#define XF_ALPHA_CHANNEL 0x0010

/* One per texture generator: the post-transform matrix index in bits 5:0
 * and the normalise bit in bit 8 (docs/gc-lighting-texgen.md B.3). */
#define XF_POST_TRANSFORM 0x0050
/* Bit 0 enables the post-transform stage at all. libogc sets it in
 * __GX_InitRevBits() and never clears it, so for a libogc title it is on
 * from the first field. */
#define XF_DUAL_TRANSFORM 0x0012
/* The post-transform matrices, three rows of four floats each, in the
 * transform unit's memory. The index in the register is already biased
 * against GX_DTTMTX0 and counts rows, so row 61 is GX_DTTIDENTITY. */
#define XF_POST_MATRIX_MEMORY 0x0500
#define XF_POST_IDENTITY 61u

/* The lights live in the transform unit's own memory, sixteen words each,
 * at 0x0600. The first three of each block are always zero. */
#define XF_LIGHT_MEMORY 0x0600
#define XF_LIGHT_WORDS 16

/* The register file is 0x60 long, which is not a power of two, so this is a
 * bounds check and not a mask: `reg & (0x60 - 1)` quietly turns register
 * 0x20 into register 0, which is a projection matrix of zeroes and a
 * picture with nothing in it. */
static float xf_float(const GcGx *gx, unsigned reg) {
  float value;
  const uint32_t bits = (reg < (unsigned)GC_GX_XF_REGS) ? gx->xf[reg] : 0u;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

void gc_gx_viewport(const GcGx *gx, GcGxViewport *out) {
  unsigned i;
  for (i = 0; i < 3; i++) {
    out->scale[i] = xf_float(gx, XF_VIEWPORT + i);
    out->offset[i] = xf_float(gx, XF_VIEWPORT + 3u + i);
  }
}

/* ------------------------------------------------------------------ */
/* Matrices                                                            */

unsigned gc_gx_position_matrix(const GcGx *gx, const GcGxVertex *v) {
  /* The descriptor says whether the vertex carried one. When it did not,
   * the register file holds the index the whole draw uses. */
  if (gx->vcd_lo & 1u) return v->position_matrix;
  return gx->xf[XF_MATRIX_INDEX_A] & 0x3fu;
}

unsigned gc_gx_texture_matrix(const GcGx *gx, const GcGxVertex *v, unsigned coordinate) {
  coordinate &= 7u;
  if (gx->vcd_lo & (2u << coordinate)) return v->texture_matrix[coordinate];
  if (coordinate < 4u) {
    return (gx->xf[XF_MATRIX_INDEX_A] >> (6u + 6u * coordinate)) & 0x3fu;
  }
  return (gx->xf[XF_MATRIX_INDEX_B] >> (6u * (coordinate - 4u))) & 0x3fu;
}

/* The matrix memory is addressed in rows of four floats, which is why the
 * indices a game uses count in threes: a position matrix is three rows. */
static void apply_matrix(const GcGx *gx, unsigned index, const float in[3], float out[3],
                         unsigned rows) {
  const unsigned base = (index & 0x3fu) * 4u;
  unsigned r;
  for (r = 0; r < rows; r++) {
    const unsigned at = base + r * 4u;
    if (at + 3u >= GC_GX_XF_MEMORY) {
      out[r] = 0.0f;
      continue;
    }
    out[r] = gx->xf_memory[at + 0] * in[0] + gx->xf_memory[at + 1] * in[1] +
             gx->xf_memory[at + 2] * in[2] + gx->xf_memory[at + 3];
  }
}

/* ------------------------------------------------------------------ */
/* Projection                                                          */

/* Six numbers and a flag. A perspective projection puts its two shear
 * terms in the z column and divides by -z; an orthographic one puts them in
 * the w column and does not divide at all. */
static void project(const GcGx *gx, const float view[3], float clip[4]) {
  const float p0 = xf_float(gx, XF_PROJECTION + 0);
  const float p1 = xf_float(gx, XF_PROJECTION + 1);
  const float p2 = xf_float(gx, XF_PROJECTION + 2);
  const float p3 = xf_float(gx, XF_PROJECTION + 3);
  const float p4 = xf_float(gx, XF_PROJECTION + 4);
  const float p5 = xf_float(gx, XF_PROJECTION + 5);
  const int orthographic = (gx->xf[XF_PROJECTION + 6u] & 1u) != 0;

  if (orthographic) {
    clip[0] = p0 * view[0] + p1;
    clip[1] = p2 * view[1] + p3;
    clip[2] = p4 * view[2] + p5;
    clip[3] = 1.0f;
  } else {
    clip[0] = p0 * view[0] + p1 * view[2];
    clip[1] = p2 * view[1] + p3 * view[2];
    clip[2] = p4 * view[2] + p5;
    clip[3] = -view[2];
  }
}

/* ------------------------------------------------------------------ */
/* Colour channels                                                     */

/* One channel's control register. */
#define CHANNEL_MATERIAL_FROM_VERTEX 0x0001u
#define CHANNEL_LIGHTING_ENABLED 0x0002u
#define CHANNEL_AMBIENT_FROM_VERTEX 0x0040u

/* The light mask is split across the register in two nibbles, low first. */
static unsigned channel_light_mask(uint32_t control) {
  return ((control >> 2) & 0xfu) | (((control >> 11) & 0xfu) << 4);
}

/* One light's block, read out of the transform unit's memory. */
typedef struct GcGxLight {
  uint8_t colour[4];
  float cosine[3];   /* a0, a1, a2: a quadratic in a cosine */
  float distance[3]; /* k0, k1, k2: a quadratic in a length */
  float position[3];
  float direction[3]; /* already negated: it points at the light */
} GcGxLight;

static void read_light(const GcGx *gx, unsigned index, GcGxLight *out) {
  const unsigned base = XF_LIGHT_MEMORY + (index & 7u) * XF_LIGHT_WORDS;
  uint32_t colour;
  unsigned i;
  memcpy(&colour, &gx->xf_memory[base + 3u], sizeof(colour));
  out->colour[0] = (uint8_t)(colour >> 24);
  out->colour[1] = (uint8_t)(colour >> 16);
  out->colour[2] = (uint8_t)(colour >> 8);
  out->colour[3] = (uint8_t)colour;
  for (i = 0; i < 3; i++) {
    out->cosine[i] = gx->xf_memory[base + 4u + i];
    out->distance[i] = gx->xf_memory[base + 7u + i];
    out->position[i] = gx->xf_memory[base + 10u + i];
    out->direction[i] = gx->xf_memory[base + 13u + i];
  }
}

static float dot3(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* The attenuation is a ratio of two quadratics, and the two are in
 * different variables: for a spot light the numerator is in the cosine of
 * the angle off the cone's axis and the denominator in the distance.
 * Feeding the cosine into both is what turns a spotlight into a specular
 * highlight, which is the usual way this is got wrong.
 *
 * The numerator is clamped to zero; the denominator is not. */
static float attenuate(const GcGxLight *light, unsigned function, const float to_light[3],
                       float distance, float half_angle) {
  float numerator, denominator, u, v;
  switch (function) {
    case 1u: /* specular: both quadratics are in the same cosine */
      u = v = half_angle;
      break;
    case 3u: /* spot */
      /* The cosine is clamped to zero before the quadratic sees it, and a
       * cosine that is not a number is zero too. A game that wants no cone
       * (a0 = 1, a1 = a2 = 0) need not give its light a direction, and Super
       * Mario Galaxy 2 does not: the three words hold whatever its stack
       * held, which is sometimes not a number - and nought times that, by
       * the host's arithmetic, turned every flower such a light shone on
       * black. Dolphin's software and shader paths both take max(0, cosine)
       * here; it is their reading, not a measurement of the hardware
       * (docs/gc-lighting-texgen.md A.4.2). */
      u = dot3(to_light, light->direction);
      if (!(u > 0.0f)) u = 0.0f;
      v = distance;
      break;
    default: /* no attenuation at all */
      return 1.0f;
  }
  numerator = light->cosine[2] * u * u + light->cosine[1] * u + light->cosine[0];
  if (numerator < 0.0f) numerator = 0.0f;
  denominator = light->distance[2] * v * v + light->distance[1] * v + light->distance[0];
  /* A zero denominator is not described by any source read for this.
   * Returning zero is the choice made here, and it is a choice, not
   * hardware behaviour: it keeps a light from becoming infinitely bright
   * at a single point. */
  if (denominator == 0.0f) return 0.0f;
  return numerator / denominator;
}

/* The illumination one light adds, per channel. */
static void light_contribution(const GcGxLight *light, uint32_t control, const float position[3],
                               const float normal[3], float out[4]) {
  const unsigned diffuse_function = (control >> 7) & 3u;
  const unsigned attenuation_function = (control >> 9) & 3u;
  float to_light[3] = {0.0f, 0.0f, 0.0f};
  float distance = 0.0f, diffuse = 1.0f, half_angle = 0.0f, factor;
  unsigned i;

  for (i = 0; i < 3; i++) to_light[i] = light->position[i] - position[i];
  distance = (float)sqrt((double)dot3(to_light, to_light));
  if (distance > 0.0f) {
    for (i = 0; i < 3; i++) to_light[i] /= distance;
  }

  if (diffuse_function == 1u || diffuse_function == 2u) {
    diffuse = dot3(normal, to_light);
    /* One of the two clamps, and one does not: a surface facing away from
     * a signed light *subtracts* from the accumulator. */
    if (diffuse_function == 2u && diffuse < 0.0f) diffuse = 0.0f;
  }

  if (attenuation_function == 1u) {
    /* In specular mode the direction words hold the half-angle vector
     * rather than a direction, and the normal is dotted with it. */
    half_angle = dot3(normal, light->direction);
  }

  factor = diffuse * attenuate(light, attenuation_function, to_light, distance, half_angle);
  for (i = 0; i < 4; i++) out[i] = factor * (float)light->colour[i];
}

static void unpack(uint32_t rgba, uint8_t out[4]) {
  out[0] = (uint8_t)(rgba >> 24);
  out[1] = (uint8_t)(rgba >> 16);
  out[2] = (uint8_t)(rgba >> 8);
  out[3] = (uint8_t)rgba;
}

/* Material times illumination, in the hardware's integers. The
 * illumination is widened the same way the texture environment widens its
 * interpolation weight - eight bits onto nought-to-256, so that white is a
 * true unity multiplier - and then the product is *truncated*. There is no
 * rounding bias here, unlike the combiner, and adding one would be wrong on
 * about half of the 65,536 pairs. */
static uint8_t modulate(unsigned material, unsigned illumination) {
  const unsigned widened = illumination + (illumination >> 7);
  return (uint8_t)((material * widened) >> 8);
}

static int clamp_channel(float value) {
  const int rounded = (int)(value + 0.5f);
  return rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded);
}

/* Every light the channel's mask names, in ascending order.
 * **Unverified:** the order, and whether the accumulator saturates per
 * light or once at the end. A clamp exists somewhere before the final
 * multiply - the widening needs one - but where is not measured. */
static void accumulate_lights(const GcGx *gx, uint32_t control, const float position[3],
                              const float normal[3], float illumination[4]) {
  const unsigned mask = channel_light_mask(control);
  unsigned light, i;
  if (!(control & CHANNEL_LIGHTING_ENABLED)) return;
  for (light = 0; light < 8u; light++) {
    GcGxLight source;
    float contribution[4];
    if (!((mask >> light) & 1u)) continue;
    read_light(gx, light, &source);
    light_contribution(&source, control, position, normal, contribution);
    for (i = 0; i < 4; i++) illumination[i] += contribution[i];
  }
}

static void shade(const GcGx *gx, const GcGxVertex *in, const float position[3],
                  const float normal[3], GcGxPoint *out) {
  unsigned channel;
  for (channel = 0; channel < 2; channel++) {
    const uint32_t colour_control = gx->xf[XF_COLOR_CHANNEL + channel];
    const uint32_t alpha_control = gx->xf[XF_ALPHA_CHANNEL + channel];
    uint8_t material[4], ambient[4];
    unsigned i;
    unpack(gx->xf[XF_MATERIAL_COLOR + channel], material);
    unpack(gx->xf[XF_AMBIENT_COLOR + channel], ambient);

    /* The colour and its alpha have their own control registers and can
     * disagree about where they come from. */
    if ((colour_control & CHANNEL_MATERIAL_FROM_VERTEX) && in->has_color[channel]) {
      for (i = 0; i < 3; i++) material[i] = in->color[channel][i];
    }
    if ((alpha_control & CHANNEL_MATERIAL_FROM_VERTEX) && in->has_color[channel]) {
      material[3] = in->color[channel][3];
    }
    if ((colour_control & CHANNEL_AMBIENT_FROM_VERTEX) && in->has_color[channel]) {
      for (i = 0; i < 3; i++) ambient[i] = in->color[channel][i];
    }
    if ((alpha_control & CHANNEL_AMBIENT_FROM_VERTEX) && in->has_color[channel]) {
      ambient[3] = in->color[channel][3];
    }

    /* An unlit channel is the material, untouched. A lit one is the
     * material times the illumination - and the ambient term is the
     * accumulator's *starting value*, not a stage of its own, which is why
     * a lit channel with no light in its mask comes out as material times
     * ambient rather than black. */
    {
      float illumination[2][4];
      unsigned which;
      for (which = 0; which < 2; which++) {
        for (i = 0; i < 4; i++) illumination[which][i] = (float)ambient[i];
      }
      accumulate_lights(gx, colour_control, position, normal, illumination[0]);
      accumulate_lights(gx, alpha_control, position, normal, illumination[1]);

      for (i = 0; i < 3; i++) {
        out->color[channel][i] =
            (colour_control & CHANNEL_LIGHTING_ENABLED)
                ? modulate(material[i], (unsigned)clamp_channel(illumination[0][i]))
                : material[i];
      }
      out->color[channel][3] =
          (alpha_control & CHANNEL_LIGHTING_ENABLED)
              ? modulate(material[3], (unsigned)clamp_channel(illumination[1][3]))
              : material[3];
    }
  }
}

/* ------------------------------------------------------------------ */
/* Texture coordinate generation                                        */

/* A generator's configuration register, at 0x1040 upwards. The API's
 * enumerations are *not* what these fields hold: libogc maps them through
 * a table, and two of the mappings are traps. The four three-component
 * sources are rows 0, 1, 3 and 4 with row 2 reserved for colour, so the
 * API's order and the hardware's differ; and the two matrix types are
 * inverted relative to the projection bit, so transcribing the enum
 * straight into the register gets every matrix generator backwards. */
#define XF_TEXGEN_INFO 0x0040
#define XF_NUM_TEXGENS 0x003f

/* Where a generator's input comes from, widened to the four components the
 * matrix multiplies. The fourth is always one, which is what makes the
 * matrix's last column a translation.
 *
 * The position and the normal are the vertex's own, as the game sent them:
 * not the position after its position matrix, nor the normal after its
 * normal matrix and brought to unit length. The texture matrix is the only
 * matrix a generated coordinate goes through, which is why a game that
 * projects a texture loads one made of the projection *and* the model's
 * view matrix, and why a skinned model that reflects its surroundings sends
 * a texture matrix index with every vertex: each bone's matrix turns that
 * bone's normals into the same view space. Fed the transformed normal, each
 * bone turned its normals twice, by a different amount, and the reflection
 * broke along the lines between bones wherever the model was posed - a
 * cracked shell on Super Mario Galaxy 2's Luma, smooth only where it faced
 * the camera at rest. This was the open question B.1 of
 * docs/gc-lighting-texgen.md; a second implementation's software and
 * hardware renderers both read the raw attribute.
 *
 * A two-component input form puts one in the third place whatever the row
 * (the same implementation), which no generator the SDK writes for a
 * position or a normal asks for. */
static void texgen_source(const GcGxVertex *in, unsigned row, int three_component, float out[4]) {
  out[0] = out[1] = 0.0f;
  out[2] = out[3] = 1.0f;
  switch (row) {
    case 0: /* position */
      out[0] = in->position[0];
      out[1] = in->position[1];
      out[2] = in->position[2];
      break;
    case 1: /* normal */
      out[0] = in->normal[0][0];
      out[1] = in->normal[0][1];
      out[2] = in->normal[0][2];
      break;
    case 3: /* binormal */
    case 4: /* tangent */
      out[0] = in->normal[row - 2u][0];
      out[1] = in->normal[row - 2u][1];
      out[2] = in->normal[row - 2u][2];
      break;
    case 2: /* a colour, which the colour-to-coordinate type reads */
      out[0] = (float)in->color[0][0];
      out[1] = (float)in->color[0][1];
      break;
    default: { /* a texture coordinate, rows five upwards */
      const unsigned which = (row >= 5u && row <= 12u) ? (row - 5u) : 0u;
      out[0] = in->texcoord[which][0];
      out[1] = in->texcoord[which][1];
      break;
    }
  }
  if (!three_component) out[2] = 1.0f;
}

/* A row of the texture matrix memory, four floats at a time. */
static float matrix_row(const GcGx *gx, unsigned index, unsigned row, const float in[4]) {
  const unsigned at = (index & 0x3fu) * 4u + row * 4u;
  if (at + 3u >= GC_GX_XF_MEMORY) return 0.0f;
  return gx->xf_memory[at] * in[0] + gx->xf_memory[at + 1u] * in[1] +
         gx->xf_memory[at + 2u] * in[2] + gx->xf_memory[at + 3u] * in[3];
}

/* The second half of a texture coordinate's journey through the transform
 * unit, and the half this used to leave out.
 *
 * `GX_SetTexCoordGen2()` gives an ordinary matrix texgen two more steps: the
 * coordinate the first-pass matrix produced may be normalised to unit
 * length, and it is then multiplied by a three-row matrix of its own. That
 * is how reflection and environment mapping is done on this machine - the
 * first matrix turns a normal into a direction, the normalise puts it on
 * the unit sphere, and the post-transform matrix maps the sphere into the
 * texture. Without the stage the coordinate keeps whatever scale the first
 * matrix left it with, which is usually far outside the texture and clamps
 * to one texel: a surface of one flat colour.
 *
 * The three gates are gx.h's, through docs/gc-lighting-texgen.md B.3: the
 * global enable at 0x1012 bit 0, an ordinary (matrix) texgen type, and the
 * normalise bit for the normalise.
 *
 * **Unverified:** where a three-row texgen's divide by q belongs. gx.h
 * describes the divide and this stage in separate places and relates them
 * nowhere. The divide happens after this stage here, which is what makes
 * the two consistent - a direction is normalised and mapped, and the result
 * is then projected - and which changes nothing for the identity matrix
 * every GX_SetTexCoordGen() texgen carries. */
static void post_transform(const GcGx *gx, unsigned coordinate, float generated[3]) {
  const unsigned reg = XF_POST_TRANSFORM + coordinate;
  const uint32_t info = (reg < (unsigned)GC_GX_XF_REGS) ? gx->xf[reg] : 0u;
  const unsigned index = info & 0x3fu;
  const int normalise = (info >> 8) & 1;
  const unsigned base = XF_POST_MATRIX_MEMORY + index * 4u;
  float out[3];
  unsigned r;

  if (!(gx->xf[XF_DUAL_TRANSFORM] & 1u)) return;

  if (normalise) {
    const float length = sqrtf(generated[0] * generated[0] + generated[1] * generated[1] +
                               generated[2] * generated[2]);
    if (length > 0.0f) {
      generated[0] /= length;
      generated[1] /= length;
      generated[2] /= length;
    }
  }

  /* The identity is a matrix like any other and the transform unit reads
   * it out of the same memory, so there is nothing to special-case - but a
   * title that never wrote those rows would be multiplying by zeroes, and
   * the index the SDK supplies by default is exactly this one. */
  if (index == XF_POST_IDENTITY) return;

  for (r = 0; r < 3u; r++) {
    const unsigned at = base + r * 4u;
    if (at + 3u >= GC_GX_XF_MEMORY) {
      out[r] = 0.0f;
      continue;
    }
    out[r] = gx->xf_memory[at + 0] * generated[0] + gx->xf_memory[at + 1] * generated[1] +
             gx->xf_memory[at + 2] * generated[2] + gx->xf_memory[at + 3];
  }
  generated[0] = out[0];
  generated[1] = out[1];
  generated[2] = out[2];
}

/* `position` is the vertex in view space, which only the emboss type
 * reads: it measures to a light, and lights are kept there. */
static void generate_texcoords(const GcGx *gx, const GcGxVertex *in, const float position[3], GcGxPoint *out) {
  const unsigned count = gx->xf[XF_NUM_TEXGENS] & 0xfu;
  unsigned i;

  for (i = 0; i < GC_GX_MAX_TEXCOORDS; i++) {
    const uint32_t info = gx->xf[XF_TEXGEN_INFO + i];
    const unsigned type = (info >> 4) & 7u;
    const unsigned row = (info >> 7) & 0x1fu;
    const int projected = (info >> 1) & 1;
    const int three_component = (info >> 2) & 1;
    float source[4], generated[3];

    out->texcoord[i][2] = 1.0f;
    if (i >= count) {
      /* A generator the register file does not claim still has to carry
       * the vertex's own coordinate through, because a stage may sample
       * with it and nothing says it must not. */
      out->texcoord[i][0] = in->texcoord[i][0];
      out->texcoord[i][1] = in->texcoord[i][1];
      continue;
    }

    texgen_source(in, row, three_component, source);

    switch (type) {
      case 0: /* a matrix, two rows or three */
        generated[0] = matrix_row(gx, gc_gx_texture_matrix(gx, in, i), 0, source);
        generated[1] = matrix_row(gx, gc_gx_texture_matrix(gx, in, i), 1, source);
        generated[2] =
            projected ? matrix_row(gx, gc_gx_texture_matrix(gx, in, i), 2, source) : 1.0f;
        break;
      case 2: /* a colour, as a coordinate */
      case 3:
        generated[0] = (float)in->color[type - 2u][0] / 255.0f;
        generated[1] = (float)in->color[type - 2u][1] / 255.0f;
        generated[2] = 1.0f;
        break;
      case 1: { /* emboss */
        /* An emboss generator does not produce a coordinate of its own. It
         * takes an *already generated* one and nudges it, so that the same
         * texture sampled twice - once straight and once nudged - differs
         * by an amount that depends on which way the surface faces the
         * light. That is the whole of bump mapping on this machine.
         *
         * Two of its fields are not where a reader expects. The light is a
         * zero-based index in bits 17:15 and the base coordinate a
         * zero-based index in bits 14:12, and the source row in bits 11:7
         * is meaningless here: every emboss generator the SDK writes
         * carries row 5, because the emboss source constants fall off the
         * end of its switch and land on the default. Reading that row as
         * though it meant something would sample the wrong thing.
         *
         * **Unverified: the offset arithmetic.** The SDK's header says
         * only "use light N in the bump map calculation" and neither it
         * nor its source says how the binormal, the tangent, the light
         * vector and the base coordinate combine, what scale the offset
         * carries, or whether the light vector is normalised first. What
         * is here is the standard reading - the light direction projected
         * onto the tangent offsets s and onto the binormal offsets t - and
         * it is consistent with the fields that exist, but the
         * *magnitude* in particular is a guess and no test here pins it.
         * See docs/gc-lighting-texgen.md section B.2.3. Producing a
         * plausible offset is still better than the nothing this used to
         * produce, which left every emboss coordinate at the origin. */
        const unsigned light = (info >> 15) & 7u;
        const unsigned base = (info >> 12) & 7u;
        GcGxLight state;
        float to_light[3], length;
        unsigned axis;

        read_light(gx, light, &state);
        for (axis = 0; axis < 3; axis++) to_light[axis] = state.position[axis] - position[axis];
        length = sqrtf(dot3(to_light, to_light));
        if (length > 0.0f) {
          for (axis = 0; axis < 3; axis++) to_light[axis] /= length;
        }

        /* The base coordinate is a lower-numbered generator's output, so
         * it has already been written this pass. */
        generated[0] = out->texcoord[base][0] + dot3(to_light, in->normal[2]);
        generated[1] = out->texcoord[base][1] + dot3(to_light, in->normal[1]);
        generated[2] = 1.0f;
        break;
      }
      default:
        generated[0] = source[0];
        generated[1] = source[1];
        generated[2] = 1.0f;
        break;
    }

    /* An ordinary texgen, and only an ordinary one, goes on through the
     * normalise and the post-transform matrix. gx.h: the post-transform
     * options "do not work for light-based texgens or emboss texgens". */
    if (type == 0u) post_transform(gx, i, generated);

    /* Only the three-row type divides, and the third row exists for no
     * other reason. */
    if (type == 0u && projected && generated[2] != 0.0f) {
      generated[0] /= generated[2];
      generated[1] /= generated[2];
    }
    out->texcoord[i][0] = generated[0];
    out->texcoord[i][1] = generated[1];
    out->texcoord[i][2] = generated[2];
  }
}

/* ------------------------------------------------------------------ */

void gc_gx_transform(const GcGx *gx, const GcGxVertex *in, GcGxPoint *out) {
  GcGxViewport viewport;
  float view[3];
  float normal[3] = {0.0f, 0.0f, 1.0f};
  unsigned i;

  memset(out, 0, sizeof(*out));

  apply_matrix(gx, gc_gx_position_matrix(gx, in), in->position, view, 3);
  if (in->has_normal) {
    /* The normal matrix is a three by three at its own base, and the index
     * that picks it is the position matrix index *as it stands*: three
     * words to a step here where the position matrices take four.
     *
     * This used to divide the index by three and step nine words, which
     * lands on the same place for every multiple of three - and every
     * matrix a program names through GX_PNMTX0 to GX_PNMTX9 is one, so
     * nothing here could tell the two apart. They part company on any other
     * index, which is what a game driving its matrices by a per-vertex index
     * produces. libogc's own formula and a second implementation's software
     * renderer both say three words a step (docs/gc-lighting.md, A.6).
     * **Unverified** against hardware; that document's case F.18 is the
     * test that would settle it. */
    const unsigned index = gc_gx_position_matrix(gx, in);
    const unsigned base = 0x400u + index * 3u;
    float length;
    for (i = 0; i < 3; i++) {
      const unsigned at = base + i * 3u;
      normal[i] = (at + 2u < GC_GX_XF_MEMORY)
                      ? gx->xf_memory[at] * in->normal[0][0] +
                            gx->xf_memory[at + 1u] * in->normal[0][1] +
                            gx->xf_memory[at + 2u] * in->normal[0][2]
                      : 0.0f;
    }
    length = (float)sqrt((double)dot3(normal, normal));
    if (length > 0.0f) {
      for (i = 0; i < 3; i++) normal[i] /= length;
    }
  }
  project(gx, view, out->clip);

  gc_gx_viewport(gx, &viewport);
  if (out->clip[3] != 0.0f) {
    /* The divide and the viewport are done wider than the registers are
     * stored, because the hardware resolves finer than a float does here;
     * see the note on GcGxPoint. */
    const double inverse = 1.0 / (double)out->clip[3];
    const double ndc[3] = {(double)out->clip[0] * inverse, (double)out->clip[1] * inverse,
                           (double)out->clip[2] * inverse};
    for (i = 0; i < 3; i++) {
      out->screen[i] = ndc[i] * (double)viewport.scale[i] + (double)viewport.offset[i];
    }
    /* The origin stays in. The scissor box carries the same 342, and the
     * scissor offset register is what takes it out - which is what lets a
     * game render in strips by moving that register rather than by
     * rewriting every viewport. The rasteriser does the subtraction. */
  }

  generate_texcoords(gx, in, view, out);

  shade(gx, in, view, normal, out);
}

/* ------------------------------------------------------------------ */
/* Primitive assembly                                                  */

void gc_gx_assemble_begin(GcGx *gx, unsigned kind) {
  gx->assembly_kind = kind;
  gx->assembly_count = 0;
  gx->assembly_parity = 0;
}

static void emit(GcMachine *m, const GcGxVertex *a, const GcGxVertex *b, const GcGxVertex *c) {
  GcGx *gx = &m->gx;
  GcGxPoint points[3];
  gx->triangles++;
  gc_gx_transform(gx, a, &points[0]);
  gc_gx_transform(gx, b, &points[1]);
  gc_gx_transform(gx, c, &points[2]);
  /* The hook is for watching; the rasteriser runs either way. */
  if (gx->on_triangle) gx->on_triangle(m, points);
  gc_gx_raster_triangle(m, points);
}

void gc_gx_assemble_vertex(GcMachine *m, const GcGxVertex *v) {
  GcGx *gx = &m->gx;
  GcGxVertex *buffer = gx->assembly;

  switch (gx->assembly_kind) {
    case GC_GX_QUADS:
    case GC_GX_QUADS_2:
      buffer[gx->assembly_count++] = *v;
      if (gx->assembly_count == 4u) {
        emit(m, &buffer[0], &buffer[1], &buffer[2]);
        emit(m, &buffer[0], &buffer[2], &buffer[3]);
        gx->assembly_count = 0;
      }
      return;

    case GC_GX_TRIANGLES:
      buffer[gx->assembly_count++] = *v;
      if (gx->assembly_count == 3u) {
        emit(m, &buffer[0], &buffer[1], &buffer[2]);
        gx->assembly_count = 0;
      }
      return;

    case GC_GX_TRIANGLE_STRIP:
      if (gx->assembly_count < 3u) {
        buffer[gx->assembly_count++] = *v;
      } else {
        buffer[0] = buffer[1];
        buffer[1] = buffer[2];
        buffer[2] = *v;
      }
      if (gx->assembly_count == 3u) {
        /* Every other triangle in a strip is wound the other way, and the
         * hardware swaps two vertices rather than changing the cull rule. */
        if (gx->assembly_parity) {
          emit(m, &buffer[1], &buffer[0], &buffer[2]);
        } else {
          emit(m, &buffer[0], &buffer[1], &buffer[2]);
        }
        gx->assembly_parity ^= 1u;
      }
      return;

    case GC_GX_TRIANGLE_FAN:
      if (gx->assembly_count < 3u) {
        buffer[gx->assembly_count++] = *v;
      } else {
        buffer[1] = buffer[2];
        buffer[2] = *v;
      }
      if (gx->assembly_count == 3u) emit(m, &buffer[0], &buffer[1], &buffer[2]);
      return;

    default:
      /* Lines and points have their own rasterisation rules and come with
       * the rest of P3. */
      gx->unsupported_primitives++;
      return;
  }
}
