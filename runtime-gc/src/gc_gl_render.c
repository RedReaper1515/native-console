/*
 * The OpenGL path's render thread: every OpenGL call after start-up, in the
 * order gc_gl.c asks for them (gc_gl_internal.h, "The render thread").
 *
 * What runs here: the pipeline's state applied and its program chosen, the
 * texture slots filled, batches drawn (cut into layers when their pixels
 * read the framebuffer), the framebuffer sent up or brought back, the EFB
 * copies read from the card and handed to the readback thread, and clears.
 * Nothing here reads the machine: a command carries everything it needs,
 * except a download, whose planes the machine's thread lends while it
 * waits.
 */
#include "gc.h"

#ifdef GC_HAVE_SDL2

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "gc_efb.h"
#include "gc_gl_cover.h"
#include "gc_gl_internal.h"
#include "gc_gx_thread.h"
#include "gc_gx_workers.h"
#include "gc_memory.h"
#include "gc_perf.h"

#define GC_GL_DEFINE(ret, name, args) ret(*gl##name) args;
GC_GL_FUNCTIONS(GC_GL_DEFINE)
#undef GC_GL_DEFINE

int gc_gl_load_functions(FILE *log) {
  const char *missing = NULL;
#define GC_GL_LOAD(ret, name, args)                          \
  gl##name = (ret(*) args)SDL_GL_GetProcAddress("gl" #name); \
  if (!gl##name && !missing) missing = "gl" #name;
  GC_GL_FUNCTIONS(GC_GL_LOAD)
#undef GC_GL_LOAD
  if (missing) {
    if (log) fprintf(log, "gl: the driver has no %s\n", missing);
    return -1;
  }
  return 0;
}

/* Units: 0-7 the eight maps, 8 the destination copy, 9 the triangle
 * buffer, 10 a scratch unit every upload binds on so that it never
 * disturbs the other ten. */
#define UNIT_DESTINATION 8
#define UNIT_TRIANGLES 9
#define UNIT_SCRATCH 10

/* How many copies made into textures are kept waiting for a draw to take
 * them, and how many numbers the program that makes one is given. */
#define GC_GL_COPY_TEXTURES 32u
#define COPY_UNIFORM_COUNT 14u
/* How many display copies' pictures are kept for late peeks. A peek asks
 * for the one before the newest issued. */
#define GC_GL_SNAPSHOTS 4u

/* Triangles a draw, at most: the buffer texture holds 25 texels a triangle
 * and is only guaranteed 65,536 texels, so the real limit is asked of the
 * driver at start. */
#define GC_GL_BATCH_MIN 2048u
#define GC_GL_BATCH_MAX 32768u

/* How far ahead of the render thread the machine may get, in bytes of
 * commands, before it waits. */
#define GC_GL_QUEUE_BYTES ((size_t)512 << 20)

/* A command: this header, then its payload. */
typedef struct GcGlBlock {
  struct GcGlBlock *next;
  uint32_t kind;
  unsigned size_class;
  size_t size;
} GcGlBlock;
#define BLOCK_HEADER ((size_t)32)
#define SIZE_CLASSES 40u

struct GcGlReadback;

/* The picture a display copy was made from, as large as it was drawn.
 *
 * At a scale the display buffer in main memory holds the picture brought
 * down to the console's size, because that is what the console's video
 * interface scans out and what a game may read back. A window can show
 * more than that, so each display copy also leaves the pixels it was made
 * from here, under the place in main memory it wrote; whoever presents asks
 * for the picture of the buffer on screen (gc_gl_render_picture) and gets
 * the larger one when the copy that filled that buffer was made here.
 *
 * A few, because a game shows one buffer while it draws into another. */
#define GC_GL_PICTURES 3u
typedef struct GcGlPicture {
  uint32_t destination, display_pitch, out_width, rows; /* the copy: where it wrote, and how much */
  unsigned width, height;                                /* pixels held */
  uint32_t *pixels;
  size_t capacity;
  uint64_t serial; /* 0: empty; larger is later */
} GcGlPicture;

struct GcGlRender {
  GcMachine *m;
  FILE *log;
  SDL_Window *window;
  SDL_GLContext context;
  SDL_Window *compile_window, *readback_window;
  SDL_GLContext compile_context, readback_context;
  GcGlShaderMode mode;
  char cache_dir[1024], id[256], name[512];
  int boxes;

  SDL_Thread *thread;
  SDL_mutex *lock;
  SDL_cond *wake, *done;
  GcGlBlock *head, *tail; /* sent, not yet run */
  GcGlBlock *filling;     /* the command the machine's thread is writing */
  GcGlBlock *free_blocks[SIZE_CLASSES];
  size_t queued_bytes;
  uint64_t sent_sequence, done_sequence;
  int started, start_result;
  int sleeping; /* the render thread is waiting on wake */

  /* What the card holds. */
  GLuint framebuffer, color_texture, depth_texture, destination_texture;
  /* The fast path at a scale (gc_gl_scale > 1) draws into a framebuffer
   * that many times the console's own size, and the machine still reads and
   * writes one of the console's size: this one, which rows are brought down
   * into before anything reads them (bring_down) and which an upload is
   * spread out from. At a scale of 1 it is the drawing framebuffer itself.
   * `half` is the step between for a scale of 4. */
  unsigned scale;
  GLuint own_framebuffer, own_color, own_depth;
  GLuint half_framebuffer, half_color;
  /* The fast path's samplers, two a map: [map][0] magnifies and [map][1]
   * minifies (apply_fast_samplers), and what each was last set to. */
  GLuint samplers[8][2];
  uint32_t sampler_set[8][2];
  SDL_mutex *picture_lock;
  GcGlPicture pictures[GC_GL_PICTURES];
  uint64_t picture_serial, pictures_kept, pictures_given;
  int pictures_off; /* GC_GL_PICTURES=0: keep none, to measure what keeping them costs */
  /* glTextureBarrier, when the driver has it (ARB_texture_barrier, core in
   * 4.5, or NV_texture_barrier, which NVIDIA exposes far below that). With
   * it the framebuffer's own texture is what a destination read samples and
   * the barrier is all that stands between the layers; without it each
   * layer copies the framebuffer first. */
  void (*texture_barrier)(void);
  uint64_t barriers;
  /* GC_GL_GPU_TIMERS: timestamps round a ring, and the nanoseconds they
   * attribute to each kind of work. */
  int gpu_on;
  GLuint *gpu_query;
  uint8_t *gpu_kind;
  uint64_t gpu_written, gpu_read, gpu_markers;
  uint64_t gpu_previous_ns;
  int gpu_have_previous;
  unsigned gpu_current;
  uint64_t gpu_ns[5];
  GLuint array, triangle_buffer, triangle_texture, uniform_buffer;
  GLuint textures[GC_GL_TEXTURES];
  GcGlPrograms *programs;
  struct GcGlReadback *readback;
  float *depth_transfer;
  unsigned capacity;

  /* The draw being run: its triangles and records, in its command. */
  const GcGlTri *tris;
  const uint32_t *records;
  /* Layers: the layer of each triangle, the rows each covers, a map of the
   * framebuffer, and the records of one layer gathered for its draw. */
  uint8_t *layer_of;
  int32_t *spans;
  size_t spans_size;
  size_t *span_at;
  uint32_t *layer_map;
  uint32_t layer_generation;
  uint32_t *sorted;

  /* What it did, and what it cost. */
  uint64_t commands, draws, dst_copies, uploads, downloads, textures_made, layers, layer_draws, layer_rows;
  uint64_t pixel_downloads, brought_down;
  uint64_t copies_sent, displays_sent, copy_limit_waits; /* gc_gl_render_limit_copies, on the sending thread */
  double copy_limit_ms;
  uint64_t copies, states;
  unsigned most_layers;
  double busy_ms, layer_ms, layer_map_ms, upload_ms, download_ms, draw_ms, state_ms, texture_ms, copy_ms;
  double machine_wait_ms; /* the machine's thread, waiting here */
  uint64_t machine_waits, full_waits;

  /* Copies kept as textures (make_copy_texture): the program that draws
   * one, the framebuffer it draws into, and the ones made and not yet
   * taken by a slot, oldest overwritten first. */
  GLuint copy_program, copy_framebuffer;
  unsigned copy_scale; /* the internal resolution, or 1 for console-sized ones (GC_GL_COPY_SCALE=1) */
  GLint copy_uniform[COPY_UNIFORM_COUNT];
  struct {
    uint64_t tag;
    GLuint texture;
  } copy_textures[GC_GL_COPY_TEXTURES];
  unsigned copy_texture_next;
  uint64_t copy_textures_made, copy_textures_taken, copy_textures_unused, copy_textures_lost;
  double copy_texture_ms;

  /* The framebuffer as it was at the last few display copies that asked
   * for it, for peeks answered late (gc_gl_render_snapshot_pixel): each
   * plane as the machine keeps it, colour as its ARGB words and depth as
   * its 24 bits. Written by the readback thread, read by the machine's. */
  struct {
    uint64_t tag[2];
    uint32_t *plane[2];
  } snapshots[GC_GL_SNAPSHOTS];
  uint64_t snapshot_newest[2];
  SDL_mutex *snapshot_lock;
  SDL_cond *snapshot_landed;
  uint64_t snapshots_taken, snapshot_waits;
  double snapshot_ms, snapshot_wait_ms;
};

/* ------------------------------------------------------------------ */
/* The framebuffer between main memory and the card                     */

/* Colour goes as BGRA with the bytes reversed, which is this machine's
 * ARGB word. Depth goes as a float of the 24-bit value over 2^24 into a
 * 32-bit float depth buffer: every 24-bit integer is exactly a float, so
 * both directions are exact and the card's depth test compares exactly
 * the integers the reference compares. */
/* Rows of the drawing framebuffer brought down into the console-sized one,
 * which is then the framebuffer bound for reading. Nothing to do at a scale
 * of 1, where the two are one.
 *
 * Colour is averaged: a scale of 2 is the mean of the four pixels drawn for
 * each of the console's, and 4 is that twice over, so a picture that
 * reaches the screen through a copy comes out supersampled. A scale of 3
 * takes the middle pixel of the nine (one linear blit cannot weigh nine),
 * which is a sharper picture and not a smoother one. Depth takes one pixel
 * of the block, never a mean: a depth between two surfaces is neither. */
static void bring_down(GcGlRender *gl, unsigned first_row, unsigned rows, int depth) {
  const GLint s = (GLint)gl->scale;
  const GLint top = (GLint)first_row, bottom = (GLint)(first_row + rows);
  if (gl->scale == 1u) {
    glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
    return;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->framebuffer);
  if (depth) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl->own_framebuffer);
    glBlitFramebuffer(0, top * s, GC_EFB_WIDTH * s, bottom * s, 0, top, GC_EFB_WIDTH, bottom, GL_DEPTH_BUFFER_BIT,
                      GL_NEAREST);
  } else if (gl->scale == 4u) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl->half_framebuffer);
    glBlitFramebuffer(0, top * 4, GC_EFB_WIDTH * 4, bottom * 4, 0, top * 2, GC_EFB_WIDTH * 2, bottom * 2,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->half_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl->own_framebuffer);
    glBlitFramebuffer(0, top * 2, GC_EFB_WIDTH * 2, bottom * 2, 0, top, GC_EFB_WIDTH, bottom, GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);
  } else {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl->own_framebuffer);
    glBlitFramebuffer(0, top * s, GC_EFB_WIDTH * s, bottom * s, 0, top, GC_EFB_WIDTH, bottom, GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, gl->own_framebuffer);
  gl->brought_down++;
}

static void run_upload(GcGlRender *gl, const uint32_t *color, const uint32_t *depth) {
  const double started = gc_gl_now_ms();
  size_t i;
  /* Writing a texture while it is attached is a feedback loop the driver
   * may ignore. */
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glActiveTexture(GL_TEXTURE0 + UNIT_SCRATCH);
  glBindTexture(GL_TEXTURE_2D, gl->own_color);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GC_EFB_WIDTH, GC_EFB_HEIGHT, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, color);
  for (i = 0; i < (size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT; i++) {
    gl->depth_transfer[i] = (float)(depth[i] & 0x00ffffffu) / 16777216.0f;
  }
  glBindTexture(GL_TEXTURE_2D, gl->own_depth);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GC_EFB_WIDTH, GC_EFB_HEIGHT, GL_DEPTH_COMPONENT, GL_FLOAT,
                  gl->depth_transfer);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (gl->scale > 1u) {
    /* ... and spread out over the drawing framebuffer, each pixel as a
     * block of itself. */
    const GLint s = (GLint)gl->scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->own_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl->framebuffer);
    glBlitFramebuffer(0, 0, GC_EFB_WIDTH, GC_EFB_HEIGHT, 0, 0, GC_EFB_WIDTH * s, GC_EFB_HEIGHT * s,
                      GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
  gl->uploads++;
  gl->upload_ms += gc_gl_now_ms() - started;
}

