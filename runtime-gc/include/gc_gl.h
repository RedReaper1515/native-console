/*
 * The OpenGL path: the same pipeline as the reference rasteriser, run on
 * a graphics card instead of one pixel at a time.
 *
 * It is a *fast path*, not a second opinion. The reference rasteriser in
 * gc_gx_raster.c is what the hardware tests are held to and what defines
 * correct; this is right exactly insofar as it agrees with that - and it is
 * built to agree byte for byte: the card rasterises only each triangle's
 * bounding box, and the fragment shader (gc_gl_shader.c) runs the
 * reference's own coverage, interpolation and pixel pipeline in doubles
 * and integers. `tools/gc/compare_gl.py` (test programs, every shader
 * mode) and `gc_gl_fuzz` (random pipelines) measure the agreement;
 * docs/GC-PLAN.md "The OpenGL path" has the design and the numbers.
 *
 * The embedded framebuffer stays where it was, in main memory. This
 * renders into a framebuffer object and copies back, so everything that
 * reads the EFB - the copy, the peek path, the display conversion - is the
 * code that was already checked against hardware, unchanged. The copying
 * is not free and is not meant to be permanent, but a fast path that is
 * wrong is worth nothing, and this way only one thing is new at a time.
 *
 * Built only when SDL2 is present. Without it every function here is a
 * stub that reports the path unavailable, and the runtime falls back to
 * the reference rasteriser - which is the correct outcome, not a
 * degradation.
 */
#ifndef GC_GL_H
#define GC_GL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcMachine GcMachine;
typedef struct GcGxPoint GcGxPoint;

/* How to run it. Any field left null falls back to an environment
 * variable, and then to the default:
 *
 *   shaders    GC_GL_SHADERS  "hybrid" (default), "uber" or "specialised"
 *   cache_dir  GC_GL_CACHE    where compiled programs and the list of
 *                             pipelines a run used are kept; "off" (in
 *                             either place) or unset keeps nothing
 *   id         GC_GL_ID       names the pipeline list, <id>.pipelines
 *   fast       GC_GL_FAST     nonzero (or the variable set to anything but
 *                             "0"): the fast path, below
 *   anisotropy GC_GL_ANISOTROPY 2 to 16: the fast path lets the card
 *                             filter a slanted texture along its longer
 *                             axis with that many samples; 0 or 1 filters as
 *                             the reference does
 *   scale      GC_GL_SCALE    2 to 4: the fast path draws at that many
 *                             times the console's own size each way; 0 or
 *                             1, or the exact path, draws at the console's
 *
 * The fast path gives up the agreement everything above is built for, in
 * exchange for speed: the card rasterises each triangle, interpolates
 * across it and blends it itself, instead of the shader asking the
 * reference's question of every pixel in doubles and reading the
 * framebuffer back to blend. The combiner, the texture unit, fog and the
 * alpha test are still the reference's arithmetic. What can differ is
 * which pixels an edge covers, the last bits of depth and of an
 * interpolated coordinate, a blended pixel by a level, the dither and
 * six-bit rounding it does not do - and so anything a game reads back from
 * the framebuffer. It is a choice a player makes, never the default, and
 * the exact path stays the one the tests hold to the reference.
 * docs/GC-PLAN.md, "The fast path", has the numbers.
 *
 * At a scale the fast path's framebuffer is larger than the console's, and
 * everything that reads it - a copy to a texture or to the display buffer,
 * a peek, a save state - reads it brought down to the console's size, with
 * the colours of each block averaged (scales 2 and 4). The game is none the
 * wiser, and the picture it copies to the display is supersampled.
 */
typedef struct GcGlOptions {
  const char *shaders;
  const char *cache_dir;
  const char *id;
  int fast;
  int scale;
  int anisotropy;
} GcGlOptions;

/* Bring up a context and a framebuffer object. Returns 0, or -1 with a
 * reason on the machine's log. Needs OpenGL 4.1 core: the shader computes
 * in doubles, which is 4.0, and keeps program binaries, which is 4.1. */
int gc_gl_start(GcMachine *m);
int gc_gl_start_with(GcMachine *m, const GcGlOptions *options);
/* Draws what is pending, prints the path's statistics to the machine's
 * log and releases everything. gc_machine_destroy calls it. */
void gc_gl_stop(GcMachine *m);

/* Whether triangles should go to the card. */
int gc_gl_active(const GcMachine *m);

