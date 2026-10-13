/*
 * High-level BIOS.
 *
 * Without a BIOS image the runtime installs a small one of its own: the
 * exception vectors and the IRQ dispatcher at the same addresses as the
 * Nintendo BIOS (so BIOS open-bus reads return the same opcodes), and a SWI
 * vector that the run loop intercepts to perform the call in C.
 *
 * The functions follow the GBATEK descriptions of the BIOS calls. The
 * sound-driver calls (0x1A-0x1E, 0x20-0x24, 0x28-0x2A) are not implemented;
 * games that use the SDK's m4a library carry their own driver in ROM.
 */
#include <math.h>
#include <string.h>

#include "gba.h"
#include "gba_bus.h"

#define BIOS_LATCH_AFTER_SWI 0xE3A02004u

/* ------------------------------------------------------------------ */
/* Image                                                               */
/* ------------------------------------------------------------------ */

static void put32(GbaMachine *m, uint32_t addr, uint32_t value)
{
    m->bios[addr + 0] = (uint8_t)value;
    m->bios[addr + 1] = (uint8_t)(value >> 8);
    m->bios[addr + 2] = (uint8_t)(value >> 16);
    m->bios[addr + 3] = (uint8_t)(value >> 24);
}

/* B from `at` to `target` (ARM). */
static uint32_t arm_b(uint32_t at, uint32_t target)
{
    return 0xEA000000u | (((target - (at + 8u)) >> 2) & 0x00FFFFFFu);
}

void gba_bios_install_hle(GbaMachine *m)
{
    memset(m->bios, 0, sizeof m->bios);
    put32(m, 0x00, arm_b(0x00, 0x00));  /* reset: never executed (direct boot) */
    put32(m, 0x04, arm_b(0x04, 0x150)); /* undefined instruction */
    put32(m, 0x08, arm_b(0x08, 0x150)); /* SWI: intercepted before it runs */
    put32(m, 0x0C, arm_b(0x0C, 0x150)); /* prefetch abort */
    put32(m, 0x10, arm_b(0x10, 0x150)); /* data abort */
    put32(m, 0x18, arm_b(0x18, 0x128)); /* IRQ */
    put32(m, 0x1C, arm_b(0x1C, 0x150)); /* FIQ */

    /* The IRQ dispatcher, instruction for instruction as in the Nintendo
     * BIOS: save the scratch registers, call the handler whose address the
     * game stored at 0x03007FFC (reached through the 0x03FFFFFC mirror),
     * restore and return. */
    put32(m, 0x128, 0xE92D500Fu); /* stmfd sp!, {r0-r3, r12, lr} */
    put32(m, 0x12C, 0xE3A00301u); /* mov   r0, #0x04000000 */
    put32(m, 0x130, 0xE28FE000u); /* add   lr, pc, #0 */
    put32(m, 0x134, 0xE510F004u); /* ldr   pc, [r0, #-4] */
    put32(m, 0x138, 0xE8BD500Fu); /* ldmfd sp!, {r0-r3, r12, lr} */
    put32(m, 0x13C, 0xE25EF004u); /* subs  pc, lr, #4 */

    /* The next two words are only ever prefetched, never executed: the
     * IRQ return leaves the opcode at 0x144 in the BIOS latch. */
    put32(m, 0x140, 0xE92D5800u); /* stmfd sp!, {r11, r12, lr} */
    put32(m, 0x144, 0xE55EC002u); /* ldrb  r12, [lr, #-2] */
    put32(m, 0x150, 0xE1B0F00Eu); /* exception fallback: movs pc, lr */
    put32(m, 0x188, 0xE1B0F00Eu); /* where the Nintendo SWI handler returns */
    put32(m, 0x190, BIOS_LATCH_AFTER_SWI);
    put32(m, 0xE4, 0xE129F000u);  /* opcode prefetched when the boot code exits */
}

/* ------------------------------------------------------------------ */
/* Memory helpers (go through the bus so I/O and timing apply)         */
/* ------------------------------------------------------------------ */

