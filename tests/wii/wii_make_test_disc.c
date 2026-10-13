/*
 * The test disc as a file, for anything else that wants one: a boot test,
 * a tool, a look in a hex editor.
 *
 *     wii_make_test_disc OUT.iso
 *     wii_make_test_disc OUT.ciso --ciso 21
 *     wii_make_test_disc OUT.wbfs --wbfs 21 --far
 *
 * The disc is the one wii_test_disc.h describes. Its apploader and its
 * DOL are patterns, not programs: it is a disc to read, not one to boot.
 * The keys that open it are printed, because they are not secrets - they
 * were made up for this disc and open nothing else.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wii_test_disc.h"

static void print_key(const char *what, const char *key) {
  unsigned i;
  printf("  %s ", what);
  for (i = 0; i < 16; i++) printf("%02x", (unsigned)(unsigned char)key[i]);
  printf("  (\"%s\")\n", key);
}

static const uint8_t *read_whole(const char *path, uint32_t *size) {
  FILE *file = fopen(path, "rb");
  uint8_t *bytes = NULL;
  long length = 0;
  if (file && fseek(file, 0, SEEK_END) == 0) length = ftell(file);
  if (file && length > 0 && fseek(file, 0, SEEK_SET) == 0) {
    bytes = (uint8_t *)malloc((size_t)length);
    if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
      free(bytes);
      bytes = NULL;
    }
  }
  if (file) fclose(file);
  if (!bytes) fprintf(stderr, "wii_make_test_disc: cannot read %s\n", path);
  *size = bytes ? (uint32_t)length : 0u;
  return bytes;
}

int main(int argc, char **argv) {
  WiiTestDisc options;
  const char *out = NULL;
  int i;

  memset(&options, 0, sizeof(options));
  options.container = WII_TEST_ISO;
  options.partition_offset = WII_TEST_NEAR_PARTITION;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--ciso") && i + 1 < argc) {
      options.container = WII_TEST_CISO;
      options.shift = (unsigned)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--wbfs") && i + 1 < argc) {
      options.container = WII_TEST_WBFS;
      options.shift = (unsigned)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--far")) {
      options.partition_offset = WII_TEST_FAR_PARTITION;
    } else if (!strcmp(argv[i], "--no-hashes")) {
      options.no_hashes = 1;
    } else if (!strcmp(argv[i], "--no-encryption")) {
      options.no_encryption = 1;
    } else if (!strcmp(argv[i], "--apploader") && i + 2 < argc) {
      /* A disc that boots: the apploader's code and its entry point. */
      options.apploader = read_whole(argv[++i], &options.apploader_size);
      options.apploader_entry = (uint32_t)strtoul(argv[++i], NULL, 16);
      if (!options.apploader) return 1;
    } else if (!strcmp(argv[i], "--dol") && i + 1 < argc) {
      options.dol = read_whole(argv[++i], &options.dol_size);
      if (!options.dol) return 1;
    } else if (argv[i][0] != '-' && !out) {
      out = argv[i];
    } else {
      out = NULL;
      break;
    }
  }
  if (!out || (options.container != WII_TEST_ISO && (options.shift < 15u || options.shift > 30u))) {
    fprintf(stderr,
            "usage: wii_make_test_disc OUT [--ciso SHIFT | --wbfs SHIFT] [--far]\n"
            "                          [--no-hashes] [--no-encryption]\n"
            "                          [--apploader CODE ENTRY-IN-HEX] [--dol FILE]\n"
            "  SHIFT   the block or sector size as a power of two, 15 to 30\n"
            "  --far   put the game partition past 4 GB (not for a plain image)\n");
    return 2;
  }
  if (wii_test_disc_write(out, &options) != 0) return 1;

  printf("wii_make_test_disc: %s, game partition at 0x%llx, %u clusters\n", out,
         (unsigned long long)options.partition_offset, (unsigned)WII_TEST_CLUSTERS);
  print_key("common key", WII_TEST_COMMON_KEY);
  print_key("title key ", WII_TEST_TITLE_KEY);
  return 0;
}
