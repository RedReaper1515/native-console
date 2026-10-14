/*
 * The graphics thread: the command stream's drawing, off the machine's
 * thread, with nothing the machine can observe moved by an instruction.
 *
 * Private to the runtime. docs/GC-PLAN.md, "The graphics thread", has the
 * design and the measurements; in short:
 *
 * The machine's thread still reads the FIFO as the processor writes it
 * (gc_gx.c, scan_stream). It walks every command - into display lists too -
 * far enough to know its length, and does there, at once, everything the
 * processor can see: the read pointer moves over whole commands exactly as
 * before, the vertex-format and blitting registers are stored, the finish
 * and token interrupts are raised, the counters count, an EFB copy is
 * prepared (the copy counter, the display buffer's address) and the pages it
 * will write are held. The commands themselves - with display lists copied
 * out of memory, matrices loaded out of arrays turned into plain register
 * writes and palettes copied - go on a queue. The graphics thread runs them
 * on a machine of its own (the *shadow*: the register files, the
 * framebuffer's planes, a copy of main memory of its own, the OpenGL
 * path), decoding, transforming, setting up and drawing exactly as the
 * single-threaded path does.
 *
 * The graphics thread reads vertex arrays and textures from a copy of main
 * memory of its own, into which the queue carries, ahead of each draw, the
 * bytes the draw reads as they are when it is issued - whole pages only when
 * they have been written since they were last sent (gc_memory.h, tracking).
 * The machine never waits for the graphics thread to read memory, and a
 * write to memory never waits for it either.
 *
 * One kind of byte the machine cannot hand over: a texture that is an EFB
 * copy still on its way. A game draws something, copies it to a texture and
 * draws with that texture at once; the copy is made by the graphics side,
 * later, and the machine reading it to send it would wait for the whole
 * pipeline to catch up - five times a field in Super Mario Galaxy 2, which
 * was half its time. So for a texture that lies wholly inside a pending
 * copy the machine sends a note instead (gc_gx_thread_send_texture), and the
 * graphics thread, which made the copy, takes the bytes itself when they
 * land: whoever finishes a copy hands its bytes over before the pages are
 * released (gc_gx_thread_copy_landing), so they are the copy's and not
 * whatever the machine wrote next. On the fast OpenGL path the bytes are
 * often not wanted at all: the card made a texture of the copy when it read
 * the framebuffer, and a draw that names the copy's address, size and format
 * uses that (gc_gx_thread_copy_texture) - also after the place has been used
 * for something else, when the bytes the copy wrote are there again.
 *
 * What the graphics thread computes that the
 * machine keeps - the transform registers, the combiner's registers, the
 * palettes, the framebuffer - is brought back only when something outside
 * the stream asks: a peek or poke at the framebuffer, a state or a hash, a
 * differential comparison, the OpenGL path started or stopped. Each of those
 * waits for the queue to drain (gc_gx_thread_sync).
 *
 * GC_GX_THREAD=0 in the environment keeps the single-threaded path. The
 * thread needs main memory's second view (Windows), and is not used for a
 * machine whose tests watch vertices (on_primitive, on_triangle).
 */
#ifndef GC_GX_THREAD_H
#define GC_GX_THREAD_H

#include <stddef.h>
#include <stdint.h>

#include "gc.h"
#include "gc_efb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcGxThread GcGxThread;

/* Start the thread for this machine, unless GC_GX_THREAD=0 or the host
 * cannot. Returns 1 when it runs, 0 when the machine stays single-threaded. */
int gc_gx_thread_start(GcMachine *m);
/* Everything finished and brought back, and the thread gone. */
void gc_gx_thread_stop(GcMachine *m);

/* The machine's thread, or NULL. */
GcGxThread *gc_gx_thread_of(const GcMachine *m);
/* Whether the calling thread is a graphics thread. */
int gc_gx_thread_on_thread(void);

/* Wait until the graphics thread has run everything queued so far, and
 * copy what it computed back into the machine. From the machine's thread. */
void gc_gx_thread_sync(GcMachine *m);

/* For an entry point outside the command stream that works on the graphics
 * side of a machine (the OpenGL path's settle, sync, invalidate and stop, the
 * framebuffer window): the machine itself when it has no thread or this is
 * the graphics thread; otherwise, once the queue has drained, the shadow the
 * graphics thread draws on - which stays still until the machine issues more
 * commands. */
GcMachine *gc_gx_thread_target(GcMachine *m);
/* The same machine without waiting for the queue, for a caller that reads
 * only what is safe beside both threads while they run (the pictures the
 * render thread keeps under their own lock). */
GcMachine *gc_gx_thread_drawn_on(GcMachine *m);

/* The machine's graphics state is about to be replaced wholesale (a state
 * loaded, a reset): everything queued finished, every copy landed, every
 * page untracked. Then gc_gx_thread_reload once it has been, which hands the
 * new state to the shadow - and must not bring the old one back over it. */
void gc_gx_thread_quiesce(GcMachine *m);
void gc_gx_thread_reload(GcMachine *m);

/* The OpenGL path was started or stopped on the machine. */
void gc_gx_thread_gl_changed(GcMachine *m);

/* ------------------------------------------------------------------ */
/* The queue, as gc_gx.c's scan fills it (the machine's thread)       */

/* Around what one gc_gx_run finds: begin notes the processor has run (what
 * was sent may have changed) and the field and SSE modes the commands run in;
 * end hands everything over. */
void gc_gx_thread_begin_run(GcGxThread *t, GcMachine *m);
void gc_gx_thread_end_run(GcGxThread *t);
/* Room for `size` bytes of commands, appended to the burst. */
uint8_t *gc_gx_thread_commands(GcGxThread *t, GcMachine *m, size_t size);
/* An EFB copy prepared on the machine (`job` NULL when it writes nothing),
 * its pages already held under `tag`. Handed over at once. */