static inline uint32_t rd8(GbaMachine *m, uint32_t a) { return gba_bus_read8(&m->cpu, a, GBA_ACCESS_SEQ); }
static inline uint32_t rd16(GbaMachine *m, uint32_t a) { return gba_bus_read16(&m->cpu, a & ~1u, GBA_ACCESS_SEQ); }
static inline uint32_t rd32(GbaMachine *m, uint32_t a) { return gba_bus_read32(&m->cpu, a & ~3u, GBA_ACCESS_SEQ); }
static inline void wr8(GbaMachine *m, uint32_t a, uint32_t v) { gba_bus_write8(&m->cpu, a, (uint8_t)v, GBA_ACCESS_SEQ); }
static inline void wr16(GbaMachine *m, uint32_t a, uint32_t v) { gba_bus_write16(&m->cpu, a & ~1u, (uint16_t)v, GBA_ACCESS_SEQ); }
static inline void wr32(GbaMachine *m, uint32_t a, uint32_t v) { gba_bus_write32(&m->cpu, a & ~3u, v, GBA_ACCESS_SEQ); }

/* The BIOS refuses to read its own memory through its copy functions. */
static inline bool source_ok(uint32_t addr)
{
    return addr >= 0x02000000u;
}

/* ------------------------------------------------------------------ */
/* Math                                                                */
/* ------------------------------------------------------------------ */

static void swi_div(GbaCpu *c, int32_t num, int32_t den)
{
    if (den == 0) {
        /* The real BIOS never returns sensible values here; these are the
         * ones it leaves for the common cases. */
        c->r[0] = num < 0 ? 0xFFFFFFFFu : 1u;
        c->r[1] = (uint32_t)num;
        c->r[3] = 1;
        return;
    }
    if (num == INT32_MIN && den == -1) {
        c->r[0] = 0x80000000u;
        c->r[1] = 0;
        c->r[3] = 0x80000000u;
        return;
    }
    int32_t q = num / den;
    int32_t r = num % den;
    c->r[0] = (uint32_t)q;
    c->r[1] = (uint32_t)r;
    c->r[3] = q < 0 ? (uint32_t)-(int64_t)q : (uint32_t)q;
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t res = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

static inline int32_t asr(int64_t v, int s)
{
    return (int32_t)(v >> s); /* arithmetic on every supported compiler */
}

/* ArcTan polynomial from GBATEK; returns r0 and leaves r1, r3 as the BIOS. */
static int32_t arctan(GbaCpu *c, int32_t x)
{
    int32_t a = -asr((int64_t)x * x, 14);
    int32_t b = asr((int64_t)0xA9 * a, 14) + 0x390;
    b = asr((int64_t)a * b, 14) + 0x91C;
    b = asr((int64_t)a * b, 14) + 0xFB6;
    b = asr((int64_t)a * b, 14) + 0x16AA;
    b = asr((int64_t)a * b, 14) + 0x2081;
    b = asr((int64_t)a * b, 14) + 0x3651;
    b = asr((int64_t)a * b, 14) + 0xA2F9;
    c->r[1] = (uint32_t)a;
    c->r[3] = (uint32_t)b;
    return asr((int64_t)x * b, 16);
}

static int32_t bios_div(int32_t num, int32_t den)
{
    if (den == 0) return num < 0 ? -1 : 1;
    if (num == INT32_MIN && den == -1) return INT32_MIN;
    return num / den;
}

static uint32_t arctan2(GbaCpu *c, int32_t x, int32_t y)
{
    if (y == 0) return x >= 0 ? 0u : 0x8000u;
    if (x == 0) return y >= 0 ? 0x4000u : 0xC000u;
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y) return (uint32_t)arctan(c, bios_div(y << 14, x)) & 0xFFFFu;
            return (uint32_t)(0x4000 - arctan(c, bios_div(x << 14, y))) & 0xFFFFu;
        }
        if (-x >= y) return (uint32_t)(0x8000 + arctan(c, bios_div(y << 14, x))) & 0xFFFFu;
        return (uint32_t)(0x4000 - arctan(c, bios_div(x << 14, y))) & 0xFFFFu;
    }
    if (x <= 0) {
        if (-x > -y) return (uint32_t)(0x8000 + arctan(c, bios_div(y << 14, x))) & 0xFFFFu;
        return (uint32_t)(0xC000 - arctan(c, bios_div(x << 14, y))) & 0xFFFFu;
    }
    if (x >= -y) return (uint32_t)(0x10000 + arctan(c, bios_div(y << 14, x))) & 0xFFFFu;
    return (uint32_t)(0xC000 - arctan(c, bios_div(x << 14, y))) & 0xFFFFu;
}

