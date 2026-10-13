/*
 * n64_rdp_workers.c: the helper threads a primitive's scanlines, and the
 * lines of the larger picture's scan-out, are shared with.
 *
 * n64_rdp_workers_run() calls fn(job, 0) on the caller and
 * fn(job, 1..helpers) on up to `helpers` helper threads at once, and returns
 * 0 when all of them have returned; it returns -1 without calling fn when
 * there are no helpers to use. `large` says the job is worth waking
 * sleeping helpers for. The pieces of a job are handed out with
 * n64_rdp_workers_take(), which returns 0, 1, 2... from a counter that
 * starts at zero.
 */
#ifndef N64_RDP_WORKERS_H
#define N64_RDP_WORKERS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*RdpWorkFn)(void *job, unsigned thread);
unsigned n64_rdp_workers_threads(void); /* the caller included */
int n64_rdp_workers_run(RdpWorkFn fn, void *job, unsigned helpers, int large);
long n64_rdp_workers_take(long *counter);

/* One more thread, which runs a job while its owner goes on: the larger
 * picture is drawn there. n64_rdp_async_run() starts fn(job, 0) on it (the
 * job before must have finished); n64_rdp_async_wait() returns when it has
 * finished. n64_rdp_async_start() returns NULL where no thread can be
 * made. */
typedef struct RdpAsync RdpAsync;
RdpAsync *n64_rdp_async_start(void);
void n64_rdp_async_run(RdpAsync *async, RdpWorkFn fn, void *job);
int n64_rdp_async_busy(const RdpAsync *async);
void n64_rdp_async_wait(RdpAsync *async);
void n64_rdp_async_stop(RdpAsync *async);

#ifdef __cplusplus
}
#endif

#endif
