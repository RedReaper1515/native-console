/*
 * What the graphics thread keeps about EFB copies, and when the card's
 * texture of one stands for a texture a draw names (gc_gx_thread.h).
 *
 * No test here has a graphics card, so this one plays the card's part: it
 * issues copies as gc_gx.c does, lets the graphics thread make them the
 * reference's way, says "the card holds this one as a texture" as the fast
 * OpenGL path would, and asks the question that path asks before it binds a
 * map. Everything in between - the entries, what is remembered of a copy
 * after its entry has gone, the bytes compared - is the code a game runs.
 *
 * The cases are the ones a wrong picture would come from:
 *
 * - a copy stands for a texture of its address, size and format, and no
 *   other;
 * - a game that uses the place for something else and writes its copy back
 *   (Shrek 2's pause screen) is answered with the first copy again, and only
 *   while every byte is the copy's;
 * - a second picture taken at the same place is the one found afterwards,
 *   never the first;
 * - a copy the card never held, or no draw ever used, is not remembered;
 * - a loaded state, a stopped renderer and GC_GL_COPY_MEMO=0 forget.
 *
 * What it cannot reach: a copy remembered while its bytes are still on
 * their way from the card. Without a card a copy lands as it is made.
 *
 * Built for Windows only (tests/gc/CMakeLists.txt): the graphics thread
 * needs main memory's second view, which no other host has yet.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_efb.h"
#include "gc_gx.h"
#include "gc_gx_thread.h"
#include "gc_memory.h"
#include "gc_state.h"

static int failures;
static const char *current = "";

#define CHECK(condition, ...)                                   \
  do {                                                          \
    if (!(condition)) {                                         \
      printf("FAIL [%s] %s:%d: ", current, __FILE__, __LINE__); \
      printf(__VA_ARGS__);                                      \
      printf("\n");                                             \
      failures++;                                               \
    }                                                           \
  } while (0)

#define CHECK_U64(got, want, what)                                                          \
  do {                                                                                      \
    const unsigned long long g_ = (unsigned long long)(got), w_ = (unsigned long long)(want); \
    CHECK(g_ == w_, "%s = %llu, expected %llu", what, g_, w_);                              \
  } while (0)

#define RGB565 4u /* the texture format a five-six-five copy reads back in */

static GcMachine *machine;
static GcGxThread *thread;

/* A copy the test issued. */
typedef struct Copy {
  uint64_t tag;
  uint32_t address, first, end;
  unsigned width, height;
  int format;
  uint8_t *bytes; /* what it wrote, read out of main memory afterwards */
} Copy;

/* The framebuffer, a picture no two seeds of which share a row. */
static void paint(uint32_t seed) {
  uint32_t x, y;
  for (y = 0; y < GC_EFB_HEIGHT; y++) {
    for (x = 0; x < GC_EFB_WIDTH; x++) {
      const uint32_t v = (x * 2654435761u) ^ (y * 40503u) ^ (seed * 0x9e3779b9u);
      machine->efb.color[(size_t)y * GC_EFB_WIDTH + x] = 0xff000000u | (v & 0x00ffffffu);
    }
  }
}

static void drain(void) { gc_gx_thread_sync(machine); }

/* The top left `width` by `height` pixels of the framebuffer (halved when
 * `halve`) copied to `address` as five-six-five, issued as gc_gx.c issues a
 * copy: prepared on the machine, its pages held, handed to the graphics
 * thread. Then everything queued is run, and the copy's bytes read back. */