/* ------------------------------------------------------------------ */
/* Copies                                                              */
/* ------------------------------------------------------------------ */

static void cpu_set(GbaMachine *m)
{
    GbaCpu *c = &m->cpu;
    uint32_t src = c->r[0], dst = c->r[1], ctrl = c->r[2];
    uint32_t count = ctrl & 0x1FFFFFu;
    bool fill = (ctrl & (1u << 24)) != 0;
    bool word = (ctrl & (1u << 26)) != 0;
    if (!source_ok(src)) return;
    if (word) {
        src &= ~3u;
        dst &= ~3u;
        uint32_t v = fill ? rd32(m, src) : 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!fill) v = rd32(m, src + i * 4);
            wr32(m, dst + i * 4, v);
        }
    } else {
        src &= ~1u;
        dst &= ~1u;
        uint32_t v = fill ? rd16(m, src) : 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!fill) v = rd16(m, src + i * 2);
            wr16(m, dst + i * 2, v);
        }
    }
}

static void cpu_fast_set(GbaMachine *m)
{
    GbaCpu *c = &m->cpu;
    uint32_t src = c->r[0] & ~3u, dst = c->r[1] & ~3u, ctrl = c->r[2];
    uint32_t count = ((ctrl & 0x1FFFFFu) + 7u) & ~7u; /* whole blocks of 8 words */
    bool fill = (ctrl & (1u << 24)) != 0;
    if (!source_ok(src)) return;
    uint32_t v = fill ? rd32(m, src) : 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!fill) v = rd32(m, src + i * 4);
        wr32(m, dst + i * 4, v);
    }
}

/* ------------------------------------------------------------------ */
/* Decompression                                                       */
/* ------------------------------------------------------------------ */

/* Output stage shared by the *Wram (8-bit writes) and *Vram (16-bit
 * writes, VRAM cannot take bytes) variants. */
typedef struct {
    GbaMachine *m;
    uint32_t dst;
    bool vram;
    uint32_t pending;
    int pending_bytes;
} Out;

static void out_byte(Out *o, uint32_t b)
{
    if (!o->vram) {
        wr8(o->m, o->dst++, b);
        return;
    }
    o->pending |= (b & 0xFFu) << (8 * o->pending_bytes);
    if (++o->pending_bytes == 2) {
        wr16(o->m, o->dst, o->pending);
        o->dst += 2;
        o->pending = 0;
        o->pending_bytes = 0;
    }
}

static uint32_t out_peek_back(Out *o, uint32_t distance)
{
    /* Bytes written so far, including a half-filled VRAM halfword. */
    uint32_t pos = o->dst + (uint32_t)o->pending_bytes - distance;
    if (o->vram && pos >= o->dst) return (o->pending >> (8 * (pos - o->dst))) & 0xFFu;
    return rd8(o->m, pos);
}

static void lz77(GbaMachine *m, bool vram)
{
    uint32_t src = m->cpu.r[0];
    if (!source_ok(src)) return;
    uint32_t header = rd32(m, src);
    uint32_t size = header >> 8;
    src += 4;
    Out o = { m, m->cpu.r[1], vram, 0, 0 };
    uint32_t written = 0;
    while (written < size) {
        uint32_t flags = rd8(m, src++);
        for (int i = 0; i < 8 && written < size; i++, flags <<= 1) {
            if (flags & 0x80u) {
                uint32_t b0 = rd8(m, src++), b1 = rd8(m, src++);
                uint32_t len = (b0 >> 4) + 3u;
                uint32_t disp = (((b0 & 0xFu) << 8) | b1) + 1u;
                for (uint32_t k = 0; k < len && written < size; k++, written++) {
                    out_byte(&o, out_peek_back(&o, disp));
                }
            } else {
                out_byte(&o, rd8(m, src++));
                written++;
            }
        }
    }
    if (vram && o.pending_bytes) wr16(m, o.dst, o.pending);
}

