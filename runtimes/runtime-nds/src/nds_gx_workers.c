/*
 * Helper threads for the larger 3D picture (nds_gx_render_scaled).
 *
 * The console's own picture is drawn on the machine's thread and nowhere
 * else. The larger one is the same work on nine or sixteen times the pixels,
 * and nothing in the machine waits on what it holds, so its lines are shared
 * out in bands between the calling thread and a few helpers. The helpers
 * work only inside nds_gx_workers_run(): it returns when every helper has
 * left the job, and what a job draws does not depend on which thread took
 * which band.
 *
 * The pool is the Nintendo 64 rasteriser's (runtimes/runtime-n64/src/
 * n64_rdp_workers.c, which has the reasoning for each step of the
 * generation, closed and inside handshake), with one difference in use: a
 * job here is a whole frame, milliseconds long, so every helper is woken
 * for it.
 */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#endif

#include "nds_gx_workers.h"

#include <stdlib.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#define GXW_PAUSE() _mm_pause()
#else
#define GXW_PAUSE() ((void)0)
#endif

#define GXW_MAX_HELPERS 15u
/* How long a helper spins after its last job before it sleeps (seconds).
 * The first two spin a little longer, as in the pool this one is taken from;
 * with one job a frame they are all asleep when it comes, and are woken. */
#define GXW_HOT_HELPERS 2
#define GXW_SPIN_HOT 0.00015
#define GXW_SPIN_COLD 0.0001

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef volatile LONG GxwAtomic;
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

typedef struct GxwWake {
    HANDLE event;
} GxwWake;
static void wake_init(GxwWake *w) { w->event = CreateEventA(NULL, FALSE, FALSE, NULL); }
static void wake_wait(GxwWake *w) { WaitForSingleObject(w->event, INFINITE); }
static void wake_signal(GxwWake *w) { SetEvent(w->event); }
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

typedef volatile long GxwAtomic;
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

typedef struct GxwWake {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int signalled;
} GxwWake;
static void wake_init(GxwWake *w)
{
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    w->signalled = 0;
}
static void wake_wait(GxwWake *w)
{
    pthread_mutex_lock(&w->lock);
    while (!w->signalled)
        pthread_cond_wait(&w->cond, &w->lock);
    w->signalled = 0;
    pthread_mutex_unlock(&w->lock);
}
static void wake_signal(GxwWake *w)
{
    pthread_mutex_lock(&w->lock);
    w->signalled = 1;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
}
#endif

typedef struct GxwHelper {
    GxwAtomic sleeping;
    GxwWake wake;
} GxwHelper;

static struct {
    GxwAtomic busy;       /* a job is being run (one caller at a time) */
    GxwAtomic generation; /* bumped to announce a job */
    GxwAtomic closed;     /* the generation whose job is finished */
    GxwAtomic inside;     /* helpers between joining a job and leaving it */
    GxwAtomic wanted;     /* helpers 1 to this take part in the current job */
    GxwWorkFn fn;
    void *job;
    /* The caller's alone. */
    double spin_hot, spin_cold;
    unsigned started; /* helper threads created so far; 0 = spin times not read yet */
    int threads;      /* threads a job may use, the caller included; 0 = not chosen yet */
    GxwHelper helper[GXW_MAX_HELPERS + 1u];
} pool;

static void helper_loop(unsigned id)
{
    GxwHelper *self = &pool.helper[id];
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
                GXW_PAUSE();
            if (pool.generation != seen)
                continue;
            if (now_seconds() - idle_since < (id <= GXW_HOT_HELPERS ? pool.spin_hot : pool.spin_cold))
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
        const char *text = getenv("NDS_GX_SPIN_US");
        pool.spin_hot = text && *text ? strtod(text, NULL) * 1e-6 : GXW_SPIN_HOT;
        pool.spin_cold = pool.spin_hot < GXW_SPIN_COLD ? pool.spin_hot : GXW_SPIN_COLD;
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

/* NDS_GX_THREADS, or half the host's processors up to eight: the machine
 * itself wants a processor, and so does whatever else the host is doing. */
static int default_threads(void)
{
    const char *text = getenv("NDS_GX_THREADS");
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
    if (n > (long)GXW_MAX_HELPERS + 1)
        n = (long)GXW_MAX_HELPERS + 1;
    return (int)n;
}

void nds_gx_workers_set_threads(int threads)
{
    if (threads > (int)GXW_MAX_HELPERS + 1)
        threads = (int)GXW_MAX_HELPERS + 1;
    pool.threads = threads > 0 ? threads : default_threads();
}

unsigned nds_gx_workers_threads(void)
{
    if (!pool.threads)
        pool.threads = default_threads();
    return (unsigned)pool.threads;
}

int nds_gx_workers_run(GxwWorkFn fn, void *job, unsigned helpers, int large)
{
    long generation;
    unsigned i;
    if (helpers + 1u > nds_gx_workers_threads())
        helpers = nds_gx_workers_threads() - 1u;
    /* One job at a time: a second machine on another thread draws alone. */
    if (!helpers || !ATOMIC_CAS(&pool.busy, 1, 0))
        return -1;
    helpers = start_helpers(helpers) < helpers ? pool.started : helpers;
    pool.fn = fn;
    pool.job = job;
    ATOMIC_SET(&pool.wanted, (long)helpers);
    generation = ATOMIC_INC(&pool.generation);
    for (i = 1; i <= helpers; i++) {
        if (pool.helper[i].sleeping && (large || i <= GXW_HOT_HELPERS))
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
                GXW_PAUSE();
            else
                yield_thread();
        }
    }
    ATOMIC_SET(&pool.busy, 0);
    return 0;
}

long nds_gx_workers_take(long *counter)
{
#ifdef _WIN32
    return (long)InterlockedIncrement((LONG *)counter) - 1;
#else
    return __sync_add_and_fetch(counter, 1) - 1;
#endif
}
