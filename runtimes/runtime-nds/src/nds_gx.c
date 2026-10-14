/*
 * The DS geometry engine: the command FIFO and its timing, the matrix
 * stacks, lighting, texture coordinate generation, and polygon set-up
 * (culling, clipping, the viewport transform) into polygon list RAM and
 * vertex RAM, which the rendering engine (nds_gx_raster.c) draws from.
 *
 * The reference is GBATEK's "DS 3D" chapters; a section is named wherever a
 * behaviour comes from one. Where GBATEK is silent or wrong, the behaviour
 * is the one melonDS (src/GPU3D.cpp, GPL-3.0) has, which its authors worked
 * out against hardware: that source was read to learn what the hardware
 * does, not copied, and every such place says "melonDS".
 *
 * Numbers are fixed point throughout, as on hardware: matrices are 20.12,
 * vertex coordinates 4.12, normals and light vectors 1.9, texture
 * coordinates 12.4. Products are formed in 64 bits and the results cut to
 * 32, which is what GBATEK "DS 3D Matrix Examples (Maths Basics)" describes.
 */
#include <string.h>

#include "nds_gx.h"

/* Negative numbers are shifted right throughout, with the sign expected to
 * come along. */
_Static_assert((-8 >> 1) == -4, "arithmetic right shift of negative values");

/* ------------------------------------------------------------------ */
/* Fixed point                                                         */
/* ------------------------------------------------------------------ */

/* A 32-bit pattern as a signed number. */
static inline int32_t s32(uint32_t v)
{
    return v <= 0x7FFFFFFFu ? (int32_t)v : -(int32_t)(~v) - 1;
}

/* The low `bits` bits of v as a signed number. */
static inline int32_t sext(uint32_t v, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1u);
    v &= (sign << 1) - 1u;
    return s32((v ^ sign) - sign);
}

/* The exact product of two 32-bit numbers, as a 64-bit pattern that sums of
 * products can wrap around in. */
static inline uint64_t product(int32_t a, int32_t b)
{
    return (uint64_t)((int64_t)a * b);
}

/* Bits `shift`..`shift`+31 of a sum of products: the sum shifted right and
 * cut to 32 bits. */
static inline int32_t fraction(uint64_t sum, unsigned shift)
{
    return s32((uint32_t)(sum >> shift));
}

static const int32_t identity[16] = {
    0x1000, 0, 0, 0,
    0, 0x1000, 0, 0,
    0, 0, 0x1000, 0,
    0, 0, 0, 0x1000,
};

/* c = m * c. GBATEK "DS 3D Matrix Examples (Maths Basics)": each element is
 * a sum of four products with the full 24-bit fraction, shifted right by 12
 * at the end. */
static void matrix_multiply(int32_t c[16], const int32_t m[16])
{
    int32_t old[16];
    memcpy(old, c, sizeof old);
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            uint64_t sum = 0;
            for (int k = 0; k < 4; k++) sum += product(m[row * 4 + k], old[k * 4 + col]);
            c[row * 4 + col] = fraction(sum, 12);
        }
    }
}

/* (x, y, z, 1.0) * m. */
static void transform_point(const int32_t m[16], const int16_t v[3], int32_t out[4])
{
    for (int col = 0; col < 4; col++) {
        uint64_t sum = product(v[0], m[col]) + product(v[1], m[4 + col]) + product(v[2], m[8 + col]) +
                       product(0x1000, m[12 + col]);
        out[col] = fraction(sum, 12);
    }
}

/* (x, y, z) times the upper left 3x3 of m: one column's sum, unshifted. */
static inline uint64_t column_dot(const int32_t m[16], const int16_t v[3], int col)
{
    return product(v[0], m[col]) + product(v[1], m[4 + col]) + product(v[2], m[8 + col]);
}

/* A 5-bit colour of a BGR555 word. */
static inline uint8_t five(uint32_t word, unsigned shift)
{
    return (uint8_t)((word >> shift) & 0x1Fu);
}

/* A packed 10-bit signed component (NORMAL, LIGHT_VECTOR, VEC_TEST,
 * VTX_DIFF). */
static inline int16_t ten(uint32_t word, unsigned shift)
{
    return (int16_t)sext(word >> shift, 10);
}

/* ------------------------------------------------------------------ */
/* Commands: how many parameters each takes                            */
/* ------------------------------------------------------------------ */

/* GBATEK "DS 3D I/O Map", the geometry command table. Anything not in it
 * takes none: "Invalid commands ... do not fetch any parameters". */
static unsigned command_params(unsigned cmd)
{
    switch (cmd) {
    case 0x10: case 0x12: case 0x13: case 0x14:
        return 1;
    case 0x16: case 0x18:
        return 16;
    case 0x17: case 0x19:
        return 12;
    case 0x1A:
        return 9;
    case 0x1B: case 0x1C:
        return 3;
    case 0x20: case 0x21: case 0x22:
    case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x40: case 0x50: case 0x60: case 0x72:
        return 1;
    case 0x23: case 0x71:
        return 2;
    case 0x34:
        return 32;
    case 0x70:
        return 3;
    default:
        return 0; /* MTX_PUSH, MTX_IDENTITY, END_VTXS, NOP and the undefined codes */
    }
}

static inline bool is_test(unsigned cmd)
{
    return cmd >= 0x70 && cmd <= 0x72;
}

static inline bool is_stack(unsigned cmd)
{
    return cmd == 0x11 || cmd == 0x12;
}

/* ------------------------------------------------------------------ */
/* The command queue                                                   */
/* ------------------------------------------------------------------ */

static inline unsigned queue_beyond(const NdsGx *gx)
{
    return (unsigned)gx->queue_count - gx->pipe_count - gx->fifo_count;
}

/* GBATEK "FIFO / PIPE Number of Entries": "If the FIFO is empty, and if the
 * PIPE isn't full, then data is moved directly into the PIPE, otherwise it
 * is moved into the FIFO." An entry that finds the FIFO full waits behind it
 * (on hardware the ARM9 waits instead). */
static void queue_push(NdsGx *gx, unsigned cmd, uint32_t param)
{
    if (gx->queue_count >= NDS_GX_QUEUE_ENTRIES) {
        gx->dropped++;
        return;
    }
    unsigned slot = ((unsigned)gx->queue_head + gx->queue_count) % NDS_GX_QUEUE_ENTRIES;
    gx->queue_cmd[slot] = (uint8_t)cmd;
    gx->queue_param[slot] = param;
    bool waiting = queue_beyond(gx) != 0;
    gx->queue_count++;
    if (!waiting && gx->fifo_count == 0 && gx->pipe_count < NDS_GX_PIPE_ENTRIES) gx->pipe_count++;
    else if (!waiting && gx->fifo_count < NDS_GX_FIFO_ENTRIES) gx->fifo_count++;
    /* GXSTAT's two busy bits cover commands still on their way (melonDS):
     * a program polls them right after writing the command. */
    if (is_test(cmd)) gx->queued_tests++;
    if (is_stack(cmd)) gx->queued_stack++;
}

/* Take the oldest entry; the PIPE must not be empty. "If the PIPE runs half
 * empty (less than 3 entries) then 2 entries are moved from the FIFO to the
 * PIPE." */
static void queue_pop(NdsGx *gx, unsigned *cmd, uint32_t *param)
{
    *cmd = gx->queue_cmd[gx->queue_head];
    *param = gx->queue_param[gx->queue_head];
    gx->queue_head = (uint16_t)((gx->queue_head + 1u) % NDS_GX_QUEUE_ENTRIES);
    gx->queue_count--;
    gx->pipe_count--;
    if (is_test(*cmd)) gx->queued_tests--;
    if (is_stack(*cmd)) gx->queued_stack--;
    if (gx->pipe_count < 3) {
        unsigned moved = gx->fifo_count < 2 ? gx->fifo_count : 2u;
        gx->pipe_count = (uint16_t)(gx->pipe_count + moved);
        gx->fifo_count = (uint16_t)(gx->fifo_count - moved);
    }
    unsigned room = NDS_GX_FIFO_ENTRIES - (unsigned)gx->fifo_count;
    unsigned waiting = queue_beyond(gx);
    gx->fifo_count = (uint16_t)(gx->fifo_count + (waiting < room ? waiting : room));
}