static void rl(GbaMachine *m, bool vram)
{
    uint32_t src = m->cpu.r[0];
    if (!source_ok(src)) return;
    uint32_t size = rd32(m, src) >> 8;
    src += 4;
    Out o = { m, m->cpu.r[1], vram, 0, 0 };
    uint32_t written = 0;
    while (written < size) {
        uint32_t flag = rd8(m, src++);
        if (flag & 0x80u) {
            uint32_t len = (flag & 0x7Fu) + 3u;
            uint32_t b = rd8(m, src++);
            for (uint32_t k = 0; k < len && written < size; k++, written++) out_byte(&o, b);
        } else {
            uint32_t len = (flag & 0x7Fu) + 1u;
            for (uint32_t k = 0; k < len && written < size; k++, written++) out_byte(&o, rd8(m, src++));
        }
    }
    if (vram && o.pending_bytes) wr16(m, o.dst, o.pending);
}

static void huffman(GbaMachine *m)
{
    uint32_t src = m->cpu.r[0] & ~3u;
    uint32_t dst = m->cpu.r[1];
    if (!source_ok(src)) return;
    uint32_t header = rd32(m, src);
    uint32_t bits = header & 0xFu;
    uint32_t size = header >> 8;
    uint32_t tree = src + 4;
    uint32_t tree_size = (rd8(m, tree) + 1u) * 2u;
    uint32_t stream = tree + tree_size;
    uint32_t out = 0, out_bits = 0, written = 0;

    uint32_t node = tree + 1;
    while (written < size) {
        uint32_t word = rd32(m, stream);
        stream += 4;
        for (int i = 31; i >= 0 && written < size; i--) {
            uint32_t n = rd8(m, node);
            uint32_t next = (node & ~1u) + ((n & 0x3Fu) + 1u) * 2u;
            bool right = (word >> i) & 1u;
            bool leaf = right ? (n & 0x40u) : (n & 0x80u);
            node = next + (right ? 1u : 0u);
            if (leaf) {
                out |= rd8(m, node) << out_bits;
                out_bits += bits;
                node = tree + 1;
                if (out_bits == 32) {
                    wr32(m, dst, out);
                    dst += 4;
                    written += 4;
                    out = 0;
                    out_bits = 0;
                }
            }
        }
    }
}

static void bit_unpack(GbaMachine *m)
{
    GbaCpu *c = &m->cpu;
    uint32_t src = c->r[0], dst = c->r[1], info = c->r[2];
    if (!source_ok(src)) return;
    uint32_t len = rd16(m, info);
    uint32_t src_width = rd8(m, info + 2);
    uint32_t dst_width = rd8(m, info + 3);
    uint32_t offset_word = rd32(m, info + 4);
    uint32_t offset = offset_word & 0x7FFFFFFFu;
    bool zero_too = (offset_word & 0x80000000u) != 0;
    uint32_t out = 0, out_bits = 0;
    uint32_t src_mask = (1u << src_width) - 1u;

    for (uint32_t i = 0; i < len; i++) {
        uint32_t byte = rd8(m, src + i);
        for (uint32_t bit = 0; bit < 8; bit += src_width) {
            uint32_t v = (byte >> bit) & src_mask;
            if (v || zero_too) v += offset;
            out |= v << out_bits;
            out_bits += dst_width;
            if (out_bits >= 32) {
                wr32(m, dst, out);
                dst += 4;
                out = 0;
                out_bits = 0;
            }
        }
    }
}

