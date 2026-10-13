/*
 * The graphics thread (gc_gx_thread.h).
 *
 * The queue is a chain of blocks the machine's thread appends records to
 * and the graphics thread reads, one producer and one consumer. A record is
 * an eight-byte header (type, payload size) and its payload. The producer
 * publishes how many bytes of records are complete (`committed`); the
 * consumer runs records up to that and publishes how many it has finished
 * (`processed`). Nothing else is shared while both run.
 */
#include "gc_gx_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_gl.h"
#include "gc_gx.h"
#include "gc_gx_draw.h"
#include "gc_memory.h"
#include "gc_perf.h"

#if defined(_MSC_VER)
#define GX_THREAD_LOCAL __declspec(thread)
#else
#define GX_THREAD_LOCAL __thread
#endif

static GX_THREAD_LOCAL int on_graphics_thread;

int gc_gx_thread_on_thread(void) { return on_graphics_thread; }

int gc_gx_thread_active(const GcMachine *m) { return gc_gx_thread_of(m) != NULL; }

#ifndef _WIN32

int gc_gx_thread_start(GcMachine *m) {
  (void)m;
  return 0;
}
void gc_gx_thread_stop(GcMachine *m) { (void)m; }
GcGxThread *gc_gx_thread_of(const GcMachine *m) {
  (void)m;
  return NULL;
}
void gc_gx_thread_sync(GcMachine *m) { (void)m; }
GcMachine *gc_gx_thread_target(GcMachine *m) { return m; }
GcMachine *gc_gx_thread_drawn_on(GcMachine *m) { return m; }
void gc_gx_thread_quiesce(GcMachine *m) { (void)m; }
void gc_gx_thread_reload(GcMachine *m) { (void)m; }
void gc_gx_thread_gl_changed(GcMachine *m) { (void)m; }
void gc_gx_thread_begin_run(GcGxThread *t, GcMachine *m) {
  (void)t;
  (void)m;
}
void gc_gx_thread_end_run(GcGxThread *t) { (void)t; }
uint8_t *gc_gx_thread_commands(GcGxThread *t, GcMachine *m, size_t size) {
  (void)t;
  (void)m;
  (void)size;
  return NULL;
}
void gc_gx_thread_copy(GcGxThread *t, GcMachine *m, uint32_t trigger, const GcEfbCopyJob *job, uint64_t tag,
                       uint32_t first, uint32_t end) {
  (void)t, (void)m, (void)trigger, (void)job, (void)tag, (void)first, (void)end;
}
void gc_gx_thread_palette(GcGxThread *t, GcMachine *m, uint32_t payload, const uint16_t *entries, unsigned count) {
  (void)t, (void)m, (void)payload, (void)entries, (void)count;
}
void gc_gx_thread_send(GcGxThread *t, GcMachine *m, uint32_t first, uint32_t size) {
  (void)t, (void)m, (void)first, (void)size;
}
void gc_gx_thread_send_texture(GcGxThread *t, GcMachine *m, uint32_t first, uint32_t size) {
  (void)t, (void)m, (void)first, (void)size;
}
uint64_t gc_gx_thread_peek_from(GcGxThread *t, int plane) {
  (void)t, (void)plane;
  return 0;
}
void gc_gx_thread_peeks_forget(GcMachine *m) { (void)m; }
void gc_gx_thread_textures_changed(GcGxThread *t) { (void)t; }
int gc_gx_thread_textures_due(GcGxThread *t) {
  (void)t;
  return 0;
}
void gc_gx_thread_copy_landing(uint64_t tag, const uint8_t *memory, uint32_t first, uint32_t end) {
  (void)tag, (void)memory, (void)first, (void)end;
}
void gc_gx_thread_copy_landed(uint64_t tag) { (void)tag; }
void gc_gx_thread_copy_has_texture(GcMachine *shadow, uint64_t tag, unsigned format) {
  (void)shadow, (void)tag, (void)format;
}
int gc_gx_thread_copy_texture(GcMachine *shadow, uint32_t address, unsigned format, unsigned width, unsigned height,
                              uint64_t *tag) {
  (void)shadow, (void)address, (void)format, (void)width, (void)height, (void)tag;
  return 0;
}
void gc_gx_thread_need_bytes(GcMachine *shadow, uint32_t first, uint32_t size) {
  (void)shadow, (void)first, (void)size;
}
void gc_gx_thread_copy_forget(GcMachine *shadow, uint64_t tag) { (void)shadow, (void)tag; }
void gc_gx_thread_copy_textures_gone(GcMachine *shadow) { (void)shadow; }
void gc_gx_thread_copy_stats(const GcMachine *m, GcGxCopyStats *stats) {
  (void)m;
  memset(stats, 0, sizeof(*stats));
}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>

enum {
  REC_NEXT = 1, /* the rest of this block is unused: go on to the next */
  REC_CONTEXT,  /* GxContext: the field and the SSE modes the commands after it run in */
  REC_COMMANDS, /* command bytes */
  REC_COPY,     /* GxCopy */
  REC_PALETTE,  /* uint32_t payload, then uint16_t entries */
  REC_BYTES,    /* uint32_t address, then the bytes of main memory there */
  REC_PENDING   /* GxPending: bytes that are a copy's, which has not landed */
};

#define HEADER_BYTES 8u
#define BLOCK_BYTES ((size_t)4 << 20)
#define PAGE_SHIFT 12u
#define PAGE_BYTES 4096u
/* Queued and not yet finished beyond this, the machine waits for the
 * graphics thread to catch up; and when the graphics thread is more than
 * GC_GX_LAG fields (2 by default) behind the machine, which is what a picture
 * on the screen would be behind by. */
#define QUEUE_LIMIT ((uint64_t)64 << 20)
/* Ranges sent in the open burst, remembered so a range read by many draws
 * of a burst is sent once: nothing can change it within the burst except a
 * copy, which forgets them all. */
#define SENT_RECENT 32u
/* How many times the page a range begins or ends in is sent whole and
 * tracked before it is taken for a page the game keeps writing, whose few
 * bytes are cheaper sent every time than its write caught every time. */
#define EDGE_TRACK_LIMIT 6u

typedef struct GxBlock {
  struct GxBlock *next;
  size_t size;
  uint8_t data[1];
} GxBlock;

typedef struct GxContext {
  uint64_t field;
  uint32_t csr;
  uint32_t pad;
} GxContext;

typedef struct GxCopy {
  uint32_t trigger;
  uint32_t prepared;
  uint64_t tag;
  uint32_t first, end;
  GcEfbCopyJob job;
} GxCopy;

typedef struct GxPending {
  uint32_t first, size;
  uint64_t tag;
} GxPending;

/* A copy the graphics thread has made (or handed to the card to make) and
 * whose bytes its own memory may not hold yet.
 *
 * `stale` is the graphics thread's alone: its memory at [first, end) is
 * still what was there before the copy. `bytes` and `landed` are written by
 * whichever thread finishes the copy and read here, under the lock. `format`
 * is the texture format the card holds the copy in, or -1: the card's
 * texture stands for these bytes for as long as nothing else is known to
 * have been written over them. `used`: a draw has named the copy while its
 * format stood, so the card's texture is in the OpenGL path's cache. While
 * the format stands `bytes` is kept after the bytes have been put in place,
 * for what may be remembered of the copy.
 *
 * The same record is what is remembered of a copy afterwards (`memos`,
 * below). There `bytes` is the bytes the copy wrote, for good; `checked`
 * says the graphics thread's memory at [first, end) has been compared with
 * them since anything last changed it, and `same` what the comparison
 * found; `used`, that a texture has been found in it. */
#define GX_COPIES 32u
typedef struct GxCopyEntry {
  uint64_t tag; /* 0: free */
  uint32_t first, end;
  uint32_t destination, out_width, rows;
  int format;
  int stale;
  int landed;
  int used;
  int checked, same;
  uint8_t *bytes;
  uint64_t serial;
} GxCopyEntry;

