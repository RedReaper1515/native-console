/*
 * The pixel path's helper threads (gc_gx_workers.h).
 *
 * A job is announced by bumping a generation number. A helper that has
 * worked recently is spinning on that number and starts within a few
 * hundred cycles; one that is asleep is woken.
 *
 * Spinning is what makes a job of a few dozen microseconds worth splitting
 * - a wake costs about that much - and it is also a processor kept busy
 * for nothing whenever the jobs are a small part of the time. So how many
 * helpers spin follows how much of the caller's time the jobs take:
 *
 * - The dispatcher measures the share of its time spent inside jobs over
 *   the last 50 ms or so, and lets helpers 1 to count x share / 40 % spin
 *   (all of them from 40 %, at least one). The software rasteriser drawing
 *   Shrek 2's play spends 40-50 % of its time in jobs and keeps every helper
 *   spinning; the OpenGL renderer spends 12-15 % in its triangle setup and
 *   keeps five or six. Fewer spinning makes jobs slower and their share
 *   larger, so the count settles where the helpers are paying for
 *   themselves.
 * - A helper allowed to spin still sleeps GC_RASTER_SPIN_MS (4 ms by
 *   default) after its last job, so a paused game, a loading screen or the
 *   time a paced frontend waits for the next field costs nothing.
 * - Sleeping helpers are woken for a job only if that job function's work
 *   is worth a wake: its time with helpers times the helpers that took a
 *   piece (plus the caller), averaged, of at least a millisecond. Otherwise
 *   the job is shared by whoever is spinning, or done by the caller.
 *
 * None of it changes what is computed - a piece runs on whichever thread
 * takes it - only how soon a helper is there to take one.
 *
 * The floating-point environment a helper computes in is the caller's:
 * its rounding mode is copied across with the job, so a pixel computed on
 * a helper is computed exactly as it would have been on the caller.
 */

#include "gc_gx_workers.h"

#include <stdint.h>
#include <stdlib.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#define GC_PAUSE() _mm_pause()
#else
#define GC_PAUSE() ((void)0)
#endif

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
typedef unsigned GcFpEnv;
static GcFpEnv fp_env_get(void) { return _mm_getcsr() & 0xffc0u; } /* modes and masks, not flags */
static void fp_env_set(GcFpEnv env) {
  const unsigned csr = _mm_getcsr();
  if ((csr & 0xffc0u) != env) _mm_setcsr((csr & 0x3fu) | env);
}
#else
#include <fenv.h>
typedef int GcFpEnv;
static GcFpEnv fp_env_get(void) { return fegetround(); }
static void fp_env_set(GcFpEnv env) {
  if (fegetround() != env) fesetround(env);
}
#endif

#if defined(_MSC_VER)
#define GC_THREAD_LOCAL __declspec(thread)
#else
#define GC_THREAD_LOCAL __thread
#endif

#define GC_MAX_HELPERS 15u

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef volatile LONG GcAtomic;
#define ATOMIC_INC(p) InterlockedIncrement((LONG *)(p))
#define ATOMIC_DEC(p) InterlockedDecrement((LONG *)(p))
#define ATOMIC_CAS(p, want, from) (InterlockedCompareExchange((LONG *)(p), (want), (from)) == (from))
#define ATOMIC_SET(p, value) ((void)InterlockedExchange((LONG *)(p), (value)))

static double now_seconds(void) {
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

static unsigned host_processors(void) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return (unsigned)info.dwNumberOfProcessors;
}

static void yield_thread(void) { SwitchToThread(); }

typedef struct GcWake {
  HANDLE event;
} GcWake;
static void wake_init(GcWake *w) { w->event = CreateEventA(NULL, FALSE, FALSE, NULL); }
static void wake_wait(GcWake *w) { WaitForSingleObject(w->event, INFINITE); }
static void wake_signal(GcWake *w) { SetEvent(w->event); }
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

typedef volatile long GcAtomic;
#define ATOMIC_INC(p) __sync_add_and_fetch((p), 1)
#define ATOMIC_DEC(p) __sync_sub_and_fetch((p), 1)
#define ATOMIC_CAS(p, want, from) __sync_bool_compare_and_swap((p), (from), (want))
#define ATOMIC_SET(p, value) ((void)__sync_lock_test_and_set((p), (value)))

static double now_seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static unsigned host_processors(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (unsigned)n : 1u;
}

static void yield_thread(void) { sched_yield(); }

typedef struct GcWake {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  int signalled;
} GcWake;
static void wake_init(GcWake *w) {
  pthread_mutex_init(&w->lock, NULL);
  pthread_cond_init(&w->cond, NULL);
  w->signalled = 0;
}
static void wake_wait(GcWake *w) {
  pthread_mutex_lock(&w->lock);
  while (!w->signalled) pthread_cond_wait(&w->cond, &w->lock);
  w->signalled = 0;
  pthread_mutex_unlock(&w->lock);
}
static void wake_signal(GcWake *w) {
  pthread_mutex_lock(&w->lock);
  w->signalled = 1;
  pthread_cond_signal(&w->cond);
  pthread_mutex_unlock(&w->lock);
}
#endif

