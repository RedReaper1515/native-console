/*
 * The RDP: the Reality Display Processor's command interface (the DPC
 * registers at 0x04100000), its command reader, and the reference
 * software rasteriser behind it - triangles and rectangles, TMEM and the
 * texture unit, the colour combiner, the blender, depth, coverage and
 * dither - writing into RDRAM exactly as the hardware does.
 *
 * Like the RSP this is a library behind a seam: the machine owns an
 * N64Rdp, links it to RDRAM and to the RSP's DMEM (the XBUS command
 * source), and supplies the interrupt callback. Tests drive it with
 * n64_rdp_execute() and inspect RDRAM.
 *
 * Seam contract (docs/N64-PLAN.md, "The RDP"):
 *
 *  - RDRAM is big-endian bytes. Colour images, depth images and textures
 *    are read and written there with the hardware's byte order. The ninth
 *    ("hidden") bit of each RDRAM byte pair that 16-bit framebuffers keep
 *    coverage in is owned by the RDP: n64_rdp_init allocates it and
 *    n64_rdp_free releases it. It is rdram_size / 2 bytes, one per 16-bit
 *    RDRAM word (index = byte address / 2), each holding that word's two
 *    hidden bits in bits 0-1; power-on (init and reset) sets every byte to
 *    3. A save state must save these bytes along with RDRAM.
 *  - RDP addresses are 24 bits. Bytes beyond rdram_size read as zero and
 *    are not written. Pixels at x >= the colour image's width are not
 *    drawn (as in parallel-rdp, which the rasteriser follows).
 *  - Commands run synchronously: a write to DPC_END (when the RDP is not
 *    frozen) reads and executes every whole command between DPC_CURRENT
 *    and DPC_END - from RDRAM, or from DMEM when XBUS is set (the DMEM
 *    address is DPC_CURRENT & 0xFF8, so lists wrap within its 4 KiB) - and
 *    a partial command at the end stays buffered (in N64RdpState) for the
 *    next END write, also across a new START. Clearing FREEZE runs what
 *    arrived while frozen. After a run the busy bits are clear and
 *    CBUF_READY is set; the counters stay 0 until cleared. FULL_SYNC
 *    raises the DP interrupt through `interrupt` (level 1). Rendering is
 *    deterministic and bit-exact: the same commands and memory always give
 *    the same bytes, whatever the host. The rasteriser is a C port of
 *    parallel-rdp's (MIT) shaders; see runtimes/runtime-n64/src/n64_rdp*.c. Helper
 *    threads may draw parts of a primitive (n64_rdp_set_threads), but
 *    each command is complete before the next is read, and all of them
 *    before the register write returns: nothing is drawn afterwards.
 *  - N64RdpState (n64_rdp_state.h) is plain data: a save state copies it
 *    and the register fields as bytes. `links` holds the only host
 *    pointers besides `state`; whatever restores an N64Rdp keeps them.
 *  - Register numbers: 0 START, 1 END, 2 CURRENT, 3 STATUS, 4 CLOCK,
 *    5 BUFBUSY, 6 PIPEBUSY, 7 TMEM (CPU address 0x04100000 + 4 * reg; RSP
 *    COP0 registers 8-15).
 */
#ifndef N64_RDP_H
#define N64_RDP_H

#include "n64_common.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    N64_DPC_REG_START = 0,
    N64_DPC_REG_END = 1,
    N64_DPC_REG_CURRENT = 2,
    N64_DPC_REG_STATUS = 3,
    N64_DPC_REG_CLOCK = 4,
    N64_DPC_REG_BUFBUSY = 5,
    N64_DPC_REG_PIPEBUSY = 6,
    N64_DPC_REG_TMEM = 7,
};

/* DPC_STATUS read bits. */
#define N64_DPC_STATUS_XBUS 0x001u
#define N64_DPC_STATUS_FREEZE 0x002u
#define N64_DPC_STATUS_FLUSH 0x004u
#define N64_DPC_STATUS_START_GCLK 0x008u
#define N64_DPC_STATUS_TMEM_BUSY 0x010u
#define N64_DPC_STATUS_PIPE_BUSY 0x020u
#define N64_DPC_STATUS_CMD_BUSY 0x040u
#define N64_DPC_STATUS_CBUF_READY 0x080u
#define N64_DPC_STATUS_DMA_BUSY 0x100u
#define N64_DPC_STATUS_END_VALID 0x200u
#define N64_DPC_STATUS_START_VALID 0x400u

typedef struct N64RdpLinks {
    uint8_t *rdram;      /* big-endian RDRAM */
    uint32_t rdram_size; /* bytes */
    const uint8_t *dmem; /* the RSP's 4 KiB DMEM, for XBUS command lists */
    uint8_t *hidden;     /* hidden RDRAM bits, owned by the RDP (n64_rdp_init) */
    void *load_plans;    /* remembered TMEM load layouts, owned by the RDP; never saved */
    void *larger;        /* the larger picture (n64_rdp_set_scale), owned by the RDP; never saved */
    void *user;
    void (*interrupt)(void *user, int level);
} N64RdpLinks;

