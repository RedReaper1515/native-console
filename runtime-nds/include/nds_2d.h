/*
 * The DS's two 2D graphics engines (GBATEK "DS Video").
 *
 * One Nds2d is one engine: A (main; I/O at 0x04000000) or B (sub; I/O at
 * 0x04001000). The machine owns VRAM, palette RAM and OAM and hands the
 * engine a view of them, as they are mapped at that moment, with every line
 * it asks for. The engine owns its registers and nothing else.
 *
 * Call order, per frame and per engine:
 *
 *   nds_2d_begin_frame()            at the start of line 0
 *   nds_2d_render_line(y)           once for each of lines 0-191, in order, at the start
 *                                   of that line's HBlank
 *   nds_2d_end_frame()              at the start of line 192
 *   nds_2d_blank_line(line)         optional, at the start of each of lines 192-262
 *
 * Register writes take effect on the next line rendered.
 */
#ifndef NDS_2D_H
#define NDS_2D_H

#include "nds_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of register space one engine decodes: 0x000-0x06F from its base. */
#define NDS_2D_REG_SIZE 0x70

/*
 * VRAM as the machine has it mapped for one engine right now. Regions are
 * given as 16 KiB pages; NULL = nothing mapped there (reads as zero).
 *
 * Engine B uses bg[0..7] and obj[0..7] only (its BG and OBJ VRAM are
 * 128 KiB each) and never looks at lcdc[] or capture[].
 *
 * lcdc[n] and capture[n] are the whole of VRAM block A-D number n (128 KiB,
 * contiguous) while that block is mapped as LCDC memory (VRAMCNT MST 0), and
 * NULL otherwise: a block that is not there displays black, reads as zero
 * as a capture source, and takes no capture. They may be the same memory.
 */
typedef struct Nds2dVram {
    const uint8_t *bg[32];        /* BG VRAM: engine A 512 KiB (32 pages), engine B 128 KiB (8 pages) */
    const uint8_t *obj[16];       /* OBJ VRAM: engine A 256 KiB (16 pages), engine B 128 KiB (8 pages) */
    const uint8_t *bg_ext_pal[4]; /* BG extended palette slots 0-3, 8 KiB each */
    const uint8_t *obj_ext_pal;   /* OBJ extended palette, 8 KiB */
    const uint8_t *palette;       /* this engine's 1 KiB of standard palette RAM: BG at 0x000, OBJ at 0x200 */
    const uint8_t *oam;           /* this engine's 1 KiB of OAM; NULL = no sprites */
    const uint8_t *lcdc[4];       /* engine A only: blocks A-D for display mode 2 and as capture source */
    uint8_t *capture[4];          /* engine A only: blocks A-D as display-capture destinations */
} Nds2dVram;

/*
 * All state of one engine. Plain data: a save state is a memcpy of it.
 * nds_2d_reset() zeroes every byte, so two engines that were given the same
 * calls compare equal with memcmp.
 */
typedef struct Nds2d {
    /* The register file, little-endian, holding what was written after the
     * write masks. Byte i is the register byte at base + i. */
    uint8_t reg[NDS_2D_REG_SIZE];

    /* BG2 and BG3 internal reference points (20.8 fixed point, 28 bits,
     * sign-extended): copies of BGxX/BGxY made at the start of the frame and
     * when those are written, stepped by PB/PD after every line. */
    int32_t ref_x[2], ref_y[2];

    /* Main memory display: the pixels written to DISP_MMEM_FIFO since the
     * last line was rendered. */
    uint16_t mmem_line[NDS_SCREEN_WIDTH];
    uint16_t mmem_pos;

    /* Windows 0 and 1 are a pair of flip-flops each: set when the counter
     * reaches the first coordinate, cleared when it reaches the second. */
    uint16_t win_line; /* first line (0-262) whose vertical comparison has not been made */
    uint8_t win_v[2];  /* vertical flip-flops */
    uint8_t win_h[2];  /* horizontal flip-flops as the last line left them */

    uint8_t engine;         /* 0 = A, 1 = B */
    uint8_t capture_active; /* engine A: a display capture is running this frame */

    /* Vertical mosaic: the picture's lines repeat in blocks. */
    uint8_t bg_mosaic_count;  /* lines since the current BG block's first line */
    uint8_t bg_mosaic_max;    /* that block's size - 1, as MOSAIC had it when the block began */
    uint8_t obj_mosaic_count; /* 4-bit line counter of the OBJ block */
    uint8_t obj_mosaic_line;  /* first line of the current OBJ block */
    uint8_t reserved[2];
} Nds2d;

