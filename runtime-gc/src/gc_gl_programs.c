/*
 * Programs for the OpenGL path: which one draws a pipeline, when it gets
 * compiled, and where compiled programs are kept between runs.
 *
 * Three ways to run, chosen by GC_GL_SHADERS (or gc_gl_start_with):
 *
 *   specialised  every pipeline gets its own program, compiled the first
 *                time the pipeline is drawn, on the drawing thread. The
 *                fastest programs and the worst hitch.
 *   uber         one general program, compiled at start, draws everything.
 *                No hitch at all; every pixel pays for branches the
 *                specialised program folded away.
 *   hybrid       the default: a new pipeline draws with the general program
 *                while a second thread, with a context that shares objects
 *                with the drawing one, compiles the specialised program -
 *                which takes over from the first batch after it links.
 *
 * The three are interchangeable by construction: both kinds of program are
 * the same text (gc_gl_shader.c) with the pipeline either read from a
 * uniform block or baked in as constants. compare_gl.py runs every test
 * program in all three and holds each to the reference.
 *
 * The fast path (gc_gl.h) has two general programs where this says one: a
 * pipeline's "depth before texture" is a declaration in the text, which a
 * uniform cannot stand in for, so there is a general program with it and
 * one without (general_for).
 *
 * Compiled programs are kept on disk with glGetProgramBinary, keyed by a
 * hash of both shaders' complete text - which includes the whole pipeline,
 * so it is also the build's hash for everything that matters - and of the
 * driver's vendor, renderer and version strings. A binary the driver
 * refuses (a new driver, a different card) is compiled again from source
 * and overwritten, and says so in the statistics rather than anywhere
 * louder. Next to them, `<id>.pipelines` records every pipeline shape a run
 * used, one per line, and a later start compiles all of them before the
 * first field - or, in hybrid mode, hands them all to the compile thread.
 */
#include "gc.h"

#ifdef GC_HAVE_SDL2

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "gc_gl_internal.h"
#include "gc_perf.h"

#define GC_GL_PIPELINES 4096u
#define GC_GL_SOURCE 131072

enum {
  PIPELINE_NEW = 0,
  PIPELINE_QUEUED = 1,
  PIPELINE_READY = 2,
  PIPELINE_FAILED = 3
};

typedef struct GcGlPipelineEntry {
  uint64_t key;
  uint32_t shape[GC_GL_SHAPE_WORDS];
  GLuint program;
  SDL_atomic_t state;
  double seen_ms; /* when it was first asked for */
  int used;       /* a slot of the table */
} GcGlPipelineEntry;

struct GcGlPrograms {
  GcMachine *m;
  GcGlShaderMode mode;
  int cache_on;
  char cache_dir[1024];
  char id[256];
  char driver[768];
  GLuint uber;
  /* The fast path's second general program, for pipelines with "depth
   * before texture" (gc_gl_general_source), and whether building it has
   * been tried: one that will not build is not tried at every draw. */
  GLuint uber_early;
  int uber_early_tried;

  GcGlPipelineEntry *entries; /* open addressing on the key */
  unsigned count;
  FILE *pipelines;

  /* The compile thread. */
  SDL_Thread *thread;
  SDL_mutex *lock;
  SDL_cond *wake;
  unsigned queue[GC_GL_PIPELINES];
  unsigned head, tail;
  int quit;
  SDL_Window *worker_window;
  SDL_GLContext worker_context;

  /* What it cost. Only the drawing thread writes the first group. */
  unsigned compiled_sync, from_disk_sync;
  double sync_ms, startup_ms;
  uint64_t field;
  double field_ms, worst_field_ms;
  uint64_t worst_field;
  unsigned fields_with_compiles;
  uint64_t uber_batches, uber_early_batches, specialised_batches;
  /* The compile thread's, read after it has stopped. */
  unsigned compiled_async, from_disk_async;
  double async_ms, worst_wait_ms;
  int parallel; /* the driver compiles a batch at once */
  SDL_atomic_t stopping; /* read by the compile thread between its polls */
  /* Both, under the lock. */
  unsigned disk_rejected, disk_written, failures;
};

/* ------------------------------------------------------------------ */
/* Small things                                                        */

double gc_gl_now_ms(void) {
  static Uint64 frequency;
  if (!frequency) frequency = SDL_GetPerformanceFrequency();
  return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)frequency;
}

/* xxHash64's structure, written from its published description: four lanes
 * of 64-bit multiply-rotate over the bulk, then the tail, then an
 * avalanche. Nothing depends on this being xxHash exactly, only on it being
 * fast and on two different textures almost never colliding. */