/* Copies remembered by their bytes. A game takes a picture of its own
 * screen, reads the copy out with the processor, uses the place for
 * something else and writes the picture back before it draws with it: a
 * pause screen's frozen backdrop. The entry went when the place was copied
 * over, and bytes from the processor are not the card's - but a texture
 * named at the same place, of the same size and format and with the very
 * bytes the copy wrote, is that copy, and the texture the card made of it
 * at the internal resolution still stands for it. So an entry the card held
 * a texture for and a draw used is kept here when it goes, with the copy's
 * bytes, and gc_gx_thread_copy_texture looks here when no entry answers.
 *
 * Equal bytes are the whole test, and they are compared, not hashed: what
 * was copied there since, or how long ago this was, does not come into it,
 * because the picture a console would decode from those bytes is the one
 * the card's texture is a finer drawing of. One is kept a place and shape,
 * the newest; past GX_COPY_MEMOS of them one goes (memo_goes_before). Each
 * holds the buffer the copy's bytes landed in, at most a framebuffer's
 * worth (1.4 MB). Remembering a copy moves that buffer and copies nothing,
 * and nothing is compared until a draw names the place; the one thing done
 * that was not done before is that a copy whose entry goes while it is
 * still on its way has its bytes kept when they land, where they were
 * dropped.
 *
 * The graphics thread's, like the entries, with the same two fields the
 * lock covers: a copy remembered while it is still on its way lands here.
 * GC_GL_COPY_MEMO=0 remembers nothing. */
#define GX_COPY_MEMOS 8u

struct GcGxThread {
  GcMachine *m;
  GcMachine *shadow;
  uint8_t *alias;
  uint8_t *image; /* the graphics thread's copy of main memory */

  /* The producer's (the machine's thread). */
  GxBlock *write_block;
  size_t write_used;
  size_t commands_at; /* where the open command record's header is, or (size_t)-1 */
  uint64_t written;   /* bytes of records appended, the open one's included */
  int textures_due;
  int context_due;           /* the run's context differs from the last one queued */
  uint64_t run_field, queued_field;
  uint32_t run_csr, queued_csr;
  const uint8_t *tracking;
  uint8_t *edge_sends; /* a page: times it was sent whole as a range's partial page */
  uint64_t edge_pages, edge_pieces;
  struct {
    uint32_t first, size;
  } sent[SENT_RECENT];
  unsigned sent_next;
  int defer_copies; /* a texture inside a pending copy is not read (GC_GX_DEFER_COPIES=0: it is) */
  uint64_t ranges_deferred;
  /* Peeks answered late: the last two display copies issued (the newer
   * first) and the planes each was asked to keep; the planes peeks want
   * kept, and how many display copies each has gone unpeeked. */
  struct {
    uint64_t tag;
    uint32_t planes;
  } display[2];
  uint32_t peek_planes;
  unsigned peek_quiet[2];

  /* The consumer's (the graphics thread). */
  GxBlock *read_block;
  size_t read_used;
  uint64_t consumed;
  /* Copies on their way: the entries are the graphics thread's, except the
   * two fields a landing writes, which the lock covers. */
  GxCopyEntry copies[GX_COPIES];
  SRWLOCK copies_lock;
  CONDITION_VARIABLE copies_landed;
  uint64_t copy_serial;
  uint64_t copies_resolved, copies_waited, copies_lost, copies_superseded;
  double copy_wait_ms;
  GxCopyEntry memos[GX_COPY_MEMOS];
  int memo_on;
  GcGxCopyStats memo_stats;
  uint64_t memo_bytes_landed; /* whichever thread lands a copy, under the lock */

  /* Shared. */
  volatile LONG64 committed;
  volatile LONG64 processed; /* run, and nothing left held back (gc_gx_raster_flush) */
  volatile LONG64 progress;  /* run, some perhaps still held back */
  volatile LONG sleeping;
  volatile LONG sync_waiting;
  volatile LONG stopping;
  volatile LONG64 gx_field; /* the field of the burst the graphics thread last began */
  uint64_t lag_fields;
  SRWLOCK lock;
  CONDITION_VARIABLE work;
  CONDITION_VARIABLE drained;
  SRWLOCK free_lock;
  GxBlock *free_blocks;
  unsigned free_count;
  HANDLE thread;
  double spin_seconds;

  /* Statistics. */
  uint64_t runs, records, syncs, commits, blocks, bytes_sent, pages_sent, ranges_sent, lag_waits, bytes_same;
  double sync_ms, busy_ms, lag_wait_ms, cpu_ms;
};

#define THREADS_MAX 8u
static struct {
  GcMachine *m;
  GcGxThread *t;
} registry[THREADS_MAX];
static SRWLOCK registry_lock = SRWLOCK_INIT;

GcGxThread *gc_gx_thread_of(const GcMachine *m) {
  unsigned i;
  for (i = 0; i < THREADS_MAX; i++) {
    if (registry[i].m == m && m) return registry[i].t;
  }
  return NULL;
}

static double now_ms(void) { return gc_perf_now_ms(); }

/* ------------------------------------------------------------------ */
/* Blocks                                                              */

static GxBlock *block_new(GcGxThread *t, size_t size) {
  GxBlock *b = NULL;
  if (size <= BLOCK_BYTES) {
    AcquireSRWLockExclusive(&t->free_lock);
    if (t->free_blocks) {
      b = t->free_blocks;
      t->free_blocks = b->next;
      t->free_count--;
    }
    ReleaseSRWLockExclusive(&t->free_lock);
    size = BLOCK_BYTES;
  }
  if (!b) {
    b = (GxBlock *)malloc(sizeof(GxBlock) + size);
    if (!b) {
      fprintf(stderr, "gx thread: out of memory for the command queue\n");
      abort();
    }
    b->size = size;
    t->blocks++;
  }
  b->next = NULL;
  return b;
}

static void block_free(GcGxThread *t, GxBlock *b) {
  if (b->size == BLOCK_BYTES) {
    AcquireSRWLockExclusive(&t->free_lock);
    if (t->free_count < 16u) {
      b->next = t->free_blocks;
      t->free_blocks = b;
      t->free_count++;
      b = NULL;
    }
    ReleaseSRWLockExclusive(&t->free_lock);
  }
  free(b);
}

/* ------------------------------------------------------------------ */
/* Producing                                                           */

static void put_header(uint8_t *at, uint32_t type, uint32_t size) {
  memcpy(at, &type, 4);
  memcpy(at + 4, &size, 4);
}

/* The open command record closed: its size written into its header. */
static void close_commands(GcGxThread *t) {
  if (t->commands_at != (size_t)-1) {
    const uint32_t size = (uint32_t)(t->write_used - t->commands_at - HEADER_BYTES);
    put_header(t->write_block->data + t->commands_at, REC_COMMANDS, size);
    t->commands_at = (size_t)-1;
  }
}

/* Room for `bytes` more at the write position, moving to a new block (and
 * leaving a REC_NEXT behind) when this one is too full. Always leaves room
 * for that REC_NEXT. */
static void ensure(GcGxThread *t, size_t bytes) {
  GxBlock *next;
  if (t->write_used + bytes + HEADER_BYTES <= t->write_block->size) return;
  next = block_new(t, bytes + 2u * HEADER_BYTES);
  close_commands(t);
  put_header(t->write_block->data + t->write_used, REC_NEXT, 0);
  t->write_block->next = next;
  t->write_block = next;
  t->write_used = 0;
  t->written += HEADER_BYTES;
}

static uint8_t *record(GcGxThread *t, uint32_t type, size_t size);

/* The field and the SSE modes of this run, queued ahead of its first
 * record when they differ from the last queued. */
static void queue_context(GcGxThread *t) {
  uint8_t *at;
  GxContext c;
  t->context_due = 0;
  c.field = t->run_field;
  c.csr = t->run_csr;
  c.pad = 0;
  at = record(t, REC_CONTEXT, sizeof(c));
  memcpy(at, &c, sizeof(c));
  t->queued_field = t->run_field;
  t->queued_csr = t->run_csr;
}

static uint8_t *record(GcGxThread *t, uint32_t type, size_t size) {
  uint8_t *at;
  if (t->context_due) queue_context(t);
  close_commands(t);
  ensure(t, HEADER_BYTES + size);
  at = t->write_block->data + t->write_used;
  put_header(at, type, (uint32_t)size);
  t->write_used += HEADER_BYTES + size;
  t->written += HEADER_BYTES + size;
  return at + HEADER_BYTES;
}

static void commit(GcGxThread *t);

