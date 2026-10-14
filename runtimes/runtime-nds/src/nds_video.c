/*
 * Video: where the machine meets the two 2D engines (nds_2d.c) and the 3D
 * engine (nds_gx.c, nds_gx_raster.c). Those modules own their registers and
 * know nothing of buses, time or memory mapping; this file routes the ARM9's
 * register accesses to them, hands them video memory as it is mapped, moves
 * the geometry engine through time, and turns each line into pixels.
 *
 * Through a frame (GBATEK, "DS Video Stuff" and "DS 3D Video"):
 *
 *   line 0        both 2D engines start a frame
 *   lines 0-191   at each line's HBlank the 2D engines draw the line, engine
 *                 A taking the 3D picture rendered during the frame before
 *   line 192      the 2D engines end the frame; a waiting SWAP_BUFFERS hands
 *                 the polygon lists to the rendering engine
 *   line 215      the rendering engine draws the next frame's 3D picture
 *
 * With an internal resolution above 1 (nds_set_internal_scale) line 215
 * also draws that picture larger, and each line of the screen engine A is
 * showing is stacked a second time around it ("The larger picture", below).
 */
#include "nds.h"

#include <stdlib.h>
#include <string.h>

#include "nds_prof.h"

/* ------------------------------------------------------------------ */
/* The geometry engine in time                                         */
/* ------------------------------------------------------------------ */

/* The present, as far as the 3D engine is concerned: the ARM9's own clock
 * while the ARM9 is the one running (only it can reach these registers),
 * otherwise the time events are being processed at. */
static uint64_t gx_present(const NdsMachine *m)
{
    uint64_t t = m->running == NDS_ARM9 ? nds9_now(m) : m->now;
    return t > m->gx_time ? t : m->gx_time;
}

/* POWCNT1 bits 2 and 3 power the rendering engine and the geometry engine
 * (GBATEK, "DS Power Control"; melonDS for what "off" does). Without power
 * an engine's ports take no writes. The geometry engine then stands still
 * and holds nothing up; the rendering engine draws nothing. */
static bool rendering_on(const NdsMachine *m)
{
    return (m->powcnt1 & 0x0004u) != 0;
}

static bool geometry_on(const NdsMachine *m)
{
    return (m->powcnt1 & 0x0008u) != 0;
}

static void gx_run_to(NdsMachine *m, uint64_t t)
{
    if (!geometry_on(m)) {
        /* Time passes; nothing runs. */
        if (t > m->gx_time) m->gx_time = t;
        return;
    }
    while (t > m->gx_time) {
        uint64_t step = t - m->gx_time;
        if (step > 0x40000000u) step = 0x40000000u;
        nds_gx_run(&m->gx, (uint32_t)step);
        m->gx_time += step;
    }
}

/* After anything that can change the engine's state: its interrupt request,
 * when it next changes by itself, a geometry-FIFO DMA that may now send, and
 * whether the ARM9 is held because the FIFO has no room. */