/* The rasteriser's own state (TMEM, tiles, modes, colours, images, the
 * command buffer) lives in N64RdpState, defined in n64_rdp_state.h. */
struct N64RdpState;

typedef struct N64Rdp {
    uint32_t start, end, current, status;
    uint32_t clock, bufbusy, pipebusy, tmem;
    struct N64RdpState *state; /* allocated by n64_rdp_init */
    N64RdpLinks links;
} N64Rdp;

/* Allocates the rasteriser state and the hidden bits; returns false when
 * out of memory. rdram_size is 4 or 8 MiB. */
bool n64_rdp_init(N64Rdp *rdp, uint8_t *rdram, uint32_t rdram_size, const uint8_t *dmem);
void n64_rdp_free(N64Rdp *rdp);
/* Power-on state: registers, TMEM and modes cleared. Keeps links. */
void n64_rdp_reset(N64Rdp *rdp);

uint32_t n64_rdp_reg_read(N64Rdp *rdp, unsigned reg);
void n64_rdp_reg_write(N64Rdp *rdp, unsigned reg, uint32_t value);

/* Execute a command list given as 64-bit words in host order (tests, and
 * the reader behind DPC_END). A partial trailing command is ignored. */
void n64_rdp_execute(N64Rdp *rdp, const uint64_t *words, size_t count);

/* How many threads the rasteriser may draw a primitive with, the calling
 * thread included: 1 draws everything on the caller, 0 restores the default
 * (the N64_RDP_THREADS environment variable, or half the host's processors
 * up to eight). The helper threads belong to the process, not to one
 * N64Rdp, and work only inside a call that draws: the bytes drawn are the
 * same for every count. Two of them wait for the next primitive by
 * spinning for a while (N64_RDP_SPIN_US microseconds, 150 by default; 0
 * turns that off) before they sleep. */
void n64_rdp_set_threads(int threads);

/*
 * Internal resolution: 1 (the console's own picture and nothing more), 2
 * or 4. Above 1 every primitive is also drawn that many times as large
 * each way, as parallel-rdp does it: into a shadow of RDRAM with scale *
 * scale samples for each byte the console has, at the same address. RDRAM
 * itself, the hidden bits and everything a game can read are drawn exactly
 * as before; the shadow is for the display (n64_vi.c). Returns false, and
 * stays at 1, when there is no memory for it (scale * scale times RDRAM
 * and a half, and RDRAM once more).
 *
 * A pixel of RDRAM whose bytes are not what the RDP last drew there (the
 * game wrote them itself) has no larger picture, until the RDP draws over
 * it again: n64_rdp_larger_pixel() says which, and where the samples are.
 */
bool n64_rdp_set_scale(N64Rdp *rdp, int scale);
int n64_rdp_scale(const N64Rdp *rdp);
/* The larger picture of the `bytes` bytes of RDRAM at `addr` (a pixel: 2 or
 * 4), or NULL when RDRAM does not hold what the RDP drew there, or there is
 * no larger picture. Otherwise sample (sx, sy) of the pixel, each 0 to
 * scale - 1, is the same bytes at the returned pointer + (sy * scale + sx)
 * * *stride. */
const uint8_t *n64_rdp_larger_pixel(const N64Rdp *rdp, uint32_t addr, uint32_t bytes, size_t *stride);
/* The same for whoever reads many pixels (n64_vi.c): the shadow itself.
 * Sample (sx, sy) of the byte at `addr` is rdram[(sy * scale + sx) *
 * rdram_stride + addr], its hidden bits hidden[(sy * scale + sx) *
 * hidden_stride + (addr >> 1)], and both count only where `reference` has
 * the bytes RDRAM has. Returns false when there is no larger picture. */
typedef struct N64RdpLargerView {
    int log2;                 /* 1 or 2: the scale is 1 << log2 */
    const uint8_t *rdram, *hidden, *reference;
    size_t rdram_stride, hidden_stride;
} N64RdpLargerView;
bool n64_rdp_larger_view(const N64Rdp *rdp, N64RdpLargerView *view);
/* The same for a reader of bytes lo up to hi of RDRAM alone. The larger
 * picture is drawn some time after the console's (a complete picture on
 * another thread, while the game runs on); a view is given when everything
 * that touches what it is of has been drawn, so this one does not wait for
 * a picture still being drawn somewhere else in RDRAM - the next frame,
 * while the display reads the last. */
bool n64_rdp_larger_view_of(const N64Rdp *rdp, N64RdpLargerView *view, uint32_t lo, uint32_t hi);

/* Nonzero draws every pixel through the pixel-by-pixel reference path
 * instead of the faster ones that give the same bytes (also
 * N64_RDP_REFERENCE=1 in the environment). For tests. */
void n64_rdp_set_reference(int reference);

#ifdef __cplusplus
}
#endif

#endif /* N64_RDP_H */
