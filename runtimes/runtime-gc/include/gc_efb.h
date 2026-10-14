/*
 * The embedded framebuffer, and the copy that gets pixels out of it.
 *
 * The EFB is 640x528 of colour and depth on the graphics chip itself. It is
 * not main memory and it is not laid out like memory: the CPU reaches it
 * through an address window where the pixel's coordinates *are* the
 * address - x at bits 2 to 11, y at bits 12 to 21, and bit 22 choosing
 * colour or depth. So this models it as two planes of pixels and decodes
 * the address, rather than as a block of bytes.
 *
 * It holds fewer bits than it appears to. The pixel format register says
 * how many, and a value written to it comes back rounded to that - eight
 * bits a channel with no alpha, six with alpha, or five-six-five. That
 * rounding is applied on the way in, because that is where the hardware
 * loses the bits.
 *
 * Nothing reads the EFB directly except a peek. Everything else goes out
 * through a *copy*: a rectangle of it is filtered down three rows at a
 * time, gamma corrected, optionally converted to luma and chroma, tiled,
 * and written into main memory as a texture. That path is
 * `gc_efb_copy`, and the order of its steps is load-bearing.
 */
#ifndef GC_EFB_H
#define GC_EFB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcMachine GcMachine;

#define GC_EFB_WIDTH 640u
#define GC_EFB_HEIGHT 528u

/* The window the CPU reaches the EFB through, in physical addresses. */
#define GC_EFB_WINDOW 0x00800000u /* two planes of 1024 x 1024 words */

/* The pixel formats the EFB can be in, as the pixel format register (BP
 * 0x43, bits 2 to 0) encodes them. */
enum {
  GC_EFB_RGB8_Z24 = 0,
  GC_EFB_RGBA6_Z24 = 1,
  GC_EFB_RGB565_Z16 = 2,
  GC_EFB_Z24 = 3,
  GC_EFB_Y8 = 4,
  GC_EFB_U8 = 5
};

typedef struct GcEfb {
  /* Colour as 0xAARRGGBB, already rounded to what the format holds, and
   * depth as 24 bits. */
  uint32_t *color;
  uint32_t *depth;
  uint64_t copies;
  unsigned unsupported_copies;

  /* The last thing copied out for display: where it went and how big it
   * was. The frame reader uses it only when the video interface's own
   * registers name no buffer. */
  uint32_t display_address;
  uint32_t display_pitch;
  uint32_t display_width;
  uint32_t display_height;
} GcEfb;

int gc_efb_create(GcEfb *efb);
void gc_efb_destroy(GcEfb *efb);
void gc_efb_clear(GcEfb *efb);

/* The address window. Returns non-zero when the access was inside it. */
int gc_efb_read(GcMachine *m, uint32_t physical, unsigned size, uint32_t *value);
int gc_efb_write(GcMachine *m, uint32_t physical, unsigned size, uint32_t value);

/* One pixel, for the rasteriser. Writing rounds the colour to what the
 * current pixel format actually holds. */
void gc_efb_set_color(GcMachine *m, uint32_t x, uint32_t y, uint32_t argb);
uint32_t gc_efb_get_color(const GcMachine *m, uint32_t x, uint32_t y);

/* A rectangle of the EFB into main memory, as the copy trigger register
 * describes it. */
void gc_efb_copy(GcMachine *m, uint32_t trigger);

/* The copy, taken apart so that it can read other planes and write another
 * view of main memory (the OpenGL path makes it from pixels read back from
 * the card, on another thread). */
typedef struct GcEfbCopyFilter {
  uint32_t previous;
  uint32_t current;
  uint32_t next;
} GcEfbCopyFilter;