static void run_download(GcGlRender *gl, uint32_t *color, uint32_t *depth) {
  const double started = gc_gl_now_ms();
  size_t i;
  bring_down(gl, 0, GC_EFB_HEIGHT, 0);
  bring_down(gl, 0, GC_EFB_HEIGHT, 1);
  glReadPixels(0, 0, GC_EFB_WIDTH, GC_EFB_HEIGHT, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, color);
  glReadPixels(0, 0, GC_EFB_WIDTH, GC_EFB_HEIGHT, GL_DEPTH_COMPONENT, GL_FLOAT, gl->depth_transfer);
  for (i = 0; i < (size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT; i++) {
    depth[i] = (uint32_t)(gl->depth_transfer[i] * 16777216.0f);
  }
  gl->downloads++;
  gl->download_ms += gc_gl_now_ms() - started;
}

/* ------------------------------------------------------------------ */
/* Textures                                                            */

/* A slot's texture: every level as eight-bit unsigned integers, which the
 * shader reads with texelFetch and filters itself (gc_gl.c decodes them).
 * A one-texel texture with a second level puts both texels side by side
 * in level 0: OpenGL has no level 1 of a one-texel texture. */
static void run_texture(GcGlRender *gl, const GcGlTextureCommand *c) {
  const double started = gc_gl_now_ms();
  const uint8_t *texels = (const uint8_t *)(c + 1);
  GLuint *const handle = &gl->textures[c->slot % GC_GL_TEXTURES];
  unsigned level;
  size_t at = 0;
  glActiveTexture(GL_TEXTURE0 + UNIT_SCRATCH);
  if (*handle) glDeleteTextures(1, handle);
  glGenTextures(1, handle);
  glBindTexture(GL_TEXTURE_2D, *handle);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  /* Whole numbers for the exact path's shader to fetch and filter itself;
   * fractions on the fast path, which a card will filter. The same bytes. */
  const GLint internal = gc_gl_card_filter ? GL_RGBA8 : GL_RGBA8UI;
  const GLenum layout = gc_gl_card_filter ? GL_RGBA : GL_RGBA_INTEGER;
  if (c->side_by_side) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, 2, 1, 0, layout, GL_UNSIGNED_BYTE, texels);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)c->top);
    for (level = 0; level <= c->top; level++) {
      const unsigned w = c->level_width[level], h = c->level_height[level];
      glTexImage2D(GL_TEXTURE_2D, (GLint)level, internal, (GLsizei)w, (GLsizei)h, 0, layout,
                   GL_UNSIGNED_BYTE, texels + at);
      at += (size_t)w * h * 4u;
    }
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  gl->textures_made++;
  gl->texture_ms += gc_gl_now_ms() - started;
}

/* ------------------------------------------------------------------ */
/* State                                                               */

/* Depth, and the channel enables where the card can say them exactly. */
static void apply_state(uint32_t z_mode, uint32_t mode, uint32_t needs_dst) {
  static const GLenum COMPARE[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                                    GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
  /* A depth write with the test off still writes: that is the card's
   * "always", not its disabled test, which writes nothing. */
  if ((z_mode & 1u) || ((z_mode >> 4) & 1u)) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc((z_mode & 1u) ? COMPARE[(z_mode >> 1) & 7u] : GL_ALWAYS);
  } else {
    glDisable(GL_DEPTH_TEST);
  }
  glDepthMask(((z_mode >> 4) & 1u) ? GL_TRUE : GL_FALSE);
  if (needs_dst) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  } else {
    const GLboolean colour = ((mode >> 3) & 1u) ? GL_TRUE : GL_FALSE;
    glColorMask(colour, colour, colour, GL_TRUE);
  }
}

/* The fast path's pixel write: the card's own blender, set up so that what
 * it computes is the reference's (s * f + d * g) >> 8 wherever it can be.
 *
 * The program does the half the card would get wrong (gc_gl_shader.c, the
 * last block of main): it hands over the source's term already multiplied,
 * so the source factor here is "one" unless it names the destination; and
 * it hands the destination's factor over as a second output, already over
 * 256, so the destination factor here only says whether to read that
 * output or one minus it. A pixel format with no alpha keeps 255 there
 * whatever the blend says, the destination alpha register's constant is
 * written as it is, and the two channel enables are the colour mask.
 * Subtraction and the logic operations are whole numbers either way. */
static void apply_fast_state(const GcGlStateCommand *c) {
  static const GLenum SOURCE[8] = {GL_ONE, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                   GL_ONE, GL_ONE, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
  static const GLenum DESTINATION[8] = {GL_ZERO, GL_SRC1_COLOR, GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR,
                                        GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR, GL_DST_ALPHA,
                                        GL_ONE_MINUS_DST_ALPHA};
  const uint32_t mode = c->blend_mode;
  const unsigned format = c->draw.pixel_engine[2] & 7u;
  const int constant_alpha = (c->draw.pixel_engine[1] >> 8) & 1u;
  /* Whether the alpha channel is the blend's to work out. */
  const int blends_alpha = format == 1u && !constant_alpha;
  const GLboolean colour = ((mode >> 3) & 1u) ? GL_TRUE : GL_FALSE;
  const GLboolean alpha = (format != 1u || ((mode >> 4) & 1u)) ? GL_TRUE : GL_FALSE;

  glColorMask(colour, colour, colour, alpha);
  if ((mode >> 11) & 1u) {
    glDisable(GL_COLOR_LOGIC_OP);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, blends_alpha ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, blends_alpha ? GL_ONE : GL_ZERO);
  } else if (mode & 1u) {
    const GLenum source = SOURCE[(mode >> 8) & 7u], destination = DESTINATION[(mode >> 5) & 7u];
    glDisable(GL_COLOR_LOGIC_OP);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(source, destination, blends_alpha ? source : GL_ONE, blends_alpha ? destination : GL_ZERO);
  } else if ((mode >> 1) & 1u) {
    glDisable(GL_BLEND);
    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(GL_CLEAR + ((mode >> 12) & 15u));
  } else {
    glDisable(GL_BLEND);
    glDisable(GL_COLOR_LOGIC_OP);
  }
}

/* The fast path's two samplers for a map, from the map's own registers as
 * the state carries them (tex_mode: wrap s, wrap t, magnify linear, minify
 * linear; tex_lod: mip mode).
 *
 * The first only ever magnifies and the second only ever minifies, whatever
 * level the program asks either for (gc_gl_shader.c, sample_with_lod): each
 * has the one filter in both of the card's places. Both step to the level
 * asked for; the second blends two levels when the map's mip mode says so.
 * With anisotropic filtering the second is also given the map's lowest and
 * highest level of detail, in sixteenths as the registers have them: there
 * the card chooses the level, and the program's own clamp is not in its way.
 * Set only when a map's registers have changed since it was last used. */
static void apply_fast_samplers(GcGlRender *gl, const GcGlStateCommand *c, unsigned map) {
  static const GLint WRAP[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE};
  const int32_t *mode = &c->draw.tex_mode[map * 4u];
  const int mip = c->draw.tex_lod[map * 4u];
  const int clamps = gc_gl_anisotropy > 1;
  const int32_t lowest = clamps ? c->draw.tex_lod[map * 4u + 1u] : 0;
  const int32_t highest = clamps ? c->draw.tex_lod[map * 4u + 2u] : 0;
  unsigned which;
  for (which = 0; which < 2u; which++) {
    const int linear = mode[2u + which] != 0;
    const int blends = which == 1u && mip != 0 && mip != 1;
    const uint32_t wanted = 0x80000000u | ((uint32_t)(mode[0] & 3) << 0) | ((uint32_t)(mode[1] & 3) << 2) |
                            ((uint32_t)linear << 4) | ((uint32_t)blends << 5) |
                            (which == 1u ? ((uint32_t)(lowest & 0xff) << 6) | ((uint32_t)(highest & 0xff) << 14) : 0u);
    const GLuint sampler = gl->samplers[map][which];
    if (gl->sampler_set[map][which] == wanted) continue;
    gl->sampler_set[map][which] = wanted;
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, WRAP[mode[0] & 3]);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, WRAP[mode[1] & 3]);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                        blends ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
                               : (linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST));
    if (which == 1u && clamps) {
      glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, (GLfloat)lowest * 0.0625f);
      glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, (GLfloat)highest * 0.0625f);
    }
  }
}

static void run_state(GcGlRender *gl, const GcGlStateCommand *c) {
  const double started = gc_gl_now_ms();
  GLuint program;
  int is_uber = 0;
  unsigned map;
  glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
  apply_state(c->z_mode, c->blend_mode, c->needs_dst);
  if (gc_gl_fast) apply_fast_state(c);
  for (map = 0; map < 8u; map++) {
    if (!(c->maps & (1u << map))) continue;
    glActiveTexture(GL_TEXTURE0 + map);
    glBindTexture(GL_TEXTURE_2D, gl->textures[c->slot[map] % GC_GL_TEXTURES]);
    if (gc_gl_card_filter) {
      glActiveTexture(GL_TEXTURE0 + GC_GL_UNIT_MINIFY + map);
      glBindTexture(GL_TEXTURE_2D, gl->textures[c->slot[map] % GC_GL_TEXTURES]);
      apply_fast_samplers(gl, c, map);
    }
  }
  program = gc_gl_programs_get(gl->programs, c->shape, c->field, &is_uber);
  glUseProgram(program);
  glBindBuffer(GL_UNIFORM_BUFFER, gl->uniform_buffer);
  glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)sizeof(c->draw), &c->draw, GL_DYNAMIC_DRAW);
  gl->states++;
  gl->state_ms += gc_gl_now_ms() - started;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */

static void draw_records(GcGlRender *gl, const uint32_t *records, unsigned count) {
  glBindBuffer(GL_TEXTURE_BUFFER, gl->triangle_buffer);
  glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * 4u * GC_GL_RECORD * count), records,
               GL_STREAM_DRAW);
  glBindVertexArray(gl->array);
  glDrawArraysInstanced(GL_TRIANGLES, 0, (gl->boxes && !gc_gl_fast) ? 6 : 3, (GLsizei)count);
  gl->draws++;
}

static void copy_destination(GcGlRender *gl, const GcGlBox *box) {
  glActiveTexture(GL_TEXTURE0 + UNIT_DESTINATION);
  glBindTexture(GL_TEXTURE_2D, gl->destination_texture);
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, box->left, box->top, box->left, box->top, box->right - box->left + 1,
                      box->bottom - box->top + 1);
  gl->dst_copies++;
}

/* What a layer needs before it draws: everything the layers before it wrote,
 * readable at UNIT_DESTINATION.
 *
 * The copy is the plain way to do that, and it is expensive - a rectangle of
 * the framebuffer, once a layer, and a scene of overlapping blended foliage
 * runs to hundreds of layers a field. A texture barrier does the same job by
 * saying the writes are finished instead of moving them: the unit samples the
 * framebuffer's own colour texture, and the barrier between layers makes the
 * previous layer's writes visible to the next layer's fetches.
 *
 * That is exactly the case ARB_texture_barrier is specified for. Within one
 * layer no two triangles share a pixel (the layering's whole point), and the
 * shader only ever fetches the texel the fragment is writing
 * (texelFetch(destination, ivec2(px, py))), so no fragment reads a texel
 * another fragment of the same draw writes. Between layers the barrier
 * orders them.
 *
 * GC_GL_TEXTURE_BARRIER=0 forces the copy, for comparing the two. */
static void prepare_destination(GcGlRender *gl, const GcGlBox *box) {
  if (gl->texture_barrier) {
    gl->texture_barrier();
    gl->barriers++;
    return;
  }
  copy_destination(gl, box);
}

/* A batch whose pixels read the colour already there.
 *
 * One draw cannot say "this pixel reads what an earlier triangle of the same
 * draw wrote", so the batch is cut into layers: a triangle's layer is one
 * more than the highest layer of any earlier triangle that covers a pixel it
 * covers (0 when there is none), found with a map of the framebuffer that
 * holds, for every pixel, one more than the highest layer covering it so far.
 * Layers are drawn in order, each after a copy of the framebuffer as the
 * layers before it left it, over the pixels the layer can touch. That is
 * exactly what each pixel reads when triangles are drawn one at a time:
 *
 *  - every triangle covering a pixel before triangle t is in a lower layer,
 *    so it is drawn, and in the copy, before t's layer is;
 *  - every triangle covering it after t is in a higher layer, so it is not;
 *  - within a layer no two triangles share a pixel, so the order of the
 *    draw does not matter; and
 *  - a triangle covering none of a pixel's history leaves it alone.
 *
 * Coverage is the reference's own (covered_run), so triangles that only
 * share an edge - the usual case in a mesh - share a layer. The map counts
 * to 255; a batch that deep is drawn in pieces, each layered on its own. */
/* Every row's covered run of triangles [first, end), each triangle's rows
 * at spans[span_at[i] ...], worked out on any thread: a triangle writes only
 * its own rows. */
typedef struct GcGlSpanJob {
  GcGlRender *gl;
  unsigned first, end;
  long next;
} GcGlSpanJob;

#define GC_GL_SPAN_RUN 8
static void span_rows(void *argument, unsigned id) {
  GcGlSpanJob *const job = (GcGlSpanJob *)argument;
  GcGlRender *const gl = job->gl;
  const long count = (long)(job->end - job->first);
  long run;
  (void)id;
  while ((run = gc_workers_take(&job->next)) * GC_GL_SPAN_RUN < count) {
    long k;
    for (k = run * GC_GL_SPAN_RUN; k < (run + 1) * GC_GL_SPAN_RUN && k < count; k++) {
      const unsigned i = job->first + (unsigned)k;
      const GcGlTri *const t = &gl->tris[i];
      int32_t *row = gl->spans + gl->span_at[i];
      int py, hint_lo = NO_HINT, hint_hi = NO_HINT;
      for (py = t->box.top; py <= t->box.bottom; py++, row += 2) {
        int lo, hi;
        covered_run(t, py, &lo, &hi, hint_lo, hint_hi);
        row[0] = lo;
        row[1] = hi;
        if (lo <= hi) {
          hint_lo = lo;
          hint_hi = hi;
        }
      }
    }
  }
}

