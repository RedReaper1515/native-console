/*
 * The two ciphers a Wii disc is built on: AES-128 and SHA-1.
 *
 * A Wii disc's game partition is encrypted cluster by cluster with
 * AES-128 in CBC mode and carries SHA-1 hashes of its own contents
 * (wii_disc.c). Reading one therefore needs exactly these two, and
 * nothing else of a cryptography library - so they are written here, from
 * the standards, in portable C with no intrinsics:
 *
 *   AES     FIPS-197, "Advanced Encryption Standard"
 *   CBC     NIST SP 800-38A, section 6.2
 *   SHA-1   FIPS 180-4, section 6.1 (the same algorithm as RFC 3174)
 *
 * tests/wii/wii_crypto_test.c holds them to the vectors those documents
 * publish. No key lives in this file or anywhere else in the repository:
 * every key is handed in by the caller.
 *
 * These are for reading discs, not for keeping secrets. The AES here
 * looks its tables up with indices that depend on the key, which is the
 * classic cache-timing leak; that does not matter for a key the user
 * already holds, and it would for anything else.
 */
#ifndef WII_CRYPTO_H
#define WII_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WII_AES_BLOCK 16u
#define WII_SHA1_SIZE 20u

/* A key, expanded both ways: eleven round keys of four words for
 * encrypting, and the same eleven rearranged for decrypting. */
typedef struct WiiAes {
  uint32_t encrypt[44];
  uint32_t decrypt[44];
} WiiAes;

void wii_aes_set_key(WiiAes *aes, const uint8_t key[16]);

/* One block. `in` and `out` may be the same sixteen bytes. */
void wii_aes_encrypt_block(const WiiAes *aes, const uint8_t in[16], uint8_t out[16]);
void wii_aes_decrypt_block(const WiiAes *aes, const uint8_t in[16], uint8_t out[16]);

/* Cipher block chaining over `length` bytes, which must be a multiple of
 * sixteen; a ragged tail is not processed. `in` and `out` may be the same
 * buffer, and may not otherwise overlap. The initialisation vector is not
 * modified. */
void wii_aes_cbc_encrypt(const WiiAes *aes, const uint8_t iv[16], const uint8_t *in,
                         uint8_t *out, size_t length);
void wii_aes_cbc_decrypt(const WiiAes *aes, const uint8_t iv[16], const uint8_t *in,
                         uint8_t *out, size_t length);

typedef struct WiiSha1 {
  uint32_t state[5];
  uint64_t length; /* in bytes, of everything fed in so far */
  uint8_t block[64];
} WiiSha1;

void wii_sha1_begin(WiiSha1 *sha);
void wii_sha1_feed(WiiSha1 *sha, const void *data, size_t length);
void wii_sha1_finish(WiiSha1 *sha, uint8_t digest[WII_SHA1_SIZE]);

/* All three at once. */
void wii_sha1(const void *data, size_t length, uint8_t digest[WII_SHA1_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* WII_CRYPTO_H */
