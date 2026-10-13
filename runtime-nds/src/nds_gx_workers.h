/*
 * nds_gx_workers.c: the helper threads the larger 3D picture's lines are
 * shared with (nds_gx_render_scaled, in nds_gx_raster.c).
 *
 * nds_gx_workers_run() calls fn(job, 0) on the caller and fn(job,
 * 1..helpers) on up to `helpers` helper threads at once, and returns 0 when
 * all of them have returned; it returns -1 without calling fn when there
 * are no helpers to use. `large` says the job is worth waking sleeping
 * helpers for. The pieces of a job are handed out with
 * nds_gx_workers_take(), which returns 0, 1, 2... from a counter that starts
 * at zero.
 */
#ifndef NDS_GX_WORKERS_H
#define NDS_GX_WORKERS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GxwWorkFn)(void *job, unsigned thread);

/* How many threads a job may use, the caller included: NDS_GX_THREADS in
 * the environment, or half the host's processors up to eight. 0 or less
 * goes back to that. */
void nds_gx_workers_set_threads(int threads);
unsigned nds_gx_workers_threads(void);
int nds_gx_workers_run(GxwWorkFn fn, void *job, unsigned helpers, int large);
long nds_gx_workers_take(long *counter);

#ifdef __cplusplus
}
#endif

#endif /* NDS_GX_WORKERS_H */
