/*
 * NES picture processing unit: a dot-accurate NTSC RP2C02G.
 *
 * 341 dots x 262 scanlines; scanlines 0-239 are the picture, 240 is the
 * post-render line, the VBlank flag is set at scanline 241 dot 1, and 261
 * is the pre-render line. With rendering enabled, every other frame is one
 * dot shorter (the pre-render line jumps from dot 339 to scanline 0 dot 0).
 *
 * Position convention. (scanline, dot) names the dot the PPU is in: its
 * work has been done. nes_ppu_run(p, n) advances n dots, doing the work of
 * each dot it enters. "The caller has run the PPU to dot D" therefore means
 * p->dot == D, and a register access made then behaves like an access the
 * CPU makes during dot D on hardware, in the numbering the nesdev wiki
 * uses: a $2002 read at (241, 0) sees the VBlank flag clear and suppresses
 * it, a read at (241, 1) or (241, 2) sees it set.
 *
 * The PPU owns OAM and the palette RAM only. Pattern tables and nametables
 * belong to the cartridge: every access goes through the nes_ppu_bus_*
 * functions below, which the final program provides (link time, as the
 * CPU cores do for their buses).
 *
 * The struct is plain data: fixed-width fields, ordered so that there is
 * no padding, and no pointers except `user`. It is saved and compared as
 * one memory block.
 */
#ifndef NES_PPU_H
#define NES_PPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NES_PPU_WIDTH 256
#define NES_PPU_HEIGHT 240
#define NES_PPU_DOTS_PER_LINE 341

/* Timing of the one supported model. nes_ppu_power() copies these into the
 * `lines`, `vblank_line` and `odd_skip` fields, and nothing else in the
 * PPU names them: PAL (312 lines, VBlank at 241, no skipped dot) and Dendy
 * (312 lines, VBlank at 291) need other values there, plus the PAL details
 * listed in nes_ppu.c. */
#define NES_PPU_NTSC_LINES 262
#define NES_PPU_NTSC_VBLANK_LINE 241

/* Dots in a frame that skips nothing; an odd frame with rendering enabled
 * is one shorter. */
#define NES_PPU_NTSC_FRAME_DOTS (NES_PPU_DOTS_PER_LINE * NES_PPU_NTSC_LINES)

/* PPUSTATUS bits. */
#define NES_PPU_STATUS_OVERFLOW 0x20u
#define NES_PPU_STATUS_SPRITE0 0x40u
#define NES_PPU_STATUS_VBLANK 0x80u

typedef struct NesPpu NesPpu;

/* Power: everything cleared (palette RAM gets the values a real PPU was
 * observed to power up with), position (0, 0), `user` kept.
 * Reset: PPUCTRL, PPUMASK, the scroll/address latch (t, fine X, w), the
 * $2007 read buffer and the odd-frame flag are cleared and the position
 * returns to (0, 0); v, PPUSTATUS, OAMADDR, OAM and the palette survive.
 *
 * Neither starts the "writes to $2000/$2001/$2005/$2006 are ignored for
 * about 29658 CPU cycles" warm-up. A machine that wants it sets
 * `warmup_dots` (for example 29658 * 3) after calling either. */
void nes_ppu_power(NesPpu *p);
void nes_ppu_reset(NesPpu *p);

/* Advance `dots` dots. Any count gives the same result as calling it one
 * dot at a time. */
void nes_ppu_run(NesPpu *p, uint32_t dots);

/* $2000-$2007 (reg = address & 7). The caller has run the PPU to the dot
 * of the access (see the position convention above). */
uint8_t nes_ppu_read(NesPpu *p, unsigned reg);
void nes_ppu_write(NesPpu *p, unsigned reg, uint8_t value);

/* The /NMI output as a level: PPUCTRL bit 7 AND the VBlank flag. The
 * machine detects the edges, so enabling NMI while the flag is set, or
 * toggling the enable during VBlank, gives further interrupts. */
bool nes_ppu_nmi_line(const NesPpu *p);

/* Dots to run until the earlier of: the VBlank flag being set (scanline
 * 241 dot 1) and `frame_ready` being set (scanline 240 dot 0). Never more
 * than the true distance and never 0; it is one short only when an odd
 * frame's skipped dot may lie in between and rendering turns out to be
 * disabled there. */
uint32_t nes_ppu_dots_until_event(const NesPpu *p);

/* 0x00RRGGBB for each frame[] value (colour | emphasis << 6), decoded from
 * the 2C02's composite signal. Integer arithmetic only: every compiler
 * produces the same table. */
void nes_ppu_palette(uint32_t rgb[512]);