void nds_2d_reset(Nds2d *e, int engine); /* 0 = engine A (main), 1 = engine B (sub) */

/*
 * Register access. offset is from the engine's base (0x04000000 for A,
 * 0x04001000 for B). The engine decodes 0x000-0x003 (DISPCNT), 0x008-0x05F
 * (BG control, scroll, affine, windows, mosaic, blending), 0x064-0x06B
 * (DISPCAPCNT and DISP_MMEM_FIFO, engine A only) and 0x06C-0x06F
 * (MASTER_BRIGHT). size is 1, 2 or 4, at any offset. Bytes the engine does
 * not decode read as zero and ignore writes, so an access that straddles a
 * register this engine does not own (DISPSTAT, VCOUNT, DISP3DCNT) is safe to
 * pass on whole.
 */
uint32_t nds_2d_read(const Nds2d *e, uint32_t offset, int size);
void nds_2d_write(Nds2d *e, uint32_t offset, uint32_t value, int size);

void nds_2d_begin_frame(Nds2d *e); /* start of line 0 */
void nds_2d_end_frame(Nds2d *e);   /* start of line 192 */

/*
 * Optional: the start of a line outside the picture (192-262). Only the
 * window flip-flops care, which compare on every line of the frame. Without
 * these calls the next nds_2d_begin_frame() makes the comparisons for the
 * lines it was not told about, with the window registers as they are then.
 */
void nds_2d_blank_line(Nds2d *e, int line);

/*
 * Render visible line y (0-191) into out: 256 pixels, bits 0-5 red, 6-11
 * green, 12-17 blue (6 bits each), after master brightness. A 15-bit colour
 * comes out with each channel doubled, so the white of a palette is 62; 63
 * is reached only by the 3D layer, by blending and brightness, and by the
 * white of forced blank and of a display that is off.
 *
 * line_3d is engine A's 3D layer for this line, or NULL: 256 pixels with
 * bits 0-5 red, 6-11 green, 12-17 blue, 18-22 alpha (0-31; 0 = nothing
 * there), not scrolled (the engine applies BG0HOFS itself). It is shown as
 * BG0 when DISPCNT bit 3 is set, and it is the "3D screen" source of a
 * display capture. Display capture, when active on this line, writes into
 * vram->capture[].
 *
 * out may be NULL to run the line for its side effects alone (capture, the
 * internal counters).
 */
void nds_2d_render_line(Nds2d *e, const Nds2dVram *vram, int y, const uint32_t *line_3d, uint32_t *out);

/*
 * A line's layers as they were just before being stacked into the picture,
 * for a display that draws the 3D picture larger than the screen and has to
 * stack them again around its own, finer pixels. Nothing of the engine is
 * left in it; the two kinds of pointer it has point into the video memory
 * the engine was given.
 *
 * Pixels are in the engine's own format (colour in bits 0-17, the layer and
 * its flags above), and mean something only to nds_2d_stack_scaled().
 *
 * Besides the 3D picture there is one more way a line can hold pixels that
 * were once drawn finer: a picture the capture unit took, shown again as a
 * bitmap. That is how games put 3D on both screens: engine A draws one
 * screen a frame and is captured, and the other screen shows last frame's
 * capture as a direct-colour bitmap background or as bitmap sprites. So
 * the layers also say where the capture unit put this line (capture_at),
 * which backgrounds are a line of direct colour shown pixel for pixel and
 * where it lies (direct_row), and for each sprite pixel that is a pixel of
 * an unscaled bitmap sprite, where that pixel lies (obj_src): whoever
 * keeps a finer twin of what was captured can see that the same memory is
 * being shown.
 */
