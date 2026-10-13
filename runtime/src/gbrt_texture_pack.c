#include "gbrt_texture_pack.h"

#include "gbrt.h"
#include "gbrt_hash.h"
#include "ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE_SLOTS 384u
#define TILE_BANKS 2u
#define TILE_BYTES 16u
#define MAX_PACK_FILE_BYTES (256u * 1024u * 1024u)
#define NO_ENTRY UINT32_MAX

uint32_t* gbrt_pixel_sources = NULL;

typedef struct TexturePackEntry {
    uint8_t tile_sha256[32];
    uint8_t palette_kind;
    uint8_t palette;
} TexturePackEntry;

typedef struct SlotLookup {
    uint32_t generation;          /* frame generation the lookup was made for */
    uint8_t tile[TILE_BYTES];     /* tile bytes the lookup was computed from */
    uint32_t first;               /* first entry with this tile hash, or NO_ENTRY */
    uint32_t count;
} SlotLookup;

typedef struct TexturePackState {
    uint32_t scale;
    uint32_t entry_count;
    TexturePackEntry* entries;
    uint8_t* pixels;              /* entry_count * (8*scale)^2 * 4 RGBA bytes */
    size_t image_bytes;
    char pack_id[GB_TEXTURE_PACK_ID_CAPACITY];
    uint32_t live_sources[GB_FRAMEBUFFER_SIZE];
    uint32_t frame_sources[GB_FRAMEBUFFER_SIZE];
    uint8_t frame_tiles[TILE_BANKS * TILE_SLOTS * TILE_BYTES];
    bool frame_valid;
    bool frame_cgb_palettes;
    uint32_t generation;
    SlotLookup lookups[TILE_BANKS * TILE_SLOTS];
    GBTexturePackStats stats;
} TexturePackState;

static TexturePackState* g_pack = NULL;

static uint32_t read_le32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
           ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static bool bounded_string(const uint8_t* data, size_t capacity) {
    return data[0] != '\0' && memchr(data, '\0', capacity) != NULL;
}

void gbrt_texture_pack_unload(void) {
    gbrt_pixel_sources = NULL;
    if (g_pack) {
        free(g_pack->entries);
        free(g_pack->pixels);
        free(g_pack);
        g_pack = NULL;
    }
}

