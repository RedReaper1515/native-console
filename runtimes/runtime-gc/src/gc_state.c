/*
 * Save states. See gc_state.h for the format and for what is and is not in
 * one; docs/GC-PLAN.md ("Save states") for how exactness was tested.
 */
#include "gc_state.h"
#include "wii.h"
#include "wii_nand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "gc.h"
#include "gc_disc.h"
#include "gc_gl.h"
#include "gc_gx_thread.h"

/* ------------------------------------------------------------------ */
/* Buffers and hashing                                                 */

static int reserve(GcStateBuffer *b, size_t more) {
  size_t want;
  uint8_t *grown;
  if (b->failed) return -1;
  if (b->size + more <= b->capacity) return 0;
  want = b->capacity ? b->capacity : 4096u;
  while (want < b->size + more) want *= 2u;
  grown = (uint8_t *)realloc(b->data, want);
  if (!grown) {
    b->failed = 1;
    return -1;
  }
  b->data = grown;
  b->capacity = want;
  return 0;
}

void gc_state_put(GcStateBuffer *b, const void *bytes, size_t size) {
  if (!size || reserve(b, size) != 0) return;
  if (bytes) memcpy(b->data + b->size, bytes, size);
  else memset(b->data + b->size, 0, size);
  b->size += size;
}

void gc_state_put_u32(GcStateBuffer *b, uint32_t value) { gc_state_put(b, &value, sizeof(value)); }
void gc_state_put_u64(GcStateBuffer *b, uint64_t value) { gc_state_put(b, &value, sizeof(value)); }

void gc_state_buffer_free(GcStateBuffer *b) {
  free(b->data);
  memset(b, 0, sizeof(*b));
}

/* A word at a time: forty megabytes is hashed on every save and load, and a
 * byte-wise FNV of it is a noticeable fraction of the time. */
static uint64_t hash_bytes(uint64_t h, const uint8_t *p, size_t n) {
  while (n >= 8u) {
    uint64_t w;
    memcpy(&w, p, 8u);
    h = (h ^ w) * 0x9e3779b97f4a7c15ull;
    h ^= h >> 32;
    p += 8u;
    n -= 8u;
  }
  while (n--) {
    h = (h ^ *p++) * 0x100000001b3ull;
    h ^= h >> 29;
  }
  return h;
}

#define HASH_SEED 0xcbf29ce484222325ull

/* ------------------------------------------------------------------ */
/* Compression                                                         */

/* A small LZ77: literals, then a match of at least four bytes within the
 * last 64 KB. Nothing is vendored in this repository that compresses, and
 * this is what a state needs - main memory, the framebuffer planes and an
 * ARAM the game has only partly filled are long runs and repeats, which
 * this finds at a few hundred megabytes a second. Each sequence is
 *
 *   varint literal count, the literals, varint match length (0 = none),
 *   and for a match a 16-bit little-endian distance back.
 */
static void put_varint(GcStateBuffer *b, size_t value) {
  uint8_t bytes[10];
  unsigned n = 0;
  do {
    uint8_t byte = (uint8_t)(value & 0x7fu);
    value >>= 7;
    if (value) byte |= 0x80u;
    bytes[n++] = byte;
  } while (value);
  gc_state_put(b, bytes, n);
}

static void lz_sequence(GcStateBuffer *out, const uint8_t *literals, size_t count, size_t match, size_t distance) {
  put_varint(out, count);
  gc_state_put(out, literals, count);
  put_varint(out, match);
  if (match) {
    uint8_t d[2];
    d[0] = (uint8_t)distance;
    d[1] = (uint8_t)(distance >> 8);
    gc_state_put(out, d, 2u);
  }
}

#define LZ_HASH_BITS 16u

static int lz_compress(const uint8_t *src, size_t n, GcStateBuffer *out) {
  uint32_t *table = (uint32_t *)calloc((size_t)1u << LZ_HASH_BITS, sizeof(uint32_t));
  size_t i = 0, anchor = 0;
  const size_t limit = n > 16u ? n - 16u : 0u;
  if (!table || n > 0xfff00000u) {
    free(table);
    return -1;
  }
  reserve(out, n / 2u + 64u);
  while (i < limit) {
    uint32_t v, c;
    size_t candidate;
    memcpy(&v, src + i, 4u);
    {
      const uint32_t h = (v * 2654435761u) >> (32u - LZ_HASH_BITS);
      candidate = table[h];
      table[h] = (uint32_t)i + 1u;
    }
    if (candidate && i - (candidate - 1u) <= 0xffffu) {
      candidate--;
      memcpy(&c, src + candidate, 4u);
      if (c == v) {
        size_t length = 4u;
        while (i + length + 8u <= n) {
          uint64_t x, y;
          memcpy(&x, src + candidate + length, 8u);
          memcpy(&y, src + i + length, 8u);
          if (x != y) break;
          length += 8u;
        }
        while (i + length < n && src[candidate + length] == src[i + length]) length++;
        lz_sequence(out, src + anchor, i - anchor, length, i - candidate);
        i += length;
        anchor = i;
        continue;
      }
    }
    /* Incompressible stretches (sampled sound in ARAM) are skipped over
     * faster the longer they go on. */
    i += 1u + ((i - anchor) >> 10);
  }
  lz_sequence(out, src + anchor, n - anchor, 0u, 0u);
  free(table);
  return out->failed ? -1 : 0;
}

static int get_varint(const uint8_t *in, size_t size, size_t *at, size_t *value) {
  size_t v = 0;
  unsigned shift = 0;
  for (;;) {
    uint8_t byte;
    if (*at >= size || shift > 63u) return -1;
    byte = in[(*at)++];
    v |= (size_t)(byte & 0x7fu) << shift;
    if (!(byte & 0x80u)) break;
    shift += 7u;
  }
  *value = v;
  return 0;
}