void gc_gx_thread_begin_run(GcGxThread *t, GcMachine *m) {
  t->runs++;
  /* The processor has run since the last: what was sent may have changed. */
  t->textures_due = 1;
  if (t->sent[0].size | t->sent[SENT_RECENT - 1u].size | t->sent_next) {
    memset(t->sent, 0, sizeof(t->sent));
    t->sent_next = 0;
  }
  t->run_field = m->field;
  /* The rounding mode and the other SSE modes the stream would have been run
   * in on this thread: the transform and the rasteriser compute in them. */
  t->run_csr = _mm_getcsr() & 0xffc0u;
  t->context_due = t->run_field != t->queued_field || t->run_csr != t->queued_csr;
}

void gc_gx_thread_end_run(GcGxThread *t) { commit(t); }

static void wait_processed(GcGxThread *t, uint64_t target) {
  unsigned spins = 0;
  while ((uint64_t)t->processed < target) {
    if (++spins < 2000u) {
      _mm_pause();
      continue;
    }
    AcquireSRWLockExclusive(&t->lock);
    InterlockedExchange(&t->sync_waiting, 1);
    while ((uint64_t)t->processed < target) SleepConditionVariableSRW(&t->drained, &t->lock, INFINITE, 0);
    InterlockedExchange(&t->sync_waiting, 0);
    ReleaseSRWLockExclusive(&t->lock);
  }
}

/* The graphics thread has begun a burst of `field` or later, or finished
 * everything up to target. */
static void wait_field(GcGxThread *t, uint64_t field, uint64_t target) {
  unsigned spins = 0;
  while ((uint64_t)t->gx_field < field && (uint64_t)t->processed < target) {
    if (++spins < 2000u) {
      _mm_pause();
      continue;
    }
    AcquireSRWLockExclusive(&t->lock);
    InterlockedExchange(&t->sync_waiting, 1);
    while ((uint64_t)t->gx_field < field && (uint64_t)t->processed < target) {
      SleepConditionVariableSRW(&t->drained, &t->lock, 1, 0);
    }
    InterlockedExchange(&t->sync_waiting, 0);
    ReleaseSRWLockExclusive(&t->lock);
  }
}

/* Everything complete handed over. */
static void commit(GcGxThread *t) {
  close_commands(t);
  if ((uint64_t)t->committed == t->written) return;
  InterlockedExchange64(&t->committed, (LONG64)t->written);
  t->commits++;
  if (t->sleeping) {
    AcquireSRWLockExclusive(&t->lock);
    WakeConditionVariable(&t->work);
    ReleaseSRWLockExclusive(&t->lock);
  }
  if (t->written - (uint64_t)t->progress > QUEUE_LIMIT) wait_processed(t, t->written - QUEUE_LIMIT / 4u);
  if (t->m->field > (uint64_t)t->gx_field + t->lag_fields) {
    const double started = now_ms();
    wait_field(t, t->m->field - t->lag_fields, t->written);
    t->lag_waits++;
    t->lag_wait_ms += now_ms() - started;
  }
}

uint8_t *gc_gx_thread_commands(GcGxThread *t, GcMachine *m, size_t size) {
  uint8_t *at;
  (void)m;
  if (t->context_due) queue_context(t);
  if (t->commands_at == (size_t)-1 || t->write_used + size + HEADER_BYTES > t->write_block->size) {
    close_commands(t);
    ensure(t, HEADER_BYTES + size);
    t->commands_at = t->write_used;
    put_header(t->write_block->data + t->write_used, REC_COMMANDS, 0);
    t->write_used += HEADER_BYTES;
    t->written += HEADER_BYTES;
  }
  at = t->write_block->data + t->write_used;
  t->write_used += size;
  t->written += size;
  return at;
}

/* How many display copies a plane's picture goes on being kept for after the
 * last peek at it. */
#define PEEK_QUIET 240u

uint64_t gc_gx_thread_peek_from(GcGxThread *t, int plane) {
  const uint32_t bit = 1u << (plane & 1);
  t->peek_planes |= bit;
  t->peek_quiet[plane & 1] = 0;
  return (t->display[1].planes & bit) ? t->display[1].tag : 0u;
}

void gc_gx_thread_peeks_forget(GcMachine *m) {
  GcGxThread *const t = gc_gx_thread_of(m);
  if (!t) return;
  memset(t->display, 0, sizeof(t->display));
  t->peek_planes = 0;
}

void gc_gx_thread_copy(GcGxThread *t, GcMachine *m, uint32_t trigger, const GcEfbCopyJob *job, uint64_t tag,
                       uint32_t first, uint32_t end) {
  GxCopy *c;
  uint32_t snapshot = 0;
  (void)m;
  if (job && job->to_display && tag) {
    /* A frame ends here: the planes peeks have been asking for go with it,
     * and it becomes the newer of the two a peek can be answered from. */
    unsigned plane;
    for (plane = 0; plane < 2u; plane++) {
      if ((t->peek_planes >> plane) & 1u && ++t->peek_quiet[plane] > PEEK_QUIET) t->peek_planes &= ~(1u << plane);
    }
    snapshot = t->peek_planes;
    t->display[1] = t->display[0];
    t->display[0].tag = tag;
    t->display[0].planes = snapshot;
  }
  c = (GxCopy *)record(t, REC_COPY, sizeof(GxCopy));
  memset(c, 0, sizeof(*c));
  c->trigger = trigger;
  c->prepared = job != NULL;
  c->tag = tag;
  c->first = first;
  c->end = end;
  if (job) {
    c->job = *job;
    c->job.snapshot = snapshot;
  }
  /* What was sent may be overwritten by the copy: send again. */
  t->textures_due = 1;
  memset(t->sent, 0, sizeof(t->sent));
  /* Handed over at once: the rest of the burst may read memory on the pages
   * the copy holds, which would otherwise wait for a copy nobody has. */
  commit(t);
}

void gc_gx_thread_palette(GcGxThread *t, GcMachine *m, uint32_t payload, const uint16_t *entries, unsigned count) {
  uint8_t *p;
  (void)m;
  p = record(t, REC_PALETTE, 4u + 2u * (size_t)count);
  memcpy(p, &payload, 4);
  memcpy(p + 4, entries, 2u * (size_t)count);
}

/* Bytes of main memory onto the queue, read now through the machine's view
 * (a page a copy still owes waits for it, as the single-threaded path's own
 * read would have). */
static void send_bytes(GcGxThread *t, GcMachine *m, uint32_t first, uint32_t size) {
  uint8_t *p;
  const uint8_t *view;
  if (!size) return;
  p = record(t, REC_BYTES, 4u + (size_t)size);
  memcpy(p, &first, 4);
  /* Bytes that only share a page with a pending copy are read through the
   * second view, where nothing stops the read (gc_memory_view_for); bytes
   * that are a pending copy's wait for it. */
  view = gc_memory_view_for(m->mem1, first, size);
  memcpy(p + 4, (view ? view : m->mem1) + first, size);
  t->bytes_sent += size;
}

/* The piece [first, end) of one page, which the range it belongs to shares
 * with whatever lies beside it. The whole page goes, once, and is tracked
 * like the pages a range covers - a model's arrays sit in the same place
 * every field, and their first and last pages were being copied here and
 * compared there for every draw that named them, eight megabytes a field of
 * bytes that had not changed (Super Mario Galaxy 2). Not when a copy is on
 * its way to the page, whose bytes this must not wait for; and not once the
 * page has been sent this way EDGE_TRACK_LIMIT times, which is a page the
 * game writes as often as it draws from it: then the piece alone, as before. */
static void send_edge(GcGxThread *t, GcMachine *m, uint32_t first, uint32_t end) {
  const uint32_t page = first >> PAGE_SHIFT;
  if (t->tracking[page]) return;
  if (t->edge_sends && t->edge_sends[page] < EDGE_TRACK_LIMIT &&
      !gc_memory_held(m->mem1, (size_t)page << PAGE_SHIFT, PAGE_BYTES)) {
    t->edge_sends[page]++;
    send_bytes(t, m, page << PAGE_SHIFT, PAGE_BYTES);
    gc_memory_track(m->mem1, page, page);
    t->pages_sent++;
    t->edge_pages++;
    return;
  }
  send_bytes(t, m, first, end - first);
  t->edge_pieces++;
}