static Copy copy_to(uint32_t address, unsigned width, unsigned height, int halve) {
  const uint32_t trigger = (halve ? 0x000200u : 0u) | (8u << 3) | 3u;
  GcEfbCopyJob job;
  Copy c;
  memset(&c, 0, sizeof(c));
  machine->gx.bp[0x49] = 0;
  machine->gx.bp[0x4a] = ((height - 1u) << 10) | (width - 1u);
  machine->gx.bp[0x4b] = address >> 5;
  machine->gx.bp[0x4d] = (halve ? width / 2u : width) / 4u; /* tiles a row */
  machine->gx.bp[0x53] = (21u << 12) | (22u << 18);         /* the filter that leaves a row alone */
  machine->gx.bp[0x54] = 21u;
  if (!gc_efb_copy_prepare(machine, trigger, &job)) {
    CHECK(0, "the copy is one this writes");
    return c;
  }
  gc_efb_copy_bytes(&job, &c.first, &c.end);
  c.tag = gc_memory_next_tag();
  gc_memory_hold_tagged(machine->mem1, c.first, c.end - c.first, c.tag);
  gc_gx_thread_begin_run(thread, machine);
  gc_gx_thread_copy(thread, machine, trigger, &job, c.tag, c.first, c.end);
  gc_gx_thread_end_run(thread);
  drain();
  c.address = job.destination;
  c.width = job.out_width;
  c.height = job.out_rows;
  c.format = gc_efb_copy_texture_format(&job, NULL, NULL);
  c.bytes = (uint8_t *)malloc(c.end - c.first);
  if (!c.bytes) abort();
  memcpy(c.bytes, machine->mem1 + c.first, c.end - c.first);
  return c;
}

/* The card's part: it made a texture of the copy when it read the
 * framebuffer for it. */
static void card_holds(const Copy *c) {
  gc_gx_thread_copy_has_texture(gc_gx_thread_target(machine), c->tag, (unsigned)c->format);
}

/* What the OpenGL path asks before it binds a map at the copy's address, of
 * its size and format. 0, 1 or 2, and the copy that answered. */
static int ask_at(uint32_t address, unsigned width, unsigned height, unsigned format, uint64_t *tag) {
  *tag = 0;
  return gc_gx_thread_copy_texture(gc_gx_thread_target(machine), address, format, width, height, tag);
}

static int ask(const Copy *c, uint64_t *tag) { return ask_at(c->address, c->width, c->height, RGB565, tag); }

/* The processor writes main memory, and a draw that reads [first, first +
 * size) is issued: the bytes go to the graphics thread ahead of it. */
static void processor_writes(uint32_t first, const uint8_t *bytes, uint32_t size) {
  volatile uint8_t *const at = machine->mem1 + first;
  uint32_t i;
  for (i = 0; i < size; i++) at[i] = bytes[i];
}

static void draw_reads(uint32_t first, uint32_t size) {
  gc_gx_thread_begin_run(thread, machine);
  gc_gx_thread_send(thread, machine, first, size);
  gc_gx_thread_end_run(thread);
  drain();
}

static void write_back(const Copy *c) {
  processor_writes(c->first, c->bytes, c->end - c->first);
  draw_reads(c->first, c->end - c->first);
}

static GcGxCopyStats stats(void) {
  GcGxCopyStats s;
  drain();
  gc_gx_thread_copy_stats(machine, &s);
  return s;
}

/* A fresh machine, its graphics thread running. */
static void start(const char *name) {
  current = name;
  if (machine) gc_machine_destroy(machine);
  machine = gc_machine_create();
  if (!machine) {
    printf("gc_gx_copies_test: the machine would not start\n");
    exit(1);
  }
  machine->log = NULL;
  if (!gc_gx_thread_start(machine)) {
    printf("gc_gx_copies_test: the graphics thread would not start\n");
    exit(1);
  }
  thread = gc_gx_thread_of(machine);
}

/* Two buffers a game would have: a snapshot's, and one beside it. A page
 * copied to PLACE covers the first 131,072 of the snapshot's 153,600
 * bytes, as in the game this is modelled on. */
#define PLACE 0x004d1f60u
#define OTHER 0x00700000u

/* ------------------------------------------------------------------ */

/* A copy the card holds stands for a texture of its address, size and
 * format - and for nothing else at that address. */