#define P1 11400714785074694791ull
#define P2 14029467366897019727ull
#define P3 1609587929392839161ull
#define P4 9650029242287828579ull
#define P5 2870177450012600261ull
static uint64_t rotl(uint64_t v, unsigned r) { return (v << r) | (v >> (64u - r)); }
static uint64_t lane(uint64_t acc, uint64_t input) {
  acc += input * P2;
  acc = rotl(acc, 31);
  return acc * P1;
}
static uint64_t merge(uint64_t acc, uint64_t v) {
  acc ^= lane(0, v);
  return acc * P1 + P4;
}

uint64_t gc_gl_hash(const void *data, size_t length, uint64_t seed) {
  const uint8_t *p = (const uint8_t *)data;
  const uint8_t *const end = p + length;
  uint64_t h, word;

  if (length >= 32u) {
    const uint8_t *const limit = end - 32;
    uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
    do {
      memcpy(&word, p, 8); v1 = lane(v1, word); p += 8;
      memcpy(&word, p, 8); v2 = lane(v2, word); p += 8;
      memcpy(&word, p, 8); v3 = lane(v3, word); p += 8;
      memcpy(&word, p, 8); v4 = lane(v4, word); p += 8;
    } while (p <= limit);
    h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    h = merge(h, v1);
    h = merge(h, v2);
    h = merge(h, v3);
    h = merge(h, v4);
  } else {
    h = seed + P5;
  }
  h += (uint64_t)length;
  while (p + 8 <= end) {
    memcpy(&word, p, 8);
    h ^= lane(0, word);
    h = rotl(h, 27) * P1 + P4;
    p += 8;
  }
  if (p + 4 <= end) {
    uint32_t half;
    memcpy(&half, p, 4);
    h ^= (uint64_t)half * P1;
    h = rotl(h, 23) * P2 + P3;
    p += 4;
  }
  while (p < end) {
    h ^= (uint64_t)(*p) * P5;
    h = rotl(h, 11) * P1;
    p++;
  }
  h ^= h >> 33;
  h *= P2;
  h ^= h >> 29;
  h *= P3;
  h ^= h >> 32;
  return h;
}

/* ------------------------------------------------------------------ */
/* Compiling one program                                               */

static GLuint compile(FILE *log, GLenum kind, const char *source) {
  GLuint shader = glCreateShader(kind);
  GLint ok = 0;
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char text[4096];
    GLsizei length = 0;
    glGetShaderInfoLog(shader, (GLsizei)sizeof(text), &length, text);
    if (log) fprintf(log, "gl: a shader would not compile: %.*s\n", (int)length, text);
    if (getenv("GC_GL_DUMP")) {
      /* The text, with line numbers as the driver counts them. */
      FILE *dump = fopen(getenv("GC_GL_DUMP"), "w");
      if (dump) {
        fputs(source, dump);
        fclose(dump);
      }
    }
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

/* What every program needs after it links, and again after it is loaded
 * from a binary - a loaded binary's uniforms start at their defaults. */
static void prepare(GLuint program) {
  static const char *NAMES[8] = {"maps[0]", "maps[1]", "maps[2]", "maps[3]", "maps[4]", "maps[5]", "maps[6]", "maps[7]"};
  /* The fast path's second binding of each map (gc_gl_shader.c). */
  static const char *MIN_NAMES[8] = {"maps_min[0]", "maps_min[1]", "maps_min[2]", "maps_min[3]",
                                     "maps_min[4]", "maps_min[5]", "maps_min[6]", "maps_min[7]"};
  GLuint block;
  GLint at;
  unsigned i;
  glUseProgram(program);
  for (i = 0; i < 8u; i++) {
    at = glGetUniformLocation(program, NAMES[i]);
    if (at >= 0) glUniform1i(at, (GLint)i);
    at = glGetUniformLocation(program, MIN_NAMES[i]);
    if (at >= 0) glUniform1i(at, (GLint)(GC_GL_UNIT_MINIFY + i));
  }
  at = glGetUniformLocation(program, "destination");
  if (at >= 0) glUniform1i(at, 8);
  at = glGetUniformLocation(program, "tris");
  if (at >= 0) glUniform1i(at, 9);
  block = glGetUniformBlockIndex(program, "Draw");
  if (block != GL_INVALID_INDEX) glUniformBlockBinding(program, block, 0);
  glUseProgram(0);
}

static void cache_path(const GcGlPrograms *p, uint64_t key, char *out, size_t size) {
  /* Named after the game as well, so they sit beside its other files (the
   * hub keeps <id>.* through a rebuild). */
  snprintf(out, size, "%s/%s.%016llx.glbin", p->cache_dir, p->id, (unsigned long long)key);
}

typedef struct GcGlBinaryHeader {
  char magic[8];
  uint32_t format;
  uint32_t length;
  uint64_t key;
} GcGlBinaryHeader;

static GLuint load_binary(GcGlPrograms *p, uint64_t key) {
  char path[1300];
  GcGlBinaryHeader header;
  FILE *f;
  void *data;
  GLuint program;
  GLint ok = 0;

  cache_path(p, key, path, sizeof(path));
  f = fopen(path, "rb");
  if (!f) return 0;
  if (fread(&header, sizeof(header), 1, f) != 1 || memcmp(header.magic, "GCGLPB01", 8) != 0 ||
      header.key != key || header.length == 0 || header.length > (64u << 20)) {
    fclose(f);
    return 0;
  }
  data = malloc(header.length);
  if (!data || fread(data, 1, header.length, f) != header.length) {
    free(data);
    fclose(f);
    return 0;
  }
  fclose(f);
  program = glCreateProgram();
  glProgramBinary(program, (GLenum)header.format, data, (GLsizei)header.length);
  free(data);
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    /* A new driver, a different card: build it again from source. */
    glDeleteProgram(program);
    SDL_LockMutex(p->lock);
    p->disk_rejected++;
    SDL_UnlockMutex(p->lock);
    return 0;
  }
  return program;
}