typedef struct GcEfbCopyJob {
  /* From the registers, at the trigger. */
  uint32_t trigger;
  /* `rows` is how many of the rectangle's rows exist in the framebuffer;
   * `out_rows` how many the copy writes - half of them when it is halved,
   * as `out_width` is half the width. */
  uint32_t left, top, width, height, out_width, rows, out_rows;
  uint32_t destination, format, tiles_per_row, display_pitch;
  unsigned gamma, efb;
  int to_display, convert, half_scale;
  GcEfbCopyFilter weights;
  int writes; /* 0 for a format nothing here writes */
  int clears;
  uint32_t clear_colour, clear_depth; /* as the framebuffer stores them */
  /* Where it reads (planes GC_EFB_WIDTH wide; only the rectangle's rows and
   * the row either side of it are read) and the main memory it writes. */
  const uint32_t *color, *depth;
  uint8_t *ram;
  /* The rows' split across helpers. */
  uint32_t chunk_rows;
  long chunks, next_chunk;
  /* The fast OpenGL path, on a display copy: the whole framebuffer is kept
   * as well, for peeks answered a frame late (gc_gl.h, gc_gl_peek_late) -
   * bit 0 its colour, bit 1 its depth. Set by the machine's thread when it
   * issues the copy, so that what a later peek waits for is settled then. */
  uint32_t snapshot;
} GcEfbCopyJob;

/* Counts the copy, records a display copy, and fills in everything but the
 * planes and memory. Returns 0 when nothing is written (and says so on the
 * log, as gc_efb_copy does). */
int gc_efb_copy_prepare(GcMachine *m, uint32_t trigger, GcEfbCopyJob *job);
/* The physical bytes [first, end) the copy can write. */
void gc_efb_copy_bytes(const GcEfbCopyJob *job, uint32_t *first, uint32_t *end);
/* The texture format a copy's bytes are read back in when a game draws with
 * them as they are - the one whose texels are exactly what the copy stored:
 * eight bits as I8, two channels as IA8, and so on - or -1 for a copy that
 * is not a texture (a display copy, one that writes nothing). Whether the
 * filter repeats the rectangle's first and last rows comes back too. The
 * fast OpenGL path makes such a texture on the card from the framebuffer
 * (gc_gl_render.c), and uses it where a draw names the copy's address,
 * size and this format. */
int gc_efb_copy_texture_format(const GcEfbCopyJob *job, int *clamp_top, int *clamp_bottom);
/* The rows, on the helpers too when allowed and the rows cannot collide. */
void gc_efb_copy_run(GcEfbCopyJob *job, int helpers_allowed);
/* Whether each row writes bytes no other row writes and reads only the
 * planes, so rows may run in any order (then gc_efb_copy_run_rows on any
 * range, and gc_efb_copy_row_bytes says which bytes a row can write). */
int gc_efb_copy_rows_independent(const GcEfbCopyJob *job);
void gc_efb_copy_row_bytes(const GcEfbCopyJob *job, uint32_t y, uint32_t *first, uint32_t *end);
void gc_efb_copy_run_rows(GcEfbCopyJob *job, uint32_t first, uint32_t end);

/* Whether the buffer on screen is in main memory - 0 while a copy made on
 * the OpenGL path, or by the graphics thread, is still on its way to it
 * (reading it then waits). Judged by the bytes the copy writes, not its
 * pages. */
int gc_efb_display_ready(GcMachine *m);
/* Where the buffer on screen starts in main memory, and how much of it is
 * shown: pixels across and frame lines down. Nonzero when there is none. */
int gc_efb_display_place(GcMachine *m, uint32_t *first, uint32_t *width, uint32_t *height);
/* Wait until it is, for at most `timeout_ms` (negative: as long as it
 * takes); returns whether it is. The copy is made on another thread, which
 * needs nothing from the caller to finish it. */
int gc_efb_display_wait(GcMachine *m, double timeout_ms);

/* The displayed frame as 32-bit pixels for a host window: the buffer the
 * video interface is scanning out, or the last display copy when it names
 * none. `out` holds `rows` rows of `pitch` pixels, and a bigger frame is
 * cut to fit. Reports the frame's full size even when there is nothing to
 * copy. */
int gc_efb_frame_rgba(GcMachine *m, uint32_t *out, unsigned pitch, unsigned rows,
                      unsigned *width_out, unsigned *height_out);

/* The same frame, decoded back to red, green and blue and written as a
 * plain portable pixmap. Returns 0, or -1 when there is nothing to write or
 * the file will not open. */
int gc_efb_write_frame(GcMachine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* GC_EFB_H */