void gc_gx_thread_send(GcGxThread *t, GcMachine *m, uint32_t first, uint32_t size) {
  uint32_t end, whole_first, whole_end, page;
  unsigned i;
  if (!size || first >= gc_ram_size) return;
  if (size > gc_ram_size - first) size = gc_ram_size - first;
  end = first + size;
  if (t->tracking) {
    /* Every page of it tracked: the graphics thread has it as it is. */
    const uint32_t last = (end - 1u) >> PAGE_SHIFT;
    page = first >> PAGE_SHIFT;
    while (page <= last && t->tracking[page]) page++;
    if (page > last) return;
  }
  for (i = 0; i < SENT_RECENT; i++) {
    if (t->sent[i].size && t->sent[i].first <= first && first + size <= t->sent[i].first + t->sent[i].size) return;
  }
  t->sent[t->sent_next].first = first;
  t->sent[t->sent_next].size = size;
  t->sent_next = (t->sent_next + 1u) % SENT_RECENT;
  t->ranges_sent++;
  whole_first = (first + PAGE_BYTES - 1u) >> PAGE_SHIFT;
  whole_end = end >> PAGE_SHIFT;
  if (!t->tracking) {
    send_bytes(t, m, first, size);
    return;
  }
  if (whole_end < whole_first) {
    /* All of it inside one page. */
    send_edge(t, m, first, end);
    return;
  }
  /* The partial pages at either end (send_edge); the whole pages only when
   * written since they were last sent. */
  if (first < (whole_first << PAGE_SHIFT)) send_edge(t, m, first, whole_first << PAGE_SHIFT);
  page = whole_first;
  while (page < whole_end) {
    uint32_t run_end;
    if (t->tracking[page]) {
      page++;
      continue;
    }
    run_end = page + 1u;
    while (run_end < whole_end && !t->tracking[run_end]) run_end++;
    send_bytes(t, m, page << PAGE_SHIFT, (run_end - page) << PAGE_SHIFT);
    gc_memory_track(m->mem1, page, run_end - 1u);
    t->pages_sent += run_end - page;
    page = run_end;
  }
  if (end > (whole_end << PAGE_SHIFT)) send_edge(t, m, whole_end << PAGE_SHIFT, end);
}

void gc_gx_thread_send_texture(GcGxThread *t, GcMachine *m, uint32_t first, uint32_t size) {
  const uint32_t end = (size > gc_ram_size - first) ? gc_ram_size : first + size;
  uint32_t at = first;
  size_t piece_end = 0;
  unsigned i;
  if (!size || first >= gc_ram_size) return;
  if (!t->defer_copies || (!gc_memory_held_at(m->mem1, first, &piece_end) && piece_end >= end)) {
    /* No pending copy anywhere in it: the ordinary way, whole. */
    gc_gx_thread_send(t, m, first, end - first);
    return;
  }
  for (i = 0; i < SENT_RECENT; i++) {
    if (t->sent[i].size && t->sent[i].first <= first && end <= t->sent[i].first + t->sent[i].size) return;
  }
  /* In pieces: the bytes that are a pending copy's are named and not read,
   * and the bytes between are sent. A texture's bytes are its first level
   * and, counted with it, a level or two that may never be sampled; a copy
   * is one level, so a texture that is a copy runs on past it - often into
   * the buffer the game's next copy is headed for. */
  while (at < end) {
    const uint64_t tag = gc_memory_held_at(m->mem1, at, &piece_end);
    const uint32_t stop = piece_end < end ? (uint32_t)piece_end : end;
    if (tag) {
      /* Nothing is read, and so nothing is marked as sent: the pages stay
       * untracked (the hold untracked them), and are sent in the ordinary
       * way the first time they are wanted after the copy has landed. */
      GxPending pending;
      pending.first = at;
      pending.size = stop - at;
      pending.tag = tag;
      memcpy(record(t, REC_PENDING, sizeof(pending)), &pending, sizeof(pending));
      t->ranges_deferred++;
    } else {
      gc_gx_thread_send(t, m, at, stop - at);
    }
    at = stop;
  }
  t->sent[t->sent_next].first = first;
  t->sent[t->sent_next].size = end - first;
  t->sent_next = (t->sent_next + 1u) % SENT_RECENT;
}

void gc_gx_thread_textures_changed(GcGxThread *t) { t->textures_due = 1; }

int gc_gx_thread_textures_due(GcGxThread *t) {
  const int due = t->textures_due;
  t->textures_due = 0;
  return due;
}

/* ------------------------------------------------------------------ */
/* Copies on their way                                                */

static GxCopyEntry *copy_entry(GcGxThread *t, uint64_t tag) {
  unsigned i;
  if (!tag) return NULL;
  for (i = 0; i < GX_COPIES; i++) {
    if (t->copies[i].tag == tag) return &t->copies[i];
  }
  return NULL;
}

/* Where the bytes of the copy `tag` land: its entry, or what is remembered
 * of it (*remembered) when the entry went while the copy was still on its
 * way. */
static GxCopyEntry *copy_landing_place(GcGxThread *t, uint64_t tag, int *remembered) {
  GxCopyEntry *const e = copy_entry(t, tag);
  unsigned i;
  *remembered = 0;
  if (e || !tag) return e;
  for (i = 0; i < GX_COPY_MEMOS; i++) {
    if (t->memos[i].tag == tag) {
      *remembered = 1;
      return &t->memos[i];
    }
  }
  return NULL;
}

void gc_gx_thread_copy_landing(uint64_t tag, const uint8_t *memory, uint32_t first, uint32_t end) {
  unsigned i;
  if (!tag || !memory) return;
  AcquireSRWLockShared(&registry_lock);
  for (i = 0; i < THREADS_MAX; i++) {
    GcGxThread *const t = registry[i].t;
    GxCopyEntry *e;
    int remembered;
    if (!t) continue;
    AcquireSRWLockExclusive(&t->copies_lock);
    e = copy_landing_place(t, tag, &remembered);
    if (e && !e->landed) {
      const uint32_t a = first > e->first ? first : e->first;
      const uint32_t b = end < e->end ? end : e->end;
      if (!e->bytes) e->bytes = (uint8_t *)malloc((size_t)(e->end - e->first));
      if (e->bytes && b > a) {
        memcpy(e->bytes + (a - e->first), memory + a, (size_t)(b - a));
        /* Bytes an entry that had gone would not have been handed. */
        if (remembered) t->memo_bytes_landed += b - a;
      }
    }
    ReleaseSRWLockExclusive(&t->copies_lock);
  }
  ReleaseSRWLockShared(&registry_lock);
}

void gc_gx_thread_copy_landed(uint64_t tag) {
  unsigned i;
  if (!tag) return;
  AcquireSRWLockShared(&registry_lock);
  for (i = 0; i < THREADS_MAX; i++) {
    GcGxThread *const t = registry[i].t;
    GxCopyEntry *e;
    int remembered;
    if (!t) continue;
    AcquireSRWLockExclusive(&t->copies_lock);
    e = copy_landing_place(t, tag, &remembered);
    if (e) {
      e->landed = 1;
      WakeAllConditionVariable(&t->copies_landed);
    }
    ReleaseSRWLockExclusive(&t->copies_lock);
  }
  ReleaseSRWLockShared(&registry_lock);
}

/* Bytes of the graphics thread's memory somewhere in [first, first + size)
 * have changed: a remembered copy compared with what was there has to be
 * compared again. */
static void memos_changed(GcGxThread *t, uint32_t first, uint32_t size) {
  unsigned i;
  for (i = 0; i < GX_COPY_MEMOS; i++) {
    GxCopyEntry *const memo = &t->memos[i];
    if (memo->tag && memo->first < first + size && first < memo->end) memo->checked = 0;
  }
}

/* The graphics thread's memory given the copy's bytes, waiting for them if
 * they have not landed. Triangles held back are drawn first when the bytes
 * differ from what is there, as for bytes off the queue. */
static void copy_resolve(GcGxThread *t, GxCopyEntry *e) {
  const uint8_t *bytes;
  if (!e->stale) return;
  AcquireSRWLockExclusive(&t->copies_lock);
  if (!e->landed) {
    const double started = now_ms();
    while (!e->landed) SleepConditionVariableSRW(&t->copies_landed, &t->copies_lock, INFINITE, 0);
    t->copies_waited++;
    t->copy_wait_ms += now_ms() - started;
  }
  bytes = e->bytes;
  ReleaseSRWLockExclusive(&t->copies_lock);
  if (bytes) {
    const size_t size = (size_t)(e->end - e->first);
    if (memcmp(t->image + e->first, bytes, size) != 0) {
      gc_gx_raster_flush(t->shadow);
      memcpy(t->image + e->first, bytes, size);
      t->shadow->cpu.cycles++;
      memos_changed(t, e->first, e->end - e->first);
    }
    /* In place now; the copy's own buffer has done its work - unless the
     * card holds the copy as a texture, which may yet be remembered by these
     * bytes (copy_remember). */
    if (!t->memo_on || e->format < 0) {
      AcquireSRWLockExclusive(&t->copies_lock);
      free(e->bytes);
      e->bytes = NULL;
      ReleaseSRWLockExclusive(&t->copies_lock);
    }
  }
  e->stale = 0;
  t->copies_resolved++;
}