/* Whether two triangles cover a pixel in common, by their rows. A run from
 * covered_run may include uncovered pixels between its ends only for a
 * triangle too large to search, so this can say "yes" too often and never
 * "no" too often. */
static int rows_meet(const GcGlRender *gl, unsigned i, unsigned j) {
  const GcGlTri *const a = &gl->tris[i], *const b = &gl->tris[j];
  const int top = a->box.top > b->box.top ? a->box.top : b->box.top;
  const int bottom = a->box.bottom < b->box.bottom ? a->box.bottom : b->box.bottom;
  const int32_t *ra, *rb;
  int py;
  if (a->box.right < b->box.left || b->box.right < a->box.left || top > bottom) return 0;
  ra = gl->spans + gl->span_at[i] + 2u * (unsigned)(top - a->box.top);
  rb = gl->spans + gl->span_at[j] + 2u * (unsigned)(top - b->box.top);
  for (py = top; py <= bottom; py++, ra += 2, rb += 2) {
    if (ra[0] <= ra[1] && rb[0] <= rb[1] && ra[0] <= rb[1] && rb[0] <= ra[1]) return 1;
  }
  return 0;
}

static void flush_layers(GcGlRender *gl, unsigned first, unsigned end) {
  unsigned i, layers = 0, layer;
  const double map_started = gc_gl_now_ms();
  size_t rows = 0;

  /* The rows of every triangle, once. */
  for (i = first; i < end; i++) {
    gl->span_at[i] = rows;
    rows += 2u * (size_t)(gl->tris[i].box.bottom - gl->tris[i].box.top + 1);
  }
  if (rows > gl->spans_size) {
    int32_t *grown = (int32_t *)realloc(gl->spans, rows * sizeof(int32_t));
    if (!grown) {
      /* No room to layer: one triangle a draw is always right. */
      for (i = first; i < end; i++) {
        prepare_destination(gl, &gl->tris[i].box);
        draw_records(gl, gl->records + 4u * GC_GL_RECORD * i, 1);
      }
      return;
    }
    gl->spans = grown;
    gl->spans_size = rows;
  }
  {
    GcGlSpanJob job;
    job.gl = gl;
    job.first = first;
    job.end = end;
    job.next = 0;
    if (end - first < 4u || rows < 512u || !gc_workers_count() || gc_workers_run(span_rows, &job) != 0) {
      span_rows(&job, 0);
    }
  }
  gl->layer_rows += rows / 2u;

  if (end - first <= 384u) {
    /* A few hundred triangles: each against the earlier ones whose layer
     * could raise its own, by their rows. */
    for (i = first; i < end; i++) {
      unsigned highest = 0, k;
      for (k = i; k-- > first;) {
        if (gl->layer_of[k] + 1u <= highest) continue;
        if (rows_meet(gl, k, i)) highest = gl->layer_of[k] + 1u;
      }
      if (highest >= 255u) {
        flush_layers(gl, first, i);
        flush_layers(gl, i, end);
        return;
      }
      gl->layer_of[i] = (uint8_t)highest;
      if (highest + 1u > layers) layers = highest + 1u;
    }
  } else {
    /* More: a map of the framebuffer, each pixel holding the batch's
     * generation above one more than the highest layer covering it. */
    uint32_t generation;
    if (++gl->layer_generation >= 0xffffffu) {
      memset(gl->layer_map, 0, sizeof(uint32_t) * GC_EFB_WIDTH * GC_EFB_HEIGHT);
      gl->layer_generation = 1;
    }
    generation = gl->layer_generation << 8;
    for (i = first; i < end; i++) {
      const GcGlTri *t = &gl->tris[i];
      const int32_t *spans = gl->spans + gl->span_at[i];
      unsigned highest = 0;
      int py, px;
      for (py = t->box.top; py <= t->box.bottom; py++) {
        const uint32_t *row = gl->layer_map + (size_t)py * GC_EFB_WIDTH;
        const int lo = spans[2 * (py - t->box.top)], hi = spans[2 * (py - t->box.top) + 1];
        for (px = lo; px <= hi; px++) {
          if ((row[px] & ~0xffu) == generation && (row[px] & 0xffu) > highest) highest = row[px] & 0xffu;
        }
      }
      if (highest >= 255u) {
        flush_layers(gl, first, i);
        flush_layers(gl, i, end);
        return;
      }
      for (py = t->box.top; py <= t->box.bottom; py++) {
        uint32_t *row = gl->layer_map + (size_t)py * GC_EFB_WIDTH;
        const int lo = spans[2 * (py - t->box.top)], hi = spans[2 * (py - t->box.top) + 1];
        for (px = lo; px <= hi; px++) row[px] = generation | (highest + 1u);
      }
      gl->layer_of[i] = (uint8_t)highest;
      if (highest + 1u > layers) layers = highest + 1u;
    }
  }

  gl->layer_map_ms += gc_gl_now_ms() - map_started;
  if (layers == 1u) {
    GcGlBox box = gl->tris[first].box;
    for (i = first + 1u; i < end; i++) {
      const GcGlTri *t = &gl->tris[i];
      if (t->box.left < box.left) box.left = t->box.left;
      if (t->box.top < box.top) box.top = t->box.top;
      if (t->box.right > box.right) box.right = t->box.right;
      if (t->box.bottom > box.bottom) box.bottom = t->box.bottom;
    }
    prepare_destination(gl, &box);
    draw_records(gl, gl->records + 4u * GC_GL_RECORD * first, end - first);
  }
  for (layer = 0; layers > 1u && layer < layers; layer++) {
    unsigned n = 0;
    GcGlBox box = {0, 0, 0, 0};
    for (i = first; i < end; i++) {
      const GcGlTri *t = &gl->tris[i];
      if (gl->layer_of[i] != layer) continue;
      if (!n) {
        box = t->box;
      } else {
        if (t->box.left < box.left) box.left = t->box.left;
        if (t->box.top < box.top) box.top = t->box.top;
        if (t->box.right > box.right) box.right = t->box.right;
        if (t->box.bottom > box.bottom) box.bottom = t->box.bottom;
      }
      memcpy(gl->sorted + 4u * GC_GL_RECORD * n, gl->records + 4u * GC_GL_RECORD * i,
             sizeof(uint32_t) * 4u * GC_GL_RECORD);
      n++;
    }
    if (!n) continue;
    prepare_destination(gl, &box);
    draw_records(gl, gl->sorted, n);
  }
  if (layers > gl->most_layers) gl->most_layers = layers;
  gl->layers += layers;
}

/* ------------------------------------------------------------------ */
/* Where the card's time goes                                          */

/* Every measurement of the OpenGL path so far has been taken on this side
 * of the fence: how long a thread waited, which says where it blocked and
 * not what the card was doing. Two optimisations were argued for on that
 * basis and neither was worth anything - the copies a layered blend makes
 * turned out to be under one percent, and merging draws that differ only by
 * texture would reach 5% of the batches. This is the instrument that would
 * have said so first.
 *
 * It is timestamps rather than elapsed-time queries because those cannot
 * nest or resume, and the work here interleaves: a marker is dropped
 * whenever the kind of work changes, and the gap between two markers
 * belongs to the kind that was running before the later one. The queries go
 * round a ring and are read back thousands of markers later, so nothing
 * waits on the card to answer.
 *
 * Off unless GC_GL_GPU_TIMERS is set: a timestamp is cheap but not free,
 * and this is an instrument, not a feature. */
enum { GPU_OTHER, GPU_DRAW, GPU_LAYERED, GPU_COPY, GPU_TEXTURE, GPU_KINDS };
#define GPU_RING 4096u

static const char *const GPU_KIND_NAME[GPU_KINDS] = {"other", "draws", "layered draws", "efb copies",
                                                     "texture uploads"};

static void gpu_timer_start(GcGlRender *gl) {
  const char *setting = getenv("GC_GL_GPU_TIMERS");
  if (!setting || strtol(setting, NULL, 10) == 0) return;
  gl->gpu_query = (GLuint *)calloc(GPU_RING, sizeof(GLuint));
  gl->gpu_kind = (uint8_t *)calloc(GPU_RING, 1u);
  if (!gl->gpu_query || !gl->gpu_kind) return;
  glGenQueries((GLsizei)GPU_RING, gl->gpu_query);
  gl->gpu_on = 1;
  gl->gpu_current = GPU_OTHER;
}

/* One marker off the ring, attributed to the kind that ended at it. */
static void gpu_harvest_one(GcGlRender *gl) {
  const unsigned slot = (unsigned)(gl->gpu_read % GPU_RING);
  uint64_t ns = 0;
  glGetQueryObjectui64v(gl->gpu_query[slot], GL_QUERY_RESULT, &ns);
  if (gl->gpu_have_previous) gl->gpu_ns[gl->gpu_kind[slot]] += ns - gl->gpu_previous_ns;
  gl->gpu_previous_ns = ns;
  gl->gpu_have_previous = 1;
  gl->gpu_read++;
}

/* The work changes kind: close the span that was running. */
static void gpu_mark(GcGlRender *gl, unsigned kind) {
  unsigned slot;
  if (!gl->gpu_on || kind == gl->gpu_current) return;
  /* The ring is full when the oldest marker has not been read yet. It is
   * thousands of markers old by then, so reading it does not wait. */
  if (gl->gpu_written - gl->gpu_read >= GPU_RING) gpu_harvest_one(gl);
  slot = (unsigned)(gl->gpu_written % GPU_RING);
  gl->gpu_kind[slot] = (uint8_t)gl->gpu_current;
  glQueryCounter(gl->gpu_query[slot], GL_TIMESTAMP);
  gl->gpu_written++;
  gl->gpu_markers++;
  gl->gpu_current = kind;
}

static void gpu_timer_drain(GcGlRender *gl) {
  if (!gl->gpu_on) return;
  gpu_mark(gl, GPU_OTHER);
  while (gl->gpu_read < gl->gpu_written) gpu_harvest_one(gl);
}

static void gpu_timer_report(GcGlRender *gl, FILE *log) {
  unsigned k;
  double total = 0.0;
  if (!gl->gpu_on || !log) return;
  gpu_timer_drain(gl);
  for (k = 0; k < GPU_KINDS; k++) total += (double)gl->gpu_ns[k] / 1e6;
  fprintf(log, "gl: gpu %llu markers, %.1f ms on the card", (unsigned long long)gl->gpu_markers, total);
  for (k = 0; k < GPU_KINDS; k++) {
    if (!gl->gpu_ns[k]) continue;
    fprintf(log, "; %s %.1f ms (%.0f%%)", GPU_KIND_NAME[k], (double)gl->gpu_ns[k] / 1e6,
            total > 0.0 ? 100.0 * ((double)gl->gpu_ns[k] / 1e6) / total : 0.0);
  }
  fprintf(log, "\n");
}

/* ------------------------------------------------------------------ */
/* Copies                                                              */

/* An EFB copy leaves this thread as a read of its rows into a pixel buffer
 * and a fence at the point where they are complete, flushed at once. The
 * copy itself - filter, gamma, luma and chroma, the tile encoding, all
 * gc_efb.c's own code - runs on the readback thread, whose context shares
 * objects with this one: it waits for the fence, maps the buffer, and
 * writes main memory through the second view (gc_memory.h), then releases
 * the pages the machine's thread held when it asked for the copy. An
 * access to one of them waits for this, and then sees what an immediate
 * copy would have left.
 *
 * What was tried and dropped (docs/GC-PLAN.md): a staging texture read back
 * from the second context, by glGetTexImage or through a framebuffer of its
 * own (both read one copy behind, whatever the fences said); mapping or
 * fetching the buffer in this context (about 14 ms a copy: the driver waits
 * for its queue); persistent mappings (stale). */
#define GC_GL_READBACK_QUEUE 64u

typedef struct GcGlPendingCopy {
  GcEfbCopyJob *job;
  GLuint buffer;
  GLsync fence;
  unsigned first_row, rows; /* the framebuffer rows in the buffer */
  int depth;                /* depth words rather than colour */
  uint32_t held_first, held_end;
  uint64_t tag; /* the hold on them */
  double asked_ms, read_ms; /* when the machine asked, and when this thread read it */
  /* A display copy at a scale: the larger pixels it was made from, read
   * into a buffer of their own (GcGlPicture). */
  GLuint picture_buffer;
  unsigned picture_width, picture_height;
  uint32_t picture_destination, picture_pitch, picture_out_width, picture_rows;
  /* The whole framebuffer, a plane to a buffer, for late peeks. */
  GLuint snapshot_buffer[2];
} GcGlPendingCopy;