void gc_gx_thread_copy(GcGxThread *t, GcMachine *m, uint32_t trigger, const GcEfbCopyJob *job, uint64_t tag,
                       uint32_t first, uint32_t end);
/* A palette load (blitting register 0x65) and the entries it copied. */
void gc_gx_thread_palette(GcGxThread *t, GcMachine *m, uint32_t payload, const uint16_t *entries, unsigned count);
/* Main memory [first, first + size) as it is now, which the next command
 * reads, into the graphics thread's copy of memory before that command:
 * the pieces of pages the range shares with anything else every time, and
 * its whole pages only when they have been written since they were last
 * sent (gc_memory.h, tracking). */
void gc_gx_thread_send(GcGxThread *t, GcMachine *m, uint32_t first, uint32_t size);
/* The same for a texture's bytes - except that when all of them belong to
 * a copy that has not landed, nothing is read: the graphics thread is told
 * which copy, and gets the bytes from it. */
void gc_gx_thread_send_texture(GcGxThread *t, GcMachine *m, uint32_t first, uint32_t size);
/* Blitting registers were written: the textures the next draw reads are
 * worked out again. */
void gc_gx_thread_textures_changed(GcGxThread *t);
/* Whether the next draw's textures still need working out (and then they
 * are taken as worked out). */
int gc_gx_thread_textures_due(GcGxThread *t);

/* The graphics thread runs commands queued as bytes (gc_gx.c). */
void gc_gx_exec_commands(GcMachine *shadow, const uint8_t *bytes, size_t size);

/* ------------------------------------------------------------------ */
/* Peeks answered late (the fast OpenGL path; gc_gl.h)                 */

/* The machine's thread wants to peek at a plane (0 colour, 1 depth) without
 * waiting for everything queued to be drawn. Returns the tag of the display
 * copy before the last one issued, when that copy was issued with a wish for
 * this plane's picture (GcEfbCopyJob.snapshot) - the peek is answered from
 * it - and 0 when it was not, which is how things start: the wish is noted,
 * and display copies issued from now on carry it for as long as peeks keep
 * coming. Which copy answers a peek depends only on what the machine has
 * issued, never on how far the drawing has got. */
uint64_t gc_gx_thread_peek_from(GcGxThread *t, int plane);
/* Late peeks are over (the OpenGL path stopped or changed): nothing issued
 * so far answers one. */
void gc_gx_thread_peeks_forget(GcMachine *m);

/* ------------------------------------------------------------------ */
/* Copies on their way                                                */

/* From whichever thread finishes the copy held under `tag`, before it
 * releases the pages: bytes [first, end) of main memory, read through
 * `memory` (either view's start), are the copy's now. In pieces when the
 * pages are released in pieces; then gc_gx_thread_copy_landed once all of
 * them have been handed over. Nothing happens for a tag no graphics thread
 * is waiting on. */
void gc_gx_thread_copy_landing(uint64_t tag, const uint8_t *memory, uint32_t first, uint32_t end);
void gc_gx_thread_copy_landed(uint64_t tag);

/* The rest are the OpenGL path's, on the graphics side of `shadow` (the
 * machine the graphics thread draws on; nothing happens for any other).
 *
 * The card holds the copy under `tag` as a texture, in texture format
 * `format` (gc_efb_copy_texture_format). */
void gc_gx_thread_copy_has_texture(GcMachine *shadow, uint64_t tag, unsigned format);
/* Whether a texture at `address` of this format and size is a copy the card
 * holds a texture of, and which. 1: the copy stands there, unchanged since
 * it was made. 2: a copy that stood there, held by the card and drawn with,
 * whose bytes are there again although the place has been written since
 * (gc_gx_thread.c, "Copies remembered by their bytes") - the caller uses the
 * card's texture if its cache still has it, and says so if it has not
 * (gc_gx_thread_copy_forget). 0: neither, and the texture is its bytes. */
int gc_gx_thread_copy_texture(GcMachine *shadow, uint32_t address, unsigned format, unsigned width, unsigned height,
                              uint64_t *tag);
/* The cache no longer has the texture of the remembered copy `tag`: there
 * is nothing left to remember it for. */
void gc_gx_thread_copy_forget(GcMachine *shadow, uint64_t tag);
/* The card's textures are gone (the OpenGL path stopped): no copy is held
 * as one any more, and none is remembered. With the queue drained. */
void gc_gx_thread_copy_textures_gone(GcMachine *shadow);
/* The graphics thread's memory is about to be read at [first, first +
 * size): any copy's bytes it is still owed there are waited for and put in
 * place. */
void gc_gx_thread_need_bytes(GcMachine *shadow, uint32_t first, uint32_t size);

/* What remembering copies by their bytes has done on the machine's
 * graphics thread (all zero without one). With the queue drained. */
typedef struct GcGxCopyStats {
  uint64_t made;           /* copies remembered */
  uint64_t bytes_landed;   /* bytes copied for those remembered while still on their way, as they landed */
  uint64_t lookups;        /* textures named where a copy is remembered and none stands */
  uint64_t compares;       /* ... for which memory had to be compared with the copy's bytes */
  uint64_t bytes_compared; /* ... the bytes of those copies (a comparison stops at the first that differs) */
  uint64_t found;          /* lookups that found the copy's bytes there again */
  uint64_t forgotten;      /* let go because the cache no longer had the texture */
} GcGxCopyStats;
void gc_gx_thread_copy_stats(const GcMachine *m, GcGxCopyStats *stats);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_THREAD_H */