/* The window the share of time in jobs is measured over, the share at
 * which every helper spins, and the work (one thread's seconds) a job needs
 * for sleeping helpers to be woken for it. */
#define GC_DUTY_WINDOW 0.05
#define GC_FULL_DUTY 0.4
#define GC_WAKE_WORK 0.001
/* Job functions whose work is remembered. */
#define GC_FN_SLOTS 8u

typedef struct GcHelper {
  GcAtomic sleeping;
  GcWake wake;
} GcHelper;

typedef struct GcFnStat {
  GcWorkFn fn;
  double work; /* seconds of one thread's work a job, averaged */
  double used_at;
} GcFnStat;

static struct {
  GcAtomic started; /* 0 not yet, 1 starting, 2 ready */
  unsigned count;
  double spin_seconds;
  int spin_all; /* GC_RASTER_SPIN_ALL: every helper spins and is woken for every job */
  GcAtomic generation;
  GcAtomic closed; /* the generation whose job is finished */
  GcAtomic inside; /* helpers between joining a job and leaving it */
  GcAtomic busy;
  GcAtomic useful;   /* helpers that took a piece of the current job */
  GcAtomic spinners; /* helpers 1 to this may spin */
  GcWorkFn fn;
  void *job;
  GcFpEnv env;
  GcHelper helper[GC_MAX_HELPERS + 1u];
  /* The dispatcher's alone. */
  GcFnStat stat[GC_FN_SLOTS];
  double window_start, window_busy;
} pool;

/* Pieces this thread has asked for (gc_workers_take): a helper that asked
 * once found none left. */
static GC_THREAD_LOCAL long takes;

static void helper_loop(unsigned id) {
  GcHelper *self = &pool.helper[id];
  /* Every helper starts before the first job is announced, at generation
   * zero - so zero, not whatever the counter reads by the time this thread
   * is scheduled, which could already be the first job. */
  long seen = 0;
  double idle_since = now_seconds();
  for (;;) {
    long generation = pool.generation;
    if (generation != seen) {
      seen = generation;
      /* Join, then look whether the job is still the current one and still
       * open. The caller closes a job before it counts who is inside, and
       * announces the next only after that count reaches zero, so either
       * this helper sees the job it was woken for, open, and the caller
       * waits for it - or it leaves. A helper held up long enough to miss a
       * job entirely must not run the one announced after it, whose
       * description the caller may still be writing, nor one already
       * finished. */
      ATOMIC_INC(&pool.inside);
      if (pool.generation == generation && pool.closed != generation) {
        fp_env_set(pool.env);
        takes = 0;
        pool.fn(pool.job, id);
        if (takes > 1) ATOMIC_INC(&pool.useful);
      }
      ATOMIC_DEC(&pool.inside);
      idle_since = now_seconds();
      continue;
    }
    if ((long)id <= pool.spinners) {
      unsigned i;
      for (i = 0; i < 256u && pool.generation == seen; i++) GC_PAUSE();
      if (pool.generation != seen) continue;
      if (now_seconds() - idle_since < pool.spin_seconds) continue;
    }
    /* Asleep - but only after looking once more, so a job announced between
     * the look and the flag is not missed: the dispatcher bumps the
     * generation before it reads the flag. */
    ATOMIC_INC(&self->sleeping);
    if (pool.generation == seen) wake_wait(&self->wake);
    ATOMIC_DEC(&self->sleeping);
    idle_since = now_seconds();
  }
}

#ifdef _WIN32
#ifdef _MSC_VER
#pragma warning(disable : 4702) /* the return below: a helper never leaves its loop */
#endif
static DWORD WINAPI helper_main(LPVOID arg) {
  helper_loop((unsigned)(uintptr_t)arg);
  return 0;
}
#else
static void *helper_main(void *arg) {
  helper_loop((unsigned)(uintptr_t)arg);
  return NULL;
}
#endif

