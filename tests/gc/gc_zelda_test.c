/*
 * The "Zelda" sound microcode's conversation, through the DSP's registers.
 *
 * A game uploads a microcode and talks to it through the mailbox. Which
 * microcode it is decides what it says back, and a game given the wrong
 * greeting waits for ever (Super Mario Galaxy 2 did; docs/gc-dsp-zelda.md).
 * This plays the game's side: it uploads a stand-in microcode that
 * identifies as a member of the family, and holds every word of the answer
 * - the mail, whether it interrupts, and the order - to the document.
 *
 *  - identification: the upload is recognised by its bytes, a microcode
 *    that is not of the family is still answered as AX, and a member that
 *    speaks the early protocol is not claimed;
 *  - the greeting, and the acknowledgement of each command;
 *  - rendering: nothing is rendered until the game has said which voices,
 *    sixteen at a time (or thirty-two, for the members that ask once a
 *    frame), each frame is announced and written, and the end of the frames
 *    is announced once;
 *  - nothing more runs until the game answers the end of the frames;
 *  - going back to the ROM, and a reset of the DSP.
 *
 * No voice here is enabled, so the frames are silence: this checks that the
 * output is written and where. What a voice sounds like is
 * gc_zelda_mix_test's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_zelda.h"

void gc_dsp_advance(GcMachine *m);

static unsigned failures, checks;

#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(cond)) {                                                                                 \
      failures++;                                                                                  \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
      printf(__VA_ARGS__);                                                                         \
      printf("\n");                                                                                \
    }                                                                                              \
  } while (0)

/* The DSP's registers, as offsets from 0xcc000000. */
#define DSP 0x5000u
#define TO_DSP_HI (DSP + 0x00u)
#define TO_DSP_LO (DSP + 0x02u)
#define TO_CPU_HI (DSP + 0x04u)
#define TO_CPU_LO (DSP + 0x06u)
#define CONTROL (DSP + 0x0au)
#define CONTROL_RESET 0x0001u
#define CONTROL_DSP_INT 0x0080u

#define CODE 0x00200000u  /* where the stand-in microcode is put */
#define LEFT 0x00300000u  /* the output buffers */
#define RIGHT 0x00301000u
#define FRAME_BYTES 0xa0u

/* ------------------------------------------------------------ the game's side */

static void send(GcMachine *m, uint32_t mail) {
  gc_io_write(m, TO_DSP_HI, 2, mail >> 16);
  gc_io_write(m, TO_DSP_LO, 2, mail & 0xffffu);
}

/* Lets time pass, as a game's wait loop does. */
static int mail_waiting(GcMachine *m) {
  unsigned i;
  for (i = 0; i < 8u; i++) {
    if (gc_io_read(m, TO_CPU_HI, 2) & 0x8000u) return 1;
    m->cpu.cycles += 20000u;
    gc_dsp_advance(m);
  }
  return (gc_io_read(m, TO_CPU_HI, 2) & 0x8000u) != 0;
}

static int interrupt_pending(GcMachine *m) { return (gc_io_read(m, CONTROL, 2) & CONTROL_DSP_INT) != 0; }

/* The next mail is `mail`, and it interrupts or does not. Whatever it was,
 * it is taken and its interrupt acknowledged, as a handler would. */
static void expect(GcMachine *m, uint32_t mail, int interrupts, const char *what) {
  uint32_t got;
  int raised;
  if (!mail_waiting(m)) {
    CHECK(0, "%s: no mail, wanted %08x", what, mail);
    return;
  }
  raised = interrupt_pending(m);
  got = (gc_io_read(m, TO_CPU_HI, 2) & 0x7fffu) << 16;
  got |= gc_io_read(m, TO_CPU_LO, 2);
  CHECK((got | 0x80000000u) == (mail | 0x80000000u), "%s: mail %08x, wanted %08x", what, got | 0x80000000u, mail);
  CHECK(raised == interrupts, "%s: mail %08x %s, and it should %s", what, mail,
        raised ? "interrupted" : "did not interrupt", interrupts ? "have" : "not have");
  if (raised) gc_io_write(m, CONTROL, 2, CONTROL_DSP_INT);
}

static void expect_nothing(GcMachine *m, const char *what) {
  CHECK(!mail_waiting(m), "%s: mail %04x.... when none was owed", what, gc_io_read(m, TO_CPU_HI, 2));
}

