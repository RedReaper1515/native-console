/*
 * The OpenGL path, the machine's side. See gc_gl.h for what this is and what
 * it is not, gc_gl_shader.c for why the card does not rasterise triangles
 * itself, and gc_gl_render.c for the thread that makes every OpenGL call.
 *
 * This side runs on the machine's thread and reads the machine: it sets
 * triangles up as the reference does, keeps batches of them per register
 * state, packs the pipeline's uniform block, decodes textures into slots,
 * works out the combiner registers a triangle leaves (carry), holds the pages
 * an EFB copy will write, and hands all of it to the render thread in order.
 */
#include "gc_gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "gc.h"
#include "gc_efb.h"
#include "gc_gx.h"
#include "gc_gx_blend.h"
#include "gc_gx_draw.h"
#include "gc_gx_tev.h"
#include "gc_gx_texture.h"
#include "gc_perf.h"

#ifndef GC_HAVE_SDL2

int gc_gl_start(GcMachine *m) {
  if (m && m->log) {
    fprintf(m->log, "gl: this build has no SDL2, so there is no context to draw into\n");
  }
  return -1;
}
int gc_gl_start_with(GcMachine *m, const GcGlOptions *options) {
  (void)options;
  return gc_gl_start(m);
}
void gc_gl_stop(GcMachine *m) { (void)m; }
int gc_gl_active(const GcMachine *m) { (void)m; return 0; }
void gc_gl_triangle(GcMachine *m, const GcGxPoint *points) { (void)m; (void)points; }
void gc_gl_pieces(GcMachine *m, const struct GcGxRasterPiece *const *pieces, unsigned count) {
  (void)m;
  (void)pieces;
  (void)count;
}
void gc_gl_sync_to_memory(GcMachine *m) { (void)m; }
void gc_gl_fetch_pixel(GcMachine *m, uint32_t x, uint32_t y, int depth) {
  (void)m;
  (void)x;
  (void)y;
  (void)depth;
}
int gc_gl_peek_late(GcMachine *m, uint32_t x, uint32_t y, int depth) {
  (void)m;
  (void)x;
  (void)y;
  (void)depth;
  return 0;
}
void gc_gl_invalidate(GcMachine *m) { (void)m; }
int gc_gl_display_picture(GcMachine *m, uint32_t *out, unsigned pitch, unsigned rows, unsigned *width,
                          unsigned *height, uint64_t *serial) {
  (void)m;
  (void)out;
  (void)pitch;
  (void)rows;
  (void)width;
  (void)height;
  (void)serial;
  return -1;
}
int gc_gl_picture_at(GcMachine *m, uint32_t first, uint32_t display_width, uint32_t display_height, uint32_t *out,
                     unsigned pitch, unsigned rows, unsigned *width, unsigned *height, uint64_t *serial) {
  (void)m;
  (void)first;
  (void)display_width;
  (void)display_height;
  (void)out;
  (void)pitch;
  (void)rows;
  (void)width;
  (void)height;
  (void)serial;
  return -1;
}
void gc_gl_forget_pictures(GcMachine *m) { (void)m; }
int gc_gl_write_picture(GcMachine *m, const char *path) {
  (void)m;
  (void)path;
  return -1;
}
int gc_gl_copy(GcMachine *m, uint32_t trigger) {
  (void)m;
  (void)trigger;
  return 0;
}
int gc_gl_copy_prepared(GcMachine *m, const struct GcEfbCopyJob *job, uint64_t tag, uint32_t first, uint32_t end) {
  (void)m;
  (void)job;
  (void)tag;
  (void)first;
  (void)end;
  return 0;
}
void gc_gl_settle(GcMachine *m) { (void)m; }

#else

#include <SDL.h>

#include "gc_gl_cover.h"
#include "gc_gl_internal.h"
#include "gc_gx_tev_plan.h"
#include "gc_gx_thread.h"
#include "gc_gx_workers.h"
#include "gc_memory.h"

/* ------------------------------------------------------------------ */
/* The state this side keeps                                           */

/* The texture cache: at most GC_GL_TEXTURES slots or this many bytes. */
#define GC_GL_TEXTURE_BUDGET ((size_t)1 << 30)

typedef struct GcGlTexture {
  uint64_t key;
  uint64_t last_used;
  size_t bytes;
  int used;
} GcGlTexture;

typedef struct GcGlState {
  GcGlRender *render;
  /* Main memory, as the machine that started the path has it: copies write
   * it (through its second view when there is one) and hold its pages. A
   * graphics thread's shadow reads a copy of memory instead
   * (gc_gx_thread.h), and draws with this same state. */
  uint8_t *memory;
  uint8_t *memory_for_copies;
  SDL_Window *window, *compile_window, *readback_window;
  SDL_GLContext context, compile_context, readback_context;
  int no_early_z; /* GC_GL_NO_EARLY_Z: no conservative depth test before the program */

  /* The batch: one register state, triangles set up and packed as the
   * shader reads them, handed over when the state changes or it is full. */
  uint32_t *records;
  GcGlTri *tris;
  unsigned count, capacity;
  int open;       /* the render thread has this register state */
  int cpu;        /* ... which the reference rasteriser draws instead */
  int needs_dst;  /* ... which reads the framebuffer */
  uint64_t state;
  uint32_t shape[GC_GL_SHAPE_WORDS];
  GcGlDraw draw;
  GcGxTevPlan *plan;
  unsigned reads, writes; /* combiner register groups: read before written, written */
  uint64_t bound_keys[8]; /* texture keys of the maps the batch bound */
  uint32_t bound_slots[8];
  unsigned bound_maps;
  uint64_t cycles_at_bind;

  /* Which register groups hold a value the last pixel left that could not
   * be computed exactly (see carry()). */
  unsigned suspect;

  GcGlTexture textures[GC_GL_TEXTURES];
  size_t texture_bytes;
  uint64_t clock;

  /* Which side of the framebuffer is out of date. */
  int memory_is_stale;
  int card_is_stale;

  /* What it did, and what it cost. */
  uint64_t triangles, batches, draws_sent, cpu_triangles, uploads, downloads;
  uint64_t carries, carry_unchecked, suspect_reads, texture_hits, texture_misses, revalidations;
  double texture_hash_ms, texture_decode_ms, total_ms;
  double flush_ms, begin_ms, carry_ms, sync_ms, key_ms;
  uint64_t syncs, copies_async, copies_sync, copy_textures;
  /* Peeks answered a frame late (gc_gl_peek_late). The counts are the
   * machine's thread's own; nothing else writes them. */
  int late_peeks;
  uint64_t late_peeks_answered, late_peeks_refused;
  uint64_t peeks, pixel_peeks, peek_field;
  unsigned copy_lag; /* GC_GL_COPY_LAG: display copies on their way before a graphics thread waits */
  unsigned peeks_this_field;
  uint64_t dst_blend, dst_alpha, box_pixels, piece_runs;
  double copy_ms;
  uint64_t field;
  double field_ms, worst_field_ms;
  uint64_t worst_field;
  /* Whole fields by the wall clock, from this file's first call in one to
   * its first call in the next: the hitch someone would see. The first
   * fields of a run, which include starting, are not counted. */
  double field_seen_ms, worst_wall_ms;
  uint64_t worst_wall_field, fields_seen, fields_over_34ms, fields_over_50ms;
} GcGlState;

static GcGlState *state_of(const GcMachine *m) { return (GcGlState *)m->gl; }

int gc_gl_active(const GcMachine *m) { return m && m->gl != NULL; }

/* Time spent in this file, charged to the field it was spent in. */
static void account(GcGlState *gl, const GcMachine *m, double started) {
  const double ms = gc_gl_now_ms() - started;
  if (m->field != gl->field) {
    const double now = gc_gl_now_ms();
    if (m->field > gl->field && ++gl->fields_seen > 3u) {
      /* A game drawing every other field calls here every other field. */
      const double wall = (now - gl->field_seen_ms) / (double)(m->field - gl->field);
      if (wall > gl->worst_wall_ms) {
        gl->worst_wall_ms = wall;
        gl->worst_wall_field = gl->field;
      }
      if (wall > 1001.0 / 30.0) gl->fields_over_34ms++;
      if (wall > 50.0) gl->fields_over_50ms++;
    }
    gl->field_seen_ms = now;
    gl->field = m->field;
    gl->field_ms = 0.0;
  }
  gl->field_ms += ms;
  gl->total_ms += ms;
  gc_perf.gl_ms += ms;
  if (gl->field_ms > gl->worst_field_ms) {
    gl->worst_field_ms = gl->field_ms;
    gl->worst_field = m->field;
  }
}

/* ------------------------------------------------------------------ */
/* The framebuffer between main memory and the card                     */

static void flush(GcMachine *m);

