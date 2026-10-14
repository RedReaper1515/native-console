/*
 * The rasteriser's helper threads (n64_rdp_internal.h, "n64_rdp_workers.c").
 *
 * A primitive's scanlines are shared out between the thread that runs the
 * RDP and a few helpers. The helpers work only inside
 * n64_rdp_workers_run(): it returns when every helper has left the job, so
 * RDRAM is complete before the next command is read, and what a job writes
 * does not depend on which thread took which piece.
 *
 * A job is announced by bumping a generation number. A helper that is
 * spinning on that number starts within a few hundred cycles; one that is
 * asleep has to be woken, which takes tens of microseconds - as long as
 * most primitives take to draw, since the RDP is handed a triangle or two
 * at a time and must finish them before it returns. So:
 *
 *  - Helpers 1 and 2 keep spinning for RDP_SPIN_HOT after a job (or
 *    N64_RDP_SPIN_US microseconds; 0 never spins). Most primitives of a
 *    display list arrive closer together than that, so the two are mostly
 *    awake while a picture is being drawn and asleep between pictures.
 *    They are woken for any job.
 *  - The other helpers spin only for RDP_SPIN_COLD, and are woken only for
 *    a job the caller calls large: one that may still be running when
 *    they arrive, and that others like it tend to follow.
 *
 * Spinning longer buys little: over Ocarina of Time's first 1,800 frames
 * with eight threads, no spinning at all took 2.55 s of RDP time, 150
 * microseconds 2.4 s and 500 microseconds 2.4 s, for 41, 45 and 49 s of
 * processor time in the whole process.
 *
 * The scheme (generation, closed, inside) is the one the GameCube
 * rasteriser's pool uses (runtimes/runtime-gc/src/gc_gx_workers.c).
 */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#endif

#include "n64_rdp_internal.h"

#include <stdlib.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#define RDP_PAUSE() _mm_pause()
#else
#define RDP_PAUSE() ((void)0)
#endif

#define RDP_MAX_HELPERS 15u
/* The helpers that stay awake through a display list, and how long a
 * helper spins after its last job before it sleeps (seconds). */
#define RDP_HOT_HELPERS 2
#define RDP_SPIN_HOT 0.00015
#define RDP_SPIN_COLD 0.0001

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef volatile LONG RdpAtomic;
#define ATOMIC_INC(p) InterlockedIncrement((LONG *)(p))
#define ATOMIC_DEC(p) InterlockedDecrement((LONG *)(p))
#define ATOMIC_CAS(p, want, from) (InterlockedCompareExchange((LONG *)(p), (want), (from)) == (from))
#define ATOMIC_SET(p, value) ((void)InterlockedExchange((LONG *)(p), (value)))

static double now_seconds(void)
{
    static double scale;
    LARGE_INTEGER t;
    if (!scale) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        scale = 1.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * scale;
}

static unsigned host_processors(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (unsigned)info.dwNumberOfProcessors;
}

static void yield_thread(void) { SwitchToThread(); }

typedef struct RdpWake {
    HANDLE event;
} RdpWake;
static void wake_init(RdpWake *w) { w->event = CreateEventA(NULL, FALSE, FALSE, NULL); }
static void wake_free(RdpWake *w) { CloseHandle(w->event); }
static void wake_wait(RdpWake *w) { WaitForSingleObject(w->event, INFINITE); }
static void wake_signal(RdpWake *w) { SetEvent(w->event); }
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

typedef volatile long RdpAtomic;
#define ATOMIC_INC(p) __sync_add_and_fetch((p), 1)
#define ATOMIC_DEC(p) __sync_sub_and_fetch((p), 1)
#define ATOMIC_CAS(p, want, from) __sync_bool_compare_and_swap((p), (from), (want))
#define ATOMIC_SET(p, value) ((void)__sync_lock_test_and_set((p), (value)))

static double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static unsigned host_processors(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1u;
}

static void yield_thread(void) { sched_yield(); }

typedef struct RdpWake {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int signalled;
} RdpWake;
static void wake_init(RdpWake *w)
{
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    w->signalled = 0;
}
static void wake_free(RdpWake *w)
{
    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->lock);
}
static void wake_wait(RdpWake *w)
{
    pthread_mutex_lock(&w->lock);
    while (!w->signalled)
        pthread_cond_wait(&w->cond, &w->lock);
    w->signalled = 0;
    pthread_mutex_unlock(&w->lock);
}
static void wake_signal(RdpWake *w)
{
    pthread_mutex_lock(&w->lock);
    w->signalled = 1;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
}
#endif

typedef struct RdpHelper {
    RdpAtomic sleeping;
    RdpWake wake;
} RdpHelper;