static void copy_drop(GcGxThread *t, GxCopyEntry *e) {
  AcquireSRWLockExclusive(&t->copies_lock);
  free(e->bytes);
  memset(e, 0, sizeof(*e));
  ReleaseSRWLockExclusive(&t->copies_lock);
}

/* Which of two remembered copies goes first when room is wanted: a free
 * place before any; then one no texture was ever found in before one that
 * has answered a draw (which a game may be drawing with every frame); then
 * the one made or found longer ago. */
static int memo_goes_before(const GxCopyEntry *a, const GxCopyEntry *b) {
  if (!a->tag || !b->tag) return !a->tag && b->tag;
  if (a->used != b->used) return !a->used;
  return a->serial < b->serial;
}

/* An entry is about to go, or its bytes to change. When the card holds its
 * texture and a draw has used it, it is remembered (`memos`, above): what
 * the entry says, and its buffer - the bytes the copy wrote, which the
 * entry has kept whether or not they were put in place (copy_resolve).
 *
 * A copy still on its way is remembered too, by a buffer not yet filled;
 * the entry gives up its tag in the same breath, so that whoever finishes
 * the copy finds what is remembered and not the entry. Nothing is waited
 * for and nothing asked of the card: whether a copy is remembered depends
 * only on what the machine has issued, never on how far the drawing has
 * got. `used` goes on to mean that a texture has been found here. */
static void copy_remember(GcGxThread *t, GxCopyEntry *e) {
  GxCopyEntry *memo = NULL, *spare = &t->memos[0];
  unsigned i;
  if (!t->memo_on || e->format < 0 || !e->used || e->end == e->first) return;
  /* The one for this place and shape, or the one that goes first. */
  for (i = 0; i < GX_COPY_MEMOS; i++) {
    GxCopyEntry *const at = &t->memos[i];
    if (at->tag && at->format == e->format && at->destination == e->destination && at->out_width == e->out_width &&
        at->rows == e->rows) {
      memo = at;
      break;
    }
    if (memo_goes_before(at, spare)) spare = at;
  }
  if (!memo) memo = spare;
  AcquireSRWLockExclusive(&t->copies_lock);
  if (e->landed && !e->bytes) {
    /* Landed with no room for its bytes: nothing to know it by. */
    ReleaseSRWLockExclusive(&t->copies_lock);
    return;
  }
  {
    /* A copy none of which has landed yet lands in the buffer of the one it
     * replaces, when that is the size: a game that copies to the same place
     * every field then allocates nothing here. */
    uint8_t *const had = memo->bytes;
    const int fits = memo->tag && memo->end - memo->first == e->end - e->first;
    *memo = *e;
    if (!memo->bytes && fits) memo->bytes = had;
    else free(had);
  }
  e->bytes = NULL;
  if (e->stale) e->tag = 0;
  memo->stale = 0;
  memo->used = 0;
  memo->checked = 0;
  memo->same = 0;
  memo->serial = ++t->copy_serial;
  ReleaseSRWLockExclusive(&t->copies_lock);
  t->memo_stats.made++;
}

/* The card's texture no longer stands for an entry: its format is over, and
 * so is the buffer kept beyond its bytes being in place. */
static void copy_format_over(GcGxThread *t, GxCopyEntry *e) {
  e->format = -1;
  e->used = 0;
  if (!e->stale && e->bytes) {
    AcquireSRWLockExclusive(&t->copies_lock);
    free(e->bytes);
    e->bytes = NULL;
    ReleaseSRWLockExclusive(&t->copies_lock);
  }
}

/* A copy is about to be made over [c->first, c->end): what was known about
 * earlier copies there is over, and this one is on its way. */
static void copy_begin(GcGxThread *t, const GxCopy *c) {
  GxCopyEntry *slot = NULL, *oldest = NULL;
  unsigned i;
  for (i = 0; i < GX_COPIES; i++) {
    GxCopyEntry *const e = &t->copies[i];
    if (!e->tag) {
      if (!slot) slot = e;
      continue;
    }
    if (e->first < c->end && c->first < e->end) {
      /* Wholly overwritten: its bytes are never wanted. Partly: the part
       * this copy leaves is still its own. */
      if (!(c->first <= e->first && e->end <= c->end)) copy_resolve(t, e);
      else if (e->stale) t->copies_superseded++;
      copy_remember(t, e);
      copy_drop(t, e);
      if (!slot) slot = e;
      continue;
    }
    if (!oldest || e->serial < oldest->serial) oldest = e;
  }
  if (!slot) {
    copy_resolve(t, oldest);
    copy_remember(t, oldest);
    copy_drop(t, oldest);
    slot = oldest;
  }
  AcquireSRWLockExclusive(&t->copies_lock);
  slot->first = c->first;
  slot->end = c->end;
  slot->destination = c->job.destination;
  slot->out_width = c->job.out_width;
  slot->rows = c->job.out_rows;
  slot->format = -1;
  slot->stale = 1;
  slot->landed = 0;
  slot->used = 0;
  slot->bytes = NULL;
  slot->serial = ++t->copy_serial;
  slot->tag = c->tag;
  ReleaseSRWLockExclusive(&t->copies_lock);
  /* The OpenGL path looks at its textures again when this count moves: a
   * map bound to the copy made here before is not this one. */
  t->shadow->cpu.cycles++;
}

/* Every copy still owed anywhere in [first, first + size), put in place -
 * the oldest first, so that where two overlap the later one's bytes are
 * what is left. */
static void copies_resolve_over(GcGxThread *t, uint32_t first, uint32_t size) {
  for (;;) {
    GxCopyEntry *next = NULL;
    unsigned i;
    for (i = 0; i < GX_COPIES; i++) {
      GxCopyEntry *const e = &t->copies[i];
      if (e->tag && e->stale && e->first < first + size && first < e->end && (!next || e->tag < next->tag)) next = e;
    }
    if (!next) return;
    copy_resolve(t, next);
  }
}

/* Bytes are about to be written to the graphics thread's memory at [first,
 * first + size) from the queue: a copy still owed there goes in first (it
 * has landed: the machine read these bytes after it), and a copy whose
 * bytes these change is no longer what the card's texture shows - unless
 * the bytes it wrote come back (copy_remember). A remembered copy whose
 * place these change is compared again the next time it is asked for. */
static void copies_before_bytes(GcGxThread *t, uint32_t first, const uint8_t *bytes, uint32_t size) {
  unsigned i;
  copies_resolve_over(t, first, size);
  for (i = 0; i < GX_COPIES; i++) {
    GxCopyEntry *const e = &t->copies[i];
    uint32_t a, b;
    if (!e->tag || !(e->first < first + size && first < e->end)) continue;
    a = first > e->first ? first : e->first;
    b = first + size < e->end ? first + size : e->end;
    if (e->format >= 0 && memcmp(t->image + a, bytes + (a - first), (size_t)(b - a)) != 0) {
      copy_remember(t, e);
      copy_format_over(t, e);
    }
  }
  for (i = 0; i < GX_COPY_MEMOS; i++) {
    GxCopyEntry *const memo = &t->memos[i];
    uint32_t a, b;
    if (!memo->tag || !memo->checked || !(memo->first < first + size && first < memo->end)) continue;
    a = first > memo->first ? first : memo->first;
    b = first + size < memo->end ? first + size : memo->end;
    if (memcmp(t->image + a, bytes + (a - first), (size_t)(b - a)) != 0) memo->checked = 0;
  }
}

static GcGxThread *thread_of_shadow(const GcMachine *shadow) {
  unsigned i;
  for (i = 0; i < THREADS_MAX; i++) {
    if (registry[i].t && registry[i].t->shadow == shadow) return registry[i].t;
  }
  return NULL;
}

