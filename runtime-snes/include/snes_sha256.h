#ifndef SNES_SHA256_H
#define SNES_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lowercase hex digest of data, NUL-terminated. */
void snes_sha256_hex(const uint8_t *data, size_t size, char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* SNES_SHA256_H */
