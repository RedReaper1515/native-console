/*
 * The one place the generated opcode metadata is defined.
 *
 * gc_cpu_decode.h is header-only: the decoder inlines into every caller,
 * and the gc_op_info table appears only where GC_CPU_DECODE_IMPLEMENTATION
 * is defined. Giving it a translation unit of its own keeps that macro away
 * from include order, where defining it after something else had already
 * pulled the header in would silently leave the table undefined.
 */

#define GC_CPU_DECODE_IMPLEMENTATION
#include "gc_cpu_decode.h"
