/*
 * Texture packs: loading, per-frame tile lookup and composition at the
 * pack's scale. See gba_texture_pack.h and TEXTURE_PACKS.md.
 */
#include "gba_texture_pack.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba_sha256.h"

#define W GBA_SCREEN_WIDTH
#define H GBA_SCREEN_HEIGHT

GbaTexRecord *gba_tex_record;

static struct {
    uint8_t *data;
    size_t size;
    uint32_t scale, tile_px, count;
    const uint8_t *entries, *palettes, *images;
    uint32_t palette_size;
    char id[64];
} pack;

static GbaTexRecord *snapshot;
static uint8_t snap_vram[GBA_VRAM_SIZE];
static uint8_t snap_palette[GBA_PALETTE_SIZE];
static bool have_frame;
static GbaTexturePackStats stats;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */
/* ------------------------------------------------------------------ */

const char *gba_texture_pack_status_string(GbaTexturePackStatus status)
{
    switch (status) {
    case GBA_TEXTURE_PACK_OK: return "ok";
    case GBA_TEXTURE_PACK_ERROR_IO: return "cannot read the file";
    case GBA_TEXTURE_PACK_ERROR_FORMAT: return "not a valid GBA texture pack";
    case GBA_TEXTURE_PACK_ERROR_VERSION: return "unsupported texture pack version";
    case GBA_TEXTURE_PACK_ERROR_ROM_MISMATCH: return "the pack is for a different ROM";
    case GBA_TEXTURE_PACK_ERROR_LIMIT: return "the pack exceeds a size limit";
    case GBA_TEXTURE_PACK_ERROR_ORDER: return "the pack's entries are not sorted";
    case GBA_TEXTURE_PACK_ERROR_MEMORY: return "out of memory";
    }
    return "unknown error";
}

static void recolour_cache_clear(void);

void gba_texture_pack_unload(void)
{
    gba_tex_record = NULL;
    free(snapshot);
    snapshot = NULL;
    free(pack.data);
    memset(&pack, 0, sizeof pack);
    recolour_cache_clear();
    have_frame = false;
    memset(&stats, 0, sizeof stats);
}

static GbaTexturePackStatus validate(const GbaMachine *m, const uint8_t *d, size_t size)
{
    if (size < GBA_TEXTURE_PACK_HEADER_SIZE || rd32(d) != GBA_TEXTURE_PACK_MAGIC) return GBA_TEXTURE_PACK_ERROR_FORMAT;
    if (rd32(d + 4) != GBA_TEXTURE_PACK_VERSION) return GBA_TEXTURE_PACK_ERROR_VERSION;
    uint32_t scale = rd32(d + 44), count = rd32(d + 48), pal_size = rd32(d + 52);
    if (scale < 1 || scale > GBA_TEXTURE_PACK_MAX_SCALE || count < 1 || count > GBA_TEXTURE_PACK_MAX_ENTRIES)
        return GBA_TEXTURE_PACK_ERROR_LIMIT;
    if (!memchr(d + 56, 0, 64) || !memchr(d + 120, 0, 64) || rd32(d + 184) || rd32(d + 188))
        return GBA_TEXTURE_PACK_ERROR_FORMAT;
    uint64_t tile_px = 8u * scale;
    uint64_t need = GBA_TEXTURE_PACK_HEADER_SIZE + (uint64_t)count * GBA_TEXTURE_PACK_ENTRY_SIZE + pal_size +
                    (uint64_t)count * tile_px * tile_px * 4u;
    if (need != size) return GBA_TEXTURE_PACK_ERROR_FORMAT;

    static const uint8_t zero[32] = { 0 };
    if (memcmp(d + 8, zero, 32) != 0) {
        uint8_t digest[32];
        if (!m || !m->rom) return GBA_TEXTURE_PACK_ERROR_ROM_MISMATCH;
        gba_sha256(m->rom, m->rom_size, digest);
        if (rd32(d + 40) != m->rom_size || memcmp(digest, d + 8, 32) != 0) return GBA_TEXTURE_PACK_ERROR_ROM_MISMATCH;
    }

    const uint8_t *e = d + GBA_TEXTURE_PACK_HEADER_SIZE;
    for (uint32_t i = 0; i < count; i++, e += GBA_TEXTURE_PACK_ENTRY_SIZE) {
        uint8_t bpp = e[64];
        uint32_t pal_len = bpp == 8 ? 512u : 32u;
        if ((bpp != 4 && bpp != 8) || e[65] || e[66] || e[67] || rd32(e + 72)) return GBA_TEXTURE_PACK_ERROR_FORMAT;
        if ((uint64_t)rd32(e + 68) + pal_len > pal_size) return GBA_TEXTURE_PACK_ERROR_FORMAT;
        if (i && memcmp(e - GBA_TEXTURE_PACK_ENTRY_SIZE, e, 64) >= 0) return GBA_TEXTURE_PACK_ERROR_ORDER;
    }
    return GBA_TEXTURE_PACK_OK;
}