static void save_binary(GcGlPrograms *p, GLuint program, uint64_t key) {
  char path[1300], temporary[1310];
  GcGlBinaryHeader header;
  GLint length = 0;
  GLsizei written = 0;
  GLenum format = 0;
  void *data;
  FILE *f;

  glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
  if (length <= 0) return;
  data = malloc((size_t)length);
  if (!data) return;
  glGetProgramBinary(program, length, &written, &format, data);
  if (written <= 0) {
    free(data);
    return;
  }
  memcpy(header.magic, "GCGLPB01", 8);
  header.format = format;
  header.length = (uint32_t)written;
  header.key = key;
  cache_path(p, key, path, sizeof(path));
  /* Written beside and renamed over, so a run that dies mid-write leaves
   * no half a binary for the next one to trust. */
  snprintf(temporary, sizeof(temporary), "%s.%p", path, (void *)SDL_ThreadID());
  f = fopen(temporary, "wb");
  if (f) {
    const int ok = fwrite(&header, sizeof(header), 1, f) == 1 &&
                   fwrite(data, 1, (size_t)written, f) == (size_t)written;
    fclose(f);
    remove(path);
    if (ok && rename(temporary, path) == 0) {
      SDL_LockMutex(p->lock);
      p->disk_written++;
      SDL_UnlockMutex(p->lock);
    } else {
      remove(temporary);
    }
  }
  free(data);
}

/* Build the program for a shape (or the general one), from disk if it is
 * there. Callable on either thread: it touches only GL objects of its own
 * and the counters under the lock. */
/* A pipeline's own program, or with a null `shape` a general one: the
 * second of the fast path's two when `early` (gc_gl_general_source). */
static GLuint build(GcGlPrograms *p, const uint32_t *shape, int early, int *from_disk) {
  char *source = (char *)malloc(GC_GL_SOURCE);
  GLuint vertex, fragment, program;
  GLint ok = 0;
  uint64_t key;

  *from_disk = 0;
  if (!source) return 0;
  if ((shape ? gc_gl_fragment_source(shape, source, GC_GL_SOURCE)
             : gc_gl_general_source(early, source, GC_GL_SOURCE)) != 0) {
    free(source);
    return 0;
  }
  if (!shape && !early && getenv("GC_GL_WRITE_SOURCE")) {
    /* For working on the shader: the general program's text as built. */
    FILE *out = fopen(getenv("GC_GL_WRITE_SOURCE"), "w");
    if (out) {
      fputs(source, out);
      fclose(out);
    }
  }
  key = gc_gl_hash(source, strlen(source), 0);
  key = gc_gl_hash(gc_gl_vertex_source, strlen(gc_gl_vertex_source), key);
  key = gc_gl_hash(p->driver, strlen(p->driver), key);

  if (p->cache_on) {
    program = load_binary(p, key);
    if (program) {
      free(source);
      prepare(program);
      *from_disk = 1;
      return program;
    }
  }

  vertex = compile(p->m->log, GL_VERTEX_SHADER, gc_gl_vertex_source);
  fragment = vertex ? compile(p->m->log, GL_FRAGMENT_SHADER, source) : 0;
  free(source);
  if (!vertex || !fragment) {
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return 0;
  }
  program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  glDetachShader(program, vertex);
  glDetachShader(program, fragment);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  if (!ok) {
    char text[4096];
    GLsizei length = 0;
    glGetProgramInfoLog(program, (GLsizei)sizeof(text), &length, text);
    if (p->m->log) fprintf(p->m->log, "gl: a program would not link: %.*s\n", (int)length, text);
    glDeleteProgram(program);
    return 0;
  }
  prepare(program);
  if (p->cache_on) save_binary(p, program, key);
  return program;
}

