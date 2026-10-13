/*
 * Opens a per-processor template section: from NDS_ARCH (5 for the ARM9,
 * 4 for the ARM7) defines NDS_V5, a compile-time 0 or 1, and NDS_FN(name),
 * which prefixes a name with nds9_ or nds7_. nds_cpu_arch_end.inc closes
 * the section. Deliberately has no include guard.
 */
#ifndef NDS_ARCH
#error "define NDS_ARCH as 4 (ARM7) or 5 (ARM9) first"
#endif

#if NDS_ARCH == 5
#define NDS_V5 1
#define NDS_FN(name) nds9_##name
#elif NDS_ARCH == 4
#define NDS_V5 0
#define NDS_FN(name) nds7_##name
#else
#error "NDS_ARCH must be 4 or 5"
#endif
