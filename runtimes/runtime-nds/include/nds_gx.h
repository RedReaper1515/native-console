/*
 * The DS 3D engine: the geometry engine (nds_gx.c) and the rendering engine,
 * a software scanline rasteriser (nds_gx_raster.c).
 *
 * The machine owns the buses, the scheduler and video memory. It routes the
 * ARM9's accesses to this module's registers here, advances the geometry
 * engine with nds_gx_run(), and asks for a frame at the points of the video
 * frame where the hardware swaps and starts rendering.
 *
 * Call order within a frame:
 *
 *   any time       nds_gx_run() to bring the engine to the present, then
 *                  nds_gx_read() / nds_gx_write(). Neither of those two
 *                  advances time.
 *   line 192, dot 0   nds_gx_vblank(): a waiting SWAP_BUFFERS hands the
 *                  polygons to the rendering engine, and the rendering
 *                  registers are latched for the frame.
 *   line 215       nds_gx_render() with texture memory as it is mapped then.
 *   lines 0-191 of the next frame   nds_gx_line() for the 2D engine.
 *
 * NdsGx is plain data: no pointers, nothing outside it. It can be copied,
 * saved and restored with memcpy.
 */
#ifndef NDS_GX_H
#define NDS_GX_H

#include "nds_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NDS_GX_WIDTH 256
#define NDS_GX_HEIGHT 192

/* Polygon list RAM and vertex RAM, per bank (GBATEK "DS 3D Status",
 * RAM_COUNT). */
#define NDS_GX_MAX_POLYGONS 2048
#define NDS_GX_MAX_VERTICES 6144
/* A quad clipped by all six planes of the view volume. */
#define NDS_GX_POLYGON_VERTICES 10

/* The command FIFO has 256 entries and feeds a 4-entry PIPE (GBATEK "DS 3D
 * Geometry Commands"). Behind them the module keeps what was written while
 * the FIFO was full, which on hardware is the ARM9 frozen in mid-write; see
 * nds_gx_fifo_full(). */
#define NDS_GX_FIFO_ENTRIES 256
#define NDS_GX_PIPE_ENTRIES 4
#define NDS_GX_QUEUE_ENTRIES 1024

/* Texture memory as the machine has it mapped right now. NULL = nothing
 * mapped in that slot (reads as zero). */
typedef struct NdsGxVram {
    const uint8_t *texture[4]; /* texture image slots 0-3, 128 KiB each */
    const uint8_t *palette[6]; /* texture palette slots 0-5, 16 KiB each */
} NdsGxVram;

/* One entry of vertex RAM. */
typedef struct NdsGxVertex {
    int32_t pos[4];   /* clip coordinates x, y, z, w; w is cut to 24 bits once the vertex is stored */
    int16_t tex[2];   /* S, T: 1 sign bit, 11 integer, 4 fraction */
    int16_t sx, sy;   /* screen position: x 0..511, y 0..255 */
    uint8_t color[3]; /* red, green, blue, 5 bits each */
    uint8_t clipped;  /* made by the clipper, so not shared along a strip */
    /* What the division that gave sx and sy threw away, in 256ths of a
     * pixel. The console's own picture never uses it; a picture drawn larger
     * than the screen does (nds_gx_render_scaled). */
    uint8_t fx, fy;
} NdsGxVertex;

#define NDS_GX_POLY_FRONT 0x01u       /* faces the viewer (or is a line): decides which way its edges are walked */
#define NDS_GX_POLY_TRANSLUCENT 0x02u /* goes in the translucent list */
#define NDS_GX_POLY_W_BUFFER 0x04u    /* its depths are W values */
#define NDS_GX_POLY_SHADOW_MASK 0x08u /* polygon mode 3, ID 0 */
#define NDS_GX_POLY_SHADOW 0x10u      /* polygon mode 3, another ID */
#define NDS_GX_POLY_DEGENERATE 0x20u  /* a vertex with W = 0, or below the screen: never drawn */

/* One entry of polygon list RAM. */
typedef struct NdsGxPolygon {
    uint32_t attr;                           /* POLYGON_ATTR as BEGIN_VTXS latched it */
    uint32_t tex_param;                      /* TEXIMAGE_PARAM */
    uint32_t depth[NDS_GX_POLYGON_VERTICES]; /* per vertex: the 24-bit depth, from Z or from W */
    uint16_t w[NDS_GX_POLYGON_VERTICES];     /* per vertex: W normalised to 16 bits for this polygon */
    uint16_t vertex[NDS_GX_POLYGON_VERTICES];/* indexes into the bank's vertex RAM */
    uint16_t palette;                        /* PLTT_BASE */
    uint8_t count;                           /* vertices: 3 or 4, up to 10 after clipping */
    uint8_t flags;                           /* NDS_GX_POLY_* */
    uint8_t top, bottom;                     /* which vertex is topmost, bottommost */
    uint8_t y_top, y_bottom;                 /* their screen lines */
} NdsGxPolygon;