GbaTexturePackStatus gba_texture_pack_load_memory(const GbaMachine *m, const uint8_t *data, size_t size)
{
    GbaTexturePackStatus status = validate(m, data, size);
    if (status != GBA_TEXTURE_PACK_OK) return status;
    uint8_t *copy = (uint8_t *)malloc(size);
    GbaTexRecord *snap = (GbaTexRecord *)calloc(1, sizeof *snap);
    GbaTexRecord *record = (GbaTexRecord *)calloc(1, sizeof *record);
    if (!copy || !snap || !record) {
        free(copy);
        free(snap);
        free(record);
        return GBA_TEXTURE_PACK_ERROR_MEMORY;
    }
    memcpy(copy, data, size);
    gba_texture_pack_unload();
    static GbaTexRecord *owned_record;
    free(owned_record);
    owned_record = record;

    pack.data = copy;
    pack.size = size;
    pack.scale = rd32(copy + 44);
    pack.tile_px = 8u * pack.scale;
    pack.count = rd32(copy + 48);
    pack.palette_size = rd32(copy + 52);
    pack.entries = copy + GBA_TEXTURE_PACK_HEADER_SIZE;
    pack.palettes = pack.entries + (size_t)pack.count * GBA_TEXTURE_PACK_ENTRY_SIZE;
    pack.images = pack.palettes + pack.palette_size;
    memcpy(pack.id, copy + 56, 64);
    snapshot = snap;
    gba_tex_record = record;
    return GBA_TEXTURE_PACK_OK;
}