static int lz_decompress(const uint8_t *in, size_t size, uint8_t *out, size_t out_size) {
  size_t at = 0, written = 0;
  while (at < size) {
    size_t count, match;
    if (get_varint(in, size, &at, &count) != 0 || count > size - at || count > out_size - written) return -1;
    memcpy(out + written, in + at, count);
    at += count;
    written += count;
    if (get_varint(in, size, &at, &match) != 0) return -1;
    if (match) {
      size_t distance, k;
      if (size - at < 2u) return -1;
      distance = (size_t)in[at] | ((size_t)in[at + 1u] << 8);
      at += 2u;
      if (!distance || distance > written || match > out_size - written) return -1;
      if (distance >= match) {
        memcpy(out + written, out + written - distance, match);
      } else if (distance == 1u) {
        memset(out + written, out[written - 1u], match);
      } else {
        for (k = 0; k < match; k++) out[written + k] = out[written + k - distance];
      }
      written += match;
    }
  }
  return written == out_size ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Identity and layout                                                 */

void gc_state_identity(GcStateIdentity *out, const GcMachine *m, const char *name, const uint8_t *image,
                       size_t image_size) {
  const GcDisc *disc = m ? m->disc : NULL;
  memset(out, 0, sizeof(*out));
  if (disc && disc->game_code[0]) {
    snprintf(out->game_id, sizeof(out->game_id), "%s", disc->game_code);
  } else if (name) {
    snprintf(out->game_id, sizeof(out->game_id), "%s", name);
  }
  if (image && image_size) out->program_hash = hash_bytes(HASH_SEED, image, image_size) | 1u;
  if (disc) {
    uint64_t h = HASH_SEED;
    h = hash_bytes(h, (const uint8_t *)disc->game_code, sizeof(disc->game_code));
    h = hash_bytes(h, (const uint8_t *)disc->name, strlen(disc->name));
    h = hash_bytes(h, (const uint8_t *)&disc->dol_offset, sizeof(disc->dol_offset));
    h = hash_bytes(h, (const uint8_t *)&disc->fst_offset, sizeof(disc->fst_offset));
    h = hash_bytes(h, (const uint8_t *)&disc->fst_size, sizeof(disc->fst_size));
    if (disc->fst && disc->fst_size) h = hash_bytes(h, disc->fst, disc->fst_size);
    out->disc_hash = h | 1u;
  }
}

/* The sizes and offsets of everything written whole. Two builds that agree
 * on all of these read each other's MACH section the same way; a field
 * added, removed, resized or moved changes one of them. */
/* The format version is the first thing hashed, so a state saved in one
 * format never passes the layout check of another by accident. It is a
 * parameter rather than the constant because a reader has to be able to work
 * out what an older state's fingerprint should have been: format 2 changed
 * the header alone and left every structure where it was, so a format 1
 * state still loads - and it loads only if its layout matches what format 1
 * would have hashed. */
static uint64_t layout_fingerprint(unsigned version) {
  const size_t values[] = {
      (size_t)version,
      sizeof(void *),
      sizeof(GcMachine),
      sizeof(GcCpu),
      sizeof(GcGx),
      sizeof(GcEfb),
      sizeof(GcPad),
      sizeof(GcFpr),
      GC_MEM1_SIZE,
      GC_ARAM_SIZE,
      GC_EFB_WIDTH,
      GC_EFB_HEIGHT,
      GC_IO_STORAGE,
      GC_LOCKED_CACHE_SIZE,
      offsetof(GcCpu, gpr),
      offsetof(GcCpu, fpr),
      offsetof(GcCpu, pc),
      offsetof(GcCpu, fpscr),
      offsetof(GcCpu, gqr),
      offsetof(GcCpu, sr),
      offsetof(GcCpu, dbat),
      offsetof(GcCpu, reserve),
      offsetof(GcCpu, cycles),
      offsetof(GcCpu, dec_write_cycles),
      offsetof(GcCpu, next_event),
      offsetof(GcCpu, branched),
      offsetof(GcCpu, stop_insn),
      offsetof(GcCpu, fp_lazy),
      offsetof(GcCpu, fp_lazy_result),
      offsetof(GcCpu, pending_vector),
      offsetof(GcCpu, machine),
      offsetof(GcCpu, fast),
      offsetof(GcCpu, fast_ready),
      offsetof(GcMachine, mem1),
      offsetof(GcMachine, aram),
      offsetof(GcMachine, efb),
      offsetof(GcMachine, efb.color),
      offsetof(GcMachine, efb.depth),
      offsetof(GcMachine, efb.copies),
      offsetof(GcMachine, efb.display_height),
      offsetof(GcMachine, disc),
      offsetof(GcMachine, locked_cache),
      offsetof(GcMachine, io),
      offsetof(GcMachine, pi_cause),
      offsetof(GcMachine, gl),
      offsetof(GcMachine, pads),
      offsetof(GcMachine, card),
      offsetof(GcMachine, exi_status),
      offsetof(GcMachine, exi_selected),
      offsetof(GcMachine, aid_address),
      offsetof(GcMachine, aid_running),
      offsetof(GcMachine, audio_ring),
      offsetof(GcMachine, audio_overruns),
      offsetof(GcMachine, ai_started_cycles),
      offsetof(GcMachine, field),
      offsetof(GcMachine, advanced_cycles),
      offsetof(GcMachine, vi_pixel),
      offsetof(GcMachine, vi_interrupt_slots_known),
      offsetof(GcMachine, hot_slots_known),
      offsetof(GcMachine, dsp_mail_to_cpu),
      offsetof(GcMachine, dsp_mail_due_cycles),
      offsetof(GcMachine, ax),
      offsetof(GcMachine, dsp_init_code_clears),
      offsetof(GcMachine, di_status),
      offsetof(GcMachine, unknown_disc_commands),
      offsetof(GcMachine, gather),
      offsetof(GcMachine, gather_bursts),
      offsetof(GcMachine, gx),
      offsetof(GcMachine, gx.bp),
      offsetof(GcMachine, gx.xf_memory),
      offsetof(GcMachine, gx.array_stride),
      offsetof(GcMachine, gx.tev),
      offsetof(GcMachine, gx.tlut),
      offsetof(GcMachine, gx.tlut_source),
      offsetof(GcMachine, gx.assembly),
      offsetof(GcMachine, gx.assembly_parity),
      offsetof(GcMachine, gx.commands),
      offsetof(GcMachine, gx.pixels),
      offsetof(GcMachine, gx.on_primitive),
      offsetof(GcMachine, gx.on_triangle),
      offsetof(GcMachine, pe_control),
      offsetof(GcMachine, osreport_addr),
      offsetof(GcMachine, osreport_size),
      offsetof(GcMachine, unknown_io_reads),
      offsetof(GcMachine, trace_io),
      offsetof(GcMachine, stop_on_bad_access),
      offsetof(GcMachine, profile),
      offsetof(GcMachine, profile_every),
      offsetof(GcMachine, coverage),
      offsetof(GcMachine, stop_after),
      offsetof(GcMachine, stop_at),
      offsetof(GcMachine, branch_from),
      offsetof(GcMachine, branch_next),
      offsetof(GcMachine, keep_branch_history),
      offsetof(GcMachine, watch_address),
      offsetof(GcMachine, log),
      offsetof(GcMachine, finished),
      offsetof(GcMachine, exit_code),
  };
  size_t i;
  uint64_t h = HASH_SEED;
  for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    const uint64_t v = (uint64_t)values[i];
    h = hash_bytes(h, (const uint8_t *)&v, sizeof(v));
  }
  return h;
}