/* A 32-bit write to GXFIFO (GBATEK "GXFIFO / Packed Commands"). A command
 * word holds up to four command bytes, lowest first; each is followed by
 * its parameters before the next byte is looked at. A zero byte is padding
 * and puts nothing in the FIFO, except that an all-zero word is one NOP
 * (melonDS). Commands without parameters go in with the word that completed
 * them as their (unused) parameter. */
static void gxfifo_write(NdsGx *gx, uint32_t value)
{
    if (gx->packed_left == 0) {
        unsigned first = value & 0xFFu;
        gx->packed = value;
        gx->packed_left = 4;
        gx->packed_need = (uint8_t)command_params(first);
        gx->packed_got = 0;
        if (gx->packed_need > 0) return; /* its parameters come next */
        if (first != 0 || value == 0) queue_push(gx, first, value);
    } else {
        queue_push(gx, gx->packed & 0xFFu, value);
        gx->packed_got++;
        if (gx->packed_got < gx->packed_need) return;
    }
    /* The current command is complete: start on the following bytes. Those
     * that take no parameters are complete at once. */
    for (;;) {
        gx->packed >>= 8;
        gx->packed_left--;
        if (gx->packed_left == 0) return;
        unsigned cmd = gx->packed & 0xFFu;
        gx->packed_need = (uint8_t)command_params(cmd);
        gx->packed_got = 0;
        if (gx->packed_need > 0) return;
        if (cmd != 0) queue_push(gx, cmd, value);
    }
}

/* ------------------------------------------------------------------ */
/* Matrices                                                            */
/* ------------------------------------------------------------------ */

/* "ClipMatrix = PositionMatrix * ProjectionMatrix", kept up to date
 * whenever either changes (GBATEK, CLIPMTX_RESULT). */
static void update_clip(NdsGx *gx)
{
    memcpy(gx->clip, gx->projection, sizeof gx->clip);
    matrix_multiply(gx->clip, gx->position);
}

/* The matrix types of GBATEK "DS 3D Matrix Types" as full 4x4 matrices:
 * what the parameters leave out comes from the identity. Multiplying by the
 * full matrix gives the same bits as the hardware's shorter sums, because a
 * 1.0 contributes an exact multiple of the 12-bit shift and a 0 nothing. */
enum { MATRIX_4X4, MATRIX_4X3, MATRIX_3X3, MATRIX_SCALE, MATRIX_TRANSLATE };

static void matrix_expand(int32_t m[16], int type, const uint32_t *p)
{
    memcpy(m, identity, sizeof identity);
    switch (type) {
    case MATRIX_4X4:
        for (int i = 0; i < 16; i++) m[i] = s32(p[i]);
        break;
    case MATRIX_4X3:
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 3; col++) m[row * 4 + col] = s32(p[row * 3 + col]);
        break;
    case MATRIX_3X3:
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++) m[row * 4 + col] = s32(p[row * 3 + col]);
        break;
    case MATRIX_SCALE:
        for (int i = 0; i < 3; i++) m[i * 5] = s32(p[i]);
        break;
    default:
        for (int i = 0; i < 3; i++) m[12 + i] = s32(p[i]);
        break;
    }
}

/* MTX_MODE (GBATEK "DS 3D Matrix Load/Multiply") selects what the load and
 * multiply commands change: 0 projection, 1 position, 2 position and
 * vector, 3 texture. */
static void matrix_load(NdsGx *gx, const int32_t m[16])
{
    switch (gx->matrix_mode) {
    case 0:
        memcpy(gx->projection, m, sizeof gx->projection);
        break;
    case 1:
        memcpy(gx->position, m, sizeof gx->position);
        break;
    case 2:
        memcpy(gx->position, m, sizeof gx->position);
        memcpy(gx->vector, m, sizeof gx->vector);
        break;
    default:
        memcpy(gx->texture, m, sizeof gx->texture);
        return;
    }
    update_clip(gx);
}

/* "MTX_SCALE in Mode 2: uses ONLY Position Matrix", to keep the light
 * vectors' lengths. */
static void matrix_apply(NdsGx *gx, const int32_t m[16], bool scale)
{
    switch (gx->matrix_mode) {
    case 0:
        matrix_multiply(gx->projection, m);
        break;
    case 1:
        matrix_multiply(gx->position, m);
        break;
    case 2:
        matrix_multiply(gx->position, m);
        if (!scale) matrix_multiply(gx->vector, m);
        break;
    default:
        matrix_multiply(gx->texture, m);
        return;
    }
    update_clip(gx);
}

/* The stacks (GBATEK "DS 3D Matrix Stack"): one projection entry, one
 * texture entry, and 31 position entries paired with 31 vector entries
 * under one 6-bit pointer. "MTX_PUSH/POP/STORE/RESTORE in Mode 1: uses BOTH
 * Position AND Vector Matrices." Entry 31 exists and works, but using it
 * (or pointing past it: 32-63 mirror 0-31) sets the error flag. */
static void matrix_push(NdsGx *gx)
{
    if (gx->matrix_mode == 0) {
        if (gx->projection_sp > 0) gx->stack_error = 1;
        memcpy(gx->projection_stack, gx->projection, sizeof gx->projection);
        gx->projection_sp ^= 1;
    } else if (gx->matrix_mode == 3) {
        if (gx->texture_sp > 0) gx->stack_error = 1;
        memcpy(gx->texture_stack, gx->texture, sizeof gx->texture);
        gx->texture_sp ^= 1;
    } else {
        if (gx->position_sp > 30) gx->stack_error = 1;
        memcpy(gx->position_stack[gx->position_sp & 31], gx->position, sizeof gx->position);
        memcpy(gx->vector_stack[gx->position_sp & 31], gx->vector, sizeof gx->vector);
        gx->position_sp = (uint8_t)((gx->position_sp + 1u) & 63u);
    }
}

static void matrix_pop(NdsGx *gx, uint32_t param)
{
    if (gx->matrix_mode == 0) {
        /* "the parameter value is ignored, the offset is always +1" */
        if (gx->projection_sp == 0) gx->stack_error = 1;
        gx->projection_sp ^= 1;
        memcpy(gx->projection, gx->projection_stack, sizeof gx->projection);
    } else if (gx->matrix_mode == 3) {
        if (gx->texture_sp == 0) gx->stack_error = 1;
        gx->texture_sp ^= 1;
        memcpy(gx->texture, gx->texture_stack, sizeof gx->texture);
        return;
    } else {
        /* "Stack Offset (signed value, -30..+31)": S = S - N, then C = [S]. */
        gx->position_sp = (uint8_t)(((uint32_t)gx->position_sp - (uint32_t)sext(param, 6)) & 63u);
        if (gx->position_sp > 30) gx->stack_error = 1;
        memcpy(gx->position, gx->position_stack[gx->position_sp & 31], sizeof gx->position);
        memcpy(gx->vector, gx->vector_stack[gx->position_sp & 31], sizeof gx->vector);
    }
    update_clip(gx);
}

static void matrix_store(NdsGx *gx, uint32_t param)
{
    if (gx->matrix_mode == 0) {
        memcpy(gx->projection_stack, gx->projection, sizeof gx->projection);
    } else if (gx->matrix_mode == 3) {
        memcpy(gx->texture_stack, gx->texture, sizeof gx->texture);
    } else {
        unsigned index = param & 31u;
        if (index > 30) gx->stack_error = 1;
        memcpy(gx->position_stack[index], gx->position, sizeof gx->position);
        memcpy(gx->vector_stack[index], gx->vector, sizeof gx->vector);
    }
}

static void matrix_restore(NdsGx *gx, uint32_t param)
{
    if (gx->matrix_mode == 0) {
        memcpy(gx->projection, gx->projection_stack, sizeof gx->projection);
    } else if (gx->matrix_mode == 3) {
        memcpy(gx->texture, gx->texture_stack, sizeof gx->texture);
        return;
    } else {
        unsigned index = param & 31u;
        if (index > 30) gx->stack_error = 1;
        memcpy(gx->position, gx->position_stack[index], sizeof gx->position);
        memcpy(gx->vector, gx->vector_stack[index], sizeof gx->vector);
    }
    update_clip(gx);
}

