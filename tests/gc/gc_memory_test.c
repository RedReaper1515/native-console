/*
 * Held pages, and what may run beside them (gc_memory.h).
 *
 * A pending EFB copy holds the pages it will write. Three things about that
 * are checked here, because a mistake in any of them is either a wrong byte
 * read by a game or a wait that makes one slow:
 *
 * - which copy a byte belongs to, and where its range ends or the next one
 *   begins (gc_memory_held_at), which is how the machine's thread decides
 *   what part of a texture it must not read yet;
 * - how many bytes one host instruction moves (gc_memory_access_reach), from
 *   hand-assembled instructions of every kind the rule knows and some it
 *   must not claim to know;
 * - the rule itself, with real accesses: one that ends on the byte before a
 *   copy's range, or starts on the byte after it, runs at once and sees
 *   memory as it is; one that takes a single byte of the range waits for the
 *   copy and sees the copy's bytes.
 *
 * Where the host has no second view of memory (anything but Windows for
 * now) nothing is held and nothing is checked.
 */

#include <stdio.h>
#include <string.h>

#include "gc_memory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define BLOCK_BYTES 0x10000u

static int failures;

#define CHECK(what, condition)                                   \
  do {                                                           \
    if (!(condition)) {                                          \
      fprintf(stderr, "gc_memory_test: %s (line %d)\n", what, __LINE__); \
      failures++;                                                \
    }                                                            \
  } while (0)

/* Accesses the compiler has to make with one plain move of the size named. */
static __declspec(noinline) uint32_t read32(const volatile uint8_t *p) { return *(const volatile uint32_t *)p; }
static __declspec(noinline) uint64_t read64(const volatile uint8_t *p) { return *(const volatile uint64_t *)p; }
static __declspec(noinline) uint8_t read8(const volatile uint8_t *p) { return *p; }
static __declspec(noinline) void write8(volatile uint8_t *p, uint8_t v) { *p = v; }
static __declspec(noinline) void write16(volatile uint8_t *p, uint16_t v) { *(volatile uint16_t *)p = v; }

static uint8_t pattern(size_t offset) { return (uint8_t)(offset * 7u + (offset >> 8) + 1u); }

/* An access that should have run at once and did not would wait for ever:
 * nothing here releases its hold. */
static DWORD WINAPI watchdog(LPVOID unused) {
  (void)unused;
  Sleep(20000);
  fprintf(stderr, "gc_memory_test: an access waited that nothing was going to let through\n");
  ExitProcess(2);
}

typedef struct {
  uint8_t *base;
  uint8_t *alias;
  size_t first, size;
  uint64_t tag;
} Landing;

/* What the thread that makes a copy does: write the bytes through the
 * second view, then let the pages go. */
static DWORD WINAPI land(LPVOID argument) {
  const Landing *const l = (const Landing *)argument;
  Sleep(100);
  memset(l->alias + l->first, 0x55, l->size);
  gc_memory_release_tagged(l->base, l->first, l->size, l->tag, 1);
  return 0;
}