static void build_string(char *out, size_t size) {
#if defined(_MSC_VER)
  snprintf(out, size, "MSVC %d %s, %s %s", _MSC_VER, sizeof(void *) == 8 ? "x64" : "x86", __DATE__, __TIME__);
#elif defined(__clang__)
  snprintf(out, size, "clang %d.%d, %s %s", __clang_major__, __clang_minor__, __DATE__, __TIME__);
#elif defined(__GNUC__)
  snprintf(out, size, "gcc %d.%d, %s %s", __GNUC__, __GNUC_MINOR__, __DATE__, __TIME__);
#else
  snprintf(out, size, "unknown compiler, %s %s", __DATE__, __TIME__);
#endif
}

/* ------------------------------------------------------------------ */
/* The machine structure                                               */

/* What in GcMachine is the process's rather than the machine's: pointers,
 * the host settings the runner and the frontend chose, and the diagnostics.
 * Zeroed in what is written, and kept from the loading machine on load. */
static void scrub(GcMachine *copy, int for_hash) {
  GcCpu *const c = &copy->cpu;
  c->machine = NULL;
  memset(c->fast, 0, sizeof(c->fast));
  c->fast_ready = 0;
  /* Not state: every entry to recompiled code clears it (gc.h). */
  c->next_event = 0;
  if (for_hash) {
    /* FPSCR as the guest would read it: the lazy note is a representation,
     * and the interpreter and recompiled code can hold it differently. */
    c->fpscr = gc_cpu_fpscr(c);
    c->fp_lazy = 0;
    c->fp_lazy_a = c->fp_lazy_b = c->fp_lazy_c = c->fp_lazy_result = 0;
    c->branched = 0;
    /* How many pixels the reference rasteriser wrote: a statistic of the
     * host's drawing, which no register returns and the OpenGL path does not
     * keep, so the same machine drawn either way hashes the same. */
    copy->gx.pixels = 0;
  }
  copy->mem1 = NULL;
  copy->aram = NULL;
  copy->efb.color = NULL;
  copy->efb.depth = NULL;
  copy->disc = NULL;
  copy->gl = NULL;
  copy->pads = NULL;
  copy->card = NULL;
  copy->audio_ring = NULL;
  copy->ax = NULL;
  copy->gx.on_primitive = NULL;
  copy->gx.on_triangle = NULL;
  copy->osreport_addr = 0;
  copy->osreport_size = 0;
  copy->trace_io = 0;
  copy->stop_on_bad_access = 0;
  copy->profile = NULL;
  copy->profile_every = 0;
  copy->coverage = NULL;
  copy->stop_after = 0;
  copy->stop_at = 0;
  memset(copy->branch_from, 0, sizeof(copy->branch_from));
  memset(copy->branch_to, 0, sizeof(copy->branch_to));
  memset(copy->branch_repeat, 0, sizeof(copy->branch_repeat));
  copy->branch_next = 0;
  copy->keep_branch_history = 0;
  copy->watch_address = 0;
  copy->log = NULL;
}

