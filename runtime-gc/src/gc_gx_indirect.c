/*
 * Indirect textures. See gc_gx_indirect.h, and docs/gc-fog-indirect.md
 * part B for where each constant comes from.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gc.h"
#include "gc_gx.h"
#include "gc_gx_indirect.h"
#include "gc_gx_texture.h"

#define BP_INDIRECT_MATRIX 0x06  /* .. 0x0e, three registers a matrix */
#define BP_INDIRECT_COMMAND 0x10 /* .. 0x1f, one a TEV stage */
#define BP_INDIRECT_SCALE 0x25   /* .. 0x26, two indirect stages a register */
#define BP_INDIRECT_ORDER 0x27

/* An eleven-bit two's-complement field, as a number. The matrix elements
 * are s0.10: a sign and ten fractional bits, so the range is -1 to just
 * under +1 and exactly +1 cannot be said at all. The SDK's own advice for
 * a coefficient of one is to halve every element and add one to the scale
 * exponent. */
static double element(uint32_t bits) {
  const int32_t signed_bits = (int32_t)(bits & 0x7ffu) - (int32_t)((bits & 0x400u) << 1);
  return (double)signed_bits / 1024.0;
}

/* One indirect matrix: two rows, three columns, and the scale exponent
 * that is spread two bits at a time across the three registers that hold
 * the columns. Register A carries the exponent's bits 1:0, B its 3:2 and C
 * its 5:4, and the whole six-bit value is biased by seventeen.
 *
 * `selector` is the command register's matrix index, which counts from
 * *one*: zero is "off" and one names the first matrix. The registers count
 * from zero, because the SDK normalises the selector back to a bare index
 * before working out which register to write. Using the selector directly
 * reads three registers nobody ever wrote, which is a matrix of zeroes and
 * an offset that never moves - and leaves every other part of the path
 * looking like it works. */
static void read_matrix(const GcGx *gx, unsigned selector, double matrix[2][3], int *exponent) {
  const unsigned base = BP_INDIRECT_MATRIX + ((selector - 1u) & 3u) * 3u;
  unsigned column;
  unsigned raw = 0;

  for (column = 0; column < 3u; column++) {
    const uint32_t reg = gx->bp[base + column];
    matrix[0][column] = element(reg);
    matrix[1][column] = element(reg >> 11);
    raw |= ((reg >> 22) & 3u) << (column * 2u);
  }
  *exponent = (int)raw - 17;
}

/* The wrap applied to the *regular* coordinate before the offset is added
 * - not to the indirect lookup's own coordinate. The enumerators run
 * downwards in size as the value rises: one is 256 texels, five is 16, and
 * six zeroes the coordinate outright, which is how "replace this
 * coordinate with the offset" is expressed.
 *
 * **Unverified:** the units. The values are plainly texel counts of the
 * regular map, but whether the hardware wraps the whole fixed-point
 * coordinate or only its integer texel part is not stated anywhere read.
 * This wraps the whole thing, which keeps the fractional position within a
 * texel and is the behaviour that makes a tiled lookup continuous. */
static float wrap_coordinate(float value, unsigned mode, unsigned size) {
  double texels, modulus;

  if (mode == 0u || size == 0u) return value;
  if (mode >= 6u) return 0.0f;

  modulus = (double)(256u >> (mode - 1u));
  texels = fmod((double)value * (double)size, modulus);
  if (texels < 0.0) texels += modulus;
  return (float)(texels / (double)size);
}

