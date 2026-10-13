/*
 * The per-field log (gc_perf.h).
 */
#include "gc_perf.h"

#include <string.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <intrin.h>
#include <windows.h>
#endif

#include "gc.h"

GcPerf gc_perf;

double gc_perf_now_ms(void) {
#if defined(_WIN32)
  static double scale;
  LARGE_INTEGER now;
  if (!scale) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    scale = 1000.0 / (double)frequency.QuadPart;
  }
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart * scale;
#else
  struct timespec now;
  timespec_get(&now, TIME_UTC);
  return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
#endif
}

/* The calling thread's own processor time in milliseconds, which other
 * processes competing for the cores do not inflate the way they inflate
 * the wall clock. Windows counts it in processor cycles, which are
 * converted at the rate the time-stamp counter runs against the
 * performance counter; elsewhere the thread's CPU clock. */
static double ms_per_cycle;
#if defined(_WIN32)
double gc_perf_ms_per_tick;
#else
double gc_perf_ms_per_tick = 0.001;
#endif

uint64_t gc_perf_ticks(void) {
#if defined(_WIN32)
  return (uint64_t)__rdtsc();
#else
  return (uint64_t)(gc_perf_now_ms() * 1000.0);
#endif
}

static double thread_cpu_scale(void) { return ms_per_cycle; }

static double thread_cpu_ms(void) {
#if defined(_WIN32)
  ULONG64 cycles = 0;
  if (!ms_per_cycle) {
    const double a = gc_perf_now_ms();
    const unsigned __int64 ta = __rdtsc();
    Sleep(50);
    ms_per_cycle = (gc_perf_now_ms() - a) / (double)(__rdtsc() - ta);
    gc_perf_ms_per_tick = ms_per_cycle;
  }
  QueryThreadCycleTime(GetCurrentThread(), &cycles);
  return (double)cycles * ms_per_cycle;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
  struct timespec now;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
#else
  return 0.0;
#endif
}

static struct {
  FILE *file;
  int pending;         /* a field has ended and its line waits for the next begin */
  double began, ended; /* this field's begin and end */
  double cpu_began, cpu_ended;
  uint64_t field, cycles_before, cycles_after;
  GcPerf before, after;
} field_log;

int gc_perf_field_log_open(const char *path) {
  field_log.file = fopen(path, "w");
  if (!field_log.file) return -1;
  (void)thread_cpu_ms(); /* calibrate now, not inside the first field */
  fprintf(field_log.file,
          "# field wall_ms run_ms cpu_ms instructions interpreted gx_ms gl_ms gl_wait_ms page_wait_ms "
          "prepare_ms rows_ms triangles pixels textures_decoded texture_decode_ms texture_hash_ms readbacks "
          "programs_waited "
          "program_wait_ms disc_reads "
          "disc_bytes disc_ms efb_copies page_steps gx_thread_ms gx_syncs gx_sync_ms watch_faults watch_waits "
          "watch_wait_ms gx_thread_cpu_ms idle_instructions\n"
          "# wall_ms: from this field's start to the next field's start (presenting and pacing included); "
          "run_ms: running the machine; cpu_ms: this thread's processor time while it ran; "
          "idle_instructions: of the field's instructions, those a wait loop counted and did not run - what "
          "the game had to spare\n");
  return 0;
}

static void write_line(double next_begin) {
  const GcPerf *a = &field_log.before, *b = &field_log.after;
  if (!field_log.file || !field_log.pending) return;
  field_log.pending = 0;
  fprintf(field_log.file,
          "%llu %.3f %.3f %.3f %llu %llu %.3f %.3f %.3f %.3f %.3f %.3f %llu %llu %llu %.3f %.3f %llu %llu %.3f %llu %llu %.3f %llu %llu "
          "%.3f %llu %.3f %llu %llu %.3f %.3f %llu\n",
          (unsigned long long)field_log.field, next_begin - field_log.began, field_log.ended - field_log.began,
          field_log.cpu_ended - field_log.cpu_began,
          (unsigned long long)(field_log.cycles_after - field_log.cycles_before),
          (unsigned long long)(b->interpreted - a->interpreted), b->gx_ms - a->gx_ms,
          b->gl_ms - a->gl_ms, b->gl_wait_ms - a->gl_wait_ms, b->page_wait_ms - a->page_wait_ms,
          b->raster_prepare_ms - a->raster_prepare_ms, b->raster_rows_ms - a->raster_rows_ms,
          (unsigned long long)(b->triangles - a->triangles), (unsigned long long)(b->pixels - a->pixels),
          (unsigned long long)(b->textures_decoded - a->textures_decoded),
          b->texture_decode_ms - a->texture_decode_ms, b->texture_hash_ms - a->texture_hash_ms,
          (unsigned long long)(b->readbacks - a->readbacks),
          (unsigned long long)(b->programs_waited - a->programs_waited), b->program_wait_ms - a->program_wait_ms,
          (unsigned long long)(b->disc_reads - a->disc_reads), (unsigned long long)(b->disc_bytes - a->disc_bytes),
          b->disc_ms - a->disc_ms, (unsigned long long)(b->efb_copies - a->efb_copies),
          (unsigned long long)(b->page_steps - a->page_steps), b->gx_thread_ms - a->gx_thread_ms,
          (unsigned long long)(b->gx_syncs - a->gx_syncs), b->gx_sync_ms - a->gx_sync_ms,
          (unsigned long long)(b->watch_faults - a->watch_faults),
          (unsigned long long)(b->watch_waits - a->watch_waits), b->watch_wait_ms - a->watch_wait_ms,
          (double)(b->gx_thread_cycles - a->gx_thread_cycles) * thread_cpu_scale(),
          (unsigned long long)(b->idle_cycles - a->idle_cycles));
}

void gc_perf_field_begin(const GcMachine *m) {
  const double now = gc_perf_now_ms();
  if (!field_log.file) return;
  write_line(now);
  field_log.began = now;
  field_log.cpu_began = thread_cpu_ms();
  field_log.field = m->field;
  field_log.cycles_before = m->cpu.cycles;
  gc_perf.efb_copies = m->efb.copies;
  gc_perf.idle_cycles = gc_idle_cycles;
  field_log.before = gc_perf;
}

void gc_perf_field_end(const GcMachine *m) {
  if (!field_log.file) return;
  field_log.ended = gc_perf_now_ms();
  field_log.cpu_ended = thread_cpu_ms();
  field_log.cycles_after = m->cpu.cycles;
  gc_perf.efb_copies = m->efb.copies;
  gc_perf.idle_cycles = gc_idle_cycles;
  field_log.after = gc_perf;
  field_log.pending = 1;
}
void gc_perf_field_log_close(void) {
  if (!field_log.file) return;
  write_line(gc_perf_now_ms());
  fclose(field_log.file);
  memset(&field_log, 0, sizeof(field_log));
}