static void start_pool(void) {
  const char *text = getenv("GC_RASTER_THREADS");
  const char *spin = getenv("GC_RASTER_SPIN_MS");
  unsigned processors = host_processors();
  unsigned count = processors > 1u ? processors - 1u : 0u;
  unsigned i;
  if (count > GC_MAX_HELPERS) count = GC_MAX_HELPERS;
  if (text && *text) {
    long wanted = strtol(text, NULL, 10);
    count = wanted <= 0 ? 0u : (wanted > (long)GC_MAX_HELPERS ? GC_MAX_HELPERS : (unsigned)wanted);
  }
  pool.spin_seconds = (spin && *spin ? strtod(spin, NULL) : 4.0) / 1000.0;
  {
    const char *all = getenv("GC_RASTER_SPIN_ALL");
    pool.spin_all = all && *all && *all != '0';
  }
  for (i = 1; i <= count; i++) {
    wake_init(&pool.helper[i].wake);
#ifdef _WIN32
    {
      HANDLE thread = CreateThread(NULL, 0, helper_main, (LPVOID)(uintptr_t)i, 0, NULL);
      if (!thread) break;
      CloseHandle(thread);
    }
#else
    {
      pthread_t thread;
      if (pthread_create(&thread, NULL, helper_main, (void *)(uintptr_t)i) != 0) break;
      pthread_detach(thread);
    }
#endif
  }
  pool.count = i - 1u;
  pool.spinners = (long)pool.count;
  pool.window_start = now_seconds();
}

unsigned gc_workers_count(void) {
  if (pool.started != 2) {
    if (ATOMIC_CAS(&pool.started, 1, 0)) {
      start_pool();
      pool.started = 2;
    } else {
      return 0; /* another thread is starting it; do this one alone */
    }
  }
  return pool.count;
}

/* The remembered work of fn's jobs; a function not seen before (or pushed
 * out) starts as worth waking for. */
static GcFnStat *stat_for(GcWorkFn fn, double now) {
  GcFnStat *oldest = &pool.stat[0];
  unsigned i;
  for (i = 0; i < GC_FN_SLOTS; i++) {
    GcFnStat *const s = &pool.stat[i];
    if (s->fn == fn) return s;
    if (s->used_at < oldest->used_at) oldest = s;
  }
  oldest->fn = fn;
  oldest->work = 1.0;
  oldest->used_at = now;
  return oldest;
}

int gc_workers_run(GcWorkFn fn, void *job) {
  long generation;
  unsigned i;
  double started;
  GcFnStat *stat;
  if (!gc_workers_count() || !ATOMIC_CAS(&pool.busy, 1, 0)) return -1;
  started = now_seconds();
  stat = stat_for(fn, started);
  stat->used_at = started;
  pool.fn = fn;
  pool.job = job;
  pool.env = fp_env_get();
  pool.useful = 0;
  generation = ATOMIC_INC(&pool.generation);
  {
    /* Helpers allowed to spin are woken when they have dozed off; the rest
     * only for a job with work enough for them. */
    const unsigned wake = stat->work >= GC_WAKE_WORK || pool.spin_all ? pool.count : (unsigned)pool.spinners;
    for (i = 1; i <= wake; i++) {
      if (pool.helper[i].sleeping) wake_signal(&pool.helper[i].wake);
    }
  }
  fn(job, 0);
  /* The caller has drawn every chunk no helper took. Helpers still asleep,
   * or not yet scheduled, are not waited for: closing the job turns them
   * away.
   *
   * Closing is a locked write, not an assignment. A helper counts itself in
   * (a locked increment) and then reads `closed`; this thread writes
   * `closed` and then reads `inside`. A plain store can sit in the
   * processor's store buffer after this thread has already read `inside`,
   * so the helper could count itself in too late to be seen and still read
   * the job as open, and run it after this function had returned and the
   * job on the caller's stack was gone. Windowed 90-second Shrek 2 runs
   * crashed that way in draw_chunks, on a helper, four times in five. A
   * locked write is committed before the next instruction, which is what
   * the two-sided handshake needs. */
  ATOMIC_SET(&pool.closed, generation);
  {
    unsigned spins = 0;
    while (pool.inside != 0) {
      if (++spins < 4096u) GC_PAUSE();
      else yield_thread();
    }
  }
  {
    /* Every helper that ran this job has left, so `useful` is final. */
    const double ended = now_seconds();
    const double took = ended - started;
    const long useful = pool.useful;
    stat->work += (took * (double)(useful + 1) - stat->work) * 0.125;
    pool.window_busy += took;
    if (ended - pool.window_start >= GC_DUTY_WINDOW) {
      const double duty = pool.window_busy / (ended - pool.window_start);
      long spinners = (long)((double)pool.count * duty / GC_FULL_DUTY + 0.999);
      if (spinners > (long)pool.count || pool.spin_all) spinners = (long)pool.count;
      if (spinners < 1) spinners = 1;
      pool.spinners = spinners;
      pool.window_start = ended;
      pool.window_busy = 0.0;
    }
  }
  pool.busy = 0;
  return 0;
}

long gc_workers_take(long *counter) {
  takes++;
#ifdef _WIN32
  return (long)InterlockedIncrement((LONG *)counter) - 1;
#else
  return __sync_add_and_fetch(counter, 1) - 1;
#endif
}
