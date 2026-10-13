/*
 * The OpenGL path's private parts: just enough of the OpenGL API, and the
 * state gc_gl.c (drawing), gc_gl_shader.c (the program text) and
 * gc_gl_programs.c (compiling, caching, the compile thread) share.
 *
 * Only compiled in when SDL2 is present; see gc_gl.h.
 */
#ifndef GC_GL_INTERNAL_H
#define GC_GL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "gc.h"
#include "gc_gx_tev_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Just enough OpenGL                                                  */

/* Declaring the entry points this needs, rather than taking a loader as a
 * dependency. */
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef double GLdouble;
typedef void *GLsync;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLES 0x0004
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_BLEND 0x0BE2
#define GL_DITHER 0x0BD0
#define GL_DEPTH_CLAMP 0x864F
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC1_COLOR 0x88F9
#define GL_ONE_MINUS_SRC1_COLOR 0x88FA
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_COLOR_LOGIC_OP 0x0BF2
#define GL_CLEAR 0x1500
#define GL_CLIP_DISTANCE0 0x3000
#define GL_EXTENSIONS 0x1F03
#define GL_NUM_EXTENSIONS 0x821D
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RGBA 0x1908
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_RGBA8 0x8058
#define GL_BGRA 0x80E1
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_MAX_LOD 0x813B
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_TEXTURE0 0x84C0
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RGBA32UI 0x8D70
#define GL_RGBA8UI 0x8D7C
#define GL_RGBA_INTEGER 0x8D99
#define GL_INVALID_INDEX 0xFFFFFFFFu
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_QUERY_RESULT 0x8866
#define GL_TIME_ELAPSED 0x88BF
#define GL_TIMESTAMP 0x8E28
#define GL_MAX_TEXTURE_BUFFER_SIZE 0x8C2B
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_READ 0x88E1
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_PERSISTENT_BIT 0x0040
#define GL_CLIENT_STORAGE_BIT 0x0200
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x8217
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
#define GL_WAIT_FAILED 0x911D
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_ALREADY_SIGNALED 0x911A
#define GL_CONDITION_SATISFIED 0x911C