/* ------------------------------------------------------------------ */
/* Texture coordinates and lighting                                    */
/* ------------------------------------------------------------------ */

/* The texture coordinate transformation mode, TEXIMAGE_PARAM bits 30-31
 * (GBATEK "DS 3D Texture Coordinates"). Only the left two columns of the
 * texture matrix take part, and the result is cut to 16 bits. The shifts
 * below are melonDS's: they treat the matrix as mapping to whole texels
 * times 16, which is how programs set it up (a scale of 64.0 for a
 * 128-texel sphere map), rather than GBATEK's "shifted to a 4-bit
 * fraction". */

/* Mode 1, at TEXCOORD: (S, T, 1/16, 1/16) * M. */
static void texcoord_from_texcoord(NdsGx *gx)
{
    for (int i = 0; i < 2; i++) {
        uint64_t sum = product(gx->tex_raw[0], gx->texture[i]) + product(gx->tex_raw[1], gx->texture[4 + i]) +
                       product(1, gx->texture[8 + i]) + product(1, gx->texture[12 + i]);
        gx->tex_coord[i] = (int16_t)sext((uint32_t)(sum >> 12), 16);
    }
}

/* Modes 2 and 3, at NORMAL and at each vertex: (x, y, z) * M + (S, T). */
static void texcoord_from_vector(const NdsGx *gx, const int16_t v[3], unsigned shift, int16_t out[2])
{
    for (int i = 0; i < 2; i++) {
        uint64_t sum = column_dot(gx->texture, v, i);
        out[i] = (int16_t)sext((uint32_t)(sum >> shift) + (uint32_t)gx->tex_raw[i], 16);
    }
}

/* LIGHT_VECTOR: "the incoming vector is multiplied by the current
 * Directional Matrix". The engine keeps it negated (pointing at the light)
 * in 11 bits, and with it the reciprocal that normalises the half vector
 * for specular reflection: the line of sight is always (0, 0, -1.0), so the
 * half vector's squared length is a function of the light's z alone. Bit
 * widths and the order of negation and sign extension: melonDS. */
static void light_vector(NdsGx *gx, uint32_t param)
{
    unsigned light = param >> 30;
    int16_t dir[3] = {ten(param, 0), ten(param, 10), ten(param, 20)};
    uint64_t z = 0;
    for (int col = 0; col < 3; col++) {
        z = column_dot(gx->vector, dir, col);
        gx->light_dir[light][col] = (int16_t)sext(0u - (uint32_t)(z >> 12), 11);
    }
    int32_t denominator = 0x200 - sext((uint32_t)(z >> 12), 11);
    gx->light_recip[light] = denominator == 0 ? 0 : (1 << 18) / denominator;
}

/* NORMAL (GBATEK "Internal Operation on Normal Command"):
 *
 *   VertexColor = EmissionColor
 *   for each light enabled in the polygon attributes:
 *     DiffuseLevel   = max(0, -(LightVector * NormalVector))
 *     ShininessLevel = max(0, (-HalfVector) * NormalVector)^2, through the
 *                      shininess table if that is enabled
 *     VertexColor += SpecularColor * LightColor * ShininessLevel
 *                  + DiffuseColor * LightColor * DiffuseLevel
 *                  + AmbientColor * LightColor
 *
 * GBATEK gives no widths. The ones here are melonDS's, found by comparing
 * with hardware: the normal is 11 bits after the vector matrix; each
 * product of the dot product loses 9 bits before the sum; the diffuse level
 * wraps at 11 bits and each diffuse term at 20; the specular level is the
 * squared (dot + Nz), 10 bits, times the light's reciprocal; colours
 * accumulate with 14 fraction bits and saturate at 31. Returns how many
 * lights were on, which sets the command's duration. */
static unsigned light_normal(NdsGx *gx)
{
    int32_t n[3];
    for (int col = 0; col < 3; col++) n[col] = sext((uint32_t)(column_dot(gx->vector, gx->normal, col) >> 12), 11);

    uint32_t sum[3];
    for (int ch = 0; ch < 3; ch++) sum[ch] = (uint32_t)gx->emission[ch] << 14;

    unsigned lights = 0;
    for (unsigned light = 0; light < 4; light++) {
        if (!(gx->polygon_attr_latched & (1u << light))) continue;
        lights++;
        const int16_t *dir = gx->light_dir[light];
        int32_t dot = ((dir[0] * n[0]) >> 9) + ((dir[1] * n[1]) >> 9) + ((dir[2] * n[2]) >> 9);
        int32_t shine = 0;
        if (dot > 0) {
            int32_t diffuse = sext((uint32_t)dot, 11);
            for (int ch = 0; ch < 3; ch++) {
                int32_t term = gx->diffuse[ch] * gx->light_color[light][ch] * diffuse;
                sum[ch] += (uint32_t)term & 0xFFFFFu;
            }
            int32_t half = sext((uint32_t)(dot + n[2]), 11);
            half = ((half * half) >> 10) & 0x3FF;
            shine = ((half * gx->light_recip[light]) >> 8) - 0x200;
            if (shine > 0) shine = sext((uint32_t)shine, 14);
            if (shine < 0) shine = 0;
            if (shine > 0x1FF) shine = 0x1FF;
        }
        /* "If the table is disabled ... reflection will act as if the table
         * would be filled with linear increasing numbers." */
        if (gx->shininess_enable) shine = gx->shininess[shine >> 2] << 1;
        for (int ch = 0; ch < 3; ch++) {
            uint32_t term = (uint32_t)gx->specular[ch] * (uint32_t)shine + ((uint32_t)gx->ambient[ch] << 9);
            sum[ch] += term * gx->light_color[light][ch];
        }
    }
    for (int ch = 0; ch < 3; ch++) gx->color[ch] = (uint8_t)((sum[ch] >> 14) > 31 ? 31 : (sum[ch] >> 14));
    return lights;
}

/* ------------------------------------------------------------------ */
/* Culling                                                             */
/* ------------------------------------------------------------------ */

/* Signed 128-bit sums, for the one place 64 bits are not enough. */
typedef struct {
    uint64_t lo, hi;
} Wide;