typedef struct GcGlReadback {
  GcMachine *m;
  SDL_Window *window;
  SDL_GLContext context;
  SDL_Thread *thread;
  SDL_mutex *lock;
  SDL_cond *wake, *done;
  GcGlPendingCopy queue[GC_GL_READBACK_QUEUE];
  unsigned head, tail, pending;
  GLuint free_buffers[GC_GL_READBACK_QUEUE];
  unsigned free_count;
  GLuint free_pictures[GC_GL_READBACK_QUEUE];
  unsigned free_picture_count;
  GLuint free_snapshots[GC_GL_READBACK_QUEUE * 2u];
  unsigned free_snapshot_count;
  GcGlRender *render;
  int quit;
  uint32_t *color, *depth; /* the planes the thread assembles */
  uint8_t *alias;
  double thread_ms;
  double to_read_ms, to_card_ms, to_memory_ms, worst_ms; /* latency, summed from asking */
  uint64_t finished, displays_finished;
  double map_ms, fence_ms;
} GcGlReadback;

#define GC_GL_PLANE_BYTES ((GLsizeiptr)GC_EFB_WIDTH * GC_EFB_HEIGHT * 4)

/* The larger picture of a display copy, kept under the copy's place in
 * main memory: over the one kept there before, or else over the oldest. */
static void keep_picture(GcGlRender *gl, const GcGlPendingCopy *c, const uint32_t *pixels) {
  const size_t count = (size_t)c->picture_width * c->picture_height;
  GcGlPicture *at = NULL;
  unsigned i;
  SDL_LockMutex(gl->picture_lock);
  for (i = 0; i < GC_GL_PICTURES; i++) {
    GcGlPicture *const p = &gl->pictures[i];
    if (p->serial && p->destination == c->picture_destination) {
      at = p;
      break;
    }
    if (!at || p->serial < at->serial) at = p;
  }
  if (at->capacity < count) {
    uint32_t *grown = (uint32_t *)realloc(at->pixels, count * sizeof(uint32_t));
    if (!grown) {
      at->serial = 0;
      SDL_UnlockMutex(gl->picture_lock);
      return;
    }
    at->pixels = grown;
    at->capacity = count;
  }
  memcpy(at->pixels, pixels, count * sizeof(uint32_t));
  at->destination = c->picture_destination;
  at->display_pitch = c->picture_pitch;
  at->out_width = c->picture_out_width;
  at->rows = c->picture_rows;
  at->width = c->picture_width;
  at->height = c->picture_height;
  at->serial = ++gl->picture_serial;
  gl->pictures_kept++;
  SDL_UnlockMutex(gl->picture_lock);
}

int gc_gl_render_picture(GcGlRender *gl, uint32_t display_first, unsigned display_width, unsigned display_height,
                         uint32_t *out, unsigned pitch, unsigned rows, unsigned *width_out, unsigned *height_out,
                         uint64_t *serial_out) {
  const GcGlPicture *found = NULL;
  unsigned i, width, height, y;
  int result = -1;
  SDL_LockMutex(gl->picture_lock);
  for (i = 0; i < GC_GL_PICTURES; i++) {
    const GcGlPicture *const p = &gl->pictures[i];
    /* The buffer on screen starts where the copy wrote, or a line in for a
     * frame shown bottom field first. */
    if (!p->serial || display_first < p->destination) continue;
    if (display_first - p->destination > p->display_pitch) continue;
    if (!found || p->serial > found->serial) found = p;
  }
  if (found && found->out_width && found->rows && display_width && display_height) {
    /* As much of the picture as the video interface shows of the copy. */
    const unsigned shown_width = display_width < found->out_width ? display_width : found->out_width;
    const unsigned shown_rows = display_height < found->rows ? display_height : found->rows;
    width = (unsigned)((uint64_t)found->width * shown_width / found->out_width);
    height = (unsigned)((uint64_t)found->height * shown_rows / found->rows);
    if (width && height) {
      if (width_out) *width_out = width;
      if (height_out) *height_out = height;
      if (serial_out) *serial_out = found->serial;
      if (!out) {
        result = 0;
      } else if (width <= pitch && height <= rows) {
        for (y = 0; y < height; y++) {
          const uint32_t *from = found->pixels + (size_t)y * found->width;
          uint32_t *to = out + (size_t)y * pitch;
          unsigned x;
          for (x = 0; x < width; x++) to[x] = from[x] | 0xff000000u;
        }
        gl->pictures_given++;
        result = 0;
      }
    }
  }
  SDL_UnlockMutex(gl->picture_lock);
  return result;
}

void gc_gl_render_forget_pictures(GcGlRender *gl) {
  unsigned i;
  SDL_LockMutex(gl->picture_lock);
  for (i = 0; i < GC_GL_PICTURES; i++) gl->pictures[i].serial = 0;
  SDL_UnlockMutex(gl->picture_lock);
}

/* A plane of the framebuffer as a display copy found it, kept under the
 * copy's tag: over the oldest picture kept. */
static void keep_snapshot(GcGlRender *gl, uint64_t tag, unsigned plane, const uint8_t *bytes) {
  const size_t count = (size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT;
  unsigned i, at = 0;
  uint32_t *into;
  SDL_LockMutex(gl->snapshot_lock);
  for (i = 0; i < GC_GL_SNAPSHOTS; i++) {
    if (gl->snapshots[i].tag[plane] < gl->snapshots[at].tag[plane]) at = i;
  }
  /* Not readable while it is being written. */
  gl->snapshots[at].tag[plane] = 0;
  into = gl->snapshots[at].plane[plane];
  if (!into) into = gl->snapshots[at].plane[plane] = (uint32_t *)malloc(count * sizeof(uint32_t));
  SDL_UnlockMutex(gl->snapshot_lock);
  if (into) {
    if (plane) {
      const float *from = (const float *)(const void *)bytes;
      size_t k;
      for (k = 0; k < count; k++) into[k] = (uint32_t)(from[k] * 16777216.0f);
    } else {
      memcpy(into, bytes, count * sizeof(uint32_t));
    }
  }
  SDL_LockMutex(gl->snapshot_lock);
  if (into) gl->snapshots[at].tag[plane] = tag;
  if (tag > gl->snapshot_newest[plane]) gl->snapshot_newest[plane] = tag;
  gl->snapshots_taken++;
  SDL_CondBroadcast(gl->snapshot_landed);
  SDL_UnlockMutex(gl->snapshot_lock);
}

int gc_gl_render_snapshot_pixel(GcGlRender *gl, uint64_t tag, int plane, uint32_t x, uint32_t y, uint32_t *value) {
  const double started = gc_gl_now_ms();
  int found = 0, waited = 0;
  unsigned i;
  plane &= 1;
  if (!gl || !gl->snapshot_lock) return 0;
  SDL_LockMutex(gl->snapshot_lock);
  for (;;) {
    for (i = 0; i < GC_GL_SNAPSHOTS && !found; i++) {
      if (gl->snapshots[i].tag[plane] == tag) {
        *value = gl->snapshots[i].plane[plane][(size_t)y * GC_EFB_WIDTH + x];
        found = 1;
      }
    }
    /* Newer pictures have landed and this one is not among those kept: it
     * never will be. Or it is taking longer than anything should. */
    if (found || gl->snapshot_newest[plane] > tag || gc_gl_now_ms() - started > 500.0) break;
    waited = 1;
    SDL_CondWaitTimeout(gl->snapshot_landed, gl->snapshot_lock, 50);
  }
  if (waited) {
    gl->snapshot_waits++;
    gl->snapshot_wait_ms += gc_gl_now_ms() - started;
    /* The machine's thread, waiting for the render thread: the field log's. */
    gc_perf.gl_wait_ms += gc_gl_now_ms() - started;
  }
  SDL_UnlockMutex(gl->snapshot_lock);
  return found;
}

static void run_pending(GcGlReadback *rb, GcGlPendingCopy *c) {
  const double started = gc_gl_now_ms();
  {
    /* Before the copy's own bytes: a peek may be waiting for this. */
    unsigned plane;
    for (plane = 0; plane < 2u; plane++) {
      const uint8_t *bytes;
      if (!c->snapshot_buffer[plane]) continue;
      glBindBuffer(GL_PIXEL_PACK_BUFFER, c->snapshot_buffer[plane]);
      bytes = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GC_GL_PLANE_BYTES, GL_MAP_READ_BIT);
      if (bytes) {
        keep_snapshot(rb->render, c->tag, plane, bytes);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      }
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
  }
  if (c->picture_buffer) {
    /* Before the copy's own bytes land: by the time the buffer on screen
     * reads as finished, its picture is here. */
    const uint8_t *bytes;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, c->picture_buffer);
    bytes = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                              (GLsizeiptr)c->picture_width * c->picture_height * 4, GL_MAP_READ_BIT);
    if (bytes) {
      keep_picture(rb->render, c, (const uint32_t *)(const void *)bytes);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }
  if (c->rows) {
    const uint8_t *bytes;
    const double map_started = gc_gl_now_ms();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, c->buffer);
    bytes = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)GC_EFB_WIDTH * c->rows * 4,
                                              GL_MAP_READ_BIT);
    rb->map_ms += gc_gl_now_ms() - map_started;
    if (bytes) {
      unsigned row;
      for (row = 0; row < c->rows; row++) {
        const size_t at = (size_t)(c->first_row + row) * GC_EFB_WIDTH;
        if (c->depth) {
          const float *from = (const float *)(bytes + (size_t)row * GC_EFB_WIDTH * 4u);
          unsigned x;
          for (x = 0; x < GC_EFB_WIDTH; x++) rb->depth[at + x] = (uint32_t)(from[x] * 16777216.0f);
        } else {
          memcpy(rb->color + at, bytes + (size_t)row * GC_EFB_WIDTH * 4u, (size_t)GC_EFB_WIDTH * 4u);
        }
      }
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }
  c->job->color = rb->color;
  c->job->depth = rb->depth;
  c->job->ram = rb->alias;
  if (c->held_end - c->held_first > 8192u && gc_efb_copy_rows_independent(c->job)) {
    /* The first and last pages the copy writes are usually shared with
     * something else the machine is using - a heap block before a display
     * buffer, one after it - which then waits for the whole copy. So the
     * rows that reach those two pages go first and the two pages are
     * released at once; the pages between, which only the copy writes,
     * follow. Rows are independent here, so the order changes nothing. */
    const uint32_t first_page = c->held_first & ~4095u;
    const uint32_t last_page = (c->held_end - 1u) & ~4095u;
    uint32_t y, lo, hi;
    for (y = 0; y < c->job->out_rows; y++) {
      gc_efb_copy_row_bytes(c->job, y, &lo, &hi);
      if (lo < first_page + 4096u || hi > last_page) gc_efb_copy_run_rows(c->job, y, y + 1u);
    }
    /* A graphics thread that is owed these bytes takes them now, before
     * the machine can write the pages again (gc_gx_thread.h). */
    gc_gx_thread_copy_landing(c->tag, rb->alias, c->held_first, first_page + 4096u);
    gc_gx_thread_copy_landing(c->tag, rb->alias, last_page, c->held_end);
    gc_memory_release_tagged(rb->m->mem1, first_page, 1u, c->tag, 0);
    gc_memory_release_tagged(rb->m->mem1, last_page, 1u, c->tag, last_page <= first_page + 4096u);
    for (y = 0; y < c->job->out_rows; y++) {
      gc_efb_copy_row_bytes(c->job, y, &lo, &hi);
      if (!(lo < first_page + 4096u || hi > last_page)) gc_efb_copy_run_rows(c->job, y, y + 1u);
    }
    if (last_page > first_page + 4096u) {
      gc_gx_thread_copy_landing(c->tag, rb->alias, first_page + 4096u, last_page);
      gc_memory_release_tagged(rb->m->mem1, first_page + 4096u, last_page - first_page - 4096u, c->tag, 1);
    }
  } else {
    gc_efb_copy_run(c->job, 0);
    gc_gx_thread_copy_landing(c->tag, rb->alias, c->held_first, c->held_end);
    gc_memory_release_tagged(rb->m->mem1, c->held_first, c->held_end - c->held_first, c->tag, 1);
  }
  gc_gx_thread_copy_landed(c->tag);
  {
    const double latency = gc_gl_now_ms() - c->asked_ms;
    rb->to_memory_ms += latency;
    rb->to_read_ms += c->read_ms - c->asked_ms;
    if (latency > rb->worst_ms) rb->worst_ms = latency;
    rb->finished++;
    if (c->job->to_display) rb->displays_finished++;
  }
  free(c->job);
  c->job = NULL;
  rb->thread_ms += gc_gl_now_ms() - started;
}

static int readback_main(void *argument) {
  GcGlReadback *rb = (GcGlReadback *)argument;
  if (SDL_GL_MakeCurrent(rb->window, rb->context) != 0) return 1;
  for (;;) {
    GcGlPendingCopy *c;
    SDL_LockMutex(rb->lock);
    while (rb->head == rb->tail && !rb->quit) SDL_CondWait(rb->wake, rb->lock);
    if (rb->head == rb->tail) {
      SDL_UnlockMutex(rb->lock);
      break;
    }
    c = &rb->queue[rb->head];
    SDL_UnlockMutex(rb->lock);

    if (c->fence) {
      const double fence_started = gc_gl_now_ms();
      glClientWaitSync(c->fence, 0, GL_TIMEOUT_IGNORED);
      rb->fence_ms += gc_gl_now_ms() - fence_started;
      rb->to_card_ms += gc_gl_now_ms() - c->asked_ms;
      glDeleteSync(c->fence);
      c->fence = NULL;
    }
    run_pending(rb, c);

    SDL_LockMutex(rb->lock);
    if (c->buffer) rb->free_buffers[rb->free_count++] = c->buffer;
    if (c->picture_buffer) rb->free_pictures[rb->free_picture_count++] = c->picture_buffer;
    if (c->snapshot_buffer[0]) rb->free_snapshots[rb->free_snapshot_count++] = c->snapshot_buffer[0];
    if (c->snapshot_buffer[1]) rb->free_snapshots[rb->free_snapshot_count++] = c->snapshot_buffer[1];
    rb->head = (rb->head + 1u) % GC_GL_READBACK_QUEUE;
    rb->pending--;
    SDL_CondBroadcast(rb->done);
    SDL_UnlockMutex(rb->lock);
  }
  SDL_GL_MakeCurrent(rb->window, NULL);
  return 0;
}