static void test_reach(void) {
  static const struct {
    const char *what;
    uint8_t code[8];
    unsigned reach;
  } cases[] = {
      {"mov eax, [rcx]", {0x8b, 0x01}, 4},
      {"mov [rcx], eax", {0x89, 0x01}, 4},
      {"mov rax, [rcx]", {0x48, 0x8b, 0x01}, 8},
      {"mov [r9], r8", {0x4d, 0x89, 0x01}, 8},
      {"mov ax, [rcx]", {0x66, 0x8b, 0x01}, 2},
      {"mov [rcx], ax", {0x66, 0x89, 0x01}, 2},
      {"mov al, [rcx]", {0x8a, 0x01}, 1},
      {"mov [rcx], al", {0x88, 0x01}, 1},
      {"mov [r9], r8b", {0x45, 0x88, 0x01}, 1},
      {"mov byte [rcx], 5", {0xc6, 0x01, 0x05}, 1},
      {"mov dword [rcx], 5", {0xc7, 0x01, 0x05, 0, 0, 0}, 4},
      {"mov word [rcx], 5", {0x66, 0xc7, 0x01, 0x05, 0}, 2},
      {"movzx eax, byte [rcx]", {0x0f, 0xb6, 0x01}, 1},
      {"movzx eax, word [rcx]", {0x0f, 0xb7, 0x01}, 2},
      {"movsx rax, byte [rcx]", {0x48, 0x0f, 0xbe, 0x01}, 1},
      {"movsx eax, word [rcx]", {0x0f, 0xbf, 0x01}, 2},
      {"movbe eax, [rcx]", {0x0f, 0x38, 0xf0, 0x01}, 4},
      {"movbe [rcx], rax", {0x48, 0x0f, 0x38, 0xf1, 0x01}, 8},
      {"movbe [rcx], ax", {0x66, 0x0f, 0x38, 0xf1, 0x01}, 2},
      {"add [rcx], eax", {0x01, 0x01}, 4},
      {"cmp al, [rcx]", {0x3a, 0x01}, 1},
      {"cmp dword [rcx], 0", {0x83, 0x39, 0x00}, 4},
      {"cmp byte [rcx], 0", {0x80, 0x39, 0x00}, 1},
      {"test [rcx], eax", {0x85, 0x01}, 4},
      {"test byte [rcx], 1", {0xf6, 0x01, 0x01}, 1},
      {"inc byte [rcx]", {0xfe, 0x01}, 1},
      {"not qword [rcx]", {0x48, 0xf7, 0x11}, 8},
      {"rep movsb (an element at a time)", {0xf3, 0xa4}, 1},
      {"rep movsq", {0xf3, 0x48, 0xa5}, 8},
      {"rep stosd", {0xf3, 0xab}, 4},
      {"stosw", {0x66, 0xab}, 2},
      /* Not a general-purpose move: the widest reach there is. */
      {"movups xmm0, [rcx]", {0x0f, 0x10, 0x01}, 64},
      {"movdqu [rcx], xmm0", {0xf3, 0x0f, 0x7f, 0x01}, 64},
      {"movdqa xmm0, [rcx]", {0x66, 0x0f, 0x6f, 0x01}, 64},
      {"vmovdqu ymm0, [rcx]", {0xc5, 0xfe, 0x6f, 0x01}, 64},
      {"movss xmm0, [rcx]", {0xf3, 0x0f, 0x10, 0x01}, 64},
      {"fld dword [rcx]", {0xd9, 0x01}, 64},
      {"cmpxchg16b [rcx]", {0x48, 0x0f, 0xc7, 0x09}, 64},
      {"push qword [rcx]", {0xff, 0x31}, 64},
  };
  unsigned i;
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const unsigned got = gc_memory_access_reach(cases[i].code);
    if (got != cases[i].reach) {
      fprintf(stderr, "gc_memory_test: %s moves %u bytes, and was taken to move %u\n", cases[i].what, cases[i].reach,
              got);
      failures++;
    }
  }
}

static void test_held_at(uint8_t *base) {
  const uint64_t older = gc_memory_next_tag();
  const uint64_t newer = gc_memory_next_tag();
  size_t end = 0;
  CHECK("tags grow", newer > older);
  CHECK("nothing is held to begin with", gc_memory_held_at(base, 0x1010u, &end) == 0 && end == BLOCK_BYTES);
  CHECK("a hold is taken", gc_memory_hold_tagged(base, 0x1010u, 0x100u, older) == 0);
  CHECK("... and a second across its end", gc_memory_hold_tagged(base, 0x10c0u, 0x2000u, newer) == 0);

  CHECK("the byte before a range is nobody's, and the range begins after it",
        gc_memory_held_at(base, 0x100fu, &end) == 0 && end == 0x1010u);
  CHECK("the first byte of a range is its own", gc_memory_held_at(base, 0x1010u, &end) == older && end == 0x1110u);
  CHECK("where two overlap the later copy has the byte",
        gc_memory_held_at(base, 0x10c0u, &end) == newer && end == 0x30c0u);
  CHECK("... to its last byte", gc_memory_held_at(base, 0x30bfu, &end) == newer && end == 0x30c0u);
  CHECK("the byte after the last range is nobody's, to the end of memory",
        gc_memory_held_at(base, 0x30c0u, &end) == 0 && end == BLOCK_BYTES);
  CHECK("the start of memory is nobody's, to the first range", gc_memory_held_at(base, 0u, &end) == 0 && end == 0x1010u);

  /* A view that needs no wait: beside a range on its page yes, in it no. */
  CHECK("bytes on a held page outside every range are read through the second view",
        gc_memory_view_for(base, 0x1000u, 0x10u) == gc_memory_alias(base));
  CHECK("... not one byte into a range", gc_memory_view_for(base, 0x1000u, 0x11u) == NULL);
  CHECK("a page nobody holds is read as it is", gc_memory_view_for(base, 0x8000u, 0x100u) == base);

  gc_memory_release_tagged(base, 0x1010u, 0x100u, older, 1);
  CHECK("with the older copy landed, its first bytes are nobody's",
        gc_memory_held_at(base, 0x1010u, &end) == 0 && end == 0x10c0u);
  CHECK("... and the later copy's still its own", gc_memory_held_at(base, 0x10c0u, &end) == newer);
  gc_memory_release_tagged(base, 0x10c0u, 0x2000u, newer, 1);
  CHECK("everything released", !gc_memory_held(base, 0u, BLOCK_BYTES) &&
                                   gc_memory_held_at(base, 0x10c0u, &end) == 0 && end == BLOCK_BYTES);
}

