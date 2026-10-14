/*
 * A small pool of helper threads for the pixel path.
 *
 * Private to the runtime. What is safe to run on them is the caller's
 * argument to make, not this file's: the pool only runs one function on
 * every helper and on the calling thread at once, and returns when all of
 * them have returned. gc_gx_raster.c says why its rows may be split.
 *
 * GC_RASTER_THREADS in the environment sets the number of helpers (0 turns
 * the pool off); the default is one fewer than the host's processors, at
 * most 15. How many of them spin between jobs follows the share of the
 * caller's time the jobs take (gc_gx_workers.c); GC_RASTER_SPIN_MS (4 by
 * default) is how long a helper spins after its last job before it sleeps,
 * and GC_RASTER_SPIN_ALL=1 lets every helper spin and be woken for every
 * job, as the pool did before it measured.
 */
#ifndef GC_GX_WORKERS_H
#define GC_GX_WORKERS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called once on each helper with its number (1 to the helper count) and
 * once on the caller with 0. */
typedef void (*GcWorkFn)(void *job, unsigned worker);

/* How many helpers there are, starting them the first time it is asked.
 * Zero means run everything on the caller. */
unsigned gc_workers_count(void);

/* Runs fn on every helper and on the caller, and waits for all of them.
 * Returns 0, or -1 without running anything if the pool is already busy
 * (a second caller on another thread) so the caller can do the work
 * itself. */
int gc_workers_run(GcWorkFn fn, void *job);

/* The next value of a shared counter, atomically: 0, 1, 2 ... */
long gc_workers_take(long *counter);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_WORKERS_H */
