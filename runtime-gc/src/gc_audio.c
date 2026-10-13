/*
 * The audio DMA: see gc_audio.h for what it is and why it matters before
 * there is anything to hear.
 */
#include "gc_audio.h"

#include <stdlib.h>
#include <string.h>

#include "gc.h"

uint32_t *gc_io_slot(struct GcMachine *m, const char *block, uint32_t offset);
void gc_dsp_audio_interrupt(struct GcMachine *m);

/* Thirty-two bytes is the unit everywhere in this engine: the address is
 * aligned to it, the length counts it, and the remaining count is in it. It
 * is also exactly eight stereo frames of sixteen-bit samples, which is what
 * makes the arithmetic below come out in whole frames. */
#define GC_AID_BLOCK 32u
#define GC_AID_FRAMES_PER_BLOCK 8u

/* A second of sound, which is far more than a frontend should ever be behind
 * by and cheap enough not to think about. */
#define GC_AUDIO_RING_FRAMES 48000u

/* The rate the engine plays at lives in the *audio interface's* control
 * register rather than in the engine's own, and the bit is inverted: clear
 * means 48 kHz and set means 32 kHz. That inversion is the kind of thing
 * that produces a machine whose sound is a semitone and a half out and whose
 * game logic is paced 50% wrong, so it is worth being explicit about. */
static unsigned aid_rate(const struct GcMachine *m) {
  /* The slot the machine already resolved for its own use, because this runs
   * between every pair of instructions while a game is making a noise and a
   * lookup by name there is the most expensive thing in the machine. */
  const uint32_t *control = m->hot_slots_known && m->ai_control_slot != 0xffffffffu
                                ? &m->io[m->ai_control_slot]
                                : gc_io_slot((struct GcMachine *)m, "ai", 0x00);
  return (control && (*control & 0x40u)) ? 32000u : 48000u;
}

static uint64_t aid_cycles_per_block(const struct GcMachine *m) {
  return ((uint64_t)GC_CPU_HZ * GC_AID_FRAMES_PER_BLOCK) / aid_rate(m);
}

/* The address and the length the guest programmed, which are not the same as
 * the ones the engine is currently using: the point of the arrangement is
 * that a game writes the *next* buffer's address while the current one is
 * still playing. */
static uint32_t aid_programmed_address(struct GcMachine *m) {
  const uint32_t *high = gc_io_slot(m, "dsp", 0x30);
  const uint32_t *low = gc_io_slot(m, "dsp", 0x32);
  if (!high || !low) return 0;
  return gc_ram_from_register(((*high & 0x1fffu) << 16) | (*low & 0xffe0u));
}

static uint32_t aid_programmed_blocks(struct GcMachine *m) {
  const uint32_t *length = gc_io_slot(m, "dsp", 0x36);
  return length ? (*length & 0x7fffu) : 0u;
}

uint32_t gc_audio_blocks_left(const struct GcMachine *m) { return m ? m->aid_blocks_left : 0u; }

uint64_t gc_audio_overruns(const struct GcMachine *m) { return m ? m->audio_overruns : 0u; }

int gc_audio_playing(const struct GcMachine *m) { return m && m->aid_running; }

unsigned gc_audio_rate(const struct GcMachine *m) { return m ? aid_rate(m) : 32000u; }

/* One frame into the ring. The oldest frame is dropped when the host has
 * stopped taking them, rather than the newest: what a listener wants when
 * something has gone wrong is the sound that is happening now. */
static void ring_put(struct GcMachine *m, int16_t left, int16_t right) {
  unsigned next;
  if (!m->audio_ring) {
    m->audio_ring = (int16_t *)calloc(GC_AUDIO_RING_FRAMES * 2u, sizeof(int16_t));
    if (!m->audio_ring) return;
    m->audio_write = m->audio_read = 0;
  }
  next = (m->audio_write + 1u) % GC_AUDIO_RING_FRAMES;
  if (next == m->audio_read) {
    m->audio_read = (m->audio_read + 1u) % GC_AUDIO_RING_FRAMES;
    m->audio_overruns++;
  }
  m->audio_ring[m->audio_write * 2u] = left;
  m->audio_ring[m->audio_write * 2u + 1u] = right;
  m->audio_write = next;
}

unsigned gc_audio_take(struct GcMachine *m, int16_t *out, unsigned frames) {
  unsigned taken = 0;
  if (!m || !out || !m->audio_ring) return 0;
  while (taken < frames && m->audio_read != m->audio_write) {
    out[taken * 2u] = m->audio_ring[m->audio_read * 2u];
    out[taken * 2u + 1u] = m->audio_ring[m->audio_read * 2u + 1u];
    m->audio_read = (m->audio_read + 1u) % GC_AUDIO_RING_FRAMES;
    taken++;
  }
  return taken;
}

/* One block: eight frames out of main memory and into the ring.
 *
 * The two samples in a frame are the right channel and then the left, which
 * is the one order nobody guesses. **Unverified against hardware** - it rests
 * on a second implementation rather than on a measurement, and getting it
 * backwards produces a perfectly plausible stereo image with the channels
 * swapped, which is exactly the kind of mistake that survives for years.
 * docs/gc-dsp-ax.md D.2 says what would settle it. */