#define GC_GL_FUNCTIONS(X)                                                                   \
  X(void, Enable, (GLenum))                                                                  \
  X(void, Disable, (GLenum))                                                                 \
  X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                        \
  X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                         \
  X(void, DepthFunc, (GLenum))                                                               \
  X(void, DepthMask, (GLboolean))                                                            \
  X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                           \
  X(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))                            \
  X(void, PixelStorei, (GLenum, GLint))                                                      \
  X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))              \
  X(void, Finish, (void))                                                                    \
  X(const GLubyte *, GetString, (GLenum))                                                    \
  X(void, GenTextures, (GLsizei, GLuint *))                                                  \
  X(void, DeleteTextures, (GLsizei, const GLuint *))                                         \
  X(void, BindTexture, (GLenum, GLuint))                                                     \
  X(void, TexParameteri, (GLenum, GLenum, GLint))                                            \
  X(void, TexImage2D,                                                                        \
    (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))           \
  X(void, TexSubImage2D,                                                                     \
    (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))           \
  X(void, CopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))  \
  X(void, TexBuffer, (GLenum, GLenum, GLuint))                                               \
  X(void, GenFramebuffers, (GLsizei, GLuint *))                                              \
  X(void, DeleteFramebuffers, (GLsizei, const GLuint *))                                     \
  X(void, BindFramebuffer, (GLenum, GLuint))                                                 \
  X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                     \
  X(GLenum, CheckFramebufferStatus, (GLenum))                                                \
  X(void, GenVertexArrays, (GLsizei, GLuint *))                                              \
  X(void, DeleteVertexArrays, (GLsizei, const GLuint *))                                     \
  X(void, BindVertexArray, (GLuint))                                                         \
  X(void, GenBuffers, (GLsizei, GLuint *))                                                   \
  X(void, DeleteBuffers, (GLsizei, const GLuint *))                                          \
  X(void, BindBuffer, (GLenum, GLuint))                                                      \
  X(void, BindBufferBase, (GLenum, GLuint, GLuint))                                          \
  X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum))                            \
  X(GLuint, CreateShader, (GLenum))                                                          \
  X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))             \
  X(void, CompileShader, (GLuint))                                                           \
  X(void, GetShaderiv, (GLuint, GLenum, GLint *))                                            \
  X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                          \
  X(void, DeleteShader, (GLuint))                                                            \
  X(GLuint, CreateProgram, (void))                                                           \
  X(void, AttachShader, (GLuint, GLuint))                                                    \
  X(void, DetachShader, (GLuint, GLuint))                                                    \
  X(void, LinkProgram, (GLuint))                                                             \
  X(void, GetProgramiv, (GLuint, GLenum, GLint *))                                           \
  X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                         \
  X(void, GetProgramBinary, (GLuint, GLsizei, GLsizei *, GLenum *, void *))                  \
  X(void, ProgramBinary, (GLuint, GLenum, const void *, GLsizei))                            \
  X(void, DeleteProgram, (GLuint))                                                           \
  X(void, UseProgram, (GLuint))                                                              \
  X(GLint, GetUniformLocation, (GLuint, const GLchar *))                                     \
  X(GLuint, GetUniformBlockIndex, (GLuint, const GLchar *))                                  \
  X(void, UniformBlockBinding, (GLuint, GLuint, GLuint))                                     \
  X(void, Uniform1i, (GLint, GLint))                                                         \
  X(void, ActiveTexture, (GLenum))                                                           \
  X(void, GetIntegerv, (GLenum, GLint *))                                                    \
  X(GLsync, FenceSync, (GLenum, GLbitfield))                                                 \
  X(GLenum, ClientWaitSync, (GLsync, GLbitfield, uint64_t))                                  \
  X(void, DeleteSync, (GLsync))                                                              \
  X(void *, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))                      \
  X(GLboolean, UnmapBuffer, (GLenum))                                                        \
  X(void, Clear, (GLbitfield))                                                               \
  X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                  \
  X(void, ClearDepth, (GLdouble))                                                            \
  X(void, Flush, (void))                                                                     \
  X(void, GetBufferSubData, (GLenum, GLintptr, GLsizeiptr, void *))                          \
  X(void, GetTexImage, (GLenum, GLint, GLenum, GLenum, void *))                               \
  X(void, GenQueries, (GLsizei, GLuint *))                                                   \
  X(void, DeleteQueries, (GLsizei, const GLuint *))                                          \
  X(void, QueryCounter, (GLuint, GLenum))                                                    \
  X(void, DeleteQueries, (GLsizei, const GLuint *))                                          \
  X(void, BeginQuery, (GLenum, GLuint))                                                      \
  X(void, EndQuery, (GLenum))                                                                \
  X(void, GetQueryObjectiv, (GLuint, GLenum, GLint *))                                       \
  X(void, GetQueryObjectui64v, (GLuint, GLenum, uint64_t *))                                 \
  X(GLenum, GetError, (void))                                                                \
  X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                               \
  X(void, BlendEquationSeparate, (GLenum, GLenum))                                           \
  X(void, LogicOp, (GLenum))                                                                 \
  X(const GLubyte *, GetStringi, (GLenum, GLuint))                                           \
  X(void, BlitFramebuffer,                                                                   \
    (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))            \
  X(void, GenSamplers, (GLsizei, GLuint *))                                                  \
  X(void, DeleteSamplers, (GLsizei, const GLuint *))                                         \
  X(void, BindSampler, (GLuint, GLuint))                                                     \
  X(void, SamplerParameteri, (GLuint, GLenum, GLint))                                        \
  X(void, SamplerParameterf, (GLuint, GLenum, GLfloat))

#define GC_GL_DECLARE(ret, name, args) extern ret(*gl##name) args;
GC_GL_FUNCTIONS(GC_GL_DECLARE)
#undef GC_GL_DECLARE

/* ------------------------------------------------------------------ */
/* The pipeline, as the shader sees it                                 */

/* The words that change the *text* of a specialised program, and that the
 * one general ("uber") program reads from a uniform block instead. Layout:
 *
 *   0          number of TEV stages
 *   1 + 4n     stage n: colour combiner register (0xc0 + 2n)
 *   2 + 4n     stage n: alpha combiner register (0xc1 + 2n)
 *   3 + 4n     stage n: order - map (2:0), coordinate (5:3), textured (6)
 *   4 + 4n     stage n: indirect command (0x10 + n), zero when it cannot matter
 *   65, 66, 67 indirect scale registers 0x25, 0x26 and indirect order 0x27
 *   68         the four swap tables, two bits a channel
 *   69         alpha compare (0xf3)
 *   70         fog: function select (2:0), orthographic (3), range adjust (4)
 *
 * Everything else a pixel reads - colours, konstants, fog constants, the
 * indirect matrices, texture parameters, the blend and depth registers -
 * is a uniform in both kinds of program. */