GbaTexturePackStatus gba_texture_pack_load_file(const GbaMachine *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return GBA_TEXTURE_PACK_ERROR_IO;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = len > 0 ? (uint8_t *)malloc((size_t)len) : NULL;
    bool ok = data && fread(data, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    GbaTexturePackStatus status = ok ? gba_texture_pack_load_memory(m, data, (size_t)len) : GBA_TEXTURE_PACK_ERROR_IO;
    free(data);
    return status;
}

bool gba_texture_pack_active(void) { return pack.data != NULL; }
uint32_t gba_texture_pack_scale(void) { return pack.data ? pack.scale : 1u; }
size_t gba_texture_pack_entry_count(void) { return pack.count; }
const char *gba_texture_pack_id(void) { return pack.data ? pack.id : ""; }
GbaTexturePackStats gba_texture_pack_stats(void) { return stats; }

void gba_texture_pack_frame_complete(const GbaMachine *m)
{
    if (!pack.data || !gba_tex_record) return;
    memcpy(snapshot, gba_tex_record, sizeof *snapshot);
    memcpy(snap_vram, m->vram, sizeof snap_vram);
    memcpy(snap_palette, m->palette, sizeof snap_palette);
    have_frame = true;
}

bool gba_texture_capture(const GbaMachine *m, FILE *f, uint64_t *last)
{
    uint64_t h = 0xCBF29CE484222325ull;
    const struct { const uint8_t *p; size_t n; } parts[] = {
        { m->io, 0x60 }, { m->palette, GBA_PALETTE_SIZE }, { m->vram, GBA_VRAM_SIZE }, { m->oam, GBA_OAM_SIZE },
    };
    for (size_t k = 0; k < sizeof parts / sizeof parts[0]; k++) {
        for (size_t i = 0; i < parts[k].n; i++) h = (h ^ parts[k].p[i]) * 0x100000001B3ull;
    }
    if (h == *last) return false;
    *last = h;
    uint8_t head[8] = { 'V', 'S', 'N', 'P' };
    uint32_t frame = (uint32_t)m->frame_count;
    for (int i = 0; i < 4; i++) head[4 + i] = (uint8_t)(frame >> (8 * i));
    fwrite(head, 1, 8, f);
    fwrite(m->io, 1, GBA_IO_SIZE, f);
    fwrite(m->palette, 1, GBA_PALETTE_SIZE, f);
    fwrite(m->vram, 1, GBA_VRAM_SIZE, f);
    fwrite(m->oam, 1, GBA_OAM_SIZE, f);
    return true;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

static const uint8_t *entry_at(uint32_t i) { return pack.entries + (size_t)i * GBA_TEXTURE_PACK_ENTRY_SIZE; }
static const uint8_t *image_at(uint32_t i) { return pack.images + (size_t)i * pack.tile_px * pack.tile_px * 4u; }

/* First entry whose first n key bytes are >= key. */
static uint32_t lower_bound(const uint8_t *key, size_t n)
{
    uint32_t lo = 0, hi = pack.count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (memcmp(entry_at(mid), key, n) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Per-channel fit dst = a * src + b over colours 1..n-1 (5-bit units);
 * returns the worst error. */
static double channel_fit(const uint8_t *src_pal, const uint8_t *dst_pal, int n, double coef[3][2])
{
    double worst = 0;
    for (int c = 0; c < 3; c++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int k = n - 1;
        for (int i = 1; i < n; i++) {
            double x = ((src_pal[i * 2] | (src_pal[i * 2 + 1] << 8)) >> (5 * c)) & 31;
            double y = ((dst_pal[i * 2] | (dst_pal[i * 2 + 1] << 8)) >> (5 * c)) & 31;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        double var = sxx - sx * sx / k, a, b;
        if (var < 1e-9) {
            a = 0;
            b = sy / k;
        } else {
            a = (sxy - sx * sy / k) / var;
            b = (sy - a * sx) / k;
        }
        coef[c][0] = a;
        coef[c][1] = b;
        for (int i = 1; i < n; i++) {
            double x = ((src_pal[i * 2] | (src_pal[i * 2 + 1] << 8)) >> (5 * c)) & 31;
            double y = ((dst_pal[i * 2] | (dst_pal[i * 2 + 1] << 8)) >> (5 * c)) & 31;
            double err = fabs(a * x + b - y);
            if (err > worst) worst = err;
        }
    }
    return worst;
}

/* Recoloured images, keyed by entry and target palette hash. Images are only
 * freed between frames (gba_texture_pack_compose clears a half-full cache
 * before its lookups), because the frame being composed holds pointers to
 * them; within a frame a full cache just leaves further tiles original. */
enum { RECOLOUR_SLOTS = 4096 };
static struct {
    uint32_t entry_plus_one;
    uint8_t palette_sha[32];
    uint8_t *image;
} recolour_cache[RECOLOUR_SLOTS];
static uint32_t recolour_used;

static void recolour_cache_clear(void)
{
    for (int i = 0; i < RECOLOUR_SLOTS; i++) free(recolour_cache[i].image);
    memset(recolour_cache, 0, sizeof recolour_cache);
    recolour_used = 0;
}

static const uint8_t *recoloured(uint32_t entry, const uint8_t *palette_sha, const double coef[3][2])
{
    uint32_t h = (entry * 2654435761u) ^ rd32(palette_sha);
    for (uint32_t probe = 0; probe < RECOLOUR_SLOTS; probe++) {
        uint32_t slot = (h + probe) & (RECOLOUR_SLOTS - 1u);
        if (recolour_cache[slot].entry_plus_one == entry + 1 &&
            memcmp(recolour_cache[slot].palette_sha, palette_sha, 32) == 0)
            return recolour_cache[slot].image;
        if (recolour_cache[slot].entry_plus_one == 0) break;
    }
    if (recolour_used >= RECOLOUR_SLOTS - RECOLOUR_SLOTS / 8) return NULL;
    size_t bytes = (size_t)pack.tile_px * pack.tile_px * 4u;
    uint8_t *img = (uint8_t *)malloc(bytes);
    if (!img) return NULL;
    const uint8_t *src = image_at(entry);
    for (size_t i = 0; i < bytes; i += 4) {
        /* Image bytes are RGBA; the fit's channel 0 is red. */
        for (int c = 0; c < 3; c++) {
            double v = coef[c][0] * src[i + c] + coef[c][1] * 255.0 / 31.0;
            img[i + c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
        }
        img[i + 3] = src[i + 3];
    }
    for (uint32_t probe = 0;; probe++) {
        uint32_t slot = (h + probe) & (RECOLOUR_SLOTS - 1u);
        if (recolour_cache[slot].entry_plus_one == 0) {
            recolour_cache[slot].entry_plus_one = entry + 1;
            memcpy(recolour_cache[slot].palette_sha, palette_sha, 32);
            recolour_cache[slot].image = img;
            recolour_used++;
            return img;
        }
    }
}

/* The image for one tile in one palette, or NULL. */
static const uint8_t *resolve(const GbaTexSource *s, int *kind)
{
    bool bpp8 = (s->flags & GBA_TEX_BPP8) != 0;
    uint32_t tile_len = bpp8 ? 64u : 32u;
    if (s->tile_off + tile_len > GBA_VRAM_SIZE) return NULL;
    const uint8_t *palette = bpp8 ? snap_palette + (s->pal_base >= 256 ? 0x200 : 0) : snap_palette + s->pal_base * 2u;
    uint32_t pal_len = bpp8 ? 512u : 32u;
    uint8_t key[64];
    gba_sha256(snap_vram + s->tile_off, tile_len, key);
    gba_sha256(palette, pal_len, key + 32);

    uint32_t i = lower_bound(key, 64);
    if (i < pack.count && memcmp(entry_at(i), key, 64) == 0) {
        *kind = 1;
        return image_at(i);
    }
    /* Same tile, another palette: the closest per-channel fit. */
    double best_coef[3][2], coef[3][2], best = 1.0 + 1e-9;
    uint32_t best_entry = UINT32_MAX;
    for (uint32_t j = lower_bound(key, 32); j < pack.count && memcmp(entry_at(j), key, 32) == 0; j++) {
        const uint8_t *e = entry_at(j);
        if (e[64] != (bpp8 ? 8 : 4)) continue;
        double worst = channel_fit(pack.palettes + rd32(e + 68), palette, bpp8 ? 256 : 16, coef);
        if (worst <= best) {
            best = worst;
            best_entry = j;
            memcpy(best_coef, coef, sizeof coef);
        }
    }
    if (best_entry == UINT32_MAX) return NULL;
    *kind = 2;
    return recoloured(best_entry, key + 32, (const double (*)[2])best_coef);
}

/* Per-frame map from (tile address, palette base, depth) to its image. */
enum { FRAME_SLOTS = 8192 };
static struct {
    uint32_t key; /* tile_off << 10 | pal_base << 1 | bpp8, plus one */
    const uint8_t *image;
} frame_map[FRAME_SLOTS];

static const uint8_t *lookup(const GbaTexSource *s)
{
    if (!(s->flags & GBA_TEX_VALID)) return NULL;
    uint32_t key = ((s->tile_off << 10) | ((uint32_t)s->pal_base << 1) | ((s->flags & GBA_TEX_BPP8) ? 1u : 0u)) + 1u;
    uint32_t h = key * 2654435761u;
    for (uint32_t probe = 0; probe < FRAME_SLOTS; probe++) {
        uint32_t slot = (h + probe) & (FRAME_SLOTS - 1u);
        if (frame_map[slot].key == key) return frame_map[slot].image;
        if (frame_map[slot].key == 0) {
            int kind = 0;
            const uint8_t *img = resolve(s, &kind);
            frame_map[slot].key = key;
            frame_map[slot].image = img;
            if (kind == 1) stats.tiles_matched++;
            else if (kind == 2) stats.tiles_recoloured++;
            else stats.tiles_missing++;
            return img;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Composition                                                         */
/* ------------------------------------------------------------------ */

typedef struct { int r, g, b, a; } Rgba;

static inline Rgba from555(uint16_t c)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    Rgba o = { (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255 };
    return o;
}

/* The image pixel at sub-position (sx, sy) of the screen pixel s drew,
 * offset by (ox, oy) screen pixels; false outside the tile. */
static inline bool sample(const uint8_t *img, const GbaTexSource *s, int ox, int oy, int sx, int sy, Rgba *out)
{
    int S = (int)pack.scale, T = (int)pack.tile_px;
    int X = ox * S + sx, Y = oy * S + sy;
    int u = s->tx * S + ((s->flags & GBA_TEX_HFLIP) ? S - 1 - X : X);
    int v = s->ty * S + ((s->flags & GBA_TEX_VFLIP) ? S - 1 - Y : Y);
    if (u < 0 || v < 0 || u >= T || v >= T) return false;
    const uint8_t *p = img + ((size_t)v * (size_t)T + (size_t)u) * 4u;
    out->r = p[0];
    out->g = p[1];
    out->b = p[2];
    out->a = p[3];
    return true;
}

static inline Rgba over(Rgba top, Rgba under)
{
    if (top.a >= 255) return top;
    Rgba o = { (top.r * top.a + under.r * (255 - top.a) + 127) / 255, (top.g * top.a + under.g * (255 - top.a) + 127) / 255,
               (top.b * top.a + under.b * (255 - top.a) + 127) / 255, 255 };
    return o;
}

static inline Rgba apply_effect(Rgba c, Rgba second, int effect, int line)
{
    const GbaTexRecord *r = snapshot;
    switch (effect) {
    case GBA_TEX_EFFECT_ALPHA: {
        int eva = r->eva[line], evb = r->evb[line];
        c.r = (c.r * eva + second.r * evb) >> 4;
        c.g = (c.g * eva + second.g * evb) >> 4;
        c.b = (c.b * eva + second.b * evb) >> 4;
        if (c.r > 255) c.r = 255;
        if (c.g > 255) c.g = 255;
        if (c.b > 255) c.b = 255;
        break;
    }
    case GBA_TEX_EFFECT_BRIGHTEN: {
        int evy = r->evy[line];
        c.r += ((255 - c.r) * evy) >> 4;
        c.g += ((255 - c.g) * evy) >> 4;
        c.b += ((255 - c.b) * evy) >> 4;
        break;
    }
    case GBA_TEX_EFFECT_DARKEN: {
        int evy = r->evy[line];
        c.r -= (c.r * evy) >> 4;
        c.g -= (c.g * evy) >> 4;
        c.b -= (c.b * evy) >> 4;
        break;
    }
    default: break;
    }
    return c;
}

bool gba_texture_pack_compose(uint32_t *output)
{
    if (!pack.data || !have_frame) return false;
    const GbaTexRecord *r = snapshot;
    const int S = (int)pack.scale, OW = W * S;
    stats.tiles_matched = stats.tiles_recoloured = stats.tiles_missing = 0;
    memset(frame_map, 0, sizeof frame_map);
    if (recolour_used >= RECOLOUR_SLOTS / 2) recolour_cache_clear();

    static const uint8_t *img_top[W * H], *img_second[W * H];
    for (int i = 0; i < W * H; i++) {
        img_top[i] = lookup(&r->top[i]);
        img_second[i] = lookup(&r->second[i]);
    }

    static const int dxs[4] = { -1, 1, 0, 0 }, dys[4] = { 0, 0, -1, 1 };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            const GbaTexSource *top = &r->top[i], *second = &r->second[i];
            const uint8_t *ti = img_top[i], *si = img_second[i];
            Rgba base_top = from555(r->top_color[i]), base_second = from555(r->second_color[i]);
            int effect = r->effect[i];

            /* Neighbours drawn in front whose upscaled edge may reach into this pixel. */
            int front[4], nfront = 0;
            for (int k = 0; k < 4; k++) {
                int nx = x + dxs[k], ny = y + dys[k];
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                int n = ny * W + nx;
                if (img_top[n] && r->depth[n] < r->depth[i]) front[nfront++] = k;
            }
            /* Farthest first, so the front-most is drawn last. */
            for (int a = 1; a < nfront; a++) {
                for (int b = a; b > 0; b--) {
                    int na = (y + dys[front[b]]) * W + x + dxs[front[b]];
                    int nb = (y + dys[front[b - 1]]) * W + x + dxs[front[b - 1]];
                    if (r->depth[na] > r->depth[nb]) {
                        int t = front[b];
                        front[b] = front[b - 1];
                        front[b - 1] = t;
                    }
                }
            }

            uint32_t *row = output + (size_t)y * S * OW;
            for (int sy = 0; sy < S; sy++, row += OW) {
                for (int sx = 0; sx < S; sx++) {
                    Rgba c = base_top, under = base_second, px;
                    if (si && sample(si, second, 0, 0, sx, sy, &px)) under = over(px, base_second);
                    if (ti && sample(ti, top, 0, 0, sx, sy, &px)) c = over(px, under);
                    c = apply_effect(c, under, effect, y);

                    for (int f = 0; f < nfront; f++) {
                        int k = front[f];
                        /* Only the half of this pixel next to that neighbour. */
                        if ((k == 0 && sx >= S / 2) || (k == 1 && sx < S - S / 2) || (k == 2 && sy >= S / 2) ||
                            (k == 3 && sy < S - S / 2))
                            continue;
                        int nx = x + dxs[k], ny = y + dys[k], n = ny * W + nx;
                        if (!sample(img_top[n], &r->top[n], x - nx, y - ny, sx, sy, &px) || px.a == 0) continue;
                        Rgba nc = over(px, c);
                        c = apply_effect(nc, c, r->effect[n], ny);
                    }
                    row[x * S + sx] = 0xFF000000u | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
                }
            }
        }
    }
    stats.frames_composed++;
    return true;
}