static void test_a_copy_stands(void) {
  Copy snapshot;
  uint64_t tag;
  start("a copy stands for its texture");
  paint(1);
  snapshot = copy_to(PLACE, 640, 480, 1);
  CHECK_U64(snapshot.width, 320, "a halved copy's width");
  CHECK_U64(snapshot.height, 240, "and height");
  CHECK_U64(snapshot.end - snapshot.first, 153600, "its bytes");
  CHECK(snapshot.format == (int)RGB565, "it reads back as five-six-five");

  CHECK(ask(&snapshot, &tag) == 0, "a copy the card does not hold is its bytes");
  card_holds(&snapshot);
  CHECK(ask(&snapshot, &tag) == 1 && tag == snapshot.tag, "a copy the card holds is the card's texture");
  CHECK(ask_at(snapshot.address, 320, 240, 5u, &tag) == 0, "not in another format");
  CHECK(ask_at(snapshot.address, 256, 240, RGB565, &tag) == 0, "nor at another size");
  CHECK(ask_at(snapshot.address + 32u, 320, 240, RGB565, &tag) == 0, "nor at another address");

  /* The same bytes sent again change nothing; one byte changed does. */
  draw_reads(snapshot.first, snapshot.end - snapshot.first);
  CHECK(ask(&snapshot, &tag) == 1, "its own bytes sent over it leave it standing");
  {
    uint8_t other = (uint8_t)(snapshot.bytes[4000] ^ 0x40u);
    processor_writes(snapshot.first + 4000u, &other, 1);
    draw_reads(snapshot.first, snapshot.end - snapshot.first);
  }
  CHECK(ask(&snapshot, &tag) == 0, "a byte the processor changed makes it bytes again");
  free(snapshot.bytes);
}

/* The pause screen: a picture of the screen, the place borrowed for pages,
 * the picture written back by the processor before each draw of it. */
static void test_written_back(void) {
  Copy snapshot, page;
  GcGxCopyStats s;
  uint64_t tag;
  start("a copy written back");
  paint(1);
  snapshot = copy_to(PLACE, 640, 480, 1);
  card_holds(&snapshot);
  CHECK(ask(&snapshot, &tag) == 1, "the snapshot, drawn with while it stands");

  paint(2);
  page = copy_to(PLACE, 256, 256, 0);
  card_holds(&page);
  CHECK_U64(stats().made, 1, "copied over in part, the snapshot is remembered");
  CHECK(ask(&page, &tag) == 1 && tag == page.tag, "the page stands at the place now");
  CHECK(ask(&snapshot, &tag) == 0, "and the snapshot's texture is not the page's bytes");
  s = stats();
  CHECK_U64(s.lookups, 1, "one look at what is remembered");
  CHECK_U64(s.compares, 1, "which compared");
  CHECK_U64(s.found, 0, "and did not find");

  write_back(&snapshot);
  CHECK(ask(&snapshot, &tag) == 2 && tag == snapshot.tag, "written back, the snapshot is the card's texture again");
  CHECK(ask(&page, &tag) == 0, "and the page, whose bytes are gone, is not");
  s = stats();
  CHECK_U64(s.made, 2, "the page was remembered when its bytes changed");
  CHECK_U64(s.found, 1, "one texture found");

  /* Asked again with nothing changed, as a draw after a draw: no byte is
   * compared. */
  {
    const uint64_t compares = s.compares;
    unsigned i, found = 0;
    for (i = 0; i < 100u; i++) found += ask(&snapshot, &tag) == 2 && tag == snapshot.tag;
    CHECK_U64(found, 100, "times found, asked a hundred times more");
    s = stats();
    CHECK_U64(s.compares, compares, "comparisons for a hundred draws of unchanged bytes");
    CHECK_U64(s.found, 101, "textures found");

    /* The bytes sent again unchanged, and a byte changed beyond the copy's
     * end in the same send: still not compared. */
    draw_reads(snapshot.first, snapshot.end - snapshot.first);
    {
      const uint8_t beside = (uint8_t)(machine->mem1[snapshot.end + 3u] + 1u);
      processor_writes(snapshot.end + 3u, &beside, 1);
      draw_reads(snapshot.first, snapshot.end - snapshot.first + 64u);
    }
    CHECK(ask(&snapshot, &tag) == 2, "a byte changed beside it leaves it found");
    CHECK_U64(stats().compares, compares, "comparisons after a change beside it");
  }

  /* Every byte counts: the last one changed, and put back. */
  {
    const uint32_t last = snapshot.end - snapshot.first - 1u;
    const uint8_t was = snapshot.bytes[last], other = (uint8_t)(was ^ 1u);
    processor_writes(snapshot.first + last, &other, 1);
    draw_reads(snapshot.first, snapshot.end - snapshot.first);
    CHECK(ask(&snapshot, &tag) == 0, "one bit of the last byte different: not the copy");
    processor_writes(snapshot.first + last, &was, 1);
    draw_reads(snapshot.first, snapshot.end - snapshot.first);
    CHECK(ask(&snapshot, &tag) == 2 && tag == snapshot.tag, "put back: the copy again");
  }

  /* The next frame of the pause: the page again, the snapshot again. */
  free(page.bytes);
  paint(3);
  page = copy_to(PLACE, 256, 256, 0);
  card_holds(&page);
  CHECK(ask(&snapshot, &tag) == 0, "the next page over it: not the snapshot");
  CHECK(ask(&page, &tag) == 1, "the page");
  write_back(&snapshot);
  CHECK(ask(&snapshot, &tag) == 2 && tag == snapshot.tag, "and the snapshot written back once more");
  free(page.bytes);
  free(snapshot.bytes);
}