/* ------------------------------------------------------------------ */
/* The table                                                           */

static uint64_t shape_key(const uint32_t *shape) {
  return gc_gl_hash(shape, sizeof(uint32_t) * GC_GL_SHAPE_WORDS, 0x6763676cull);
}

static GcGlPipelineEntry *find(GcGlPrograms *p, const uint32_t *shape, int *created) {
  const uint64_t key = shape_key(shape);
  unsigned at = (unsigned)(key & (GC_GL_PIPELINES - 1u));
  unsigned probes;
  *created = 0;
  for (probes = 0; probes < GC_GL_PIPELINES; probes++) {
    GcGlPipelineEntry *e = &p->entries[at];
    if (!e->used) {
      if (p->count + 1u >= GC_GL_PIPELINES) return NULL;
      e->used = 1;
      e->key = key;
      memcpy(e->shape, shape, sizeof(e->shape));
      e->program = 0;
      SDL_AtomicSet(&e->state, PIPELINE_NEW);
      e->seen_ms = gc_gl_now_ms();
      p->count++;
      *created = 1;
      return e;
    }
    if (e->key == key && memcmp(e->shape, shape, sizeof(e->shape)) == 0) return e;
    at = (at + 1u) & (GC_GL_PIPELINES - 1u);
  }
  return NULL;
}

static void record_shape(GcGlPrograms *p, const uint32_t *shape) {
  unsigned i;
  if (!p->pipelines) return;
  for (i = 0; i < GC_GL_SHAPE_WORDS; i++) fprintf(p->pipelines, "%s%x", i ? " " : "", shape[i]);
  fputc('\n', p->pipelines);
  fflush(p->pipelines);
}

static void enqueue(GcGlPrograms *p, GcGlPipelineEntry *e) {
  SDL_AtomicSet(&e->state, PIPELINE_QUEUED);
  SDL_LockMutex(p->lock);
  p->queue[p->tail] = (unsigned)(e - p->entries);
  p->tail = (p->tail + 1u) % GC_GL_PIPELINES;
  SDL_CondSignal(p->wake);
  SDL_UnlockMutex(p->lock);
}

/* Compile time charged to the field it happened in, so the worst field -
 * the hitch someone would see - can be reported rather than only a sum. */
static void charge(GcGlPrograms *p, uint64_t field, double ms) {
  if (field != p->field) {
    p->field = field;
    p->field_ms = 0.0;
  }
  if (p->field_ms == 0.0 && ms > 0.0) p->fields_with_compiles++;
  p->field_ms += ms;
  p->sync_ms += ms;
  if (p->field_ms > p->worst_field_ms) {
    p->worst_field_ms = p->field_ms;
    p->worst_field = field;
  }
}

static void build_now(GcGlPrograms *p, GcGlPipelineEntry *e, uint64_t field, int at_startup) {
  const double started = gc_gl_now_ms();
  int from_disk = 0;
  GLuint program = build(p, e->shape, 0, &from_disk);
  const double ms = gc_gl_now_ms() - started;
  if (at_startup) {
    p->startup_ms += ms;
  } else {
    charge(p, field, ms);
    gc_perf.programs_waited++;
    gc_perf.program_wait_ms += ms;
  }
  if (from_disk) p->from_disk_sync++;
  else if (program) p->compiled_sync++;
  e->program = program;
  if (!program) p->failures++;
  SDL_AtomicSet(&e->state, program ? PIPELINE_READY : PIPELINE_FAILED);
}

/* The compile thread starts a batch of programs at once and finishes each
 * as the driver reports it linked. With GL_ARB_parallel_shader_compile (or
 * the KHR one) the driver compiles them on threads of its own, so a batch of
 * pipelines listed from an earlier run takes about as long as one program
 * rather than their sum; without it, the status query waits and they finish
 * one after another, as before. */
