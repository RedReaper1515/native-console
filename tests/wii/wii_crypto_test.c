/*
 * AES-128 and SHA-1 against the vectors their standards publish.
 *
 * runtime-gc/src/wii_crypto.c was written from the standards rather than
 * taken from a library, so this is what says it is right: the worked
 * examples in FIPS-197, the CBC vectors in NIST SP 800-38A, and the SHA-1
 * test cases in RFC 3174. Every key here is one of those documents' own
 * example keys.
 *
 * After the vectors come the properties the disc reader leans on that no
 * vector covers: decrypting undoes encrypting for arbitrary keys, a
 * buffer can be decrypted in place, and a hash fed in pieces is the hash
 * of the whole.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wii_crypto.h"

static int failures;
static const char *current;

#define CHECK(condition, ...)                                   \
  do {                                                          \
    if (!(condition)) {                                         \
      printf("FAIL [%s] %s:%d: ", current, __FILE__, __LINE__); \
      printf(__VA_ARGS__);                                      \
      printf("\n");                                             \
      failures++;                                               \
    }                                                           \
  } while (0)

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

/* Hexadecimal text into bytes; returns how many. */
static size_t hex(const char *text, uint8_t *out) {
  size_t count = 0;
  while (text[0] && text[1]) {
    out[count++] = (uint8_t)((hex_value(text[0]) << 4) | hex_value(text[1]));
    text += 2;
  }
  return count;
}

static void check_bytes(const uint8_t *got, const char *want_hex, const char *what) {
  uint8_t want[128];
  const size_t length = hex(want_hex, want);
  size_t i;
  if (memcmp(got, want, length) == 0) return;
  printf("FAIL [%s] %s\n  got  ", current, what);
  for (i = 0; i < length; i++) printf("%02x", got[i]);
  printf("\n  want %s\n", want_hex);
  failures++;
}

/* A stream of bytes that is the same on every run. */
static uint32_t random_state = 0x2545f491u;
static uint8_t random_byte(void) {
  random_state = random_state * 1664525u + 1013904223u;
  return (uint8_t)(random_state >> 24);
}

static void test_aes_blocks(void) {
  WiiAes aes;
  uint8_t key[16], in[16], out[16], back[16];

  /* FIPS-197, Appendix C.1: the example vector for AES-128. */
  current = "FIPS-197 C.1";
  hex("000102030405060708090a0b0c0d0e0f", key);
  hex("00112233445566778899aabbccddeeff", in);
  wii_aes_set_key(&aes, key);
  wii_aes_encrypt_block(&aes, in, out);
  check_bytes(out, "69c4e0d86a7b0430d8cdb78070b4c55a", "the cipher");
  wii_aes_decrypt_block(&aes, out, back);
  check_bytes(back, "00112233445566778899aabbccddeeff", "the inverse cipher");

  /* FIPS-197, Appendix B: the cipher worked through round by round. */
  current = "FIPS-197 B";
  hex("2b7e151628aed2a6abf7158809cf4f3c", key);
  hex("3243f6a8885a308d313198a2e0370734", in);
  wii_aes_set_key(&aes, key);
  wii_aes_encrypt_block(&aes, in, out);
  check_bytes(out, "3925841d02dc09fbdc118597196a0b32", "the cipher");
  wii_aes_decrypt_block(&aes, out, back);
  check_bytes(back, "3243f6a8885a308d313198a2e0370734", "the inverse cipher");

  /* FIPS-197, Appendix A.1: the same key's expansion. Its first four
   * words are the key and its last four are the final round key. */
  current = "FIPS-197 A.1";
  CHECK(aes.encrypt[0] == 0x2b7e1516u && aes.encrypt[3] == 0x09cf4f3cu, "w0 and w3 are the key");
  CHECK(aes.encrypt[4] == 0xa0fafe17u, "w4 = %08x, expected a0fafe17", aes.encrypt[4]);
  CHECK(aes.encrypt[40] == 0xd014f9a8u && aes.encrypt[41] == 0xc9ee2589u &&
            aes.encrypt[42] == 0xe13f0cc8u && aes.encrypt[43] == 0xb6630ca6u,
        "the last round key = %08x %08x %08x %08x", aes.encrypt[40], aes.encrypt[41],
        aes.encrypt[42], aes.encrypt[43]);

  /* SP 800-38A, F.1.1 and F.1.2: four blocks under one key, each on its
   * own. */
  current = "SP 800-38A F.1 (ECB-AES128)";
  {
    static const char *const plain[4] = {
        "6bc1bee22e409f96e93d7e117393172a", "ae2d8a571e03ac9c9eb76fac45af8e51",
        "30c81c46a35ce411e5fbc1191a0a52ef", "f69f2445df4f9b17ad2b417be66c3710"};
    static const char *const cipher[4] = {
        "3ad77bb40d7a3660a89ecaf32466ef97", "f5d3d58503b9699de785895a96fdbaaf",
        "43b1cd7f598ece23881b00e3ed030688", "7b0c785e27e8ad3f8223207104725dd4"};
    unsigned i;
    for (i = 0; i < 4; i++) {
      hex(plain[i], in);
      wii_aes_encrypt_block(&aes, in, out);
      check_bytes(out, cipher[i], "a block encrypted");
      wii_aes_decrypt_block(&aes, out, back);
      check_bytes(back, plain[i], "a block decrypted");
    }
  }

  /* The same sixteen bytes as input and output, which the chaining code
   * relies on. */
  current = "a block in place";
  hex("00112233445566778899aabbccddeeff", in);
  hex("000102030405060708090a0b0c0d0e0f", key);
  wii_aes_set_key(&aes, key);
  wii_aes_encrypt_block(&aes, in, in);
  check_bytes(in, "69c4e0d86a7b0430d8cdb78070b4c55a", "encrypted in place");
  wii_aes_decrypt_block(&aes, in, in);
  check_bytes(in, "00112233445566778899aabbccddeeff", "decrypted in place");
}