/* ------------------------------------------------------------ the microcode */

/* A stand-in microcode whose identification is `hash`.
 *
 * The identification folds each byte in and rotates left by three, so it
 * can be steered. Some filler; then bytes each chosen to cancel the low
 * eight bits, which after a dozen rotations leaves nothing; then the bytes
 * that build the target from nothing, found by running the same thing
 * backwards from the target until nothing is left and writing them out in
 * the other order. */
static uint32_t forge(uint8_t *out, uint32_t hash) {
  uint8_t build[16];
  uint32_t at, running, h, n;
  for (at = 0; at < 21u; at++) out[at] = (uint8_t)(0x40u + at);
  running = gc_dsp_ucode_hash(out, at);
  for (n = 0; n < 16u && running; n++) {
    out[at] = (uint8_t)running;
    running ^= out[at];
    running = (running << 3) | (running >> 29);
    at++;
  }
  CHECK(running == 0u, "the forward walk did not clear the hash");
  h = hash;
  for (n = 0; n < 16u && h; n++) {
    const uint32_t before = (h >> 3) | (h << 29);
    build[n] = (uint8_t)before;
    h = before ^ build[n];
  }
  CHECK(h == 0u, "the backward walk did not reach nothing");
  while (n--) out[at++] = build[n];
  CHECK(gc_dsp_ucode_hash(out, at) == hash, "the stand-in identifies as %08x, wanted %08x",
        gc_dsp_ucode_hash(out, at), hash);
  return at;
}

/* Uploads a microcode through the ROM, the way the SDK's driver does. */
static void upload(GcMachine *m, uint32_t address, uint32_t length) {
  send(m, 0x80f3a001u);
  send(m, address);
  send(m, 0x80f3c002u);
  send(m, 0x00000000u);
  send(m, 0x80f3a002u);
  send(m, length);
  send(m, 0x80f3b002u);
  send(m, 0x00000000u);
  send(m, 0x80f3d001u);
  send(m, 0x00000010u);
}

static GcMachine *machine_with(uint32_t hash, uint32_t *length_out) {
  GcMachine *m = gc_machine_create();
  uint8_t *code;
  if (!m) {
    printf("gc_zelda_test: no machine\n");
    exit(2);
  }
  code = gc_machine_ram(m, CODE, 256);
  *length_out = forge(code, hash);
  return m;
}

/* A command: how many words, then the words. */
static void command(GcMachine *m, const uint32_t *words, unsigned count) {
  unsigned i;
  send(m, count);
  for (i = 0; i < count; i++) send(m, words[i]);
}

/* "Which voices": the word that says it is coming, then the word. */
static void voices(GcMachine *m, uint32_t word) {
  send(m, 0x00000000u);
  send(m, word);
}

/* ------------------------------------------------------------ the tests */

#define GALAXY 0xd643001fu   /* per sixteen voices; the render command carries two more words */
#define SUNSHINE 0x56d36052u /* asks once a frame */
#define LUIGI 0x42f64ac4u    /* the early protocol */

static void identification(void) {
  uint32_t length;
  uint32_t flags = 0;
  GcMachine *m;

  CHECK(gc_zelda_knows(GALAXY, &flags) && (flags & GC_ZELDA_NO_ARAM) && (flags & GC_ZELDA_COMBINED_CMD_0D) &&
            !(flags & GC_ZELDA_SYNC_PER_FRAME),
        "Super Mario Galaxy's microcode and its traits (%x)", flags);
  CHECK(gc_zelda_knows(SUNSHINE, &flags) && (flags & GC_ZELDA_SYNC_PER_FRAME) && !(flags & GC_ZELDA_NO_ARAM),
        "Super Mario Sunshine's microcode and its traits (%x)", flags);
  CHECK(!gc_zelda_knows(0x12345678u, NULL), "a hash that is nobody's");
  {
    static const uint8_t bytes[4] = {0x01, 0x02, 0x03, 0x04};
    /* By hand: ((((1 rotl 3) ^ 2) rotl 3) ^ 3) rotl 3 ... */
    uint32_t h = 0;
    unsigned i;
    for (i = 0; i < 4u; i++) h = ((h ^ bytes[i]) << 3) | ((h ^ bytes[i]) >> 29);
    CHECK(gc_dsp_ucode_hash(bytes, 4) == h && h == 0x000014e0u, "the identification of 01 02 03 04 is %08x",
          gc_dsp_ucode_hash(bytes, 4));
  }

  /* Not of the family: answered as AX always was, with one word. */
  m = machine_with(0x12345678u, &length);
  upload(m, CODE, length);
  CHECK(!gc_zelda_active(m), "a microcode nobody knows is not taken for one of the family");
  expect(m, 0xdcd10000u, 1, "AX's greeting");
  expect_nothing(m, "after AX's greeting");
  gc_machine_destroy(m);

  /* Of the family, but the early protocol: not claimed. */
  m = machine_with(LUIGI, &length);
  upload(m, CODE, length);
  CHECK(!gc_zelda_active(m), "a member that speaks the early protocol is not claimed");
  gc_machine_destroy(m);
}