#define GC_GL_COMPILE_BATCH 64u
#define GL_COMPLETION_STATUS 0x91B1

typedef struct GcGlPendingBuild {
  GcGlPipelineEntry *e;
  GLuint fragment, program;
  uint64_t key;
} GcGlPendingBuild;

static int start_build(GcGlPrograms *p, GcGlPipelineEntry *e, GLuint vertex, char *source, GcGlPendingBuild *out) {
  const char *text = source;
  uint64_t key;
  out->e = e;
  out->fragment = 0;
  out->program = 0;
  if (gc_gl_fragment_source(e->shape, source, GC_GL_SOURCE) != 0) return -1;
  key = gc_gl_hash(source, strlen(source), 0);
  key = gc_gl_hash(gc_gl_vertex_source, strlen(gc_gl_vertex_source), key);
  key = gc_gl_hash(p->driver, strlen(p->driver), key);
  out->key = key;
  if (p->cache_on) {
    GLuint program = load_binary(p, key);
    if (program) {
      prepare(program);
      out->program = program;
      return 1;
    }
  }
  out->fragment = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(out->fragment, 1, &text, NULL);
  glCompileShader(out->fragment);
  out->program = glCreateProgram();
  glAttachShader(out->program, vertex);
  glAttachShader(out->program, out->fragment);
  glLinkProgram(out->program);
  return 0;
}

/* Whether it has finished; then the program, or 0 if it failed. */
static int finish_build(GcGlPrograms *p, GLuint vertex, GcGlPendingBuild *b, GLuint *program) {
  GLint done = 1, ok = 0;
  glGetProgramiv(b->program, GL_COMPLETION_STATUS, &done);
  if (!done) return 0;
  glGetProgramiv(b->program, GL_LINK_STATUS, &ok);
  glDetachShader(b->program, vertex);
  glDetachShader(b->program, b->fragment);
  if (!ok) {
    char text[4096];
    GLsizei length = 0;
    GLint compiled = 0;
    glGetShaderiv(b->fragment, GL_COMPILE_STATUS, &compiled);
    if (!compiled) glGetShaderInfoLog(b->fragment, (GLsizei)sizeof(text), &length, text);
    else glGetProgramInfoLog(b->program, (GLsizei)sizeof(text), &length, text);
    if (p->m->log) {
      fprintf(p->m->log, "gl: a program would not %s: %.*s\n", compiled ? "link" : "compile", (int)length, text);
    }
    glDeleteShader(b->fragment);
    glDeleteProgram(b->program);
    *program = 0;
    return 1;
  }
  glDeleteShader(b->fragment);
  prepare(b->program);
  if (p->cache_on) save_binary(p, b->program, b->key);
  *program = b->program;
  return 1;
}

static void publish(GcGlPrograms *p, GcGlPipelineEntry *e, GLuint program, int from_disk) {
  SDL_LockMutex(p->lock);
  if (gc_gl_now_ms() - e->seen_ms > p->worst_wait_ms) p->worst_wait_ms = gc_gl_now_ms() - e->seen_ms;
  if (from_disk) p->from_disk_async++;
  else if (program) p->compiled_async++;
  if (!program) p->failures++;
  SDL_UnlockMutex(p->lock);
  /* Linked by now (the completion status said so), so the drawing thread
   * can use the program the moment it sees it. */
  e->program = program;
  SDL_AtomicSet(&e->state, program ? PIPELINE_READY : PIPELINE_FAILED);
}