#define NDS_2D_LAYER_OBJ 4 /* the sprites, where a layer is named by number: 0-3 are the backgrounds */
typedef struct Nds2dLayers {
    uint8_t composed;        /* the line shows the stacked layers: display mode 1, not forced blank */
    uint8_t has_3d;          /* ... and the 3D picture is one of them, as BG0 */
    uint8_t obj_on;          /* ... and so are the sprites */
    uint8_t layers;          /* how many backgrounds are on, and in drawing order: */
    uint8_t order[4];        /* which, */
    uint8_t order_prio[4];   /* and its priority */
    uint16_t hofs;           /* BG0HOFS, which scrolls the 3D picture */
    uint16_t bldcnt, bldalpha, bldy;
    uint16_t master_bright;
    uint32_t backdrop;
    uint32_t bg[4][NDS_SCREEN_WIDTH]; /* bg[0] is the 3D picture's line, as the screen has it, with has_3d */
    uint32_t obj[NDS_SCREEN_WIDTH];
    uint8_t obj_prio[NDS_SCREEN_WIDTH];
    uint8_t enabled[NDS_SCREEN_WIDTH]; /* the windows: which layers show at each pixel, and the colour effect */
    /* For each background: when it is a direct-colour bitmap this line
     * shows 256 pixels of in a row, unscaled, where those pixels are (512
     * bytes); otherwise NULL. Set only with `composed`. */
    const uint8_t *direct_row[4];
    /* For each pixel of obj[] that is a pixel of a bitmap sprite drawn
     * neither scaled, turned nor flipped: where its two bytes are;
     * otherwise NULL. Set only with `composed` and obj_on. */
    const uint8_t *obj_src[NDS_SCREEN_WIDTH];
    /* The capture unit on this line: 0 took nothing; 1 took source A alone,
     * 256 pixels, and wrote them at capture_at (512 bytes), source A being
     * the stacked layers before master brightness, or with capture_3d the
     * 3D picture; 2 took something else (source B, or the two blended, or a
     * narrower line) into the same block. capture_block is the block (0-3
     * for A-D). */
    uint8_t capture, capture_3d, capture_block;
    const uint8_t *capture_at;
} Nds2dLayers;

/* nds_2d_render_line(), which also leaves the line's layers in `layers`
 * (when it is not NULL). The line drawn into `out`, the capture and every
 * counter are exactly nds_2d_render_line()'s. */
void nds_2d_render_line_layers(Nds2d *e, const Nds2dVram *vram, int y, const uint32_t *line_3d, uint32_t *out,
                               Nds2dLayers *layers);

/*
 * The line again, `scale` times as large each way, around one layer whose
 * pixels the caller has that much finer. Each of the screen's pixels
 * becomes scale by scale pixels whose other layers are that pixel's and
 * whose pixel of `layer` is its own; colour effects are applied to each.
 *
 * `finer` has an entry for each of the line's 256 pixels: NULL where the
 * caller has nothing finer (the layer's pixel is then the screen's own,
 * repeated), or the first of `scale` rows of `scale` pixels, each row
 * `finer_pitch` pixels after the one before, in line_3d's format: a colour,
 * and an alpha of 0 where there is nothing.
 *
 * `layer` is:
 *   0 on a line with layers->has_3d: the 3D picture, stacked as the 3D
 *     layer is (its own alpha blends it). The caller scrolls: entry x is
 *     the picture's pixel (x + layers->hofs) mod 512, NULL from 256 on.
 *   a background with layers->direct_row[layer]: that bitmap's line; a
 *     pixel is there or it is not.
 *   NDS_2D_LAYER_OBJ: the sprites; an entry only where layers->obj_src has
 *     one. The pixel keeps what its sprite gave it (priority, its own
 *     alpha) and takes the finer colour.
 *
 * `out` gets `scale` rows of 256 * scale pixels in out's format, each
 * `pitch` pixels after the one before, **before master brightness**, which
 * is what the capture unit takes; nds_2d_brighten_scaled() applies that.
 * Only for a line with layers->composed; any other line is the screen's
 * own, repeated.
 */
void nds_2d_stack_scaled(const Nds2dLayers *layers, int layer, const uint32_t *const *finer, size_t finer_pitch,
                         int scale, uint32_t *out, size_t pitch);
/* MASTER_BRIGHT, as the line had it, on `count` pixels in out's format. */
void nds_2d_brighten_scaled(const Nds2dLayers *layers, uint32_t *pixels, size_t count);
/*
 * `count` pixels as the capture unit would keep them and a direct-colour
 * bitmap show them again: five bits a channel, and there or not. `in` is in
 * out's format (stacked layers: every pixel is there) or, with from_3d, in
 * line_3d's (there where its alpha is not 0); `out` is in line_3d's, with
 * an alpha of 31 or 0.
 */
void nds_2d_capture_scaled(const uint32_t *in, size_t count, int from_3d, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NDS_2D_H */