/* Pause, play on, pause again: the second picture is the one found, and the
 * first one's bytes are then only bytes. */
static void test_the_newer_picture(void) {
  Copy first, second, page;
  uint64_t tag;
  start("a second picture at the same place");
  paint(1);
  first = copy_to(PLACE, 640, 480, 1);
  card_holds(&first);
  CHECK(ask(&first, &tag) == 1, "the first picture");
  paint(2);
  page = copy_to(PLACE, 256, 256, 0);
  card_holds(&page);
  write_back(&first);
  CHECK(ask(&first, &tag) == 2 && tag == first.tag, "found while paused");
  free(page.bytes);

  paint(7);
  second = copy_to(PLACE, 640, 480, 1);
  card_holds(&second);
  CHECK(memcmp(first.bytes, second.bytes, first.end - first.first) != 0, "the second picture is another picture");
  CHECK(ask(&second, &tag) == 1 && tag == second.tag, "the second picture stands, and is the one a draw gets");
  paint(8);
  page = copy_to(PLACE, 256, 256, 0);
  card_holds(&page);
  write_back(&second);
  CHECK(ask(&second, &tag) == 2 && tag == second.tag, "written back, it is the second picture that is found");
  write_back(&first);
  CHECK(ask(&first, &tag) == 0, "the first picture's bytes are bytes: only the newest is remembered");
  free(page.bytes);
  free(first.bytes);
  free(second.bytes);
}

/* A copy covered whole by the next one, its bytes never put in place: its
 * own buffer is what it is remembered by. */
static void test_covered_whole(void) {
  Copy strip, page;
  uint64_t tag;
  start("a copy covered whole");
  paint(1);
  strip = copy_to(PLACE, 256, 128, 0);
  card_holds(&strip);
  CHECK(ask(&strip, &tag) == 1, "the strip");
  paint(2);
  page = copy_to(PLACE, 256, 256, 0);
  CHECK(strip.end <= page.end && page.first == strip.first, "the page covers the strip");
  CHECK_U64(stats().made, 1, "the strip is remembered");
  CHECK(ask(&strip, &tag) == 0, "under the page it is not found");
  write_back(&strip);
  CHECK(ask(&strip, &tag) == 2 && tag == strip.tag, "written back it is");
  free(strip.bytes);
  free(page.bytes);
}