static void upload(GcMachine *m) {
  GcGlState *gl = state_of(m);
  const size_t plane = sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT;
  uint32_t *const planes = (uint32_t *)gc_gl_render_command(gl->render, GC_GL_CMD_UPLOAD, 2u * plane);
  memcpy(planes, m->efb.color, plane);
  memcpy(planes + (size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT, m->efb.depth, plane);
  gc_gl_render_send(gl->render);
  gl->card_is_stale = 0;
  gl->uploads++;
}

/* Everything handed over so far is drawn, and the framebuffer comes back
 * into the machine's planes; this thread waits for it. */
static void download(GcMachine *m) {
  GcGlState *gl = state_of(m);
  GcGlDownloadCommand *const c =
      (GcGlDownloadCommand *)gc_gl_render_command(gl->render, GC_GL_CMD_DOWNLOAD, sizeof(GcGlDownloadCommand));
  c->color = m->efb.color;
  c->depth = m->efb.depth;
  c->pixel = 0;
  gc_gl_render_send(gl->render);
  gc_gl_render_wait(gl->render);
  gl->memory_is_stale = 0;
  gl->downloads++;
  gc_perf.readbacks++;
}

/* A peek reads one pixel. While the framebuffer in memory is out of date,
 * only that pixel comes back from the card, into its place in the plane;
 * a field that peeks more than this many brings the whole framebuffer back
 * instead, once (the hardware tests peek every pixel of a rectangle). */
#define GC_GL_PIXEL_PEEKS 64u

int gc_gl_peek_late(GcMachine *m, uint32_t x, uint32_t y, int depth) {
  GcGlState *const gl = m ? state_of(m) : NULL;
  GcGxThread *t;
  uint64_t tag;
  uint32_t value = 0;
  if (!gl || !gl->late_peeks || x >= GC_EFB_WIDTH || y >= GC_EFB_HEIGHT) return 0;
  t = gc_gx_thread_of(m);
  if (!t) return 0;
  tag = gc_gx_thread_peek_from(t, depth ? 1 : 0);
  if (!tag || !gc_gl_render_snapshot_pixel(gl->render, tag, depth ? 1 : 0, x, y, &value)) {
    gl->late_peeks_refused++;
    return 0;
  }
  (depth ? m->efb.depth : m->efb.color)[(size_t)y * GC_EFB_WIDTH + x] = value;
  gl->late_peeks_answered++;
  return 1;
}

void gc_gl_fetch_pixel(GcMachine *m, uint32_t x, uint32_t y, int depth) {
  GcGlState *gl;
  double started;
  m = gc_gx_thread_target(m);
  gl = m ? state_of(m) : NULL;
  if (!gl) return;
  started = gc_gl_now_ms();
  gl->peeks++;
  if (m->field != gl->peek_field) {
    gl->peek_field = m->field;
    gl->peeks_this_field = 0;
  }
  flush(m);
  if (gl->memory_is_stale && x < GC_EFB_WIDTH && y < GC_EFB_HEIGHT) {
    if (++gl->peeks_this_field > GC_GL_PIXEL_PEEKS) {
      download(m);
    } else {
      GcGlDownloadCommand *const c =
          (GcGlDownloadCommand *)gc_gl_render_command(gl->render, GC_GL_CMD_DOWNLOAD, sizeof(GcGlDownloadCommand));
      c->color = m->efb.color;
      c->depth = m->efb.depth;
      c->pixel = 1;
      c->x = x;
      c->y = y;
      c->depth_plane = (uint32_t)(depth != 0);
      gc_gl_render_send(gl->render);
      gc_gl_render_wait(gl->render);
      gl->pixel_peeks++;
    }
  }
  gl->sync_ms += gc_gl_now_ms() - started;
  account(gl, m, started);
}

/* ------------------------------------------------------------------ */
/* Textures                                                            */

/* gc_gx_texture.c's tile geometry, for how many bytes a level occupies:
 * the cache hashes exactly the bytes a texture is made of. */
static uint32_t level_bytes(unsigned format, unsigned width, unsigned height) {
  unsigned tile_width = 4, tile_height = 4, bytes = 32;
  switch (format) {
    case 0x0: case 0x8: case 0xe: tile_width = 8; tile_height = 8; break;
    case 0x1: case 0x2: case 0x9: tile_width = 8; tile_height = 4; break;
    case 0x6: bytes = 64; break;
    default: break;
  }
  return ((width + tile_width - 1u) / tile_width) * ((height + tile_height - 1u) / tile_height) * bytes;
}

/* The highest level a pixel can read: the level of detail is clamped to
 * the larger of the two range registers, and a blend between levels reads
 * one past it. Levels above this are never sampled, so they are neither
 * decoded nor hashed - a texture without mipmaps is followed in memory by
 * whatever the game put next, and hashing that would make every write to
 * the neighbouring buffer look like a change to this texture. */
static unsigned top_level(const GcGxTexture *texture) {
  /* How many different levels the reference's chain holds. Its level walk
   * (gc_gx_texture_bind) moves on one level before it notices a level is
   * one texel square, so a texture that *starts* one by one has a second,
   * distinct level after it in memory - which a pixel reads whenever the
   * level of detail is above zero. Every larger chain stops where it
   * reaches one by one. */
  const unsigned levels = gc_gx_texture_levels(texture) + ((texture->width == 1u && texture->height == 1u) ? 1u : 0u);
  const unsigned range = (texture->min_lod > texture->max_lod ? texture->min_lod : texture->max_lod) >> 4;
  const unsigned top = range + 1u;
  return top < levels ? top : levels - 1u;
}

static uint64_t texture_key(GcMachine *m, const GcGxTexture *texture, unsigned top) {
  GcGlState *gl = state_of(m);
  const double started = gc_gl_now_ms();
  uint32_t words[8];
  uint64_t key;
  GcGxTexture last;
  uint32_t span;

  words[0] = texture->address;
  words[1] = texture->width;
  words[2] = texture->height;
  words[3] = texture->format;
  words[4] = texture->palette_offset;
  words[5] = texture->palette_format;
  words[6] = top;
  words[7] = 0x74657874u;
  key = gc_gl_hash(words, sizeof(words), 0);

  gc_gx_texture_level(texture, top, &last);
  span = (last.address - texture->address) + level_bytes(texture->format, last.width, last.height);
  if (texture->address < gc_ram_size) {
    const uint32_t available = gc_ram_size - texture->address;
    key = gc_gl_hash(m->mem1 + texture->address, span < available ? span : available, key);
  }
  if (texture->format == 0x8 || texture->format == 0x9 || texture->format == 0xa) {
    /* A paletted texture is its indices and the table they index. */
    const size_t first = (size_t)texture->palette_offset * GC_GX_TLUT_UNIT_ENTRIES;
    size_t count = texture->format == 0x8 ? 16u : (texture->format == 0x9 ? 256u : 16384u);
    if (first < GC_GX_TLUT_ENTRIES) {
      if (first + count > GC_GX_TLUT_ENTRIES) count = GC_GX_TLUT_ENTRIES - first;
      key = gc_gl_hash(m->gx.tlut.entry + first, count * sizeof(uint16_t), key);
    }
  }
  gl->texture_hash_ms += gc_gl_now_ms() - started;
  gc_perf.texture_hash_ms += gc_gl_now_ms() - started;
  return key;
}

/* The map's decoded chain on the card, from the cache when its bytes have
 * not changed. Every level is decoded on the CPU through the reference's
 * own texel decoder and goes up as eight-bit unsigned integers, which the
 * shader reads with texelFetch and filters itself.
 *
 * Not BC1 for CMPR: the GameCube's compressed format interpolates its
 * endpoints with weights of 3/8 and 5/8, not thirds, and its three-colour
 * mode's fourth entry is the midpoint made transparent rather than black -
 * so no card's BC1 decoder produces the reference's texels. Decoding once
 * and caching is what keeps it exact. */
/* The cache's slot for `key`: the one that has it (*found), or one made
 * free for a texture of `bytes`, the least recently used going first. */
static unsigned texture_slot(GcGlState *gl, uint64_t key, size_t bytes, int *found) {
  unsigned i, slot = GC_GL_TEXTURES;
  *found = 0;
  for (i = 0; i < GC_GL_TEXTURES; i++) {
    if (gl->textures[i].used && gl->textures[i].key == key) {
      gl->textures[i].last_used = ++gl->clock;
      gl->texture_hits++;
      *found = 1;
      return i;
    }
    if (slot == GC_GL_TEXTURES && !gl->textures[i].used) slot = i;
  }
  gl->texture_misses++;
  while (slot == GC_GL_TEXTURES || gl->texture_bytes + bytes > GC_GL_TEXTURE_BUDGET) {
    /* The least recently used goes; its slot is filled again by the caller. */
    unsigned oldest = GC_GL_TEXTURES;
    for (i = 0; i < GC_GL_TEXTURES; i++) {
      if (gl->textures[i].used &&
          (oldest == GC_GL_TEXTURES || gl->textures[i].last_used < gl->textures[oldest].last_used)) {
        oldest = i;
      }
    }
    if (oldest == GC_GL_TEXTURES) break;
    gl->texture_bytes -= gl->textures[oldest].bytes;
    gl->textures[oldest].used = 0;
    slot = oldest;
  }
  return slot;
}

static void texture_slot_filled(GcGlState *gl, unsigned slot, uint64_t key, size_t bytes) {
  gl->textures[slot].key = key;
  gl->textures[slot].used = 1;
  gl->textures[slot].bytes = bytes;
  gl->textures[slot].last_used = ++gl->clock;
  gl->texture_bytes += bytes;
}

/* A map that is an EFB copy the card still holds as a texture (the fast
 * path; gc_gx_thread.h): the slot it is in, taking the card's texture into
 * one the first time. No byte of the copy is read. */
static uint32_t texture_for_copy(GcMachine *m, const GcGxTexture *texture, uint64_t key, uint64_t tag) {
  GcGlState *gl = state_of(m);
  const size_t bytes = (size_t)texture->width * texture->height * 4u;
  int found;
  const unsigned slot = texture_slot(gl, key, bytes, &found);
  GcGlCopyTextureCommand *c;
  if (found) return slot;
  c = (GcGlCopyTextureCommand *)gc_gl_render_command(gl->render, GC_GL_CMD_COPY_TEXTURE, sizeof(GcGlCopyTextureCommand));
  c->slot = slot;
  c->tag = tag;
  gc_gl_render_send(gl->render);
  texture_slot_filled(gl, slot, key, bytes);
  gl->copy_textures++;
  return slot;
}

static uint32_t texture_for(GcMachine *m, const GcGxTexture *texture, unsigned top, uint64_t key) {
  GcGlState *gl = state_of(m);
  unsigned slot, level;
  double started;
  size_t bytes = 0;
  int side_by_side, found;
  GcGlTextureCommand *c;
  uint8_t *texels;

  side_by_side = texture->width == 1u && texture->height == 1u && top >= 1u;
  if (side_by_side) {
    bytes = 8u;
  } else {
    for (level = 0; level <= top; level++) {
      GcGxTexture at;
      gc_gx_texture_level(texture, level, &at);
      bytes += (size_t)at.width * at.height * 4u;
    }
  }
  slot = texture_slot(gl, key, bytes, &found);
  if (found) return slot;

  started = gc_gl_now_ms();
  c = (GcGlTextureCommand *)gc_gl_render_command(gl->render, GC_GL_CMD_TEXTURE, sizeof(GcGlTextureCommand) + bytes);
  memset(c, 0, sizeof(*c));
  c->slot = slot;
  c->top = top;
  c->side_by_side = (uint32_t)side_by_side;
  c->bytes = (uint32_t)bytes;
  texels = (uint8_t *)(c + 1);
  if (side_by_side) {
    /* A one-texel texture with a second level (top_level): OpenGL has no
     * level 1 of a one-texel texture, so both texels go side by side in
     * level 0 and the shader reads the second at x = 1 (tex_size.w). */
    GcGxTexture at;
    gc_gx_texture_texel(m, texture, 0, 0, texels);
    gc_gx_texture_level(texture, 1, &at);
    gc_gx_texture_texel(m, &at, 0, 0, texels + 4);
  } else {
    size_t offset = 0;
    for (level = 0; level <= top; level++) {
      GcGxTexture at;
      unsigned s, t;
      gc_gx_texture_level(texture, level, &at);
      c->level_width[level] = at.width;
      c->level_height[level] = at.height;
      for (t = 0; t < at.height; t++) {
        for (s = 0; s < at.width; s++) {
          gc_gx_texture_texel(m, &at, s, t, texels + offset + ((size_t)t * at.width + s) * 4u);
        }
      }
      offset += (size_t)at.width * at.height * 4u;
    }
  }
  gc_gl_render_send(gl->render);
  gl->texture_decode_ms += gc_gl_now_ms() - started;
  gc_perf.textures_decoded++;
  gc_perf.texture_decode_ms += gc_gl_now_ms() - started;

  texture_slot_filled(gl, slot, key, bytes);
  return slot;
}

/* Whether the cache has a texture under `key`. */
static int texture_cached(const GcGlState *gl, uint64_t key) {
  unsigned i;
  for (i = 0; i < GC_GL_TEXTURES; i++) {
    if (gl->textures[i].used && gl->textures[i].key == key) return 1;
  }
  return 0;
}

/* What a map is bound by: the card's texture of a copy, when the map is one
 * (*copy_tag then names it), and otherwise its bytes - which, on a graphics
 * thread, are first made sure of (a copy's bytes it is still owed). */
static uint64_t map_key(GcMachine *m, const GcGxTexture *texture, unsigned top, uint64_t *copy_tag) {
  *copy_tag = 0;
  if (gc_gl_fast && gc_gl_card_filter) {
    uint64_t tag = 0;
    /* A copy is one level. A map that never leaves its first level - no
     * mipmapping asked for, whatever its range registers say - is the copy
     * and nothing else. */
    const int copy = (top == 0u || texture->mip_mode == 0u)
                         ? gc_gx_thread_copy_texture(m, texture->address, texture->format, texture->width,
                                                     texture->height, &tag)
                         : 0;
    if (copy) {
      const uint64_t words[2] = {tag, 0x636f707974657874ull};
      const uint64_t key = gc_gl_hash(words, sizeof(words), 0);
      /* A copy that no longer stands there but whose bytes do (2) is the
       * card's texture only while the cache has the slot that took it: the
       * texture went into that slot and nowhere else. */
      if (copy == 1 || texture_cached(state_of(m), key)) {
        *copy_tag = tag;
        return key;
      }
      gc_gx_thread_copy_forget(m, tag);
    }
  }
  {
    uint32_t first = 0, bytes = 0;
    gc_gx_texture_span(texture, &first, &bytes);
    gc_gx_thread_need_bytes(m, first, bytes);
  }
  return texture_key(m, texture, top);
}

/* Which maps the pipeline reads: each textured stage's, and the indirect
 * map of each textured stage that names an indirect matrix - which only
 * register 0x27 names, never an order register. */
static unsigned maps_read(const GcMachine *m) {
  const unsigned stages = ((m->gx.bp[0x00] >> 10) & 0xfu) + 1u;
  unsigned used = 0, stage;
  for (stage = 0; stage < stages && stage < 16u; stage++) {
    const uint32_t reg = m->gx.bp[0x28 + (stage >> 1)];
    const uint32_t order = (stage & 1u) ? (reg >> 12) : reg;
    const uint32_t command = m->gx.bp[0x10 + stage];
    if (!((order >> 6) & 1u)) continue;
    used |= 1u << (order & 7u);
    if (((command >> 9) & 3u) != 0u) used |= 1u << ((m->gx.bp[0x27] >> ((command & 3u) * 6u)) & 7u);
  }
  return used;
}

static void bind_textures(GcMachine *m) {
  GcGlState *gl = state_of(m);
  const unsigned used = maps_read(m);
  unsigned map;

  gl->bound_maps = used;
  for (map = 0; map < 8u; map++) {
    GcGxTexture texture;
    unsigned top;
    gc_gx_texture_read(m, map, &texture);
    top = top_level(&texture);
    gl->draw.tex_size[map * 4u] = (int32_t)texture.width;
    gl->draw.tex_size[map * 4u + 1u] = (int32_t)texture.height;
    gl->draw.tex_size[map * 4u + 2u] = (int32_t)top;
    gl->draw.tex_size[map * 4u + 3u] = (texture.width == 1u && texture.height == 1u && top >= 1u) ? 1 : 0;
    gl->draw.tex_mode[map * 4u] = (int32_t)texture.wrap_s;
    gl->draw.tex_mode[map * 4u + 1u] = (int32_t)texture.wrap_t;
    gl->draw.tex_mode[map * 4u + 2u] = (int32_t)texture.magnify_linear;
    gl->draw.tex_mode[map * 4u + 3u] = (int32_t)texture.minify_linear;
    gl->draw.tex_lod[map * 4u] = (int32_t)texture.mip_mode;
    gl->draw.tex_lod[map * 4u + 1u] = (int32_t)texture.min_lod;
    gl->draw.tex_lod[map * 4u + 2u] = (int32_t)texture.max_lod;
    gl->draw.tex_lod[map * 4u + 3u] = (int32_t)texture.diagonal_lod;
    gl->draw.tex_bias[map * 4u] = texture.lod_bias;
    if (!(used & (1u << map))) continue;
    {
      uint64_t copy_tag;
      gl->bound_keys[map] = map_key(m, &texture, top, &copy_tag);
      gl->bound_slots[map] = copy_tag ? texture_for_copy(m, &texture, gl->bound_keys[map], copy_tag)
                                      : texture_for(m, &texture, top, gl->bound_keys[map]);
    }
  }
  gl->cycles_at_bind = m->cpu.cycles;
}

/* Whether a bound map's bytes changed under an unchanged register state.
 * Checked when the processor has run since the maps were bound: nothing
 * else writes main memory, and a game that decodes a movie into a texture
 * between two draws of one field does exactly that. */
static int textures_changed(GcMachine *m) {
  GcGlState *gl = state_of(m);
  unsigned map;
  gl->revalidations++;
  gl->cycles_at_bind = m->cpu.cycles;
  for (map = 0; map < 8u; map++) {
    GcGxTexture texture;
    if (!(gl->bound_maps & (1u << map))) continue;
    gc_gx_texture_read(m, map, &texture);
    {
      uint64_t copy_tag;
      if (map_key(m, &texture, top_level(&texture), &copy_tag) != gl->bound_keys[map]) return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* The pipeline                                                        */

/* Which combiner register groups the pipeline reads before any stage has
 * written them in the same pixel, which it writes, and whether it is
 * independent - gc_gx_tev_plan_independent's rule, over the registers.
 * Bit 2k is register k's colour, bit 2k+1 its alpha. */
static int register_use(const GcMachine *m, unsigned *reads_out, unsigned *writes_out) {
  const unsigned stages = ((m->gx.bp[0x00] >> 10) & 0xfu) + 1u;
  unsigned ever_written = 0, written = 0, reads_before = 0, stage, i;
  int independent = 1;
  for (stage = 0; stage < stages && stage < 16u; stage++) {
    ever_written |= 1u << (2u * ((m->gx.bp[0xc0 + 2u * stage] >> 22) & 3u));
    ever_written |= 2u << (2u * ((m->gx.bp[0xc1 + 2u * stage] >> 22) & 3u));
  }
  for (stage = 0; stage < stages && stage < 16u; stage++) {
    const uint32_t colour_env = m->gx.bp[0xc0 + 2u * stage];
    const uint32_t alpha_env = m->gx.bp[0xc1 + 2u * stage];
    unsigned reads = 0;
    for (i = 0; i < 4u; i++) {
      const unsigned selector = (colour_env >> (12u - 4u * i)) & 0xfu;
      if (selector < 8u) reads |= ((selector & 1u) ? 2u : 1u) << (2u * (selector >> 1));
    }
    if (reads & ever_written & ~written) independent = 0;
    reads_before |= reads & ~written;
    written |= 1u << (2u * ((colour_env >> 22) & 3u));
    reads = 0;
    for (i = 0; i < 4u; i++) {
      const unsigned selector = (alpha_env >> (13u - 3u * i)) & 7u;
      if (selector < 4u) reads |= 2u << (2u * selector);
    }
    if (reads & ever_written & ~written) independent = 0;
    reads_before |= reads & ~written;
    written |= 2u << (2u * ((alpha_env >> 22) & 3u));
  }
  *reads_out = reads_before;
  *writes_out = written;
  return independent;
}

/* Everything the batch has to break for. The per-triangle registers - the
 * scissor box, its offset, the cull mode - are applied on the CPU to each
 * triangle and are left out. The texels are not here either; see
 * textures_changed. */
static uint64_t state_key(const GcMachine *m) {
  const GcGx *gx = &m->gx;
  uint64_t h = 0x676c7374u;
  unsigned reads, writes, k;
  h = gc_gl_hash(gx->bp + 0x00, sizeof(uint32_t) * 0x20u, h);
  h = gc_gl_hash(gx->bp + 0x25, sizeof(uint32_t) * (0x44u - 0x25u), h);
  h = gc_gl_hash(gx->bp + 0x64, sizeof(uint32_t) * 2u, h);
  h = gc_gl_hash(gx->bp + 0x80, sizeof(uint32_t) * (0xbcu - 0x80u), h);
  h = gc_gl_hash(gx->bp + 0xc0, sizeof(uint32_t) * (0xe0u - 0xc0u), h);
  /* Fog, the alpha test, and the depth texture's two registers. */
  h = gc_gl_hash(gx->bp + 0xe8, sizeof(uint32_t) * (0xf6u - 0xe8u), h);
  h = gc_gl_hash(gx->tev.konstant, sizeof(gx->tev) - offsetof(GcGxTev, konstant), h);
  /* The combiner registers, but only the groups a pixel reads as loaded:
   * a group a stage writes first is the pixel's own, and the value the
   * last triangle left in it (carry()) must not break the batch. */
  (void)register_use(m, &reads, &writes);
  for (k = 0; k < 4u; k++) {
    if (reads & (1u << (2u * k))) h = gc_gl_hash(gx->tev.reg[k], sizeof(int) * 3u, h);
    if (reads & (2u << (2u * k))) h = gc_gl_hash(&gx->tev.reg[k][3], sizeof(int), h);
  }
  return h;
}

static void build_shape(const GcMachine *m, uint32_t *shape) {
  const GcGx *gx = &m->gx;
  const unsigned stages = ((gx->bp[0x00] >> 10) & 0xfu) + 1u;
  unsigned stage, table, channel;
  int indirect = 0;

  memset(shape, 0, sizeof(uint32_t) * GC_GL_SHAPE_WORDS);
  shape[0] = stages;
  for (stage = 0; stage < stages && stage < 16u; stage++) {
    const uint32_t reg = gx->bp[0x28 + (stage >> 1)];
    const uint32_t order = ((stage & 1u) ? (reg >> 12) : reg) & 0x7fu;
    const uint32_t command = gx->bp[0x10 + stage];
    shape[1 + 4u * stage] = gx->bp[0xc0 + 2u * stage] & 0xffffffu;
    shape[2 + 4u * stage] = gx->bp[0xc1 + 2u * stage] & 0xffffffu;
    if ((order >> 6) & 1u) {
      shape[3 + 4u * stage] = order;
      /* The command matters only to a textured stage that names a matrix. */
      if (((command >> 9) & 3u) != 0u) {
        shape[4 + 4u * stage] = command & 0x7ffffu;
        indirect = 1;
      }
    } else {
      /* The map and coordinate of an untextured stage are never read. */
      shape[3 + 4u * stage] = 0;
    }
  }
  if (indirect) {
    shape[65] = gx->bp[0x25] & 0xffffu;
    shape[66] = gx->bp[0x26] & 0xffffu;
    shape[67] = gx->bp[0x27] & 0xffffffu;
  }
  for (table = 0; table < 4u; table++) {
    for (channel = 0; channel < 4u; channel++) {
      shape[68] |= (uint32_t)(gx->tev.swap[table][channel] & 3u) << (table * 8u + channel * 2u);
    }
  }
  shape[69] = gx->bp[0xf3] & 0xffffffu;
  /* Which way the depth test can reject a pixel before the program runs
   * (gc_gl_fragment_source): 1 for less or less-equal, 2 for greater or
   * greater-equal, 0 for the rest and whenever the test is off. */
  if (gx->bp[0x40] & 1u) {
    const unsigned function = (gx->bp[0x40] >> 1) & 7u;
    if (function == 1u || function == 3u) shape[71] = 1u;
    else if (function == 4u || function == 6u) shape[71] = 2u;
  }
  if (((GcGlState *)m->gl)->no_early_z) shape[71] = 0u;
  shape[70] = ((gx->bp[0xf1] >> 21) & 7u) | (((gx->bp[0xf1] >> 20) & 1u) << 3);
  if (gc_gl_fast) {
    /* The fast path has no depth bound to hand the card: it interpolates
     * depth itself. What its text does depend on is "depth before texture"
     * (gc_gl_fragment_source). */
    shape[71] = 0u;
    if (gc_gl_fast_early && ((gx->bp[0x43] >> 6) & 1u)) shape[72] = 1u;
  }
  {
    /* The depth texture (gc_gx_raster.c, depth_from_texture): the pixel's
     * depth comes out of the combiner, so the card can know nothing about
     * it before the program runs - no bound, no early test - and the fast
     * path's program has to write a depth, which otherwise it never does. */
    const unsigned z_texture = (gx->bp[0xf5] >> 2) & 3u;
    if (z_texture == 1u || z_texture == 2u) {
      shape[71] = 0u;
      shape[72] = 0u;
      shape[73] = 1u;
    }
  }
}

/* Whether a pixel needs the colour already in the framebuffer. */
static int needs_destination(const GcMachine *m) {
  const uint32_t mode = m->gx.bp[0x41];
  const uint32_t z_mode = m->gx.bp[0x40];
  const unsigned format = m->gx.bp[0x43] & 7u;
  const int write_colour = (mode >> 3) & 1u, write_alpha = (mode >> 4) & 1u;
  int dst = 0;

  /* The fast path never reads it: the card blends (gc_gl_render.c,
   * apply_fast_state), so a batch is one draw however its triangles
   * overlap. */
  if (gc_gl_fast) return 0;

  if ((mode >> 11) & 1u) {
    dst = 1; /* subtract */
  } else if (mode & 1u) {
    const unsigned destination_factor = (mode >> 5) & 7u, source_factor = (mode >> 8) & 7u;
    if (destination_factor != 0u || source_factor == 2u || source_factor == 3u || source_factor == 6u ||
        source_factor == 7u) {
      dst = 1;
    }
  } else if ((mode >> 1) & 1u) {
    const unsigned op = (mode >> 12) & 15u;
    if (op != 0u && op != 3u && op != 12u && op != 15u) dst = 1;
  }
  if (dst) ((GcGlState *)m->gl)->dst_blend++;
  /* Early depth with a write, and an alpha test that can fail: the pixel's
   * depth is written and its colour is not, which one fragment can only
   * say by writing the old colour back. */
  if (((m->gx.bp[0x43] >> 6) & 1u) && ((z_mode >> 4) & 1u)) {
    int a;
    for (a = 0; a < 256; a++) {
      if (!gc_gx_alpha_test(m, a)) {
        if (!dst) ((GcGlState *)m->gl)->dst_alpha++;
        dst = 1;
        break;
      }
    }
  }
  /* A channel the enables leave alone is still rounded to the pixel
   * format when the pixel is written (gc_efb_set_color). For the eight-bit
   * formats that leaves colour unchanged and makes alpha 255, which the
   * card's colour mask can say; for the narrow ones it can change what was
   * there, which only a destination read can say. */
  if (format == 1u && (!write_colour || !write_alpha)) dst = 1;
  if (format == 2u && !write_colour) dst = 1;
  return dst;
}

/* ------------------------------------------------------------------ */
/* Batches                                                             */

static void flush(GcMachine *m) {
  GcGlState *gl = state_of(m);
  const size_t tris = sizeof(GcGlTri) * gl->count;
  const size_t records = sizeof(uint32_t) * 4u * GC_GL_RECORD * gl->count;
  GcGlDrawCommand *c;
  double started;
  if (!gl->count) return;
  started = gc_gl_now_ms();
  c = (GcGlDrawCommand *)gc_gl_render_command(gl->render, GC_GL_CMD_DRAW, sizeof(*c) + tris + records);
  c->count = gl->count;
  c->needs_dst = (uint32_t)gl->needs_dst;
  memcpy((char *)(c + 1), gl->tris, tris);
  memcpy((char *)(c + 1) + tris, gl->records, records);
  gc_gl_render_send(gl->render);
  gl->draws_sent++;
  gl->count = 0;
  gl->memory_is_stale = 1;
  gl->flush_ms += gc_gl_now_ms() - started;
}

static void close_batch(GcMachine *m) {
  GcGlState *gl = state_of(m);
  flush(m);
  gl->open = 0;
}

static void begin_batch(GcMachine *m) {
  GcGlState *gl = state_of(m);
  const GcGx *gx = &m->gx;
  GcGlStateCommand *c;
  unsigned i, k;

  gl->batches++;
  gl->open = 1;
  gl->count = 0;
  gl->cpu = !register_use(m, &gl->reads, &gl->writes);
  if (gl->cpu) return;

  if (gl->card_is_stale) upload(m);

  gl->needs_dst = needs_destination(m);
  build_shape(m, gl->shape);

  memcpy(gl->draw.shape, gl->shape, sizeof(gl->shape));
  for (i = 0; i < 4u; i++) {
    for (k = 0; k < 4u; k++) gl->draw.tev_register[i * 4u + k] = gx->tev.reg[i][k];
  }
  for (i = 0; i < 16u; i++) {
    int konstant[4];
    gc_gx_tev_konstant(m, i, konstant);
    for (k = 0; k < 4u; k++) gl->draw.konstant[i * 4u + k] = konstant[k];
  }
  gl->draw.fog[0] = gx->bp[0xee];
  gl->draw.fog[1] = gx->bp[0xef];
  gl->draw.fog[2] = gx->bp[0xf0];
  gl->draw.fog[3] = gx->bp[0xf1];
  gl->draw.fog[4] = gx->bp[0xf2];
  gl->draw.fog[5] = gx->bp[0xe8];
  for (i = 0; i < 10u; i++) {
    const uint32_t reg = gx->bp[0xe9u + (i >> 1)];
    gl->draw.fog_table[i] = (i & 1u) ? ((reg >> 12) & 0xfffu) : (reg & 0xfffu);
  }
  for (i = 0; i < 9u; i++) gl->draw.indirect_matrix[i] = gx->bp[0x06u + i];
  for (i = 0; i < 8u; i++) {
    gl->draw.coord_size[i * 4u] = gx->bp[0x30u + 2u * i];
    gl->draw.coord_size[i * 4u + 1u] = gx->bp[0x31u + 2u * i];
  }
  {
    unsigned coordinate, map;
    for (coordinate = 0; coordinate < 8u; coordinate++) {
      for (map = 0; map < 8u; map++) {
        float scale[2];
        gc_gx_texcoord_scale(m, coordinate, map, scale);
        memcpy(&gl->draw.scale_bits[coordinate * 16u + map * 2u], &scale[0], 4);
        memcpy(&gl->draw.scale_bits[coordinate * 16u + map * 2u + 1u], &scale[1], 4);
      }
    }
  }
  gl->draw.pixel_engine[0] = gx->bp[0x41];
  gl->draw.pixel_engine[1] = gx->bp[0x42];
  gl->draw.pixel_engine[2] = gx->bp[0x43];
  gl->draw.pixel_engine[3] = gx->bp[0x40];
  gl->draw.pixel_engine[4] = (uint32_t)gl->needs_dst;

  /* The reference's own plan of the same registers: for carry(), and for
   * which coordinate sets need a gradient. */
  gc_gx_tev_prepare(m, gl->plan);
  gl->draw.pixel_engine[5] = gl->plan->gradients_used;
  gl->draw.pixel_engine[6] = gx->bp[0xf5] & 0xfu;        /* the depth texture: width and operation */
  gl->draw.pixel_engine[7] = gx->bp[0xf4] & 0x00ffffffu; /* and its bias */

  bind_textures(m);

  c = (GcGlStateCommand *)gc_gl_render_command(gl->render, GC_GL_CMD_STATE, sizeof(GcGlStateCommand));
  c->draw = gl->draw;
  memcpy(c->shape, gl->shape, sizeof(c->shape));
  c->z_mode = gx->bp[0x40];
  c->blend_mode = gx->bp[0x41];
  c->needs_dst = (uint32_t)gl->needs_dst;
  c->maps = gl->bound_maps;
  memcpy(c->slot, gl->bound_slots, sizeof(c->slot));
  c->field = m->field;
  gc_gl_render_send(gl->render);
}

/* ------------------------------------------------------------------ */
/* The combiner registers a triangle leaves behind                      */

/* The reference writes the combiner's colour registers as each pixel runs,
 * so after a triangle they hold what its *last* pixel computed, and a
 * later triangle whose pipeline reads a register no stage has written yet
 * sees that. The card cannot hand a value from one pixel to the next, so
 * this works the last pixel out on the CPU with the reference's own
 * combiner: the last row with a covered pixel, its last covered pixel.
 *
 * One thing it cannot see is the depth buffer, which is on the card. With
 * early depth testing on, the last covered pixel may fail the test and
 * never run the combiner; this assumes it passed, counts the assumption,
 * and marks the groups it wrote as suspect until a triangle writes them
 * again with nothing assumed. A pipeline that reads a suspect group is
 * counted too, so a run says whether the assumption ever mattered.
 *
 * Only the last triangle of a run that covers anything matters, so a run
 * handed over together (gc_gl_pieces) is carried once, from its end. */
typedef struct GcGlPoint {
  double w[3], a[3], c[3];
  float coordinate[GC_GX_MAX_TEXCOORDS][2];
} GcGlPoint;

static void edges_at(const GcGlTri *t, double sample_x, double sample_y, double w[3]) {
  const double *x = t->x, *y = t->y;
  w[0] = (x[2] - x[1]) * (sample_y - y[1]) - (y[2] - y[1]) * (sample_x - x[1]);
  w[1] = (x[0] - x[2]) * (sample_y - y[2]) - (y[0] - y[2]) * (sample_x - x[2]);
  w[2] = (x[1] - x[0]) * (sample_y - y[0]) - (y[1] - y[0]) * (sample_x - x[0]);
}

static void finish_point(const GcGlTri *t, const GcGxPoint *points, unsigned used, GcGlPoint *p) {
  const double a0 = p->w[0] / t->area, a1 = p->w[1] / t->area, a2 = p->w[2] / t->area;
  const double sum = a0 * t->inverse_w[0] + a1 * t->inverse_w[1] + a2 * t->inverse_w[2];
  double c0, c1, c2;
  p->a[0] = a0;
  p->a[1] = a1;
  p->a[2] = a2;
  if (sum > 0.0) {
    const double scale = 1.0 / sum;
    c0 = a0 * t->inverse_w[0] * scale;
    c1 = a1 * t->inverse_w[1] * scale;
    c2 = a2 * t->inverse_w[2] * scale;
  } else {
    c0 = a0;
    c1 = a1;
    c2 = a2;
  }
  p->c[0] = c0;
  p->c[1] = c1;
  p->c[2] = c2;
  while (used) {
    unsigned channel = 0;
    while (!((used >> channel) & 1u)) channel++;
    used &= used - 1u;
    p->coordinate[channel][0] = (float)(c0 * points[0].texcoord[channel][0] + c1 * points[1].texcoord[channel][0] +
                                        c2 * points[2].texcoord[channel][0]);
    p->coordinate[channel][1] = (float)(c0 * points[0].texcoord[channel][1] + c1 * points[1].texcoord[channel][1] +
                                        c2 * points[2].texcoord[channel][1]);
  }
}

/* Returns 1 when the triangle covers a pixel (and so has been carried). */
static int carry(GcMachine *m, const GcGlTri *t, const GcGxPoint *points) {
  GcGlState *gl = state_of(m);
  const GcGxTevPlan *plan = gl->plan;
  const unsigned used = plan->coordinates_used;
  int px, py;

  for (py = t->box.bottom; py >= t->box.top; py--) {
    const double sample_y = (double)py + SAMPLE_POINT;
    int lo, hi;
    /* The last covered pixel of the last row that has one. */
    covered_run(t, py, &lo, &hi, NO_HINT, NO_HINT);
    if (lo > hi) continue;
    px = hi;
    {
      GcGlPoint centre;
      edges_at(t, (double)px + SAMPLE_POINT, sample_y, centre.w);
      finish_point(t, points, used, &centre);
      {
        GcGxTevPixel pixel;
        float coordinate[GC_GX_MAX_TEXCOORDS][2];
        float gradient[GC_GX_MAX_TEXCOORDS][4];
        unsigned channel;
        memset(&pixel, 0, sizeof(pixel));
        memset(coordinate, 0, sizeof(coordinate));
        memset(gradient, 0, sizeof(gradient));
        for (channel = 0; channel < 4u; channel++) {
          const double value = centre.c[0] * (double)points[0].color[0][channel] +
                               centre.c[1] * (double)points[1].color[0][channel] +
                               centre.c[2] * (double)points[2].color[0][channel];
          const double r = value + 0.5;
          pixel.raster[channel] = (int)(r < 0.0 ? 0.0 : (r > 255.0 ? 255.0 : r));
        }
        if (used) {
          GcGlPoint right_of, under;
          unsigned left = used;
          edges_at(t, (double)(px + 1) + SAMPLE_POINT, sample_y, right_of.w);
          finish_point(t, points, used, &right_of);
          edges_at(t, (double)px + SAMPLE_POINT, (double)(py + 1) + SAMPLE_POINT, under.w);
          finish_point(t, points, used, &under);
          while (left) {
            channel = 0;
            while (!((left >> channel) & 1u)) channel++;
            left &= left - 1u;
            coordinate[channel][0] = centre.coordinate[channel][0];
            coordinate[channel][1] = centre.coordinate[channel][1];
            gradient[channel][0] = right_of.coordinate[channel][0] - centre.coordinate[channel][0];
            gradient[channel][1] = right_of.coordinate[channel][1] - centre.coordinate[channel][1];
            gradient[channel][2] = under.coordinate[channel][0] - centre.coordinate[channel][0];
            gradient[channel][3] = under.coordinate[channel][1] - centre.coordinate[channel][1];
          }
        }
        gc_gx_tev_run_planned(m, gl->plan, &m->gx.tev, (const float(*)[2])coordinate,
                              (const float(*)[4])gradient, &pixel);
      }
      gl->carries++;
      if (gc_gx_early_depth_test(m) && (m->gx.bp[0x40] & 1u)) {
        gl->carry_unchecked++;
        gl->suspect |= gl->writes;
      } else {
        gl->suspect &= ~gl->writes;
      }
      return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* A triangle                                                          */

static void scissor_box(const GcMachine *m, GcGlBox *box, int *shift_x, int *shift_y) {
  const uint32_t top_left = m->gx.bp[0x20];
  const uint32_t bottom_right = m->gx.bp[0x21];
  const uint32_t offset = m->gx.bp[0x59];
  *shift_x = (int)(offset & 0x3ffu) * 2;
  *shift_y = (int)((offset >> 10) & 0x3ffu) * 2;
  box->top = (int)(top_left & 0xfffu) - *shift_y;
  box->left = (int)((top_left >> 12) & 0xfffu) - *shift_x;
  box->bottom = (int)(bottom_right & 0xfffu) - *shift_y;
  box->right = (int)((bottom_right >> 12) & 0xfffu) - *shift_x;
  if (box->left < 0) box->left = 0;
  if (box->top < 0) box->top = 0;
  if (box->right > (int)GC_EFB_WIDTH - 1) box->right = (int)GC_EFB_WIDTH - 1;
  if (box->bottom > (int)GC_EFB_HEIGHT - 1) box->bottom = (int)GC_EFB_HEIGHT - 1;
}

/* gc_gx_raster_one's set-up, expression for expression: the same doubles,
 * the same rejections, the same bounding box. Returns 0 when nothing is
 * drawn. */
static int setup_triangle(const GcMachine *m, const GcGxPoint *points, GcGlTri *t) {
  GcGlBox box;
  int shift_x, shift_y, left, right, top, bottom;
  const double *x = t->x, *y = t->y;
  unsigned i;

  scissor_box(m, &box, &shift_x, &shift_y);
  for (i = 0; i < 3; i++) {
    t->x[i] = points[i].screen[0] - (double)shift_x;
    t->y[i] = points[i].screen[1] - (double)shift_y;
    t->z[i] = points[i].screen[2];
    t->inverse_w[i] = (points[i].clip[3] != 0.0f) ? 1.0 / (double)points[i].clip[3] : 0.0;
  }
  t->area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
  if (t->area == 0.0) return 0;
  switch ((m->gx.bp[0x00] >> 14) & 3u) {
    case 1: if (t->area < 0.0) return 0; break;
    case 2: if (t->area > 0.0) return 0; break;
    case 3: return 0;
    default: break;
  }
  left = (int)(x[0] < x[1] ? (x[0] < x[2] ? x[0] : x[2]) : (x[1] < x[2] ? x[1] : x[2])) - 1;
  right = (int)(x[0] > x[1] ? (x[0] > x[2] ? x[0] : x[2]) : (x[1] > x[2] ? x[1] : x[2])) + 1;
  top = (int)(y[0] < y[1] ? (y[0] < y[2] ? y[0] : y[2]) : (y[1] < y[2] ? y[1] : y[2])) - 1;
  bottom = (int)(y[0] > y[1] ? (y[0] > y[2] ? y[0] : y[2]) : (y[1] > y[2] ? y[1] : y[2])) + 1;
  if (left < box.left) left = box.left;
  if (top < box.top) top = box.top;
  if (right > box.right) right = box.right;
  if (bottom > box.bottom) bottom = box.bottom;
  if (left > right || top > bottom) return 0;
  t->box.left = left;
  t->box.right = right;
  t->box.top = top;
  t->box.bottom = bottom;
  return 1;
}

static void put_double(uint32_t *at, double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  at[0] = (uint32_t)bits;
  at[1] = (uint32_t)(bits >> 32);
}

static void put_record(uint32_t *r, const GcGlTri *t, const GcGxPoint *points) {
  unsigned k, v;
  memset(r, 0, sizeof(uint32_t) * 4u * GC_GL_RECORD);
  r[0] = (uint32_t)t->box.left;
  r[1] = (uint32_t)t->box.top;
  r[2] = (uint32_t)t->box.right;
  r[3] = (uint32_t)t->box.bottom;
  put_double(r + 4, t->x[0]);
  put_double(r + 6, t->x[1]);
  put_double(r + 8, t->x[2]);
  put_double(r + 10, t->y[0]);
  put_double(r + 12, t->y[1]);
  put_double(r + 14, t->y[2]);
  put_double(r + 16, t->z[0]);
  put_double(r + 18, t->z[1]);
  put_double(r + 20, t->z[2]);
  put_double(r + 22, t->inverse_w[0]);
  put_double(r + 24, t->inverse_w[1]);
  put_double(r + 26, t->inverse_w[2]);
  put_double(r + 28, t->area);
  for (v = 0; v < 3u; v++) {
    const uint8_t *c = points[v].color[0];
    r[30 + v] = (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
  }
  for (k = 0; k < GC_GX_MAX_TEXCOORDS; k++) {
    const uint32_t *base = r + 36 + 8u * k;
    memcpy((uint32_t *)base + 0, &points[0].texcoord[k][0], 4);
    memcpy((uint32_t *)base + 1, &points[0].texcoord[k][1], 4);
    memcpy((uint32_t *)base + 2, &points[1].texcoord[k][0], 4);
    memcpy((uint32_t *)base + 3, &points[1].texcoord[k][1], 4);
    memcpy((uint32_t *)base + 4, &points[2].texcoord[k][0], 4);
    memcpy((uint32_t *)base + 5, &points[2].texcoord[k][1], 4);
  }
}

/* A triangle the card cannot draw exactly - a pipeline whose pixels read
 * what the pixel before them left in the combiner's registers - goes to
 * the reference rasteriser, with the framebuffer brought back first and
 * sent up again after. Slow, and exact. */
static void cpu_triangle(GcMachine *m, const GcGxPoint *points) {
  GcGlState *gl = state_of(m);
  flush(m);
  if (gl->memory_is_stale) download(m);
  if (gl->reads & gl->suspect) gl->suspect_reads++;
  m->gl = NULL;
  gc_gx_raster_one(m, points);
  m->gl = gl;
  gl->card_is_stale = 1;
  gl->cpu_triangles++;
  if (!(gl->reads & gl->suspect)) gl->suspect &= ~gl->writes;
  else gl->suspect |= gl->writes;
}

/* The batch for the registers as they stand: the open one if nothing it
 * depends on has changed, or a new one. Afterwards gl->cpu says whether the
 * reference has to draw instead. */
static void batch_for_registers(GcMachine *m) {
  GcGlState *gl = state_of(m);
  uint64_t key;
  {
    const double keyed = gc_gl_now_ms();
    key = state_key(m);
    gl->key_ms += gc_gl_now_ms() - keyed;
  }
  if (gl->open && key != gl->state) close_batch(m);
  if (gl->open && !gl->cpu && m->cpu.cycles != gl->cycles_at_bind && textures_changed(m)) close_batch(m);
  gl->state = key;
  if (!gl->open) {
    const double begun = gc_gl_now_ms();
    begin_batch(m);
    gl->begin_ms += gc_gl_now_ms() - begun;
  }
  if (!gl->cpu && gl->card_is_stale) {
    /* The framebuffer was replaced under an open batch (a CPU triangle, a
     * poke): send it again and bind everything again. */
    close_batch(m);
    begin_batch(m);
  }
}

void gc_gl_triangle(GcMachine *m, const GcGxPoint *points) {
  GcGlState *gl = m ? state_of(m) : NULL;
  const double started = gl ? gc_gl_now_ms() : 0.0;
  GcGlTri t;

  if (!gl) return;
  if (!setup_triangle(m, points, &t)) goto done;
  gl->triangles++;
  batch_for_registers(m);
  if (gl->cpu) {
    cpu_triangle(m, points);
    goto done;
  }

  if (gl->reads & gl->suspect) gl->suspect_reads++;
  if (gl->count == gl->capacity) flush(m);
  gl->tris[gl->count] = t;
  put_record(gl->records + 4u * GC_GL_RECORD * gl->count, &t, points);
  gl->box_pixels += (uint64_t)(t.box.right - t.box.left + 1) * (uint64_t)(t.box.bottom - t.box.top + 1);
  gl->count++;
  {
    const double carried = gc_gl_now_ms();
    carry(m, &t, points);
    gl->carry_ms += gc_gl_now_ms() - carried;
  }

done:
  account(gl, m, started);
}

static void piece_triangle(const GcGxRasterPiece *piece, GcGlTri *t) {
  memcpy(t->x, piece->x, sizeof(t->x));
  memcpy(t->y, piece->y, sizeof(t->y));
  memcpy(t->z, piece->z, sizeof(t->z));
  memcpy(t->inverse_w, piece->inverse_w, sizeof(t->inverse_w));
  t->area = piece->area;
  t->box.left = piece->left;
  t->box.right = piece->right;
  t->box.top = piece->top;
  t->box.bottom = piece->bottom;
}

/* Records for pieces, on any thread: each writes only its own slot. */
typedef struct GcGlFill {
  GcGlState *gl;
  const GcGxRasterPiece *const *pieces;
  unsigned slot, count;
  long next;
} GcGlFill;

#define GC_GL_FILL_RUN 32
static void fill_records(void *argument, unsigned id) {
  GcGlFill *const job = (GcGlFill *)argument;
  long run;
  (void)id;
  while ((run = gc_workers_take(&job->next)) * GC_GL_FILL_RUN < (long)job->count) {
    unsigned i;
    const unsigned end = (unsigned)((run + 1) * GC_GL_FILL_RUN);
    for (i = (unsigned)(run * GC_GL_FILL_RUN); i < end && i < job->count; i++) {
      const unsigned slot = job->slot + i;
      GcGlTri *const t = &job->gl->tris[slot];
      piece_triangle(job->pieces[i], t);
      put_record(job->gl->records + 4u * GC_GL_RECORD * slot, t, job->pieces[i]->points);
    }
  }
}

void gc_gl_pieces(GcMachine *m, const GcGxRasterPiece *const *pieces, unsigned count) {
  GcGlState *gl = m ? state_of(m) : NULL;
  const double started = gl ? gc_gl_now_ms() : 0.0;
  unsigned taken = 0, i;

  if (!gl || !count) return;
  gl->triangles += count;
  gl->piece_runs++;
  batch_for_registers(m);
  if (gl->cpu) {
    /* Not expected - the rasteriser holds only independent plans - but
     * drawn right if it happens. */
    for (i = 0; i < count; i++) cpu_triangle(m, pieces[i]->points);
    goto done;
  }
  if (gl->reads & gl->suspect) gl->suspect_reads++;

  while (taken < count) {
    GcGlFill job;
    if (gl->count == gl->capacity) flush(m);
    job.gl = gl;
    job.pieces = pieces + taken;
    job.slot = gl->count;
    job.count = gl->capacity - gl->count < count - taken ? gl->capacity - gl->count : count - taken;
    job.next = 0;
    if (job.count < 256u || !gc_workers_count() || GC_WORKERS_RUN_SHARED(fill_records, &job) != 0) fill_records(&job, 0);
    for (i = 0; i < job.count; i++) {
      const GcGlBox *b = &gl->tris[job.slot + i].box;
      gl->box_pixels += (uint64_t)(b->right - b->left + 1) * (uint64_t)(b->bottom - b->top + 1);
    }
    gl->count += job.count;
    taken += job.count;
  }

  {
    /* The registers are left by the last piece that covers a pixel. */
    const double carried = gc_gl_now_ms();
    for (i = count; i-- > 0;) {
      GcGlTri t;
      piece_triangle(pieces[i], &t);
      if (carry(m, &t, pieces[i]->points)) break;
    }
    gl->carry_ms += gc_gl_now_ms() - carried;
  }

done:
  account(gl, m, started);
}

/* ------------------------------------------------------------------ */
/* Copies                                                              */

/* An EFB copy, made from the card's framebuffer: the render thread reads its
 * rows and the readback thread writes main memory (gc_gl_render.c). Here the
 * pages it will write are held first (gc_memory.h), before the processor can
 * run again, so the machine cannot read one early or write one that the copy
 * would then overwrite: an access waits for the copy, and then sees exactly
 * what an immediate copy would have left. The clear goes to the card after
 * the read.
 *
 * Where holds are not available (no second view of main memory, no second
 * context) the copy is made here, at once, from the framebuffer brought
 * back into memory. */
/* A copy asked for by `trigger` (prepared here), or already prepared with
 * its pages held under held_tag (the graphics thread's). */
static int copy_with(GcMachine *m, uint32_t trigger, const GcEfbCopyJob *prepared, uint64_t held_tag,
                     uint32_t held_first, uint32_t held_end) {
  GcGlState *gl = m ? state_of(m) : NULL;
  GcEfbCopyJob *job;
  GcGlCopyCommand *c;
  double started;
  unsigned first_row, last_row, rows = 0, depth = 0;
  int clears, texture_format, display;
  GcGlClearCommand clear = {0, 0, 0, 0, 0, 0};
  if (!gl) return 0;
  started = gc_gl_now_ms();
  flush(m);

  if (!gc_gl_render_async_copies(gl->render)) {
    GcEfbCopyJob now;
    gc_gl_sync_to_memory(m);
    if (prepared ? (now = *prepared, 1) : gc_efb_copy_prepare(m, trigger, &now)) {
      uint32_t x, y;
      now.color = m->efb.color;
      now.depth = m->efb.depth;
      now.ram = gl->memory_for_copies;
      gc_efb_copy_run(&now, 1);
      gc_gx_thread_copy_landing(held_tag, gl->memory_for_copies, held_first, held_end);
      if (now.clears) {
        const uint32_t room = (now.left < GC_EFB_WIDTH) ? GC_EFB_WIDTH - now.left : 0u;
        const uint32_t across = (now.width < room) ? now.width : room;
        for (y = 0; y < now.height && now.top + y < GC_EFB_HEIGHT; y++) {
          const size_t row = (size_t)(now.top + y) * GC_EFB_WIDTH + now.left;
          for (x = 0; x < across; x++) {
            m->efb.color[row + x] = now.clear_colour;
            m->efb.depth[row + x] = now.clear_depth;
          }
        }
        gc_gl_invalidate(m);
      }
    }
    gc_gx_thread_copy_landed(held_tag);
    if (held_tag) gc_memory_release_tagged(gl->memory, held_first, held_end - held_first, held_tag, 1);
    gl->copies_sync++;
    gl->copy_ms += gc_gl_now_ms() - started;
    account(gl, m, started);
    return 1;
  }

  if (gl->card_is_stale) upload(m);
  job = (GcEfbCopyJob *)malloc(sizeof(GcEfbCopyJob));
  if (!job) {
    /* No room to hand it over: made at once instead. */
    gc_gl_render_settle(gl->render);
    gc_gl_sync_to_memory(m);
    account(gl, m, started);
    if (prepared) {
      fprintf(stderr, "gl: out of memory for a copy\n");
      abort();
    }
    return 0;
  }
  if (prepared) {
    *job = *prepared;
  } else if (!gc_efb_copy_prepare(m, trigger, job)) {
    /* Nothing written and nothing cleared, as gc_efb_copy. */
    free(job);
    account(gl, m, started);
    return 1;
  }

  /* The rows the filter reads: the rectangle's and one either side. */
  first_row = job->top > 0u ? job->top - 1u : 0u;
  last_row = job->top + job->rows;
  if (last_row > GC_EFB_HEIGHT - 1u) last_row = GC_EFB_HEIGHT - 1u;
  if (job->rows && first_row <= last_row) {
    rows = last_row - first_row + 1u;
    depth = job->efb == GC_EFB_Z24;
  }
  clears = job->clears;
  if (clears) {
    const uint32_t room = (job->left < GC_EFB_WIDTH) ? GC_EFB_WIDTH - job->left : 0u;
    const uint32_t below = (job->top < GC_EFB_HEIGHT) ? GC_EFB_HEIGHT - job->top : 0u;
    clear.left = job->left;
    clear.top = job->top;
    clear.across = (job->width < room) ? job->width : room;
    clear.down = (job->height < below) ? job->height : below;
    clear.colour = job->clear_colour;
    clear.depth = job->clear_depth;
  }

  c = (GcGlCopyCommand *)gc_gl_render_command(gl->render, GC_GL_CMD_COPY, sizeof(GcGlCopyCommand));
  c->job = job;
  c->asked_ms = gc_gl_now_ms();
  c->first_row = first_row;
  c->rows = rows;
  c->depth = depth;
  /* On the fast path the card keeps the copy as a texture too, for the
   * draws that sample it before its bytes are back (gc_gx_thread.h). */
  c->texture_format = -1;
  if (held_tag && rows && gc_gl_fast && gc_gl_card_filter && gc_gl_render_copy_textures(gl->render)) {
    c->texture_format = gc_efb_copy_texture_format(job, NULL, NULL);
  }
  texture_format = c->texture_format;
  display = job->to_display;
  {
    uint32_t first, end;
    gc_efb_copy_bytes(job, &first, &end);
    if (first > gc_ram_size) first = gc_ram_size;
    if (end > gc_ram_size) end = gc_ram_size;
    if (end < first) end = first;
    c->held_first = first;
    c->held_end = end;
    if (held_tag) {
      c->tag = held_tag;
    } else {
      c->tag = gc_memory_next_tag();
      gc_memory_hold_tagged(gl->memory, first, end - first, c->tag);
    }
  }
  /* From here the job belongs to the other threads. */
  gc_gl_render_send(gl->render);
  if (texture_format >= 0) gc_gx_thread_copy_has_texture(m, held_tag, (unsigned)texture_format);

  if (clears && clear.across && clear.down) {
    *(GcGlClearCommand *)gc_gl_render_command(gl->render, GC_GL_CMD_CLEAR, sizeof(GcGlClearCommand)) = clear;
    gc_gl_render_send(gl->render);
    /* The masks belong to the next batch again. */
    gl->open = 0;
    gl->memory_is_stale = 1;
  }

  gl->copies_async++;
  if (gc_gx_thread_on_thread()) {
    /* A graphics thread can hand the card work faster than it draws, and
     * the pictures a frontend shows would then trail the game by however
     * much is queued. It waits instead once a few frames - display copies -
     * are still on their way; the machine's thread waits for it in turn
     * (gc_gx_thread.c). */
    gc_gl_render_limit_copies(gl->render, gl->copy_lag, display);
  }
  gl->copy_ms += gc_gl_now_ms() - started;
  account(gl, m, started);
  return 1;
}

int gc_gl_copy(GcMachine *m, uint32_t trigger) { return copy_with(m, trigger, NULL, 0, 0, 0); }

int gc_gl_copy_prepared(GcMachine *m, const GcEfbCopyJob *job, uint64_t tag, uint32_t first, uint32_t end) {
  return copy_with(m, job->trigger, job, tag, first, end);
}

void gc_gl_settle(GcMachine *m) {
  GcGlState *gl;
  m = gc_gx_thread_target(m);
  gl = m ? state_of(m) : NULL;
  if (gl) gc_gl_render_settle(gl->render);
}

void gc_gl_sync_to_memory(GcMachine *m) {
  GcGlState *gl;
  double started;
  m = gc_gx_thread_target(m);
  gl = m ? state_of(m) : NULL;
  if (!gl) return;
  started = gc_gl_now_ms();
  gl->syncs++;
  flush(m);
  if (gl->memory_is_stale) download(m);
  gl->sync_ms += gc_gl_now_ms() - started;
  account(gl, m, started);
}

void gc_gl_invalidate(GcMachine *m) {
  GcGlState *gl;
  m = gc_gx_thread_target(m);
  gl = m ? state_of(m) : NULL;
  if (!gl) return;
  /* Triangles already handed over were drawn before whatever wrote main
   * memory, so they are drawn and brought back first; then the card's copy
   * is stale. Dropping them instead would lose them under a one-pixel
   * poke. Copies still on their way land first too: they were asked for
   * earlier. */
  gc_gl_render_settle(gl->render);
  gc_gl_sync_to_memory(m);
  gl->open = 0;
  gl->card_is_stale = 1;
}

int gc_gl_display_picture(GcMachine *m, uint32_t *out, unsigned pitch, unsigned rows, unsigned *width,
                          unsigned *height, uint64_t *serial) {
  uint32_t first, display_width, display_height;
  /* The video interface is the machine's own, never a shadow's. */
  if (!m || gc_gl_scale <= 1) return -1;
  if (gc_efb_display_place(m, &first, &display_width, &display_height) != 0) return -1;
  /* Everything the machine has issued is drawn first, as it always was
   * here: the caller has the machine to itself. */
  (void)gc_gx_thread_target(m);
  return gc_gl_picture_at(m, first, display_width, display_height, out, pitch, rows, width, height, serial);
}

int gc_gl_picture_at(GcMachine *m, uint32_t first, uint32_t display_width, uint32_t display_height, uint32_t *out,
                     unsigned pitch, unsigned rows, unsigned *width, unsigned *height, uint64_t *serial) {
  GcGlState *gl;
  if (!m || gc_gl_scale <= 1) return -1;
  /* Not gc_gx_thread_target: that waits for the graphics thread's queue,
   * which the machine may be filling. */
  m = gc_gx_thread_drawn_on(m);
  gl = m ? state_of(m) : NULL;
  if (!gl) return -1;
  return gc_gl_render_picture(gl->render, first, display_width, display_height, out, pitch, rows, width, height,
                              serial);
}

void gc_gl_forget_pictures(GcMachine *m) {
  GcGlState *gl;
  m = gc_gx_thread_target(m);
  gl = m ? state_of(m) : NULL;
  if (gl) gc_gl_render_forget_pictures(gl->render);
}

int gc_gl_write_picture(GcMachine *m, const char *path) {
  unsigned width = 0, height = 0, got_width = 0, got_height = 0, x, y;
  uint64_t serial = 0;
  uint32_t *pixels;
  FILE *file;
  int result = -1;
  gc_gl_settle(m);
  if (gc_gl_display_picture(m, NULL, 0, 0, &width, &height, &serial) != 0) return -1;
  pixels = (uint32_t *)malloc((size_t)width * height * sizeof(uint32_t));
  if (!pixels) return -1;
  if (gc_gl_display_picture(m, pixels, width, height, &got_width, &got_height, &serial) == 0 && got_width == width &&
      got_height == height && (file = fopen(path, "wb")) != NULL) {
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (y = 0; y < height; y++) {
      for (x = 0; x < width; x++) {
        const uint32_t argb = pixels[(size_t)y * width + x];
        const uint8_t rgb[3] = {(uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb};
        fwrite(rgb, 1, 3, file);
      }
    }
    result = fclose(file) == 0 ? 0 : -1;
  }
  free(pixels);
  return result;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */

static GcGlShaderMode parse_mode(const char *text, GcMachine *m) {
  if (!text || !*text || !strcmp(text, "hybrid")) return GC_GL_SHADERS_HYBRID;
  if (!strcmp(text, "uber") || !strcmp(text, "general")) return GC_GL_SHADERS_UBER;
  if (!strcmp(text, "specialised") || !strcmp(text, "specialized")) return GC_GL_SHADERS_SPECIALISED;
  if (m->log) fprintf(m->log, "gl: shader mode \"%s\" is not uber, specialised or hybrid; using hybrid\n", text);
  return GC_GL_SHADERS_HYBRID;
}

static void release(GcGlState *gl) {
  if (gl->readback_context) SDL_GL_DeleteContext(gl->readback_context);
  if (gl->readback_window) SDL_DestroyWindow(gl->readback_window);
  if (gl->compile_context) SDL_GL_DeleteContext(gl->compile_context);
  if (gl->compile_window) SDL_DestroyWindow(gl->compile_window);
  if (gl->context) SDL_GL_DeleteContext(gl->context);
  if (gl->window) SDL_DestroyWindow(gl->window);
  free(gl->records);
  free(gl->tris);
  free(gl->plan);
  free(gl);
}

/* A hidden window with a context sharing objects with the current one. */
static void shared_context(SDL_Window *window, SDL_GLContext context, const char *title, SDL_Window **window_out,
                           SDL_GLContext *context_out) {
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  *window_out = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 16, 16,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  *context_out = *window_out ? SDL_GL_CreateContext(*window_out) : NULL;
  SDL_GL_MakeCurrent(window, context);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
}

int gc_gl_start(GcMachine *m) { return gc_gl_start_with(m, NULL); }

int gc_gl_start_with(GcMachine *m, const GcGlOptions *options) {
  GcGlState *gl;
  const char *mode_text = options && options->shaders ? options->shaders : getenv("GC_GL_SHADERS");
  const char *cache = options && options->cache_dir ? options->cache_dir : getenv("GC_GL_CACHE");
  const char *id = options && options->id ? options->id : getenv("GC_GL_ID");
  const char *version;
  GcGlShaderMode mode;
  GcGlRenderSetup setup;
  int major = 0, minor = 0;

  if (!m) return -1;
  if (m->gl) return 0;
  if (getenv("GC_GL_CACHE") && !strcmp(getenv("GC_GL_CACHE"), "off")) cache = NULL;
  if (cache && !strcmp(cache, "off")) cache = NULL;
  mode = parse_mode(mode_text, m);
  gc_gl_fast = (options && options->fast) || (getenv("GC_GL_FAST") && strcmp(getenv("GC_GL_FAST"), "0") != 0);
  gc_gl_card_filter = gc_gl_fast && !(getenv("GC_GL_CARD_FILTER") && !strcmp(getenv("GC_GL_CARD_FILTER"), "0"));
  gc_gl_anisotropy = 1;
  if (gc_gl_card_filter) {
    const int asked = (options && options->anisotropy) ? options->anisotropy
                                                        : (getenv("GC_GL_ANISOTROPY") ? atoi(getenv("GC_GL_ANISOTROPY")) : 1);
    if (asked > 1) gc_gl_anisotropy = asked > GC_GL_ANISOTROPY_MAX ? GC_GL_ANISOTROPY_MAX : asked;
  }
  gc_gl_scale = 1;
  {
    const int asked = (options && options->scale) ? options->scale
                                                   : (getenv("GC_GL_SCALE") ? atoi(getenv("GC_GL_SCALE")) : 1);
    if (asked > 1 && !gc_gl_fast) {
      if (m->log) fprintf(m->log, "gl: a scale of %d is the fast path's; the exact path draws at the console's size\n", asked);
    } else if (asked > 1) {
      gc_gl_scale = asked > GC_GL_SCALE_MAX ? GC_GL_SCALE_MAX : asked;
    }
  }

  gl = (GcGlState *)calloc(1, sizeof(*gl));
  if (!gl) return -1;
  gl->plan = (GcGxTevPlan *)calloc(1, sizeof(GcGxTevPlan));
  if (!gl->plan) {
    release(gl);
    return -1;
  }

  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    if (m->log) fprintf(m->log, "gl: SDL would not start: %s\n", SDL_GetError());
    release(gl);
    return -1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  /* Hidden: what is drawn goes into a framebuffer object and then into
   * main memory, and nothing is presented. */
  gl->window = SDL_CreateWindow("gc", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64, 64,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (!gl->window) {
    if (m->log) fprintf(m->log, "gl: no window: %s\n", SDL_GetError());
    release(gl);
    return -1;
  }
  gl->context = SDL_GL_CreateContext(gl->window);
  if (!gl->context) {
    /* Doubles in shaders (4.0) and program binaries (4.1) are the floor. */
    if (m->log) fprintf(m->log, "gl: no OpenGL 4.1 core context: %s\n", SDL_GetError());
    release(gl);
    return -1;
  }
  if (gc_gl_load_functions(m->log) != 0) {
    release(gl);
    return -1;
  }
  version = (const char *)glGetString(GL_VERSION);
  if (!version || sscanf(version, "%d.%d", &major, &minor) != 2 || major * 10 + minor < 41) {
    if (m->log) fprintf(m->log, "gl: OpenGL %s is older than 4.1\n", version ? version : "(unknown)");
    release(gl);
    return -1;
  }
  /* The other threads' contexts, made here where the windows live. */
  if (mode == GC_GL_SHADERS_HYBRID) {
    shared_context(gl->window, gl->context, "gc-compile", &gl->compile_window, &gl->compile_context);
  }
  if (!getenv("GC_GL_SYNC_COPIES") && gc_memory_alias(m->mem1)) {
    shared_context(gl->window, gl->context, "gc-readback", &gl->readback_window, &gl->readback_context);
  }

  memset(&setup, 0, sizeof(setup));
  setup.m = m;
  setup.window = gl->window;
  setup.context = gl->context;
  setup.compile_window = gl->compile_window;
  setup.compile_context = gl->compile_context;
  setup.readback_window = gl->readback_window;
  setup.readback_context = gl->readback_context;
  setup.mode = mode;
  setup.cache_dir = cache;
  setup.id = id;
  setup.boxes = getenv("GC_GL_BOXES") != NULL;
  gl->render = gc_gl_render_start(&setup);
  if (!gl->render) {
    release(gl);
    return -1;
  }

  /* Peeks answered a frame late: the fast path's way, where there is a
   * readback thread to bring a frame's picture back without anybody
   * waiting for it. GC_GL_LATE_PEEKS=0 keeps every peek exact. */
  gl->late_peeks = gc_gl_fast && gc_gl_render_async_copies(gl->render) &&
                   !(getenv("GC_GL_LATE_PEEKS") && !strcmp(getenv("GC_GL_LATE_PEEKS"), "0"));

  gl->capacity = gc_gl_render_capacity(gl->render);
  gl->records = (uint32_t *)malloc(sizeof(uint32_t) * 4u * GC_GL_RECORD * gl->capacity);
  gl->tris = (GcGlTri *)malloc(sizeof(GcGlTri) * gl->capacity);
  if (!gl->records || !gl->tris) {
    gc_gl_render_stop(gl->render, NULL);
    release(gl);
    return -1;
  }
  gl->no_early_z = getenv("GC_GL_NO_EARLY_Z") != NULL;
  gl->copy_lag = getenv("GC_GL_COPY_LAG") ? (unsigned)strtoul(getenv("GC_GL_COPY_LAG"), NULL, 10) : 2u;

  gl->memory = m->mem1;
  gl->memory_for_copies = gc_memory_alias(m->mem1) ? gc_memory_alias(m->mem1) : m->mem1;
  m->gl = gl;
  gc_cpu_forget_fast(&m->cpu); /* see gc_bus_fast_build */
  gc_gx_thread_gl_changed(m);  /* the graphics thread draws with it from here */
  gl->card_is_stale = 1;
  gl->field = (uint64_t)-1;
  if (m->log) {
    fprintf(m->log, "gl: ready: %s, %s, shaders %s, program cache %s, copies %s\n", gc_gl_render_name(gl->render),
            gc_gl_fast ? "fast path" : "exact",
            mode == GC_GL_SHADERS_UBER ? "uber" : (mode == GC_GL_SHADERS_SPECIALISED ? "specialised" : "hybrid"),
            cache ? cache : "off", gc_gl_render_async_copies(gl->render) ? "on the readback thread" : "at once");
  }
  return 0;
}

void gc_gl_stop(GcMachine *m) {
  GcMachine *const machine = m;
  GcGlState *gl;
  m = gc_gx_thread_target(m);
  gl = m ? state_of(m) : NULL;
  if (!gl) return;
  flush(m);
  gc_gl_render_settle(gl->render);
  /* The reference may draw next (a renderer switched off between fields):
   * the framebuffer it finds in memory is the card's. */
  gc_gl_sync_to_memory(m);
  if (m->log) {
    fprintf(m->log,
            "gl: stats drawing triangles=%llu batches=%llu draws_sent=%llu cpu_triangles=%llu uploads=%llu "
            "downloads=%llu carries=%llu carry_unchecked=%llu suspect_reads=%llu texture_hits=%llu "
            "texture_misses=%llu revalidations=%llu texture_hash_ms=%.1f texture_decode_ms=%.1f flush_ms=%.1f "
            "begin_ms=%.1f carry_ms=%.1f syncs=%llu sync_ms=%.1f key_ms=%.1f gl_ms=%.1f worst_field_gl_ms=%.1f "
            "worst_gl_field=%llu worst_field_wall_ms=%.1f worst_wall_field=%llu fields_over_34ms=%llu "
            "fields_over_50ms=%llu\n",
            (unsigned long long)gl->triangles, (unsigned long long)gl->batches, (unsigned long long)gl->draws_sent,
            (unsigned long long)gl->cpu_triangles, (unsigned long long)gl->uploads,
            (unsigned long long)gl->downloads, (unsigned long long)gl->carries,
            (unsigned long long)gl->carry_unchecked, (unsigned long long)gl->suspect_reads,
            (unsigned long long)gl->texture_hits, (unsigned long long)gl->texture_misses,
            (unsigned long long)gl->revalidations, gl->texture_hash_ms, gl->texture_decode_ms, gl->flush_ms,
            gl->begin_ms, gl->carry_ms, (unsigned long long)gl->syncs, gl->sync_ms, gl->key_ms, gl->total_ms,
            gl->worst_field_ms, (unsigned long long)gl->worst_field, gl->worst_wall_ms,
            (unsigned long long)gl->worst_wall_field, (unsigned long long)gl->fields_over_34ms,
            (unsigned long long)gl->fields_over_50ms);
    fprintf(m->log,
            "gl: stats batching capacity=%u piece_runs=%llu dst_blend_batches=%llu dst_alpha_batches=%llu "
            "box_pixels=%llu copies_async=%llu copies_sync=%llu copy_ms=%.1f waits=%llu wait_ms=%.1f peeks=%llu "
            "pixel_peeks=%llu copy_textures=%llu late_peeks=%llu late_peeks_refused=%llu\n",
            gl->capacity, (unsigned long long)gl->piece_runs, (unsigned long long)gl->dst_blend,
            (unsigned long long)gl->dst_alpha, (unsigned long long)gl->box_pixels,
            (unsigned long long)gl->copies_async, (unsigned long long)gl->copies_sync, gl->copy_ms,
            (unsigned long long)gc_memory_waits(gl->memory), gc_memory_wait_ms(gl->memory),
            (unsigned long long)gl->peeks, (unsigned long long)gl->pixel_peeks,
            (unsigned long long)gl->copy_textures, (unsigned long long)gl->late_peeks_answered,
            (unsigned long long)gl->late_peeks_refused);
  }
  gc_gl_render_stop(gl->render, m->log);
  gl->render = NULL;
  /* The textures the card made of copies went with it. */
  gc_gx_thread_copy_textures_gone(m);
  m->gl = NULL;
  machine->gl = NULL;
  gc_cpu_forget_fast(&machine->cpu);
  release(gl);
}

#endif /* GC_HAVE_SDL2 */