void gc_gx_thread_copy_has_texture(GcMachine *shadow, uint64_t tag, unsigned format) {
  GcGxThread *const t = thread_of_shadow(shadow);
  GxCopyEntry *const e = t ? copy_entry(t, tag) : NULL;
  if (e) e->format = (int)format;
}

/* Whether the graphics thread's memory holds a remembered copy's bytes
 * again. A copy still owed there goes in first, as it would for a texture
 * read from its bytes (gc_gx_thread_need_bytes), and the copy's own bytes
 * are waited for if they are still on their way: the answer is the same
 * however far the card has got. Compared once; then not again until
 * something changes a byte there, however often a draw asks. */
static int memo_same(GcGxThread *t, GxCopyEntry *memo) {
  const size_t size = (size_t)(memo->end - memo->first);
  const uint8_t *bytes;
  t->memo_stats.lookups++;
  copies_resolve_over(t, memo->first, memo->end - memo->first);
  if (memo->checked) return memo->same;
  AcquireSRWLockExclusive(&t->copies_lock);
  while (!memo->landed) SleepConditionVariableSRW(&t->copies_landed, &t->copies_lock, INFINITE, 0);
  bytes = memo->bytes;
  ReleaseSRWLockExclusive(&t->copies_lock);
  memo->same = bytes && memcmp(t->image + memo->first, bytes, size) == 0;
  memo->checked = 1;
  t->memo_stats.compares++;
  t->memo_stats.bytes_compared += size;
  return memo->same;
}

int gc_gx_thread_copy_texture(GcMachine *shadow, uint32_t address, unsigned format, unsigned width, unsigned height,
                              uint64_t *tag) {
  GcGxThread *const t = thread_of_shadow(shadow);
  unsigned i;
  if (!t) return 0;
  for (i = 0; i < GX_COPIES; i++) {
    GxCopyEntry *const e = &t->copies[i];
    if (e->tag && e->format == (int)format && e->destination == address && e->out_width == width &&
        e->rows == height) {
      e->used = 1;
      *tag = e->tag;
      return 1;
    }
  }
  /* No copy stands there now - but one may have, whose bytes these are
   * again. */
  for (i = 0; i < GX_COPY_MEMOS; i++) {
    GxCopyEntry *const memo = &t->memos[i];
    if (memo->tag && memo->format == (int)format && memo->destination == address && memo->out_width == width &&
        memo->rows == height) {
      if (!memo_same(t, memo)) return 0;
      memo->used = 1;
      memo->serial = ++t->copy_serial;
      t->memo_stats.found++;
      *tag = memo->tag;
      return 2;
    }
  }
  return 0;
}

void gc_gx_thread_copy_forget(GcMachine *shadow, uint64_t tag) {
  GcGxThread *const t = thread_of_shadow(shadow);
  unsigned i;
  if (!t || !tag) return;
  for (i = 0; i < GX_COPY_MEMOS; i++) {
    if (t->memos[i].tag == tag) {
      copy_drop(t, &t->memos[i]);
      t->memo_stats.forgotten++;
    }
  }
}

void gc_gx_thread_need_bytes(GcMachine *shadow, uint32_t first, uint32_t size) {
  GcGxThread *const t = thread_of_shadow(shadow);
  if (!t || !size) return;
  copies_resolve_over(t, first, size);
}

/* Nothing remembered any more. */
static void memos_forget(GcGxThread *t) {
  unsigned i;
  for (i = 0; i < GX_COPY_MEMOS; i++) {
    if (t->memos[i].tag) copy_drop(t, &t->memos[i]);
  }
}

void gc_gx_thread_copy_textures_gone(GcMachine *shadow) {
  GcGxThread *const t = thread_of_shadow(shadow);
  unsigned i;
  if (!t) return;
  /* The entries stay - their bytes may still be owed - as copies nobody
   * holds a texture of. */
  for (i = 0; i < GX_COPIES; i++) {
    if (t->copies[i].tag) copy_format_over(t, &t->copies[i]);
  }
  memos_forget(t);
}

static void copy_stats(GcGxThread *t, GcGxCopyStats *stats) {
  *stats = t->memo_stats;
  AcquireSRWLockShared(&t->copies_lock);
  stats->bytes_landed = t->memo_bytes_landed;
  ReleaseSRWLockShared(&t->copies_lock);
}

void gc_gx_thread_copy_stats(const GcMachine *m, GcGxCopyStats *stats) {
  GcGxThread *const t = gc_gx_thread_of(m);
  if (t) copy_stats(t, stats);
  else memset(stats, 0, sizeof(*stats));
}

/* Every copy forgotten: the machine's memory is about to be replaced, or
 * the thread is going. From the machine's thread, with the queue drained
 * and every copy landed. What was remembered goes too: after a state is
 * loaded the picture is what the state's bytes give, whatever this run drew
 * before. */
static void copies_forget(GcGxThread *t) {
  unsigned i;
  for (i = 0; i < GX_COPIES; i++) {
    if (t->copies[i].tag) copy_drop(t, &t->copies[i]);
  }
  memos_forget(t);
}



/* ------------------------------------------------------------------ */
/* Consuming                                                           */

/* GcGx fields the machine's thread keeps itself, in the stream's order of
 * events, and which the shadow's copies must not overwrite when state comes
 * back. */
static void copy_back(GcMachine *m, const GcMachine *shadow) {
  GcGx machine = m->gx;
  m->gx = shadow->gx;
  m->gx.fifo_base = machine.fifo_base;
  m->gx.fifo_end = machine.fifo_end;
  m->gx.read_ptr = machine.read_ptr;
  m->gx.write_ptr = machine.write_ptr;
  m->gx.linked = machine.linked;
  m->gx.perf_metric = machine.perf_metric;
  m->gx.matrix_index_a = machine.matrix_index_a;
  m->gx.matrix_index_b = machine.matrix_index_b;
  m->gx.vcd_lo = machine.vcd_lo;
  m->gx.vcd_hi = machine.vcd_hi;
  memcpy(m->gx.vat_a, machine.vat_a, sizeof(machine.vat_a));
  memcpy(m->gx.vat_b, machine.vat_b, sizeof(machine.vat_b));
  memcpy(m->gx.vat_c, machine.vat_c, sizeof(machine.vat_c));
  memcpy(m->gx.array_base, machine.array_base, sizeof(machine.array_base));
  memcpy(m->gx.array_stride, machine.array_stride, sizeof(machine.array_stride));
  memcpy(m->gx.bp, machine.bp, sizeof(machine.bp));
  m->gx.commands = machine.commands;
  m->gx.primitives = machine.primitives;
  m->gx.vertices = machine.vertices;
  m->gx.triangles = machine.triangles;
  m->gx.display_lists = machine.display_lists;
  m->gx.bytes = machine.bytes;
  m->gx.unknown_commands = machine.unknown_commands;
  m->gx.truncated_lists = machine.truncated_lists;
  m->gx.display_list_depth = machine.display_list_depth;
  m->gx.on_primitive = machine.on_primitive;
  m->gx.on_triangle = machine.on_triangle;
  if (getenv("GC_GX_THREAD_CHECK")) {
    /* The registers both sides store must agree. */
    if (memcmp(shadow->gx.bp, machine.bp, sizeof(machine.bp)) != 0 || shadow->gx.vcd_lo != machine.vcd_lo ||
        shadow->gx.vcd_hi != machine.vcd_hi || memcmp(shadow->gx.vat_a, machine.vat_a, sizeof(machine.vat_a)) != 0 ||
        memcmp(shadow->gx.array_base, machine.array_base, sizeof(machine.array_base)) != 0) {
      fprintf(stderr, "gx thread: the graphics thread's registers differ from the machine's\n");
    }
  }
}