static void conversation(void) {
  uint32_t length;
  GcMachine *m = machine_with(GALAXY, &length);
  uint8_t *left = gc_machine_ram(m, LEFT, 4u * FRAME_BYTES);
  uint8_t *right = gc_machine_ram(m, RIGHT, 4u * FRAME_BYTES);
  unsigned i;

  memset(left, 0xaa, 4u * FRAME_BYTES);
  memset(right, 0xaa, 4u * FRAME_BYTES);

  upload(m, CODE, length);
  CHECK(gc_zelda_active(m), "the upload is recognised as one of the family");
  expect(m, 0xdcd10000u, 1, "the greeting's first word");
  expect(m, 0xf3551111u, 0, "the greeting's second word");
  expect_nothing(m, "after the greeting");

  /* Set-up: thirty-two voices a frame, and four addresses. */
  {
    const uint32_t setup[5] = {0x81aa0020u, 0x00400000u, 0x00410000u, 0x00420000u, 0x00430000u};
    command(m, setup, 5);
    expect(m, 0xdcd10004u, 1, "set-up is acknowledged");
    expect(m, 0xf35581aau, 0, "with the command's own number");
  }
  /* Where the samples are, on a console with no audio memory. */
  {
    const uint32_t base[2] = {0x8e010000u, 0x10000000u};
    command(m, base, 2);
    expect(m, 0xdcd10004u, 1, "the sample base is acknowledged");
    expect(m, 0xf3558e01u, 0, "with its number");
  }
  /* A command that does nothing is still answered. */
  {
    const uint32_t nothing[1] = {0x80070000u};
    command(m, nothing, 1);
    expect(m, 0xdcd10004u, 1, "a command that does nothing is acknowledged");
    expect(m, 0xf3558007u, 0, "with its number");
  }

  /* Render two frames. Nothing happens until the game says which voices. */
  {
    const uint32_t render[5] = {0x82020100u, LEFT, RIGHT, 0u, 0u};
    command(m, render, 5);
    expect_nothing(m, "a render command before any voices are named");
    CHECK(left[0] == 0xaa, "nothing is written before a frame is finished");

    voices(m, 0x0000ffffu); /* voices 0 to 15 */
    expect_nothing(m, "half the frame's voices");
    voices(m, 0x0001ffffu); /* voices 16 to 31 */
    expect(m, 0xdcd10004u, 1, "the first frame is announced");
    expect(m, 0xf355ff00u, 0, "as frame 0");
    for (i = 0; i < FRAME_BYTES; i++) {
      if (left[i] || right[i]) break;
    }
    CHECK(i == FRAME_BYTES, "the first frame's left and right are written (as silence)");
    CHECK(left[FRAME_BYTES] == 0xaa && right[FRAME_BYTES] == 0xaa, "and nothing past them yet");

    /* A command sent while frames are owed waits its turn. */
    {
      const uint32_t waiting[1] = {0x80330000u};
      command(m, waiting, 1);
      expect_nothing(m, "a command sent in the middle of rendering");
    }

    voices(m, 0x00000000u); /* none of 0 to 15 */
    voices(m, 0x00018000u); /* only voice 16 */
    expect(m, 0xdcd10004u, 1, "the second frame is announced");
    expect(m, 0xf355ff01u, 0, "as frame 1");
    expect(m, 0xdcd10005u, 1, "and the end of the frames, once");
    expect_nothing(m, "after the end of the frames");
    for (i = 0; i < 2u * FRAME_BYTES; i++) {
      if (left[i] || right[i]) break;
    }
    CHECK(i == 2u * FRAME_BYTES, "the second frame follows the first in memory");
    CHECK(left[2u * FRAME_BYTES] == 0xaa && right[2u * FRAME_BYTES] == 0xaa, "and two frames is all that is written");
    CHECK(gc_zelda_frames(m) == 2u, "two frames were rendered (%llu)", (unsigned long long)gc_zelda_frames(m));

    /* The command that waited runs when the game answers the end. */
    send(m, 0xcdd10003u);
    expect(m, 0xdcd10004u, 1, "the waiting command is acknowledged after the game's answer");
    expect(m, 0xf3558033u, 0, "with its number");
    expect_nothing(m, "after the waiting command");
    CHECK(gc_zelda_commands(m) == 5u, "five commands were taken (%llu)", (unsigned long long)gc_zelda_commands(m));
  }

  /* A frame with no voices at all needs no asking. */
  {
    const uint32_t setup[5] = {0x81010000u, 0x00400000u, 0x00410000u, 0x00420000u, 0x00430000u};
    const uint32_t render[5] = {0x82010100u, LEFT, RIGHT, 0u, 0u};
    command(m, setup, 5);
    expect(m, 0xdcd10004u, 1, "a second set-up");
    expect(m, 0xf3558101u, 0, "its number");
    command(m, render, 5);
    expect(m, 0xdcd10004u, 1, "a frame of no voices is announced at once");
    expect(m, 0xf355ff00u, 0, "as frame 0");
    expect(m, 0xdcd10005u, 1, "and ended");
    send(m, 0xcdd10003u);
    expect_nothing(m, "after answering with nothing waiting");
  }

  /* Back to the ROM, which says what it says out of reset. */
  send(m, 0xcdd10002u);
  CHECK(!gc_zelda_active(m), "sent back to the ROM, the microcode is not running");
  expect(m, 0x8071feedu, 0, "the ROM's word");

  /* And a new upload starts from nothing. */
  upload(m, CODE, length);
  expect(m, 0xdcd10000u, 1, "the greeting again");
  expect(m, 0xf3551111u, 0, "and its second word");
  CHECK(gc_zelda_frames(m) == 0u, "a new upload has rendered nothing");

  /* A reset of the DSP drops what the microcode owed. */
  {
    const uint32_t nothing[1] = {0x80070000u};
    command(m, nothing, 1);
    gc_io_write(m, CONTROL, 2, CONTROL_RESET);
    CHECK(!gc_zelda_active(m), "a reset stops the microcode");
    expect(m, 0x8071feedu, 0, "the ROM's word after a reset, not the acknowledgement that was owed");
    expect_nothing(m, "after a reset");
  }
  gc_machine_destroy(m);
}

