/*
 * AES-128 and SHA-1, written from the standards (wii_crypto.h).
 *
 * AES works on a four-by-four grid of bytes, the "state", filled a column
 * at a time, and a round does four things to it: every byte goes through
 * a substitution table, the rows are rotated, each column is multiplied
 * by a fixed matrix, and a round key is added (FIPS-197, section 5.1).
 * Done literally that is sixteen table lookups and a page of shifts per
 * round. The first three steps are all per-byte or linear, though, so for
 * each of the four rows there is one table of 256 words that holds "what
 * this byte contributes to its output column" - substitution and column
 * multiply already done - and the row rotation is just which byte is
 * picked from which column. A round is then sixteen lookups and sixteen
 * exclusive-ors, with the state held as four big-endian words, one per
 * column. That form is in the Rijndael submission's own implementation
 * notes; the tables here are built at first use from the standard's
 * definitions rather than typed in, so there are no constants to mistype.
 *
 * Decrypting uses the standard's "equivalent inverse cipher" (FIPS-197,
 * section 5.3.5): the same shape of round with the inverse tables, and a
 * key schedule that is the encryption one reversed with the inverse
 * column multiply applied to the middle nine round keys.
 */

#include <string.h>

#include "wii_crypto.h"

/* ------------------------------------------------------------------ */
/* AES                                                                 */

static uint8_t sbox[256];
static uint8_t inverse_sbox[256];
static uint32_t encrypt_table[4][256];
static uint32_t decrypt_table[4][256];
static int tables_ready;

/* Times two in AES's field: a shift, and the field's polynomial
 * (x^8 + x^4 + x^3 + x + 1) taken off when the top bit falls out. */
static uint8_t times2(uint8_t x) {
  return (uint8_t)((x << 1) ^ ((x & 0x80u) ? 0x1bu : 0u));
}

static uint8_t rotate_byte(uint8_t x, unsigned by) {
  return (uint8_t)((x << by) | (x >> (8u - by)));
}

static uint32_t rotate_right(uint32_t x, unsigned by) { return (x >> by) | (x << (32u - by)); }

static uint32_t rotate_left(uint32_t x, unsigned by) { return (x << by) | (x >> (32u - by)); }

/* The tables, from their definitions.
 *
 * The substitution is "the multiplicative inverse in the field, then an
 * affine map" (FIPS-197, section 5.1.1). Inverses come from a table of
 * powers of three, which generates every non-zero element: the inverse of
 * 3^n is 3^(255-n).
 *
 * Two threads arriving here together both write the same values, and
 * neither is told the tables are ready until one of them has written them
 * all, so nothing reads a half-built table. */