#define GC_GL_SHAPE_WORDS 80

/* How many texels of the triangle buffer one triangle occupies. */
#define GC_GL_RECORD 25

/* The uniform block, laid out as std140 lays out arrays of vec4s: every
 * member here is a multiple of four 32-bit words, so C and GLSL agree on
 * every offset without padding. */
typedef struct GcGlDraw {
  uint32_t shape[GC_GL_SHAPE_WORDS]; /* uvec4 cfg[20] */
  int32_t tev_register[16];          /* ivec4 tev_reg[4] */
  int32_t konstant[64];              /* ivec4 konst[16] */
  uint32_t fog[8];                   /* uvec4 fog0, fog1 */
  uint32_t fog_table[12];            /* uvec4 fog_table[3] */
  uint32_t indirect_matrix[12];      /* uvec4 ind_matrix[3] */
  int32_t tex_size[32];              /* ivec4 tex_size[8]: width, height, top level, 0 */
  int32_t tex_mode[32];              /* ivec4 tex_mode[8]: wrap s, wrap t, magnify linear, minify linear */
  int32_t tex_lod[32];               /* ivec4 tex_lod[8]: mip mode, min lod, max lod, diagonal */
  int32_t tex_bias[32];              /* ivec4 tex_bias[8]: lod bias */
  uint32_t coord_size[32];           /* uvec4 coord_size[8]: scale registers 0x30 + 2n, 0x31 + 2n */
  uint32_t pixel_engine[8];          /* uvec4 pe[2]: blend, dst alpha, pe control, z mode; needs dst */
  uint32_t scale_bits[128];          /* uvec4 scale_bits[32]: the float bits of every coordinate's
                                        scale to every map, s then t, at [coordinate * 16 + map * 2] */
} GcGlDraw;

/* The program text for a pipeline. Returns 0, or -1 when it did not fit. */
int gc_gl_fragment_source(const uint32_t *shape, char *buffer, size_t size);
/* ... and for the general program, which reads the pipeline from a uniform
 * block. The fast path has two: with `early`, the one whose depth test the
 * card runs before the program, for pipelines with the console's "depth
 * before texture" set (shape[72]). Everywhere else `early` changes nothing. */
int gc_gl_general_source(int early, char *buffer, size_t size);
extern const char *gc_gl_vertex_source;
/* Fills gc_gl_vertex_source in once gc_gl_unpack_double is known. */
void gc_gl_build_vertex_source(void);
extern const char *gc_gl_unpack_double;
/* Nonzero: both texts are the fast path's (gc_gl.h). Set once, before the
 * first program is built, and part of every program's cache key through
 * the text it changes. */
extern int gc_gl_fast;
/* ... and the driver can run the depth test before a program that discards
 * (ARB_shader_image_load_store's early_fragment_tests): what a console's
 * "depth before texture" asks for. Found once the context exists. */
extern int gc_gl_fast_early;
/* Nonzero: the card filters the fast path's textures (gc_gl_shader.c,
 * sample_with_lod). The fast path's way unless GC_GL_CARD_FILTER=0, which
 * keeps the exact path's fetch-and-filter in the program instead, to
 * measure one against the other. Set with gc_gl_fast. */
extern int gc_gl_card_filter;
/* How many samples the card may take along a slanted texture's longer
 * axis: 1 (the reference's filtering), or 2 to GC_GL_ANISOTROPY_MAX.
 * Needs gc_gl_card_filter and the card's extension; the render thread puts
 * it back to 1 when the card has none. */
#define GC_GL_ANISOTROPY_MAX 16
extern int gc_gl_anisotropy;
/* The fast path binds each map a second time, on these units, with the
 * sampler that minifies (gc_gl_shader.c). Clear of the eight maps, the
 * destination, the triangles and the scratch unit. */
#define GC_GL_UNIT_MINIFY 16
/* How many times the console's own size the fast path draws at, each way:
 * 1 to GC_GL_SCALE_MAX. Always 1 on the exact path. Set with gc_gl_fast. */
#define GC_GL_SCALE_MAX 4
extern int gc_gl_scale;