static struct {
    RdpAtomic busy;       /* a job is being run (one caller at a time) */
    RdpAtomic generation; /* bumped to announce a job */
    RdpAtomic closed;     /* the generation whose job is finished */
    RdpAtomic inside;     /* helpers between joining a job and leaving it */
    RdpAtomic wanted;     /* helpers 1 to this take part in the current job */
    RdpWorkFn fn;
    void *job;
    /* The caller's alone. */
    double spin_hot, spin_cold;
    unsigned started; /* helper threads created so far; 0 = spin times not read yet */
    int threads;      /* threads a job may use, the caller included; 0 = not chosen yet */
    RdpHelper helper[RDP_MAX_HELPERS + 1u];
} pool;

static void helper_loop(unsigned id)
{
    RdpHelper *self = &pool.helper[id];
    /* A helper is created before the job it is first wanted for is
     * announced, with every generation up to then already closed. */
    long seen = pool.closed;
    double idle_since = now_seconds();
    for (;;) {
        long generation = pool.generation;
        if (generation != seen) {
            seen = generation;
            /* Join, then look whether the job is still the current one and
             * still open. The caller closes a job before it counts who is
             * inside, and announces the next only after that count reaches
             * zero, so either this helper sees the job it was woken for,
             * open, and the caller waits for it - or it leaves. A helper held
             * up long enough to miss a job entirely must not run the one
             * announced after it, whose description the caller may still be
             * writing, nor one already finished. */
            ATOMIC_INC(&pool.inside);
            if (pool.generation == generation && pool.closed != generation && (long)id <= pool.wanted)
                pool.fn(pool.job, id);
            ATOMIC_DEC(&pool.inside);
            idle_since = now_seconds();
            continue;
        }
        {
            unsigned i;
            for (i = 0; i < 256u && pool.generation == seen; i++)
                RDP_PAUSE();
            if (pool.generation != seen)
                continue;
            if (now_seconds() - idle_since < (id <= RDP_HOT_HELPERS ? pool.spin_hot : pool.spin_cold))
                continue;
        }
        /* Asleep - but only after looking once more, so a job announced
         * between the look and the flag is not missed: the caller bumps the
         * generation before it reads the flag. */
        ATOMIC_INC(&self->sleeping);
        if (pool.generation == seen)
            wake_wait(&self->wake);
        ATOMIC_DEC(&self->sleeping);
        idle_since = now_seconds();
    }
}

#ifdef _WIN32
#ifdef _MSC_VER
#pragma warning(disable : 4702) /* the return below: a helper never leaves its loop */
#endif
static DWORD WINAPI helper_main(LPVOID arg)
{
    helper_loop((unsigned)(uintptr_t)arg);
    return 0;
}
#else
static void *helper_main(void *arg)
{
    helper_loop((unsigned)(uintptr_t)arg);
    return NULL;
}
#endif

/* Create helper threads up to `count`; returns how many exist. */
static unsigned start_helpers(unsigned count)
{
    if (!pool.started) {
        const char *text = getenv("N64_RDP_SPIN_US");
        pool.spin_hot = text && *text ? strtod(text, NULL) * 1e-6 : RDP_SPIN_HOT;
        pool.spin_cold = pool.spin_hot < RDP_SPIN_COLD ? pool.spin_hot : RDP_SPIN_COLD;
    }
    while (pool.started < count) {
        unsigned id = pool.started + 1u;
        wake_init(&pool.helper[id].wake);
#ifdef _WIN32
        {
            HANDLE thread = CreateThread(NULL, 0, helper_main, (LPVOID)(uintptr_t)id, 0, NULL);
            if (!thread)
                break;
            CloseHandle(thread);
        }
#else
        {
            pthread_t thread;
            if (pthread_create(&thread, NULL, helper_main, (void *)(uintptr_t)id) != 0)
                break;
            pthread_detach(thread);
        }
#endif
        pool.started = id;
    }
    return pool.started;
}

/* N64_RDP_THREADS, or half the host's processors up to eight: primitives
 * are short, and past that helpers arrive after the work is done. */
static int default_threads(void)
{
    const char *text = getenv("N64_RDP_THREADS");
    long n;
    if (text && *text) {
        n = strtol(text, NULL, 10);
    } else {
        n = (long)(host_processors() / 2u);
        if (n > 8)
            n = 8;
    }
    if (n < 1)
        n = 1;
    if (n > (long)RDP_MAX_HELPERS + 1)
        n = (long)RDP_MAX_HELPERS + 1;
    return (int)n;
}

void n64_rdp_set_threads(int threads)
{
    if (threads > (int)RDP_MAX_HELPERS + 1)
        threads = (int)RDP_MAX_HELPERS + 1;
    pool.threads = threads > 0 ? threads : default_threads();
}