/* One triangle, already clipped, in screen coordinates. */
/* Three points. Written as a pointer rather than as an array of three,
 * because this header only forward-declares the point type, and an array
 * parameter of an incomplete type is an error in standard C - which MSVC
 * lets through and clang does not. The two spellings mean the same thing. */
void gc_gl_triangle(GcMachine *m, const GcGxPoint *points);

/* Triangles the rasteriser held back and set up (GcGxRasterPiece in
 * gc_gx_draw.h), in drawing order, all under the registers as they stand.
 * The pieces and their points are read only during the call. */
struct GcGxRasterPiece;
void gc_gl_pieces(GcMachine *m, const struct GcGxRasterPiece *const *pieces, unsigned count);

/* An EFB copy (gc_efb_copy's trigger), made from the card's framebuffer.
 * Returns 0 when the path is not active and the caller makes the copy. The
 * bytes may reach main memory later, from another thread; until then the
 * pages they go to are held (gc_memory.h), so nothing can tell. */
int gc_gl_copy(GcMachine *m, uint32_t trigger);

/* The same for a copy already prepared (gc_efb_copy_prepare) whose pages
 * [first, end) are already held under `tag` (0: none held): the graphics
 * thread's copies (gc_gx_thread.h). The hold is released when the bytes have
 * landed. Returns 0, having done nothing, when the path is not active. */
struct GcEfbCopyJob;
int gc_gl_copy_prepared(GcMachine *m, const struct GcEfbCopyJob *job, uint64_t tag, uint32_t first, uint32_t end);

/* Every copy the card still owes main memory, written. */
void gc_gl_settle(GcMachine *m);

/* Everything drawn so far, back into the framebuffer in main memory.
 * Anything that reads the EFB calls this first; it does nothing when
 * nothing has been drawn since the last time. */
void gc_gl_sync_to_memory(GcMachine *m);

/* One pixel of one plane in the framebuffer in main memory as the card has
 * it, for a peek: the rest of memory's framebuffer may stay out of date. */
void gc_gl_fetch_pixel(GcMachine *m, uint32_t x, uint32_t y, int depth);
/* The fast path's other answer to a peek, tried first and from the
 * machine's own thread with nothing waited for: the pixel as it was when
 * the frame before last was shown. A game that peeks does it every frame -
 * what is under the pointer, whether the sun is covered - and each exact
 * answer stops the machine until everything it has asked for is drawn,
 * which is the whole point of drawing on other threads undone a dozen times
 * a field. Returns 1 with the pixel put in the machine's plane, or 0 when
 * the exact path has to answer (not the fast path, the first frames, or
 * GC_GL_LATE_PEEKS=0). Which frame answers depends only on what the machine
 * has issued, so a run repeats. */
int gc_gl_peek_late(GcMachine *m, uint32_t x, uint32_t y, int depth);

/* The other direction: something wrote the framebuffer in main memory, so
 * whatever the card holds is stale. */
void gc_gl_invalidate(GcMachine *m);

/* The picture on screen as the fast path drew it at its scale: larger than
 * the display buffer, which holds it brought down to the console's size.
 * There is one when the buffer the video interface shows was filled by a
 * display copy this path made at a scale above 1.
 *
 * Returns 0 with the picture's size and a serial that changes when the
 * picture does. With `out` null that is all; otherwise the pixels are
 * written too, `pitch` pixels a row, when they fit `pitch` by `rows`.
 * Returns -1 when there is no such picture (show gc_efb_frame_rgba's) or it
 * does not fit. */
int gc_gl_display_picture(GcMachine *m, uint32_t *out, unsigned pitch, unsigned rows, unsigned *width,
                          unsigned *height, uint64_t *serial);
/* The same for a buffer the caller names (gc_efb_display_place's answer,
 * asked earlier): nothing of the machine is read, only the pictures the
 * render thread keeps, so this may be called while the machine runs. */
int gc_gl_picture_at(GcMachine *m, uint32_t first, uint32_t display_width, uint32_t display_height, uint32_t *out,
                     unsigned pitch, unsigned rows, unsigned *width, unsigned *height, uint64_t *serial);
/* Main memory was replaced from outside (a state loaded): the pictures
 * kept are of display buffers that are no longer what they were. */
void gc_gl_forget_pictures(GcMachine *m);
/* That picture as a binary PPM, once every copy on its way has landed.
 * Returns 0, or -1 when there is none or the file cannot be written. */
int gc_gl_write_picture(GcMachine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* GC_GL_H */