static void build_tables(void) {
  uint8_t power[256], logarithm[256];
  uint8_t x = 1;
  unsigned i;

  memset(logarithm, 0, sizeof(logarithm));
  for (i = 0; i < 255u; i++) {
    power[i] = x;
    logarithm[x] = (uint8_t)i;
    x = (uint8_t)(x ^ times2(x)); /* times three */
  }
  power[255] = power[0];

  for (i = 0; i < 256u; i++) {
    const uint8_t inverse = i ? power[255u - logarithm[i]] : 0;
    const uint8_t s = (uint8_t)(inverse ^ rotate_byte(inverse, 1) ^ rotate_byte(inverse, 2) ^
                                rotate_byte(inverse, 3) ^ rotate_byte(inverse, 4) ^ 0x63u);
    sbox[i] = s;
    inverse_sbox[s] = (uint8_t)i;
  }

  for (i = 0; i < 256u; i++) {
    /* Encrypting: the column matrix's first column is (2, 1, 1, 3). */
    const uint8_t s = sbox[i];
    const uint8_t s2 = times2(s);
    const uint8_t s3 = (uint8_t)(s2 ^ s);
    /* Decrypting: the inverse matrix's first column is (14, 9, 13, 11). */
    const uint8_t v = inverse_sbox[i];
    const uint8_t v2 = times2(v);
    const uint8_t v4 = times2(v2);
    const uint8_t v8 = times2(v4);
    const uint8_t v9 = (uint8_t)(v8 ^ v);
    const uint8_t v11 = (uint8_t)(v8 ^ v2 ^ v);
    const uint8_t v13 = (uint8_t)(v8 ^ v4 ^ v);
    const uint8_t v14 = (uint8_t)(v8 ^ v4 ^ v2);
    const uint32_t e = ((uint32_t)s2 << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | s3;
    const uint32_t d = ((uint32_t)v14 << 24) | ((uint32_t)v9 << 16) | ((uint32_t)v13 << 8) | v11;
    unsigned row;
    /* A byte one row further down contributes the same four products one
     * place further round the column. */
    for (row = 0; row < 4u; row++) {
      encrypt_table[row][i] = row ? rotate_right(e, 8u * row) : e;
      decrypt_table[row][i] = row ? rotate_right(d, 8u * row) : d;
    }
  }
  tables_ready = 1;
}

static uint32_t load32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void store32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static uint32_t substitute_word(uint32_t w) {
  return ((uint32_t)sbox[w >> 24] << 24) | ((uint32_t)sbox[(w >> 16) & 0xffu] << 16) |
         ((uint32_t)sbox[(w >> 8) & 0xffu] << 8) | sbox[w & 0xffu];
}

/* The inverse column multiply on one word. The decryption table already
 * holds "inverse substitution, then the multiply", so substituting first
 * leaves just the multiply. */
static uint32_t inverse_mix(uint32_t w) {
  return decrypt_table[0][sbox[w >> 24]] ^ decrypt_table[1][sbox[(w >> 16) & 0xffu]] ^
         decrypt_table[2][sbox[(w >> 8) & 0xffu]] ^ decrypt_table[3][sbox[w & 0xffu]];
}

void wii_aes_set_key(WiiAes *aes, const uint8_t key[16]) {
  uint8_t round_constant = 1;
  unsigned i, round;

  if (!tables_ready) build_tables();

  /* FIPS-197, section 5.2: the key is the first four words, and each word
   * after is the one four back, mixed with the one before - which, every
   * fourth word, is first rotated, substituted and given a constant. */
  for (i = 0; i < 4u; i++) aes->encrypt[i] = load32(key + 4u * i);
  for (i = 4; i < 44u; i++) {
    uint32_t previous = aes->encrypt[i - 1u];
    if ((i & 3u) == 0) {
      previous = substitute_word(rotate_left(previous, 8)) ^ ((uint32_t)round_constant << 24);
      round_constant = times2(round_constant);
    }
    aes->encrypt[i] = aes->encrypt[i - 4u] ^ previous;
  }

  for (round = 0; round <= 10u; round++) {
    for (i = 0; i < 4u; i++) {
      const uint32_t w = aes->encrypt[4u * (10u - round) + i];
      aes->decrypt[4u * round + i] = (round == 0 || round == 10u) ? w : inverse_mix(w);
    }
  }
}

void wii_aes_encrypt_block(const WiiAes *aes, const uint8_t in[16], uint8_t out[16]) {
  const uint32_t *key = aes->encrypt;
  uint32_t s0 = load32(in) ^ key[0];
  uint32_t s1 = load32(in + 4) ^ key[1];
  uint32_t s2 = load32(in + 8) ^ key[2];
  uint32_t s3 = load32(in + 12) ^ key[3];
  uint32_t t0, t1, t2, t3;
  unsigned round;

  /* Row r of the output column c comes from column c + r: that is the row
   * rotation. */
  for (round = 1; round < 10u; round++) {
    key += 4;
    t0 = encrypt_table[0][s0 >> 24] ^ encrypt_table[1][(s1 >> 16) & 0xffu] ^
         encrypt_table[2][(s2 >> 8) & 0xffu] ^ encrypt_table[3][s3 & 0xffu] ^ key[0];
    t1 = encrypt_table[0][s1 >> 24] ^ encrypt_table[1][(s2 >> 16) & 0xffu] ^
         encrypt_table[2][(s3 >> 8) & 0xffu] ^ encrypt_table[3][s0 & 0xffu] ^ key[1];
    t2 = encrypt_table[0][s2 >> 24] ^ encrypt_table[1][(s3 >> 16) & 0xffu] ^
         encrypt_table[2][(s0 >> 8) & 0xffu] ^ encrypt_table[3][s1 & 0xffu] ^ key[2];
    t3 = encrypt_table[0][s3 >> 24] ^ encrypt_table[1][(s0 >> 16) & 0xffu] ^
         encrypt_table[2][(s1 >> 8) & 0xffu] ^ encrypt_table[3][s2 & 0xffu] ^ key[3];
    s0 = t0;
    s1 = t1;
    s2 = t2;
    s3 = t3;
  }

  /* The last round has no column multiply, so it is the substitution
   * table alone. */
  key += 4;
  t0 = ((uint32_t)sbox[s0 >> 24] << 24) | ((uint32_t)sbox[(s1 >> 16) & 0xffu] << 16) |
       ((uint32_t)sbox[(s2 >> 8) & 0xffu] << 8) | sbox[s3 & 0xffu];
  t1 = ((uint32_t)sbox[s1 >> 24] << 24) | ((uint32_t)sbox[(s2 >> 16) & 0xffu] << 16) |
       ((uint32_t)sbox[(s3 >> 8) & 0xffu] << 8) | sbox[s0 & 0xffu];
  t2 = ((uint32_t)sbox[s2 >> 24] << 24) | ((uint32_t)sbox[(s3 >> 16) & 0xffu] << 16) |
       ((uint32_t)sbox[(s0 >> 8) & 0xffu] << 8) | sbox[s1 & 0xffu];
  t3 = ((uint32_t)sbox[s3 >> 24] << 24) | ((uint32_t)sbox[(s0 >> 16) & 0xffu] << 16) |
       ((uint32_t)sbox[(s1 >> 8) & 0xffu] << 8) | sbox[s2 & 0xffu];
  store32(out, t0 ^ key[0]);
  store32(out + 4, t1 ^ key[1]);
  store32(out + 8, t2 ^ key[2]);
  store32(out + 12, t3 ^ key[3]);
}

void wii_aes_decrypt_block(const WiiAes *aes, const uint8_t in[16], uint8_t out[16]) {
  const uint32_t *key = aes->decrypt;
  uint32_t s0 = load32(in) ^ key[0];
  uint32_t s1 = load32(in + 4) ^ key[1];
  uint32_t s2 = load32(in + 8) ^ key[2];
  uint32_t s3 = load32(in + 12) ^ key[3];
  uint32_t t0, t1, t2, t3;
  unsigned round;

  /* The rows rotate the other way: row r of column c comes from
   * column c - r. */
  for (round = 1; round < 10u; round++) {
    key += 4;
    t0 = decrypt_table[0][s0 >> 24] ^ decrypt_table[1][(s3 >> 16) & 0xffu] ^
         decrypt_table[2][(s2 >> 8) & 0xffu] ^ decrypt_table[3][s1 & 0xffu] ^ key[0];
    t1 = decrypt_table[0][s1 >> 24] ^ decrypt_table[1][(s0 >> 16) & 0xffu] ^
         decrypt_table[2][(s3 >> 8) & 0xffu] ^ decrypt_table[3][s2 & 0xffu] ^ key[1];
    t2 = decrypt_table[0][s2 >> 24] ^ decrypt_table[1][(s1 >> 16) & 0xffu] ^
         decrypt_table[2][(s0 >> 8) & 0xffu] ^ decrypt_table[3][s3 & 0xffu] ^ key[2];
    t3 = decrypt_table[0][s3 >> 24] ^ decrypt_table[1][(s2 >> 16) & 0xffu] ^
         decrypt_table[2][(s1 >> 8) & 0xffu] ^ decrypt_table[3][s0 & 0xffu] ^ key[3];
    s0 = t0;
    s1 = t1;
    s2 = t2;
    s3 = t3;
  }

  key += 4;
  t0 = ((uint32_t)inverse_sbox[s0 >> 24] << 24) | ((uint32_t)inverse_sbox[(s3 >> 16) & 0xffu] << 16) |
       ((uint32_t)inverse_sbox[(s2 >> 8) & 0xffu] << 8) | inverse_sbox[s1 & 0xffu];
  t1 = ((uint32_t)inverse_sbox[s1 >> 24] << 24) | ((uint32_t)inverse_sbox[(s0 >> 16) & 0xffu] << 16) |
       ((uint32_t)inverse_sbox[(s3 >> 8) & 0xffu] << 8) | inverse_sbox[s2 & 0xffu];
  t2 = ((uint32_t)inverse_sbox[s2 >> 24] << 24) | ((uint32_t)inverse_sbox[(s1 >> 16) & 0xffu] << 16) |
       ((uint32_t)inverse_sbox[(s0 >> 8) & 0xffu] << 8) | inverse_sbox[s3 & 0xffu];
  t3 = ((uint32_t)inverse_sbox[s3 >> 24] << 24) | ((uint32_t)inverse_sbox[(s2 >> 16) & 0xffu] << 16) |
       ((uint32_t)inverse_sbox[(s1 >> 8) & 0xffu] << 8) | inverse_sbox[s0 & 0xffu];
  store32(out, t0 ^ key[0]);
  store32(out + 4, t1 ^ key[1]);
  store32(out + 8, t2 ^ key[2]);
  store32(out + 12, t3 ^ key[3]);
}

/* Chaining (SP 800-38A, section 6.2): each plaintext block is mixed with
 * the ciphertext block before it - the initialisation vector, for the
 * first - before it is encrypted. */
void wii_aes_cbc_encrypt(const WiiAes *aes, const uint8_t iv[16], const uint8_t *in,
                         uint8_t *out, size_t length) {
  uint8_t chain[16];
  size_t at;
  unsigned i;

  memcpy(chain, iv, sizeof(chain));
  for (at = 0; at + 16u <= length; at += 16u) {
    for (i = 0; i < 16u; i++) chain[i] ^= in[at + i];
    wii_aes_encrypt_block(aes, chain, chain);
    memcpy(out + at, chain, sizeof(chain));
  }
}

/* And back: decrypt the block, then take the previous ciphertext block
 * off. The ciphertext block is kept aside first, because decrypting in
 * place is about to overwrite it and the next block needs it. */
void wii_aes_cbc_decrypt(const WiiAes *aes, const uint8_t iv[16], const uint8_t *in,
                         uint8_t *out, size_t length) {
  uint8_t chain[16], cipher[16];
  size_t at;
  unsigned i;

  memcpy(chain, iv, sizeof(chain));
  for (at = 0; at + 16u <= length; at += 16u) {
    memcpy(cipher, in + at, sizeof(cipher));
    wii_aes_decrypt_block(aes, cipher, out + at);
    for (i = 0; i < 16u; i++) out[at + i] ^= chain[i];
    memcpy(chain, cipher, sizeof(chain));
  }
}

/* ------------------------------------------------------------------ */
/* SHA-1                                                               */

/* One 64-byte block into the running state (FIPS 180-4, section 6.1.2):
 * the block is stretched to eighty words, and eighty steps stir them in,
 * twenty at a time with a different mixing function and constant. */
static void sha1_block(uint32_t state[5], const uint8_t *block) {
  uint32_t w[80];
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  unsigned t;

  for (t = 0; t < 16u; t++) w[t] = load32(block + 4u * t);
  for (t = 16; t < 80u; t++) {
    w[t] = rotate_left(w[t - 3u] ^ w[t - 8u] ^ w[t - 14u] ^ w[t - 16u], 1);
  }

  for (t = 0; t < 80u; t++) {
    uint32_t f, k, next;
    if (t < 20u) {
      f = (b & c) | (~b & d); /* choose */
      k = 0x5a827999u;
    } else if (t < 40u) {
      f = b ^ c ^ d; /* parity */
      k = 0x6ed9eba1u;
    } else if (t < 60u) {
      f = (b & c) | (b & d) | (c & d); /* majority */
      k = 0x8f1bbcdcu;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6u;
    }
    next = rotate_left(a, 5) + f + e + k + w[t];
    e = d;
    d = c;
    c = rotate_left(b, 30);
    b = a;
    a = next;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void wii_sha1_begin(WiiSha1 *sha) {
  sha->state[0] = 0x67452301u;
  sha->state[1] = 0xefcdab89u;
  sha->state[2] = 0x98badcfeu;
  sha->state[3] = 0x10325476u;
  sha->state[4] = 0xc3d2e1f0u;
  sha->length = 0;
}

void wii_sha1_feed(WiiSha1 *sha, const void *data, size_t length) {
  const uint8_t *bytes = (const uint8_t *)data;
  size_t held = (size_t)(sha->length & 63u);

  sha->length += length;
  /* Top up a block left part full by the last call. */
  if (held) {
    size_t room = 64u - held;
    if (room > length) room = length;
    memcpy(sha->block + held, bytes, room);
    bytes += room;
    length -= room;
    if (held + room < 64u) return;
    sha1_block(sha->state, sha->block);
  }
  while (length >= 64u) {
    sha1_block(sha->state, bytes);
    bytes += 64;
    length -= 64u;
  }
  if (length) memcpy(sha->block, bytes, length);
}

/* The padding (FIPS 180-4, section 5.1.1): a one bit, zeroes up to eight
 * bytes short of a block boundary, and the message's length in bits. */
void wii_sha1_finish(WiiSha1 *sha, uint8_t digest[WII_SHA1_SIZE]) {
  const uint64_t bits = sha->length * 8u;
  size_t held = (size_t)(sha->length & 63u);
  unsigned i;

  sha->block[held++] = 0x80u;
  if (held > 56u) {
    memset(sha->block + held, 0, 64u - held);
    sha1_block(sha->state, sha->block);
    held = 0;
  }
  memset(sha->block + held, 0, 56u - held);
  store32(sha->block + 56, (uint32_t)(bits >> 32));
  store32(sha->block + 60, (uint32_t)bits);
  sha1_block(sha->state, sha->block);

  for (i = 0; i < 5u; i++) store32(digest + 4u * i, sha->state[i]);
}

void wii_sha1(const void *data, size_t length, uint8_t digest[WII_SHA1_SIZE]) {
  WiiSha1 sha;
  wii_sha1_begin(&sha);
  wii_sha1_feed(&sha, data, length);
  wii_sha1_finish(&sha, digest);
}