static int worker(void *argument) {
  GcGlPrograms *p = (GcGlPrograms *)argument;
  GcGlPendingBuild pending[GC_GL_COMPILE_BATCH];
  char *source = (char *)malloc(GC_GL_SOURCE);
  GLuint vertex;
  if (!source) return 1;
  if (SDL_GL_MakeCurrent(p->worker_window, p->worker_context) != 0) {
    free(source);
    return 1;
  }
  if (SDL_GL_ExtensionSupported("GL_KHR_parallel_shader_compile") ||
      SDL_GL_ExtensionSupported("GL_ARB_parallel_shader_compile")) {
    void (*threads)(GLuint) = (void (*)(GLuint))SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsKHR");
    if (!threads) threads = (void (*)(GLuint))SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsARB");
    if (threads) {
      threads(getenv("GC_GL_COMPILE_THREADS") ? (GLuint)strtoul(getenv("GC_GL_COMPILE_THREADS"), NULL, 10)
                                              : 0xffffffffu);
      p->parallel = 1;
    }
  }
  /* Every program has the same vertex shader; this thread's copy is
   * attached to each of its programs. */
  vertex = compile(p->m->log, GL_VERTEX_SHADER, gc_gl_vertex_source);
  for (;;) {
    unsigned count = 0, i, left;
    double started;
    SDL_LockMutex(p->lock);
    while (p->head == p->tail && !p->quit) SDL_CondWait(p->wake, p->lock);
    if (p->quit) {
      SDL_UnlockMutex(p->lock);
      break;
    }
    while (p->head != p->tail && count < GC_GL_COMPILE_BATCH) {
      pending[count++].e = &p->entries[p->queue[p->head]];
      p->head = (p->head + 1u) % GC_GL_PIPELINES;
    }
    SDL_UnlockMutex(p->lock);

    started = gc_gl_now_ms();
    left = 0;
    for (i = 0; i < count; i++) {
      GcGlPipelineEntry *e = pending[i].e;
      const int state = vertex ? start_build(p, e, vertex, source, &pending[left]) : -1;
      if (state == 0) {
        left++;
      } else {
        publish(p, e, state == 1 ? pending[left].program : 0, state == 1);
      }
    }
    while (left) {
      unsigned kept = 0;
      if (SDL_AtomicGet(&p->stopping)) {
        /* Stopping: what has not linked is not wanted. Deleting a program
         * the driver is still building is allowed; it goes when it is done. */
        for (i = 0; i < left; i++) {
          glDetachShader(pending[i].program, vertex);
          glDetachShader(pending[i].program, pending[i].fragment);
          glDeleteShader(pending[i].fragment);
          glDeleteProgram(pending[i].program);
        }
        break;
      }
      for (i = 0; i < left; i++) {
        GLuint program;
        if (finish_build(p, vertex, &pending[i], &program)) {
          publish(p, pending[i].e, program, 0);
        } else {
          pending[kept++] = pending[i];
        }
      }
      if (kept == left) SDL_Delay(1);
      left = kept;
    }
    SDL_LockMutex(p->lock);
    p->async_ms += gc_gl_now_ms() - started;
    SDL_UnlockMutex(p->lock);
  }
  if (vertex) glDeleteShader(vertex);
  free(source);
  SDL_GL_MakeCurrent(p->worker_window, NULL);
  return 0;
}

/* ------------------------------------------------------------------ */

static int make_directory(const char *path) {
#ifdef _WIN32
  return _mkdir(path);
#else
  return mkdir(path, 0777);
#endif
}

static void precompile_listed(GcGlPrograms *p) {
  char path[1300];
  char line[4096];
  FILE *f;
  unsigned listed = 0;

  snprintf(path, sizeof(path), "%s/%s.pipelines", p->cache_dir, p->id);
  f = fopen(path, "r");
  if (f) {
    while (fgets(line, sizeof(line), f)) {
      uint32_t shape[GC_GL_SHAPE_WORDS];
      char *at = line;
      unsigned i;
      int created;
      GcGlPipelineEntry *e;
      for (i = 0; i < GC_GL_SHAPE_WORDS; i++) {
        char *next;
        shape[i] = (uint32_t)strtoul(at, &next, 16);
        if (next == at) break;
        at = next;
      }
      if (i != GC_GL_SHAPE_WORDS) continue;
      e = find(p, shape, &created);
      if (!e || !created) continue;
      listed++;
      if (p->mode == GC_GL_SHADERS_SPECIALISED) {
        build_now(p, e, 0, 1);
      } else if (p->mode == GC_GL_SHADERS_HYBRID) {
        enqueue(p, e);
      }
    }
    fclose(f);
  }
  /* Appended to from here on: every shape this run adds. */
  p->pipelines = fopen(path, "a");
  if (p->m->log && listed) {
    fprintf(p->m->log, "gl: %u pipelines listed in %s, %s\n", listed, path,
            p->mode == GC_GL_SHADERS_SPECIALISED ? "compiled before the first field"
            : p->mode == GC_GL_SHADERS_HYBRID     ? "queued for the compile thread"
                                                  : "not needed by the general program");
  }
}