/* ------------------------------------------------------------------ */
/* The render thread                                                   */

/* Every OpenGL call the path makes after start-up happens on one thread of
 * its own (gc_gl_render.c), which owns the drawing context. The machine's
 * thread (gc_gl.c) works out what to draw - triangles set up as the
 * reference sets them up, the pipeline's uniform block, textures decoded -
 * and hands it over as commands, in order; it waits only when it needs an
 * answer (the framebuffer back in memory) or when a queue too far ahead has
 * to let the card catch up. A driver that stops the calling thread to
 * catch up with itself (NVIDIA's did, for 1.5 ms a copy headless and 12 ms
 * windowed) stops that thread and not the machine. */

typedef enum GcGlShaderMode {
  GC_GL_SHADERS_SPECIALISED = 0,
  GC_GL_SHADERS_UBER = 1,
  GC_GL_SHADERS_HYBRID = 2
} GcGlShaderMode;

/* The texture cache's size: slots the machine's thread fills and the render
 * thread keeps a texture for each of. */
#define GC_GL_TEXTURES 1024u

enum {
  GC_GL_CMD_STATE = 1,   /* GcGlStateCommand: the register state a batch draws under */
  GC_GL_CMD_DRAW,        /* GcGlDrawCommand, then the triangles, then their records */
  GC_GL_CMD_TEXTURE,     /* GcGlTextureCommand, then the texels of every level */
  GC_GL_CMD_UPLOAD,      /* the colour plane, then the depth plane, as the machine holds them */
  GC_GL_CMD_DOWNLOAD,    /* GcGlDownloadCommand */
  GC_GL_CMD_COPY,        /* GcGlCopyCommand */
  GC_GL_CMD_CLEAR,       /* GcGlClearCommand */
  GC_GL_CMD_SYNC,        /* uint64_t: the sequence number the machine's thread waits for */
  GC_GL_CMD_COPY_TEXTURE /* GcGlCopyTextureCommand: a slot takes the texture the card made of a copy */
};

typedef struct GcGlStateCommand {
  GcGlDraw draw;
  uint32_t shape[GC_GL_SHAPE_WORDS];
  uint32_t z_mode, blend_mode, needs_dst, maps;
  uint32_t slot[8]; /* the texture slot bound to each map the pipeline reads */
  uint64_t field;
} GcGlStateCommand;

typedef struct GcGlDrawCommand {
  uint32_t count;
  uint32_t needs_dst;
} GcGlDrawCommand;

typedef struct GcGlTextureCommand {
  uint32_t slot, top, side_by_side;
  uint32_t level_width[16], level_height[16];
  uint32_t bytes; /* of texels that follow: RGBA8 of every level, level 0 first */
} GcGlTextureCommand;

typedef struct GcGlDownloadCommand {
  uint32_t *color, *depth; /* the machine's planes; its thread waits while they are written */
  /* One pixel of one plane (a peek), or the whole of both when `pixel` is 0. */
  uint32_t pixel, x, y, depth_plane;
} GcGlDownloadCommand;

typedef struct GcGlClearCommand {
  uint32_t left, top, across, down, colour, depth;
} GcGlClearCommand;

struct GcEfbCopyJob;
typedef struct GcGlCopyCommand {
  struct GcEfbCopyJob *job; /* heap-allocated by the machine's thread; the render side frees it */
  uint32_t first_row, rows, depth;
  uint32_t held_first, held_end;
  uint64_t tag;    /* the hold on [held_first, held_end) (gc_memory_hold_tagged) */
  double asked_ms; /* when the machine asked, for the copies' latency */
  /* The fast path: the texture format the card is to keep the copy in as
   * well (gc_efb_copy_texture_format), or -1 for none. */
  int32_t texture_format;
} GcGlCopyCommand;

/* The texture the card made of the copy held under `tag`, into a slot of the
 * texture cache: a draw then samples the copy without its bytes ever having
 * come back from the card. */
typedef struct GcGlCopyTextureCommand {
  uint32_t slot;
  uint64_t tag;
} GcGlCopyTextureCommand;

typedef struct GcGlRender GcGlRender;

typedef struct GcGlRenderSetup {
  GcMachine *m;
  void *window, *context;                 /* the drawing context: made on the machine's thread, released */
  void *compile_window, *compile_context; /* for hybrid mode's compile thread, or NULL */
  void *readback_window, *readback_context; /* for copies, or NULL */
  GcGlShaderMode mode;
  const char *cache_dir, *id;
  int boxes;
} GcGlRenderSetup;