GBTexturePackStatus gbrt_texture_pack_load_memory(GBContext* context,
                                                  const uint8_t* data,
                                                  size_t size) {
    if (!context || !data || !context->rom) {
        return GB_TEXTURE_PACK_ERROR_ARGUMENT;
    }
    if (size < GB_TEXTURE_PACK_HEADER_SIZE ||
        read_le32(data) != GB_TEXTURE_PACK_MAGIC) {
        return GB_TEXTURE_PACK_ERROR_FORMAT;
    }
    if (read_le32(data + 4) != GB_TEXTURE_PACK_VERSION) {
        return GB_TEXTURE_PACK_ERROR_VERSION;
    }

    uint8_t rom_digest[32];
    gbrt_sha256(context->rom, context->rom_size, rom_digest);
    if (read_le32(data + 40) != context->rom_size ||
        memcmp(rom_digest, data + 8, sizeof(rom_digest)) != 0) {
        return GB_TEXTURE_PACK_ERROR_ROM_MISMATCH;
    }

    const uint32_t scale = read_le32(data + 44);
    const uint32_t entry_count = read_le32(data + 48);
    if (scale < 1u || scale > GB_TEXTURE_PACK_MAX_SCALE ||
        entry_count < 1u || entry_count > GB_TEXTURE_PACK_MAX_ENTRIES) {
        return GB_TEXTURE_PACK_ERROR_LIMIT;
    }
    if (read_le32(data + 52) != 0u ||
        !bounded_string(data + 56, GB_TEXTURE_PACK_ID_CAPACITY) ||
        !bounded_string(data + 120, GB_TEXTURE_PACK_LICENSE_CAPACITY)) {
        return GB_TEXTURE_PACK_ERROR_FORMAT;
    }

    const size_t side = 8u * scale;
    const size_t image_bytes = side * side * 4u;
    const size_t expected = GB_TEXTURE_PACK_HEADER_SIZE +
                            (size_t)entry_count * GB_TEXTURE_PACK_ENTRY_SIZE +
                            (size_t)entry_count * image_bytes;
    if (size != expected) {
        return GB_TEXTURE_PACK_ERROR_FORMAT;
    }

    const uint8_t* entries = data + GB_TEXTURE_PACK_HEADER_SIZE;
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint8_t* entry = entries + (size_t)i * GB_TEXTURE_PACK_ENTRY_SIZE;
        const uint8_t kind = entry[32];
        if (kind > GB_TEXTURE_PALETTE_CGB ||
            (kind == GB_TEXTURE_PALETTE_ANY && entry[33] != 0u) ||
            (kind == GB_TEXTURE_PALETTE_CGB && entry[33] > 7u) ||
            entry[34] != 0u || entry[35] != 0u) {
            return GB_TEXTURE_PACK_ERROR_FORMAT;
        }
        if (i > 0 && memcmp(entry - GB_TEXTURE_PACK_ENTRY_SIZE, entry, 34u) >= 0) {
            return GB_TEXTURE_PACK_ERROR_ORDER;
        }
    }

    TexturePackState* pack = (TexturePackState*)calloc(1, sizeof(*pack));
    if (!pack) {
        return GB_TEXTURE_PACK_ERROR_MEMORY;
    }
    pack->entries = (TexturePackEntry*)calloc(entry_count, sizeof(*pack->entries));
    pack->pixels = (uint8_t*)malloc((size_t)entry_count * image_bytes);
    if (!pack->entries || !pack->pixels) {
        free(pack->entries);
        free(pack->pixels);
        free(pack);
        return GB_TEXTURE_PACK_ERROR_MEMORY;
    }
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint8_t* entry = entries + (size_t)i * GB_TEXTURE_PACK_ENTRY_SIZE;
        memcpy(pack->entries[i].tile_sha256, entry, 32u);
        pack->entries[i].palette_kind = entry[32];
        pack->entries[i].palette = entry[33];
    }
    memcpy(pack->pixels,
           entries + (size_t)entry_count * GB_TEXTURE_PACK_ENTRY_SIZE,
           (size_t)entry_count * image_bytes);
    memcpy(pack->pack_id, data + 56, GB_TEXTURE_PACK_ID_CAPACITY);
    pack->scale = scale;
    pack->entry_count = entry_count;
    pack->image_bytes = image_bytes;
    pack->generation = 1u;

    gbrt_texture_pack_unload();
    g_pack = pack;
    gbrt_pixel_sources = pack->live_sources;
    return GB_TEXTURE_PACK_OK;
}

GBTexturePackStatus gbrt_texture_pack_load_file(GBContext* context, const char* path) {
    if (!context || !path || !path[0]) {
        return GB_TEXTURE_PACK_ERROR_ARGUMENT;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        return GB_TEXTURE_PACK_ERROR_IO;
    }
    GBTexturePackStatus status = GB_TEXTURE_PACK_ERROR_IO;
    uint8_t* data = NULL;
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        length = ftell(file);
    }
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        goto done;
    }
    if ((unsigned long)length > MAX_PACK_FILE_BYTES) {
        status = GB_TEXTURE_PACK_ERROR_LIMIT;
        goto done;
    }
    data = (uint8_t*)malloc(length > 0 ? (size_t)length : 1u);
    if (!data) {
        status = GB_TEXTURE_PACK_ERROR_MEMORY;
        goto done;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        goto done;
    }
    status = gbrt_texture_pack_load_memory(context, data, (size_t)length);
done:
    free(data);
    fclose(file);
    return status;
}

bool gbrt_texture_pack_is_active(void) {
    return g_pack != NULL;
}

uint32_t gbrt_texture_pack_scale(void) {
    return g_pack ? g_pack->scale : 1u;
}