GcGlPrograms *gc_gl_programs_create(GcMachine *m, GcGlShaderMode mode, const char *cache_dir,
                                    const char *id, void *compile_window, void *compile_context) {
  GcGlPrograms *p = (GcGlPrograms *)calloc(1, sizeof(GcGlPrograms));
  const char *vendor, *renderer, *version, *language;
  if (!p) return NULL;
  p->entries = (GcGlPipelineEntry *)calloc(GC_GL_PIPELINES, sizeof(GcGlPipelineEntry));
  p->lock = SDL_CreateMutex();
  p->wake = SDL_CreateCond();
  if (!p->entries || !p->lock || !p->wake) {
    gc_gl_programs_destroy(p);
    return NULL;
  }
  p->m = m;
  p->mode = mode;
  p->field = (uint64_t)-1;

  vendor = (const char *)glGetString(GL_VENDOR);
  renderer = (const char *)glGetString(GL_RENDERER);
  version = (const char *)glGetString(GL_VERSION);
  language = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
  snprintf(p->driver, sizeof(p->driver), "%s|%s|%s|%s", vendor ? vendor : "", renderer ? renderer : "",
           version ? version : "", language ? language : "");

  {
    /* Which spelling of the double unpacking function this compiler takes
     * (see gc_gl_shader.c). Asked quietly: a failure here is the answer,
     * not an error. */
    static const char *const CANDIDATES[2] = {"unpackDouble2x32", "packDouble2x32"};
    unsigned i;
    for (i = 0; i < 2u; i++) {
      char probe[512];
      const char *text = probe;
      GLuint shader = glCreateShader(GL_FRAGMENT_SHADER);
      GLint ok = 0;
      snprintf(probe, sizeof(probe),
               "#version 410 core\nout vec4 c;\nvoid main() { double d = %s(uvec2(0u, 0x3ff00000u));"
               " c = vec4(float(d)); }\n",
               CANDIDATES[i]);
      glShaderSource(shader, 1, &text, NULL);
      glCompileShader(shader);
      glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
      glDeleteShader(shader);
      if (ok) {
        gc_gl_unpack_double = CANDIDATES[i];
        break;
      }
    }
    gc_gl_build_vertex_source();
  }

  if (cache_dir && *cache_dir) {
    snprintf(p->cache_dir, sizeof(p->cache_dir), "%s", cache_dir);
    snprintf(p->id, sizeof(p->id), "%s", (id && *id) ? id : "game");
    make_directory(p->cache_dir);
    p->cache_on = 1;
  }

  if (mode == GC_GL_SHADERS_HYBRID) {
    /* The thread compiles in a context that shares objects with the drawing
     * one, made by the caller on the thread that owns the windows. Without
     * one, hybrid becomes specialised programs compiled on the drawing
     * thread, and the log says so. */
    p->worker_window = (SDL_Window *)compile_window;
    p->worker_context = (SDL_GLContext)compile_context;
    if (p->worker_context) p->thread = SDL_CreateThread(worker, "gc-gl-compile", p);
    if (!p->thread) {
      if (m->log) fprintf(m->log, "gl: no compile thread (%s); compiling on the drawing thread\n", SDL_GetError());
      p->mode = GC_GL_SHADERS_SPECIALISED;
    }
  }

  if (p->mode != GC_GL_SHADERS_SPECIALISED) {
    const double started = gc_gl_now_ms();
    int from_disk = 0;
    p->uber = build(p, NULL, 0, &from_disk);
    p->startup_ms += gc_gl_now_ms() - started;
    if (from_disk) p->from_disk_sync++;
    else if (p->uber) p->compiled_sync++;
    if (!p->uber) {
      if (m->log) fprintf(m->log, "gl: the general program would not build\n");
      gc_gl_programs_destroy(p);
      return NULL;
    }
    if (gc_gl_fast && gc_gl_fast_early) {
      /* The fast path's second one, now and not at the first pipeline that
       * wants it: it is as long a compile as the first. */
      const double second = gc_gl_now_ms();
      from_disk = 0;
      p->uber_early = build(p, NULL, 1, &from_disk);
      p->uber_early_tried = 1;
      p->startup_ms += gc_gl_now_ms() - second;
      if (from_disk) p->from_disk_sync++;
      else if (p->uber_early) p->compiled_sync++;
      if (!p->uber_early && m->log) {
        fprintf(m->log, "gl: the general program with depth before texture would not build; "
                        "the other one draws those pipelines until their own are ready\n");
      }
    }
  }

  if (p->cache_on) precompile_listed(p);
  return p;
}

static void stop_thread(GcGlPrograms *p) {
  if (!p->thread) return;
  SDL_AtomicSet(&p->stopping, 1);
  SDL_LockMutex(p->lock);
  p->quit = 1;
  SDL_CondSignal(p->wake);
  SDL_UnlockMutex(p->lock);
  SDL_WaitThread(p->thread, NULL);
  p->thread = NULL;
}