static void diff_unfilter(GbaMachine *m, bool wide, bool vram)
{
    uint32_t src = m->cpu.r[0], dst = m->cpu.r[1];
    if (!source_ok(src)) return;
    uint32_t size = rd32(m, src) >> 8;
    src += 4;
    if (wide) {
        uint32_t acc = 0;
        for (uint32_t i = 0; i < size; i += 2) {
            acc = (acc + rd16(m, src + i)) & 0xFFFFu;
            wr16(m, dst + i, acc);
        }
        return;
    }
    Out o = { m, dst, vram, 0, 0 };
    uint32_t acc = 0;
    for (uint32_t i = 0; i < size; i++) {
        acc = (acc + rd8(m, src + i)) & 0xFFu;
        out_byte(&o, acc);
    }
    if (vram && o.pending_bytes) wr16(m, o.dst, o.pending);
}

/* ------------------------------------------------------------------ */
/* Affine setup                                                        */
/* ------------------------------------------------------------------ */

/* BIOS sine table: 256 steps per turn, 1.14 fixed point. */
static int16_t sin_lut(uint32_t angle)
{
    return (int16_t)lrint(sin((double)(angle & 0xFFu) * (3.14159265358979323846 * 2.0 / 256.0)) * 16384.0);
}

static void bg_affine_set(GbaMachine *m)
{
    GbaCpu *c = &m->cpu;
    uint32_t src = c->r[0], dst = c->r[1];
    for (uint32_t n = c->r[2]; n; n--, src += 20, dst += 16) {
        int32_t ox = (int32_t)rd32(m, src);
        int32_t oy = (int32_t)rd32(m, src + 4);
        int16_t cx = (int16_t)rd16(m, src + 8);
        int16_t cy = (int16_t)rd16(m, src + 10);
        int16_t sx = (int16_t)rd16(m, src + 12);
        int16_t sy = (int16_t)rd16(m, src + 14);
        uint32_t theta = rd16(m, src + 16) >> 8;
        int32_t sn = sin_lut(theta), cs = sin_lut(theta + 64);
        int16_t pa = (int16_t)asr((int64_t)sx * cs, 14);
        int16_t pb = (int16_t)-asr((int64_t)sx * sn, 14);
        int16_t pc = (int16_t)asr((int64_t)sy * sn, 14);
        int16_t pd = (int16_t)asr((int64_t)sy * cs, 14);
        wr16(m, dst, (uint16_t)pa);
        wr16(m, dst + 2, (uint16_t)pb);
        wr16(m, dst + 4, (uint16_t)pc);
        wr16(m, dst + 6, (uint16_t)pd);
        wr32(m, dst + 8, (uint32_t)(ox - (pa * cx + pb * cy)));
        wr32(m, dst + 12, (uint32_t)(oy - (pc * cx + pd * cy)));
    }
}

static void obj_affine_set(GbaMachine *m)
{
    GbaCpu *c = &m->cpu;
    uint32_t src = c->r[0], dst = c->r[1], stride = c->r[3];
    for (uint32_t n = c->r[2]; n; n--, src += 8) {
        int16_t sx = (int16_t)rd16(m, src);
        int16_t sy = (int16_t)rd16(m, src + 2);
        uint32_t theta = rd16(m, src + 4) >> 8;
        int32_t sn = sin_lut(theta), cs = sin_lut(theta + 64);
        wr16(m, dst, (uint16_t)asr((int64_t)sx * cs, 14));
        dst += stride;
        wr16(m, dst, (uint16_t)-asr((int64_t)sx * sn, 14));
        dst += stride;
        wr16(m, dst, (uint16_t)asr((int64_t)sy * sn, 14));
        dst += stride;
        wr16(m, dst, (uint16_t)asr((int64_t)sy * cs, 14));
        dst += stride;
    }
}

/* ------------------------------------------------------------------ */
/* Resets                                                              */
/* ------------------------------------------------------------------ */