static void test_stepping(uint8_t *base) {
  uint8_t *const alias = gc_memory_alias(base);
  const uint64_t tag = gc_memory_next_tag();
  const uint64_t waits = gc_memory_waits(base);
  uint64_t steps = gc_memory_steps(base);
  Landing landing;
  HANDLE thread;
  uint32_t word;
  size_t i;
  for (i = 0; i < BLOCK_BYTES; i++) base[i] = pattern(i);

  /* A copy's bytes in the middle of a page, as a heap block's are. */
  CHECK("a hold is taken", gc_memory_hold_tagged(base, 0x4010u, 0x200u, tag) == 0);

  word = read32(base + 0x400cu);
  CHECK("four bytes that end where a copy begins are read at once", gc_memory_steps(base) == steps + 1u);
  CHECK("... and are what memory holds", word == (uint32_t)(pattern(0x400cu) | pattern(0x400du) << 8 |
                                                              pattern(0x400eu) << 16 | (uint32_t)pattern(0x400fu) << 24));
  steps = gc_memory_steps(base);
  CHECK("eight bytes that end there too", read64(base + 0x4008u) != 0 && gc_memory_steps(base) == steps + 1u);
  steps = gc_memory_steps(base);
  CHECK("the byte before a copy", read8(base + 0x400fu) == pattern(0x400fu) && gc_memory_steps(base) == steps + 1u);
  steps = gc_memory_steps(base);
  write8(base + 0x4210u, 0xa1);
  CHECK("the byte after a copy is written at once", gc_memory_steps(base) == steps + 1u);
  CHECK("... and the write is there", alias[0x4210u] == 0xa1);
  steps = gc_memory_steps(base);
  write16(base + 0x400eu, 0xb2c3);
  CHECK("two bytes before a copy are written at once", gc_memory_steps(base) == steps + 1u);
  CHECK("... and the write is there", alias[0x400eu] == 0xc3 && alias[0x400fu] == 0xb2);
  CHECK("... and the copy's first byte is untouched", alias[0x4010u] == pattern(0x4010u));
  CHECK("nothing waited", gc_memory_waits(base) == waits);
  CHECK("the page is still held", gc_memory_held(base, 0x4010u, 0x200u));

  /* One byte of the copy: the access waits, and sees the copy. */
  landing.base = base;
  landing.alias = alias;
  landing.first = 0x4010u;
  landing.size = 0x200u;
  landing.tag = tag;
  thread = CreateThread(NULL, 0, land, &landing, 0, NULL);
  word = read32(base + 0x400du);
  CHECK("four bytes that take one of a copy's wait for it", gc_memory_waits(base) == waits + 1u);
  CHECK("... and see the copy's byte", word == (uint32_t)(pattern(0x400du) | 0xc3u << 8 | 0xb2u << 16 | 0x55u << 24));
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
  CHECK("the hold is gone", !gc_memory_held(base, 0x4010u, 0x200u));
}

int main(void) {
  uint8_t *const base = gc_memory_create(BLOCK_BYTES);
  HANDLE dog;
  if (!base || !gc_memory_alias(base)) {
    fprintf(stderr, "gc_memory_test: no memory with a second view\n");
    return 1;
  }
  dog = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
  test_reach();
  test_held_at(base);
  test_stepping(base);
  (void)dog;
  if (failures) {
    fprintf(stderr, "gc_memory_test: %d failed\n", failures);
    return 1;
  }
  printf("gc_memory_test: held ranges, instruction reach and stepping beside a copy agree\n");
  return 0;
}

#else

int main(void) {
  printf("gc_memory_test: this host has no second view of memory; nothing is held, nothing checked\n");
  return 0;
}

#endif