static void keep_host(GcMachine *to, const GcMachine *from) {
  to->cpu.machine = from->cpu.machine;
  gc_cpu_forget_fast(&to->cpu);
  to->cpu.next_event = 0;
  to->mem1 = from->mem1;
  to->aram = from->aram;
  to->efb.color = from->efb.color;
  to->efb.depth = from->efb.depth;
  to->disc = from->disc;
  to->gl = from->gl;
  to->pads = from->pads;
  to->card = from->card;
  to->audio_ring = from->audio_ring;
  to->ax = from->ax;
  to->gx.on_primitive = from->gx.on_primitive;
  to->gx.on_triangle = from->gx.on_triangle;
  to->osreport_addr = from->osreport_addr;
  to->osreport_size = from->osreport_size;
  to->trace_io = from->trace_io;
  to->stop_on_bad_access = from->stop_on_bad_access;
  to->profile = from->profile;
  to->profile_every = from->profile_every;
  to->coverage = from->coverage;
  to->stop_after = from->stop_after;
  to->stop_at = from->stop_at;
  memcpy(to->branch_from, from->branch_from, sizeof(to->branch_from));
  memcpy(to->branch_to, from->branch_to, sizeof(to->branch_to));
  memcpy(to->branch_repeat, from->branch_repeat, sizeof(to->branch_repeat));
  to->branch_next = from->branch_next;
  to->keep_branch_history = from->keep_branch_history;
  to->watch_address = from->watch_address;
  to->log = from->log;
}

/* ------------------------------------------------------------------ */
/* Sections                                                            */

#define TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define TAG_MACH TAG('M', 'A', 'C', 'H')
#define TAG_MEM1 TAG('M', 'E', 'M', '1')
#define TAG_ARAM TAG('A', 'R', 'A', 'M')
#define TAG_EFBC TAG('E', 'F', 'B', 'C')
#define TAG_EFBZ TAG('E', 'F', 'B', 'Z')
#define TAG_PADS TAG('P', 'A', 'D', 'S')
#define TAG_CARD TAG('C', 'A', 'R', 'D')
#define TAG_AX TAG('A', 'X', ' ', ' ')
#define TAG_AUDI TAG('A', 'U', 'D', 'I')
#define TAG_WII TAG('W', 'I', 'I', ' ')
#define TAG_NAND TAG('N', 'A', 'N', 'D')
#define TAG_END TAG('E', 'N', 'D', ' ')

#define SECTION_HEADER 16u
#define EFB_PLANE_BYTES ((size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT * sizeof(uint32_t))

static size_t section_begin(GcStateBuffer *b, uint32_t tag, uint32_t version) {
  const size_t at = b->size;
  gc_state_put_u32(b, tag);
  gc_state_put_u32(b, version);
  gc_state_put_u64(b, 0);
  return at;
}

static void section_end(GcStateBuffer *b, size_t at) {
  if (!b->failed) {
    const uint64_t length = (uint64_t)(b->size - at - SECTION_HEADER);
    memcpy(b->data + at + 8u, &length, sizeof(length));
  }
}

static int build_payload(GcMachine *m, GcStateBuffer *b, int for_hash) {
  size_t at;
  GcMachine *copy;
  /* What the graphics thread computed, back in the machine; then the OpenGL
   * path's framebuffer and the copies it still owes main memory, where a
   * state reads them. */
  gc_gx_thread_sync(m);
  gc_gl_settle(m);
  gc_gl_sync_to_memory(m);
  copy = (GcMachine *)malloc(sizeof(GcMachine));
  if (!copy) return -1;
  memcpy(copy, m, sizeof(GcMachine));
  scrub(copy, for_hash);
  reserve(b, sizeof(GcMachine) + gc_ram_size + GC_ARAM_SIZE + 2u * EFB_PLANE_BYTES + (1u << 20));

  at = section_begin(b, TAG_MACH, 1u);
  gc_state_put(b, copy, sizeof(GcMachine));
  section_end(b, at);
  free(copy);

  at = section_begin(b, TAG_MEM1, 1u);
  gc_state_put(b, m->mem1, gc_ram_size);
  section_end(b, at);

  at = section_begin(b, TAG_ARAM, 1u);
  gc_state_put(b, m->aram, GC_ARAM_SIZE);
  section_end(b, at);

  at = section_begin(b, TAG_EFBC, 1u);
  gc_state_put(b, m->efb.color, EFB_PLANE_BYTES);
  section_end(b, at);

  at = section_begin(b, TAG_EFBZ, 1u);
  gc_state_put(b, m->efb.depth, EFB_PLANE_BYTES);
  section_end(b, at);

  at = section_begin(b, TAG_PADS, 1u);
  gc_pad_state_save(m, b);
  section_end(b, at);

  at = section_begin(b, TAG_CARD, 1u);
  gc_card_state_save(m, b, for_hash);
  section_end(b, at);

  at = section_begin(b, TAG_AX, 1u);
  gc_ax_state_save(m, b);
  section_end(b, at);

  at = section_begin(b, TAG_AUDI, 1u);
  gc_audio_state_save(m, b);
  section_end(b, at);

  /* A Wii's own state (wii.h): everything but what is the process's, and
   * its second memory is the tail of MEM1's section (gc_ram_size). Then the
   * flash filesystem, as the memory card is carried for a GameCube - in a
   * state's file only: a hash is of the machine, and has never counted what
   * is in its flash. */
  if (m->cpu.machine) {
    WiiState *wii = (WiiState *)malloc(sizeof(WiiState));
    if (!wii) return -1;
    memcpy(wii, m->cpu.machine, sizeof(WiiState));
    memset(&wii->host, 0, sizeof(wii->host));
    at = section_begin(b, TAG_WII, 1u);
    gc_state_put(b, wii, sizeof(WiiState));
    section_end(b, at);
    free(wii);
    if (!for_hash) {
      at = section_begin(b, TAG_NAND, 1u);
      wii_fs_state_save(m, b);
      section_end(b, at);
    }
  }

  at = section_begin(b, TAG_END, 1u);
  section_end(b, at);
  return b->failed ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* The header                                                          */

static const char k_magic[8] = {'N', 'C', 'G', 'C', 'S', 'T', 'A', 'T'};

/* Format 1's header was 176 bytes of which 168 were used. Format 2 adds the
 * name and the time of saving and rounds up to 256, so there is room for the
 * next thing without moving anything. A reader takes the size from the
 * header itself and reads only what is there. */
#define HEADER_SIZE 256u
#define HEADER_SIZE_V1 176u
#define BUILD_CHARS 64u
#define HEADER_USED_V1 (8u + 4u + 4u + 8u * 3u + 16u + BUILD_CHARS + 8u * 2u + 4u * 2u + 8u * 3u)
#define HEADER_USED_V2 (HEADER_USED_V1 + GC_STATE_NAME_CHARS + 8u)

typedef struct GcStateHeader {
  uint32_t version;
  uint32_t header_size;
  uint64_t layout;
  uint64_t program_hash;
  uint64_t disc_hash;
  char game_id[16];
  char build[BUILD_CHARS];
  uint64_t field;
  uint64_t cycles;
  uint32_t compression;
  uint32_t reserved;
  uint64_t payload_size;
  uint64_t stored_size;
  uint64_t payload_hash;
  /* Format 2. */
  char name[GC_STATE_NAME_CHARS];
  uint64_t saved_at;
} GcStateHeader;

static void put_header(GcStateBuffer *out, const GcStateHeader *h) {
  gc_state_put(out, k_magic, sizeof(k_magic));
  gc_state_put_u32(out, h->version);
  gc_state_put_u32(out, h->header_size);
  gc_state_put_u64(out, h->layout);
  gc_state_put_u64(out, h->program_hash);
  gc_state_put_u64(out, h->disc_hash);
  gc_state_put(out, h->game_id, sizeof(h->game_id));
  gc_state_put(out, h->build, sizeof(h->build));
  gc_state_put_u64(out, h->field);
  gc_state_put_u64(out, h->cycles);
  gc_state_put_u32(out, h->compression);
  gc_state_put_u32(out, h->reserved);
  gc_state_put_u64(out, h->payload_size);
  gc_state_put_u64(out, h->stored_size);
  gc_state_put_u64(out, h->payload_hash);
  gc_state_put(out, h->name, sizeof(h->name));
  gc_state_put_u64(out, h->saved_at);
  gc_state_put(out, NULL, HEADER_SIZE - HEADER_USED_V2);
}

static uint32_t get_u32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4u);
  return v;
}