/* Only a copy the card held and a draw used is worth remembering. */
static void test_not_remembered(void) {
  Copy unused, unheld, page;
  uint64_t tag;
  start("copies not remembered");
  paint(1);
  unused = copy_to(PLACE, 640, 480, 1);
  card_holds(&unused);
  paint(2);
  page = copy_to(PLACE, 256, 256, 0);
  CHECK_U64(stats().made, 0, "a copy no draw used is not remembered");
  write_back(&unused);
  CHECK(ask(&unused, &tag) == 0, "nor found");
  free(page.bytes);

  paint(3);
  unheld = copy_to(OTHER, 640, 480, 1);
  CHECK(ask(&unheld, &tag) == 0, "a copy the card does not hold (the software renderer's, the exact path's)");
  paint(4);
  page = copy_to(OTHER, 256, 256, 0);
  write_back(&unheld);
  CHECK(ask(&unheld, &tag) == 0, "is its bytes afterwards too");
  CHECK_U64(stats().made, 0, "and nothing was remembered");
  CHECK_U64(stats().lookups, 0, "or looked for");
  free(page.bytes);
  free(unused.bytes);
  free(unheld.bytes);
}

/* Thirty-two entries: the thirty-third copy pushes the oldest out, with its
 * bytes still where it wrote them - and it is found by them at once. */
static void test_pushed_out(void) {
  Copy oldest;
  uint64_t tag;
  unsigned i;
  start("an entry pushed out of a full table");
  paint(1);
  oldest = copy_to(OTHER, 64, 64, 0);
  card_holds(&oldest);
  CHECK(ask(&oldest, &tag) == 1, "the oldest copy");
  for (i = 1; i <= 32u; i++) {
    Copy c = copy_to(OTHER + i * 0x4000u, 64, 64, 0);
    free(c.bytes);
  }
  CHECK_U64(stats().made, 1, "pushed out, it is remembered");
  CHECK(ask(&oldest, &tag) == 2 && tag == oldest.tag, "and found: nothing has written its bytes");
  free(oldest.bytes);
}

/* More places than are kept: one a texture was found in outlasts the ones
 * nothing was ever found in. */
static void test_more_than_are_kept(void) {
  Copy snapshot, page;
  uint64_t tag;
  unsigned i;
  start("more copies than are remembered");
  paint(1);
  snapshot = copy_to(PLACE, 640, 480, 1);
  card_holds(&snapshot);
  CHECK(ask(&snapshot, &tag) == 1, "the snapshot");
  paint(2);
  page = copy_to(PLACE, 256, 256, 0);
  write_back(&snapshot);
  CHECK(ask(&snapshot, &tag) == 2, "found once");
  free(page.bytes);
  /* Twenty more places, each copied twice so that the first copy at each is
   * remembered. */
  for (i = 0; i < 20u; i++) {
    const uint32_t at = OTHER + i * 0x8000u;
    Copy a, b;
    paint(10 + i);
    a = copy_to(at, 64, 64, 0);
    card_holds(&a);
    CHECK(ask(&a, &tag) == 1, "a copy elsewhere");
    b = copy_to(at, 64, 64, 0);
    free(a.bytes);
    free(b.bytes);
  }
  CHECK_U64(stats().made, 21, "all of them remembered in their turn");
  CHECK(ask(&snapshot, &tag) == 2 && tag == snapshot.tag, "the snapshot is still found");
  free(snapshot.bytes);
}

/* What forgets: the cache saying it no longer has the texture, the renderer
 * stopping, and a state loaded - after which the picture is what the bytes
 * give, whatever this run drew before. */