typedef struct NdsGx {
    /* ---- Registers ---- */
    uint16_t disp3dcnt;        /* DISP3DCNT, with its two sticky flags */
    uint8_t render_io[0xA0];   /* 0x04000320-0x040003BF as written: edge colours, alpha test, clear, fog, toon */
    uint8_t irq_mode;          /* GXSTAT bits 30-31 */
    uint8_t stack_error;       /* GXSTAT bit 15 */
    uint8_t box_result;        /* GXSTAT bit 1 */
    uint16_t one_dot_depth;    /* DISP_1DOT_DEPTH */
    int32_t pos_result[4];     /* POS_RESULT */
    int16_t vec_result[3];     /* VEC_RESULT */

    /* ---- Command queue: PIPE, then FIFO, then what did not fit ---- */
    uint32_t queue_param[NDS_GX_QUEUE_ENTRIES];
    uint8_t queue_cmd[NDS_GX_QUEUE_ENTRIES];
    uint16_t queue_head;       /* oldest entry */
    uint16_t queue_count;      /* entries in the ring: pipe + fifo + beyond */
    uint16_t pipe_count;       /* of them, in the PIPE (0-4) */
    uint16_t fifo_count;       /* of them, in the FIFO (0-256) */
    uint16_t queued_tests;     /* BOX/POS/VEC_TEST entries in the ring */
    uint16_t queued_stack;     /* MTX_PUSH/MTX_POP entries in the ring */
    uint32_t dropped;          /* entries lost because the ring itself overflowed (a machine that ignores fifo_full) */
    /* a GXFIFO word being unpacked */
    uint32_t packed;           /* the command bytes not yet done, lowest first */
    uint8_t packed_left;       /* how many of them; 0 = the next word is a command word */
    uint8_t packed_need;       /* parameters the current command takes */
    uint8_t packed_got;        /* parameters it has */

    /* ---- Execution ---- */
    uint32_t busy;             /* cycles until the engine can take the next entry */
    uint32_t exec_param[32];   /* parameters of the command being collected */
    uint8_t exec_cmd;
    uint8_t exec_count;
    uint8_t exec_test;         /* the command in progress is a test (GXSTAT bit 0) */
    uint8_t exec_stack;        /* the command in progress is a push or pop (GXSTAT bit 14) */
    uint8_t swap_wait;         /* SWAP_BUFFERS executed: halted until VBlank */
    uint8_t swap_param;        /* its parameter */
    uint8_t swap_mode;         /* the previous swap's parameter: in force for the polygons being defined */

    /* ---- Matrices (20.12 fixed point, row vectors: v' = v * M) ---- */
    uint8_t matrix_mode;
    uint8_t projection_sp;     /* 0-1 */
    uint8_t position_sp;       /* 0-63; shared by the position and vector stacks */
    uint8_t texture_sp;        /* 0-1 */
    int32_t projection[16];
    int32_t position[16];
    int32_t vector[16];
    int32_t texture[16];
    int32_t clip[16];          /* position * projection */
    int32_t projection_stack[16];
    int32_t position_stack[32][16];
    int32_t vector_stack[32][16];
    int32_t texture_stack[16];

    /* ---- Vertex and polygon state ---- */
    uint32_t polygon_attr;     /* POLYGON_ATTR as written */
    uint32_t polygon_attr_latched; /* as of the last BEGIN_VTXS */
    uint32_t tex_param;
    uint16_t tex_palette;
    int16_t vertex[3];         /* the last vertex's x, y, z (4.12) */
    uint8_t color[3];          /* current vertex colour, 5 bits each */
    int16_t tex_raw[2];        /* TEXCOORD as written */
    int16_t tex_coord[2];      /* after the texture matrix */
    int16_t normal[3];         /* NORMAL as written (10 bits signed) */
    /* lighting */
    int16_t light_dir[4][3];   /* towards the light, view space, 11 bits signed */
    int32_t light_recip[4];    /* specular reciprocal */
    uint8_t light_color[4][3];
    uint8_t diffuse[3], ambient[3], specular[3], emission[3];
    uint8_t shininess_enable;
    uint8_t shininess[128];
    /* viewport, as the vertex transform uses it */
    uint16_t viewport_x, viewport_width;   /* left column; width 0-511 */
    uint8_t viewport_y, viewport_height;   /* top line (from the top of the screen); height 0-255 */
    /* primitive assembly */
    uint8_t prim_type;         /* BEGIN_VTXS parameter */
    uint8_t prim_count;        /* vertices waiting in prim */
    uint8_t strip_odd;         /* triangle strip: the next triangle is an odd one */
    uint8_t strip_linked;      /* the last strip polygon is in RAM unclipped: its last two vertices can be shared */
    uint16_t strip_vertex[2];  /* where those two are in vertex RAM, in the next polygon's order */
    NdsGxVertex prim[4];

    /* ---- Polygon list RAM and vertex RAM: two banks ---- */
    uint8_t geometry_bank;     /* the bank being filled; the other one is being rendered */
    uint16_t polygon_count;    /* in the geometry bank */
    uint16_t vertex_count;
    uint16_t render_count;     /* polygons in the rendering bank */
    uint16_t render_order[NDS_GX_MAX_POLYGONS]; /* the order they are drawn in */
    NdsGxPolygon polygons[2][NDS_GX_MAX_POLYGONS];
    NdsGxVertex vertices[2][NDS_GX_MAX_VERTICES];

    /* ---- Rendering engine ---- */
    uint16_t latched_disp3dcnt;   /* the rendering registers as of the last VBlank */
    uint8_t latched_io[0xA0];
    uint32_t frame[NDS_GX_HEIGHT][NDS_GX_WIDTH]; /* the last rendered frame, in nds_gx_line()'s format */
} NdsGx;

