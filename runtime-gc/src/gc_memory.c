/*
 * Main memory with a second view, page holds and page tracking (gc_memory.h).
 */
#include "gc_memory.h"
#include "gc_perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SHIFT 12u
#define PAGE_BYTES ((size_t)1 << PAGE_SHIFT)

volatile long long gc_memory_generation;
static volatile long long tags;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t gc_memory_next_tag(void) { return (uint64_t)InterlockedIncrement64(&tags); }

#define HELD_RANGES 256u

/* What a page of the machine's view is set to, as this file last set it. */
enum { PROT_RW = 0, PROT_RO = 1, PROT_NONE = 2 };

typedef struct GcHeldRange {
  size_t first, end;
  uint64_t tag; /* 0: forgotten when none of its pages is held */
} GcHeldRange;

typedef struct GcMemory {
  uint8_t *base;
  uint8_t *alias;
  size_t size;
  HANDLE mapping;
  uint16_t *holds;  /* a count a page */
  uint8_t *tracked; /* a page: unwritten since it was last tracked (the machine's thread's) */
  uint8_t *prot;    /* a page: PROT_* (under the lock) */
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE settled; /* a hold released */
  volatile LONG64 waits;
  double wait_ms; /* under the lock */
  /* The byte ranges holds were taken for, while they are open (under the
   * lock). One that cannot be recorded makes gc_memory_view_for answer
   * NULL and stepping stop. */
  GcHeldRange ranges[HELD_RANGES];
  volatile LONG range_count;
  int ranges_overflowed;
  DWORD holder;   /* the thread that last took a hold: the machine's */
  uint64_t steps; /* accesses run with their page opened (under the lock) */
  uint64_t track_faults, track_protects; /* under the lock */
  struct GcMemory *next;
} GcMemory;

static SRWLOCK regions_lock = SRWLOCK_INIT;
static GcMemory *regions;
static volatile LONG handler_installed;

/* The block whose *machine's* view contains `address`: what a fault asks. */
static GcMemory *find_base(const uint8_t *address) {
  GcMemory *r;
  AcquireSRWLockShared(&regions_lock);
  for (r = regions; r; r = r->next) {
    if (address >= r->base && address < r->base + r->size) break;
  }
  ReleaseSRWLockShared(&regions_lock);
  return r;
}

/* The block a caller names by either view's start. */
static GcMemory *region_of(const uint8_t *base) {
  GcMemory *r;
  if (!base) return NULL;
  AcquireSRWLockShared(&regions_lock);
  for (r = regions; r; r = r->next) {
    if (base == r->base || base == r->alias) break;
  }
  ReleaseSRWLockShared(&regions_lock);
  return r;
}

/* Under the lock: what a page should be set to now. */
static int wanted(const GcMemory *r, size_t page) {
  if (r->holds[page]) return PROT_NONE;
  if (r->tracked[page]) return PROT_RO;
  return PROT_RW;
}

static DWORD protection_of(int prot) {
  return prot == PROT_NONE ? PAGE_NOACCESS : (prot == PROT_RO ? PAGE_READONLY : PAGE_READWRITE);
}

/* Under the lock: set pages [from, to) to `prot`, in one call. */
static void set_run(GcMemory *r, size_t from, size_t to, int prot) {
  DWORD old;
  size_t page;
  if (to <= from) return;
  VirtualProtect(r->base + (from << PAGE_SHIFT), (to - from) << PAGE_SHIFT, protection_of(prot), &old);
  for (page = from; page < to; page++) r->prot[page] = (uint8_t)prot;
  if (prot == PROT_RO) r->track_protects += to - from;
}

/* Under the lock: every page of [first, last] set to what it should be,
 * in runs, calling the system only where a setting changes. */