/* Makes the drawing context current on a thread of its own and brings the
 * path up there. NULL on failure, with the reason on the machine's log. */
GcGlRender *gc_gl_render_start(const GcGlRenderSetup *setup);
/* Room for a command of `size` payload bytes: filled, then sent. One at a
 * time, from the machine's thread. */
void *gc_gl_render_command(GcGlRender *r, unsigned kind, size_t size);
/* Whether the render thread can make a texture of a copy on the card (the
 * fast path with the card filtering, and its program built). */
int gc_gl_render_copy_textures(const GcGlRender *r);
/* One pixel of the framebuffer as it was at the display copy `tag`, which
 * was issued with a wish for that plane (GcEfbCopyJob.snapshot): waited for
 * if the copy has not got there yet. 0 when it never will (the picture has
 * been overwritten by newer ones, or half a second has gone by). From the
 * machine's thread. */
int gc_gl_render_snapshot_pixel(GcGlRender *r, uint64_t tag, int plane, uint32_t x, uint32_t y, uint32_t *value);
void gc_gl_render_send(GcGlRender *r);
/* Waits until every command sent so far has run. */
void gc_gl_render_wait(GcGlRender *r);
/* ... and every copy sent so far has reached main memory. */
void gc_gl_render_settle(GcGlRender *r);
/* A copy was just sent. After a display copy: wait while more than `pending`
 * display copies sent have not reached main memory. */
void gc_gl_render_limit_copies(GcGlRender *r, unsigned pending, int display);
/* Whether copies go to main memory on a thread of their own (holds); when
 * not, the machine's thread makes them from the framebuffer brought back. */
int gc_gl_render_async_copies(const GcGlRender *r);
/* Triangles a draw can hold. */
unsigned gc_gl_render_capacity(const GcGlRender *r);
const char *gc_gl_render_name(const GcGlRender *r);
/* Runs what is left, prints the render thread's statistics, takes the path
 * down on its thread and joins it. The contexts are then released and the
 * caller deletes them. */
void gc_gl_render_stop(GcGlRender *r, FILE *log);

/* The picture of the display buffer that starts at `display_first` in main
 * memory and shows `display_width` by `display_height` of it, as large as
 * it was drawn - when the copy that filled that buffer was made here at a
 * scale. Returns 0 with its size and a serial that changes with the
 * picture; with `out` null that is all, otherwise the pixels too, when
 * they fit `pitch` by `rows`. -1 when there is no such picture, or it does
 * not fit. From any thread. */
int gc_gl_render_picture(GcGlRender *r, uint32_t display_first, unsigned display_width, unsigned display_height,
                         uint32_t *out, unsigned pitch, unsigned rows, unsigned *width_out, unsigned *height_out,
                         uint64_t *serial_out);
/* Main memory was replaced under them (a state loaded): none is of
 * anything now. */
void gc_gl_render_forget_pictures(GcGlRender *r);

int gc_gl_load_functions(FILE *log);

/* ------------------------------------------------------------------ */
/* Programs                                                            */


typedef struct GcGlPrograms GcGlPrograms;

/* Everything about compiling is kept apart from drawing; these are the
 * calls drawing makes. */
/* compile_window and compile_context: a context sharing objects with the
 * drawing one, for hybrid mode's compile thread (NULL: none, and hybrid
 * compiles on the drawing thread). The caller keeps and deletes them. */
GcGlPrograms *gc_gl_programs_create(GcMachine *m, GcGlShaderMode mode, const char *cache_dir,
                                    const char *id, void *compile_window, void *compile_context);
void gc_gl_programs_destroy(GcGlPrograms *p);

/* The program to draw the given shape with, and whether it is the general
 * one. Never 0 unless nothing at all could be compiled. `field` is the
 * machine's field, for attributing compile time. */
GLuint gc_gl_programs_get(GcGlPrograms *p, const uint32_t *shape, uint64_t field, int *is_uber);

/* Statistics, one line. */
void gc_gl_programs_report(GcGlPrograms *p, FILE *out);

/* A 64-bit hash over bytes, used for textures, shapes and cache keys. */
uint64_t gc_gl_hash(const void *data, size_t length, uint64_t seed);

double gc_gl_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* GC_GL_INTERNAL_H */