/* ---- Provided by the final program (the cartridge). ----
 *
 * `addr` is the 14-bit value on the PPU address pins. $0000-$1FFF are the
 * pattern tables, $2000-$2FFF the nametables (the cartridge chooses the
 * mirroring; the PPU has no nametable RAM of its own), and $3000-$3FFF
 * mirror $2000-$2FFF. The PPU answers $3F00-$3FFF from its palette RAM,
 * but a $2007 read there still reads the bus to refill the read buffer,
 * so nes_ppu_bus_read() can be called with $3F00-$3FFF; nes_ppu_bus_write()
 * is only called with $0000-$3EFF.
 *
 * nes_ppu_bus_address() reports a change of the address pins that comes
 * with no read or write: the v update after the second $2006 write, the
 * $2007 increment outside rendering, the return to v when rendering stops
 * (scanline 240 dot 1, or PPUMASK turning rendering off), and the idle dot
 * 0 of a rendered line, which shows the background pattern address of the
 * tile fetched next. It is not called when the address stays the same. A
 * read or write implies its own address change.
 *
 * p->scanline and p->dot are valid inside all three. During rendering
 * each fetch is reported once, at the first of its two dots (the dot the
 * address appears; hardware latches the data on the second). */
uint8_t nes_ppu_bus_read(NesPpu *p, uint16_t addr);
void nes_ppu_bus_write(NesPpu *p, uint16_t addr, uint8_t value);
void nes_ppu_bus_address(NesPpu *p, uint16_t addr);

/* One of the eight sprite output units. */
typedef struct NesPpuSprite {
    uint8_t id;       /* 0 = sprite zero, 1 = any other sprite, 64 = empty */
    uint8_t y;
    uint8_t tile;
    uint8_t attr;
    uint8_t x;        /* X counter: pixels until the sprite starts */
    uint8_t lo, hi;   /* pattern shift registers */
    uint8_t counting; /* the X counter is still running */
} NesPpuSprite;

struct NesPpu {
    /* ---- Public. ---- */

    void *user;           /* owner (machine / cartridge); never touched */
    uint64_t frame_count; /* pictures completed since power */

    /* Machine option: writes to $2000/$2001/$2005/$2006 are ignored while
     * this is non-zero; it counts down one per dot. 0 after power/reset. */
    uint32_t warmup_dots;

    uint16_t scanline; /* 0 .. lines - 1; lines - 1 is the pre-render line */
    uint16_t dot;      /* 0 .. 340 */

    /* Timing model (see NES_PPU_NTSC_*). */
    uint16_t lines;
    uint16_t vblank_line;

    /* ---- Private (nes_ppu.c), listed here because of field order. ---- */

    uint16_t v, t;     /* current and temporary VRAM address (15 bits) */
    uint16_t bus_addr; /* value on the address pins */
    uint16_t bg_lo, bg_hi, at_lo, at_hi; /* background shift registers */

    /* ---- Public. ---- */

    /* The picture: colour index (0-63) | emphasis bits (PPUMASK 5-7) << 6,
     * row y at frame + y * 256. Written as the dots go by. */
    uint16_t frame[NES_PPU_HEIGHT * NES_PPU_WIDTH];

    bool frame_ready; /* set at scanline 240 dot 0; the caller clears it */
    bool odd_frame;   /* this frame's parity; toggles at scanline 0 dot 0 */
    bool odd_skip;    /* timing model: odd frames skip a dot when rendering */

    /* ---- Private (nes_ppu.c). ---- */

    uint8_t ctrl;        /* $2000 */
    uint8_t mask;        /* $2001 */
    uint8_t status;      /* $2002 bits 7-5 */
    uint8_t fine_x;      /* x */
    uint8_t w;           /* first/second write toggle */
    uint8_t oam_addr;    /* $2003, and the sprite evaluation counter */
    uint8_t read_buffer; /* $2007 */
    uint8_t io_latch;    /* the register data bus latch (open bus) */
    uint8_t io_decay[8]; /* frames until each latch bit decays to 0 */

    uint8_t transfer_delay; /* dots until a $2006 second write reaches v */
    uint8_t read_block;     /* dots during which a $2007 read is ignored */
    bool vbl_suppress;      /* $2002 was read on the dot before the flag */

    uint8_t nt_latch, at_latch, pt_lo_latch, pt_hi_latch; /* fetched tile */

    uint8_t oam_data;          /* the OAM data latch $2004 reads in rendering */
    uint8_t eval_temp;         /* secondary OAM counter (5 bits) */
    bool eval_main_overflow;   /* all 64 sprites were looked at */
    bool eval_temp_overflow;   /* secondary OAM is full */
    uint8_t units_active;      /* some unit[] holds a sprite */
    uint8_t eval_id[8];        /* id for each secondary OAM slot */

    uint8_t palette[32];
    uint8_t soam[32];
    uint8_t oam[256];
    NesPpuSprite unit[8];

    uint8_t reserved[7]; /* keeps the size a multiple of 8 with no padding */
};

#ifdef __cplusplus
}
#endif

#endif /* NES_PPU_H */
