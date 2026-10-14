/*
 * SNES picture processing unit (PPU1 + PPU2): VRAM, CGRAM, OAM, the
 * registers at $2100-$213F, and a scanline renderer.
 *
 * The machine (snes.c) owns timing. Before every call it sets the public
 * position fields (hcounter, vcounter, vblank), so register reads, the
 * counter latch and VRAM/OAM/CGRAM access rules see the current beam
 * position. It calls snes_ppu_render_line() once per visible line, at the
 * point in the line where the line's registers are final (after the CPU
 * and HDMA wrote them for that line), and snes_ppu_frame_start() /
 * snes_ppu_vblank_start() at the frame edges.
 *
 * The struct holds no pointers: the machine is saved as one block.
 */
#ifndef SNES_PPU_H
#define SNES_PPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Output: always 512 x 480 XRGB8888 storage. out_width is 256, or 512 when
 * a pseudo-hires / hires mode was used this frame; out_height is 224 or 239
 * (overscan), doubled when interlaced. Row y is at framebuffer + y * 512. */
#define SNES_FB_WIDTH 512
#define SNES_FB_HEIGHT 480

typedef struct SnesPpu SnesPpu;

void snes_ppu_power(SnesPpu *p);
void snes_ppu_reset(SnesPpu *p);

/* $2100-$2133 (reg = address & 0xFF, 0x00-0x33). */
void snes_ppu_write(SnesPpu *p, uint8_t reg, uint8_t value);

/* $2134-$213F. Other registers are write-only; the caller supplies the CPU
 * open-bus value `mdr` for them and for the unused bits of the readable
 * ones (PPU1 and PPU2 keep their own open-bus latches as on hardware). */
uint8_t snes_ppu_read(SnesPpu *p, uint8_t reg, uint8_t mdr);

/* Latch the H/V counters ($2137 read, or WRIO bit 7 falling). */
void snes_ppu_latch_counters(SnesPpu *p);

/* Frame edges: frame_start at V = 0 (interlace field, range/time-over
 * flags); vblank_start when V reaches the first line after the picture
 * (OAM address reload). */
void snes_ppu_frame_start(SnesPpu *p);
void snes_ppu_vblank_start(SnesPpu *p);

/* Render picture line y (1..224, or 1..239 with overscan) into the
 * framebuffer (output row y - 1). */
void snes_ppu_render_line(SnesPpu *p, int y);

/*
 * Mode 7 at a higher resolution. With a scale of 2 to SNES_MODE7_SCALE_MAX,
 * a line that shows the mode 7 plane is drawn a second time with that many
 * samples of the plane to a pixel each way, between the lines as well as
 * along them, into a second picture `scale` times the size. The picture in
 * `framebuffer`, and everything a game can read, is drawn exactly as
 * before: the larger one is for the display.
 *
 * snes_ppu_large_frame() is the last complete frame's larger picture, 256 *
 * scale pixels wide (0x00RRGGBB, a row every *width pixels), or NULL when
 * that frame showed no mode 7 line (then `framebuffer` is all there is).
 * Sprites, the backdrop and the lines of other modes are the console's
 * pixels, repeated. Lines with mosaic on the plane, interlaced frames and
 * pseudo-hires lines are not drawn larger.
 *
 * The larger picture is kept beside the PPU, not in it (a save state is
 * the PPU's bytes), and one PPU at a time has one: setting a scale for
 * another PPU takes it over. Returns false, changing nothing, for a scale
 * out of range or when there is no memory.
 */
#define SNES_MODE7_SCALE_MAX 4
bool snes_ppu_set_mode7_scale(SnesPpu *p, int scale);
int snes_ppu_mode7_scale(const SnesPpu *p);
const uint32_t *snes_ppu_large_frame(const SnesPpu *p, int *width, int *height);

/* Register state the machine needs. */
bool snes_ppu_overscan(const SnesPpu *p);   /* SETINI bit 2: 239 lines */
bool snes_ppu_interlace(const SnesPpu *p);  /* SETINI bit 0 */
bool snes_ppu_forced_blank(const SnesPpu *p);

/* Debug and test access: VRAM (64 KiB, little-endian words), CGRAM (512
 * bytes), OAM (544 bytes), and the last value written to a write-only
 * register (reg 0x00-0x33). */
const uint8_t *snes_ppu_vram(const SnesPpu *p);
const uint8_t *snes_ppu_cgram(const SnesPpu *p);
const uint8_t *snes_ppu_oam(const SnesPpu *p);
uint8_t snes_ppu_reg(const SnesPpu *p, uint8_t reg);

/* The full definition (snes_ppu_state.h) is owned by snes_ppu.c. */
#include "snes_ppu_state.h"

#ifdef __cplusplus
}
#endif

#endif /* SNES_PPU_H */