static void register_ram_reset(GbaMachine *m, uint32_t flags)
{
    m->rc_ram_gen++; /* RAM changes below bypass the bus */
    if (flags & 0x01u) memset(m->ewram, 0, sizeof m->ewram);
    if (flags & 0x02u) memset(m->iwram, 0, GBA_IWRAM_SIZE - 0x200u);
    if (flags & 0x04u) memset(m->palette, 0, sizeof m->palette);
    if (flags & 0x08u) memset(m->vram, 0, sizeof m->vram);
    if (flags & 0x10u) memset(m->oam, 0, sizeof m->oam);
    if (flags & 0x20u) {
        for (uint32_t off = 0x120; off < 0x160; off += 2) gba_io_write16(m, off, 0, 0xFFFF);
        gba_io_write16(m, 0x134, 0x8000, 0xFFFF); /* RCNT: general-purpose mode */
    }
    if (flags & 0x40u) {
        for (uint32_t off = 0x60; off < 0xB0; off += 2) gba_io_write16(m, off, 0, 0xFFFF);
        gba_io_write16(m, 0x88, 0x200, 0xFFFF);
    }
    if (flags & 0x80u) {
        for (uint32_t off = 0x000; off < 0x060; off += 2) gba_io_write16(m, off, 0, 0xFFFF);
        for (uint32_t off = 0x0B0; off < 0x120; off += 2) gba_io_write16(m, off, 0, 0xFFFF);
        for (uint32_t off = 0x200; off < 0x20A; off += 2) {
            if (off != 0x202) gba_io_write16(m, off, 0, 0xFFFF);
        }
        gba_io_write16(m, 0x202, 0xFFFF, 0xFFFF); /* acknowledge everything */
        gba_io_write16(m, 0x000, 0x0080, 0xFFFF); /* forced blank */
        gba_io_write16(m, 0x020, 0x0100, 0xFFFF); /* identity affine */
        gba_io_write16(m, 0x026, 0x0100, 0xFFFF);
        gba_io_write16(m, 0x030, 0x0100, 0xFFFF);
        gba_io_write16(m, 0x036, 0x0100, 0xFFFF);
    }
}

