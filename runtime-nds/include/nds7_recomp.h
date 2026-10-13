/*
 * For generated ARM7 code: the ARM7's handlers (nds7_arm_dp and so on) and
 * nds7_rc_step and nds7_rc_refill. See nds_recomp_cpu.inc.
 */
#ifndef NDS7_RECOMP_H
#define NDS7_RECOMP_H

#include "nds_cpu_decode.h"
#include "nds_recomp.h"
#include "nds7_cpu_impl.h"

#define NDS_ARCH 4
#include "nds_recomp_cpu.inc"
#undef NDS_ARCH

#endif /* NDS7_RECOMP_H */