void gc_gl_programs_destroy(GcGlPrograms *p) {
  unsigned i;
  if (!p) return;
  stop_thread(p);
  if (p->entries) {
    for (i = 0; i < GC_GL_PIPELINES; i++) {
      if (p->entries[i].used && p->entries[i].program) glDeleteProgram(p->entries[i].program);
    }
  }
  if (p->uber) glDeleteProgram(p->uber);
  if (p->uber_early) glDeleteProgram(p->uber_early);
  if (p->pipelines) fclose(p->pipelines);
  if (p->wake) SDL_DestroyCond(p->wake);
  if (p->lock) SDL_DestroyMutex(p->lock);
  free(p->entries);
  free(p);
}

/* The general program that draws a pipeline: on the fast path the one with
 * the card's depth test first when the pipeline has "depth before texture"
 * (shape[72]), and the other one otherwise and everywhere else. Built here
 * when the mode did not build it at the start (specialised, which only
 * needs one when a pipeline's own program fails or the table is full). */
static GLuint general_for(GcGlPrograms *p, const uint32_t *shape, uint64_t field) {
  int from_disk = 0;
  if (gc_gl_fast && gc_gl_fast_early && shape[72] != 0u) {
    if (!p->uber_early && !p->uber_early_tried) {
      const double started = gc_gl_now_ms();
      p->uber_early = build(p, NULL, 1, &from_disk);
      p->uber_early_tried = 1;
      charge(p, field, gc_gl_now_ms() - started);
    }
    if (p->uber_early) {
      p->uber_early_batches++;
      return p->uber_early;
    }
  }
  if (!p->uber) {
    const double started = gc_gl_now_ms();
    p->uber = build(p, NULL, 0, &from_disk);
    charge(p, field, gc_gl_now_ms() - started);
  }
  return p->uber;
}

GLuint gc_gl_programs_get(GcGlPrograms *p, const uint32_t *shape, uint64_t field, int *is_uber) {
  int created = 0;
  GcGlPipelineEntry *e;

  *is_uber = 0;
  if (p->mode == GC_GL_SHADERS_UBER) {
    e = find(p, shape, &created);
    if (created) record_shape(p, shape);
    *is_uber = 1;
    p->uber_batches++;
    return general_for(p, shape, field);
  }

  e = find(p, shape, &created);
  if (!e) {
    /* The table is full; the general program still draws it right. */
    *is_uber = 1;
    p->uber_batches++;
    return general_for(p, shape, field);
  }
  if (created) {
    record_shape(p, shape);
    if (p->mode == GC_GL_SHADERS_SPECIALISED) build_now(p, e, field, 0);
    else enqueue(p, e);
  }
  if (SDL_AtomicGet(&e->state) == PIPELINE_READY) {
    p->specialised_batches++;
    return e->program;
  }
  /* Hybrid, and the pipeline's own program is not ready yet; or it failed
   * to build. */
  *is_uber = 1;
  p->uber_batches++;
  return general_for(p, shape, field);
}

void gc_gl_programs_report(GcGlPrograms *p, FILE *out) {
  static const char *MODE[3] = {"specialised", "uber", "hybrid"};
  if (!p || !out) return;
  /* The thread's counters are only read once it has stopped. */
  stop_thread(p);
  fprintf(out,
          "gl: stats programs mode=%s pipelines=%u compiled_on_draw_thread=%u loaded_on_draw_thread=%u "
          "compiled_on_thread=%u loaded_on_thread=%u disk_written=%u disk_rejected=%u failures=%u "
          "startup_ms=%.1f draw_thread_compile_ms=%.1f worst_field_compile_ms=%.1f worst_field=%llu "
          "fields_with_compiles=%u thread_compile_ms=%.1f worst_wait_for_specialised_ms=%.1f "
          "uber_batches=%llu specialised_batches=%llu parallel_compile=%d uber_early_batches=%llu\n",
          MODE[p->mode], p->count, p->compiled_sync, p->from_disk_sync, p->compiled_async, p->from_disk_async,
          p->disk_written, p->disk_rejected, p->failures, p->startup_ms, p->sync_ms, p->worst_field_ms,
          (unsigned long long)p->worst_field, p->fields_with_compiles, p->async_ms, p->worst_wait_ms,
          (unsigned long long)p->uber_batches, (unsigned long long)p->specialised_batches, p->parallel,
          (unsigned long long)p->uber_early_batches);
}

#endif /* GC_HAVE_SDL2 */