static void apply(GcMemory *r, size_t first, size_t last) {
  size_t page, run = first;
  int run_prot = -1;
  for (page = first; page <= last + 1u; page++) {
    const int want = page <= last ? wanted(r, page) : -2;
    const int differs = page <= last && want != r->prot[page];
    if (run_prot >= 0 && (!differs || want != run_prot)) {
      set_run(r, run, page, run_prot);
      run_prot = -1;
    }
    if (differs && run_prot < 0) {
      run = page;
      run_prot = want;
    }
  }
}

/* How far one host instruction's access can reach from the address it
 * faulted at, either way: the widest vector store or load, and one element
 * of a string instruction (which traps after each element when stepped). */
#define STEP_REACH 64u
#define STEP_PAGES 4u

volatile LONG gc_memory_shared_readers;

/* The pages this thread opened for the one instruction it is stepping. */
static __declspec(thread) struct {
  GcMemory *region[STEP_PAGES];
  size_t page[STEP_PAGES];
  unsigned count;
} stepping;

/* Whether this thread reads main memory only through the second view. */
static __declspec(thread) int second_view_only;

/* How many bytes of memory the instruction at `code` touches, when that can
 * be told: a general-purpose integer instruction with a memory operand - a
 * move, a widening move, a byte-swapping move, arithmetic, a compare, a
 * test, one element of a string instruction (which traps after each element
 * when stepped) - by its operand size. Anything else (vector moves, and
 * whatever is not recognised here) is given the widest reach there is. */
static unsigned access_reach(const uint8_t *code) {
  unsigned at = 0, op, wide;
  int rep = 0, narrow = 0, rex_w = 0;
  for (;;) {
    const uint8_t b = code[at];
    if (b == 0x66u) {
      narrow = 1;
      at++;
    } else if (b == 0x67u) {
      at++;
    } else if (b == 0xf2u || b == 0xf3u) {
      rep = 1;
      at++;
    } else {
      break;
    }
    if (at > 4u) return STEP_REACH;
  }
  if ((code[at] & 0xf0u) == 0x40u) { /* REX */
    rex_w = (code[at] & 8u) != 0;
    at++;
  }
  op = code[at];
  /* What an instruction on "the operand size" moves. */
  wide = rex_w ? 8u : (narrow ? 2u : 4u);
  if ((op >= 0xa4u && op <= 0xa7u) || (op >= 0xaau && op <= 0xafu)) return (op & 1u) ? wide : 1u; /* strings */
  if (rep) return STEP_REACH;
  if (op <= 0x3fu && (op & 7u) <= 3u) return (op & 1u) ? wide : 1u; /* add, or, adc, sbb, and, sub, xor, cmp */
  if (op >= 0x80u && op <= 0x8bu) return (op & 1u) ? wide : 1u;      /* immediate groups, test, xchg, mov */
  if (op == 0xc6u || op == 0xf6u || op == 0xfeu) return 1u;
  if (op == 0xc7u || op == 0xf7u) return wide;
  if (op == 0x0fu) {
    const uint8_t next = code[at + 1u];
    if (next == 0xb6u || next == 0xbeu) return 1u; /* movzx, movsx from a byte */
    if (next == 0xb7u || next == 0xbfu) return 2u; /* ... from a word */
    if (next == 0x38u && (code[at + 2u] == 0xf0u || code[at + 2u] == 0xf1u)) return wide; /* movbe */
  }
  return STEP_REACH;
}

unsigned gc_memory_access_reach(const uint8_t *code) { return access_reach(code); }

/* Under the lock: whether the thread that faulted at `offset` may run that
 * one instruction with its page opened, instead of waiting for the copy.
 * Only when no held range comes within the instruction's reach of it - the
 * bytes it touches are then none of a pending copy's, which read and write
 * the same now as after the copy lands - and only on the thread that holds
 * pages (the machine's), while nothing else may be reading the machine's
 * view through the opened page: no helper job is running
 * (gc_memory_shared_begin). */
