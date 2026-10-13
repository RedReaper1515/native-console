/*
 * Main memory as the host holds it: one block the machine reads and writes,
 * a second view of the same bytes for a thread that must write while the
 * machine's view is held, holds on pages of the machine's view, and pages
 * tracked for a graphics thread's copy of memory.
 *
 * Private to the runtime. What it is for:
 *
 * Holds. An EFB copy is made later than the command that asks for it - on
 * the card by the OpenGL path, or on the graphics thread (gc_gx_thread.h) -
 * and its bytes reach main memory from another thread. Until then the pages
 * the copy writes are *held* - the machine's view of them is not accessible
 * - so anything that touches them first (recompiled code, the interpreter, a
 * DMA transfer, a save state) stops on the access, waits for the bytes to
 * arrive and carries on - unless the access is on the machine's thread and
 * lands far enough from every pending copy's bytes that it cannot touch one
 * (a heap block on the page a display buffer ends in): then that one
 * instruction runs at once with its page opened. Nothing can read a page
 * before its copy has been written or write a page before its copy lands on
 * top of it, so what every reader sees is exactly what it would have seen
 * had the copy been made at once. The thread that makes the copy writes
 * through the second view and releases the hold.
 *
 * Tracking. The graphics thread reads vertex arrays and textures from a copy
 * of main memory of its own, which the machine's thread keeps up to date by
 * sending it, in command order, the bytes each command will read. A whole
 * page sent is *tracked*: the machine's view of it becomes read-only, and
 * the first write to it after that opens it and forgets the mark. So a page
 * still tracked is one the graphics thread already has as it is, and need
 * not be sent again. Nothing ever waits for the graphics thread here.
 *
 * Windows only for now (a file mapping viewed twice, VirtualProtect and a
 * vectored exception handler); elsewhere there is no second view,
 * gc_memory_hold fails, tracking does nothing, and neither the OpenGL
 * path's asynchronous copies nor the graphics thread are used.
 */
#ifndef GC_MEMORY_H
#define GC_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zeroed memory of `size` bytes (a multiple of 4096), or NULL. */
uint8_t *gc_memory_create(size_t size);
void gc_memory_destroy(uint8_t *base);

/* The second view of the block that starts at base, or NULL when the host
 * has none. Every function here also accepts the second view's own address
 * as `base`. */
uint8_t *gc_memory_alias(const uint8_t *base);

/* Hold every page that [offset, offset + size) touches, counting holds, so
 * that the machine's view of them stops anybody who touches them until
 * each hold is released. Returns 0, or -1 when holds are not available
 * (then nothing is held). From the machine's thread.
 *
 * The tagged forms name the hold (a copy's number) so that the byte range it
 * was taken for is forgotten exactly when its last pages are released
 * (`final`), whatever other holds share those pages. The untagged forms
 * forget a range once none of its pages is held. */
int gc_memory_hold(uint8_t *base, size_t offset, size_t size);
void gc_memory_release(uint8_t *base, size_t offset, size_t size);
/* A new tag: numbers grow, in the order they are asked for, from 1. */
uint64_t gc_memory_next_tag(void);
int gc_memory_hold_tagged(uint8_t *base, size_t offset, size_t size, uint64_t tag);
void gc_memory_release_tagged(uint8_t *base, size_t offset, size_t size, uint64_t tag, int final);

/* Whether any page of the range is held right now. */
int gc_memory_held(const uint8_t *base, size_t offset, size_t size);

/* The tag of the pending hold whose byte range contains `offset`, and where
 * that range ends. The newest, when several do: what the byte will hold
 * once every copy has landed is the last one's. Or 0 when none does, and
 * then *end is where the next pending range after `offset` begins (the end
 * of the block when there is none). */
uint64_t gc_memory_held_at(const uint8_t *base, size_t offset, size_t *end);