static GcGlReadback *readback_start(GcGlRender *gl, GcMachine *m, SDL_Window *window, SDL_GLContext context) {
  GcGlReadback *rb;
  if (!window || !context || !gc_memory_alias(m->mem1)) return NULL;
  rb = (GcGlReadback *)calloc(1, sizeof(GcGlReadback));
  if (!rb) return NULL;
  rb->m = m;
  rb->render = gl;
  rb->window = window;
  rb->context = context;
  rb->alias = gc_memory_alias(m->mem1);
  rb->color = (uint32_t *)calloc((size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT, sizeof(uint32_t));
  rb->depth = (uint32_t *)calloc((size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT, sizeof(uint32_t));
  rb->lock = SDL_CreateMutex();
  rb->wake = SDL_CreateCond();
  rb->done = SDL_CreateCond();
  if (rb->color && rb->depth && rb->lock && rb->wake && rb->done) {
    rb->thread = SDL_CreateThread(readback_main, "gc-gl-readback", rb);
  }
  if (!rb->thread) {
    if (m->log) fprintf(m->log, "gl: no readback thread (%s); copies wait for the card\n", SDL_GetError());
    if (rb->done) SDL_DestroyCond(rb->done);
    if (rb->wake) SDL_DestroyCond(rb->wake);
    if (rb->lock) SDL_DestroyMutex(rb->lock);
    free(rb->color);
    free(rb->depth);
    free(rb);
    return NULL;
  }
  return rb;
}

/* Wait for every copy the thread has been handed. */
static void readback_drain(GcGlReadback *rb) {
  if (!rb) return;
  SDL_LockMutex(rb->lock);
  while (rb->pending) SDL_CondWait(rb->done, rb->lock);
  SDL_UnlockMutex(rb->lock);
}

static void readback_stop(GcGlReadback *rb) {
  unsigned i;
  if (!rb) return;
  readback_drain(rb);
  if (rb->m->log && rb->finished) {
    fprintf(rb->m->log,
            "gl: stats readback copies=%llu thread_ms=%.1f mean_ms_to_render_thread=%.2f mean_ms_to_card_done=%.2f "
            "mean_ms_to_memory=%.2f worst_ms_to_memory=%.1f fence_ms=%.1f map_ms=%.1f\n",
            (unsigned long long)rb->finished, rb->thread_ms, rb->to_read_ms / (double)rb->finished,
            rb->to_card_ms / (double)rb->finished, rb->to_memory_ms / (double)rb->finished, rb->worst_ms, rb->fence_ms, rb->map_ms);
  }
  SDL_LockMutex(rb->lock);
  rb->quit = 1;
  SDL_CondSignal(rb->wake);
  SDL_UnlockMutex(rb->lock);
  SDL_WaitThread(rb->thread, NULL);
  for (i = 0; i < rb->free_count; i++) glDeleteBuffers(1, &rb->free_buffers[i]);
  for (i = 0; i < rb->free_picture_count; i++) glDeleteBuffers(1, &rb->free_pictures[i]);
  for (i = 0; i < rb->free_snapshot_count; i++) glDeleteBuffers(1, &rb->free_snapshots[i]);
  SDL_DestroyCond(rb->done);
  SDL_DestroyCond(rb->wake);
  SDL_DestroyMutex(rb->lock);
  free(rb->color);
  free(rb->depth);
  free(rb);
}

/* ------------------------------------------------------------------ */
/* A copy kept as a texture                                            */

/* The fast path's other way out of the framebuffer. An EFB copy to a
 * texture is how a game makes a texture of what it has just drawn - a
 * bloom's source, a depth map, a shadow - and it nearly always draws with
 * it at once. The bytes are still read back (above) because main memory has
 * to hold them; but a draw that waits for them waits for the whole pipeline
 * (docs/GC-PLAN.md, "Copies kept on the card"). So when the copy is read
 * off the framebuffer here it is also drawn, by the program below, into a
 * texture of its own: the texels a sampler would decode from the bytes the
 * copy is about to store. gc_efb.c's copy_row and store_copy_pixel, and
 * gc_gx_texture.c's decoder after them, as whole numbers:
 *
 *  - each texel from one pixel of the rectangle or the mean of a block of
 *    four (a halved copy), each pixel the row itself and the rows above and
 *    below through the copy filter's three sums, shifted right six, masked
 *    to nine bits and clamped;
 *  - gamma, then the intensity conversion when the copy asks for it;
 *  - narrowed to the copy's format and widened again as the texture
 *    format that reads it back widens: RGB565's five and six bits by
 *    repeating their top bits, RGB5A3 either way round, the four-bit
 *    formats times seventeen.
 *
 * Not the reference's to the bit in one place: gamma is the card's pow()
 * rather than a table of the C library's. A copy of depth reads the depth
 * buffer as the readback does, a float times 2^24.
 *
 * At an internal resolution above 1 the texture is made of the pixels that
 * were drawn, not of the console-sized ones the bytes are made of: `p_scale`
 * times as wide and as tall, each texel through the same sums with the row
 * above and the row below a console's row away. A draw samples a map by
 * fractions of it, so it neither knows nor cares; and what a game draws
 * with a copy of its own picture - the far half of a depth of field, a
 * heat haze, a pause screen's backdrop - is then as fine as the picture it
 * was copied from, where the console-sized texture drew it as blocks of
 * `p_scale` pixels beside geometry that was not. The bytes in main memory
 * are the console's either way.
 *
 * The texture is kept under the copy's tag until a draw's slot takes it
 * (GC_GL_CMD_COPY_TEXTURE) or enough newer copies have been made that
 * nothing will. */
static const char *const COPY_VERTEX =
    "#version 410 core\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *const COPY_FRAGMENT =
    "uniform sampler2D src;\n"
    "uniform int p_left, p_top, p_taps, p_height, p_wp, p_wc, p_wn;\n"
    "uniform int p_convert, p_gamma, p_clamp_top, p_clamp_bottom, p_format, p_depth, p_scale;\n"
    "out vec4 texel;\n"
    "ivec4 source(int x, int y) {\n"
    "  if (p_depth != 0) {\n"
    "    uint z = uint(texelFetch(src, ivec2(x, y), 0).r * 16777216.0) & 0xffffffu;\n"
    "    return ivec4(int((z >> 16) & 255u), int((z >> 8) & 255u), int(z & 255u), 255);\n"
    "  }\n"
    "  return ivec4(texelFetch(src, ivec2(x, y), 0) * 255.0 + 0.5);\n"
    "}\n"
    "int filtered(int previous, int current, int next) {\n"
    "  return min(((previous * p_wp + current * p_wc + next * p_wn) >> 6) & 0x1ff, 255);\n"
    "}\n"
    "int curve(int v) {\n"
    "  if (p_gamma == 0) return v;\n"
    "  return int(pow(float(v) / 255.0, p_gamma == 1 ? 1.0 / 1.7 : 1.0 / 2.2) * 255.0 + 0.5);\n"
    "}\n"
    "int w3(int v) { return ((v << 5) | (v << 2) | (v >> 1)) & 255; }\n"
    "int w4(int v) { return (v << 4) | v; }\n"
    "int w5(int v) { return (v << 3) | (v >> 2); }\n"
    "int w6(int v) { return (v << 2) | (v >> 4); }\n"
    "void main() {\n"
    "  int s = int(gl_FragCoord.x), t = int(gl_FragCoord.y);\n"
    "  ivec4 sum = ivec4(0);\n"
    "  int taps = 0;\n"
    "  int wide = EFB_WIDTH * p_scale, high = EFB_HEIGHT * p_scale;\n"
    "  for (int down = 0; down < p_taps; down++) {\n"
    "    int in_t = t * p_taps + down;\n"
    "    int line = in_t / p_scale;\n"
    "    int row = p_top * p_scale + in_t;\n"
    "    if (row >= high) break;\n"
    "    int above = (line == 0 && p_clamp_top != 0) ? row : (row >= p_scale ? row - p_scale : row);\n"
    "    int below = (line + 1 == p_height && p_clamp_bottom != 0) ? row : row + p_scale;\n"
    "    if (above >= high) above = row;\n"
    "    if (below >= high) below = row;\n"
    "    for (int tap = 0; tap < p_taps; tap++) {\n"
    "      int column = p_left * p_scale + s * p_taps + tap;\n"
    "      if (column >= wide) break;\n"
    "      ivec4 current = source(column, row), previous = source(column, above), next = source(column, below);\n"
    "      ivec3 c = ivec3(curve(filtered(previous.r, current.r, next.r)),\n"
    "                      curve(filtered(previous.g, current.g, next.g)),\n"
    "                      curve(filtered(previous.b, current.b, next.b)));\n"
    "      if (p_convert != 0) {\n"
    "        int y = (66 * c.r + 129 * c.g + 25 * c.b) & 0xffff;\n"
    "        int u = -38 * c.r - 74 * c.g + 112 * c.b;\n"
    "        int v = 112 * c.r - 94 * c.g - 18 * c.b;\n"
    "        c = ivec3(((y >> 8) + ((y >> 7) & 1) + 16) & 255, ((u >> 8) + ((u >> 7) & 1) + 128) & 255,\n"
    "                  ((v >> 8) + ((v >> 7) & 1) + 128) & 255);\n"
    "      }\n"
    "      sum += ivec4(c, current.a);\n"
    "      taps++;\n"
    "    }\n"
    "  }\n"
    "  if (taps == 0) { texel = vec4(0.0); return; }\n"
    "  ivec4 p = sum / taps;\n"
    "  ivec4 o;\n"
    "  int f = p_format;\n"
    "  if (f == 6) o = p;\n"
    "  else if (f == 4) o = ivec4(w5(p.r >> 3), w6(p.g >> 2), w5(p.b >> 3), 255);\n"
    "  else if (f == 5) {\n"
    "    if (p.a >= 0xe0) o = ivec4(w5(p.r >> 3), w5(p.g >> 3), w5(p.b >> 3), 255);\n"
    "    else o = ivec4(w4(p.r >> 4), w4(p.g >> 4), w4(p.b >> 4), w3(p.a >> 5));\n"
    "  }\n"
    "  else if (f == 3) o = ivec4(p.r, p.r, p.r, p.a);\n"
    "  else if (f == 2) o = ivec4(ivec3(w4(p.r >> 4)), w4(p.a >> 4));\n"
    "  else if (f == 1 || f == 8) o = ivec4(p.r);\n"
    "  else if (f == 9) o = ivec4(p.g);\n"
    "  else if (f == 10) o = ivec4(p.b);\n"
    "  else if (f == 7) o = ivec4(p.a);\n"
    "  else if (f == 11) o = ivec4(p.r, p.r, p.r, p.g);\n"
    "  else if (f == 12) o = ivec4(p.g, p.g, p.g, p.b);\n"
    "  else o = ivec4(w4(p.r >> 4));\n"
    "  texel = vec4(o) / 255.0;\n"
    "}\n";

static const char *const COPY_UNIFORMS[COPY_UNIFORM_COUNT] = {
    "p_left", "p_top", "p_taps", "p_height", "p_wp", "p_wc", "p_wn",
    "p_convert", "p_gamma", "p_clamp_top", "p_clamp_bottom", "p_format", "p_depth", "p_scale"};

static GLuint copy_shader(FILE *log, GLenum kind, const char *source) {
  const GLuint shader = glCreateShader(kind);
  GLint ok = 0;
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char text[2048];
    GLsizei length = 0;
    glGetShaderInfoLog(shader, (GLsizei)sizeof(text), &length, text);
    if (log) fprintf(log, "gl: the copy program would not compile: %.*s\n", (int)length, text);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

/* The program and the framebuffer it draws into, once. Without them copies
 * are not kept as textures, and nothing else changes. */
static void copy_textures_init(GcGlRender *gl) {
  char fragment[8192];
  GLuint vertex_shader, fragment_shader;
  GLint ok = 0, at;
  unsigned i;
  const char *off = getenv("GC_GL_COPY_TEXTURES");
  if (!gc_gl_fast || !gc_gl_card_filter || (off && !strcmp(off, "0"))) return;
  snprintf(fragment, sizeof(fragment), "#version 410 core\n#define EFB_WIDTH %d\n#define EFB_HEIGHT %d\n%s",
           (int)GC_EFB_WIDTH, (int)GC_EFB_HEIGHT, COPY_FRAGMENT);
  vertex_shader = copy_shader(gl->log, GL_VERTEX_SHADER, COPY_VERTEX);
  fragment_shader = copy_shader(gl->log, GL_FRAGMENT_SHADER, fragment);
  if (vertex_shader && fragment_shader) {
    gl->copy_program = glCreateProgram();
    glAttachShader(gl->copy_program, vertex_shader);
    glAttachShader(gl->copy_program, fragment_shader);
    glLinkProgram(gl->copy_program);
    glGetProgramiv(gl->copy_program, GL_LINK_STATUS, &ok);
    if (!ok) {
      if (gl->log) fprintf(gl->log, "gl: the copy program would not link\n");
      glDeleteProgram(gl->copy_program);
      gl->copy_program = 0;
    }
  }
  if (vertex_shader) glDeleteShader(vertex_shader);
  if (fragment_shader) glDeleteShader(fragment_shader);
  if (!gl->copy_program) return;
  glUseProgram(gl->copy_program);
  at = glGetUniformLocation(gl->copy_program, "src");
  if (at >= 0) glUniform1i(at, UNIT_SCRATCH);
  for (i = 0; i < COPY_UNIFORM_COUNT; i++) gl->copy_uniform[i] = glGetUniformLocation(gl->copy_program, COPY_UNIFORMS[i]);
  glUseProgram(0);
  glGenFramebuffers(1, &gl->copy_framebuffer);
  {
    const char *small = getenv("GC_GL_COPY_SCALE");
    gl->copy_scale = (small && !strcmp(small, "1")) ? 1u : gl->scale;
  }
  if (gl->log) {
    fprintf(gl->log, "gl: fast path: a copy to a texture is kept on the card as one");
    if (gl->scale > 1u) fprintf(gl->log, gl->copy_scale > 1u ? ", at %ux" : ", console-sized", gl->copy_scale);
    fprintf(gl->log, "\n");
  }
}

int gc_gl_render_copy_textures(const GcGlRender *gl) { return gl && gl->copy_program != 0; }

static void make_copy_texture(GcGlRender *gl, const GcEfbCopyJob *job, uint64_t tag, int depth) {
  const double started = gc_gl_now_ms();
  const unsigned scale = gl->copy_scale;
  int clamp_top = 0, clamp_bottom = 0;
  GLint values[COPY_UNIFORM_COUNT];
  GLuint texture = 0;
  unsigned i, at;
  (void)gc_efb_copy_texture_format(job, &clamp_top, &clamp_bottom);
  values[0] = (GLint)job->left;
  values[1] = (GLint)job->top;
  values[2] = job->half_scale ? 2 : 1;
  values[3] = (GLint)job->height;
  values[4] = (GLint)job->weights.previous;
  values[5] = (GLint)job->weights.current;
  values[6] = (GLint)job->weights.next;
  values[7] = job->convert ? 1 : 0;
  values[8] = (GLint)job->gamma;
  values[9] = clamp_top;
  values[10] = clamp_bottom;
  values[11] = (GLint)job->format;
  values[12] = depth ? 1 : 0;
  values[13] = (GLint)scale;

  glActiveTexture(GL_TEXTURE0 + UNIT_SCRATCH);
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)(job->out_width * scale), (GLsizei)(job->out_rows * scale), 0,
               GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glBindFramebuffer(GL_FRAMEBUFFER, gl->copy_framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  /* The planes that were drawn into; or, for a console-sized texture at a
   * larger scale, the console-sized planes the copy's rows were just
   * brought down into (run_copy). At a scale of 1 they are the same. */
  if (scale == gl->scale) glBindTexture(GL_TEXTURE_2D, depth ? gl->depth_texture : gl->color_texture);
  else glBindTexture(GL_TEXTURE_2D, depth ? gl->own_depth : gl->own_color);
  glViewport(0, 0, (GLsizei)(job->out_width * scale), (GLsizei)(job->out_rows * scale));
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_COLOR_LOGIC_OP);
  for (i = 0; i < 4u; i++) glDisable(GL_CLIP_DISTANCE0 + (GLenum)i);
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glUseProgram(gl->copy_program);
  for (i = 0; i < COPY_UNIFORM_COUNT; i++) {
    if (gl->copy_uniform[i] >= 0) glUniform1i(gl->copy_uniform[i], values[i]);
  }
  glBindVertexArray(gl->array);
  glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 1);
  /* Back to what every draw assumes; the state a draw sets itself (depth,
   * blending, masks, the program) it sets again before it draws. */
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  for (i = 0; i < 4u; i++) glEnable(GL_CLIP_DISTANCE0 + (GLenum)i);
  glViewport(0, 0, (GLsizei)(GC_EFB_WIDTH * gl->scale), (GLsizei)(GC_EFB_HEIGHT * gl->scale));
  glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);

  at = gl->copy_texture_next;
  gl->copy_texture_next = (at + 1u) % GC_GL_COPY_TEXTURES;
  if (gl->copy_textures[at].texture) {
    glDeleteTextures(1, &gl->copy_textures[at].texture);
    gl->copy_textures_unused++;
  }
  gl->copy_textures[at].texture = texture;
  gl->copy_textures[at].tag = tag;
  gl->copy_textures_made++;
  gl->copy_texture_ms += gc_gl_now_ms() - started;
}