static int may_step(const GcMemory *r, size_t offset, const uint8_t *code) {
  /* The bytes it can touch. The address a fault reports is the first byte
   * of the access that lies in a closed page: where the access starts, when
   * it starts in the page - and only at the very start of a page can the
   * access have begun before it, in the page below. */
  const unsigned reach = access_reach(code);
  const size_t before = (offset & (PAGE_BYTES - 1u)) ? 0u : reach - 1u;
  const size_t low = offset > before ? offset - before : 0u;
  const size_t high = offset + reach;
  unsigned i;
  if (GetCurrentThreadId() != r->holder || gc_memory_shared_readers != 0 || r->ranges_overflowed ||
      stepping.count >= STEP_PAGES) {
    return 0;
  }
  for (i = 0; i < (unsigned)r->range_count; i++) {
    if (r->ranges[i].first < high && low < r->ranges[i].end) return 0;
  }
  return 1;
}

/* An access to a held page: run it now when it cannot touch a pending
 * copy's bytes (may_step: the page is opened, the instruction runs with the
 * trap flag set, and the page is closed again on the trap if it is still
 * held), and otherwise wait for its holds to be released and run the
 * instruction again. A write to a tracked page: the page is no longer
 * tracked, and opened. Anything else is somebody else's fault. */
static LONG CALLBACK on_access(EXCEPTION_POINTERS *info) {
  const EXCEPTION_RECORD *record = info->ExceptionRecord;
  const uint8_t *address;
  GcMemory *r;
  size_t page;
  int held, tracked = 0;
  if (record->ExceptionCode == EXCEPTION_SINGLE_STEP) {
    unsigned i;
    if (!stepping.count) return EXCEPTION_CONTINUE_SEARCH;
    for (i = 0; i < stepping.count; i++) {
      GcMemory *const opened = stepping.region[i];
      EnterCriticalSection(&opened->lock);
      apply(opened, stepping.page[i], stepping.page[i]);
      LeaveCriticalSection(&opened->lock);
    }
    stepping.count = 0;
    info->ContextRecord->EFlags &= ~0x100u;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  address = (const uint8_t *)record->ExceptionInformation[1];
  r = find_base(address);
  if (!r) return EXCEPTION_CONTINUE_SEARCH;
  page = (size_t)(address - r->base) >> PAGE_SHIFT;
  EnterCriticalSection(&r->lock);
  held = r->holds[page] != 0;
  if (held && may_step(r, (size_t)(address - r->base), (const uint8_t *)record->ExceptionAddress)) {
    /* The instruction may write the page: it is not tracked any more. */
    r->tracked[page] = 0;
    set_run(r, page, page + 1u, PROT_RW);
    stepping.region[stepping.count] = r;
    stepping.page[stepping.count] = page;
    stepping.count++;
    info->ContextRecord->EFlags |= 0x100u;
    r->steps++;
    gc_perf.page_steps++;
    LeaveCriticalSection(&r->lock);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (held) {
    LARGE_INTEGER a, b, f;
    InterlockedIncrement64(&r->waits);
    QueryPerformanceCounter(&a);
    while (r->holds[page]) SleepConditionVariableCS(&r->settled, &r->lock, INFINITE);
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    r->wait_ms += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    gc_perf.page_wait_ms += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
  } else if (r->prot[page] == PROT_RO) {
    /* A write to a tracked page (a read never faults on one). */
    tracked = 1;
    r->tracked[page] = 0;
    set_run(r, page, page + 1u, PROT_RW);
    r->track_faults++;
    gc_perf.watch_faults++;
  }
  LeaveCriticalSection(&r->lock);
  if (!held && !tracked) {
    /* Released or opened between the access and here - or not a page anybody
     * closed, which would fault again for ever: tell the two apart. */
    MEMORY_BASIC_INFORMATION about;
    const int write = record->ExceptionInformation[0] == 1;
    if (VirtualQuery(address, &about, sizeof(about)) &&
        ((about.Protect & PAGE_NOACCESS) || (write && (about.Protect & PAGE_READONLY)))) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

uint8_t *gc_memory_create(size_t size) {
  GcMemory *r;
  const size_t pages = (size + PAGE_BYTES - 1u) >> PAGE_SHIFT;
  if (!size || (size & (PAGE_BYTES - 1u))) return NULL;
  r = (GcMemory *)calloc(1, sizeof(GcMemory));
  if (!r) return NULL;
  r->size = size;
  r->holds = (uint16_t *)calloc(pages, sizeof(uint16_t));
  r->tracked = (uint8_t *)calloc(pages, 1u);
  r->prot = (uint8_t *)calloc(pages, 1u);
  r->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32),
                                  (DWORD)size, NULL);
  if (r->mapping) {
    r->base = (uint8_t *)MapViewOfFile(r->mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    r->alias = (uint8_t *)MapViewOfFile(r->mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
  }
  if (!r->holds || !r->tracked || !r->prot || !r->base || !r->alias) {
    if (r->alias) UnmapViewOfFile(r->alias);
    if (r->base) UnmapViewOfFile(r->base);
    if (r->mapping) CloseHandle(r->mapping);
    free(r->holds);
    free(r->tracked);
    free(r->prot);
    free(r);
    return NULL;
  }
  InitializeCriticalSection(&r->lock);
  InitializeConditionVariable(&r->settled);
  if (InterlockedCompareExchange(&handler_installed, 1, 0) == 0) AddVectoredExceptionHandler(1, on_access);
  AcquireSRWLockExclusive(&regions_lock);
  r->next = regions;
  regions = r;
  ReleaseSRWLockExclusive(&regions_lock);
  return r->base;
}

void gc_memory_destroy(uint8_t *base) {
  GcMemory **at, *r = NULL;
  if (!base) return;
  AcquireSRWLockExclusive(&regions_lock);
  for (at = &regions; *at; at = &(*at)->next) {
    if ((*at)->base == base) {
      r = *at;
      *at = r->next;
      break;
    }
  }
  ReleaseSRWLockExclusive(&regions_lock);
  if (!r) return;
  UnmapViewOfFile(r->alias);
  UnmapViewOfFile(r->base);
  CloseHandle(r->mapping);
  DeleteCriticalSection(&r->lock);
  free(r->holds);
  free(r->tracked);
  free(r->prot);
  free(r);
}

uint8_t *gc_memory_alias(const uint8_t *base) {
  const GcMemory *r = region_of(base);
  return r ? r->alias : NULL;
}

static int page_range(const GcMemory *r, size_t offset, size_t size, size_t *first, size_t *last) {
  if (!size || offset >= r->size) return 0;
  if (size > r->size - offset) size = r->size - offset;
  *first = offset >> PAGE_SHIFT;
  *last = (offset + size - 1u) >> PAGE_SHIFT;
  return 1;
}

int gc_memory_hold_tagged(uint8_t *base, size_t offset, size_t size, uint64_t tag) {
  GcMemory *r = region_of(base);
  size_t first, last, page;
  if (!r) return -1;
  if (!page_range(r, offset, size, &first, &last)) return 0;
  EnterCriticalSection(&r->lock);
  /* The copy writes these pages through the second view, where no fault
   * sees it: they are not tracked any more. (Holds are taken on the
   * machine's thread, the only one that marks pages.) */
  for (page = first; page <= last; page++) {
    r->holds[page]++;
    r->tracked[page] = 0;
  }
  apply(r, first, last);
  r->holder = GetCurrentThreadId();
  if ((unsigned)r->range_count < HELD_RANGES) {
    r->ranges[r->range_count].first = offset;
    r->ranges[r->range_count].end = offset + size;
    r->ranges[r->range_count].tag = tag;
    InterlockedIncrement(&r->range_count);
  } else {
    r->ranges_overflowed = 1;
  }
  InterlockedIncrement64(&gc_memory_generation);
  LeaveCriticalSection(&r->lock);
  return 0;
}

int gc_memory_hold(uint8_t *base, size_t offset, size_t size) {
  return gc_memory_hold_tagged(base, offset, size, 0u);
}

/* Under the lock: whether any page of [first, end) is still held. */
static int any_page_held(const GcMemory *r, size_t first, size_t end) {
  size_t page;
  if (end <= first) return 0;
  for (page = first >> PAGE_SHIFT; page <= (end - 1u) >> PAGE_SHIFT && page < (r->size >> PAGE_SHIFT); page++) {
    if (r->holds[page]) return 1;
  }
  return 0;
}

/* Under the lock: whether a pending hold covers any byte of [offset, offset +
 * size) - by the byte ranges holds were taken for, or by pages when they
 * could not all be recorded. A held page whose held bytes lie elsewhere (a
 * copy into the buffer beside this one, sharing its last page) does not. */
static int bytes_held(const GcMemory *r, size_t offset, size_t size) {
  unsigned i;
  if (!any_page_held(r, offset, offset + size)) return 0;
  if (r->ranges_overflowed) return 1;
  for (i = 0; i < (unsigned)r->range_count; i++) {
    if (r->ranges[i].first < offset + size && offset < r->ranges[i].end) return 1;
  }
  return 0;
}

uint8_t *gc_memory_view_for(uint8_t *base, size_t offset, size_t size) {
  GcMemory *r = region_of(base);
  size_t first, last;
  uint8_t *view = base;
  if (!r || !page_range(r, offset, size, &first, &last)) return base;
  EnterCriticalSection(&r->lock);
  if (any_page_held(r, offset, offset + size)) view = bytes_held(r, offset, size) ? NULL : r->alias;
  LeaveCriticalSection(&r->lock);
  return view;
}

void gc_memory_release_tagged(uint8_t *base, size_t offset, size_t size, uint64_t tag, int final) {
  GcMemory *r = region_of(base);
  size_t first, last, page;
  if (!r) return;
  EnterCriticalSection(&r->lock);
  if (page_range(r, offset, size, &first, &last)) {
    for (page = first; page <= last; page++) {
      if (r->holds[page]) r->holds[page]--;
    }
    apply(r, first, last);
  }
  {
    /* A tagged range is forgotten on its final release; an untagged one once
     * none of its pages is held. */
    unsigned i, kept = 0;
    const unsigned count = (unsigned)r->range_count;
    for (i = 0; i < count; i++) {
      const GcHeldRange *const range = &r->ranges[i];
      const int drop = range->tag ? (final && range->tag == tag) : !any_page_held(r, range->first, range->end);
      if (!drop) r->ranges[kept++] = *range;
    }
    r->range_count = (LONG)kept;
    if (!kept && r->ranges_overflowed && !any_page_held(r, 0, r->size)) r->ranges_overflowed = 0;
  }
  InterlockedIncrement64(&gc_memory_generation);
  WakeAllConditionVariable(&r->settled);
  LeaveCriticalSection(&r->lock);
}

void gc_memory_release(uint8_t *base, size_t offset, size_t size) {
  gc_memory_release_tagged(base, offset, size, 0u, 0);
}

uint64_t gc_memory_held_at(const uint8_t *base, size_t offset, size_t *end) {
  GcMemory *r = region_of(base);
  uint64_t tag = 0;
  size_t next = (size_t)-1;
  unsigned i;
  if (!r) {
    *end = (size_t)-1;
    return 0;
  }
  EnterCriticalSection(&r->lock);
  next = r->size;
  if (!r->ranges_overflowed) {
    for (i = 0; i < (unsigned)r->range_count; i++) {
      const GcHeldRange *const range = &r->ranges[i];
      if (!range->tag) continue;
      if (range->tag > tag && range->first <= offset && offset < range->end) {
        tag = range->tag;
        *end = range->end;
      }
      if (range->first > offset && range->first < next) next = range->first;
    }
  }
  if (!tag) *end = next;
  LeaveCriticalSection(&r->lock);
  return tag;
}

int gc_memory_held(const uint8_t *base, size_t offset, size_t size) {
  GcMemory *r = region_of(base);
  size_t first, last, page;
  int held = 0;
  if (!r || !page_range(r, offset, size, &first, &last)) return 0;
  EnterCriticalSection(&r->lock);
  for (page = first; page <= last && !held; page++) held = r->holds[page] != 0;
  LeaveCriticalSection(&r->lock);
  return held;
}

int gc_memory_wait_released(const uint8_t *base, size_t offset, size_t size, double timeout_ms) {
  GcMemory *r = region_of(base);
  size_t first, last;
  int held;
  LARGE_INTEGER start, now, freq;
  if (!r || !page_range(r, offset, size, &first, &last)) return 1;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  EnterCriticalSection(&r->lock);
  for (;;) {
    double left;
    held = bytes_held(r, offset, size);
    if (!held) break;
    if (timeout_ms < 0.0) {
      SleepConditionVariableCS(&r->settled, &r->lock, INFINITE);
      continue;
    }
    QueryPerformanceCounter(&now);
    left = timeout_ms - (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
    if (left <= 0.0) break;
    SleepConditionVariableCS(&r->settled, &r->lock, left < 1.0 ? 1u : (DWORD)left);
  }
  LeaveCriticalSection(&r->lock);
  return !held;
}

uint64_t gc_memory_waits(const uint8_t *base) {
  const GcMemory *r = region_of(base);
  return r ? (uint64_t)r->waits : 0u;
}

uint64_t gc_memory_steps(const uint8_t *base) {
  const GcMemory *r = region_of(base);
  return r ? r->steps : 0u;
}

void gc_memory_shared_begin(void) {
  if (!second_view_only) InterlockedIncrement(&gc_memory_shared_readers);
}
void gc_memory_shared_end(void) {
  if (!second_view_only) InterlockedDecrement(&gc_memory_shared_readers);
}
int gc_memory_shared_end_with(int result) {
  gc_memory_shared_end();
  return result;
}
void gc_memory_second_view_thread(void) { second_view_only = 1; }

double gc_memory_wait_ms(const uint8_t *base) {
  const GcMemory *r = region_of(base);
  return r ? r->wait_ms : 0.0;
}

void gc_memory_settle(const uint8_t *base, size_t offset, size_t size) {
  GcMemory *r = region_of(base);
  size_t first, last, page;
  if (!r || !page_range(r, offset, size, &first, &last)) return;
  EnterCriticalSection(&r->lock);
  for (;;) {
    int held = 0;
    for (page = first; page <= last && !held; page++) held = r->holds[page] != 0;
    if (!held) break;
    InterlockedIncrement64(&r->waits);
    SleepConditionVariableCS(&r->settled, &r->lock, INFINITE);
  }
  /* Tracked pages opened, as a write from the processor would open them. */
  for (page = first; page <= last; page++) r->tracked[page] = 0;
  apply(r, first, last);
  LeaveCriticalSection(&r->lock);
}

/* ------------------------------------------------------------------ */
/* Tracking                                                            */

const uint8_t *gc_memory_tracking(const uint8_t *base) {
  const GcMemory *r = region_of(base);
  return r ? r->tracked : NULL;
}

void gc_memory_track(uint8_t *base, size_t first_page, size_t last_page) {
  GcMemory *r = region_of(base);
  size_t page;
  int closed = 1;
  if (!r || first_page > last_page || last_page >= (r->size >> PAGE_SHIFT)) return;
  /* Only this thread (the machine's) marks pages tracked or finds them
   * written, and a page's setting is decided under the lock from the marks;
   * so the marks go first and the lock is taken only when some page is not
   * already read-only. A read-only page is not held, so no release can open
   * it. */
  for (page = first_page; page <= last_page; page++) {
    r->tracked[page] = 1;
    if (r->prot[page] != PROT_RO) closed = 0;
  }
  if (closed) return;
  EnterCriticalSection(&r->lock);
  apply(r, first_page, last_page);
  LeaveCriticalSection(&r->lock);
}

void gc_memory_untrack_all(uint8_t *base) {
  GcMemory *r = region_of(base);
  const size_t pages = r ? r->size >> PAGE_SHIFT : 0u;
  if (!r) return;
  EnterCriticalSection(&r->lock);
  memset(r->tracked, 0, pages);
  apply(r, 0, pages - 1u);
  LeaveCriticalSection(&r->lock);
}

void gc_memory_track_stats(const uint8_t *base, uint64_t *faults, uint64_t *protects) {
  GcMemory *r = region_of(base);
  *faults = *protects = 0;
  if (!r) return;
  EnterCriticalSection(&r->lock);
  *faults = r->track_faults;
  *protects = r->track_protects;
  LeaveCriticalSection(&r->lock);
}

#else

uint64_t gc_memory_next_tag(void) { return (uint64_t)++tags; }

void gc_memory_settle(const uint8_t *base, size_t offset, size_t size) {
  (void)base;
  (void)offset;
  (void)size;
}

uint8_t *gc_memory_create(size_t size) { return (uint8_t *)calloc(1, size); }
void gc_memory_destroy(uint8_t *base) { free(base); }
uint8_t *gc_memory_alias(const uint8_t *base) {
  (void)base;
  return NULL;
}
int gc_memory_hold(uint8_t *base, size_t offset, size_t size) {
  (void)base;
  (void)offset;
  (void)size;
  return -1;
}
void gc_memory_release(uint8_t *base, size_t offset, size_t size) {
  (void)base;
  (void)offset;
  (void)size;
}
int gc_memory_hold_tagged(uint8_t *base, size_t offset, size_t size, uint64_t tag) {
  (void)tag;
  return gc_memory_hold(base, offset, size);
}
void gc_memory_release_tagged(uint8_t *base, size_t offset, size_t size, uint64_t tag, int final) {
  (void)tag;
  (void)final;
  gc_memory_release(base, offset, size);
}
uint64_t gc_memory_held_at(const uint8_t *base, size_t offset, size_t *end) {
  (void)base;
  (void)offset;
  *end = (size_t)-1;
  return 0;
}
int gc_memory_held(const uint8_t *base, size_t offset, size_t size) {
  (void)base;
  (void)offset;
  (void)size;
  return 0;
}
int gc_memory_wait_released(const uint8_t *base, size_t offset, size_t size, double timeout_ms) {
  (void)base;
  (void)offset;
  (void)size;
  (void)timeout_ms;
  return 1;
}
uint8_t *gc_memory_view_for(uint8_t *base, size_t offset, size_t size) {
  (void)offset;
  (void)size;
  return base;
}
const uint8_t *gc_memory_tracking(const uint8_t *base) {
  (void)base;
  return NULL;
}
void gc_memory_track(uint8_t *base, size_t first_page, size_t last_page) {
  (void)base;
  (void)first_page;
  (void)last_page;
}
void gc_memory_untrack_all(uint8_t *base) { (void)base; }
void gc_memory_track_stats(const uint8_t *base, uint64_t *faults, uint64_t *protects) {
  (void)base;
  *faults = *protects = 0;
}
uint64_t gc_memory_waits(const uint8_t *base) {
  (void)base;
  return 0;
}
uint64_t gc_memory_steps(const uint8_t *base) {
  (void)base;
  return 0;
}
unsigned gc_memory_access_reach(const uint8_t *code) {
  (void)code;
  return 0;
}
void gc_memory_shared_begin(void) {}
void gc_memory_shared_end(void) {}
int gc_memory_shared_end_with(int result) { return result; }
void gc_memory_second_view_thread(void) {}
double gc_memory_wait_ms(const uint8_t *base) {
  (void)base;
  return 0.0;
}

#endif