/* The members that ask which voices once a frame: two words, thirty-two
 * voices each, and no word before them after the first. */
static void once_a_frame(void) {
  uint32_t length;
  GcMachine *m = machine_with(SUNSHINE, &length);
  const uint32_t setup[5] = {0x81010040u, 0x00400000u, 0x00410000u, 0x00420000u, 0x00430000u};
  const uint32_t render[3] = {0x82010100u, LEFT, RIGHT};

  upload(m, CODE, length);
  expect(m, 0xdcd10000u, 1, "the greeting's first word");
  expect(m, 0xf3551111u, 0, "the greeting's second word");
  command(m, setup, 5);
  expect(m, 0xdcd10004u, 1, "set-up is acknowledged");
  expect(m, 0xf3558101u, 0, "with its number");
  command(m, render, 3); /* no two extra words on this member */
  expect_nothing(m, "a render command before any voices are named");
  send(m, 0x00000000u);  /* "which voices" is coming */
  send(m, 0xffffffffu);  /* voices 0 to 31 */
  expect_nothing(m, "half the frame's voices");
  send(m, 0xffffffffu);  /* voices 32 to 63 */
  expect(m, 0xdcd10004u, 1, "the frame is announced");
  expect(m, 0xf355ff00u, 0, "as frame 0");
  expect(m, 0xdcd10005u, 1, "and the end of the frames");
  gc_machine_destroy(m);
}

int main(void) {
  identification();
  conversation();
  once_a_frame();
  if (failures) {
    printf("gc_zelda_test: %u of %u checks failed\n", failures, checks);
    return 1;
  }
  printf("gc_zelda_test: %u checks: the Zelda microcode is recognised by its bytes, greets, acknowledges, asks "
         "which voices and announces its frames as docs/gc-dsp-zelda.md says\n",
         checks);
  return 0;
}
