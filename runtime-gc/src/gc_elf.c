/*
 * Loading a program into guest memory: ELF for the test and homebrew
 * programs devkitPPC builds, DOL for what comes off a disc.
 *
 * Both formats address memory the way a running game does - 0x80000000 for
 * cached main memory - so the loader maps an effective address to a
 * physical one the same way the apploader's BATs do. That is the only place
 * in the runtime allowed to assume that mapping: once the program is
 * running, translation is the BAT walk in gc_bus.c.
 *
 * An ELF's symbol table is read as well, because it is what lets the runner
 * stand in for OSReport. That is how dolphin-emu/hwtests reports its
 * verdicts, and how anything we build for the guest prints.
 */

#include <string.h>

#include "gc.h"

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/* The address the apploader leaves a program at, as a physical address. */
static uint32_t to_physical(uint32_t ea) { return ea & 0x3fffffffu; }

static int copy_in(GcMachine *m, uint32_t ea, const uint8_t *data, uint32_t size, uint32_t fill) {
  uint8_t *p = gc_machine_ram(m, to_physical(ea), size ? size : fill);
  if (!p) {
    if (m->log) fprintf(m->log, "gc: segment at %08x (%u bytes) is outside memory\n", ea, size);
    return -1;
  }
  if (data && size) memcpy(p, data, size);
  if (fill > size) memset(p + size, 0, fill - size);
  return 0;
}

/* ------------------------------------------------------------------ */

void gc_register_symbol(GcMachine *m, const char *name, uint32_t address, uint32_t size);

int gc_load_elf(GcMachine *m, const void *data, size_t size, uint32_t *entry) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t phoff, shoff, i;
  uint16_t phentsize, phnum, shentsize, shnum, shstrndx;
  uint32_t loaded_end = 0;

  if (size < 52 || memcmp(bytes, "\x7f" "ELF", 4) != 0) return -1;
  if (bytes[4] != 1 || bytes[5] != 2) {
    if (m->log) fprintf(m->log, "gc: not a 32-bit big-endian ELF\n");
    return -1;
  }
  if (be16(bytes + 18) != 20) { /* EM_PPC */
    if (m->log) fprintf(m->log, "gc: not a PowerPC ELF\n");
    return -1;
  }

  *entry = be32(bytes + 24);
  phoff = be32(bytes + 28);
  shoff = be32(bytes + 32);
  phentsize = be16(bytes + 42);
  phnum = be16(bytes + 44);
  shentsize = be16(bytes + 46);
  shnum = be16(bytes + 48);
  shstrndx = be16(bytes + 50);

  for (i = 0; i < phnum; i++) {
    const uint8_t *ph = bytes + phoff + (size_t)i * phentsize;
    uint32_t type, offset, vaddr, filesz, memsz;
    if ((size_t)(ph - bytes) + phentsize > size) return -1;
    type = be32(ph);
    offset = be32(ph + 4);
    vaddr = be32(ph + 8);
    filesz = be32(ph + 16);
    memsz = be32(ph + 20);
    if (type != 1 /* PT_LOAD */ || memsz == 0) continue;
    if ((size_t)offset + filesz > size) return -1;
    if (copy_in(m, vaddr, bytes + offset, filesz, memsz) != 0) return -1;
    if (vaddr + memsz > loaded_end) loaded_end = vaddr + memsz;
  }
  gc_set_arena_low(m, loaded_end);
  (void)shoff;
  (void)shentsize;
  (void)shnum;
  (void)shstrndx;
  /* Symbols, so the runner can stand in for the functions it hooks. */
  return gc_elf_symbols(m, data, size);
}

/* The symbols alone: for a program that got into memory some other way -
 * off a disc, through its apploader - and whose ELF is still at hand to say
 * where its functions are. */