static void aid_play_block(struct GcMachine *m) {
  const uint8_t *p = gc_ram_ptr(m, m->aid_address, GC_AID_BLOCK);
  unsigned frame;
  for (frame = 0; frame < GC_AID_FRAMES_PER_BLOCK; frame++) {
    int16_t right = 0, left = 0;
    if (p) {
      const uint8_t *f = p + frame * 4u;
      right = (int16_t)(((uint16_t)f[0] << 8) | f[1]);
      left = (int16_t)(((uint16_t)f[2] << 8) | f[3]);
    }
    ring_put(m, left, right);
  }
  m->aid_address = m->aid_address + GC_AID_BLOCK;
}

void gc_audio_advance(struct GcMachine *m) {
  uint64_t per_block;
  if (!m || !m->aid_running) return;
  per_block = aid_cycles_per_block(m);
  if (!per_block) return;
  /* A guard against catching up a very long way at once: if the machine has
   * been stopped in a debugger for a minute, playing a minute of sound is
   * the wrong answer. */
  if (m->cpu.cycles > m->aid_next_block_cycles + per_block * 4096u) {
    m->aid_next_block_cycles = m->cpu.cycles;
  }
  while (m->cpu.cycles >= m->aid_next_block_cycles) {
    m->aid_next_block_cycles += per_block;
    aid_play_block(m);
    if (m->aid_blocks_left) m->aid_blocks_left--;
    if (m->aid_blocks_left) continue;
    /* The end of the buffer. The engine does not stop: it reloads both the
     * address and the count from what the guest last programmed and plays on,
     * and raises the interrupt that tells the game to program the next one.
     * A game that has not got round to it hears the same buffer again, which
     * is the right failure - a repeated fragment rather than a silence. */
    m->aid_address = aid_programmed_address(m);
    m->aid_blocks_left = aid_programmed_blocks(m);
    gc_dsp_audio_interrupt(m);
    if (!m->aid_blocks_left) {
      m->aid_running = 0;
      break;
    }
  }
}

uint64_t gc_audio_next_event(const struct GcMachine *m, uint64_t next) {
  if (m && m->aid_running && m->aid_next_block_cycles < next) return m->aid_next_block_cycles;
  return next;
}

void gc_audio_length_write(struct GcMachine *m, uint32_t value) {
  const int enabled = (value & 0x8000u) != 0;
  if (!m) return;
  if (enabled && !m->aid_running) {
    m->aid_address = aid_programmed_address(m);
    m->aid_blocks_left = value & 0x7fffu;
    m->aid_next_block_cycles = m->cpu.cycles + aid_cycles_per_block(m);
    m->aid_running = m->aid_blocks_left != 0;
  } else if (!enabled) {
    m->aid_running = 0;
  }
}

void gc_audio_reset(struct GcMachine *m) {
  if (!m) return;
  m->aid_address = 0;
  m->aid_blocks_left = 0;
  m->aid_next_block_cycles = 0;
  m->aid_running = 0;
  m->audio_read = m->audio_write = 0;
  m->audio_overruns = 0;
}

void gc_audio_free(struct GcMachine *m) {
  if (!m) return;
  free(m->audio_ring);
  m->audio_ring = NULL;
  m->audio_read = m->audio_write = 0;
}

/* ------------------------------------------------------------ save states */

#include "gc_state.h"

/* The ring is part of the state: what was played and not yet taken is
 * sound the resumed run has to hand over, and the ring's indices are in
 * GcMachine. */
void gc_audio_state_save(struct GcMachine *m, GcStateBuffer *b) {
  gc_state_put_u32(b, m->audio_ring ? 1u : 0u);
  gc_state_put_u32(b, GC_AUDIO_RING_FRAMES);
  if (m->audio_ring) gc_state_put(b, m->audio_ring, GC_AUDIO_RING_FRAMES * 2u * sizeof(int16_t));
}

int gc_audio_state_load(struct GcMachine *m, const uint8_t *payload, size_t size, int apply) {
  uint32_t present, frames;
  const size_t bytes = GC_AUDIO_RING_FRAMES * 2u * sizeof(int16_t);
  if (size < 8u) return -1;
  memcpy(&present, payload, 4u);
  memcpy(&frames, payload + 4u, 4u);
  if (present > 1u || frames != GC_AUDIO_RING_FRAMES || size != 8u + (present ? bytes : 0u)) return -1;
  if (!apply) return 0;
  if (!present) {
    free(m->audio_ring);
    m->audio_ring = NULL;
    return 0;
  }
  if (!m->audio_ring) m->audio_ring = (int16_t *)calloc(GC_AUDIO_RING_FRAMES * 2u, sizeof(int16_t));
  if (!m->audio_ring) return -1;
  memcpy(m->audio_ring, payload + 8u, bytes);
  return 0;
}
