/*
 * The hash that ties a recompiled function to the bytes it was compiled
 * from (see nds_recomp.h): FNV-1a, 64 bits, continued across calls from
 * NDS_RC_HASH_INIT. ndsrecomp computes it when it emits a function and the
 * runtime computes it over guest memory before running one, both from this
 * definition. Valid C and C++.
 */
#ifndef NDS_RC_HASH_H
#define NDS_RC_HASH_H

#include <stdint.h>

#define NDS_RC_HASH_INIT 0xCBF29CE484222325ull

static inline uint64_t nds_rc_hash(uint64_t hash, const uint8_t *bytes, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

#endif /* NDS_RC_HASH_H */
