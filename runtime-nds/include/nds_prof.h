/*
 * Where the time goes: a section profiler for the DS runtime, compiled in
 * only when NDS_PROFILE is defined (CMake option NDS_PROFILE). Without it
 * the macros below are nothing.
 *
 * NDS_PROF_BEGIN(section) ... NDS_PROF_END() bracket a piece of work.
 * Sections nest: time is charged to the innermost one open, so each
 * section's total is its own time without what it called into. The runner's
 * --profile prints the totals. Process-wide, not per machine: for
 * measurement runs, not for two machines at once (--differential).
 */
#ifndef NDS_PROF_H
#define NDS_PROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NDS_PROF_OTHER,       /* the run loop itself and whatever is not bracketed */
    NDS_PROF_INTERPRET9,  /* interpreted ARM9 instructions */
    NDS_PROF_INTERPRET7,
    NDS_PROF_NATIVE9,     /* recompiled ARM9 code, with the dispatch into it */
    NDS_PROF_NATIVE7,
    NDS_PROF_EVENTS,      /* the scheduler's event handlers, less the ones below */
    NDS_PROF_GEOMETRY,    /* the 3D geometry engine: command writes and execution */
    NDS_PROF_RENDER_3D,   /* the 3D rasteriser */
    NDS_PROF_RENDER_2D,   /* the 2D engines' lines */
    NDS_PROF_SOUND,
    NDS_PROF_IO,          /* other I/O register accesses */
    NDS_PROF_COUNT,
};

#ifdef NDS_PROFILE

#if defined(_MSC_VER)
#include <intrin.h>
#define NDS_PROF_NOW() __rdtsc()
#else
#include <x86intrin.h>
#define NDS_PROF_NOW() __rdtsc()
#endif

typedef struct NdsProf {
    uint64_t ticks[NDS_PROF_COUNT];
    uint64_t entries[NDS_PROF_COUNT];
    uint8_t stack[32];
    int depth;
    uint64_t last;
} NdsProf;

extern NdsProf nds_prof;

static inline void nds_prof_begin(int section)
{
    uint64_t now = NDS_PROF_NOW();
    nds_prof.ticks[nds_prof.stack[nds_prof.depth]] += now - nds_prof.last;
    nds_prof.last = now;
    if (nds_prof.depth < 31) nds_prof.depth++;
    nds_prof.stack[nds_prof.depth] = (uint8_t)section;
    nds_prof.entries[section]++;
}

static inline void nds_prof_end(void)
{
    uint64_t now = NDS_PROF_NOW();
    nds_prof.ticks[nds_prof.stack[nds_prof.depth]] += now - nds_prof.last;
    nds_prof.last = now;
    if (nds_prof.depth > 0) nds_prof.depth--;
}

#define NDS_PROF_BEGIN(section) nds_prof_begin(section)
#define NDS_PROF_END() nds_prof_end()

/* Start counting from now, and print what was counted. */
void nds_prof_reset(void);
void nds_prof_report(void);

#else

#define NDS_PROF_BEGIN(section) ((void)0)
#define NDS_PROF_END() ((void)0)

#endif

#ifdef __cplusplus
}
#endif

#endif /* NDS_PROF_H */