size_t gbrt_texture_pack_entry_count(void) {
    return g_pack ? g_pack->entry_count : 0u;
}

const char* gbrt_texture_pack_id(void) {
    return g_pack ? g_pack->pack_id : "";
}

GBTexturePackStats gbrt_texture_pack_stats(void) {
    GBTexturePackStats empty = {0, 0, 0};
    return g_pack ? g_pack->stats : empty;
}

const char* gbrt_texture_pack_status_string(GBTexturePackStatus status) {
    switch (status) {
        case GB_TEXTURE_PACK_OK: return "ok";
        case GB_TEXTURE_PACK_ERROR_ARGUMENT: return "invalid argument";
        case GB_TEXTURE_PACK_ERROR_IO: return "could not read file";
        case GB_TEXTURE_PACK_ERROR_FORMAT: return "malformed texture pack";
        case GB_TEXTURE_PACK_ERROR_VERSION: return "unsupported texture pack version";
        case GB_TEXTURE_PACK_ERROR_ROM_MISMATCH: return "texture pack is for a different ROM";
        case GB_TEXTURE_PACK_ERROR_LIMIT: return "texture pack exceeds limits";
        case GB_TEXTURE_PACK_ERROR_ORDER: return "texture pack entries are unsorted or duplicated";
        case GB_TEXTURE_PACK_ERROR_MEMORY: return "out of memory";
    }
    return "unknown";
}

void gbrt_texture_pack_frame_complete(const GBContext* context, bool cgb_palettes) {
    if (!g_pack || !context || !context->vram) {
        return;
    }
    memcpy(g_pack->frame_sources, g_pack->live_sources, sizeof(g_pack->frame_sources));
    for (uint32_t bank = 0; bank < TILE_BANKS; ++bank) {
        memcpy(g_pack->frame_tiles + bank * TILE_SLOTS * TILE_BYTES,
               context->vram + bank * VRAM_SIZE,
               TILE_SLOTS * TILE_BYTES);
    }
    g_pack->frame_cgb_palettes = cgb_palettes;
    g_pack->frame_valid = true;
    g_pack->generation++;
}

static int compare_hash(const uint8_t* digest, const TexturePackEntry* entry) {
    return memcmp(digest, entry->tile_sha256, 32u);
}