static uint64_t get_u64(const uint8_t *p) {
  uint64_t v;
  memcpy(&v, p, 8u);
  return v;
}

/* `available` is how many bytes of the file there are. The caller has
 * checked there are at least HEADER_SIZE_V1 of them; anything past that is
 * read only when the header says it is there. */
static void get_header(const uint8_t *p, size_t available, GcStateHeader *h) {
  size_t at = 8u;
  memset(h, 0, sizeof(*h));
  h->version = get_u32(p + at), at += 4u;
  h->header_size = get_u32(p + at), at += 4u;
  h->layout = get_u64(p + at), at += 8u;
  h->program_hash = get_u64(p + at), at += 8u;
  h->disc_hash = get_u64(p + at), at += 8u;
  memcpy(h->game_id, p + at, sizeof(h->game_id)), at += sizeof(h->game_id);
  memcpy(h->build, p + at, sizeof(h->build)), at += sizeof(h->build);
  h->game_id[sizeof(h->game_id) - 1u] = 0;
  h->build[sizeof(h->build) - 1u] = 0;
  h->field = get_u64(p + at), at += 8u;
  h->cycles = get_u64(p + at), at += 8u;
  h->compression = get_u32(p + at), at += 4u;
  h->reserved = get_u32(p + at), at += 4u;
  h->payload_size = get_u64(p + at), at += 8u;
  h->stored_size = get_u64(p + at), at += 8u;
  h->payload_hash = get_u64(p + at), at += 8u;
  if (h->header_size >= HEADER_USED_V2 && available >= HEADER_USED_V2) {
    memcpy(h->name, p + at, sizeof(h->name)), at += sizeof(h->name);
    h->name[sizeof(h->name) - 1u] = 0;
    h->saved_at = get_u64(p + at);
  }
}

/* ------------------------------------------------------------------ */
/* Saving                                                              */

/* A capture is the payload and the header that goes with it, taken on the
 * machine's thread; turning it into a file touches nothing of the machine's,
 * so a frontend can do that part on another thread and not stall a field. */
struct GcStateCapture {
  GcStateHeader header;
  GcStateBuffer payload;
};

GcStateCapture *gc_state_capture(GcMachine *m, const GcStateIdentity *id) {
  GcStateCapture *c = (GcStateCapture *)calloc(1, sizeof(GcStateCapture));
  GcStateHeader *h;
  if (!c) return NULL;
  if (build_payload(m, &c->payload, 0) != 0) {
    gc_state_capture_free(c);
    return NULL;
  }
  h = &c->header;
  h->version = GC_STATE_VERSION;
  h->header_size = HEADER_SIZE;
  h->layout = layout_fingerprint(GC_STATE_VERSION);
  h->program_hash = id->program_hash;
  h->disc_hash = id->disc_hash;
  memcpy(h->game_id, id->game_id, sizeof(h->game_id));
  build_string(h->build, sizeof(h->build));
  h->field = m->field;
  h->cycles = m->cpu.cycles;
  /* When, by the host's clock. A state that says so can be listed without
   * trusting a file's modification time, which copying does not carry. */
  h->saved_at = (uint64_t)time(NULL);
  return c;
}

