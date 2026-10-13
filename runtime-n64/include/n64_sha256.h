#ifndef N64_SHA256_H
#define N64_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lowercase hex digest of data, NUL-terminated. */
void n64_sha256_hex(const uint8_t *data, size_t size, char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* N64_SHA256_H */
