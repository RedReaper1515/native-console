#ifndef GBRT_TEXTURE_PACK_H
#define GBRT_TEXTURE_PACK_H

/*
 * Texture packs: host-side high-resolution tile replacement.
 *
 * While a pack is active, the PPU records which tile, tile pixel, flip, and
 * palette produced every pixel it emits. When a frame completes, the pack
 * snapshots that record and the tile data, so composition sees exactly what
 * the accurate PPU drew, including mid-frame scroll, window, and sprite
 * priority effects. Composition replaces each pixel whose tile has a
 * replacement image with the matching block of that image and keeps the
 * original pixel everywhere else.
 *
 * Replacement never feeds back into guest-visible state: registers, memory,
 * timing, saves, and the accurate framebuffer are unchanged. See
 * docs/TEXTURE_PACKS.md for the file format and authoring workflow.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;

/* Pixel source record, one uint32_t per screen pixel. */
#define GB_PIXEL_LAYER_NONE 0u   /* LCD or background off: no tile */
#define GB_PIXEL_LAYER_BG 1u
#define GB_PIXEL_LAYER_WINDOW 2u
#define GB_PIXEL_LAYER_OBJ 3u

#define GB_PIXEL_SOURCE(layer, bank, slot, tx, ty, flip_x, flip_y, raw, palette) \
    ((uint32_t)(layer) | ((uint32_t)(bank) << 2) | ((uint32_t)(slot) << 3) |    \
     ((uint32_t)(tx) << 12) | ((uint32_t)(ty) << 15) |                           \
     ((uint32_t)(flip_x) << 18) | ((uint32_t)(flip_y) << 19) |                   \
     ((uint32_t)(raw) << 20) | ((uint32_t)(palette) << 22))
#define GB_PIXEL_SOURCE_LAYER(s) ((s) & 3u)
#define GB_PIXEL_SOURCE_BANK(s) (((s) >> 2) & 1u)
#define GB_PIXEL_SOURCE_SLOT(s) (((s) >> 3) & 0x1FFu)  /* tile index in VRAM, 0-383 */
#define GB_PIXEL_SOURCE_TX(s) (((s) >> 12) & 7u)      /* tile-data column sampled */
#define GB_PIXEL_SOURCE_TY(s) (((s) >> 15) & 7u)      /* tile-data row sampled */
#define GB_PIXEL_SOURCE_FLIP_X(s) (((s) >> 18) & 1u)
#define GB_PIXEL_SOURCE_FLIP_Y(s) (((s) >> 19) & 1u)
#define GB_PIXEL_SOURCE_RAW(s) (((s) >> 20) & 3u)
/* DMG: the BGP/OBP0/OBP1 value in effect. CGB: the palette number (0-7). */
#define GB_PIXEL_SOURCE_PALETTE(s) (((s) >> 22) & 0xFFu)

/*
 * NULL unless a texture pack is active. The PPU writes one record per emitted
 * pixel into this 160x144 buffer; nothing else should write to it.
 */
extern uint32_t* gbrt_pixel_sources;

#define GB_TEXTURE_PACK_MAGIC 0x50544247u /* 'GBTP' */
#define GB_TEXTURE_PACK_VERSION 1u
#define GB_TEXTURE_PACK_MAX_SCALE 8u
#define GB_TEXTURE_PACK_MAX_ENTRIES 4096u
#define GB_TEXTURE_PACK_ID_CAPACITY 64u
#define GB_TEXTURE_PACK_LICENSE_CAPACITY 64u

/* Which palette an entry applies to. */
#define GB_TEXTURE_PALETTE_ANY 0u
#define GB_TEXTURE_PALETTE_DMG 1u /* palette byte is a BGP/OBP register value */
#define GB_TEXTURE_PALETTE_CGB 2u /* palette byte is a CGB palette number */

/*
 * File layout, little endian:
 *
 *   header, 184 bytes
 *     0   u32  magic 'GBTP'
 *     4   u32  version (1)
 *     8   u8[32] SHA-256 of the exact ROM
 *     40  u32  ROM size
 *     44  u32  scale (1-8): replacement tiles are 8*scale pixels square
 *     48  u32  entry count
 *     52  u32  reserved, zero
 *     56  char[64] pack ID, NUL terminated
 *     120 char[64] SPDX license expression, NUL terminated
 *   entry_count entries, 36 bytes each, strictly ascending by all 34 key bytes
 *     0   u8[32] SHA-256 of the tile's 16 bytes of 2bpp data
 *     32  u8   palette kind (GB_TEXTURE_PALETTE_*)
 *     33  u8   palette (0 when the kind is ANY)
 *     34  u16  reserved, zero
 *   entry_count images, (8*scale)^2 RGBA pixels each, rows top to bottom
 *
 * When a tile has both a palette-specific entry and an ANY entry, the
 * palette-specific one wins.
 */
#define GB_TEXTURE_PACK_HEADER_SIZE 184u
#define GB_TEXTURE_PACK_ENTRY_SIZE 36u

typedef enum GBTexturePackStatus {
    GB_TEXTURE_PACK_OK = 0,
    GB_TEXTURE_PACK_ERROR_ARGUMENT,
    GB_TEXTURE_PACK_ERROR_IO,
    GB_TEXTURE_PACK_ERROR_FORMAT,
    GB_TEXTURE_PACK_ERROR_VERSION,
    GB_TEXTURE_PACK_ERROR_ROM_MISMATCH,
    GB_TEXTURE_PACK_ERROR_LIMIT,
    GB_TEXTURE_PACK_ERROR_ORDER,
    GB_TEXTURE_PACK_ERROR_MEMORY,
} GBTexturePackStatus;

typedef struct GBTexturePackStats {
    uint64_t frames_composed;
    uint64_t pixels_replaced;   /* in the most recent composed frame */
    uint32_t tiles_matched;     /* distinct VRAM tiles with an entry, last frame */
} GBTexturePackStats;

/* Validate and activate a pack for this context. Replaces any active pack. */
GBTexturePackStatus gbrt_texture_pack_load_file(GBContext* context, const char* path);
GBTexturePackStatus gbrt_texture_pack_load_memory(GBContext* context,
                                                  const uint8_t* data,
                                                  size_t size);
void gbrt_texture_pack_unload(void);
bool gbrt_texture_pack_is_active(void);
uint32_t gbrt_texture_pack_scale(void);
size_t gbrt_texture_pack_entry_count(void);
const char* gbrt_texture_pack_id(void);
const char* gbrt_texture_pack_status_string(GBTexturePackStatus status);
GBTexturePackStats gbrt_texture_pack_stats(void);

/*
 * Called by the PPU when a frame completes. Snapshots sources and tile data.
 * `cgb_palettes` says whether source palettes are CGB palette numbers.
 */
void gbrt_texture_pack_frame_complete(const GBContext* context, bool cgb_palettes);

/*
 * Compose the last completed frame. `base` is the 160x144 0xAARRGGBB image the
 * host would otherwise show (after any host recoloring). `output` must hold
 * (160*scale) x (144*scale) pixels. Returns false when no pack is active or no
 * frame has completed yet; the caller then shows `base` unchanged.
 */
bool gbrt_texture_pack_compose(const uint32_t* base, uint32_t* output);

#ifdef __cplusplus
}
#endif

#endif