/* The header alone, off the front of the file. Nothing is decompressed and
 * no machine is involved, so a menu can ask about every slot at once. */
int gc_state_peek_file(const char *path, GcStateInfo *out, char *error, size_t error_size) {
  uint8_t bytes[HEADER_SIZE];
  GcStateHeader h;
  FILE *file;
  size_t read;

  if (error && error_size) error[0] = 0;
  if (out) memset(out, 0, sizeof(*out));
  if (!path || !out) {
    if (error) snprintf(error, error_size, "no state to look at");
    return -1;
  }
  file = fopen(path, "rb");
  if (!file) {
    if (error) snprintf(error, error_size, "there is no state there");
    return -1;
  }
  read = fread(bytes, 1u, sizeof(bytes), file);
  fclose(file);
  if (read < HEADER_SIZE_V1 || memcmp(bytes, k_magic, sizeof(k_magic)) != 0) {
    if (error) snprintf(error, error_size, "not a GameCube save state");
    return -1;
  }
  get_header(bytes, read, &h);
  out->version = h.version;
  memcpy(out->name, h.name, sizeof(out->name));
  memcpy(out->game_id, h.game_id, sizeof(out->game_id));
  out->name[sizeof(out->name) - 1u] = 0;
  out->game_id[sizeof(out->game_id) - 1u] = 0;
  out->field = h.field;
  out->cycles = h.cycles;
  out->saved_at = h.saved_at;
  out->stored_size = h.stored_size;
  return 0;
}

void gc_state_capture_set_name(GcStateCapture *c, const char *name) {
  if (!c) return;
  memset(c->header.name, 0, sizeof(c->header.name));
  if (name) snprintf(c->header.name, sizeof(c->header.name), "%s", name);
}

void gc_state_capture_free(GcStateCapture *c) {
  if (!c) return;
  gc_state_buffer_free(&c->payload);
  free(c);
}

static int capture_bytes(GcStateCapture *c, int compress, GcStateBuffer *out) {
  GcStateHeader h = c->header;
  size_t header_at, stored_at;
  memset(out, 0, sizeof(*out));
  h.compression = compress ? 1u : 0u;
  h.payload_size = c->payload.size;
  h.payload_hash = hash_bytes(HASH_SEED, c->payload.data, c->payload.size);
  header_at = out->size;
  put_header(out, &h);
  stored_at = out->size;
  if (compress) {
    if (lz_compress(c->payload.data, c->payload.size, out) != 0) {
      gc_state_buffer_free(out);
      return -1;
    }
  } else {
    gc_state_put(out, c->payload.data, c->payload.size);
  }
  if (out->failed) {
    gc_state_buffer_free(out);
    return -1;
  }
  h.stored_size = out->size - stored_at;
  {
    /* The stored size is known only now; rewrite the header in place. */
    const size_t end = out->size;
    out->size = header_at;
    put_header(out, &h);
    out->size = end;
  }
  return 0;
}

int gc_state_save_memory(GcMachine *m, const GcStateIdentity *id, int compress, GcStateBuffer *out) {
  GcStateCapture *c = gc_state_capture(m, id);
  int result;
  memset(out, 0, sizeof(*out));
  if (!c) return -1;
  result = capture_bytes(c, compress, out);
  gc_state_capture_free(c);
  return result;
}

