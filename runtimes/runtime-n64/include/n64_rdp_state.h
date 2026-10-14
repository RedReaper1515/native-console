/*
 * The RDP rasteriser's state: everything the command stream sets (TMEM,
 * the eight tile descriptors, the modes, the colours, the image pointers,
 * the scissor) plus the command reader's partial-command buffer.
 *
 * It is plain data with no host pointers, so a save state copies it as
 * bytes. The span table at the end is per-primitive scratch: nothing reads
 * it between commands, so restoring a stale copy is harmless.
 *
 * The fields mirror parallel-rdp's CPU-side state (rdp_device.cpp,
 * rdp_renderer.cpp) in its already-decoded form, because that is the form
 * the per-pixel pipeline consumes: the other-modes bits are kept as
 * parallel-rdp's StaticRasterizationState and DepthBlendState flags rather
 * than as the raw command words.
 */
#ifndef N64_RDP_STATE_H
#define N64_RDP_STATE_H

#include "n64_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One of the eight tile descriptors (SET_TILE, SET_TILE_SIZE). Coordinates
 * are 10.2 fixed point; offset and stride are in bytes. */
typedef struct N64RdpTile {
    uint32_t slo, shi, tlo, thi;
    uint32_t offset, stride;
    uint8_t fmt, size, palette;
    uint8_t mask_s, shift_s, mask_t, shift_t;
    uint8_t flags; /* N64_RDP_TILE_* */
} N64RdpTile;

#define N64_RDP_TILE_CLAMP_S 0x01u
#define N64_RDP_TILE_MIRROR_S 0x02u
#define N64_RDP_TILE_CLAMP_T 0x04u
#define N64_RDP_TILE_MIRROR_T 0x08u

/* Colour image formats, as parallel-rdp's FBFormat. */
enum {
    N64_RDP_FB_I4 = 0,
    N64_RDP_FB_I8 = 1,
    N64_RDP_FB_RGBA5551 = 2,
    N64_RDP_FB_IA88 = 3,
    N64_RDP_FB_RGBA8888 = 4,
};

/* One scanline of a primitive after span setup (span_setup.comp). x
 * coordinates are in eighths of a pixel, one per sub-scanline. */
typedef struct N64RdpSpan {
    int32_t rgba[4];
    int32_t stzw[4];
    uint16_t xleft[4];
    uint16_t xright[4];
    int32_t interpolation_base_x;
    int32_t start_x;
    int32_t end_x;
    int16_t lodlength;
    uint16_t valid_line;
} N64RdpSpan;

/* A primitive covers at most 1024 scanlines; span setup also computes the
 * scanline after the last one, which the pipelined-texel1 path peeks at. */
#define N64_RDP_MAX_SPANS 1026

/* The longest command is a shaded, textured, z-buffered triangle. */
#define N64_RDP_MAX_COMMAND_WORDS 22

struct N64RdpState {
    /* 4 KiB of TMEM, big-endian, as the RDP addresses it: the low 2 KiB
     * and the high 2 KiB are the halves 32-bit and YUV textures split
     * across, and TLUTs live in the high half. */
    uint8_t tmem[4096];
    N64RdpTile tiles[8];

    /* SET_TEXTURE_IMAGE. */
    uint32_t tex_addr, tex_width;
    uint8_t tex_fmt, tex_size;

    /* SET_COLOR_IMAGE and SET_Z_IMAGE (SET_MASK_IMAGE). */
    uint8_t fb_fmt; /* N64_RDP_FB_* */
    uint32_t fb_addr, fb_width;
    uint32_t depth_addr;

    /* SET_SCISSOR, 10.2 fixed point. The interlace bits live in
     * static_flags. */
    uint32_t scissor_xlo, scissor_ylo, scissor_xhi, scissor_yhi;

    /* SET_OTHER_MODES and SET_COMBINE in parallel-rdp's decoded form:
     * static_flags holds RASTERIZATION_* bits, depth_blend_flags holds
     * DEPTH_BLEND_* bits (n64_rdp_internal.h), combiner[c] holds the eight
     * selectors of cycle c (RGB a, b, c, d then alpha a, b, c, d), and
     * blend_modes[c] the blender's P, A, M, B selectors of cycle c. */
    uint32_t static_flags;
    uint32_t dither;
    uint8_t combiner[2][8];
    uint32_t depth_blend_flags;
    uint8_t blend_modes[2][4];
    uint8_t coverage_mode, z_mode;
    uint8_t use_prim_depth;

    /* The colour registers, as RGBA words. */
    uint32_t blend_color, fog_color, env_color, prim_color, fill_color;
    uint8_t prim_min_level, prim_lod_frac;
    int32_t prim_depth; /* 15-bit depth << 16, as an interpolated z */
    uint16_t prim_dz;

    /* SET_CONVERT: K0-K3 as 2 * K + 1 (signed), K4 and K5 raw 9-bit. */
    int32_t convert[6];
    /* SET_KEY_R and SET_KEY_GB. */
    uint32_t key_width[3];
    uint8_t key_center[3], key_scale[3];

    /* How many primitives have been drawn since reset: parallel-rdp seeds
     * its noise with it. */
    uint32_t primitive_count;

    /* The command reader's partial command (DPC_END reads). */
    uint64_t cmd_buf[N64_RDP_MAX_COMMAND_WORDS];
    uint32_t cmd_words;

    /* Scratch for the primitive being drawn. */
    N64RdpSpan spans[N64_RDP_MAX_SPANS];
};

typedef struct N64RdpState N64RdpState;

#ifdef __cplusplus
}
#endif

#endif /* N64_RDP_STATE_H */