static void gx_changed(NdsMachine *m)
{
    bool more_dma = false;
    for (int bursts = 0;; bursts++) {
        /* The request is a level: IF.21 is set while the FIFO's condition
         * holds and clear while it does not (GBATEK: "set while and as long
         * as the IRQ condition is true"). */
        if (nds_gx_irq(&m->gx)) {
            nds_raise_irq(m, NDS_ARM9, NDS_IRQ_GXFIFO);
        } else if (m->if_[NDS_ARM9] & NDS_IRQ_GXFIFO) {
            m->if_[NDS_ARM9] &= ~(uint32_t)NDS_IRQ_GXFIFO;
            nds_update_irq(m, NDS_ARM9);
        }
        /* The DMA sends up to 112 words whenever the FIFO is less than half
         * full. Its writes come back through nds_video_write9, hence the
         * guard. */
        if (m->gx_in_dma || !geometry_on(m) || !nds_gx_dma_request(&m->gx)) break;
        /* A channel whose words never fill the FIFO (16-bit units, or a
         * destination that is not the FIFO) would keep this going for ever:
         * a few bursts at a time, then the rest from an event. */
        if (bursts == 8) {
            more_dma = true;
            break;
        }
        m->gx_in_dma = true;
        bool sent = nds_dma_trigger(m, NDS_ARM9, NDS_DMA_GXFIFO);
        m->gx_in_dma = false;
        if (!sent) break;
        gx_run_to(m, gx_present(m));
    }
    m->stalled[NDS_ARM9] = geometry_on(m) && nds_gx_fifo_full(&m->gx);
    /* A held CPU stops where it is: the engine has to catch up first. */
    if (m->stalled[NDS_ARM9] && m->running == NDS_ARM9) nds_sync(m);

    /* The engine is otherwise brought up to date only when something looks
     * at it. An event is needed when something is waiting for it to get
     * somewhere by itself: the held ARM9, an interrupt that is enabled and
     * not yet requested, or a DMA waiting for room. The engine says when it
     * finishes what it is doing; a FIFO holding L entries cannot hold fewer
     * than N until L - N - 1 cycles after that (nds_gx.h), so a waiter for
     * "less than half full" or "empty" need not be woken for every entry. */
    uint64_t at = UINT64_MAX;
    const uint32_t busy = geometry_on(m) ? nds_gx_cycles_until_event(&m->gx) : 0;
    if (more_dma) {
        at = m->gx_time + 1u;
    } else if (busy) {
        const uint32_t level = m->gx.fifo_count;
        const bool requested = nds_gx_irq(&m->gx);
        uint32_t extra = UINT32_MAX;
        bool wants_half = m->gx.irq_mode == 1 && !requested;
        if (!nds_gx_dma_request(&m->gx)) {
            for (int ch = 0; ch < 4; ch++) {
                uint32_t control = m->dma[NDS_ARM9][ch].control;
                if ((control & 0x80000000u) && ((control >> 27) & 7u) == NDS_DMA_GXFIFO) wants_half = true;
            }
        }
        if (wants_half) extra = level > 129u ? level - 129u : 0;
        if (m->gx.irq_mode == 2 && !requested) {
            uint32_t until_empty = level > 2u ? level - 2u : 0;
            if (until_empty < extra) extra = until_empty;
        }
        if (m->stalled[NDS_ARM9]) extra = 0;
        if (extra != UINT32_MAX) at = m->gx_time + busy + extra;
    }
    nds_schedule(m, NDS_EVENT_GX, at);
}

void nds_video_gx_event(NdsMachine *m)
{
    NDS_PROF_BEGIN(NDS_PROF_GEOMETRY);
    gx_run_to(m, m->now);
    gx_changed(m);
    NDS_PROF_END();
}

bool nds_video_gx_irq(const NdsMachine *m)
{
    return nds_gx_irq(&m->gx);
}

/* A geometry-FIFO DMA was just enabled: it starts at once if the FIFO
 * already has room. */