static const SlotLookup* lookup_slot(uint32_t index) {
    SlotLookup* lookup = &g_pack->lookups[index];
    if (lookup->generation == g_pack->generation) {
        return lookup;
    }
    const uint8_t* tile = g_pack->frame_tiles + index * TILE_BYTES;
    if (lookup->generation != 0u && memcmp(lookup->tile, tile, TILE_BYTES) == 0) {
        lookup->generation = g_pack->generation;
        return lookup;
    }

    uint8_t digest[32];
    gbrt_sha256(tile, TILE_BYTES, digest);
    uint32_t lo = 0;
    uint32_t hi = g_pack->entry_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (compare_hash(digest, &g_pack->entries[mid]) > 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    uint32_t end = lo;
    while (end < g_pack->entry_count && compare_hash(digest, &g_pack->entries[end]) == 0) {
        end++;
    }
    memcpy(lookup->tile, tile, TILE_BYTES);
    lookup->first = end > lo ? lo : NO_ENTRY;
    lookup->count = end - lo;
    lookup->generation = g_pack->generation;
    return lookup;
}

static uint32_t choose_entry(const SlotLookup* lookup, uint32_t palette) {
    if (lookup->first == NO_ENTRY) {
        return NO_ENTRY;
    }
    const uint8_t kind = g_pack->frame_cgb_palettes ? GB_TEXTURE_PALETTE_CGB
                                                    : GB_TEXTURE_PALETTE_DMG;
    uint32_t any = NO_ENTRY;
    for (uint32_t i = lookup->first; i < lookup->first + lookup->count; ++i) {
        const TexturePackEntry* entry = &g_pack->entries[i];
        if (entry->palette_kind == kind && entry->palette == palette) {
            return i;
        }
        if (entry->palette_kind == GB_TEXTURE_PALETTE_ANY) {
            any = i;
        }
    }
    return any;
}

static uint32_t blend(uint32_t base, const uint8_t* rgba) {
    const uint32_t alpha = rgba[3];
    if (alpha == 255u) {
        return 0xFF000000u | ((uint32_t)rgba[0] << 16u) | ((uint32_t)rgba[1] << 8u) | rgba[2];
    }
    if (alpha == 0u) {
        return base;
    }
    const uint32_t inverse = 255u - alpha;
    const uint32_t r = (rgba[0] * alpha + ((base >> 16u) & 0xFFu) * inverse + 127u) / 255u;
    const uint32_t g = (rgba[1] * alpha + ((base >> 8u) & 0xFFu) * inverse + 127u) / 255u;
    const uint32_t b = (rgba[2] * alpha + (base & 0xFFu) * inverse + 127u) / 255u;
    return 0xFF000000u | (r << 16u) | (g << 8u) | b;
}

bool gbrt_texture_pack_compose(const uint32_t* base, uint32_t* output) {
    if (!g_pack || !g_pack->frame_valid || !base || !output) {
        return false;
    }
    const uint32_t scale = g_pack->scale;
    const uint32_t side = 8u * scale;
    const size_t out_width = (size_t)GB_SCREEN_WIDTH * scale;
    uint64_t replaced = 0;
    uint32_t matched_tiles = 0;
    uint8_t matched[TILE_BANKS * TILE_SLOTS];
    memset(matched, 0, sizeof(matched));

    for (uint32_t y = 0; y < GB_SCREEN_HEIGHT; ++y) {
        for (uint32_t x = 0; x < GB_SCREEN_WIDTH; ++x) {
            const size_t index = (size_t)y * GB_SCREEN_WIDTH + x;
            const uint32_t source = g_pack->frame_sources[index];
            const uint32_t original = base[index];
            uint32_t entry = NO_ENTRY;

            if (GB_PIXEL_SOURCE_LAYER(source) != GB_PIXEL_LAYER_NONE) {
                const uint32_t slot = GB_PIXEL_SOURCE_BANK(source) * TILE_SLOTS +
                                      GB_PIXEL_SOURCE_SLOT(source);
                if (GB_PIXEL_SOURCE_SLOT(source) < TILE_SLOTS) {
                    entry = choose_entry(lookup_slot(slot), GB_PIXEL_SOURCE_PALETTE(source));
                    if (entry != NO_ENTRY && !matched[slot]) {
                        matched[slot] = 1;
                        matched_tiles++;
                    }
                }
            }

            uint32_t* row = output + (size_t)y * scale * out_width + (size_t)x * scale;
            if (entry == NO_ENTRY) {
                for (uint32_t sy = 0; sy < scale; ++sy) {
                    for (uint32_t sx = 0; sx < scale; ++sx) {
                        row[sy * out_width + sx] = original;
                    }
                }
                continue;
            }

            replaced++;
            const uint8_t* image = g_pack->pixels + (size_t)entry * g_pack->image_bytes;
            const uint32_t tx = GB_PIXEL_SOURCE_TX(source) * scale;
            const uint32_t ty = GB_PIXEL_SOURCE_TY(source) * scale;
            const bool flip_x = GB_PIXEL_SOURCE_FLIP_X(source) != 0u;
            const bool flip_y = GB_PIXEL_SOURCE_FLIP_Y(source) != 0u;
            for (uint32_t sy = 0; sy < scale; ++sy) {
                const uint32_t hy = ty + (flip_y ? scale - 1u - sy : sy);
                for (uint32_t sx = 0; sx < scale; ++sx) {
                    const uint32_t hx = tx + (flip_x ? scale - 1u - sx : sx);
                    row[sy * out_width + sx] = blend(original, image + ((size_t)hy * side + hx) * 4u);
                }
            }
        }
    }

    g_pack->stats.frames_composed++;
    g_pack->stats.pixels_replaced = replaced;
    g_pack->stats.tiles_matched = matched_tiles;
    return true;
}