static void test_aes_cbc(void) {
  static const char plain_hex[] =
      "6bc1bee22e409f96e93d7e117393172a" "ae2d8a571e03ac9c9eb76fac45af8e51"
      "30c81c46a35ce411e5fbc1191a0a52ef" "f69f2445df4f9b17ad2b417be66c3710";
  static const char cipher_hex[] =
      "7649abac8119b246cee98e9b12e9197d" "5086cb9b507219ee95db113a917678b2"
      "73bed6b8e3c1743b7116e69e22229516" "3ff1caa1681fac09120eca307586e1a7";
  WiiAes aes;
  uint8_t key[16], iv[16], iv_before[16], plain[64], cipher[64], out[64];

  /* SP 800-38A, F.2.1 and F.2.2. */
  current = "SP 800-38A F.2 (CBC-AES128)";
  hex("2b7e151628aed2a6abf7158809cf4f3c", key);
  hex("000102030405060708090a0b0c0d0e0f", iv);
  memcpy(iv_before, iv, sizeof(iv));
  hex(plain_hex, plain);
  hex(cipher_hex, cipher);
  wii_aes_set_key(&aes, key);

  wii_aes_cbc_encrypt(&aes, iv, plain, out, sizeof(out));
  check_bytes(out, cipher_hex, "four blocks encrypted");
  wii_aes_cbc_decrypt(&aes, iv, cipher, out, sizeof(out));
  check_bytes(out, plain_hex, "four blocks decrypted");
  CHECK(memcmp(iv, iv_before, sizeof(iv)) == 0, "the vector is left as it was");

  /* In place, which is how a cluster's hashes could be decrypted. */
  memcpy(out, plain, sizeof(out));
  wii_aes_cbc_encrypt(&aes, iv, out, out, sizeof(out));
  check_bytes(out, cipher_hex, "encrypted in place");
  wii_aes_cbc_decrypt(&aes, iv, out, out, sizeof(out));
  check_bytes(out, plain_hex, "decrypted in place");

  /* A length that is not whole blocks: the tail is left alone. */
  memset(out, 0xee, sizeof(out));
  wii_aes_cbc_decrypt(&aes, iv, cipher, out, 40);
  CHECK(memcmp(out, plain, 32) == 0, "two whole blocks of a ragged length decrypt");
  CHECK(out[32] == 0xee && out[63] == 0xee, "and the part block after them is not touched");
}

/* No vector covers every key; a round trip covers the property that
 * matters for all of them. A cluster's worth of bytes each time, under a
 * fresh key and vector, compared with a block-at-a-time decryption that
 * does its own chaining. */