static void run_record(GcGxThread *t, uint32_t type, const uint8_t *p, uint32_t size) {
  GcMachine *const s = t->shadow;
  switch (type) {
    case REC_CONTEXT: {
      GxContext c;
      memcpy(&c, p, sizeof(c));
      s->field = c.field;
      if (c.field != (uint64_t)t->gx_field) InterlockedExchange64(&t->gx_field, (LONG64)c.field);
      {
        const unsigned csr = _mm_getcsr();
        if ((csr & 0xffc0u) != c.csr) _mm_setcsr((csr & 0x3fu) | c.csr);
      }
      break;
    }
    case REC_COMMANDS: gc_gx_exec_commands(s, p, size); break;
    case REC_BYTES: {
      /* Bytes the triangles held back may still read must not change under
       * them: drawn first, when these bytes are new. The shadow's instruction
       * count stands for "memory has changed" (the OpenGL path looks at its
       * textures again when it moves). */
      uint32_t first;
      memcpy(&first, p, 4);
      copies_before_bytes(t, first, p + 4, size - 4u);
      if (memcmp(t->image + first, p + 4, size - 4u) != 0) {
        gc_gx_raster_flush(s);
        memcpy(t->image + first, p + 4, size - 4u);
        s->cpu.cycles++;
      } else {
        t->bytes_same += size - 4u;
      }
      break;
    }
    case REC_PALETTE: {
      uint32_t payload;
      memcpy(&payload, p, 4);
      gc_gx_raster_flush(s);
      s->gx.bp[0x65] = payload & 0x00ffffffu;
      {
        const uint32_t offset = (payload & 0x3ffu) * GC_GX_TLUT_UNIT_ENTRIES;
        const unsigned count = (size - 4u) / 2u;
        unsigned i;
        for (i = 0; i < count && offset + i < GC_GX_TLUT_ENTRIES; i++) {
          memcpy(&s->gx.tlut.entry[offset + i], p + 4u + 2u * i, 2);
        }
      }
      break;
    }
    case REC_COPY: {
      GxCopy c;
      memcpy(&c, p, sizeof(c));
      gc_gx_raster_flush(s);
      s->gx.bp[0x52] = c.trigger;
      if (c.prepared && c.tag && c.end > c.first) copy_begin(t, &c);
      if (c.prepared && !gc_gl_copy_prepared(s, &c.job, c.tag, c.first, c.end)) {
        /* The reference: from the framebuffer's planes into main memory
         * through its second view (the pages are held), then the clear, then
         * the pages go. */
        uint32_t x, y;
        c.job.color = s->efb.color;
        c.job.depth = s->efb.depth;
        c.job.ram = t->alias;
        gc_efb_copy_run(&c.job, 1);
        if (c.job.clears) {
          const uint32_t room = (c.job.left < GC_EFB_WIDTH) ? GC_EFB_WIDTH - c.job.left : 0u;
          const uint32_t across = (c.job.width < room) ? c.job.width : room;
          for (y = 0; y < c.job.height && c.job.top + y < GC_EFB_HEIGHT; y++) {
            const size_t row = (size_t)(c.job.top + y) * GC_EFB_WIDTH + c.job.left;
            for (x = 0; x < across; x++) {
              s->efb.color[row + x] = c.job.clear_colour;
              s->efb.depth[row + x] = c.job.clear_depth;
            }
          }
        }
        gc_gx_thread_copy_landing(c.tag, t->alias, c.first, c.end);
        gc_gx_thread_copy_landed(c.tag);
        if (c.tag) gc_memory_release_tagged(t->alias, c.first, c.end - c.first, c.tag, 1);
      }
      break;
    }
    case REC_PENDING: {
      GxPending pending;
      GxCopyEntry *e;
      memcpy(&pending, p, sizeof(pending));
      e = copy_entry(t, pending.tag);
      if (!e) {
        /* Not remembered any more - and an entry is only ever let go with
         * its bytes in place (copy_begin: a copy that overlaps it in part,
         * or the table full), or because a later copy covers all of it, and
         * a draw issued after that copy names that copy and not this one.
         * So the bytes are here already. Nothing is waited for: the pages
         * may be held again by a copy further down this very queue, which
         * waiting here would never reach - and did, in a window, where the
         * machine runs at a console's pace and the copies land sooner. */
        t->copies_lost++;
      } else if (e->format < 0) {
        /* Nobody holds it as a texture: the bytes are wanted. */
        copy_resolve(t, e);
      }
      break;
    }
    default:
      fprintf(stderr, "gx thread: a record of unknown type %u in the queue\n", type);
      abort();
  }
}

static DWORD WINAPI thread_main(LPVOID argument) {
  GcGxThread *const t = (GcGxThread *)argument;
  on_graphics_thread = 1;
  gc_memory_second_view_thread();
  /* Triangles may be held back across everything queued; they are drawn
   * before any register or memory they read changes, and whenever this
   * thread has caught up. */
  gc_gx_raster_begin(t->shadow);
  for (;;) {
    const uint64_t target = (uint64_t)t->committed;
    if (t->consumed < target) {
      const double started = now_ms();
      while (t->consumed < target) {
        uint32_t type, size;
        const uint8_t *const at = t->read_block->data + t->read_used;
        memcpy(&type, at, 4);
        memcpy(&size, at + 4, 4);
        if (type == REC_NEXT) {
          GxBlock *const done = t->read_block;
          t->read_block = done->next;
          t->read_used = 0;
          t->consumed += HEADER_BYTES;
          block_free(t, done);
          continue;
        }
        run_record(t, type, at + HEADER_BYTES, size);
        t->read_used += HEADER_BYTES + size;
        t->consumed += HEADER_BYTES + size;
        t->records++;
      }
      {
        const double ms = now_ms() - started;
        t->busy_ms += ms;
        gc_perf.gx_thread_ms += ms;
      }
      {
        /* This thread's own processor time, which other processes' load
         * inflates far less than the wall clock (gc_perf.c's reckoning). */
        ULONG64 cycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cycles);
        gc_perf.gx_thread_cycles = (uint64_t)cycles;
      }
      InterlockedExchange64(&t->progress, (LONG64)t->consumed);
      if (t->consumed == (uint64_t)t->committed) {
        /* Caught up: what is held is drawn, and everything so far is done. */
        gc_gx_raster_flush(t->shadow);
        InterlockedExchange64(&t->processed, (LONG64)t->consumed);
      }
      if (t->sync_waiting) {
        AcquireSRWLockExclusive(&t->lock);
        WakeAllConditionVariable(&t->drained);
        ReleaseSRWLockExclusive(&t->lock);
      }
      continue;
    }
    if (t->stopping) break;
    {
      /* Look for more for a moment before sleeping: bursts come a few hundred
       * microseconds apart while a game draws. */
      const double until = now_ms() + t->spin_seconds * 1000.0;
      unsigned spins = 0;
      while ((uint64_t)t->committed == t->consumed && !t->stopping) {
        _mm_pause();
        if ((++spins & 255u) == 0u && now_ms() >= until) break;
      }
      if ((uint64_t)t->committed != t->consumed) continue;
    }
    AcquireSRWLockExclusive(&t->lock);
    InterlockedExchange(&t->sleeping, 1);
    while ((uint64_t)t->committed == t->consumed && !t->stopping) {
      SleepConditionVariableSRW(&t->work, &t->lock, INFINITE, 0);
    }
    InterlockedExchange(&t->sleeping, 0);
    ReleaseSRWLockExclusive(&t->lock);
  }
  gc_gx_raster_end(t->shadow);
  gc_gx_raster_forget_thread();
  return 0;
}

/* ------------------------------------------------------------------ */
/* The machine's side                                                  */

static void shadow_from_machine(GcGxThread *t) {
  GcMachine *const m = t->m, *const s = t->shadow;
  s->gx = m->gx;
  s->gx.on_primitive = NULL;
  s->gx.on_triangle = NULL;
  s->efb = m->efb;
  s->mem1 = t->image;
  s->gl = m->gl;
  s->log = m->log;
  s->field = m->field;
  s->cpu.cycles = m->cpu.cycles;
}