/* Wait until no pending hold covers a byte of the range (by the byte ranges
 * holds were taken for, as gc_memory_view_for judges it: a held page whose
 * held bytes lie outside the range does not count), for at most `timeout_ms`
 * (negative: no limit). Returns 1 when none does, 0 on the time running out.
 * Changes nothing about the pages (compare gc_memory_settle). */
int gc_memory_wait_released(const uint8_t *base, size_t offset, size_t size, double timeout_ms);

/* Wait until no page of the range is held, and open (untrack) every page of
 * it. For a host call that writes or reads main memory from inside the
 * operating system (a file read straight into it), which reports a closed
 * page as an error instead of stopping. */
void gc_memory_settle(const uint8_t *base, size_t offset, size_t size);

/* A view of the whole block through which every byte of [offset, offset +
 * size) can be read and written now, without waiting: the machine's view
 * (base) when no page of the range is held, the second view when pages are
 * held but no held range covers a byte of it - it only shares a page with
 * one, as a ring buffer beside a display buffer does - and NULL when a held
 * range covers one of its bytes (then the caller goes through the machine's
 * view and waits, as anything else would).
 *
 * Exact for the same reason holds are: a pending copy writes only the bytes
 * it was held for, so a byte outside them reads the same through either
 * view, and a write to one is not overwritten when the copy lands. The
 * answer lasts until gc_memory_generation changes. */
uint8_t *gc_memory_view_for(uint8_t *base, size_t offset, size_t size);

/* Changes whenever a hold is taken or released, in any block. */
extern volatile long long gc_memory_generation;

/* Tracking, from the machine's thread only. The marks, a byte a page (read
 * without a lock by that thread; non-zero: tracked), or NULL when the host
 * has none. */
const uint8_t *gc_memory_tracking(const uint8_t *base);
/* Pages [first_page, last_page] tracked (and read-only). */
void gc_memory_track(uint8_t *base, size_t first_page, size_t last_page);
/* Every mark forgotten and every page writable again. */
void gc_memory_untrack_all(uint8_t *base);
/* Writes that found a tracked page, and pages made read-only, so far. */
void gc_memory_track_stats(const uint8_t *base, uint64_t *faults, uint64_t *protects);

/* Around a job that other threads run over the machine's view (the pixel
 * path's helpers, gc_gx_workers.h): while one runs, an access to a held
 * page on the machine's thread waits for the copy rather than running with
 * the page opened, because a helper could read a copy's bytes through the
 * opened page. Every gc_workers_run that the machine's thread starts over
 * main memory is inside one; one the render thread starts over its own
 * buffers need not be, and neither need the graphics thread's, whose jobs
 * read its own copy (gc_memory_second_view_thread). */
void gc_memory_shared_begin(void);
void gc_memory_shared_end(void);
/* gc_workers_run(fn, job) inside one, as an expression with its result. */
int gc_memory_shared_end_with(int result);
#define GC_WORKERS_RUN_SHARED(fn, job) \
  (gc_memory_shared_begin(), gc_memory_shared_end_with(gc_workers_run((fn), (job))))
/* This thread, and the helper jobs it starts, do not read the machine's
 * view: gc_memory_shared_begin and end do nothing on it. */
void gc_memory_second_view_thread(void);

/* How many bytes of memory the host instruction at `code` moves, as the
 * rule that lets an access beside a pending copy through judges it: the
 * operand's size for a general-purpose move, compare or arithmetic
 * instruction and for one element of a string instruction, and 64 - more
 * than any instruction moves - for everything it does not recognise. For
 * the tests; 0 where nothing is ever held. */
unsigned gc_memory_access_reach(const uint8_t *code);

/* How many accesses have stopped on a hold so far. */
uint64_t gc_memory_waits(const uint8_t *base);
/* ... and how many ran at once instead, touching none of a copy's bytes. */
uint64_t gc_memory_steps(const uint8_t *base);
/* ... and how long they stopped for, in all. */
double gc_memory_wait_ms(const uint8_t *base);

#ifdef __cplusplus
}
#endif

#endif /* GC_MEMORY_H */