void gc_gx_indirect_coordinate(GcMachine *m, unsigned tev_stage, unsigned map,
                               const float generated[8][2], const float in[2], float out[2]) {
  GcGx *gx = &m->gx;
  const uint32_t command = gx->bp[BP_INDIRECT_COMMAND + (tev_stage & 0xfu)];
  const unsigned matrix_field = (command >> 9) & 0xfu;
  const unsigned matrix_index = matrix_field & 3u;
  const unsigned matrix_type = (matrix_field >> 2) & 3u;
  GcGxTexture regular;

  out[0] = in[0];
  out[1] = in[1];

  /* Index zero is GX_ITM_OFF, which the SDK describes as a matrix of all
   * zeroes: the offset is zero and the whole path is inert. GX_Init puts
   * every stage here, and GX_SetTevDirect returns a stage to it, so this
   * is the branch nearly every pixel takes. */
  if (matrix_index == 0u) return;

  gc_gx_texture_read(m, map, &regular);
  if (!regular.width || !regular.height) return;

  /* The wrap comes first: it is the regular coordinate that is wrapped,
   * and the offset is added to the wrapped value. */
  out[0] = wrap_coordinate(in[0], (command >> 13) & 7u, regular.width);
  out[1] = wrap_coordinate(in[1], (command >> 16) & 7u, regular.height);

  if (matrix_type != 0u) {
    /* The dynamic S-type and T-type matrices build their coefficients
     * from the incoming regular coordinate rather than from the stored
     * elements, and **no source read states what those coefficients
     * are** - the SDK only selects the mode. Rather than invent them,
     * this leaves the coordinate wrapped but unoffset, which is the same
     * thing a matrix of zeroes would do, and says so once. */
    if (m->log && gx->unsupported_indirect < 8u) {
      fprintf(m->log, "gx: a dynamic indirect matrix, which is not modelled (stage %u)\n",
              tev_stage);
    }
    gx->unsupported_indirect++;
    return;
  }

  {
    const unsigned indirect_stage = command & 3u;
    const unsigned format = (command >> 2) & 3u;
    const unsigned bias_select = (command >> 4) & 7u;
    const uint32_t order = gx->bp[BP_INDIRECT_ORDER];
    const unsigned indirect_map = (order >> (indirect_stage * 6u)) & 7u;
    const unsigned indirect_coord = (order >> (indirect_stage * 6u + 3u)) & 7u;
    const uint32_t scale_reg = gx->bp[BP_INDIRECT_SCALE + (indirect_stage >> 1)];
    const unsigned scale_s = (scale_reg >> ((indirect_stage & 1u) * 8u)) & 0xfu;
    const unsigned scale_t = (scale_reg >> ((indirect_stage & 1u) * 8u + 4u)) & 0xfu;
    /* The number of bits taken from the top of each component byte, and
     * the bias that goes with it. An eight-bit texel is unsigned 0-255 and
     * is biased by -128 so the offset can move either way; the narrow
     * formats are biased by +1 instead, which is asymmetric and is what
     * the hardware does - it is not a half-range bias written down wrong. */
    static const unsigned SHIFT[4] = {0u, 3u, 4u, 5u};
    const int bias = (format == 0u) ? -128 : 1;
    double matrix[2][3], offset[2], component[3];
    int exponent;
    uint8_t texel[4];
    unsigned i;

    /* The indirect lookup's own coordinate, divided by two to the scale.
     * This is what lets one generator feed both a large regular map and a
     * small indirect one. */
    {
      float to_map[2];
      gc_gx_texcoord_scale(m, indirect_coord & 7u, indirect_map, to_map);
      gc_gx_texture_sample(m, indirect_map,
                           generated[indirect_coord & 7u][0] * to_map[0] / (float)(1u << scale_s),
                           generated[indirect_coord & 7u][1] * to_map[1] / (float)(1u << scale_t),
                           texel);
    }

    /* **Unverified: which channel supplies S, T and U.** Neither the SDK's
     * header nor its source names the mapping. First channel to S, second
     * to T, third to U is the obvious reading and very likely right, but
     * it is an assumption, and a texel whose channels are all equal - which
     * is what a hand-written test map usually has - cannot tell. */
    for (i = 0; i < 3u; i++) {
      component[i] = (double)(texel[i] >> SHIFT[format]);
      if ((bias_select >> i) & 1u) component[i] += (double)bias;
    }

    read_matrix(gx, matrix_index, matrix, &exponent);
    for (i = 0; i < 2u; i++) {
      offset[i] = matrix[i][0] * component[0] + matrix[i][1] * component[1] +
                  matrix[i][2] * component[2];
      offset[i] *= ldexp(1.0, exponent);
    }

    /* The offset is in texels of the regular map, which the tiling case
     * proves: a tile index k with a spacing s produces k*s, exactly the
     * texels needed to reach tile k. The coordinate is normalised, so it
     * divides by the map's own size.
     *
     * **Unverified:** the fixed-point alignment the hardware uses between
     * the two. The texel-unit relation follows from the tiling
     * derivation; how many fractional bits it shifts by does not. */
    out[0] = (float)((double)out[0] + offset[0] / (double)regular.width);
    out[1] = (float)((double)out[1] + offset[1] / (double)regular.height);
  }

  /* Bit 20 chains the previous stage's coordinate result into this one and
   * bit 19 asks for the unmodified coordinate to drive the LOD. Neither is
   * modelled: the first needs a result carried between stages and the
   * second needs the LOD, which does not exist yet. */
}