int gc_gx_thread_start(GcMachine *m) {
  const char *env = getenv("GC_GX_THREAD");
  GcGxThread *t;
  unsigned i;
  if (!m || gc_gx_thread_of(m)) return m ? 1 : 0;
  if (env && (!strcmp(env, "0") || !strcmp(env, "off"))) return 0;
  if (m->gx.on_primitive || m->gx.on_triangle || !gc_memory_alias(m->mem1)) return 0;
  t = (GcGxThread *)calloc(1, sizeof(GcGxThread));
  if (!t) return 0;
  t->shadow = (GcMachine *)calloc(1, sizeof(GcMachine));
  t->image = (uint8_t *)VirtualAlloc(NULL, gc_ram_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!t->shadow || !t->image) {
    if (t->image) VirtualFree(t->image, 0, MEM_RELEASE);
    free(t->shadow);
    free(t);
    return 0;
  }
  t->m = m;
  t->alias = gc_memory_alias(m->mem1);
  t->tracking = gc_memory_tracking(m->mem1);
  /* GC_GX_TRACK_EDGES=0: a range's partial pages go as pieces, every time. */
  if (t->tracking && !(getenv("GC_GX_TRACK_EDGES") && !strcmp(getenv("GC_GX_TRACK_EDGES"), "0"))) {
    t->edge_sends = (uint8_t *)calloc(gc_ram_size >> PAGE_SHIFT, 1u);
  }
  t->commands_at = (size_t)-1;
  InitializeSRWLock(&t->lock);
  InitializeSRWLock(&t->free_lock);
  InitializeSRWLock(&t->copies_lock);
  InitializeConditionVariable(&t->work);
  InitializeConditionVariable(&t->drained);
  InitializeConditionVariable(&t->copies_landed);
  {
    const char *defer = getenv("GC_GX_DEFER_COPIES");
    const char *memo = getenv("GC_GL_COPY_MEMO");
    t->defer_copies = !(defer && !strcmp(defer, "0"));
    t->memo_on = !(memo && !strcmp(memo, "0"));
  }
  {
    const char *spin = getenv("GC_GX_SPIN_MS");
    const char *lag = getenv("GC_GX_LAG");
    t->spin_seconds = (spin && *spin ? strtod(spin, NULL) : 2.0) / 1000.0;
    t->lag_fields = lag && *lag ? strtoull(lag, NULL, 10) : 2u;
  }
  t->gx_field = (LONG64)m->field;
  t->write_block = t->read_block = block_new(t, BLOCK_BYTES);
  /* Nothing is tracked yet (the graphics thread's copy starts empty), and the
   * machine's graphics state goes over to the shadow. */
  gc_memory_untrack_all(m->mem1);
  shadow_from_machine(t);
  AcquireSRWLockExclusive(&registry_lock);
  for (i = 0; i < THREADS_MAX && registry[i].m; i++) {
  }
  if (i < THREADS_MAX) t->thread = CreateThread(NULL, 0, thread_main, t, 0, NULL);
  if (!t->thread) {
    ReleaseSRWLockExclusive(&registry_lock);
    free(t->write_block);
    VirtualFree(t->image, 0, MEM_RELEASE);
    free(t->shadow);
    free(t);
    return 0;
  }
  registry[i].t = t;
  registry[i].m = m;
  ReleaseSRWLockExclusive(&registry_lock);
  if (m->log) fprintf(m->log, "gx: the command stream draws on a thread of its own\n");
  return 1;
}

/* Everything queued, run. */
static void drain(GcGxThread *t) {
  const double started = now_ms();
  commit(t);
  wait_processed(t, t->written);
  t->syncs++;
  t->sync_ms += now_ms() - started;
  gc_perf.gx_syncs++;
  gc_perf.gx_sync_ms += now_ms() - started;
}

void gc_gx_thread_sync(GcMachine *m) {
  GcGxThread *const t = gc_gx_thread_of(m);
  if (!t || on_graphics_thread) return;
  drain(t);
  copy_back(m, t->shadow);
}

GcMachine *gc_gx_thread_target(GcMachine *m) {
  GcGxThread *const t = gc_gx_thread_of(m);
  if (!t || on_graphics_thread) return m;
  drain(t);
  return t->shadow;
}

GcMachine *gc_gx_thread_drawn_on(GcMachine *m) {
  GcGxThread *const t = gc_gx_thread_of(m);
  return (!t || on_graphics_thread) ? m : t->shadow;
}

void gc_gx_thread_quiesce(GcMachine *m) {
  GcGxThread *const t = gc_gx_thread_of(m);
  if (!t) return;
  gc_gx_thread_sync(m);
  /* Every copy landed, nothing tracked: memory can be written wholesale. */
  gc_gl_settle(t->shadow);
  gc_memory_settle(m->mem1, 0, gc_ram_size);
  gc_memory_untrack_all(m->mem1);
}

void gc_gx_thread_reload(GcMachine *m) {
  GcGxThread *const t = gc_gx_thread_of(m);
  if (!t) return;
  /* Drained, but nothing brought back: the machine's graphics state is the
   * new one, and it goes over to the shadow. */
  drain(t);
  gc_gl_settle(t->shadow);
  gc_memory_settle(m->mem1, 0, gc_ram_size);
  gc_memory_untrack_all(m->mem1);
  if (t->edge_sends) memset(t->edge_sends, 0, gc_ram_size >> PAGE_SHIFT);
  copies_forget(t);
  memset(t->display, 0, sizeof(t->display));
  shadow_from_machine(t);
  InterlockedExchange64(&t->gx_field, (LONG64)m->field);
}

void gc_gx_thread_gl_changed(GcMachine *m) {
  GcGxThread *const t = gc_gx_thread_of(m);
  if (!t) return;
  gc_gx_thread_sync(m);
  t->shadow->gl = m->gl;
  gc_gx_thread_peeks_forget(m);
}

void gc_gx_thread_stop(GcMachine *m) {
  GcGxThread *t;
  unsigned i;
  t = gc_gx_thread_of(m);
  if (!t) return;
  gc_gx_thread_quiesce(m);
  InterlockedExchange(&t->stopping, 1);
  AcquireSRWLockExclusive(&t->lock);
  WakeConditionVariable(&t->work);
  ReleaseSRWLockExclusive(&t->lock);
  WaitForSingleObject(t->thread, INFINITE);
  CloseHandle(t->thread);
  AcquireSRWLockExclusive(&registry_lock);
  for (i = 0; i < THREADS_MAX; i++) {
    if (registry[i].m == m) {
      registry[i].m = NULL;
      registry[i].t = NULL;
    }
  }
  ReleaseSRWLockExclusive(&registry_lock);
  if (m->log) {
    uint64_t faults, protects;
    gc_memory_track_stats(m->mem1, &faults, &protects);
    fprintf(m->log,
            "gx: thread stats runs=%llu records=%llu commits=%llu syncs=%llu sync_ms=%.1f busy_ms=%.1f "
            "queue_blocks=%llu ranges_sent=%llu bytes_sent=%llu pages_sent=%llu track_faults=%llu "
            "track_protects=%llu lag_waits=%llu lag_wait_ms=%.1f bytes_unchanged=%llu copies_deferred=%llu "
            "copies_resolved=%llu copies_waited=%llu copy_wait_ms=%.1f copies_superseded=%llu copies_lost=%llu "
            "edge_pages=%llu edge_pieces=%llu\n",
            (unsigned long long)t->runs, (unsigned long long)t->records, (unsigned long long)t->commits,
            (unsigned long long)t->syncs, t->sync_ms, t->busy_ms, (unsigned long long)t->blocks,
            (unsigned long long)t->ranges_sent, (unsigned long long)t->bytes_sent, (unsigned long long)t->pages_sent,
            (unsigned long long)faults, (unsigned long long)protects, (unsigned long long)t->lag_waits,
            t->lag_wait_ms, (unsigned long long)t->bytes_same, (unsigned long long)t->ranges_deferred,
            (unsigned long long)t->copies_resolved, (unsigned long long)t->copies_waited, t->copy_wait_ms,
            (unsigned long long)t->copies_superseded, (unsigned long long)t->copies_lost,
            (unsigned long long)t->edge_pages, (unsigned long long)t->edge_pieces);
    if (t->memo_stats.made) {
      GcGxCopyStats s;
      copy_stats(t, &s);
      fprintf(m->log,
              "gx: stats copies remembered made=%llu bytes_landed=%llu lookups=%llu compares=%llu "
              "bytes_compared=%llu found=%llu forgotten=%llu\n",
              (unsigned long long)s.made, (unsigned long long)s.bytes_landed, (unsigned long long)s.lookups,
              (unsigned long long)s.compares, (unsigned long long)s.bytes_compared, (unsigned long long)s.found,
              (unsigned long long)s.forgotten);
    }
  }
  free(t->edge_sends);
  t->edge_sends = NULL;
  copies_forget(t);
  {
    GxBlock *b = t->read_block;
    while (b) {
      GxBlock *const next = b->next;
      free(b);
      b = next;
    }
    b = t->free_blocks;
    while (b) {
      GxBlock *const next = b->next;
      free(b);
      b = next;
    }
  }
  VirtualFree(t->image, 0, MEM_RELEASE);
  free(t->shadow);
  free(t);
}

#endif /* _WIN32 */
