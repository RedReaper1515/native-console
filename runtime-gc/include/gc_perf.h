/*
 * Where a field's time went, one line a field: the runner's and the
 * frontend's --field-log FILE.
 *
 * The counters are running totals the runtime adds to as it works - the
 * interpreter's instructions, the command stream, the OpenGL path's own
 * work and its waits, textures decoded, programs compiled where a field
 * waits for them, framebuffer readbacks, disc reads - and the log prints
 * what each grew by in the field. They are host-side measurement only:
 * nothing the machine can see reads them, and they are not in a state.
 *
 * The counters are process-wide (a differential's second machine adds to
 * them too) and written without locks from the machine's thread, except
 * where noted.
 */
#ifndef GC_PERF_H
#define GC_PERF_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcMachine GcMachine;

typedef struct GcPerf {
  uint64_t interpreted;       /* instructions stepped by the interpreter */
  double gx_ms;               /* the command stream, including the draws it flushed */
  double gl_ms;              /* the OpenGL path's work on the machine's thread */
  double gl_wait_ms;          /* ... waiting for its render thread */
  double page_wait_ms;        /* waiting for a held page (an EFB copy on its way) */
  uint64_t page_steps;        /* accesses to a held page run at once, clear of the copy */
  uint64_t textures_decoded;  /* texture chains decoded (OpenGL cache misses) */
  double texture_decode_ms;
  double texture_hash_ms;
  uint64_t readbacks;         /* the whole framebuffer brought back from the card */
  uint64_t programs_waited;   /* programs compiled or loaded where a draw waited */
  double program_wait_ms;
  uint64_t disc_reads;
  uint64_t disc_bytes;
  double disc_ms;
  uint64_t efb_copies;
  /* The graphics thread (gc_gx_thread.h). */
  /* What a field actually drew, counted where the drawing happens - so
   * these are right whether or not there is a graphics thread, which the
   * machine's own gx.triangles and gx.pixels are not: those belong to the
   * drawing machine and only come back at a sync.
   *
   * Approximate by a hair: the helpers that prepare triangles add to the
   * count without locking, so a field can lose one. They are here to show
   * what a field drew relative to another field, and a race that misses
   * one in a hundred thousand does not change that. */
  /* The software rasteriser's two phases, wall time on the thread that
   * runs the flush: setting every triangle up (clip, box, edges, plan) and
   * then drawing the rows. Both include whatever the helper threads did,
   * because the flush waits for them. */
  double raster_prepare_ms;
  double raster_rows_ms;
  uint64_t triangles;         /* triangles rasterised, after clipping */
  uint64_t pixels;            /* pixels the combiner ran for */
  double gx_thread_ms;        /* its own time running commands (written by that thread) */
  uint64_t gx_thread_cycles;  /* its processor cycles so far (written by that thread) */
  uint64_t gx_syncs;          /* times the machine's thread waited for it to finish everything */
  double gx_sync_ms;
  uint64_t watch_faults;      /* writes to a page a queued command still read, or had read */
  uint64_t watch_waits;       /* ... of which waited for the graphics thread */
  double watch_wait_ms;
  uint64_t idle_cycles;       /* instructions of wait loops counted and not run (gc_idle_cycles) */
} GcPerf;

extern GcPerf gc_perf;

/* Milliseconds on a monotonic clock. */
double gc_perf_now_ms(void);

/* A cheaper clock for code run thousands of times a field: the time-stamp
 * counter, and what one tick is in milliseconds once a field log has
 * measured it (0 until then, when nobody reads the sums). */
uint64_t gc_perf_ticks(void);
extern double gc_perf_ms_per_tick;

/* --field-log: open the file (0, or -1 if it cannot be written), then call
 * begin before running a field and end after it. end writes the line: the
 * field, the wall time from the last begin to this one (the whole field as
 * someone watching sees it, pacing and presenting included), the time
 * spent running the machine, and each counter's growth. */
int gc_perf_field_log_open(const char *path);
void gc_perf_field_begin(const GcMachine *m);
void gc_perf_field_end(const GcMachine *m);
void gc_perf_field_log_close(void);

#ifdef __cplusplus
}
#endif

#endif /* GC_PERF_H */
