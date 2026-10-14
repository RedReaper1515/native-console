/*
 * GBA texture packs: host-side high-resolution tile replacement
 * (gba_texture_pack.c). See docs/TEXTURE_PACKS.md.
 *
 * While a pack is active the PPU records, for every screen pixel, the tile
 * pixel that made its top layer and the one under it (tile address in VRAM,
 * the tile-data column and row sampled, flips, palette base), their raw
 * colours, and the colour effect applied. When the frame completes the
 * record, VRAM and palette RAM are snapshotted. Composition then rebuilds
 * the frame at scale x from the pack's images: each tile is found by the
 * SHA-256 of its VRAM bytes and of its palette RAM bytes, blended and faded
 * the way the PPU blended the original, with edges of the layer in front
 * drawn over their neighbours so upscaled outlines stay smooth.
 *
 * A tile drawn with a palette the pack has no image for uses an image of the
 * same tile whose palette maps onto the current one by a per-channel linear
 * change (fades, flashes), recoloured by that change. Pixels from affine or
 * mosaic layers, bitmap modes, and tiles without an image keep their
 * original colour, scaled.
 *
 * Nothing here feeds back into the emulation: memory, timing, saves and the
 * accurate framebuffer are unchanged.
 */
#ifndef GBA_TEXTURE_PACK_H
#define GBA_TEXTURE_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "gba.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One recorded tile pixel. flags == 0: no tile (backdrop, affine, mosaic,
 * bitmap). */
typedef struct GbaTexSource {
    uint32_t tile_off;  /* VRAM offset of the tile's first byte */
    uint16_t pal_base;  /* palette RAM index of colour 0 (BG 0-255, OBJ 256-511) */
    uint8_t tx, ty;     /* tile-data column and row sampled, 0-7 */
    uint8_t flags;      /* GBA_TEX_* */
    uint8_t reserved[3];
} GbaTexSource;

#define GBA_TEX_VALID 1u
#define GBA_TEX_BPP8 2u
#define GBA_TEX_HFLIP 4u
#define GBA_TEX_VFLIP 8u

enum { GBA_TEX_EFFECT_NONE, GBA_TEX_EFFECT_ALPHA, GBA_TEX_EFFECT_BRIGHTEN, GBA_TEX_EFFECT_DARKEN };

/* What the PPU fills in while a pack is active (gba_tex_record != NULL). */
typedef struct GbaTexRecord {
    GbaTexSource top[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];
    GbaTexSource second[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];
    uint16_t top_color[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];    /* BGR555 before effects */
    uint16_t second_color[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];
    uint8_t effect[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];        /* GBA_TEX_EFFECT_* */
    uint8_t depth[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];         /* top layer: lower is in front */
    uint8_t eva[GBA_SCREEN_HEIGHT], evb[GBA_SCREEN_HEIGHT], evy[GBA_SCREEN_HEIGHT];
} GbaTexRecord;

extern GbaTexRecord *gba_tex_record;

/*
 * File layout (.gbatp), little endian:
 *
 *   header, 192 bytes
 *     0   u32  magic 'GATP'
 *     4   u32  version (1)
 *     8   u8[32] SHA-256 of the ROM, or all zero for any ROM
 *     40  u32  ROM size (0 with a zero hash)
 *     44  u32  scale (1-8): images are 8*scale pixels square
 *     48  u32  entry count (1-65536)
 *     52  u32  palette data size
 *     56  char[64] pack ID, NUL terminated
 *     120 char[64] SPDX license expression, NUL terminated
 *     184 u32  reserved, zero
 *     188 u32  reserved, zero
 *   entry_count entries, 76 bytes each, strictly ascending by the first 64 bytes
 *     0   u8[32] SHA-256 of the tile's VRAM bytes (32 for 4bpp, 64 for 8bpp)
 *     32  u8[32] SHA-256 of its palette RAM bytes (32 or 512)
 *     64  u8   bits per pixel (4 or 8)
 *     65  u8[3] reserved, zero
 *     68  u32  offset of those palette bytes in the palette data
 *     72  u32  reserved, zero
 *   palette data
 *   entry_count images, (8*scale)^2 RGBA pixels each, rows top to bottom
 */
#define GBA_TEXTURE_PACK_MAGIC 0x50544147u /* 'GATP' */
#define GBA_TEXTURE_PACK_VERSION 1u
#define GBA_TEXTURE_PACK_HEADER_SIZE 192u
#define GBA_TEXTURE_PACK_ENTRY_SIZE 76u
#define GBA_TEXTURE_PACK_MAX_SCALE 8u
#define GBA_TEXTURE_PACK_MAX_ENTRIES 65536u

typedef enum GbaTexturePackStatus {
    GBA_TEXTURE_PACK_OK = 0,
    GBA_TEXTURE_PACK_ERROR_IO,
    GBA_TEXTURE_PACK_ERROR_FORMAT,
    GBA_TEXTURE_PACK_ERROR_VERSION,
    GBA_TEXTURE_PACK_ERROR_ROM_MISMATCH,
    GBA_TEXTURE_PACK_ERROR_LIMIT,
    GBA_TEXTURE_PACK_ERROR_ORDER,
    GBA_TEXTURE_PACK_ERROR_MEMORY,
} GbaTexturePackStatus;

typedef struct GbaTexturePackStats {
    uint64_t frames_composed;
    uint32_t tiles_matched;    /* distinct tiles with an exact image, last frame */
    uint32_t tiles_recoloured; /* distinct tiles shown through a palette fit, last frame */
    uint32_t tiles_missing;    /* distinct recorded tiles with no usable image, last frame */
} GbaTexturePackStats;

/* Validates and activates a pack for the ROM loaded in m (replacing any
 * active pack). The pack stays active until gba_texture_pack_unload. */
GbaTexturePackStatus gba_texture_pack_load_file(const GbaMachine *m, const char *path);
GbaTexturePackStatus gba_texture_pack_load_memory(const GbaMachine *m, const uint8_t *data, size_t size);
void gba_texture_pack_unload(void);
bool gba_texture_pack_active(void);
uint32_t gba_texture_pack_scale(void);
size_t gba_texture_pack_entry_count(void);
const char *gba_texture_pack_id(void);
const char *gba_texture_pack_status_string(GbaTexturePackStatus status);
GbaTexturePackStats gba_texture_pack_stats(void);

/* Called when a frame completes: snapshots the record, VRAM and palettes. */
void gba_texture_pack_frame_complete(const GbaMachine *m);

/* Texture capture for authoring packs (tools/gba/texpack): appends the
 * video state (I/O registers, palette RAM, VRAM, OAM) to f as one record,
 * "VSNP" u32 frame then those 0x400 + 0x400 + 0x18000 + 0x400 bytes, unless
 * it is unchanged since the last record written with this *last hash.
 * Returns true when a record was written. */
bool gba_texture_capture(const GbaMachine *m, FILE *f, uint64_t *last);

/* Composes the last completed frame into output, (240*scale) x (160*scale)
 * 0xAARRGGBB pixels. False when no pack is active or no frame is complete. */
bool gba_texture_pack_compose(uint32_t *output);

#ifdef __cplusplus
}
#endif

#endif