int gc_elf_symbols(GcMachine *m, const void *data, size_t size) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t shoff, i;
  uint16_t shentsize, shnum, shstrndx;

  if (size < 52 || bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') return -1;
  if (bytes[4] != 1 || bytes[5] != 2) return -1;
  shoff = be32(bytes + 32);
  shentsize = be16(bytes + 46);
  shnum = be16(bytes + 48);
  shstrndx = be16(bytes + 50);

  if (shoff && shnum && shstrndx < shnum) {
    const uint8_t *shstr = bytes + be32(bytes + shoff + (size_t)shstrndx * shentsize + 16);
    for (i = 0; i < shnum; i++) {
      const uint8_t *sh = bytes + shoff + (size_t)i * shentsize;
      const uint32_t name_offset = be32(sh);
      const uint32_t type = be32(sh + 4);
      const uint32_t section_offset = be32(sh + 16);
      const uint32_t section_size = be32(sh + 20);
      /* Elf32_Shdr: name 0, type 4, flags 8, addr 12, offset 16, size 20,
       * link 24, info 28, addralign 32, entsize 36. */
      const uint32_t link = be32(sh + 24);
      const uint32_t entsize = be32(sh + 36);
      const char *section_name = (const char *)(shstr + name_offset);
      const uint8_t *strtab;
      uint32_t strtab_size, s;

      if (type != 2 /* SHT_SYMTAB */ || strcmp(section_name, ".symtab") != 0) continue;
      if (link >= shnum || entsize < 16) continue;
      strtab = bytes + be32(bytes + shoff + (size_t)link * shentsize + 16);
      strtab_size = be32(bytes + shoff + (size_t)link * shentsize + 20);
      if ((size_t)section_offset + section_size > size) continue;

      for (s = 0; s + entsize <= section_size; s += entsize) {
        const uint8_t *sym = bytes + section_offset + s;
        const uint32_t sym_name = be32(sym);
        const uint32_t value = be32(sym + 4);
        const uint32_t sym_size = be32(sym + 8);
        const uint8_t info = sym[12];
        if (sym_name >= strtab_size || (info & 0xf) != 2 /* STT_FUNC */) continue;
        gc_register_symbol(m, (const char *)(strtab + sym_name), value, sym_size);
      }
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */

/* A DOL is seven text sections, eleven data sections and a bss range. It
 * carries no symbols, which is why a game needs the analyzer rather than a
 * hook table. */
int gc_load_dol(GcMachine *m, const void *data, size_t size, uint32_t *entry) {
  const uint8_t *bytes = (const uint8_t *)data;
  unsigned i;
  uint32_t bss_address, bss_size;
  uint32_t end = 0;

  if (size < 0x100) return -1;

  /* The zeroed region comes **first**, and the order is the whole of it.
   *
   * A DOL's BSS descriptor is one address and one length, and a linker
   * uses it for everything that needs zeroing - .bss, .sbss and .sbss2.
   * Those are not contiguous: the small *data* sections sit between them,
   * and they are real loaded sections that land inside the range the BSS
   * descriptor covers. Shrek 2 has its BSS running 0x802f3660 to
   * 0x8039020c with two loaded data sections inside it.
   *
   * So zeroing after loading erases initialised data - and erases it in
   * the small-data area, which is where a compiler puts the globals that
   * are reached most often. The game read one of those, got a null
   * pointer, and took a fault four hundred thousand instructions into its
   * own start-up; from there it wandered into the exception vectors and
   * stayed. Every symptom was a long way from the cause. */
  bss_address = be32(bytes + 0xd8);
  bss_size = be32(bytes + 0xdc);
  if (bss_size) {
    if (copy_in(m, bss_address, NULL, 0, bss_size) != 0) return -1;
  }

  for (i = 0; i < 18; i++) {
    const uint32_t offset = be32(bytes + 0x00 + i * 4);
    const uint32_t address = be32(bytes + 0x48 + i * 4);
    const uint32_t section_size = be32(bytes + 0x90 + i * 4);
    if (!section_size || !address) continue;
    if ((size_t)offset + section_size > size) return -1;
    if (copy_in(m, address, bytes + offset, section_size, section_size) != 0) return -1;
    if (address + section_size > end) end = address + section_size;
  }
  if (bss_address + bss_size > end) end = bss_address + bss_size;
  gc_set_arena_low(m, end);
  *entry = be32(bytes + 0xe0);
  return 0;
}
