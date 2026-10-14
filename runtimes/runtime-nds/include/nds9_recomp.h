/*
 * For generated ARM9 code: the ARM9's handlers (nds9_arm_dp and so on) and
 * nds9_rc_step and nds9_rc_refill. See nds_recomp_cpu.inc.
 */
#ifndef NDS9_RECOMP_H
#define NDS9_RECOMP_H

#include "nds_cpu_decode.h"
#include "nds_recomp.h"
#include "nds9_cpu_impl.h"

#define NDS_ARCH 5
#include "nds_recomp_cpu.inc"
#undef NDS_ARCH

#endif /* NDS9_RECOMP_H */
