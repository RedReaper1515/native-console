/*
 * The section profiler's totals and report (see nds_prof.h). Empty unless
 * NDS_PROFILE is defined.
 */
#include "nds_prof.h"

#ifdef NDS_PROFILE

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

NdsProf nds_prof;

void nds_prof_reset(void)
{
    memset(&nds_prof, 0, sizeof nds_prof);
    nds_prof.last = NDS_PROF_NOW();
}

void nds_prof_report(void)
{
    static const char *const names[NDS_PROF_COUNT] = {
        "other (run loop)", "ARM9 interpreted", "ARM7 interpreted", "ARM9 native", "ARM7 native", "events",
        "3D geometry", "3D rasteriser", "2D engines", "sound", "other I/O",
    };
    uint64_t now = NDS_PROF_NOW();
    nds_prof.ticks[nds_prof.stack[nds_prof.depth]] += now - nds_prof.last;
    nds_prof.last = now;
    uint64_t total = 0;
    for (int i = 0; i < NDS_PROF_COUNT; i++) total += nds_prof.ticks[i];
    if (!total) total = 1;
    printf("profile (share of time, times entered):\n");
    for (int i = 0; i < NDS_PROF_COUNT; i++) {
        printf("  %-18s %5.1f%%  %12" PRIu64 "\n", names[i], 100.0 * (double)nds_prof.ticks[i] / (double)total,
               nds_prof.entries[i]);
    }
}

#else

/* ISO C forbids an empty translation unit. */
typedef int nds_prof_unused;

#endif