static void soft_reset(GbaMachine *m)
{
    uint8_t flag = m->iwram[0x7FFA];
    memset(&m->iwram[0x7E00], 0, 0x200);
    m->rc_ram_gen++;
    GbaCpu *c = &m->cpu;
    gba_cpu_reset(c, true); /* SYS mode, BIOS stack pointers, r0-r12 = 0 */
    uint32_t entry = flag ? 0x02000000u : 0x08000000u;
    c->r[15] = entry + 8;
    c->branched = true;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* Return from the SWI the way the BIOS handler does: CPSR from SPSR_svc,
 * PC from LR_svc. */
static void swi_return(GbaMachine *m)
{
    GbaCpu *c = &m->cpu;
    uint32_t lr = c->r[14];
    gba_cpu_set_cpsr(c, gba_cpu_read_spsr(c));
    gba_cpu_branch(c, (c->cpsr & GBA_CPSR_T) ? (lr & ~1u) : (lr & ~3u));
    m->bios_latch = BIOS_LATCH_AFTER_SWI;
}

/* Return to the SWI instruction itself so it runs again after the next
 * interrupt (IntrWait loops this way). */
static void swi_retry(GbaMachine *m)
{
    GbaCpu *c = &m->cpu;
    uint32_t lr = c->r[14];
    gba_cpu_set_cpsr(c, gba_cpu_read_spsr(c));
    bool thumb = (c->cpsr & GBA_CPSR_T) != 0;
    gba_cpu_branch(c, thumb ? (lr - 2u) & ~1u : (lr - 4u) & ~3u);
    m->bios_latch = BIOS_LATCH_AFTER_SWI;
}

/* IntrWait / VBlankIntrWait. The BIOS loops internally: halt, let the
 * game's handler run and OR its flags into BIOS_IF (0x03007FF8), check.
 * Here the SWI instruction is re-executed after each interrupt instead, so
 * the wait remembers that it is in progress (by the SWI's return address)
 * and discards old flags only on the first entry. */
static void intr_wait(GbaMachine *m, bool discard, uint32_t mask)
{
    GbaCpu *c = &m->cpu;
    uint32_t flags = m->iwram[0x7FF8] | ((uint32_t)m->iwram[0x7FF9] << 8);
    bool resuming = m->intr_wait_active && m->intr_wait_return == c->r[14];
    if (!resuming) {
        if (discard) flags &= ~mask;
        m->intr_wait_active = true;
        m->intr_wait_return = c->r[14];
    }
    if (resuming && (flags & mask)) {
        flags &= ~mask;
        m->iwram[0x7FF8] = (uint8_t)flags;
        m->iwram[0x7FF9] = (uint8_t)(flags >> 8);
        m->intr_wait_active = false;
        m->ime = 1;
        gba_update_attention(m);
        swi_return(m);
        return;
    }
    m->iwram[0x7FF8] = (uint8_t)flags;
    m->iwram[0x7FF9] = (uint8_t)(flags >> 8);
    m->ime = 1;
    m->halted = true;
    gba_update_attention(m);
    swi_retry(m);
}

bool gba_bios_hle_swi(GbaMachine *m)
{
    GbaCpu *c = &m->cpu;
    /* LR_svc points after the SWI; the SPSR says which state it ran in. */
    uint32_t spsr = gba_cpu_read_spsr(c);
    uint32_t number;
    if (spsr & GBA_CPSR_T) number = gba_peek8(m, c->r[14] - 2u);
    else number = (gba_peek32(m, c->r[14] - 4u) >> 16) & 0xFFu;

    m->cpu.cycles += 8; /* entry and exit overhead, roughly */

    switch (number) {
    case 0x00: soft_reset(m); m->bios_latch = BIOS_LATCH_AFTER_SWI; return true;
    case 0x01: register_ram_reset(m, c->r[0]); break;
    case 0x02: m->halted = true; gba_update_attention(m); break;
    case 0x03: m->stopped = true; gba_update_attention(m); break;
    case 0x04: intr_wait(m, c->r[0] != 0, c->r[1]); return true;
    case 0x05: intr_wait(m, true, 1); return true;
    case 0x06: swi_div(c, (int32_t)c->r[0], (int32_t)c->r[1]); break;
    case 0x07: swi_div(c, (int32_t)c->r[1], (int32_t)c->r[0]); break;
    case 0x08: c->r[0] = isqrt(c->r[0]); break;
    case 0x09: c->r[0] = (uint32_t)arctan(c, (int32_t)(int16_t)c->r[0]); break;
    case 0x0A: c->r[0] = arctan2(c, (int32_t)(int16_t)c->r[0], (int32_t)(int16_t)c->r[1]); break;
    case 0x0B: cpu_set(m); break;
    case 0x0C: cpu_fast_set(m); break;
    case 0x0D: c->r[0] = 0xBAAE187Fu; break;
    case 0x0E: bg_affine_set(m); break;
    case 0x0F: obj_affine_set(m); break;
    case 0x10: bit_unpack(m); break;
    case 0x11: lz77(m, false); break;
    case 0x12: lz77(m, true); break;
    case 0x13: huffman(m); break;
    case 0x14: rl(m, false); break;
    case 0x15: rl(m, true); break;
    case 0x16: diff_unfilter(m, false, false); break;
    case 0x17: diff_unfilter(m, false, true); break;
    case 0x18: diff_unfilter(m, true, false); break;
    case 0x19: {
        uint16_t bias = (uint16_t)(m->io[0x88] | (m->io[0x89] << 8));
        bias = (uint16_t)((bias & 0xFC00u) | (c->r[0] ? 0x200u : 0u));
        gba_io_write16(m, 0x88, bias, 0xFFFF);
        break;
    }
    case 0x1F: { /* MidiKey2Freq */
        uint32_t freq = rd32(m, c->r[0] + 4);
        double exponent = (180.0 - (double)c->r[1] - (double)(c->r[2] & 0xFFu) / 256.0) / 12.0;
        c->r[0] = (uint32_t)((double)freq / pow(2.0, exponent));
        break;
    }
    case 0x27: /* CustomHalt */
        if (c->r[2] & 0x80u) m->stopped = true;
        else m->halted = true;
        gba_update_attention(m);
        break;
    default:
        /* Unimplemented (sound driver, multiboot): return without effect. */
        break;
    }
    swi_return(m);
    return true;
}