static void test_aes_round_trips(void) {
  enum { LENGTH = 0x7c00 };
  uint8_t *plain = (uint8_t *)malloc(LENGTH);
  uint8_t *cipher = (uint8_t *)malloc(LENGTH);
  uint8_t *back = (uint8_t *)malloc(LENGTH);
  unsigned round, i, at;

  current = "round trips";
  if (!plain || !cipher || !back) {
    CHECK(0, "out of memory");
    return;
  }
  for (round = 0; round < 8; round++) {
    WiiAes aes;
    uint8_t key[16], iv[16], previous[16], block[16];
    unsigned wrong = 0;

    for (i = 0; i < 16; i++) key[i] = random_byte();
    for (i = 0; i < 16; i++) iv[i] = random_byte();
    for (i = 0; i < LENGTH; i++) plain[i] = random_byte();
    wii_aes_set_key(&aes, key);
    wii_aes_cbc_encrypt(&aes, iv, plain, cipher, LENGTH);
    wii_aes_cbc_decrypt(&aes, iv, cipher, back, LENGTH);
    CHECK(memcmp(plain, back, LENGTH) == 0, "round %u: decrypting undoes encrypting", round);
    CHECK(memcmp(plain, cipher, LENGTH) != 0, "round %u: and encrypting did something", round);

    memcpy(previous, iv, sizeof(previous));
    for (at = 0; at < LENGTH; at += 16) {
      wii_aes_decrypt_block(&aes, cipher + at, block);
      for (i = 0; i < 16; i++) {
        if ((uint8_t)(block[i] ^ previous[i]) != plain[at + i]) wrong++;
      }
      memcpy(previous, cipher + at, sizeof(previous));
    }
    CHECK(wrong == 0, "round %u: %u bytes differ from chaining by hand", round, wrong);
  }
  free(plain);
  free(cipher);
  free(back);
}

static void test_sha1(void) {
  uint8_t digest[WII_SHA1_SIZE];
  WiiSha1 sha;
  unsigned i;

  /* RFC 3174, section 7.3: the four test cases its driver runs. */
  current = "RFC 3174 test 1";
  wii_sha1("abc", 3, digest);
  check_bytes(digest, "a9993e364706816aba3e25717850c26c9cd0d89d", "\"abc\"");

  current = "RFC 3174 test 2";
  wii_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, digest);
  check_bytes(digest, "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "the 448-bit message");

  current = "RFC 3174 test 3";
  wii_sha1_begin(&sha);
  for (i = 0; i < 1000000u; i++) wii_sha1_feed(&sha, "a", 1);
  wii_sha1_finish(&sha, digest);
  check_bytes(digest, "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "a million times \"a\"");

  current = "RFC 3174 test 4";
  wii_sha1_begin(&sha);
  for (i = 0; i < 10u; i++) {
    wii_sha1_feed(&sha, "0123456701234567012345670123456701234567012345670123456701234567", 64);
  }
  wii_sha1_finish(&sha, digest);
  check_bytes(digest, "dea356a2cddd90c7a7ecedc5ebb563934f460452", "ten whole blocks");

  /* FIPS 180's other standing example: nothing at all. */
  current = "SHA-1 of the empty message";
  wii_sha1("", 0, digest);
  check_bytes(digest, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "no bytes");
}

/* The padding has three shapes - room for the length in this block, no
 * room, and exactly none left over - and feeding in pieces has to land on
 * the same digest whatever the pieces. Every length around two block
 * boundaries, each fed whole and fed in uneven pieces. */
static void test_sha1_pieces(void) {
  uint8_t message[200];
  unsigned length, i;

  current = "SHA-1 fed in pieces";
  for (i = 0; i < sizeof(message); i++) message[i] = random_byte();
  for (length = 0; length <= sizeof(message); length++) {
    uint8_t whole[WII_SHA1_SIZE], pieces[WII_SHA1_SIZE];
    WiiSha1 sha;
    unsigned at = 0, step = 1;

    wii_sha1(message, length, whole);
    wii_sha1_begin(&sha);
    while (at < length) {
      const unsigned piece = step < length - at ? step : length - at;
      wii_sha1_feed(&sha, message + at, piece);
      at += piece;
      step = step * 3u % 71u + 1u;
    }
    wii_sha1_finish(&sha, pieces);
    CHECK(memcmp(whole, pieces, sizeof(whole)) == 0, "a message of %u bytes", length);
  }
}

int main(void) {
  test_aes_blocks();
  test_aes_cbc();
  test_aes_round_trips();
  test_sha1();
  test_sha1_pieces();
  printf("wii_crypto_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