void nds_gx_reset(NdsGx *gx);

/* ARM9 I/O. addr is the full address; size is 1, 2 or 4; any size at any
 * offset. The module owns 0x04000060-0x04000063 (DISP3DCNT),
 * 0x04000320-0x040003BF (the rendering engine's registers) and
 * 0x04000400-0x040006A3 (GXFIFO, the command ports, GXSTAT, RAM_COUNT,
 * DISP_1DOT_DEPTH and the result registers). GXFIFO and the command ports
 * take 32-bit writes only; narrower ones do nothing. Write-only registers
 * read as zero. */
uint32_t nds_gx_read(NdsGx *gx, uint32_t addr, int size);
void nds_gx_write(NdsGx *gx, uint32_t addr, uint32_t value, int size);

/* Advance the geometry engine by system cycles (33.51 MHz). Commands take
 * the times GBATEK lists. Any count is fine: every command that fits in it
 * is executed. */
void nds_gx_run(NdsGx *gx, uint32_t cycles);
/* How many cycles until the engine finishes what it is doing, which is when
 * its observable state (IRQ line, FIFO level, busy flags) can next change by
 * itself; 0 if nothing will change until the next write or VBlank (idle, or
 * halted by SWAP_BUFFERS). Ask again after every write, run and VBlank.
 *
 * A machine waiting for the FIFO to reach a level need not wake for every
 * entry. The engine takes one entry at a time, never in less than a cycle,
 * and each one it takes moves at most two from the FIFO to the PIPE, so a
 * FIFO that holds L entries now (GXSTAT bits 16-24) cannot hold fewer than
 * N until this many cycles plus L - N - 1 have passed. */
uint32_t nds_gx_cycles_until_event(const NdsGx *gx);

bool nds_gx_irq(const NdsGx *gx);         /* the GXFIFO interrupt request, as GXSTAT bits 30-31 select; a level */
bool nds_gx_dma_request(const NdsGx *gx); /* FIFO less than half full: a geometry-FIFO DMA may send */
/* The FIFO has no room: the machine stalls the ARM9 (and its DMA) until this
 * is false again, running the engine meanwhile. A write made anyway is not
 * lost: it is kept behind the FIFO, and this stays true until that drains. */
bool nds_gx_fifo_full(const NdsGx *gx);

void nds_gx_vblank(NdsGx *gx); /* start of line 192: a pending SWAP_BUFFERS takes effect */
/* Render the frame from the polygon lists the last swap produced (the
 * machine calls this at line 215, where the hardware starts rendering). */
void nds_gx_render(NdsGx *gx, const NdsGxVram *vram);
/* One rendered line, 256 pixels: bits 0-5 red, 6-11 green, 12-17 blue,
 * 18-22 alpha (0-31; 0 = nothing there). */
const uint32_t *nds_gx_line(const NdsGx *gx, int y);

/* The largest whole number of times the screen's size a picture can be drawn
 * at. */
#define NDS_GX_SCALE_MAX 4
/*
 * The same frame nds_gx_render() would draw, drawn `scale` times as large
 * each way (1 to NDS_GX_SCALE_MAX): 256 * scale pixels a line, 192 * scale
 * lines, into `frame` in nds_gx_line()'s format. Nothing in `gx` changes, so
 * the machine cannot tell whether this was called.
 *
 * It is the same rasteriser with the same rules, on a finer grid: vertices
 * sit where the viewport division really put them (NdsGxVertex.fx, fy)
 * instead of at the top left of their pixel, edges are walked and attributes
 * weighted with more fraction bits than the console has, and the rear
 * plane's bitmap, which has one pixel a screen pixel, is repeated. At a
 * scale of 1 it is not the console's picture to the bit (the console's own
 * rounding is in nds_gx_render() alone); it is for a display that wants more
 * pixels than the console had.
 *
 * Returns false, with nothing drawn, when it could not get the memory.
 */
bool nds_gx_render_scaled(const NdsGx *gx, const NdsGxVram *vram, int scale, uint32_t *frame);
/* nds_gx_render() and nds_gx_render_scaled() of the same frame, at once: the
 * calling thread draws the console's picture while the helper threads have
 * begun the larger one, and then joins them. The two pictures are the ones
 * the two calls give. Returns what nds_gx_render_scaled() would; the
 * console's picture is drawn either way. */
bool nds_gx_render_both(NdsGx *gx, const NdsGxVram *vram, int scale, uint32_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* NDS_GX_H */