static void test_forgetting(void) {
  Copy snapshot, page;
  GcStateIdentity id;
  GcStateBuffer state;
  char error[256];
  uint64_t tag;
  start("forgetting");
  paint(1);
  snapshot = copy_to(PLACE, 640, 480, 1);
  card_holds(&snapshot);
  CHECK(ask(&snapshot, &tag) == 1, "the snapshot");
  paint(2);
  page = copy_to(PLACE, 256, 256, 0);
  write_back(&snapshot);
  CHECK(ask(&snapshot, &tag) == 2, "found");

  gc_gx_thread_copy_forget(gc_gx_thread_target(machine), snapshot.tag);
  CHECK(ask(&snapshot, &tag) == 0, "the cache has lost the texture: forgotten");
  CHECK_U64(stats().forgotten, 1, "counted");
  free(page.bytes);
  free(snapshot.bytes);

  /* The renderer stops: a copy that stands is bytes, and so is one
   * remembered. */
  start("forgetting when the card's textures go");
  paint(1);
  snapshot = copy_to(PLACE, 640, 480, 1);
  card_holds(&snapshot);
  CHECK(ask(&snapshot, &tag) == 1, "the snapshot");
  paint(2);
  page = copy_to(PLACE, 256, 256, 0);
  card_holds(&page);
  write_back(&snapshot);
  CHECK(ask(&snapshot, &tag) == 2, "found");
  free(page.bytes);
  paint(3);
  page = copy_to(OTHER, 256, 256, 0);
  card_holds(&page);
  CHECK(ask(&page, &tag) == 1, "and a copy standing elsewhere");
  gc_gx_thread_copy_textures_gone(gc_gx_thread_target(machine));
  CHECK(ask(&snapshot, &tag) == 0, "the card's textures gone: nothing is remembered");
  CHECK(ask(&page, &tag) == 0, "and the copy that stands is its bytes");
  CHECK(memcmp(machine->mem1 + page.first, page.bytes, page.end - page.first) == 0, "which are still there");
  free(page.bytes);
  free(snapshot.bytes);

  /* A state saved while the snapshot is being found, and loaded. */
  start("forgetting when a state is loaded");
  paint(1);
  snapshot = copy_to(PLACE, 640, 480, 1);
  card_holds(&snapshot);
  CHECK(ask(&snapshot, &tag) == 1, "the snapshot");
  paint(2);
  page = copy_to(PLACE, 256, 256, 0);
  write_back(&snapshot);
  CHECK(ask(&snapshot, &tag) == 2, "found");
  memset(&state, 0, sizeof(state));
  gc_state_identity(&id, machine, "gc_gx_copies_test", NULL, 0);
  CHECK(gc_state_save_memory(machine, &id, 0, &state) == 0, "a state saved");
  CHECK(gc_state_load_memory(machine, &id, state.data, state.size, error, sizeof(error)) == 0, "and loaded: %s", error);
  CHECK(memcmp(machine->mem1 + snapshot.first, snapshot.bytes, snapshot.end - snapshot.first) == 0,
        "the snapshot's bytes are in the loaded memory");
  draw_reads(snapshot.first, snapshot.end - snapshot.first);
  CHECK(ask(&snapshot, &tag) == 0, "but the texture is its bytes: a load forgets");
  gc_state_buffer_free(&state);
  free(page.bytes);
  free(snapshot.bytes);
}

/* GC_GL_COPY_MEMO=0: nothing is remembered, and a copy stands exactly as
 * long as it did before any of this. */
static void test_switched_off(void) {
  Copy snapshot, page;
  uint64_t tag;
  _putenv("GC_GL_COPY_MEMO=0");
  start("switched off");
  paint(1);
  snapshot = copy_to(PLACE, 640, 480, 1);
  card_holds(&snapshot);
  CHECK(ask(&snapshot, &tag) == 1 && tag == snapshot.tag, "a copy still stands");
  paint(2);
  page = copy_to(PLACE, 256, 256, 0);
  card_holds(&page);
  write_back(&snapshot);
  CHECK(ask(&snapshot, &tag) == 0, "written back, it is its bytes");
  CHECK_U64(stats().made, 0, "nothing remembered");
  CHECK_U64(stats().lookups, 0, "or looked for");
  _putenv("GC_GL_COPY_MEMO=");
  free(page.bytes);
  free(snapshot.bytes);
}

int main(void) {
  /* Whatever the caller was comparing with, the tests choose for themselves. */
  _putenv("GC_GL_COPY_MEMO=");
  test_a_copy_stands();
  test_written_back();
  test_the_newer_picture();
  test_covered_whole();
  test_not_remembered();
  test_pushed_out();
  test_more_than_are_kept();
  test_forgetting();
  test_switched_off();
  if (machine) gc_machine_destroy(machine);
  printf("gc_gx_copies_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
