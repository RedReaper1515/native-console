#ifndef GBA_SHA256_H
#define GBA_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lowercase hex digest of data, NUL-terminated. */
void gba_sha256(const uint8_t *data, size_t size, uint8_t out[32]);
void gba_sha256_hex(const uint8_t *data, size_t size, char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* GBA_SHA256_H */