void nds_video_dma_enabled(NdsMachine *m)
{
    gx_run_to(m, gx_present(m));
    gx_changed(m);
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static bool engine_powered(const NdsMachine *m, int engine);

/* Which module an ARM9 I/O offset belongs to. Accesses are aligned to their
 * size, so one never spans two of these. */
enum { OWNER_NONE, OWNER_2D_A, OWNER_2D_B, OWNER_GX };

static int owner_of(uint32_t off)
{
    if (off < 0x070) {
        if ((off & ~3u) == 0x004) return OWNER_NONE; /* DISPSTAT, VCOUNT */
        if ((off & ~3u) == 0x060) return OWNER_GX;   /* DISP3DCNT */
        return OWNER_2D_A;
    }
    if ((off >= 0x320 && off < 0x3C0) || (off >= 0x400 && off < 0x6A4)) return OWNER_GX;
    if (off >= 0x1000 && off < 0x1070) return OWNER_2D_B;
    return OWNER_NONE;
}

bool nds_video_read9(NdsMachine *m, uint32_t addr, int size, uint32_t *value)
{
    uint32_t off = addr - 0x04000000u;
    switch (owner_of(off)) {
    case OWNER_2D_A:
        *value = nds_2d_read(&m->engine[0], off, size);
        return true;
    case OWNER_2D_B:
        *value = nds_2d_read(&m->engine[1], off - 0x1000u, size);
        return true;
    case OWNER_GX:
        NDS_PROF_BEGIN(NDS_PROF_GEOMETRY);
        gx_run_to(m, gx_present(m));
        *value = nds_gx_read(&m->gx, addr, size);
        /* Reading can change nothing by itself, but the run just made may
         * have. */
        gx_changed(m);
        NDS_PROF_END();
        return true;
    default:
        return false;
    }
}

bool nds_video_write9(NdsMachine *m, uint32_t addr, uint32_t value, int size)
{
    uint32_t off = addr - 0x04000000u;
    switch (owner_of(off)) {
    case OWNER_2D_A:
    case OWNER_2D_B: {
        const int engine = owner_of(off) == OWNER_2D_B;
        const uint32_t reg = off & 0xFFFu;
        /* 0x008-0x05F do not take writes while the engine has no power. */
        if (reg >= 0x008 && reg < 0x060 && !engine_powered(m, engine)) return true;
        nds_2d_write(&m->engine[engine], reg, value, size);
        return true;
    }
    case OWNER_GX:
        /* An engine without power takes no writes: the geometry engine's
         * ports are 0x400 up, the rendering engine's 0x320-0x3BF. DISP3DCNT
         * always does. */
        if (off >= 0x400 ? !geometry_on(m) : off >= 0x320 && !rendering_on(m)) return true;
        NDS_PROF_BEGIN(NDS_PROF_GEOMETRY);
        gx_run_to(m, gx_present(m));
        nds_gx_write(&m->gx, addr, value, size);
        gx_changed(m);
        NDS_PROF_END();
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Lines                                                               */
/* ------------------------------------------------------------------ */

/* A VRAM block (A-D, 128 KiB: eight pages) as the LCDC region shows it,
 * which is where display mode 2 and display capture reach it. The bus's map
 * says whether it is there. */
static uint8_t *lcdc_block(NdsMachine *m, int block)
{
    return m->map.lcdc[block * 8];
}

/* POWCNT1 bits 1 and 9 power the two engines (GBATEK, "DS Power Control").
 * A switched-off engine B shows white and ignores writes to its background,
 * window and blend registers. Engine A's registers are ignored the same way
 * but it goes on drawing, as melonDS 1.1 has it; GBATEK does not say. */
static bool engine_powered(const NdsMachine *m, int engine)
{
    return (m->powcnt1 & (engine == 0 ? 0x0002u : 0x0200u)) != 0;
}

static bool engine_on(const NdsMachine *m, int engine)
{
    return engine == 0 || engine_powered(m, engine);
}

static void vram_for_engine(NdsMachine *m, int engine, Nds2dVram *v)
{
    const NdsVramMap *map = &m->map;
    memset(v, 0, sizeof *v);
    if (engine == 0) {
        for (int i = 0; i < 32; i++) v->bg[i] = map->bg_a[i];
        for (int i = 0; i < 16; i++) v->obj[i] = map->obj_a[i];
        for (int i = 0; i < 4; i++) v->bg_ext_pal[i] = map->bg_ext_a[i];
        v->obj_ext_pal = map->obj_ext_a;
        v->palette = m->palette;
        v->oam = m->oam;
        for (int i = 0; i < 4; i++) v->lcdc[i] = v->capture[i] = lcdc_block(m, i);
    } else {
        for (int i = 0; i < 8; i++) v->bg[i] = map->bg_b[i];
        for (int i = 0; i < 8; i++) v->obj[i] = map->obj_b[i];
        for (int i = 0; i < 4; i++) v->bg_ext_pal[i] = map->bg_ext_b[i];
        v->obj_ext_pal = map->obj_ext_b;
        v->palette = m->palette + 0x400;
        v->oam = m->oam + 0x400;
    }
}

/* 6 bits a channel, as the engines produce, to 0xAARRGGBB. */
static inline uint32_t argb_from_rgb666(uint32_t c)
{
    uint32_t r = c & 63u, g = (c >> 6) & 63u, b = (c >> 12) & 63u;
    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* ------------------------------------------------------------------ */
/* The larger picture                                                  */
/* ------------------------------------------------------------------ */

/*
 * What nds_set_internal_scale() adds, all of it beside the machine: the 3D
 * engine's frame on the finer grid, a frame for each screen, a finer twin of
 * what the capture unit took, and what is known of which screen has been
 * showing any of it.
 *
 * A line is drawn larger in one of two ways, both a second stacking of its
 * layers (nds_2d_stack_scaled):
 *
 *  - Engine A with the 3D picture as BG0: around the larger 3D picture.
 *  - Either engine with a direct-colour bitmap background, or bitmap
 *    sprites, showing pixel for pixel what the capture unit took from a
 *    line that was itself drawn larger: around those pixels' finer twins.
 *    This is the other screen of a game with 3D on both: engine A draws
 *    one screen a frame and is captured, and the other screen shows last
 *    frame's capture. (Phantom Hourglass shows one screen's capture as a
 *    background and the other's as sprites.)
 *
 * The twin is kept by block and line with the address the capture unit
 * wrote the line at and what it wrote. A bitmap pixel is taken to be
 * showing it when it is read from those same bytes of video memory and
 * they still hold what was captured; anything else - a game that drew over
 * the capture, scaled it, flipped it - is the screen's own pixel.
 *
 * A screen's larger frame is offered (nds_fine_frame) once every line of
 * it has been written in each of the last two frames, with a line drawn
 * larger among them. A screen with nothing drawn larger the frame before
 * is not written at all, so a screen of backgrounds and sprites alone
 * costs nothing; one that starts showing 3D is offered three frames later.
 */
typedef struct FineCapture {
    uint32_t *rows;                    /* 192 * scale rows of 256 * scale, as nds_2d_capture_scaled() leaves them */
    const uint8_t *first;              /* where the capture unit's line 0 begins: line y is 512 * y bytes on */
    bool have[NDS_SCREEN_HEIGHT];      /* which lines have a twin, */
    uint16_t taken[NDS_SCREEN_HEIGHT][NDS_SCREEN_WIDTH]; /* ... and what the capture unit wrote for them */
} FineCapture;

struct NdsFine {
    int scale;
    uint32_t *gx;          /* 192 * scale rows of 256 * scale, in nds_gx_line()'s format */
    bool gx_drawn;         /* ... and it is the frame the 3D engine's own picture is of */
    uint32_t *frame[2];    /* each screen, 0xAARRGGBB, the same size */
    uint32_t *rows;        /* scale rows of 256 * scale: a line being stacked */
    FineCapture capture[4]; /* VRAM blocks A to D */
    bool keep[2];          /* this frame: the screen is being written (it had a larger line last frame) */
    uint8_t lines[2];      /* ... lines of it written so far, */
    bool larger[2];        /* ... whether one of them was drawn larger, */
    bool missed[2];        /* ... and whether a line was left out */
    uint8_t streak[2];     /* finished frames in a row the screen was whole, with a larger line: up to 2 */
};

static void fine_free(NdsMachine *m)
{
    if (!m->fine) return;
    free(m->fine->gx);
    free(m->fine->frame[0]);
    free(m->fine->frame[1]);
    free(m->fine->rows);
    for (int i = 0; i < 4; i++) free(m->fine->capture[i].rows);
    free(m->fine);
    m->fine = NULL;
}

void nds_set_internal_scale(NdsMachine *m, int scale)
{
    if (!m) return;
    if (scale < 1) scale = 1;
    if (scale > NDS_INTERNAL_SCALE_MAX) scale = NDS_INTERNAL_SCALE_MAX;
    if (nds_internal_scale(m) == scale) return;
    fine_free(m);
    if (scale == 1) return;
    struct NdsFine *f = calloc(1, sizeof *f);
    const size_t pixels = (size_t)NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * (size_t)scale * (size_t)scale;
    if (f) {
        f->scale = scale;
        f->gx = calloc(pixels, sizeof(uint32_t));
        f->frame[0] = calloc(pixels, sizeof(uint32_t));
        f->frame[1] = calloc(pixels, sizeof(uint32_t));
        f->rows = calloc((size_t)NDS_SCREEN_WIDTH * (size_t)scale * (size_t)scale, sizeof(uint32_t));
    }
    m->fine = f;
    /* No memory for it: the console's own picture, as at a scale of 1. */
    if (!f || !f->gx || !f->frame[0] || !f->frame[1] || !f->rows) fine_free(m);
}

int nds_internal_scale(const NdsMachine *m)
{
    return m && m->fine ? m->fine->scale : 1;
}

const uint32_t *nds_fine_frame(const NdsMachine *m, int screen, int *width, int *height)
{
    if (!m || !m->fine || screen < 0 || screen > 1 || m->fine->streak[screen] < 2) return NULL;
    if (width) *width = NDS_SCREEN_WIDTH * m->fine->scale;
    if (height) *height = NDS_SCREEN_HEIGHT * m->fine->scale;
    return m->fine->frame[screen];
}

/* Line 215: the 3D engine draws its picture, and with it the larger one. */
static void render_3d(NdsMachine *m, const NdsGxVram *vram)
{
    struct NdsFine *f = m->fine;
    if (f) f->gx_drawn = nds_gx_render_both(&m->gx, vram, f->scale, f->gx);
    else nds_gx_render(&m->gx, vram);
}

/* The finer twin of a pixel read from `p` in video memory: the first of
 * `scale` rows of `scale` pixels, 256 * scale apart; or NULL when the
 * capture unit did not write there, the line it wrote has no twin, or the
 * two bytes are no longer what it wrote. */
static const uint32_t *fine_twin(const struct NdsFine *f, const uint8_t *p)
{
    const size_t n = (size_t)f->scale, wide = NDS_SCREEN_WIDTH * n;
    for (int block = 0; block < 4; block++) {
        const FineCapture *c = &f->capture[block];
        if (!c->rows || !c->first || p < c->first) continue;
        const size_t offset = (size_t)(p - c->first);
        if (offset >= (size_t)NDS_SCREEN_HEIGHT * NDS_SCREEN_WIDTH * 2u || (offset & 1u)) continue;
        const size_t line = offset / (NDS_SCREEN_WIDTH * 2u), column = (offset / 2u) % NDS_SCREEN_WIDTH;
        if (!c->have[line] || nds_get_le16(p) != c->taken[line][column]) return NULL;
        return c->rows + line * n * wide + column * n;
    }
    return NULL;
}

/* The capture unit took line y. Keep its finer twin when the line it took
 * was drawn larger: `stacked` is that line's `scale` rows before master
 * brightness, or NULL when the line was the screen's own. */
static void fine_captured(NdsMachine *m, int y, const Nds2dLayers *layers, const uint32_t *stacked)
{
    struct NdsFine *f = m->fine;
    FineCapture *c = &f->capture[layers->capture_block & 3u];
    const size_t n = (size_t)f->scale, wide = NDS_SCREEN_WIDTH * n;
    c->have[y] = false;
    if (layers->capture != 1) return;
    if (layers->capture_3d ? !f->gx_drawn : !stacked) return;
    if (!c->rows) {
        c->rows = calloc((size_t)NDS_SCREEN_HEIGHT * n * wide, sizeof(uint32_t));
        if (!c->rows) return;
    }
    /* A capture somewhere else in the block: the lines kept are where the
     * last one put them, and are forgotten. */
    const uint8_t *first = layers->capture_at - (size_t)y * NDS_SCREEN_WIDTH * 2u;
    if (c->first != first) {
        memset(c->have, 0, sizeof c->have);
        c->first = first;
    }
    uint32_t *twin = c->rows + (size_t)y * n * wide;
    if (!layers->capture_3d) {
        nds_2d_capture_scaled(stacked, n * wide, 0, twin);
    } else {
        /* The 3D picture alone, scrolled as BG0 is (nds_2d.c, scroll_3d). */
        const uint32_t *gx = f->gx + (size_t)y * n * wide;
        const size_t hofs = (size_t)layers->hofs * n, region = 512u * n;
        for (size_t j = 0; j < n; j++) {
            for (size_t x = 0; x < wide; x++) {
                size_t from = x + hofs;
                if (from >= region) from -= region;
                twin[j * wide + x] = from < wide ? gx[j * wide + from] : 0;
            }
        }
        nds_2d_capture_scaled(twin, n * wide, 1, twin);
    }
    c->have[y] = true;
    memcpy(c->taken[y], layers->capture_at, sizeof c->taken[y]);
}

/* Line y of a screen, larger. `line` is that line as the engine drew it,
 * and `layers` what it was stacked from. */
static void fine_line(NdsMachine *m, int screen, int y, const Nds2dLayers *layers, const uint32_t *line)
{
    struct NdsFine *f = m->fine;
    const size_t n = (size_t)f->scale, wide = NDS_SCREEN_WIDTH * n;

    /* Which layer, if any, has finer pixels, and where each pixel's are:
     * the 3D picture; a bitmap background showing a capture that has a
     * twin; bitmap sprites doing the same. */
    const uint32_t *finer[NDS_SCREEN_WIDTH];
    int layer = -1;
    if (layers->composed) {
        if (layers->has_3d && f->gx_drawn) {
            /* "256 pixels for the 3D image, followed by 256 transparent
             * pixels", moved by BG0HOFS (nds_2d.c, scroll_3d). */
            const uint32_t *gx = f->gx + (size_t)y * n * wide;
            for (size_t x = 0; x < NDS_SCREEN_WIDTH; x++) {
                const size_t from = (x + layers->hofs) & 0x1FFu;
                finer[x] = from < NDS_SCREEN_WIDTH ? gx + from * n : NULL;
            }
            layer = 0;
        }
        for (int k = 0; k < layers->layers && layer < 0; k++) {
            const int bg = layers->order[k];
            const uint8_t *row = layers->direct_row[bg];
            if (!row) continue;
            bool any = false;
            for (size_t x = 0; x < NDS_SCREEN_WIDTH; x++) {
                finer[x] = fine_twin(f, row + x * 2u);
                any = any || finer[x];
            }
            if (any) layer = bg;
        }
        if (layer < 0 && layers->obj_on) {
            bool any = false;
            for (size_t x = 0; x < NDS_SCREEN_WIDTH; x++) {
                finer[x] = layers->obj_src[x] ? fine_twin(f, layers->obj_src[x]) : NULL;
                any = any || finer[x];
            }
            if (any) layer = NDS_2D_LAYER_OBJ;
        }
    }

    const bool write = f->keep[screen];
    uint32_t *rows = f->frame[screen] + (size_t)y * n * wide;
    if (layer >= 0) {
        /* Stacked into the working rows: the capture unit takes them as
         * they are, the screen after master brightness. */
        nds_2d_stack_scaled(layers, layer, finer, wide, f->scale, f->rows, wide);
        if (layers->capture) fine_captured(m, y, layers, f->rows);
        f->larger[screen] = true;
        if (write) {
            nds_2d_brighten_scaled(layers, f->rows, n * wide);
            for (size_t i = 0; i < n * wide; i++) rows[i] = argb_from_rgb666(f->rows[i]);
        }
    } else {
        if (layers->capture) fine_captured(m, y, layers, NULL);
        if (write) {
            for (size_t x = 0; x < NDS_SCREEN_WIDTH; x++) {
                const uint32_t pixel = argb_from_rgb666(line[x]);
                for (size_t i = 0; i < n; i++) rows[x * n + i] = pixel;
            }
            for (size_t j = 1; j < n; j++) memcpy(rows + j * wide, rows, wide * sizeof(uint32_t));
        }
    }
    if (!write) f->missed[screen] = true;
    else if (f->lines[screen] < NDS_SCREEN_HEIGHT) f->lines[screen]++;
}

/* Line 192: the frame is finished. Which screens were whole, with a line
 * drawn larger; and which are worth writing next frame. */
static void fine_end_frame(NdsMachine *m)
{
    struct NdsFine *f = m->fine;
    if (!f) return;
    for (int screen = 0; screen < 2; screen++) {
        const bool whole = f->lines[screen] == NDS_SCREEN_HEIGHT && f->larger[screen] && !f->missed[screen];
        f->streak[screen] = whole ? (uint8_t)(f->streak[screen] < 2 ? f->streak[screen] + 1 : 2) : 0;
        f->keep[screen] = f->larger[screen];
        f->lines[screen] = 0;
        f->larger[screen] = false;
        f->missed[screen] = false;
    }
}

/* One visible line of both screens, at its HBlank. */
void nds_video_render_line(NdsMachine *m, int y)
{
    /* POWCNT1 bit 15: engine A drives the top screen when set. */
    const int screen_a = (m->powcnt1 & 0x8000u) ? 0 : 1;
    uint32_t line[NDS_SCREEN_WIDTH];
    Nds2dVram vram;

    NDS_PROF_BEGIN(NDS_PROF_RENDER_2D);
    for (int engine = 0; engine < 2; engine++) {
        const int screen = engine == 0 ? screen_a : screen_a ^ 1;
        uint32_t *out = m->framebuffer[screen] + y * NDS_SCREEN_WIDTH;
        if (!engine_on(m, engine)) {
            /* Engine B switched off: its screen is white. */
            for (int x = 0; x < NDS_SCREEN_WIDTH; x++) out[x] = 0xFFFFFFFFu;
            continue;
        }
        vram_for_engine(m, engine, &vram);
        /* Engine A always gets the 3D line: a display capture can take it
         * even when the picture does not show it. */
        const uint32_t *line_3d = engine == 0 && rendering_on(m) ? nds_gx_line(&m->gx, y) : NULL;
        if (m->fine) {
            Nds2dLayers layers;
            nds_2d_render_line_layers(&m->engine[engine], &vram, y, line_3d, line, &layers);
            fine_line(m, screen, y, &layers, line);
        } else {
            nds_2d_render_line(&m->engine[engine], &vram, y, line_3d, line);
        }
        for (int x = 0; x < NDS_SCREEN_WIDTH; x++) out[x] = argb_from_rgb666(line[x]);
    }
    NDS_PROF_END();
}

/* A new line has begun (VCOUNT has just changed to `line`). */
void nds_video_line_start(NdsMachine *m, int line)
{
    if (line == 0) {
        nds_2d_begin_frame(&m->engine[0]);
        nds_2d_begin_frame(&m->engine[1]);
    } else if (line == NDS_VISIBLE_LINES) {
        nds_2d_end_frame(&m->engine[0]);
        nds_2d_end_frame(&m->engine[1]);
        fine_end_frame(m);
        NDS_PROF_BEGIN(NDS_PROF_GEOMETRY);
        gx_run_to(m, m->now);
        if (geometry_on(m)) nds_gx_vblank(&m->gx);
        gx_changed(m);
        NDS_PROF_END();
    }
    if (line >= NDS_VISIBLE_LINES) {
        nds_2d_blank_line(&m->engine[0], line);
        nds_2d_blank_line(&m->engine[1], line);
    }
    if (line == 215 && rendering_on(m)) {
        NdsGxVram vram;
        for (int i = 0; i < 4; i++) vram.texture[i] = m->map.texture[i];
        for (int i = 0; i < 6; i++) vram.palette[i] = m->map.tex_pal[i];
        NDS_PROF_BEGIN(NDS_PROF_RENDER_3D);
        render_3d(m, &vram);
        NDS_PROF_END();
    }
}

/* POWCNT1 is about to change, and has changed from `before`. */
void nds_video_power_changing(NdsMachine *m)
{
    gx_run_to(m, gx_present(m));
}

void nds_video_power_changed(NdsMachine *m, uint16_t before)
{
    /* The rendering engine switched off shows nothing until the next swap
     * after it comes back. */
    if ((before & 0x0004u) && !rendering_on(m)) m->gx.render_count = 0;
    gx_changed(m);
}

void nds_video_reset(NdsMachine *m)
{
    nds_2d_reset(&m->engine[0], 0);
    nds_2d_reset(&m->engine[1], 1);
    nds_gx_reset(&m->gx);
    if (m->fine) {
        /* The larger picture starts again with the machine. */
        m->fine->gx_drawn = false;
        for (int screen = 0; screen < 2; screen++) {
            m->fine->streak[screen] = m->fine->lines[screen] = 0;
            m->fine->keep[screen] = m->fine->larger[screen] = m->fine->missed[screen] = false;
        }
        for (int block = 0; block < 4; block++) memset(m->fine->capture[block].have, 0, sizeof m->fine->capture[block].have);
    }
    m->gx_time = 0;
    m->gx_in_dma = false;
    m->stalled[NDS_ARM9] = m->stalled[NDS_ARM7] = false;
    /* Line 0 of the first frame. */
    nds_2d_begin_frame(&m->engine[0]);
    nds_2d_begin_frame(&m->engine[1]);
}