static Wide wide_product(int64_t a, int32_t b)
{
    uint64_t ua = a < 0 ? 0u - (uint64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? 0u - (uint64_t)b : (uint64_t)b;
    uint64_t low = (ua & 0xFFFFFFFFu) * ub;
    uint64_t high = (ua >> 32) * ub;
    Wide r;
    r.lo = low + (high << 32);
    r.hi = (high >> 32) + (r.lo < low ? 1u : 0u);
    if ((a < 0) != (b < 0)) {
        r.lo = ~r.lo + 1u;
        r.hi = ~r.hi + (r.lo == 0 ? 1u : 0u);
    }
    return r;
}

static void wide_add(Wide *sum, Wide term)
{
    sum->lo += term.lo;
    sum->hi += term.hi + (sum->lo < term.lo ? 1u : 0u);
}

/* Which way a polygon faces, from its first three vertices in clip space:
 * the sign of v1 . ((v0 - v1) x (v2 - v1)) taken over (x, y, w), which is
 * minus the determinant of the three rows. Negative: front (the vertices
 * run anticlockwise on screen); positive: back; zero: edge on, or a line.
 * melonDS has the formula and cuts the cross product to 32 bits by guess;
 * this takes the sign exactly. */
static int facing(const NdsGxVertex *v0, const NdsGxVertex *v1, const NdsGxVertex *v2)
{
    const int32_t *a = v0->pos, *b = v1->pos, *c = v2->pos;
    Wide det = {0, 0};
    wide_add(&det, wide_product((int64_t)b[1] * c[3], a[0]));
    wide_add(&det, wide_product(-((int64_t)b[3] * c[1]), a[0]));
    wide_add(&det, wide_product(-((int64_t)b[0] * c[3]), a[1]));
    wide_add(&det, wide_product((int64_t)b[3] * c[0], a[1]));
    wide_add(&det, wide_product((int64_t)b[0] * c[1], a[3]));
    wide_add(&det, wide_product(-((int64_t)b[1] * c[0]), a[3]));
    if (det.hi >> 63) return 1;               /* determinant negative: the dot product is positive */
    return (det.hi | det.lo) != 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Clipping                                                            */
/* ------------------------------------------------------------------ */

/* Outside one side of the view volume: coordinate > w, or < -w. */
static inline bool outside(const NdsGxVertex *v, int axis, int side)
{
    return side > 0 ? v->pos[axis] > v->pos[3] : (int64_t)v->pos[axis] < -(int64_t)v->pos[3];
}

/* The point where the edge from an outside vertex to an inside one crosses
 * the plane (GBATEK "Clipping": "vertices are replaced by two newly created
 * vertices ... on the intersections of the polygon edges and the view
 * volume edge"). The new vertex is worked out from the outside end, with a
 * division that truncates; its clipped coordinate is then set to exactly
 * +-w. Colours stay 5-bit, which is what hardware shows (melonDS: "kept at
 * 5-bit during clipping"); how the hardware rounds them is not known, and
 * this truncates. */
static void clip_edge(NdsGxVertex *out, const NdsGxVertex *from, const NdsGxVertex *to, int axis, int side,
                      bool attributes)
{
    /* How far out `from` is, and that plus how far in `to` is: both
     * positive, the first no larger than the second. */
    int64_t part = side * (int64_t)from->pos[axis] - from->pos[3];
    int64_t whole = part + ((int64_t)to->pos[3] - side * (int64_t)to->pos[axis]);
    /* Coordinates this large (2^29 and up) are garbage; halving both keeps
     * the products below inside 64 bits. */
    while (whole >= ((int64_t)1 << 30)) {
        part >>= 1;
        whole >>= 1;
    }

    *out = *from;
    out->clipped = 1;
    for (int i = 0; i < 4; i++) {
        if (i == axis) continue;
        int64_t delta = (int64_t)to->pos[i] - from->pos[i];
        out->pos[i] = (int32_t)(from->pos[i] + delta * part / whole);
    }
    out->pos[axis] = side > 0 ? out->pos[3] : s32(0u - (uint32_t)out->pos[3]);
    if (!attributes) return;
    for (int i = 0; i < 3; i++) {
        int32_t delta = to->color[i] - from->color[i];
        out->color[i] = (uint8_t)(from->color[i] + (int32_t)(delta * part / whole));
    }
    for (int i = 0; i < 2; i++) {
        int64_t delta = (int64_t)to->tex[i] - from->tex[i];
        out->tex[i] = (int16_t)sext((uint32_t)(from->tex[i] + delta * part / whole), 16);
    }
}

/* Clip a polygon against one side of the view volume, edge by edge: an
 * edge contributes its first vertex if that is inside, and then the
 * crossing if it has one. So when the first vertex is outside, the clipped
 * polygon starts with the crossing of its first edge and ends with that of
 * its last. The order matters to the rasteriser, which treats the first,
 * second and last vertices of a polygon specially; polyrastertest's "Until
 * the first vertex gets clipped" scenes (62-67) show this order. Convex
 * polygons gain at most one vertex per plane; one that is not convex could
 * gain more, and is cut off at the size of the buffer. */
static unsigned clip_side(const NdsGxVertex *in, unsigned count, NdsGxVertex *out, int axis, int side,
                          bool attributes)
{
    unsigned made = 0;
    for (unsigned i = 0; i < count; i++) {
        const NdsGxVertex *v = &in[i];
        const NdsGxVertex *next = &in[i + 1 < count ? i + 1 : 0];
        bool v_out = outside(v, axis, side), next_out = outside(next, axis, side);
        if (!v_out && made < NDS_GX_POLYGON_VERTICES) out[made++] = *v;
        if (v_out != next_out && made < NDS_GX_POLYGON_VERTICES)
            clip_edge(&out[made++], v_out ? v : next, v_out ? next : v, axis, side, attributes);
    }
    return made;
}

/* Clip against all six sides: far and near, then top and bottom, then right
 * and left (melonDS: "clipping seems to process the Y plane before the X
 * plane"). Returns the number of vertices left, 0 if nothing is.
 * POLYGON_ATTR bit 12 clear: "Far-plane intersecting polygons" are hidden
 * whole, not clipped. */
static unsigned clip_polygon(NdsGxVertex poly[NDS_GX_POLYGON_VERTICES], unsigned count, bool clip_far,
                             bool attributes)
{
    NdsGxVertex other[NDS_GX_POLYGON_VERTICES];
    for (int axis = 2; axis >= 0; axis--) {
        if (axis == 2 && !clip_far) {
            for (unsigned i = 0; i < count; i++)
                if (outside(&poly[i], 2, 1)) return 0;
        }
        count = clip_side(poly, count, other, axis, 1, attributes);
        count = clip_side(other, count, poly, axis, -1, attributes);
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Polygons into RAM                                                   */
/* ------------------------------------------------------------------ */

/* The viewport transform (GBATEK "Notes on VTX commands"):
 *
 *   screen_x = (xx + ww) * viewport_width / (2 * ww) + viewport_x1
 *   screen_y = (yy + ww) * viewport_height / (2 * ww) + viewport_y1
 *
 * with y counted from the top of the screen here. melonDS: W is cut to 24
 * bits first; the divider is 32 bits wide, so when W does not fit 16 bits
 * the numerator and denominator are halved, losing a bit; the results wrap
 * at 9 and 8 bits. A vertex with W = 0 goes to the origin, and its polygon
 * is not drawn.
 *
 * The remainders of the two divisions are kept as fractions of a pixel (fx,
 * fy): nothing the console draws uses them, and a picture drawn larger than
 * the screen puts its vertices where they say. */
static void to_screen(const NdsGx *gx, NdsGxVertex *v)
{
    uint32_t w = (uint32_t)v->pos[3] & 0xFFFFFFu;
    v->pos[3] = (int32_t)w;
    uint32_t x = 0, y = 0;
    v->fx = v->fy = 0;
    if (w != 0) {
        x = (uint32_t)v->pos[0] + w;
        y = w - (uint32_t)v->pos[1];
        uint32_t divisor = w;
        if (w > 0xFFFF) {
            x >>= 1;
            y >>= 1;
            divisor >>= 1;
        }
        divisor <<= 1;
        x *= gx->viewport_width;
        y *= gx->viewport_height;
        v->fx = (uint8_t)(((uint64_t)(x % divisor) << 8) / divisor);
        v->fy = (uint8_t)(((uint64_t)(y % divisor) << 8) / divisor);
        x = x / divisor + gx->viewport_x;
        y = y / divisor + gx->viewport_y;
    }
    v->sx = (int16_t)(x & 0x1FFu);
    v->sy = (int16_t)(y & 0xFFu);
}

/* A complete polygon: `n` vertices in the order the hardware takes them.
 * Culled and clipped away polygons leave nothing; the rest go into polygon
 * list RAM and vertex RAM with everything the rasteriser needs. */
static void submit_polygon(NdsGx *gx, const NdsGxVertex *const corner[4], unsigned n)
{
    uint32_t attr = gx->polygon_attr_latched;
    NdsGxVertex *ram = gx->vertices[gx->geometry_bank];
    bool strip = gx->prim_type >= 2;
    /* Along a strip, the two vertices shared with the polygon before are
     * the same vertex RAM entries, as long as that polygon went in whole. */
    bool linked = strip && gx->strip_linked;
    gx->strip_linked = 0;

    /* POLYGON_ATTR bits 6-7: "Polygon Back Surface", "Polygon Front
     * Surface". A polygon seen edge on, and a line, is neither. */
    int side = facing(corner[0], corner[1], corner[2]);
    if (side < 0 && !(attr & 0x80u)) return;
    if (side > 0 && !(attr & 0x40u)) return;

    NdsGxVertex poly[NDS_GX_POLYGON_VERTICES];
    for (unsigned i = 0; i < n; i++) poly[i] = *corner[i];
    if (linked) {
        poly[0] = ram[gx->strip_vertex[0]];
        poly[1] = ram[gx->strip_vertex[1]];
    }
    unsigned count = clip_polygon(poly, n, (attr & 0x1000u) != 0, true);
    if (count == 0) return;

    /* DISP3DCNT bit 13: "Polygon/Vertex RAM Overflow". The test is on the
     * clipped polygon's whole vertex count (melonDS; polyrastertest's
     * vertex limit scenes rely on a clipped quad fitting where the whole
     * one would not). */
    if (gx->polygon_count >= NDS_GX_MAX_POLYGONS || gx->vertex_count + count > NDS_GX_MAX_VERTICES) {
        gx->disp3dcnt |= 0x2000u;
        return;
    }

    /* Shared vertices keep the screen position they were given. */
    for (unsigned i = linked ? 2u : 0u; i < count; i++) to_screen(gx, &poly[i]);

    /* DISP_1DOT_DEPTH: a polygon whose vertices all land on one pixel is
     * hidden when every vertex is farther than the limit, unless
     * POLYGON_ATTR bit 13 says to draw it. The limit is 12.3 fixed point
     * against W's 12.12 (melonDS: its low 9 bits read as ones). */
    if (!(attr & 0x2000u)) {
        uint32_t limit = (uint32_t)gx->one_dot_depth * 0x200u + 0x1FFu;
        bool hidden = true;
        for (unsigned i = 0; i < count && hidden; i++) {
            hidden = poly[i].sx == poly[0].sx && poly[i].sy == poly[0].sy && (uint32_t)poly[i].pos[3] > limit;
        }
        if (hidden) return;
    }

    NdsGxPolygon *p = &gx->polygons[gx->geometry_bank][gx->polygon_count++];
    memset(p, 0, sizeof *p);
    p->attr = attr;
    p->tex_param = gx->tex_param;
    p->palette = gx->tex_palette;
    p->count = (uint8_t)count;

    /* A strip polygon that clipping changed becomes a polygon of its own
     * (GBATEK "Clipping": "clipped polygons which are part of polygon
     * strips are converted to separate polygons"). */
    bool share = linked && count == n;
    for (unsigned i = 0; i < count; i++) {
        if (share && i < 2) {
            p->vertex[i] = gx->strip_vertex[i];
        } else {
            p->vertex[i] = gx->vertex_count;
            ram[gx->vertex_count++] = poly[i];
        }
    }

    /* The top vertex is the first with the smallest y; the bottom one the
     * first with the largest y and, among those, the largest x (melonDS). */
    int y_top = 192, y_bottom = 0, x_bottom = 0;
    unsigned w_bits = 0;
    for (unsigned i = 0; i < count; i++) {
        const NdsGxVertex *v = &poly[i];
        if (v->sy < y_top) {
            y_top = v->sy;
            p->top = (uint8_t)i;
        }
        if (v->sy > y_bottom || (v->sy == y_bottom && v->sx > x_bottom)) {
            y_bottom = v->sy;
            x_bottom = v->sx;
            p->bottom = (uint8_t)i;
        }
        if (v->pos[3] == 0) p->flags |= NDS_GX_POLY_DEGENERATE;
        while (w_bits < 32 && ((uint32_t)v->pos[3] >> w_bits) != 0) w_bits += 4;
    }
    p->y_top = (uint8_t)y_top;
    p->y_bottom = (uint8_t)y_bottom;
    if (y_bottom > 192) p->flags |= NDS_GX_POLY_DEGENERATE;

    if (side <= 0) p->flags |= NDS_GX_POLY_FRONT;
    /* Translucent polygons are drawn after the opaque ones: alpha 1-30, or
     * a texture format with its own alpha (A3I5, A5I3). */
    unsigned format = (gx->tex_param >> 26) & 7u;
    unsigned alpha = (attr >> 16) & 31u;
    if (format == 1 || format == 6 || (alpha > 0 && alpha < 31)) p->flags |= NDS_GX_POLY_TRANSLUCENT;
    /* GBATEK "DS 3D Shadow Polygons": polygon mode 3 with ID 0 is the mask,
     * with any other ID the shadow itself. */
    if ((attr & 0x30u) == 0x30u) p->flags |= (attr & 0x3F000000u) ? NDS_GX_POLY_SHADOW : NDS_GX_POLY_SHADOW_MASK;
    /* SWAP_BUFFERS bit 1, from the swap before this polygon: "Depth
     * Buffering (0=With Z-value, 1=With W-value)". */
    bool w_buffer = (gx->swap_mode & 2u) != 0;
    if (w_buffer) p->flags |= NDS_GX_POLY_W_BUFFER;

    /* Depth and W per vertex (melonDS). The W values are normalised per
     * polygon: shifted, in steps of 4 bits, so that the largest just fits
     * 16 bits. The interpolators work from those. The depth is W itself,
     * less the bits a right shift dropped, or from Z:
     * (z * 0x4000 / w + 0x3FFF) * 0x200, kept to 24 bits. */
    for (unsigned i = 0; i < count; i++) {
        const NdsGxVertex *v = &poly[i];
        uint32_t w = (uint32_t)v->pos[3];
        uint32_t normal, from_w;
        if (w_bits < 16) {
            normal = w << (16 - w_bits);
            from_w = w;
        } else {
            normal = w >> (w_bits - 16);
            from_w = normal << (w_bits - 16);
        }
        int64_t depth;
        if (w_buffer) depth = from_w;
        else if (w != 0) depth = ((int64_t)v->pos[2] * 0x4000 / (int64_t)w + 0x3FFF) * 0x200;
        else depth = 0x7FFE00;
        if (depth < 0) depth = 0;
        if (depth > 0xFFFFFF) depth = 0xFFFFFF;
        p->w[i] = (uint16_t)normal;
        p->depth[i] = (uint32_t)depth;
    }

    /* What the next polygon of the strip shares, in its own order: a
     * triangle strip alternates (2, 1) and (0, 2); a quad strip takes
     * (3, 2). Vertices the clipper made are not shared. */
    if (strip && count == n) {
        unsigned first, second;
        if (gx->prim_type == 3) {
            first = 3;
            second = 2;
        } else if (gx->strip_odd) {
            first = 0;
            second = 2;
        } else {
            first = 2;
            second = 1;
        }
        if (!poly[first].clipped && !poly[second].clipped) {
            gx->strip_vertex[0] = p->vertex[first];
            gx->strip_vertex[1] = p->vertex[second];
            gx->strip_linked = 1;
        }
    }
}

/* A VTX command: transform the vertex and, when it completes a polygon,
 * submit that (GBATEK "DS 3D Polygon Definitions by Vertices"). Triangle
 * strips alternate winding, so every second triangle takes its first two
 * vertices in the other order; a quad strip's quads are (0, 1, 3, 2). */
static void submit_vertex(NdsGx *gx)
{
    NdsGxVertex *v = &gx->prim[gx->prim_count];
    memset(v, 0, sizeof *v);
    transform_point(gx->clip, gx->vertex, v->pos);
    memcpy(v->color, gx->color, sizeof v->color);
    /* Mode 3, "Vertex source": (Vx, Vy, Vz) * M + (S, T). */
    if ((gx->tex_param >> 30) == 3) texcoord_from_vector(gx, gx->vertex, 24, v->tex);
    else memcpy(v->tex, gx->tex_coord, sizeof v->tex);
    gx->prim_count++;

    const NdsGxVertex *corner[4] = {&gx->prim[0], &gx->prim[1], &gx->prim[2], &gx->prim[3]};
    switch (gx->prim_type) {
    case 0:
        if (gx->prim_count < 3) return;
        submit_polygon(gx, corner, 3);
        gx->prim_count = 0;
        break;
    case 1:
        if (gx->prim_count < 4) return;
        submit_polygon(gx, corner, 4);
        gx->prim_count = 0;
        break;
    case 2:
        if (gx->prim_count < 3) return;
        if (gx->strip_odd) {
            corner[0] = &gx->prim[1];
            corner[1] = &gx->prim[0];
        }
        submit_polygon(gx, corner, 3);
        gx->strip_odd ^= 1;
        gx->prim[0] = gx->prim[1];
        gx->prim[1] = gx->prim[2];
        gx->prim_count = 2;
        break;
    default:
        if (gx->prim_count < 4) return;
        corner[2] = &gx->prim[3];
        corner[3] = &gx->prim[2];
        submit_polygon(gx, corner, 4);
        gx->prim[0] = gx->prim[2];
        gx->prim[1] = gx->prim[3];
        gx->prim_count = 2;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* BOX_TEST (GBATEK "DS 3D Tests"): "indicates if one or more of the 6 faces
 * of the box are fully or parts of inside of the view volume". Each face
 * goes through the polygon clipper, far-plane rule included, which is why
 * GBATEK says to enable far-plane-intersecting polygons first. */
static void box_test(NdsGx *gx, const uint32_t *p)
{
    int16_t origin[3] = {(int16_t)sext(p[0], 16), (int16_t)sext(p[0] >> 16, 16), (int16_t)sext(p[1], 16)};
    int16_t size[3] = {(int16_t)sext(p[1] >> 16, 16), (int16_t)sext(p[2], 16), (int16_t)sext(p[2] >> 16, 16)};
    /* Corner i has the far x, y, z where bits 0, 1, 2 of i are set. "The
     * result of the coordinate+offset additions should not overflow": it
     * wraps at 16 bits. */
    NdsGxVertex corner[8];
    memset(corner, 0, sizeof corner);
    for (unsigned i = 0; i < 8; i++) {
        int16_t at[3];
        for (unsigned axis = 0; axis < 3; axis++)
            at[axis] = (i >> axis) & 1u ? (int16_t)sext((uint32_t)(origin[axis] + size[axis]), 16) : origin[axis];
        transform_point(gx->clip, at, corner[i].pos);
    }
    static const uint8_t faces[6][4] = {
        {0, 1, 3, 2}, {4, 6, 7, 5}, {0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6},
    };
    gx->box_result = 0;
    for (unsigned f = 0; f < 6 && !gx->box_result; f++) {
        NdsGxVertex face[NDS_GX_POLYGON_VERTICES];
        for (unsigned i = 0; i < 4; i++) face[i] = corner[faces[f][i]];
        if (clip_polygon(face, 4, (gx->polygon_attr_latched & 0x1000u) != 0, false) != 0) gx->box_result = 1;
    }
}

/* VEC_TEST: "Multiplies the specified line-vector (x,y,z,0) by the
 * directional vector matrix." The result is 4.12 with no integer part:
 * "+1.0 aka 1000h will be returned as -1.0 aka F000h". */
static void vec_test(NdsGx *gx, uint32_t param)
{
    int16_t v[3] = {ten(param, 0), ten(param, 10), ten(param, 20)};
    for (int col = 0; col < 3; col++)
        gx->vec_result[col] = (int16_t)sext((uint32_t)(column_dot(gx->vector, v, col) >> 9), 13);
}

/* ------------------------------------------------------------------ */
/* Command execution                                                   */
/* ------------------------------------------------------------------ */

static void set_viewport(NdsGx *gx, uint32_t param)
{
    /* GBATEK VIEWPORT: X1, Y1, X2, Y2 with (0, 0) the lower left. Lines
     * are counted from the top here, so the top line is 191 - Y2. */
    unsigned x1 = param & 0xFFu, y1 = (param >> 8) & 0xFFu, x2 = (param >> 16) & 0xFFu, y2 = param >> 24;
    unsigned top = (191u - y2) & 0xFFu, bottom = (191u - y1) & 0xFFu;
    gx->viewport_x = (uint16_t)x1;
    gx->viewport_width = (uint16_t)((x2 - x1 + 1u) & 0x1FFu);
    gx->viewport_y = (uint8_t)top;
    gx->viewport_height = (uint8_t)((bottom - top + 1u) & 0xFFu);
}

static void set_vertex(NdsGx *gx, int16_t x, int16_t y, int16_t z)
{
    gx->vertex[0] = x;
    gx->vertex[1] = y;
    gx->vertex[2] = z;
}

/* Carry out a command whose parameters are all here. Returns how long it
 * takes in all, in cycles of the 33.51 MHz clock: the "Cy." column of
 * GBATEK's geometry command table, plus what its notes add. */
static unsigned run_command(NdsGx *gx, unsigned cmd, const uint32_t *p)
{
    int32_t m[16];
    /* "In MTX_MODE=2 (Simultanous Set), MTX_MULT/TRANS take additional 30
     * cycles." */
    unsigned both = gx->matrix_mode == 2 ? 30u : 0u;

    switch (cmd) {
    case 0x10: /* MTX_MODE */
        gx->matrix_mode = (uint8_t)(p[0] & 3u);
        return 1;
    case 0x11: /* MTX_PUSH */
        matrix_push(gx);
        return 17;
    case 0x12: /* MTX_POP */
        matrix_pop(gx, p[0]);
        return 36;
    case 0x13: /* MTX_STORE */
        matrix_store(gx, p[0]);
        return 17;
    case 0x14: /* MTX_RESTORE */
        matrix_restore(gx, p[0]);
        return 36;
    case 0x15: /* MTX_IDENTITY */
        matrix_load(gx, identity);
        return 19;
    case 0x16: /* MTX_LOAD_4x4 */
        matrix_expand(m, MATRIX_4X4, p);
        matrix_load(gx, m);
        return 34;
    case 0x17: /* MTX_LOAD_4x3 */
        matrix_expand(m, MATRIX_4X3, p);
        matrix_load(gx, m);
        return 30;
    case 0x18: /* MTX_MULT_4x4 */
        matrix_expand(m, MATRIX_4X4, p);
        matrix_apply(gx, m, false);
        return 35 + both;
    case 0x19: /* MTX_MULT_4x3 */
        matrix_expand(m, MATRIX_4X3, p);
        matrix_apply(gx, m, false);
        return 31 + both;
    case 0x1A: /* MTX_MULT_3x3 */
        matrix_expand(m, MATRIX_3X3, p);
        matrix_apply(gx, m, false);
        return 28 + both;
    case 0x1B: /* MTX_SCALE */
        matrix_expand(m, MATRIX_SCALE, p);
        matrix_apply(gx, m, true);
        return 22;
    case 0x1C: /* MTX_TRANS */
        matrix_expand(m, MATRIX_TRANSLATE, p);
        matrix_apply(gx, m, false);
        return 22 + both;

    case 0x20: /* COLOR */
        gx->color[0] = five(p[0], 0);
        gx->color[1] = five(p[0], 5);
        gx->color[2] = five(p[0], 10);
        return 1;
    case 0x21: { /* NORMAL */
        gx->normal[0] = ten(p[0], 0);
        gx->normal[1] = ten(p[0], 10);
        gx->normal[2] = ten(p[0], 20);
        /* Mode 2, "Normal source": (Nx, Ny, Nz) * M + (S, T). */
        if ((gx->tex_param >> 30) == 2) texcoord_from_vector(gx, gx->normal, 21, gx->tex_coord);
        unsigned lights = light_normal(gx);
        /* "NORMAL commands takes 9..12 cycles, depending on the number of
         * enabled lights": four timings for five counts, so none and one
         * take the same (melonDS). */
        return 9 + (lights > 1 ? lights - 1 : 0);
    }
    case 0x22: /* TEXCOORD */
        gx->tex_raw[0] = (int16_t)sext(p[0], 16);
        gx->tex_raw[1] = (int16_t)sext(p[0] >> 16, 16);
        if ((gx->tex_param >> 30) == 1) texcoord_from_texcoord(gx);
        else memcpy(gx->tex_coord, gx->tex_raw, sizeof gx->tex_coord);
        return 1;
    case 0x23: /* VTX_16 */
        set_vertex(gx, (int16_t)sext(p[0], 16), (int16_t)sext(p[0] >> 16, 16), (int16_t)sext(p[1], 16));
        submit_vertex(gx);
        return 9;
    case 0x24: /* VTX_10: 6 fraction bits */
        set_vertex(gx, (int16_t)(ten(p[0], 0) * 64), (int16_t)(ten(p[0], 10) * 64), (int16_t)(ten(p[0], 20) * 64));
        submit_vertex(gx);
        return 8;
    case 0x25: /* VTX_XY */
        set_vertex(gx, (int16_t)sext(p[0], 16), (int16_t)sext(p[0] >> 16, 16), gx->vertex[2]);
        submit_vertex(gx);
        return 8;
    case 0x26: /* VTX_XZ */
        set_vertex(gx, (int16_t)sext(p[0], 16), gx->vertex[1], (int16_t)sext(p[0] >> 16, 16));
        submit_vertex(gx);
        return 8;
    case 0x27: /* VTX_YZ */
        set_vertex(gx, gx->vertex[0], (int16_t)sext(p[0], 16), (int16_t)sext(p[0] >> 16, 16));
        submit_vertex(gx);
        return 8;
    case 0x28: /* VTX_DIFF: 10-bit steps of 1/4096, added with 16-bit wrap */
        set_vertex(gx, (int16_t)sext((uint32_t)(gx->vertex[0] + ten(p[0], 0)), 16),
                   (int16_t)sext((uint32_t)(gx->vertex[1] + ten(p[0], 10)), 16),
                   (int16_t)sext((uint32_t)(gx->vertex[2] + ten(p[0], 20)), 16));
        submit_vertex(gx);
        return 8;
    case 0x29: /* POLYGON_ATTR: "no effect until next BEGIN_VTXS" */
        gx->polygon_attr = p[0];
        return 1;
    case 0x2A: /* TEXIMAGE_PARAM */
        gx->tex_param = p[0];
        return 1;
    case 0x2B: /* PLTT_BASE */
        gx->tex_palette = (uint16_t)(p[0] & 0x1FFFu);
        return 1;

    case 0x30: /* DIF_AMB */
        for (unsigned ch = 0; ch < 3; ch++) {
            gx->diffuse[ch] = five(p[0], 5 * ch);
            gx->ambient[ch] = five(p[0], 16 + 5 * ch);
        }
        /* Bit 15: "Set Diffuse Reflection Color as Vertex Color". */
        if (p[0] & 0x8000u) memcpy(gx->color, gx->diffuse, sizeof gx->color);
        return 4;
    case 0x31: /* SPE_EMI */
        for (unsigned ch = 0; ch < 3; ch++) {
            gx->specular[ch] = five(p[0], 5 * ch);
            gx->emission[ch] = five(p[0], 16 + 5 * ch);
        }
        gx->shininess_enable = (uint8_t)((p[0] >> 15) & 1u);
        return 4;
    case 0x32: /* LIGHT_VECTOR */
        light_vector(gx, p[0]);
        return 6;
    case 0x33: /* LIGHT_COLOR */
        for (unsigned ch = 0; ch < 3; ch++) gx->light_color[p[0] >> 30][ch] = five(p[0], 5 * ch);
        return 1;
    case 0x34: /* SHININESS: 128 entries, four to a word, lowest first */
        for (unsigned i = 0; i < 128; i++) gx->shininess[i] = (uint8_t)(p[i >> 2] >> (8 * (i & 3u)));
        return 32;

    case 0x40: /* BEGIN_VTXS: "additionally applies changes to POLYGON_ATTR" */
        gx->prim_type = (uint8_t)(p[0] & 3u);
        gx->prim_count = 0;
        gx->strip_odd = 0;
        gx->strip_linked = 0;
        gx->polygon_attr_latched = gx->polygon_attr;
        return 1;
    case 0x41: /* END_VTXS: "On real NDS consoles ... it does have no effect" */
        return 1;
    case 0x50: /* SWAP_BUFFERS: "isn't executed until next VBlank ... (the Geometry Engine is halted for that
                  duration)" */
        gx->swap_param = (uint8_t)(p[0] & 3u);
        gx->swap_wait = 1;
        return 0;
    case 0x60: /* VIEWPORT */
        set_viewport(gx, p[0]);
        return 1;

    case 0x70: /* BOX_TEST */
        box_test(gx, p);
        return 103;
    case 0x71: /* POS_TEST: "overwrites the internal VTX registers" */
        set_vertex(gx, (int16_t)sext(p[0], 16), (int16_t)sext(p[0] >> 16, 16), (int16_t)sext(p[1], 16));
        transform_point(gx->clip, gx->vertex, gx->pos_result);
        return 9;
    case 0x72: /* VEC_TEST */
        vec_test(gx, p[0]);
        return 5;

    default: /* NOP and the undefined codes: "simply ignored" */
        return 1;
    }
}

/* Take one entry from the PIPE. Each entry takes a cycle to read; a
 * command runs when its last parameter has been read and takes the rest of
 * its time then. */
static void execute_entry(NdsGx *gx)
{
    unsigned cmd;
    uint32_t param;
    queue_pop(gx, &cmd, &param);

    unsigned need = command_params(cmd);
    unsigned cycles;
    if (need > 1) {
        /* A command port can be written fewer times than its command needs
         * before another port is; what was collected is then dropped. */
        if (gx->exec_count != 0 && gx->exec_cmd != cmd) gx->exec_count = 0;
        gx->exec_cmd = (uint8_t)cmd;
        gx->exec_param[gx->exec_count++] = param;
        if (gx->exec_count < need) {
            gx->busy = 1;
            return;
        }
        gx->exec_count = 0;
        cycles = run_command(gx, cmd, gx->exec_param) - (need - 1);
    } else {
        gx->exec_count = 0;
        cycles = run_command(gx, cmd, &param);
    }
    if (gx->swap_wait) return;
    gx->busy = cycles;
    gx->exec_test = is_test(cmd);
    gx->exec_stack = is_stack(cmd);
}

/* The engine takes the next entry the moment it is free. */
static void pump(NdsGx *gx)
{
    while (!gx->swap_wait && gx->busy == 0 && gx->pipe_count > 0) execute_entry(gx);
}

void nds_gx_run(NdsGx *gx, uint32_t cycles)
{
    while (cycles > 0 && gx->busy > 0) {
        uint32_t step = gx->busy < cycles ? gx->busy : cycles;
        gx->busy -= step;
        cycles -= step;
        if (gx->busy == 0) {
            gx->exec_test = 0;
            gx->exec_stack = 0;
            pump(gx);
        }
    }
}

uint32_t nds_gx_cycles_until_event(const NdsGx *gx)
{
    return gx->busy;
}

bool nds_gx_irq(const NdsGx *gx)
{
    /* GXSTAT bits 30-31: "Command FIFO IRQ (0=Never, 1=Less than half full,
     * 2=Empty, 3=Reserved)"; the request stands "while and as long as the
     * IRQ condition is true". */
    if (gx->irq_mode == 1) return gx->fifo_count < NDS_GX_FIFO_ENTRIES / 2;
    if (gx->irq_mode == 2) return gx->fifo_count == 0;
    return false;
}

bool nds_gx_dma_request(const NdsGx *gx)
{
    /* "DMA starts when the FIFO becomes less than half full". */
    return gx->fifo_count < NDS_GX_FIFO_ENTRIES / 2;
}

bool nds_gx_fifo_full(const NdsGx *gx)
{
    return gx->fifo_count >= NDS_GX_FIFO_ENTRIES;
}

/* ------------------------------------------------------------------ */
/* VBlank: swap buffers                                                */
/* ------------------------------------------------------------------ */

/* A stable sort of polygon indexes by one of their bytes. */
static void sort_by(const NdsGxPolygon *polygons, uint16_t *list, unsigned count, bool by_bottom)
{
    uint16_t sorted[NDS_GX_MAX_POLYGONS];
    uint16_t start[257];
    memset(start, 0, sizeof start);
    for (unsigned i = 0; i < count; i++) {
        const NdsGxPolygon *p = &polygons[list[i]];
        start[(by_bottom ? p->y_bottom : p->y_top) + 1u]++;
    }
    for (unsigned i = 1; i < 257; i++) start[i] = (uint16_t)(start[i] + start[i - 1]);
    for (unsigned i = 0; i < count; i++) {
        const NdsGxPolygon *p = &polygons[list[i]];
        sorted[start[by_bottom ? p->y_bottom : p->y_top]++] = list[i];
    }
    memcpy(list, sorted, count * sizeof list[0]);
}

/* The order the rendering engine draws in: opaque polygons first, then the
 * translucent ones. Opaque polygons are always sorted, by their bottom line
 * and then their top line, earlier polygons first among equals (melonDS).
 * SWAP_BUFFERS bit 0: "Translucent polygon Y-sorting (0=Auto-sort,
 * 1=Manual-sort)": sorted the same way, or left as they were sent. */
static void build_render_order(NdsGx *gx)
{
    const NdsGxPolygon *polygons = gx->polygons[gx->geometry_bank];
    unsigned opaque = 0, count = gx->polygon_count;
    for (unsigned i = 0; i < count; i++)
        if (!(polygons[i].flags & NDS_GX_POLY_TRANSLUCENT)) gx->render_order[opaque++] = (uint16_t)i;
    unsigned translucent = opaque;
    for (unsigned i = 0; i < count; i++)
        if (polygons[i].flags & NDS_GX_POLY_TRANSLUCENT) gx->render_order[translucent++] = (uint16_t)i;

    sort_by(polygons, gx->render_order, opaque, false);
    sort_by(polygons, gx->render_order, opaque, true);
    if (!(gx->swap_mode & 1u)) {
        sort_by(polygons, gx->render_order + opaque, count - opaque, false);
        sort_by(polygons, gx->render_order + opaque, count - opaque, true);
    }
    gx->render_count = (uint16_t)count;
}

void nds_gx_vblank(NdsGx *gx)
{
    /* The rendering engine works from the registers as they are now, not as
     * they are when a line is drawn (melonDS: "several registers are latched
     * upon VBlank"). */
    gx->latched_disp3dcnt = gx->disp3dcnt;
    memcpy(gx->latched_io, gx->render_io, sizeof gx->latched_io);

    if (!gx->swap_wait) return;
    /* GBATEK "Swap Buffers": the new polygons and vertices pass to the
     * rendering engine; the other bank is emptied for the geometry engine.
     * "The two parameter bits of the SwapBuffers command are used for the
     * following gxcommands (ie. not for the old gxcommands prior to
     * SwapBuffers)", so the list just finished is ordered by the swap
     * before this one. */
    build_render_order(gx);
    gx->geometry_bank ^= 1;
    gx->polygon_count = 0;
    gx->vertex_count = 0;
    gx->strip_linked = 0;
    gx->swap_mode = gx->swap_param;
    gx->swap_wait = 0;
    /* "Total execution time of SwapBuffers is Duration until VBlank, plus
     * 392 cycles." */
    gx->busy = 392;
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

/* GXSTAT (GBATEK "DS 3D Status"). */
static uint32_t gxstat(const NdsGx *gx)
{
    uint32_t value = 0;
    bool collecting = gx->exec_count != 0;
    if (gx->queued_tests || gx->exec_test || (collecting && is_test(gx->exec_cmd))) value |= 1u << 0;
    if (gx->box_result) value |= 1u << 1;
    value |= (uint32_t)(gx->position_sp & 31u) << 8;
    value |= (uint32_t)(gx->projection_sp & 1u) << 13;
    if (gx->queued_stack || gx->exec_stack) value |= 1u << 14;
    if (gx->stack_error) value |= 1u << 15;
    value |= (uint32_t)gx->fifo_count << 16;
    if (gx->fifo_count < NDS_GX_FIFO_ENTRIES / 2) value |= 1u << 25;
    if (gx->fifo_count == 0) value |= 1u << 26;
    if (gx->busy || gx->swap_wait || gx->queue_count) value |= 1u << 27;
    value |= (uint32_t)gx->irq_mode << 30;
    return value;
}

/* The register that holds `addr`, as the 32-bit word at addr & ~3. */
static uint32_t read_word(const NdsGx *gx, uint32_t addr)
{
    if (addr == 0x04000060u) return gx->disp3dcnt;
    /* RDLINES_COUNT: "Minimum Number (minus 2) of buffered lines in previous
     * frame (0..46)". Rendering here never falls behind the display. */
    if (addr == 0x04000320u) return 46;
    if (addr == 0x04000600u) return gxstat(gx);
    /* RAM_COUNT: polygons in bits 0-11, vertices in bits 16-28. */
    if (addr == 0x04000604u) return gx->polygon_count | ((uint32_t)gx->vertex_count << 16);
    if (addr >= 0x04000620u && addr < 0x04000630u) return (uint32_t)gx->pos_result[(addr - 0x04000620u) >> 2];
    if (addr == 0x04000630u) return (uint16_t)gx->vec_result[0] | ((uint32_t)(uint16_t)gx->vec_result[1] << 16);
    if (addr == 0x04000634u) return (uint16_t)gx->vec_result[2];
    if (addr >= 0x04000640u && addr < 0x04000680u) return (uint32_t)gx->clip[(addr - 0x04000640u) >> 2];
    /* VECMTX_RESULT: the vector matrix's 3x3, row by row. */
    if (addr >= 0x04000680u && addr < 0x040006A4u) {
        unsigned index = (addr - 0x04000680u) >> 2;
        return (uint32_t)gx->vector[index / 3 * 4 + index % 3];
    }
    return 0;
}

uint32_t nds_gx_read(NdsGx *gx, uint32_t addr, int size)
{
    uint32_t value = 0;
    for (int i = 0; i < size && i < 4; i++) {
        uint32_t at = addr + (uint32_t)i;
        value |= ((read_word(gx, at & ~3u) >> (8 * (at & 3u))) & 0xFFu) << (8 * i);
    }
    return value;
}

static void write_byte(NdsGx *gx, uint32_t addr, uint8_t value)
{
    if (addr >= 0x04000320u && addr < 0x040003C0u) {
        gx->render_io[addr - 0x04000320u] = value;
        return;
    }
    switch (addr) {
    case 0x04000060u:
        gx->disp3dcnt = (uint16_t)((gx->disp3dcnt & 0xFF00u) | value);
        break;
    case 0x04000061u: {
        /* DISP3DCNT bits 8-11 and 14 are read/write; bits 12 (line buffer
         * underflow) and 13 (polygon/vertex RAM overflow) are set by the
         * engine and cleared by writing 1. */
        uint16_t kept = (uint16_t)(gx->disp3dcnt & 0x3000u & ~((uint32_t)value << 8));
        gx->disp3dcnt = (uint16_t)((gx->disp3dcnt & 0x00FFu) | (((uint32_t)value << 8) & 0x4F00u) | kept);
        break;
    }
    case 0x04000601u:
        /* GXSTAT bit 15: "Writing 1 ... does reset the Error Flag, and
         * additionally resets the Projection Stack Pointer, and probably (?)
         * also the Texture Stack Pointer". */
        if (value & 0x80u) {
            gx->stack_error = 0;
            gx->projection_sp = 0;
            gx->texture_sp = 0;
        }
        break;
    case 0x04000603u:
        gx->irq_mode = (uint8_t)(value >> 6);
        break;
    case 0x04000610u:
        gx->one_dot_depth = (uint16_t)((gx->one_dot_depth & 0x7F00u) | value);
        break;
    case 0x04000611u:
        gx->one_dot_depth = (uint16_t)((gx->one_dot_depth & 0x00FFu) | (((uint32_t)value << 8) & 0x7F00u));
        break;
    default:
        break;
    }
}

void nds_gx_write(NdsGx *gx, uint32_t addr, uint32_t value, int size)
{
    if (addr >= 0x04000400u && addr < 0x04000600u) {
        /* GXFIFO (mirrored over 0x400-0x43F, so that STM can send a command
         * and its parameters) and the command ports, where each write is one
         * FIFO entry for the command at (address - 0x400) / 4. Both are
         * 32-bit registers; what a narrower write does on hardware is not
         * known, and it is ignored (melonDS). */
        if (size != 4 || (addr & 3u)) return;
        if (addr < 0x04000440u) gxfifo_write(gx, value);
        else queue_push(gx, (addr - 0x04000400u) >> 2, value);
        pump(gx);
        return;
    }
    for (int i = 0; i < size && i < 4; i++) write_byte(gx, addr + (uint32_t)i, (uint8_t)(value >> (8 * i)));
}

void nds_gx_reset(NdsGx *gx)
{
    memset(gx, 0, sizeof *gx);
    memcpy(gx->projection, identity, sizeof identity);
    memcpy(gx->position, identity, sizeof identity);
    memcpy(gx->vector, identity, sizeof identity);
    memcpy(gx->texture, identity, sizeof identity);
    memcpy(gx->clip, identity, sizeof identity);
    set_viewport(gx, 0);
    /* What these three hold before a program writes them is not known;
     * melonDS's choices: nothing is too far to be a dot, and the rear plane
     * is at the far end with polygon ID 63. */
    gx->one_dot_depth = 0x7FFF;
    nds_put_le32(&gx->render_io[0x30], 0x3F000000u); /* CLEAR_COLOR */
    nds_put_le16(&gx->render_io[0x34], 0x7FFF);      /* CLEAR_DEPTH */
    memcpy(gx->latched_io, gx->render_io, sizeof gx->latched_io);
}
