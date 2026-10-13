/*
 * lift_check.h - what a build of the lift checker is given to check.
 *
 * tools/gb_lift.py writes a table of these next to the lifted sources and
 * links both with the checker.
 */

#ifndef LIFT_CHECK_H
#define LIFT_CHECK_H

#include "gblift.h"

#include <stdint.h>

typedef struct LiftCandidate {
    uint16_t bank;             /* ROM bank of the original subroutine */
    uint16_t address;          /* its entry address */
    GBLiftFunction function;   /* the lifted source */
    const char* name;
    uint32_t cover_low;        /* ROM offsets of the original's first... */
    uint32_t cover_high;       /* ...and last byte, for the coverage report */
    const char* ignore;        /* registers and flags not compared, e.g. "fh,fn" */
    uint8_t called;            /* something is known to call or enter it: it is code */
    /* Its signature, when it is held to one: registers and flags as the
     * LIFT_REG_ and LIFT_FLAG_ bits of gblift.h. What it does not take is
     * given another value before the source runs; what it keeps is put back
     * as it came once the source has returned; what it neither keeps nor
     * gives is not compared. */
    uint8_t held;
    uint16_t takes;
    uint16_t gives;
    uint16_t keeps;
} LiftCandidate;

extern const LiftCandidate lift_candidates[];
extern const unsigned lift_candidate_count;

#endif /* LIFT_CHECK_H */