int gc_state_write_capture(GcStateCapture *c, const char *path, int compress, char *error, size_t error_size) {
  GcStateBuffer bytes;
  char temporary[1100];
  FILE *file;
  size_t written;
  if (error && error_size) error[0] = 0;
  if (capture_bytes(c, compress, &bytes) != 0) {
    if (error) snprintf(error, error_size, "out of memory building the state");
    return -1;
  }
  snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  file = fopen(temporary, "wb");
  if (!file) {
    if (error) snprintf(error, error_size, "cannot write %s", temporary);
    gc_state_buffer_free(&bytes);
    return -1;
  }
  written = fwrite(bytes.data, 1, bytes.size, file);
  if (fclose(file) != 0 || written != bytes.size) {
    remove(temporary);
    if (error) snprintf(error, error_size, "cannot write %s (disk full?)", temporary);
    gc_state_buffer_free(&bytes);
    return -1;
  }
  gc_state_buffer_free(&bytes);
#if defined(_WIN32)
  if (!MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
  if (rename(temporary, path) != 0) {
#endif
    remove(temporary);
    if (error) snprintf(error, error_size, "cannot replace %s", path);
    return -1;
  }
  return 0;
}

int gc_state_save_file(GcMachine *m, const GcStateIdentity *id, const char *path, int compress, char *error,
                       size_t error_size) {
  GcStateCapture *c = gc_state_capture(m, id);
  int result;
  if (!c) {
    if (error) snprintf(error, error_size, "out of memory building the state");
    return -1;
  }
  result = gc_state_write_capture(c, path, compress, error, error_size);
  gc_state_capture_free(c);
  return result;
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */

typedef struct GcSection {
  uint32_t tag;
  uint32_t version;
  const uint8_t *data;
  size_t size;
} GcSection;

/* The last two are a Wii's, and only a Wii's state has them. */
enum { SECTION_MACH, SECTION_MEM1, SECTION_ARAM, SECTION_EFBC, SECTION_EFBZ, SECTION_PADS, SECTION_CARD,
       SECTION_AX, SECTION_AUDI, SECTION_WII, SECTION_NAND, SECTION_COUNT };

static const struct {
  uint32_t tag;
  const char *name;
} k_sections[SECTION_COUNT] = {
    {TAG_MACH, "MACH"}, {TAG_MEM1, "MEM1"}, {TAG_ARAM, "ARAM"}, {TAG_EFBC, "EFBC"}, {TAG_EFBZ, "EFBZ"},
    {TAG_PADS, "PADS"}, {TAG_CARD, "CARD"}, {TAG_AX, "AX"},     {TAG_AUDI, "AUDI"}, {TAG_WII, "WII"},
    {TAG_NAND, "NAND"},
};

#define FAIL(...)                                          \
  do {                                                     \
    if (error) snprintf(error, error_size, __VA_ARGS__);   \
    goto done;                                             \
  } while (0)

static const char *nonempty(const char *text) { return text[0] ? text : "(none)"; }

int gc_state_load_memory(GcMachine *m, const GcStateIdentity *id, const uint8_t *data, size_t size, char *error,
                         size_t error_size) {
  GcStateHeader h;
  GcSection sections[SECTION_COUNT];
  uint8_t *payload = NULL;
  GcMachine *keep = NULL;
  const uint8_t *p;
  size_t at, k;
  int result = -1, ended = 0;
  char build[BUILD_CHARS];

  if (error && error_size) error[0] = 0;
  memset(sections, 0, sizeof(sections));
  if (size < HEADER_SIZE_V1 || memcmp(data, k_magic, sizeof(k_magic)) != 0) FAIL("not a GameCube save state");
  get_header(data, size, &h);
  if (h.version != GC_STATE_VERSION && h.version != 1u) {
    FAIL("saved in state format %u; this runtime reads formats 1 and %u", h.version, GC_STATE_VERSION);
  }
  if (h.header_size != HEADER_SIZE && h.header_size != HEADER_SIZE_V1) {
    FAIL("the header is %u bytes, expected %u or %u", h.header_size, HEADER_SIZE_V1, HEADER_SIZE);
  }
  if (size < h.header_size) FAIL("the header is cut off");
  if (strncmp(h.game_id, id->game_id, sizeof(h.game_id)) != 0) {
    FAIL("saved by %s, not %s", nonempty(h.game_id), nonempty(id->game_id));
  }
  if (h.program_hash != id->program_hash) {
    FAIL("saved by a different program image of %s (%016llx, this is %016llx)", nonempty(h.game_id),
         (unsigned long long)h.program_hash, (unsigned long long)id->program_hash);
  }
  if (h.disc_hash != id->disc_hash) {
    FAIL("saved with %s disc (%016llx, this machine has %016llx)", h.disc_hash ? "a different" : "no",
         (unsigned long long)h.disc_hash, (unsigned long long)id->disc_hash);
  }
  build_string(build, sizeof(build));
  if (h.layout != layout_fingerprint(h.version)) {
    FAIL("saved by a runtime build whose machine layout differs (%s; this is %s)", h.build, build);
  }
  if (h.stored_size != size - h.header_size) {
    FAIL("the file is truncated or has trailing bytes (%llu stored, %llu present)",
         (unsigned long long)h.stored_size, (unsigned long long)(size - h.header_size));
  }
  if (h.payload_size > ((uint64_t)1u << 32)) FAIL("the payload size is implausible");
  if (h.compression > 1u) FAIL("unknown compression %u", h.compression);

  if (h.compression) {
    payload = (uint8_t *)malloc((size_t)h.payload_size);
    if (!payload) FAIL("out of memory");
    if (lz_decompress(data + h.header_size, (size_t)h.stored_size, payload, (size_t)h.payload_size) != 0) {
      FAIL("the compressed payload is damaged");
    }
    p = payload;
  } else {
    if (h.stored_size != h.payload_size) FAIL("the payload size does not match");
    p = data + h.header_size;
  }
  if (hash_bytes(HASH_SEED, p, (size_t)h.payload_size) != h.payload_hash) FAIL("the checksum does not match");

  /* Every section, checked before anything is applied. */
  at = 0;
  while (at < h.payload_size) {
    uint32_t tag, version;
    uint64_t length;
    if (h.payload_size - at < SECTION_HEADER) FAIL("a section header is cut off");
    tag = get_u32(p + at);
    version = get_u32(p + at + 4u);
    length = get_u64(p + at + 8u);
    at += SECTION_HEADER;
    if (length > h.payload_size - at) FAIL("a section runs past the end");
    if (tag == TAG_END) {
      ended = 1;
      at += (size_t)length;
      break;
    }
    for (k = 0; k < SECTION_COUNT; k++) {
      if (k_sections[k].tag == tag) break;
    }
    if (k == SECTION_COUNT) {
      char name[5];
      memcpy(name, &tag, 4u);
      name[4] = 0;
      FAIL("unknown section %.4s", name);
    }
    if (version != 1u) FAIL("section %s is version %u; this runtime reads version 1", k_sections[k].name, version);
    if (sections[k].data) FAIL("section %s appears twice", k_sections[k].name);
    sections[k].tag = tag;
    sections[k].version = version;
    sections[k].data = p + at;
    sections[k].size = (size_t)length;
    at += (size_t)length;
  }
  if (!ended || at != h.payload_size) FAIL("the section list does not end properly");
  for (k = 0; k < SECTION_COUNT; k++) {
    const int wii_only = k == SECTION_WII || k == SECTION_NAND;
    if (wii_only && !m->cpu.machine) {
      if (sections[k].data) FAIL("section %s is a Wii's, and this machine is a GameCube", k_sections[k].name);
      continue;
    }
    if (!sections[k].data) FAIL("section %s is missing", k_sections[k].name);
  }
  if (sections[SECTION_MACH].size != sizeof(GcMachine)) FAIL("section MACH has the wrong size");
  if (sections[SECTION_MEM1].size != gc_ram_size) FAIL("section MEM1 has the wrong size");
  if (sections[SECTION_ARAM].size != GC_ARAM_SIZE) FAIL("section ARAM has the wrong size");
  if (sections[SECTION_EFBC].size != EFB_PLANE_BYTES) FAIL("section EFBC has the wrong size");
  if (sections[SECTION_EFBZ].size != EFB_PLANE_BYTES) FAIL("section EFBZ has the wrong size");
  if (gc_pad_state_load(m, sections[SECTION_PADS].data, sections[SECTION_PADS].size, 0) != 0) {
    FAIL("section PADS is damaged");
  }
  if (gc_card_state_load(m, sections[SECTION_CARD].data, sections[SECTION_CARD].size, 0) != 0) {
    FAIL("section CARD is damaged");
  }
  if (gc_ax_state_load(m, sections[SECTION_AX].data, sections[SECTION_AX].size, 0) != 0) {
    FAIL("section AX is damaged");
  }
  if (gc_audio_state_load(m, sections[SECTION_AUDI].data, sections[SECTION_AUDI].size, 0) != 0) {
    FAIL("section AUDI is damaged");
  }
  if (m->cpu.machine) {
    if (sections[SECTION_WII].size != sizeof(WiiState)) FAIL("section WII has the wrong size");
    if (wii_fs_state_load(m, sections[SECTION_NAND].data, sections[SECTION_NAND].size, 0) != 0) {
      FAIL("section NAND is damaged");
    }
  }
  keep = (GcMachine *)malloc(sizeof(GcMachine));
  if (!keep) FAIL("out of memory");

  /* Everything checks; from here on the machine changes. What the OpenGL
   * path holds is finished first and then forgotten: the framebuffer loaded
   * below is the one it draws on next. */
  gc_gx_thread_quiesce(m);
  gc_gl_invalidate(m);
  gc_gl_forget_pictures(m);
  memcpy(keep, m, sizeof(GcMachine));
  memcpy(m, sections[SECTION_MACH].data, sizeof(GcMachine));
  keep_host(m, keep);
  memcpy(m->mem1, sections[SECTION_MEM1].data, gc_ram_size);
  memcpy(m->aram, sections[SECTION_ARAM].data, GC_ARAM_SIZE);
  memcpy(m->efb.color, sections[SECTION_EFBC].data, EFB_PLANE_BYTES);
  memcpy(m->efb.depth, sections[SECTION_EFBZ].data, EFB_PLANE_BYTES);
  if (gc_pad_state_load(m, sections[SECTION_PADS].data, sections[SECTION_PADS].size, 1) != 0 ||
      gc_card_state_load(m, sections[SECTION_CARD].data, sections[SECTION_CARD].size, 1) != 0 ||
      gc_ax_state_load(m, sections[SECTION_AX].data, sections[SECTION_AX].size, 1) != 0 ||
      gc_audio_state_load(m, sections[SECTION_AUDI].data, sections[SECTION_AUDI].size, 1) != 0) {
    /* Only an allocation can fail here, and the machine is half loaded. */
    m->finished = 1;
    FAIL("out of memory while loading; the machine has been stopped");
  }
  if (m->cpu.machine) {
    WiiState *const wii = (WiiState *)m->cpu.machine;
    const WiiHost host = wii->host;
    memcpy(wii, sections[SECTION_WII].data, sizeof(WiiState));
    wii->host = host;
    if (wii_fs_state_load(m, sections[SECTION_NAND].data, sections[SECTION_NAND].size, 1) != 0) {
      m->finished = 1;
      FAIL("out of memory while loading; the machine has been stopped");
    }
  }
  gc_gx_thread_reload(m);
  result = 0;

done:
  free(keep);
  free(payload);
  return result;
}

int gc_state_load_file(GcMachine *m, const GcStateIdentity *id, const char *path, char *error, size_t error_size) {
  FILE *file = fopen(path, "rb");
  uint8_t *data;
  long length;
  int result;
  if (error && error_size) error[0] = 0;
  if (!file) {
    if (error) snprintf(error, error_size, "cannot read %s", path);
    return -1;
  }
  fseek(file, 0, SEEK_END);
  length = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (length <= 0) {
    fclose(file);
    if (error) snprintf(error, error_size, "%s is empty", path);
    return -1;
  }
  data = (uint8_t *)malloc((size_t)length);
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
    fclose(file);
    free(data);
    if (error) snprintf(error, error_size, "cannot read %s", path);
    return -1;
  }
  fclose(file);
  result = gc_state_load_memory(m, id, data, (size_t)length, error, error_size);
  free(data);
  return result;
}

/* ------------------------------------------------------------------ */
/* Hashes                                                              */

uint64_t gc_state_hash(GcMachine *m) {
  GcStateBuffer payload = {0};
  uint64_t h = 0;
  if (build_payload(m, &payload, 1) == 0) h = hash_bytes(HASH_SEED, payload.data, payload.size);
  gc_state_buffer_free(&payload);
  return h;
}

uint64_t gc_state_hash_memory(const GcMachine *m) {
  uint64_t h = 1469598103934665603ull;
  size_t i;
  const uint8_t *p = m->mem1;
  for (i = 0; i < gc_ram_size; i++) h = (h ^ p[i]) * 1099511628211ull;
  p = (const uint8_t *)m->cpu.gpr;
  for (i = 0; i < sizeof(m->cpu.gpr); i++) h = (h ^ p[i]) * 1099511628211ull;
  p = (const uint8_t *)m->cpu.fpr;
  for (i = 0; i < sizeof(m->cpu.fpr); i++) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}