unsigned n64_rdp_workers_threads(void)
{
    if (!pool.threads)
        pool.threads = default_threads();
    return (unsigned)pool.threads;
}

int n64_rdp_workers_run(RdpWorkFn fn, void *job, unsigned helpers, int large)
{
    long generation;
    unsigned i;
    if (helpers + 1u > n64_rdp_workers_threads())
        helpers = n64_rdp_workers_threads() - 1u;
    /* One job at a time: a second RDP on another thread draws alone. */
    if (!helpers || !ATOMIC_CAS(&pool.busy, 1, 0))
        return -1;
    helpers = start_helpers(helpers) < helpers ? pool.started : helpers;
    pool.fn = fn;
    pool.job = job;
    ATOMIC_SET(&pool.wanted, (long)helpers);
    generation = ATOMIC_INC(&pool.generation);
    for (i = 1; i <= helpers; i++) {
        if (pool.helper[i].sleeping && (large || i <= RDP_HOT_HELPERS))
            wake_signal(&pool.helper[i].wake);
    }
    fn(job, 0);
    /* The caller has done every piece no helper took. Helpers still asleep,
     * or not yet scheduled, are not waited for: closing the job turns them
     * away.
     *
     * Closing is a locked write, not an assignment. A helper counts itself
     * in (a locked increment) and then reads `closed`; this thread writes
     * `closed` and then reads `inside`. A plain store can sit in the
     * processor's store buffer after this thread has already read `inside`,
     * so the helper could count itself in too late to be seen and still
     * read the job as open, and run it after this function had returned. A
     * locked write is committed before the next instruction, which is what
     * the two-sided handshake needs. */
    ATOMIC_SET(&pool.closed, generation);
    {
        unsigned spins = 0;
        while (pool.inside != 0) {
            if (++spins < 4096u)
                RDP_PAUSE();
            else
                yield_thread();
        }
    }
    ATOMIC_SET(&pool.busy, 0);
    return 0;
}

long n64_rdp_workers_take(long *counter)
{
#ifdef _WIN32
    return (long)InterlockedIncrement((LONG *)counter) - 1;
#else
    return __sync_add_and_fetch(counter, 1) - 1;
#endif
}

/* ---- the background thread ---- */

struct RdpAsync {
    RdpAtomic running; /* a job has been handed over and is not finished */
    RdpAtomic quit;
    RdpWake go, done; /* each stays signalled until it is waited for */
    RdpWorkFn fn;
    void *job;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
};

static void async_loop(RdpAsync *a)
{
    for (;;) {
        wake_wait(&a->go);
        if (a->quit)
            return;
        a->fn(a->job, 0);
        ATOMIC_SET(&a->running, 0);
        wake_signal(&a->done);
    }
}

#ifdef _WIN32
static DWORD WINAPI async_main(LPVOID arg)
{
    async_loop((RdpAsync *)arg);
    return 0;
}
#else
static void *async_main(void *arg)
{
    async_loop((RdpAsync *)arg);
    return NULL;
}
#endif

RdpAsync *n64_rdp_async_start(void)
{
    RdpAsync *a = (RdpAsync *)calloc(1, sizeof *a);
    int made;
    if (!a)
        return NULL;
    wake_init(&a->go);
    wake_init(&a->done);
#ifdef _WIN32
    a->thread = CreateThread(NULL, 0, async_main, a, 0, NULL);
    made = a->thread != NULL;
#else
    made = pthread_create(&a->thread, NULL, async_main, a) == 0;
#endif
    if (!made) {
        wake_free(&a->go);
        wake_free(&a->done);
        free(a);
        return NULL;
    }
    return a;
}

void n64_rdp_async_run(RdpAsync *a, RdpWorkFn fn, void *job)
{
    n64_rdp_async_wait(a);
    a->fn = fn;
    a->job = job;
    ATOMIC_SET(&a->running, 1);
    wake_signal(&a->go);
}

int n64_rdp_async_busy(const RdpAsync *a) { return a->running != 0; }

/* `done` may still be signalled from a job nobody waited for, so the flag
 * is what says the job is over; the thread clears it before it signals. */
void n64_rdp_async_wait(RdpAsync *a)
{
    while (a->running)
        wake_wait(&a->done);
}

void n64_rdp_async_stop(RdpAsync *a)
{
    if (!a)
        return;
    n64_rdp_async_wait(a);
    ATOMIC_SET(&a->quit, 1);
    wake_signal(&a->go);
#ifdef _WIN32
    WaitForSingleObject(a->thread, INFINITE);
    CloseHandle(a->thread);
#else
    pthread_join(a->thread, NULL);
#endif
    wake_free(&a->go);
    wake_free(&a->done);
    free(a);
}