/* A slot of the texture cache takes the texture made of the copy `tag`. */
static void run_copy_texture(GcGlRender *gl, const GcGlCopyTextureCommand *c) {
  GLuint *const handle = &gl->textures[c->slot % GC_GL_TEXTURES];
  unsigned i;
  if (*handle) {
    glDeleteTextures(1, handle);
    *handle = 0;
  }
  for (i = 0; i < GC_GL_COPY_TEXTURES; i++) {
    if (gl->copy_textures[i].texture && gl->copy_textures[i].tag == c->tag) {
      *handle = gl->copy_textures[i].texture;
      gl->copy_textures[i].texture = 0;
      gl->copy_textures[i].tag = 0;
      gl->copy_textures_taken++;
      return;
    }
  }
  /* Gone: more copies were made between this one and its draw than are
   * kept. The slot has no texture, and samples as black. */
  gl->copy_textures_lost++;
}

static void run_copy(GcGlRender *gl, const GcGlCopyCommand *command) {
  const double started = gc_gl_now_ms();
  GcGlReadback *const rb = gl->readback;
  GcGlPendingCopy *c;

  SDL_LockMutex(rb->lock);
  while (rb->pending == GC_GL_READBACK_QUEUE - 1u) SDL_CondWait(rb->done, rb->lock);
  c = &rb->queue[rb->tail];
  memset(c, 0, sizeof(*c));
  if (rb->free_count) c->buffer = rb->free_buffers[--rb->free_count];
  SDL_UnlockMutex(rb->lock);

  c->job = command->job;
  c->first_row = command->first_row;
  c->rows = command->rows;
  c->depth = (int)command->depth;
  c->held_first = command->held_first;
  c->held_end = command->held_end;
  c->tag = command->tag;
  c->asked_ms = command->asked_ms;
  c->read_ms = gc_gl_now_ms();
  if (c->rows) {
    if (!c->buffer) {
      glGenBuffers(1, &c->buffer);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, c->buffer);
      glBufferData(GL_PIXEL_PACK_BUFFER, GC_GL_PLANE_BYTES, NULL, GL_STREAM_READ);
    } else {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, c->buffer);
    }
    bring_down(gl, c->first_row, c->rows, c->depth);
    if (c->depth) {
      glReadPixels(0, (GLint)c->first_row, GC_EFB_WIDTH, (GLsizei)c->rows, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    } else {
      glReadPixels(0, (GLint)c->first_row, GC_EFB_WIDTH, (GLsizei)c->rows, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                   NULL);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (command->texture_format >= 0 && gl->copy_program) make_copy_texture(gl, command->job, command->tag, c->depth);
  }
  if (command->job->snapshot) {
    /* The whole framebuffer as this frame leaves it, for peeks answered
     * late: each plane asked for, brought down and read into a buffer of
     * its own. */
    const double snapshot_started = gc_gl_now_ms();
    unsigned plane;
    for (plane = 0; plane < 2u; plane++) {
      if (!((command->job->snapshot >> plane) & 1u)) continue;
      SDL_LockMutex(rb->lock);
      if (rb->free_snapshot_count) c->snapshot_buffer[plane] = rb->free_snapshots[--rb->free_snapshot_count];
      SDL_UnlockMutex(rb->lock);
      if (!c->snapshot_buffer[plane]) {
        glGenBuffers(1, &c->snapshot_buffer[plane]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, c->snapshot_buffer[plane]);
        glBufferData(GL_PIXEL_PACK_BUFFER, GC_GL_PLANE_BYTES, NULL, GL_STREAM_READ);
      } else {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, c->snapshot_buffer[plane]);
      }
      bring_down(gl, 0, GC_EFB_HEIGHT, (int)plane);
      if (plane) {
        glReadPixels(0, 0, GC_EFB_WIDTH, GC_EFB_HEIGHT, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
      } else {
        glReadPixels(0, 0, GC_EFB_WIDTH, GC_EFB_HEIGHT, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
      }
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    gl->snapshot_ms += gc_gl_now_ms() - snapshot_started;
  }
  {
    /* A display copy at a scale leaves the larger pixels it was made from
     * as well (GcGlPicture): the copy's rectangle of the drawing
     * framebuffer, read as it is. */
    const GcEfbCopyJob *const job = command->job;
    if (gl->scale > 1u && !gl->pictures_off && job->to_display && !job->half_scale && !c->depth && job->rows && job->out_width &&
        job->left < GC_EFB_WIDTH && job->top < GC_EFB_HEIGHT) {
      const unsigned s = gl->scale;
      const unsigned width = (job->width < GC_EFB_WIDTH - job->left) ? job->width : GC_EFB_WIDTH - job->left;
      const unsigned height = (job->height < GC_EFB_HEIGHT - job->top) ? job->height : GC_EFB_HEIGHT - job->top;
      if (width && height) {
        SDL_LockMutex(rb->lock);
        if (rb->free_picture_count) c->picture_buffer = rb->free_pictures[--rb->free_picture_count];
        SDL_UnlockMutex(rb->lock);
        if (!c->picture_buffer) {
          glGenBuffers(1, &c->picture_buffer);
          glBindBuffer(GL_PIXEL_PACK_BUFFER, c->picture_buffer);
          glBufferData(GL_PIXEL_PACK_BUFFER, GC_GL_PLANE_BYTES * (GLsizeiptr)(s * s), NULL, GL_STREAM_READ);
        } else {
          glBindBuffer(GL_PIXEL_PACK_BUFFER, c->picture_buffer);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
        glReadPixels((GLint)(job->left * s), (GLint)(job->top * s), (GLsizei)(width * s), (GLsizei)(height * s),
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        c->picture_width = width * s;
        c->picture_height = height * s;
        c->picture_destination = job->destination;
        c->picture_pitch = job->display_pitch;
        c->picture_out_width = job->out_width;
        c->picture_rows = job->rows;
      }
    }
  }
  c->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();

  SDL_LockMutex(rb->lock);
  rb->tail = (rb->tail + 1u) % GC_GL_READBACK_QUEUE;
  rb->pending++;
  SDL_CondSignal(rb->wake);
  SDL_UnlockMutex(rb->lock);
  gl->copies++;
  gl->copy_ms += gc_gl_now_ms() - started;
}

/* The copy's clear, on the card: the colour as the framebuffer stores it
 * (eight-bit channels, k / 255 stored as k) and the depth as n / 2^24 in the
 * float depth buffer, both exact. */
static void run_clear(GcGlRender *gl, const GcGlClearCommand *c) {
  glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glClearColor((float)((c->colour >> 16) & 0xffu) / 255.0f, (float)((c->colour >> 8) & 0xffu) / 255.0f,
               (float)(c->colour & 0xffu) / 255.0f, (float)(c->colour >> 24) / 255.0f);
  glClearDepth((double)(c->depth & 0x00ffffffu) / 16777216.0);
  glEnable(GL_SCISSOR_TEST);
  glScissor((GLint)(c->left * gl->scale), (GLint)(c->top * gl->scale), (GLsizei)(c->across * gl->scale),
            (GLsizei)(c->down * gl->scale));
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
}

static void run_draw(GcGlRender *gl, const GcGlDrawCommand *c) {
  const double started = gc_gl_now_ms();
  if (!c->count) return;
  gl->tris = (const GcGlTri *)(c + 1);
  gl->records = (const uint32_t *)(void *)((const char *)gl->tris + sizeof(GcGlTri) * c->count);
  glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
  gpu_mark(gl, (c->needs_dst && c->count > 1u) ? GPU_LAYERED : GPU_DRAW);
  if (c->needs_dst && c->count > 1u) {
    const double layered = gc_gl_now_ms();
    const uint64_t draws = gl->draws;
    flush_layers(gl, 0, c->count);
    gl->layer_ms += gc_gl_now_ms() - layered;
    gl->layer_draws += gl->draws - draws;
  } else {
    if (c->needs_dst) prepare_destination(gl, &gl->tris[0].box);
    draw_records(gl, gl->records, c->count);
  }
  gl->tris = NULL;
  gl->records = NULL;
  gl->draw_ms += gc_gl_now_ms() - started;
}

/* ------------------------------------------------------------------ */
/* The queue                                                           */

enum { GC_GL_CMD_QUIT = 0 };

static unsigned size_class(size_t bytes) {
  unsigned c = 8u;
  while (((size_t)1 << c) < bytes && c + 1u < SIZE_CLASSES) c++;
  return c;
}

void *gc_gl_render_command(GcGlRender *gl, unsigned kind, size_t size) {
  const size_t total = BLOCK_HEADER + size;
  const unsigned cls = size_class(total);
  const int cached = ((size_t)1 << cls) >= total;
  GcGlBlock *b = NULL;
  if (cached) {
    SDL_LockMutex(gl->lock);
    b = gl->free_blocks[cls];
    if (b) gl->free_blocks[cls] = b->next;
    SDL_UnlockMutex(gl->lock);
  }
  if (!b) {
    b = (GcGlBlock *)malloc(cached ? ((size_t)1 << cls) : total);
    if (!b) {
      if (gl->log) fprintf(gl->log, "gl: out of memory for a command of %zu bytes\n", size);
      abort();
    }
  }
  b->next = NULL;
  b->kind = kind;
  b->size_class = cached ? cls : SIZE_CLASSES;
  b->size = size;
  gl->filling = b;
  return (char *)b + BLOCK_HEADER;
}

void gc_gl_render_send(GcGlRender *gl) {
  GcGlBlock *const b = gl->filling;
  gl->filling = NULL;
  if (!b) return;
  SDL_LockMutex(gl->lock);
  if (gl->queued_bytes > GC_GL_QUEUE_BYTES) {
    const double started = gc_gl_now_ms();
    gl->full_waits++;
    while (gl->queued_bytes > GC_GL_QUEUE_BYTES && gl->head) SDL_CondWait(gl->done, gl->lock);
    gl->machine_wait_ms += gc_gl_now_ms() - started;
    gc_perf.gl_wait_ms += gc_gl_now_ms() - started;
  }
  if (gl->tail) gl->tail->next = b;
  else gl->head = b;
  gl->tail = b;
  gl->queued_bytes += b->size;
  if (gl->sleeping) SDL_CondSignal(gl->wake);
  SDL_UnlockMutex(gl->lock);
}

void gc_gl_render_wait(GcGlRender *gl) {
  const double started = gc_gl_now_ms();
  uint64_t target;
  uint64_t *const sequence = (uint64_t *)gc_gl_render_command(gl, GC_GL_CMD_SYNC, sizeof(uint64_t));
  target = *sequence = ++gl->sent_sequence;
  gc_gl_render_send(gl);
  SDL_LockMutex(gl->lock);
  while (gl->done_sequence < target) SDL_CondWait(gl->done, gl->lock);
  SDL_UnlockMutex(gl->lock);
  gl->machine_waits++;
  gl->machine_wait_ms += gc_gl_now_ms() - started;
  gc_perf.gl_wait_ms += gc_gl_now_ms() - started;
}

void gc_gl_render_settle(GcGlRender *gl) {
  gc_gl_render_wait(gl);
  readback_drain(gl->readback);
}

void gc_gl_render_limit_copies(GcGlRender *gl, unsigned pending, int display) {
  GcGlReadback *const rb = gl->readback;
  double started;
  if (!rb) return;
  gl->copies_sent++;
  /* Counted in frames. A display copy is a frame that somebody is waiting
   * to see; a copy to a texture is part of drawing one, and a game may make
   * five of them a frame - waiting on each kept this thread in step with
   * the readback all frame long. */
  if (!display) return;
  gl->displays_sent++;
  if (gl->displays_sent - rb->displays_finished <= pending) return;
  started = gc_gl_now_ms();
  SDL_LockMutex(rb->lock);
  while (gl->displays_sent - rb->displays_finished > pending) SDL_CondWait(rb->done, rb->lock);
  SDL_UnlockMutex(rb->lock);
  gl->copy_limit_waits++;
  gl->copy_limit_ms += gc_gl_now_ms() - started;
}

int gc_gl_render_async_copies(const GcGlRender *gl) { return gl->readback != NULL; }
unsigned gc_gl_render_capacity(const GcGlRender *gl) { return gl->capacity; }
const char *gc_gl_render_name(const GcGlRender *gl) { return gl->name; }

static void run_command(GcGlRender *gl, GcGlBlock *b) {
  const void *const payload = (char *)b + BLOCK_HEADER;
  switch (b->kind) {
    case GC_GL_CMD_STATE: run_state(gl, (const GcGlStateCommand *)payload); break;
    case GC_GL_CMD_DRAW: run_draw(gl, (const GcGlDrawCommand *)payload); break;
    case GC_GL_CMD_TEXTURE:
      gpu_mark(gl, GPU_TEXTURE);
      run_texture(gl, (const GcGlTextureCommand *)payload);
      break;
    case GC_GL_CMD_UPLOAD: {
      const uint32_t *const planes = (const uint32_t *)payload;
      run_upload(gl, planes, planes + (size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT);
      break;
    }
    case GC_GL_CMD_DOWNLOAD: {
      const GcGlDownloadCommand *const c = (const GcGlDownloadCommand *)payload;
      if (c->pixel) {
        /* One pixel, into its place in the plane: what a peek reads. */
        const size_t at = (size_t)c->y * GC_EFB_WIDTH + c->x;
        const double started = gc_gl_now_ms();
        bring_down(gl, c->y, 1, (int)c->depth_plane);
        if (c->depth_plane) {
          float value = 0.0f;
          glReadPixels((GLint)c->x, (GLint)c->y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &value);
          c->depth[at] = (uint32_t)(value * 16777216.0f);
        } else {
          glReadPixels((GLint)c->x, (GLint)c->y, 1, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, c->color + at);
        }
        gl->pixel_downloads++;
        gl->download_ms += gc_gl_now_ms() - started;
      } else {
        run_download(gl, c->color, c->depth);
      }
      break;
    }
    case GC_GL_CMD_COPY:
      gpu_mark(gl, GPU_COPY);
      run_copy(gl, (const GcGlCopyCommand *)payload);
      break;
    case GC_GL_CMD_CLEAR: run_clear(gl, (const GcGlClearCommand *)payload); break;
    case GC_GL_CMD_COPY_TEXTURE: run_copy_texture(gl, (const GcGlCopyTextureCommand *)payload); break;
    default: break;
  }
}

/* ------------------------------------------------------------------ */
/* Bring-up, and the thread                                            */

/* A framebuffer `scale` times the console's size: colour, and depth when
 * asked for. Returns 0 when it is complete. */
static int make_framebuffer(GLuint *framebuffer, GLuint *color, GLuint *depth, unsigned scale) {
  const GLsizei width = (GLsizei)(GC_EFB_WIDTH * scale), height = (GLsizei)(GC_EFB_HEIGHT * scale);
  int complete;
  glGenFramebuffers(1, framebuffer);
  glActiveTexture(GL_TEXTURE0 + UNIT_SCRATCH);
  glGenTextures(1, color);
  glBindTexture(GL_TEXTURE_2D, *color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (depth) {
    glGenTextures(1, depth);
    glBindTexture(GL_TEXTURE_2D, *depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, *framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *color, 0);
  if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, *depth, 0);
  complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  return complete ? 0 : -1;
}

static int render_init(GcGlRender *gl) {
  GcMachine *const m = gl->m;
  GLint texels = 0;

  glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &texels);
  gl->capacity = texels > 0 ? (unsigned)(texels / GC_GL_RECORD) : GC_GL_BATCH_MIN;
  if (gl->capacity < GC_GL_BATCH_MIN) gl->capacity = GC_GL_BATCH_MIN;
  if (gl->capacity > GC_GL_BATCH_MAX) gl->capacity = GC_GL_BATCH_MAX;
  gl->sorted = (uint32_t *)malloc(sizeof(uint32_t) * 4u * GC_GL_RECORD * gl->capacity);
  gl->layer_of = (uint8_t *)malloc(gl->capacity);
  gl->span_at = (size_t *)malloc(sizeof(size_t) * gl->capacity);
  gl->layer_map = (uint32_t *)calloc((size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT, sizeof(uint32_t));
  gl->depth_transfer = (float *)malloc(sizeof(float) * GC_EFB_WIDTH * GC_EFB_HEIGHT);
  if (!gl->sorted || !gl->layer_of || !gl->span_at || !gl->layer_map || !gl->depth_transfer) return -1;

  gl->scale = (gc_gl_scale >= 1 && gc_gl_scale <= GC_GL_SCALE_MAX) ? (unsigned)gc_gl_scale : 1u;
  gl->pictures_off = getenv("GC_GL_PICTURES") && strtol(getenv("GC_GL_PICTURES"), NULL, 10) == 0;
  {
    GLint largest = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &largest);
    while (gl->scale > 1u && (GLint)(GC_EFB_WIDTH * gl->scale) > largest) gl->scale--;
  }
  if (make_framebuffer(&gl->framebuffer, &gl->color_texture, &gl->depth_texture, gl->scale) != 0) {
    if (m->log) fprintf(m->log, "gl: the framebuffer object is not complete\n");
    return -1;
  }
  gl->own_framebuffer = gl->framebuffer;
  gl->own_color = gl->color_texture;
  gl->own_depth = gl->depth_texture;
  if (gl->scale > 1u) {
    if (make_framebuffer(&gl->own_framebuffer, &gl->own_color, &gl->own_depth, 1u) != 0 ||
        (gl->scale == 4u && make_framebuffer(&gl->half_framebuffer, &gl->half_color, NULL, 2u) != 0)) {
      if (m->log) fprintf(m->log, "gl: a framebuffer for scaling is not complete\n");
      return -1;
    }
    if (m->log) {
      fprintf(m->log, "gl: fast path: drawing at %ux, %u by %u\n", gl->scale, GC_EFB_WIDTH * gl->scale,
              GC_EFB_HEIGHT * gl->scale);
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);

  glActiveTexture(GL_TEXTURE0 + UNIT_DESTINATION);
  glGenTextures(1, &gl->destination_texture);
  glBindTexture(GL_TEXTURE_2D, gl->destination_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GC_EFB_WIDTH, GC_EFB_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  gpu_timer_start(gl);

  {
    /* The barrier, if the driver has one. The unit then stays bound to the
     * framebuffer's own colour texture for the whole run: nothing else binds
     * on it once the copy path is out of the way. */
    const char *setting = getenv("GC_GL_TEXTURE_BARRIER");
    if (!setting || strtol(setting, NULL, 10) != 0) {
      gl->texture_barrier = (void (*)(void))SDL_GL_GetProcAddress("glTextureBarrier");
      if (!gl->texture_barrier) {
        gl->texture_barrier = (void (*)(void))SDL_GL_GetProcAddress("glTextureBarrierNV");
      }
    }
    if (gl->texture_barrier) glBindTexture(GL_TEXTURE_2D, gl->color_texture);
    if (m->log) {
      fprintf(m->log, "gl: destination reads %s\n",
              gl->texture_barrier ? "sample the framebuffer through a texture barrier"
                                  : "copy the framebuffer");
    }
  }

  glGenBuffers(1, &gl->triangle_buffer);
  glBindBuffer(GL_TEXTURE_BUFFER, gl->triangle_buffer);
  glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * 4u * GC_GL_RECORD), gl->sorted, GL_STREAM_DRAW);
  glActiveTexture(GL_TEXTURE0 + UNIT_TRIANGLES);
  glGenTextures(1, &gl->triangle_texture);
  glBindTexture(GL_TEXTURE_BUFFER, gl->triangle_texture);
  glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, gl->triangle_buffer);

  glGenBuffers(1, &gl->uniform_buffer);
  glBindBuffer(GL_UNIFORM_BUFFER, gl->uniform_buffer);
  glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)sizeof(GcGlDraw), NULL, GL_DYNAMIC_DRAW);
  glBindBufferBase(GL_UNIFORM_BUFFER, 0, gl->uniform_buffer);

  glGenVertexArrays(1, &gl->array);
  glBindVertexArray(gl->array);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glViewport(0, 0, (GLsizei)(GC_EFB_WIDTH * gl->scale), (GLsizei)(GC_EFB_HEIGHT * gl->scale));
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glDisable(GL_DITHER);
  glDisable(GL_SCISSOR_TEST);
  if (gc_gl_fast) {
    GLint extensions = 0, i;
    int anisotropic = 0;
    /* The reference clamps a depth outside the buffer's range instead of
     * dropping the pixel; the card is asked to do the same. The four clip
     * distances are the scissor (gc_gl_shader.c). */
    glEnable(GL_DEPTH_CLAMP);
    for (i = 0; i < 4; i++) glEnable(GL_CLIP_DISTANCE0 + (GLenum)i);
    /* The card filters this path's textures: two samplers a map, bound
     * once, set as the maps' registers ask (apply_fast_samplers). */
    for (i = 0; i < 8 && gc_gl_card_filter; i++) {
      glGenSamplers(2, gl->samplers[i]);
      glBindSampler((GLuint)i, gl->samplers[i][0]);
      glBindSampler((GLuint)(GC_GL_UNIT_MINIFY + i), gl->samplers[i][1]);
    }
    gc_gl_fast_early = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    for (i = 0; i < extensions; i++) {
      const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
      if (name && !strcmp(name, "GL_ARB_shader_image_load_store")) gc_gl_fast_early = 1;
      if (name && (!strcmp(name, "GL_EXT_texture_filter_anisotropic") ||
                   !strcmp(name, "GL_ARB_texture_filter_anisotropic"))) {
        anisotropic = 1;
      }
    }
    if (gc_gl_anisotropy > 1 && (!anisotropic || !gc_gl_card_filter)) {
      if (m->log) fprintf(m->log, "gl: fast path: no anisotropic filtering here\n");
      gc_gl_anisotropy = 1;
    }
    if (gc_gl_anisotropy > 1) {
      GLint most = 1;
      glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &most);
      if (gc_gl_anisotropy > most) gc_gl_anisotropy = most > 1 ? most : 1;
      /* On the samplers that minify; it is the program that decides which
       * pixels are filtered this way (gc_gl_shader.c, sample_bound). */
      for (i = 0; i < 8; i++) glSamplerParameteri(gl->samplers[i][1], GL_TEXTURE_MAX_ANISOTROPY, gc_gl_anisotropy);
      if (m->log) fprintf(m->log, "gl: fast path: anisotropic filtering, %dx\n", gc_gl_anisotropy);
    }
    if (m->log) {
      fprintf(m->log, "gl: fast path: the card rasterises and blends; depth before texture %s\n",
              gc_gl_fast_early ? "is the card's early test" : "is not available here and is tested late");
    }
  }

  gl->programs = gc_gl_programs_create(m, gl->mode, gl->cache_dir[0] ? gl->cache_dir : NULL, gl->id,
                                       gl->compile_window, gl->compile_context);
  if (!gl->programs) {
    if (m->log) fprintf(m->log, "gl: no programs\n");
    return -1;
  }
  gl->readback = readback_start(gl, m, gl->readback_window, gl->readback_context);
  if (gl->readback) copy_textures_init(gl);
  return 0;
}

static void render_release(GcGlRender *gl) {
  unsigned i;
  if (gl->programs) {
    gc_gl_programs_report(gl->programs, gl->log);
    gc_gl_programs_destroy(gl->programs);
    gl->programs = NULL;
  }
  readback_stop(gl->readback);
  gl->readback = NULL;
  if (gl->gpu_query) {
    glDeleteQueries((GLsizei)GPU_RING, gl->gpu_query);
    free(gl->gpu_query);
    gl->gpu_query = NULL;
  }
  free(gl->gpu_kind);
  gl->gpu_kind = NULL;
  gl->gpu_on = 0;
  for (i = 0; i < GC_GL_TEXTURES; i++) {
    if (gl->textures[i]) glDeleteTextures(1, &gl->textures[i]);
  }
  for (i = 0; i < GC_GL_COPY_TEXTURES; i++) {
    if (gl->copy_textures[i].texture) glDeleteTextures(1, &gl->copy_textures[i].texture);
  }
  if (gl->copy_program) {
    if (gl->log) {
      fprintf(gl->log, "gl: stats copy textures made=%llu taken=%llu unused=%llu lost=%llu ms=%.1f\n",
              (unsigned long long)gl->copy_textures_made, (unsigned long long)gl->copy_textures_taken,
              (unsigned long long)gl->copy_textures_unused, (unsigned long long)gl->copy_textures_lost,
              gl->copy_texture_ms);
    }
    glDeleteProgram(gl->copy_program);
  }
  if (gl->log && gl->snapshots_taken) {
    fprintf(gl->log, "gl: stats late peeks pictures=%llu read_ms=%.1f waits=%llu wait_ms=%.1f\n",
            (unsigned long long)gl->snapshots_taken, gl->snapshot_ms, (unsigned long long)gl->snapshot_waits,
            gl->snapshot_wait_ms);
  }
  if (gl->copy_framebuffer) glDeleteFramebuffers(1, &gl->copy_framebuffer);
  for (i = 0; i < 8u; i++) {
    if (gl->samplers[i][0]) glDeleteSamplers(2, gl->samplers[i]);
  }
  if (gl->array) glDeleteVertexArrays(1, &gl->array);
  if (gl->triangle_buffer) glDeleteBuffers(1, &gl->triangle_buffer);
  if (gl->uniform_buffer) glDeleteBuffers(1, &gl->uniform_buffer);
  if (gl->triangle_texture) glDeleteTextures(1, &gl->triangle_texture);
  if (gl->destination_texture) glDeleteTextures(1, &gl->destination_texture);
  if (gl->own_framebuffer != gl->framebuffer) {
    if (gl->own_color) glDeleteTextures(1, &gl->own_color);
    if (gl->own_depth) glDeleteTextures(1, &gl->own_depth);
    if (gl->own_framebuffer) glDeleteFramebuffers(1, &gl->own_framebuffer);
  }
  if (gl->half_color) glDeleteTextures(1, &gl->half_color);
  if (gl->half_framebuffer) glDeleteFramebuffers(1, &gl->half_framebuffer);
  if (gl->color_texture) glDeleteTextures(1, &gl->color_texture);
  if (gl->depth_texture) glDeleteTextures(1, &gl->depth_texture);
  if (gl->framebuffer) glDeleteFramebuffers(1, &gl->framebuffer);
}

static int render_main(void *argument) {
  GcGlRender *const gl = (GcGlRender *)argument;
  int result = -1;
  if (SDL_GL_MakeCurrent(gl->window, gl->context) == 0) {
    result = render_init(gl);
  } else if (gl->log) {
    fprintf(gl->log, "gl: the drawing context would not move to the render thread: %s\n", SDL_GetError());
  }
  SDL_LockMutex(gl->lock);
  gl->start_result = result;
  gl->started = 1;
  SDL_CondBroadcast(gl->done);
  SDL_UnlockMutex(gl->lock);
  if (result != 0) {
    render_release(gl);
    SDL_GL_MakeCurrent(gl->window, NULL);
    return 1;
  }

  for (;;) {
    GcGlBlock *b;
    int quit;
    double started;
    if (!gl->head) {
      /* Nothing yet: the machine sends a command every few dozen
       * microseconds while a stream runs, so look again for a moment before
       * sleeping - waking a sleeping thread for each one cost the machine's
       * thread more than the look costs this one. */
      const double idle_since = gc_gl_now_ms();
      while (!gl->head && gc_gl_now_ms() - idle_since < 2.0) SDL_Delay(0);
    }
    SDL_LockMutex(gl->lock);
    while (!gl->head) {
      gl->sleeping = 1;
      SDL_CondWait(gl->wake, gl->lock);
    }
    gl->sleeping = 0;
    b = gl->head;
    gl->head = b->next;
    if (!gl->head) gl->tail = NULL;
    SDL_UnlockMutex(gl->lock);

    started = gc_gl_now_ms();
    quit = b->kind == GC_GL_CMD_QUIT;
    run_command(gl, b);
    gl->commands++;
    gl->busy_ms += gc_gl_now_ms() - started;

    SDL_LockMutex(gl->lock);
    gl->queued_bytes -= b->size;
    if (b->kind == GC_GL_CMD_SYNC) memcpy(&gl->done_sequence, (char *)b + BLOCK_HEADER, sizeof(uint64_t));
    if (b->size_class < SIZE_CLASSES) {
      b->next = gl->free_blocks[b->size_class];
      gl->free_blocks[b->size_class] = b;
      b = NULL;
    }
    SDL_CondBroadcast(gl->done);
    SDL_UnlockMutex(gl->lock);
    free(b);
    if (quit) break;
  }
  /* On this thread, while the context is still current: the queries cannot
   * be read once it is gone. */
  gpu_timer_report(gl, gl->log);
  render_release(gl);
  SDL_GL_MakeCurrent(gl->window, NULL);
  return 0;
}

static void render_free(GcGlRender *gl) {
  unsigned i;
  for (i = 0; i < SIZE_CLASSES; i++) {
    while (gl->free_blocks[i]) {
      GcGlBlock *const next = gl->free_blocks[i]->next;
      free(gl->free_blocks[i]);
      gl->free_blocks[i] = next;
    }
  }
  if (gl->done) SDL_DestroyCond(gl->done);
  if (gl->wake) SDL_DestroyCond(gl->wake);
  if (gl->lock) SDL_DestroyMutex(gl->lock);
  if (gl->picture_lock) SDL_DestroyMutex(gl->picture_lock);
  if (gl->snapshot_landed) SDL_DestroyCond(gl->snapshot_landed);
  if (gl->snapshot_lock) SDL_DestroyMutex(gl->snapshot_lock);
  for (i = 0; i < GC_GL_SNAPSHOTS; i++) {
    free(gl->snapshots[i].plane[0]);
    free(gl->snapshots[i].plane[1]);
  }
  for (i = 0; i < GC_GL_PICTURES; i++) free(gl->pictures[i].pixels);
  free(gl->sorted);
  free(gl->layer_of);
  free(gl->spans);
  free(gl->span_at);
  free(gl->layer_map);
  free(gl->depth_transfer);
  free(gl);
}

GcGlRender *gc_gl_render_start(const GcGlRenderSetup *setup) {
  GcGlRender *gl = (GcGlRender *)calloc(1, sizeof(GcGlRender));
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  const char *version = (const char *)glGetString(GL_VERSION);
  if (!gl) return NULL;
  gl->m = setup->m;
  gl->log = setup->m->log;
  gl->window = (SDL_Window *)setup->window;
  gl->context = (SDL_GLContext)setup->context;
  gl->compile_window = (SDL_Window *)setup->compile_window;
  gl->compile_context = (SDL_GLContext)setup->compile_context;
  gl->readback_window = (SDL_Window *)setup->readback_window;
  gl->readback_context = (SDL_GLContext)setup->readback_context;
  gl->mode = setup->mode;
  gl->boxes = setup->boxes;
  snprintf(gl->cache_dir, sizeof(gl->cache_dir), "%s", setup->cache_dir ? setup->cache_dir : "");
  snprintf(gl->id, sizeof(gl->id), "%s", setup->id ? setup->id : "game");
  snprintf(gl->name, sizeof(gl->name), "%s, OpenGL %s", renderer ? renderer : "(unknown)",
           version ? version : "(unknown)");
  gl->lock = SDL_CreateMutex();
  gl->picture_lock = SDL_CreateMutex();
  gl->snapshot_lock = SDL_CreateMutex();
  gl->snapshot_landed = SDL_CreateCond();
  gl->wake = SDL_CreateCond();
  gl->done = SDL_CreateCond();
  if (!gl->lock || !gl->picture_lock || !gl->wake || !gl->done) {
    render_free(gl);
    return NULL;
  }
  /* The context moves: released here, made current there. */
  SDL_GL_MakeCurrent(gl->window, NULL);
  gl->thread = SDL_CreateThread(render_main, "gc-gl-render", gl);
  if (!gl->thread) {
    if (gl->log) fprintf(gl->log, "gl: no render thread: %s\n", SDL_GetError());
    SDL_GL_MakeCurrent(gl->window, gl->context);
    render_free(gl);
    return NULL;
  }
  SDL_LockMutex(gl->lock);
  while (!gl->started) SDL_CondWait(gl->done, gl->lock);
  SDL_UnlockMutex(gl->lock);
  if (gl->start_result != 0) {
    SDL_WaitThread(gl->thread, NULL);
    render_free(gl);
    return NULL;
  }
  return gl;
}

void gc_gl_render_stop(GcGlRender *gl, FILE *log) {
  if (!gl) return;
  gc_gl_render_settle(gl);
  gc_gl_render_command(gl, GC_GL_CMD_QUIT, 0);
  gc_gl_render_send(gl);
  SDL_WaitThread(gl->thread, NULL);
  if (log) {
    fprintf(log,
            "gl: stats render commands=%llu busy_ms=%.1f states=%llu state_ms=%.1f draws=%llu draw_ms=%.1f "
            "dst_copies=%llu layers=%llu most_layers=%u layer_draws=%llu layer_ms=%.1f layer_map_ms=%.1f "
            "layer_rows=%llu textures=%llu texture_ms=%.1f uploads=%llu upload_ms=%.1f downloads=%llu "
            "download_ms=%.1f copies=%llu copy_ms=%.1f machine_waits=%llu machine_wait_ms=%.1f full_waits=%llu "
            "capacity=%u pixel_downloads=%llu copy_limit_waits=%llu copy_limit_ms=%.1f barriers=%llu scale=%u brought_down=%llu pictures_kept=%llu pictures_given=%llu\n",
            (unsigned long long)gl->commands, gl->busy_ms, (unsigned long long)gl->states, gl->state_ms,
            (unsigned long long)gl->draws, gl->draw_ms, (unsigned long long)gl->dst_copies,
            (unsigned long long)gl->layers, gl->most_layers, (unsigned long long)gl->layer_draws, gl->layer_ms,
            gl->layer_map_ms, (unsigned long long)gl->layer_rows, (unsigned long long)gl->textures_made,
            gl->texture_ms, (unsigned long long)gl->uploads, gl->upload_ms, (unsigned long long)gl->downloads,
            gl->download_ms, (unsigned long long)gl->copies, gl->copy_ms, (unsigned long long)gl->machine_waits,
            gl->machine_wait_ms, (unsigned long long)gl->full_waits, gl->capacity,
            (unsigned long long)gl->pixel_downloads, (unsigned long long)gl->copy_limit_waits, gl->copy_limit_ms,
            (unsigned long long)gl->barriers, gl->scale, (unsigned long long)gl->brought_down,
            (unsigned long long)gl->pictures_kept, (unsigned long long)gl->pictures_given);
  }
  render_free(gl);
}

#endif /* GC_HAVE_SDL2 */
