/*
 * The DS sound unit, driven through its registers the way the ARM7 drives
 * it, against a small RAM for sample and capture data.
 *
 * Every expected value comes from GBATEK's "DS Sound" chapter (the section
 * is named at each group of checks), either quoted as a constant or
 * calculated here from GBATEK's formula by code written differently from
 * the unit's: divisions that round explicitly instead of shifts, a decoder
 * with a generated table, closed forms instead of running counters. A few
 * checks pin what GBATEK does not describe. Where another reference
 * decided, they name it; where none did, they say that they record this
 * unit's own choice, or a promise its header makes to the machine. The
 * last sections check no values at all, only that nothing a game can
 * write breaks the unit's rules.
 *
 * With SOUNDxTMR = 0xFE00 a channel's timer overflows once per mixer period
 * (0x10000 - 0xFE00 = 512 ticks), so sample k of a PCM sound is what the
 * channel holds at the end of frame k + 3: "PCM starts after 3 samples".
 *
 * `test_nds_spu --checksum` also prints checksums of every level produced,
 * for comparing one compiler's build with another's.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nds_spu.h"

/* ------------------------------------------------------------------ */
/* Test plumbing                                                       */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;
static const char *g_section = "";

static void check_eq(const char *what, long long got, long long want, int line)
{
    g_checks++;
    if (got == want) return;
    g_failures++;
    if (g_failures <= 60) {
        printf("FAIL %s: %s = %lld (0x%llX), expected %lld (0x%llX) (line %d)\n", g_section, what, got,
               (unsigned long long)got, want, (unsigned long long)want, line);
    }
}

#define EQ(what, got, want) check_eq(what, (long long)(got), (long long)(want), __LINE__)

/* A label for a check inside a loop. */
static const char *at(const char *format, ...)
{
    static char text[128];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    return text;
}

/* ------------------------------------------------------------------ */
/* The ARM7's bus: 128 KiB of RAM at 0x02000000                        */
/* ------------------------------------------------------------------ */

#define RAM_BASE 0x02000000u
#define RAM_SIZE 0x20000u

static uint8_t g_ram[RAM_SIZE];
static int g_bus_tag; /* what NdsSpuHost.user points at */

static struct {
    unsigned reads;
    unsigned writes;
    uint32_t read_end; /* one past the highest byte read */
} g_bus;

/* Accesses that were not whole words at word-aligned addresses inside the
 * RAM, or that did not carry the user pointer. Never reset. */
static unsigned g_bus_bad;

static bool bus_ok(void *user, uint32_t addr, int size)
{
    if (user == &g_bus_tag && size == 4 && (addr & 3u) == 0 && addr >= RAM_BASE && addr - RAM_BASE <= RAM_SIZE - 4u) {
        return true;
    }
    g_bus_bad++;
    return false;
}

static uint32_t bus_read(void *user, uint32_t addr, int size)
{
    if (!bus_ok(user, addr, size)) return 0;
    g_bus.reads++;
    if (addr + 4u > g_bus.read_end) g_bus.read_end = addr + 4u;
    return nds_get_le32(&g_ram[addr - RAM_BASE]);
}

static void bus_write(void *user, uint32_t addr, uint32_t value, int size)
{
    if (!bus_ok(user, addr, size)) return;
    g_bus.writes++;
    nds_put_le32(&g_ram[addr - RAM_BASE], value);
}

static const NdsSpuHost g_host = { bus_read, bus_write, &g_bus_tag };

static void put8(uint32_t addr, int value)
{
    g_ram[addr - RAM_BASE] = (uint8_t)(value & 0xFF);
}

static void put16(uint32_t addr, int value)
{
    nds_put_le16(&g_ram[addr - RAM_BASE], (uint16_t)(value & 0xFFFF));
}

static void put32(uint32_t addr, uint32_t value)
{
    nds_put_le32(&g_ram[addr - RAM_BASE], value);
}

static int get8s(uint32_t addr)
{
    int v = g_ram[addr - RAM_BASE];
    return v >= 0x80 ? v - 0x100 : v;
}

static int get16s(uint32_t addr)
{
    int v = nds_get_le16(&g_ram[addr - RAM_BASE]);
    return v >= 0x8000 ? v - 0x10000 : v;
}

/* Where each channel's sample data and each capture unit's buffer live. */
#define DATA(n) (RAM_BASE + 0x1000u + 0x400u * (uint32_t)(n))
#define CAPBUF(x) (RAM_BASE + 0x8000u + 0x1000u * (uint32_t)(x))

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

#define CH(n, reg) (0x04000400u + 0x10u * (uint32_t)(n) + (uint32_t)(reg)) /* reg: 0 CNT, 4 SAD, 8 TMR, 0xA PNT, 0xC LEN */
#define SOUNDCNT 0x04000500u
#define SOUNDBIAS 0x04000504u
#define SNDCAPCNT(x) (0x04000508u + (uint32_t)(x))
#define SNDCAPDAD(x) (0x04000510u + 8u * (uint32_t)(x))
#define SNDCAPLEN(x) (0x04000514u + 8u * (uint32_t)(x))

/* The word of NdsSpu.reg a register is kept in: what a write-only register
 * holds can only be seen there. */
#define REG_WORD(addr) (((addr) - NDS_SPU_IO_BASE) / 4u)

/* SOUNDxCNT fields. */
#define DIVIDER(d) ((uint32_t)(d) << 8)
#define HOLD 0x00008000u
#define PAN(p) ((uint32_t)(p) << 16)
#define DUTY(d) ((uint32_t)(d) << 24)
#define REPEAT(r) ((uint32_t)(r) << 27) /* 0 manual, 1 loop, 2 one-shot */
#define FORMAT(f) ((uint32_t)(f) << 29) /* 0 PCM8, 1 PCM16, 2 ADPCM, 3 PSG/noise */
#define START 0x80000000u

/* When set, every register write below goes to the unit a byte at a time,
 * lowest address first, the way an I/O layer built on byte writes hands
 * halfwords and words on. */
static bool g_split_writes;

static void wr(NdsSpu *spu, uint32_t addr, uint32_t value, int size)
{
    if (!g_split_writes) {
        nds_spu_write(spu, &g_host, addr, value, size);
        return;
    }
    for (int i = 0; i < size; i++) nds_spu_write(spu, &g_host, addr + (uint32_t)i, (value >> (8 * i)) & 0xFFu, 1);
}

static void wr8(NdsSpu *spu, uint32_t addr, uint32_t value)
{
    wr(spu, addr, value, 1);
}

static void wr16(NdsSpu *spu, uint32_t addr, uint32_t value)
{
    wr(spu, addr, value, 2);
}

static void wr32(NdsSpu *spu, uint32_t addr, uint32_t value)
{
    wr(spu, addr, value, 4);
}

static uint32_t rd8(const NdsSpu *spu, uint32_t addr)
{
    return nds_spu_read(spu, addr, 1);
}

static uint32_t rd16(const NdsSpu *spu, uint32_t addr)
{
    return nds_spu_read(spu, addr, 2);
}

static uint32_t rd32(const NdsSpu *spu, uint32_t addr)
{
    return nds_spu_read(spu, addr, 4);
}

static int busy(const NdsSpu *spu, int n)
{
    return (int)(rd32(spu, CH(n, 0)) >> 31);
}

/* SAD, TMR, PNT and LEN of a channel. */
static void setup(NdsSpu *spu, int n, uint32_t sad, uint32_t tmr, uint32_t pnt, uint32_t len)
{
    wr32(spu, CH(n, 4), sad);
    wr16(spu, CH(n, 8), tmr);
    wr16(spu, CH(n, 0xA), pnt);
    wr32(spu, CH(n, 0xC), len);
}

/* A sound unit out of reset with the settings every game runs with: master
 * enable, master volume 127, SOUNDBIAS 0x200. RAM and bus counters clear. */
static void fresh(NdsSpu *spu)
{
    memset(spu, 0xA5, sizeof *spu);
    nds_spu_reset(spu);
    memset(g_ram, 0, sizeof g_ram);
    memset(&g_bus, 0, sizeof g_bus);
    wr16(spu, SOUNDCNT, 0x807F);
    wr16(spu, SOUNDBIAS, 0x200);
}

static int16_t g_out[2];
static long g_frames;
static long g_host_sample_errors;
static uint32_t g_frame_checksum; /* of every level of every frame; see --checksum in main */

/* One mixer period. Every frame of every test also checks the host sample
 * against the documented derivation from the 10-bit level. */
static void frame(NdsSpu *spu)
{
    nds_spu_sample(spu, &g_host, g_out);
    g_frames++;
    for (int s = 0; s < 2; s++) {
        if (g_out[s] != ((int)nds_spu_dac(spu, s) - 0x200) * 64) g_host_sample_errors++;
        g_frame_checksum = g_frame_checksum * 31u + nds_spu_dac(spu, s);
    }
}

static void frames(NdsSpu *spu, int count)
{
    while (count-- > 0) frame(spu);
}

#define LEFT(spu) ((int)nds_spu_dac(spu, 0))
#define RIGHT(spu) ((int)nds_spu_dac(spu, 1))

/* ------------------------------------------------------------------ */
/* Expected values, calculated as GBATEK states them                   */
/* ------------------------------------------------------------------ */

/* a / b rounded towards minus infinity, b > 0. */
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

/* "7bit Volume and Panning Values": "Register settings of 0..126,127 are
 * interpreted as N=0..126,128." */
static int64_t setting7(int setting)
{
    return setting == 127 ? 128 : setting;
}

/* "Channel/Mixer Bit-Widths" steps 0-2: a channel after its volume divider
 * (16.4) and volume factor (16.11). */
static int64_t want_level(int sample, int divider, int volume)
{
    static const int divide_by[4] = { 1, 2, 4, 16 };
    return (int64_t)sample * (16 / divide_by[divider]) * setting7(volume);
}

/* Steps 3-4: "pan.left = data*(128-N)/128", "pan.right = data*N/128", then
 * "Rounding Down (strip 10bit)": 16.8. */
static int64_t want_side(int64_t level, int pan, int right)
{
    return floor_div(level * (right ? setting7(pan) : 128 - setting7(pan)), 1024);
}

/* Steps 6-9: "master.vol = data*N/128/64", "Strip fraction", "Add Bias",
 * "Clip (min/max 0h..3FFh)". `line` is 16.8 or wider. */
static int want_dac(int64_t line, int master, int bias)
{
    int64_t level = floor_div(line * setting7(master), (int64_t)128 * 64 * 256) + bias;
    return (int)(level < 0 ? 0 : level > 0x3FF ? 0x3FF : level);
}

/* One channel alone at volume 127, pan 0, master volume 127, bias 0x200:
 * the left level is the sample's top ten bits around 0x200. */
static int want_left_full(int sample)
{
    return (int)floor_div(sample, 64) + 0x200;
}

/* "Capture Clipping/Rounding": "If the MSB of the fractional part is set,
 * then data is rounded towards zero. (Positive values are rounded down,
 * negative values are rounded up.) The fractional part is then discarded". */
static int want_capture(int64_t value, int fraction_bits)
{
    int64_t one = (int64_t)1 << fraction_bits;
    int64_t whole = floor_div(value, one);
    int64_t fraction = value - whole * one;

    if (value < 0 && fraction * 2 >= one) whole++;
    return (int)whole;
}

/* "Incoming mixer data (20.8bits) is clipped to 16.8bits (MinMax
 * -8000h..7FFFh)". */
static int64_t clip_mixer(int64_t value)
{
    if (value < -0x8000 * 256) return -0x8000 * 256;
    if (value > 0x7FFF * 256 + 255) return 0x7FFF * 256 + 255;
    return value;
}

/* ------------------------------------------------------------------ */
/* Sample data                                                         */
/* ------------------------------------------------------------------ */

/* A 64-sample PCM16 loop with awkward values first: both rails, odd
 * values of both signs. Sample k of the endless sound; 0 before it starts. */
static int ramp(int k)
{
    static const int special[12] = { -3, -1, 1, 3, 0x7FFF, -0x8000, -0x1235, 0x1235, 5, -5, 0x4000, -0x4000 };

    if (k < 0) return 0;
    k %= 64;
    return k < 12 ? special[k] : (k * 2731) % 60000 - 30000;
}

/* Start the ramp on channel n at one sample per mixer period. `bits` are
 * volume, divider, pan and hold. */
static void start_ramp(NdsSpu *spu, int n, uint32_t bits)
{
    for (int k = 0; k < 64; k++) put16(DATA(n) + 2u * (uint32_t)k, ramp(k));
    setup(spu, n, DATA(n), 0xFE00, 0, 32);
    wr32(spu, CH(n, 0), START | FORMAT(1) | REPEAT(1) | bits);
}

/* Start a looped PCM16 sound that is `value` throughout: from the third
 * frame on the channel holds `value`. */
static void start_constant(NdsSpu *spu, int n, int value, uint32_t bits)
{
    for (int k = 0; k < 8; k++) put16(DATA(n) + 2u * (uint32_t)k, value);
    setup(spu, n, DATA(n), 0xFE00, 0, 4);
    wr32(spu, CH(n, 0), START | FORMAT(1) | REPEAT(1) | bits);
}

/* Bytes for PCM8 tests: no zeros in the first hundred. */
static int pattern8(int i)
{
    int v = (0x11 + i * 0x1D) & 0xFF;
    return v >= 0x80 ? v - 0x100 : v;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

static void test_reset(void)
{
    static NdsSpu a, b;
    unsigned nonzero = 0, sounding = 0;

    g_section = "reset";
    memset(&a, 0xA5, sizeof a);
    memset(&b, 0x5A, sizeof b);
    nds_spu_reset(&a);
    nds_spu_reset(&b);
    EQ("bytes differing between two resets", memcmp(&a, &b, sizeof a) != 0, 0);

    for (uint32_t addr = NDS_SPU_IO_BASE; addr < NDS_SPU_IO_END; addr++) nonzero += rd8(&a, addr) != 0;
    EQ("register bytes that read non-zero", nonzero, 0);
    EQ("left level before any frame", nds_spu_dac(&a, 0), 0);
    EQ("right level before any frame", nds_spu_dac(&a, 1), 0);

    /* Every register is 0 after reset, SOUNDBIAS with them (GBATEK gives no
     * reset values; melonDS resets it to 0 as well and has the direct boot
     * set 0x200). The bias is then all the output there is. */
    memset(&g_bus, 0, sizeof g_bus);
    frames(&a, 4);
    EQ("left level", LEFT(&a), 0);
    EQ("right level", RIGHT(&a), 0);
    EQ("left host sample", g_out[0], -32768);
    EQ("right host sample", g_out[1], -32768);
    EQ("bus reads", g_bus.reads, 0);
    EQ("bus writes", g_bus.writes, 0);
    for (int n = 0; n < NDS_SPU_CHANNELS; n++) sounding += nds_spu_channel_sample(&a, n) != 0;
    EQ("channels sounding", sounding, 0);

    /* With the bias the firmware leaves, silence is the middle level and a
     * host sample of 0, master enable or not. */
    wr16(&a, SOUNDBIAS, 0x200);
    frame(&a);
    EQ("left level, bias 0x200", LEFT(&a), 0x200);
    EQ("left host sample, bias 0x200", g_out[0], 0);
    EQ("right host sample, bias 0x200", g_out[1], 0);
}

/* ------------------------------------------------------------------ */
/* Register read-back                                                  */
/* ------------------------------------------------------------------ */

/* GBATEK "DS Sound Channels 0..15", "DS Sound Control Registers", "DS Sound
 * Capture": which bits exist, and which registers are (R/W) or (W). That a
 * (W) register reads 0 is melonDS's answer; GBATEK does not give one. */
static void test_registers(void)
{
    static NdsSpu spu, before;
    const uint32_t value = 0x6D40835Au; /* every field non-zero, start bit clear, no unused bit */

    g_section = "registers";
    fresh(&spu);
    for (int n = 0; n < NDS_SPU_CHANNELS; n++) {
        wr32(&spu, CH(n, 0), 0x7FFFFFFFu);
        EQ(at("SOUND%dCNT after 7FFFFFFF", n), rd32(&spu, CH(n, 0)), 0x7F7F837Fu);
        wr32(&spu, CH(n, 0), 0);
        EQ(at("SOUND%dCNT after 0", n), rd32(&spu, CH(n, 0)), 0);

        wr32(&spu, CH(n, 4), 0xFFFFFFFFu);
        wr32(&spu, CH(n, 8), 0xFFFFFFFFu);
        wr32(&spu, CH(n, 0xC), 0xFFFFFFFFu);
        EQ(at("SOUND%dSAD reads", n), rd32(&spu, CH(n, 4)), 0);
        EQ(at("SOUND%dTMR reads", n), rd16(&spu, CH(n, 8)), 0);
        EQ(at("SOUND%dPNT reads", n), rd16(&spu, CH(n, 0xA)), 0);
        EQ(at("SOUND%dLEN reads", n), rd32(&spu, CH(n, 0xC)), 0);
        /* What a write-only register holds: the bits GBATEK lists. */
        EQ(at("SOUND%dSAD holds", n), spu.reg[REG_WORD(CH(n, 4))], 0x07FFFFFCu);
        EQ(at("SOUND%dTMR/PNT hold", n), spu.reg[REG_WORD(CH(n, 8))], 0xFFFFFFFFu);
        EQ(at("SOUND%dLEN holds", n), spu.reg[REG_WORD(CH(n, 0xC))], 0x003FFFFFu);
    }
    EQ("bus reads from register writes that start nothing", g_bus.reads, 0);

    wr32(&spu, SOUNDCNT, 0xFFFFFFFFu);
    EQ("SOUNDCNT as a word", rd32(&spu, SOUNDCNT), 0x0000BF7Fu);
    EQ("SOUNDCNT as a halfword", rd16(&spu, SOUNDCNT), 0xBF7Fu);
    EQ("SOUNDCNT upper half", rd16(&spu, SOUNDCNT + 2), 0);
    EQ("SOUNDCNT byte 0", rd8(&spu, SOUNDCNT), 0x7F);
    EQ("SOUNDCNT byte 1", rd8(&spu, SOUNDCNT + 1), 0xBF);

    wr32(&spu, SOUNDBIAS, 0xFFFFFFFFu);
    EQ("SOUNDBIAS as a word", rd32(&spu, SOUNDBIAS), 0x000003FFu);
    EQ("SOUNDBIAS byte 0", rd8(&spu, SOUNDBIAS), 0xFF);
    EQ("SOUNDBIAS byte 1", rd8(&spu, SOUNDBIAS + 1), 0x03);
    EQ("SOUNDBIAS upper half", rd16(&spu, SOUNDBIAS + 2), 0);

    wr8(&spu, SNDCAPCNT(0), 0x7F);
    wr8(&spu, SNDCAPCNT(1), 0x7F);
    EQ("SNDCAP0CNT after 7F", rd8(&spu, SNDCAPCNT(0)), 0x0F);
    EQ("SNDCAP1CNT after 7F", rd8(&spu, SNDCAPCNT(1)), 0x0F);
    EQ("both as a halfword", rd16(&spu, SNDCAPCNT(0)), 0x0F0F);
    wr16(&spu, SNDCAPCNT(0), 0xFFFF);
    EQ("both after FFFF, as a word", rd32(&spu, SNDCAPCNT(0)), 0x00008F8Fu);
    wr8(&spu, SNDCAPCNT(0), 0);
    EQ("SNDCAP1CNT after a byte write to SNDCAP0CNT", rd16(&spu, SNDCAPCNT(0)), 0x8F00);
    wr8(&spu, SNDCAPCNT(1), 0);

    wr32(&spu, 0x0400050Cu, 0xFFFFFFFFu);
    EQ("0400050C reads", rd32(&spu, 0x0400050Cu), 0);
    for (int x = 0; x < NDS_SPU_CAPTURES; x++) {
        wr32(&spu, SNDCAPDAD(x), 0xFFFFFFFFu);
        wr32(&spu, SNDCAPLEN(x), 0xFFFFFFFFu);
        EQ(at("SNDCAP%dDAD", x), rd32(&spu, SNDCAPDAD(x)), 0x07FFFFFCu);
        EQ(at("SNDCAP%dLEN reads", x), rd32(&spu, SNDCAPLEN(x)), 0);
        EQ(at("SNDCAP%dLEN holds", x), spu.reg[REG_WORD(SNDCAPLEN(x))], 0x0000FFFFu);
    }

    /* The same register through bytes, halfwords and a word. */
    fresh(&spu);
    for (int i = 0; i < 4; i++) wr8(&spu, CH(0, i), (value >> (8 * i)) & 0xFFu);
    wr16(&spu, CH(1, 0), value & 0xFFFFu);
    wr16(&spu, CH(1, 2), value >> 16);
    wr32(&spu, CH(2, 0), value);
    for (int n = 0; n < 3; n++) {
        EQ(at("SOUND%dCNT read as a word", n), rd32(&spu, CH(n, 0)), value);
        EQ(at("SOUND%dCNT read as halfwords", n), rd16(&spu, CH(n, 0)) | (rd16(&spu, CH(n, 2)) << 16), value);
        EQ(at("SOUND%dCNT read as bytes", n),
           rd8(&spu, CH(n, 0)) | (rd8(&spu, CH(n, 1)) << 8) | (rd8(&spu, CH(n, 2)) << 16) | (rd8(&spu, CH(n, 3)) << 24),
           value);
    }
    wr8(&spu, CH(0, 0), 0x11);
    EQ("after a write to byte 0", rd32(&spu, CH(0, 0)), 0x6D408311u);
    wr8(&spu, CH(0, 2), 0x22);
    EQ("after a write to byte 2", rd32(&spu, CH(0, 0)), 0x6D228311u);
    wr8(&spu, CH(0, 1), 0xFF);
    EQ("byte 1 after FF", rd8(&spu, CH(0, 1)), 0x83);
    wr16(&spu, CH(0, 2), 0xFFFF);
    EQ("upper half after FFFF", rd16(&spu, CH(0, 2)), 0xFF7F);
    EQ("the lower half is untouched", rd16(&spu, CH(0, 0)), 0x8311);

    /* Accesses that are not aligned to a register, or lie across two. */
    fresh(&spu);
    EQ("word at 04000502", rd32(&spu, 0x04000502u), 0x02000000u);
    EQ("halfword at 04000501", rd16(&spu, 0x04000501u), 0x0080);
    wr32(&spu, 0x040004FEu, 0x3F15FFFFu); /* SOUND15LEN's upper half, then SOUNDCNT */
    EQ("SOUNDCNT from a word written at 040004FE", rd16(&spu, SOUNDCNT), 0x3F15);
    EQ("SOUND15LEN's upper half from that word", spu.reg[REG_WORD(CH(15, 0xC))], 0x003F0000u);
    EQ("that word read back", rd32(&spu, 0x040004FEu), 0x3F150000u);

    /* Outside 0x04000400-0x0400051F nothing is read or written. */
    wr32(&spu, 0x040003FEu, 0x12345678u); /* two bytes outside, then SOUND0CNT's lower half */
    EQ("SOUND0CNT's lower half from a word at 040003FE", rd16(&spu, CH(0, 0)), 0x0234);
    EQ("that word read back", rd32(&spu, 0x040003FEu), 0x02340000u);
    wr16(&spu, CH(0, 8), 0xFE7F);
    wr32(&spu, CH(0, 0), START | FORMAT(3) | 0x7F); /* something running, so that the state is not all zeros */
    wr32(&spu, SNDCAPLEN(1), 0xFFFFFFFFu);
    memcpy(&before, &spu, sizeof spu);
    wr32(&spu, 0x04000520u, 0xFFFFFFFFu);
    wr32(&spu, 0x040003FCu, 0xFFFFFFFFu);
    wr16(&spu, 0x0400051Eu, 0xFFFFu); /* SNDCAP1LEN's unused upper half */
    EQ("state changed by writes outside the registers", memcmp(&before, &spu, sizeof spu) != 0, 0);
    EQ("word at 04000520", rd32(&spu, 0x04000520u), 0);
    EQ("word at 04000524", rd32(&spu, 0x04000524u), 0);
    EQ("word at 040003FC", rd32(&spu, 0x040003FCu), 0);
    EQ("word at 0400051E, across the end", rd32(&spu, 0x0400051Eu), 0);
    EQ("word at 00000400", rd32(&spu, 0x00000400u), 0);

    /* The start bit alone, through its byte. */
    fresh(&spu);
    wr16(&spu, CH(8, 8), 0xFE00);
    wr8(&spu, CH(8, 0), 127);
    wr8(&spu, CH(8, 3), (START | FORMAT(3) | DUTY(6)) >> 24);
    EQ("busy after a byte write of the start bit", busy(&spu, 8), 1);
    EQ("the byte reads back", rd8(&spu, CH(8, 3)), 0xE6);
    frames(&spu, 2); /* duty 6: low, then high */
    EQ("and the channel plays", nds_spu_channel_sample(&spu, 8), 0x7FFF);
}

/* ------------------------------------------------------------------ */
/* PSG                                                                 */
/* ------------------------------------------------------------------ */

/* GBATEK "DS Sound Notes", PSG Wave Duty. */
static const char *const k_duty_wave[8] = {
    "_______-_______-_______-", /* 0  12.5% */
    "______--______--______--", /* 1  25.0% */
    "_____---_____---_____---", /* 2  37.5% */
    "____----____----____----", /* 3  50.0% */
    "___-----___-----___-----", /* 4  62.5% */
    "__------__------__------", /* 5  75.0% */
    "_-------_-------_-------", /* 6  87.5% */
    "________________________", /* 7   0.0% */
};

static void test_psg(void)
{
    static NdsSpu spu;
    unsigned wrong;

    /* "The output volume equals to PCM16 values +7FFFh (HIGH) and -7FFFh
     * (LOW)"; "The duty cycle always starts at the begin of the LOW period";
     * "PSG/Noise starts after 1 sample". At full volume on the left alone
     * HIGH is +511 and LOW -512 around the bias: both rails. */
    g_section = "PSG duty";
    for (int duty = 0; duty < 8; duty++) {
        int n = 8 + duty % 6;
        int highs = 0;

        fresh(&spu);
        wr16(&spu, CH(n, 8), 0xFE00);
        wr32(&spu, CH(n, 0), START | FORMAT(3) | DUTY(duty) | 127);
        for (int step = 0; step < 24; step++) {
            bool high = k_duty_wave[duty][step] == '-';

            frame(&spu);
            EQ(at("duty %d step %d sample", duty, step), nds_spu_channel_sample(&spu, n), high ? 0x7FFF : -0x7FFF);
            EQ(at("duty %d step %d left", duty, step), LEFT(&spu), high ? 0x3FF : 0x000);
            EQ(at("duty %d step %d right", duty, step), RIGHT(&spu), 0x200);
            highs += high;
        }
        /* SOUNDxCNT's own description, "HIGH=(N+1)*12.5%", would make duty
         * 7 always high; the table in the notes has it at 0.0%. */
        EQ(at("duty %d high steps in three cycles", duty), highs, duty == 7 ? 0 : 3 * (duty + 1));
        EQ(at("duty %d busy", duty), busy(&spu, n), 1);
        EQ(at("duty %d bus reads", duty), g_bus.reads, 0);
    }

    /* "the frequency for Rectangular Wave is 1/8th of the selected sample
     * frequency": one step per timer overflow. With a period of 1024 ticks
     * a step takes two frames, with 256 two steps fit in one. */
    g_section = "PSG rate";
    fresh(&spu);
    wr16(&spu, CH(8, 8), 0xFC00);
    wr32(&spu, CH(8, 0), START | FORMAT(3) | DUTY(3) | 127);
    wr16(&spu, CH(13, 8), 0xFF00);
    wr32(&spu, CH(13, 0), START | FORMAT(3) | DUTY(3) | PAN(127) | 127);
    for (int f = 1; f <= 40; f++) {
        int slow_steps = 512 * f / 1024, fast_steps = 512 * f / 256;
        int slow = slow_steps == 0 ? 0 : k_duty_wave[3][(slow_steps - 1) % 8] == '-' ? 0x7FFF : -0x7FFF;
        int fast = k_duty_wave[3][(fast_steps - 1) % 8] == '-' ? 0x7FFF : -0x7FFF;

        frame(&spu);
        EQ(at("frame %d, a step in two frames", f), nds_spu_channel_sample(&spu, 8), slow);
        EQ(at("frame %d, two steps a frame", f), nds_spu_channel_sample(&spu, 13), fast);
        EQ(at("frame %d left", f), LEFT(&spu), want_left_full(slow));
        EQ(at("frame %d right", f), RIGHT(&spu), want_left_full(fast));
    }

    /* "PSG sound is always Infinite (the SOUNDxLEN Register, and the
     * SOUNDxCNT Repeat Mode bits have no effect)". */
    g_section = "PSG is infinite";
    fresh(&spu);
    setup(&spu, 9, 0, 0xFE00, 0, 0);
    wr32(&spu, CH(9, 0), START | FORMAT(3) | REPEAT(2) | DUTY(3) | 127);
    frames(&spu, 200);
    EQ("busy after 200 steps", busy(&spu, 9), 1);
    wrong = 0;
    for (int step = 200; step < 216; step++) {
        frame(&spu);
        wrong += nds_spu_channel_sample(&spu, 9) != (k_duty_wave[3][step % 8] == '-' ? 0x7FFF : -0x7FFF);
    }
    EQ("steps 200-215 off the wave", wrong, 0);
    EQ("bus reads", g_bus.reads, 0);

    /* "PSG rectangular wave can be used only on channels 8..13". */
    g_section = "PSG on channels 0-7";
    for (int n = 0; n < 8; n++) {
        fresh(&spu);
        wr16(&spu, CH(n, 8), 0xFE00);
        wr32(&spu, CH(n, 0), START | FORMAT(3) | DUTY(3) | 127);
        wrong = 0;
        for (int f = 0; f < 20; f++) {
            frame(&spu);
            wrong += nds_spu_channel_sample(&spu, n) != 0 || LEFT(&spu) != 0x200 || RIGHT(&spu) != 0x200;
        }
        EQ(at("channel %d frames with output", n), wrong, 0);
        EQ(at("channel %d busy", n), busy(&spu, n), 1);
    }

    /* The duty bits are looked at for every step: a write to them takes
     * effect at the next one, without restarting the cycle. */
    g_section = "PSG duty change";
    fresh(&spu);
    wr16(&spu, CH(10, 8), 0xFE00);
    wr32(&spu, CH(10, 0), START | FORMAT(3) | DUTY(0) | 127);
    frames(&spu, 2);
    wr8(&spu, CH(10, 3), (START | FORMAT(3) | DUTY(6)) >> 24);
    for (int step = 2; step < 18; step++) {
        frame(&spu);
        EQ(at("step %d", step), nds_spu_channel_sample(&spu, 10), k_duty_wave[6][step % 8] == '-' ? 0x7FFF : -0x7FFF);
    }
}

/* ------------------------------------------------------------------ */
/* Noise                                                               */
/* ------------------------------------------------------------------ */

/* GBATEK "DS Sound Notes", PSG Noise, worked by hand from X=7FFFh. The
 * first fourteen steps each shift a one out (LOW) and put 6000h back:
 * 5FFFh, 4FFFh, 47FFh ... 4000h. The one bit left takes fourteen steps
 * (HIGH) to come down to bit 0 and one more to leave (LOW, X=6000h); those
 * two bits take thirteen (HIGH) to come down to 0003h and two to leave
 * (LOW, LOW, X=5000h); the next step is HIGH. */
static const char k_noise_start[] = "LLLLLLLLLLLLLL"
                                    "HHHHHHHHHHHHHH"
                                    "L"
                                    "HHHHHHHHHHHHH"
                                    "LL"
                                    "H";

/* "X=X SHR 1, IF carry THEN Out=LOW, X=X XOR 6000h ELSE Out=HIGH", on a
 * register kept as fifteen separate bits. Returns true for HIGH. */
static bool reference_noise(uint8_t bits[15])
{
    uint8_t carry = bits[0];

    for (int i = 0; i < 14; i++) bits[i] = bits[i + 1];
    bits[14] = 0;
    if (carry) {
        bits[13] ^= 1u; /* 2000h */
        bits[14] ^= 1u; /* 4000h */
    }
    return !carry;
}

static void test_noise(void)
{
    static NdsSpu spu;

    g_section = "noise";
    for (int n = 14; n < 16; n++) {
        uint8_t bits[15];
        unsigned wrong = 0, wrong_level = 0;
        const int length = 32767 + 200; /* past the point where a 15-bit register repeats */

        fresh(&spu);
        memset(bits, 1, sizeof bits);
        wr16(&spu, CH(n, 8), 0xFE00);
        wr32(&spu, CH(n, 0), START | FORMAT(3) | DUTY(3) | 127);
        for (int i = 0; i < length; i++) {
            bool high = reference_noise(bits);
            int sample;

            frame(&spu);
            sample = nds_spu_channel_sample(&spu, n);
            if (i < (int)sizeof k_noise_start - 1) {
                EQ(at("channel %d value %d", n, i), sample, k_noise_start[i] == 'H' ? 0x7FFF : -0x7FFF);
            }
            wrong += sample != (high ? 0x7FFF : -0x7FFF);
            wrong_level += LEFT(&spu) != (high ? 0x3FF : 0x000);
        }
        EQ(at("channel %d values off the reference", n), wrong, 0);
        EQ(at("channel %d levels off the reference", n), wrong_level, 0);
        EQ(at("channel %d busy", n), busy(&spu, n), 1);
        EQ(at("channel %d bus reads", n), g_bus.reads, 0);

        /* "The initial value when (re-)starting the sound is X=7FFFh." */
        wr32(&spu, CH(n, 0), 0);
        frame(&spu);
        EQ(at("channel %d stopped", n), nds_spu_channel_sample(&spu, n), 0);
        wr32(&spu, CH(n, 0), START | FORMAT(3) | 127);
        wrong = 0;
        for (int i = 0; i < (int)sizeof k_noise_start - 1; i++) {
            frame(&spu);
            wrong += nds_spu_channel_sample(&spu, n) != (k_noise_start[i] == 'H' ? 0x7FFF : -0x7FFF);
        }
        EQ(at("channel %d values off the start after a restart", n), wrong, 0);
    }
}

/* ------------------------------------------------------------------ */
/* PCM8 and PCM16                                                      */
/* ------------------------------------------------------------------ */

/* 16-bit data for the PCM16 length tests. */
static int pattern16(int i)
{
    return (i + 1) * 0x0123 * (i & 1 ? -1 : 1);
}

static void test_pcm(void)
{
    static NdsSpu spu;
    unsigned wrong, reads_before;

    /* GBATEK "DS Sound Notes": "Signed samples in range -80h..+7Fh (PCM8)";
     * "The output volume of PCM8=NNh is equal to PCM16=NN00h". "In One-shot
     * mode, the sound length is the sum of (PNT+LEN)", 4 PCM8 samples a
     * word. Sound Stop: "the Busy bit gets cleared automatically at the
     * BEGIN of the last sample period", and that sample is still output to
     * the end of its period. Every channel can play PCM. */
    g_section = "PCM8 one-shot";
    for (int n = 0; n < NDS_SPU_CHANNELS; n++) {
        fresh(&spu);
        for (int i = 0; i < 64; i++) put8(DATA(n) + (uint32_t)i, pattern8(i));
        setup(&spu, n, DATA(n), 0xFE00, n & 1 ? 0 : 3, n & 1 ? 4 : 1); /* 4 words either way */
        wr32(&spu, CH(n, 0), START | FORMAT(0) | REPEAT(2) | 127);
        EQ(at("channel %d busy once started", n), busy(&spu, n), 1);
        for (int f = 1; f <= 22; f++) {
            int k = f - 3;
            int want = k >= 0 && k < 16 ? pattern8(k) * 256 : 0;

            frame(&spu);
            EQ(at("channel %d frame %d sample", n, f), nds_spu_channel_sample(&spu, n), want);
            EQ(at("channel %d frame %d busy", n, f), busy(&spu, n), f < 18);
            EQ(at("channel %d frame %d left", n, f), LEFT(&spu), want_left_full(want));
        }
        /* The whole sound is 16 bytes, and nothing past them is fetched. */
        EQ(at("channel %d words read", n), g_bus.reads, 4);
        EQ(at("channel %d highest byte read", n), g_bus.read_end, DATA(n) + 16u);
    }

    /* "2*N PCM16 samples" for N words. */
    g_section = "PCM16 one-shot";
    for (int n = 0; n < NDS_SPU_CHANNELS; n += 5) {
        fresh(&spu);
        for (int i = 0; i < 32; i++) put16(DATA(n) + 2u * (uint32_t)i, pattern16(i));
        setup(&spu, n, DATA(n), 0xFE00, 1, 3);
        wr32(&spu, CH(n, 0), START | FORMAT(1) | REPEAT(2) | 127);
        for (int f = 1; f <= 14; f++) {
            int k = f - 3;
            int want = k >= 0 && k < 8 ? pattern16(k) : 0;

            frame(&spu);
            EQ(at("channel %d frame %d sample", n, f), nds_spu_channel_sample(&spu, n), want);
            EQ(at("channel %d frame %d busy", n, f), busy(&spu, n), f < 10);
        }
        EQ(at("channel %d words read", n), g_bus.reads, 4);
        EQ(at("channel %d highest byte read", n), g_bus.read_end, DATA(n) + 16u);
    }

    /* "In Looped mode, the length is (1*PNT+Infinite*LEN), ie. the first
     * part (PNT) is played once, the second part (LEN) is repeated
     * infinitely." */
    g_section = "PCM8 loop";
    fresh(&spu);
    for (int i = 0; i < 64; i++) put8(DATA(5) + (uint32_t)i, pattern8(i));
    setup(&spu, 5, DATA(5), 0xFE00, 1, 3); /* 4 bytes, then 12 for ever */
    wr32(&spu, CH(5, 0), START | FORMAT(0) | REPEAT(1) | 127);
    for (int f = 1; f <= 70; f++) {
        int k = f - 3;
        int want = k < 0 ? 0 : pattern8(k < 16 ? k : 4 + (k - 4) % 12) * 256;

        frame(&spu);
        EQ(at("frame %d sample", f), nds_spu_channel_sample(&spu, 5), want);
    }
    EQ("busy", busy(&spu, 5), 1);
    EQ("highest byte read", g_bus.read_end, DATA(5) + 16u);

    g_section = "PCM16 loop";
    fresh(&spu);
    for (int i = 0; i < 32; i++) put16(DATA(11) + 2u * (uint32_t)i, pattern16(i));
    setup(&spu, 11, DATA(11), 0xFE00, 2, 4); /* 4 samples, then 8 for ever */
    wr32(&spu, CH(11, 0), START | FORMAT(1) | REPEAT(1) | 127);
    for (int f = 1; f <= 70; f++) {
        int k = f - 3;
        int want = k < 0 ? 0 : pattern16(k < 12 ? k : 4 + (k - 4) % 8);

        frame(&spu);
        EQ(at("frame %d sample", f), nds_spu_channel_sample(&spu, 11), want);
    }
    EQ("busy", busy(&spu, 11), 1);
    EQ("highest byte read", g_bus.read_end, DATA(11) + 24u);

    /* The FIFO between memory and the channel ("DS Sound Block Diagrams";
     * the sizes are melonDS's): 8 words are read when the channel starts
     * and 4 more each time it is down to 4. A PCM16 sample is two bytes,
     * so the 8th sample, at frame 10, leaves 16. */
    g_section = "read-ahead";
    fresh(&spu);
    start_ramp(&spu, 12, 127);
    EQ("words read by the start", g_bus.reads, 8);
    EQ("highest byte read by the start", g_bus.read_end, DATA(12) + 32u);
    frames(&spu, 9);
    EQ("words read after 7 samples", g_bus.reads, 8);
    frame(&spu);
    EQ("words read after 8 samples", g_bus.reads, 12);
    frames(&spu, 7);
    EQ("words read after 15 samples", g_bus.reads, 12);
    frame(&spu);
    EQ("words read after 16 samples", g_bus.reads, 16);
    EQ("highest byte read", g_bus.read_end, DATA(12) + 64u);

    /* SOUNDxSAD: "Bit0-26 Source Address (must be word aligned, bit0-1 are
     * always zero)", "Bit27-31 Not used". */
    g_section = "SOUNDxSAD bits";
    fresh(&spu);
    for (int i = 0; i < 16; i++) put8(DATA(3) + (uint32_t)i, pattern8(i));
    setup(&spu, 3, DATA(3) | 0xF8000003u, 0xFE00, 0, 4);
    wr32(&spu, CH(3, 0), START | FORMAT(0) | REPEAT(2) | 127);
    wrong = 0;
    for (int f = 1; f <= 18; f++) {
        frame(&spu);
        wrong += nds_spu_channel_sample(&spu, 3) != (f >= 3 ? pattern8(f - 3) * 256 : 0);
    }
    EQ("frames off the data at the aligned 27-bit address", wrong, 0);

    /* "A sound will be started/restarted when changing its start bit from 0
     * to 1": writing 1 over 1 changes nothing, 0 stops the output at once,
     * and the next 1 starts from the top, delay included. */
    g_section = "start and stop";
    fresh(&spu);
    start_ramp(&spu, 6, 127);
    frames(&spu, 10);
    EQ("sample before the rewrite", nds_spu_channel_sample(&spu, 6), ramp(7));
    wr32(&spu, CH(6, 0), START | FORMAT(1) | REPEAT(1) | 100);
    frame(&spu);
    EQ("sample after writing the start bit again", nds_spu_channel_sample(&spu, 6), ramp(8));
    wr8(&spu, CH(6, 3), (FORMAT(1) | REPEAT(1)) >> 24);
    EQ("busy after writing 0", busy(&spu, 6), 0);
    reads_before = g_bus.reads;
    for (int f = 0; f < 6; f++) {
        frame(&spu);
        EQ(at("stopped, frame %d", f), nds_spu_channel_sample(&spu, 6), 0);
    }
    EQ("words read while stopped", g_bus.reads - reads_before, 0);
    wr8(&spu, CH(6, 3), (START | FORMAT(1) | REPEAT(1)) >> 24);
    EQ("busy after writing 1", busy(&spu, 6), 1);
    for (int f = 1; f <= 8; f++) {
        frame(&spu);
        EQ(at("restarted, frame %d", f), nds_spu_channel_sample(&spu, 6), ramp(f - 3));
    }
}

/* Repeat modes 0 and 3, and lengths too short to play. */
static void test_repeat_modes(void)
{
    static NdsSpu spu;
    unsigned wrong;

    /* GBATEK only names mode 0 "Manual". melonDS decided: the channel
     * plays on past PNT+LEN through the memory that follows and stays
     * busy. (DeSmuME ends the sound there.) */
    g_section = "repeat mode 0";
    fresh(&spu);
    for (int i = 0; i < 64; i++) put8(DATA(7) + (uint32_t)i, pattern8(i));
    setup(&spu, 7, DATA(7), 0xFE00, 0, 4);
    wr32(&spu, CH(7, 0), START | FORMAT(0) | REPEAT(0) | 127);
    wrong = 0;
    for (int f = 1; f <= 50; f++) {
        frame(&spu);
        wrong += nds_spu_channel_sample(&spu, 7) != (f >= 3 ? pattern8(f - 3) * 256 : 0);
    }
    EQ("frames off the data, three times the length in", wrong, 0);
    EQ("busy", busy(&spu, 7), 1);

    /* Mode 3, "Prohibited": bit 0 makes it loop, as in melonDS. */
    g_section = "repeat mode 3";
    fresh(&spu);
    for (int i = 0; i < 64; i++) put8(DATA(7) + (uint32_t)i, pattern8(i));
    setup(&spu, 7, DATA(7), 0xFE00, 1, 3);
    wr32(&spu, CH(7, 0), START | FORMAT(0) | REPEAT(3) | 127);
    wrong = 0;
    for (int f = 1; f <= 50; f++) {
        int k = f - 3;

        frame(&spu);
        wrong += nds_spu_channel_sample(&spu, 7) != (k < 0 ? 0 : pattern8(k < 16 ? k : 4 + (k - 4) % 12) * 256);
    }
    EQ("frames off the loop", wrong, 0);
    EQ("busy", busy(&spu, 7), 1);

    /* GBATEK "DS Sound Channels 0..15": "Minimum length (the sum of
     * PNT+LEN) is 4 words (16 bytes), smaller values (0..3 words) are
     * causing hang-ups (busy bit remains set infinite, but no sound output
     * occurs)." */
    g_section = "length under 4 words";
    for (int format = 0; format < 3; format++) {
        for (int repeat = 0; repeat < 3; repeat++) {
            for (int words = 0; words < 4; words++) {
                fresh(&spu);
                for (int i = 0; i < 64; i++) put8(DATA(0) + (uint32_t)i, 0x55);
                setup(&spu, 0, DATA(0), 0xFE00, (uint32_t)words / 2u, (uint32_t)words - (uint32_t)words / 2u);
                wr32(&spu, CH(0, 0), START | FORMAT(format) | REPEAT(repeat) | 127);
                wrong = 0;
                for (int f = 0; f < 40; f++) {
                    frame(&spu);
                    wrong += nds_spu_channel_sample(&spu, 0) != 0 || LEFT(&spu) != 0x200;
                }
                EQ(at("format %d repeat %d, %d words: frames with output", format, repeat, words), wrong, 0);
                EQ(at("format %d repeat %d, %d words: busy", format, repeat, words), busy(&spu, 0), 1);
            }
        }
    }

    /* Two corners GBATEK does not describe. They pin what this unit does,
     * so that a change shows; neither is a hardware fact.
     *
     * A loop of no length (LEN = 0 after a first part): the sound plays
     * once and then there is nothing to fetch. The channel stays busy on
     * its last sample and reads nothing beyond the sound. */
    g_section = "undocumented: a loop of no length";
    fresh(&spu);
    for (int i = 0; i < 64; i++) put8(DATA(7) + (uint32_t)i, pattern8(i));
    setup(&spu, 7, DATA(7), 0xFE00, 4, 0);
    wr32(&spu, CH(7, 0), START | FORMAT(0) | REPEAT(1) | 127);
    wrong = 0;
    for (int f = 1; f <= 60; f++) {
        int k = f - 3;

        frame(&spu);
        wrong += nds_spu_channel_sample(&spu, 7) != (k < 0 ? 0 : pattern8(k < 16 ? k : 15) * 256);
    }
    EQ("frames off", wrong, 0);
    EQ("busy", busy(&spu, 7), 1);
    EQ("highest byte read", g_bus.read_end, DATA(7) + 16u);

    /* SOUNDxLEN cut under a playing one-shot to before where it is: the
     * sound is over at its next sample. */
    g_section = "undocumented: length cut under a one-shot";
    fresh(&spu);
    for (int i = 0; i < 64; i++) put8(DATA(7) + (uint32_t)i, pattern8(i));
    setup(&spu, 7, DATA(7), 0xFE00, 0, 16);
    wr32(&spu, CH(7, 0), START | FORMAT(0) | REPEAT(2) | 127);
    frames(&spu, 20);
    EQ("playing", nds_spu_channel_sample(&spu, 7), pattern8(17) * 256);
    wr32(&spu, CH(7, 0xC), 4);
    frame(&spu);
    EQ("sample", nds_spu_channel_sample(&spu, 7), 0);
    EQ("busy", busy(&spu, 7), 0);
}

/* ------------------------------------------------------------------ */
/* Timer                                                               */
/* ------------------------------------------------------------------ */

/* GBATEK "DS Sound Channels 0..15": "timerval=-(33513982Hz/2)/freq", so a
 * channel moves on every (0x10000 - SOUNDxTMR) ticks of a clock that gives
 * a mixer period 512 ticks, and "PCM starts after 3 samples". At the end
 * of frame f the channel has stepped floor(512 f / period) times and holds
 * sample (steps - 3). */
static void test_timer(void)
{
    static NdsSpu spu;
    static const struct {
        uint32_t reload;
        const char *rate;
    } k_rates[] = {
        { 0xFE00, "one sample a frame" },
        { 0xFC00, "half a sample a frame" },
        { 0xFF00, "two samples a frame" },
        { 0xFD00, "two samples in three frames" },
        { 0xFFC0, "eight samples a frame" },
        { 0xF000, "one sample in eight frames" },
        { 0xFE01, "a period of 511 ticks" },
        { 0xFFFF, "512 samples a frame" },
    };

    g_section = "timer";
    for (size_t r = 0; r < sizeof k_rates / sizeof k_rates[0]; r++) {
        long period = 0x10000 - (long)k_rates[r].reload;
        unsigned wrong = 0, most_reads = 0;

        fresh(&spu);
        for (int k = 0; k < 64; k++) put16(DATA(2) + 2u * (uint32_t)k, ramp(k));
        setup(&spu, 2, DATA(2), k_rates[r].reload, 0, 32);
        wr32(&spu, CH(2, 0), START | FORMAT(1) | REPEAT(1) | 127);
        for (long f = 1; f <= 300; f++) {
            unsigned reads = g_bus.reads;

            frame(&spu);
            wrong += nds_spu_channel_sample(&spu, 2) != ramp((int)(512 * f / period) - 3);
            if (g_bus.reads - reads > most_reads) most_reads = g_bus.reads - reads;
        }
        EQ(at("%s: frames off the expected sample", k_rates[r].rate), wrong, 0);
        /* 512 PCM16 samples are 1024 bytes: 64 bursts of four words, one
         * more or less as the FIFO's level drifts. */
        EQ(at("%s: more than 260 words read in a frame", k_rates[r].rate), most_reads > 260, 0);
    }

    /* SOUNDxTMR is a reload value: a new one counts from the next
     * overflow. GBATEK calls it the "Timer Value" and says no more; sound
     * drivers bend pitch by rewriting it under a playing channel. After
     * ten frames at one sample a frame the overflow of frame 11 still
     * comes on time, and from there one comes every other frame. */
    g_section = "timer reload";
    fresh(&spu);
    start_ramp(&spu, 2, 127);
    frames(&spu, 10);
    wr16(&spu, CH(2, 8), 0xFC00);
    for (int f = 11; f <= 40; f++) {
        frame(&spu);
        EQ(at("frame %d sample", f), nds_spu_channel_sample(&spu, 2), ramp(8 + (f - 11) / 2));
    }
}

/* ------------------------------------------------------------------ */
/* IMA-ADPCM                                                           */
/* ------------------------------------------------------------------ */

static int g_step_table[128];

/* GBATEK "DS Sound Notes", IMA-ADPCM Format: "The closest way to reproduce
 * the AdpcmTable with 32bit integer maths appears:
 *   X=000776d2h, FOR I=0 TO 88, Table[I]=X SHR 16, X=X+(X/10), NEXT I
 *   Table[3]=000Ah, Table[4]=000Bh, Table[88]=7FFFh, Table[89..127]=0000h" */
static void build_step_table(void)
{
    uint32_t x = 0x000776D2u;

    for (int i = 0; i <= 88; i++) {
        g_step_table[i] = (int)(x >> 16);
        x += x / 10u;
    }
    g_step_table[3] = 0x000A;
    g_step_table[4] = 0x000B;
    g_step_table[88] = 0x7FFF;
    for (int i = 89; i < 128; i++) g_step_table[i] = 0;
}

/* The reference decoder: `stream` is the 4-byte header and then the data,
 * `count` 4-bit values are decoded into out[].
 *   Diff = AdpcmTable[Index]/8
 *   IF (data4bit AND 1) THEN Diff = Diff + AdpcmTable[Index]/4
 *   IF (data4bit AND 2) THEN Diff = Diff + AdpcmTable[Index]/2
 *   IF (data4bit AND 4) THEN Diff = Diff + AdpcmTable[Index]/1
 *   IF (Data4bit AND 8)=0 THEN Pcm16bit = Max(Pcm16bit+Diff,+7FFFh)
 *   IF (Data4bit AND 8)=8 THEN Pcm16bit = Min(Pcm16bit-Diff,-7FFFh)
 *   Index = MinMax (Index+IndexTable[Data4bit AND 7],0,88) */
static bool reference_step(int *pcm, int *index, int data)
{
    static const int index_table[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    int entry = g_step_table[*index];
    int diff = entry / 8;
    bool clipped = false;

    for (int bit = 0; bit < 3; bit++) {
        if (data & (1 << bit)) diff += entry >> (2 - bit);
    }
    if (data & 8) {
        *pcm -= diff;
        if (*pcm < -0x7FFF) {
            *pcm = -0x7FFF;
            clipped = true;
        }
    } else {
        *pcm += diff;
        if (*pcm > 0x7FFF) {
            *pcm = 0x7FFF;
            clipped = true;
        }
    }
    *index += index_table[data & 7];
    *index = *index < 0 ? 0 : *index > 88 ? 88 : *index;
    return clipped;
}

static void reference_adpcm(const uint8_t *stream, int count, int *out)
{
    int pcm = stream[0] | (stream[1] << 8);
    int index = stream[2] & 0x7F;

    if (pcm >= 0x8000) pcm -= 0x10000;
    for (int k = 0; k < count; k++) {
        int data = (stream[4 + k / 2] >> (4 * (k % 2))) & 15; /* "the first value in the lower 4bits" */

        reference_step(&pcm, &index, data);
        out[k] = pcm;
    }
}

/* A stream that takes the decoder through every table entry, 88 down to 0,
 * with a value of magnitude 7 at each (the whole entry and all three of
 * its fractions go into the difference) and without clipping there, so
 * that a single wrong bit anywhere in a table shows. Between those values
 * the index is walked back down with magnitude 0. Returns the number of
 * 4-bit values; *unclipped counts the table entries so used. */
static int build_table_walk(uint8_t *stream, int room, int *unclipped)
{
    int pcm = -30000, index = 88, count = 0;

    memset(stream, 0, (size_t)room);
    stream[0] = (uint8_t)((uint32_t)pcm & 0xFFu);
    stream[1] = (uint8_t)(((uint32_t)pcm >> 8) & 0xFFu);
    stream[2] = (uint8_t)index;
    *unclipped = 0;
    for (int entry = 88; entry >= 0; entry--) {
        int data = pcm < 0 ? 7 : 15;

        if (index != entry || 4 + count / 2 >= room) return 0;
        stream[4 + count / 2] |= (uint8_t)(data << (4 * (count % 2)));
        count++;
        *unclipped += !reference_step(&pcm, &index, data);
        while (entry > 0 && index > entry - 1) {
            data = pcm < 0 ? 0 : 8;
            if (4 + count / 2 >= room) return 0;
            stream[4 + count / 2] |= (uint8_t)(data << (4 * (count % 2)));
            count++;
            reference_step(&pcm, &index, data);
        }
    }
    return count;
}

static void test_adpcm(void)
{
    enum { WORDS = 32, LOOP_WORD = 9, NIBBLES = (WORDS - 1) * 8, LOOP_NIBBLE = (LOOP_WORD - 1) * 8 };
    static NdsSpu spu;
    static uint8_t stream[4 + (WORDS - 1) * 4 + (WORDS - LOOP_WORD) * 4];
    static int want[NIBBLES], straight[NIBBLES + (WORDS - LOOP_WORD) * 8];
    uint32_t seed = 0x12345678u;
    int low = 0, high = 0, unreloaded = 0;

    build_step_table();
    g_section = "ADPCM table";
    EQ("entry 0", g_step_table[0], 0x0007);
    EQ("entry 44", g_step_table[44], 0x01EE);
    EQ("entry 87", g_step_table[87], 0x7462);

    /* A 32-word sound: the header, then 124 bytes of data. Most of it
     * steps gently (small magnitudes, either sign), which keeps the index
     * low and the value off the rails, so that what is decoded depends on
     * the state the decoder is in. Inside the loop, which starts at word
     * 9, there are also a run of +7 values up to the +7FFFh clip, a run
     * of -7 values down to the -7FFFh clip, and 88 values alternating +0
     * and -0 that walk the index back down through every table entry. */
    stream[0] = 0x23;
    stream[1] = 0x01;
    stream[2] = 20;
    stream[3] = 0;
    for (int i = 4; i < WORDS * 4; i++) {
        uint32_t nibble[2];

        for (int half = 0; half < 2; half++) {
            uint32_t r;

            seed = seed * 1103515245u + 12345u;
            r = seed >> 16;
            nibble[half] = (r % 3u == 0 ? 4u : (r >> 4) & 3u) | ((r >> 8) & 8u);
        }
        stream[i] = (uint8_t)(nibble[0] | (nibble[1] << 4));
        if (i >= 52 && i < 60) stream[i] = 0x77;
        if (i >= 60 && i < 68) stream[i] = 0xFF;
        if (i >= 68 && i < 112) stream[i] = 0x80;
    }
    reference_adpcm(stream, NIBBLES, want);
    for (int k = 0; k < NIBBLES; k++) {
        if (want[k] < low) low = want[k];
        if (want[k] > high) high = want[k];
    }
    EQ("the stream reaches the upper clip", high, 0x7FFF);
    EQ("the stream reaches the lower clip", low, -0x7FFF);
    /* Decoded straight on, without reloading the decoder at the loop, the
     * second pass would differ at once and for a good while (until the
     * run of +7 values has both at the clip): the checks below can tell a
     * unit that does not reload. */
    memcpy(stream + WORDS * 4, stream + LOOP_WORD * 4, (WORDS - LOOP_WORD) * 4);
    reference_adpcm(stream, NIBBLES + (WORDS - LOOP_WORD) * 8, straight);
    for (int k = 0; k < NIBBLES - LOOP_NIBBLE; k++) unreloaded += straight[NIBBLES + k] != want[LOOP_NIBBLE + k];
    EQ("the loop start decodes differently without the reload", straight[NIBBLES] != want[LOOP_NIBBLE], 1);
    EQ("the second pass differs in 16 values or more without the reload", unreloaded >= 16, 1);

    /* GBATEK "DS Sound Notes": "ADPCM starts after 11 samples (3 dummy
     * samples as for PCM, plus 8 dummy samples for the ADPCM header)". "At
     * the loop end, the SAD value is reloaded to the loop start location,
     * additionally index and pcm16 values are reloaded to the values that
     * have originally appeared at that location": every pass over the loop
     * is the first pass again. */
    g_section = "ADPCM";
    for (int n = 0; n < NDS_SPU_CHANNELS; n += 15) {
        unsigned wrong = 0, wrong_level = 0;

        fresh(&spu);
        memcpy(&g_ram[DATA(n) - RAM_BASE], stream, WORDS * 4);
        setup(&spu, n, DATA(n), 0xFE00, LOOP_WORD, WORDS - LOOP_WORD);
        wr32(&spu, CH(n, 0), START | FORMAT(2) | REPEAT(1) | 127);
        for (int f = 1; f <= 11 + NIBBLES + 3 * (NIBBLES - LOOP_NIBBLE) + 7; f++) {
            int k = f - 11;
            int sample = k < 0 ? 0 : want[k < NIBBLES ? k : LOOP_NIBBLE + (k - LOOP_NIBBLE) % (NIBBLES - LOOP_NIBBLE)];

            frame(&spu);
            if (f <= 20 || (k >= NIBBLES - 2 && k < NIBBLES + 6)) {
                EQ(at("channel %d frame %d sample", n, f), nds_spu_channel_sample(&spu, n), sample);
            }
            wrong += nds_spu_channel_sample(&spu, n) != sample;
            wrong_level += LEFT(&spu) != want_left_full(sample);
        }
        EQ(at("channel %d frames off the reference decoder", n), wrong, 0);
        EQ(at("channel %d levels off the reference decoder", n), wrong_level, 0);
        EQ(at("channel %d busy", n), busy(&spu, n), 1);
        EQ(at("channel %d highest byte read", n), g_bus.read_end, DATA(n) + WORDS * 4u);
    }

    /* Every entry of the table, against the table GBATEK's recipe gives. */
    g_section = "ADPCM table walk";
    {
        static uint8_t walk[512];
        static int walk_want[1024];
        int unclipped = 0;
        int count = build_table_walk(walk, (int)sizeof walk, &unclipped);
        int words = (4 + (count + 1) / 2 + 3) / 4;
        int total = (words - 1) * 8;
        unsigned wrong = 0;

        EQ("table entries used at full magnitude, unclipped", unclipped, 89);
        EQ("the stream fits", count > 0 && total <= 1024, 1);
        reference_adpcm(walk, total, walk_want);
        fresh(&spu);
        memcpy(&g_ram[DATA(8) - RAM_BASE], walk, (size_t)words * 4u);
        setup(&spu, 8, DATA(8), 0xFE00, 0, (uint32_t)words);
        wr32(&spu, CH(8, 0), START | FORMAT(2) | REPEAT(2) | 127);
        for (int f = 1; f <= 10 + total; f++) {
            frame(&spu);
            wrong += nds_spu_channel_sample(&spu, 8) != (f >= 11 ? walk_want[f - 11] : 0);
        }
        EQ("frames off the reference decoder", wrong, 0);
        EQ("busy at the last sample", busy(&spu, 8), 0);
        frame(&spu);
        EQ("sample after the end", nds_spu_channel_sample(&spu, 8), 0);
    }

    /* The corners GBATEK spells out, by hand. Header: value -8000h, index
     * 100. Data 0, 8, 7, 0, then zeros.
     *   0: "Table[89..127]=0000h", so Diff = 0 and the value stays -8000h
     *      ("Max(+7FFFh) leaves -8000h unclipped"); Index = MinMax(99) = 88
     *   8: Diff = 7FFFh/8 = 4095; -32768 - 4095 clips to -7FFFh ("Min(-7FFFh)
     *      clips -8000h to -7FFFh"); Index = 87
     *   7: Table[87] = 7462h = 29794; Diff = 3724 + 7448 + 14897 + 29794 =
     *      55863; -32767 + 55863 = 23096; Index = MinMax(95) = 88
     *   0: Diff = 4095; 27191; Index = 87
     * and on up to the +7FFFh clip, where zeros keep it.
     * One-shot, 4 words: "8*(N-1) ADPCM samples", so 24, frames 11 to 34;
     * busy clears as the last one starts. */
    g_section = "ADPCM corners";
    fresh(&spu);
    put32(DATA(4), 0x00648000u);
    put8(DATA(4) + 4, 0x80);
    put8(DATA(4) + 5, 0x07);
    for (int i = 16; i < 64; i++) put8(DATA(4) + (uint32_t)i, 0xFF); /* never played */
    setup(&spu, 4, DATA(4), 0xFE00, 0, 4);
    wr32(&spu, CH(4, 0), START | FORMAT(2) | REPEAT(2) | 127);
    for (int f = 1; f <= 10; f++) {
        frame(&spu);
        EQ(at("frame %d sample", f), nds_spu_channel_sample(&spu, 4), 0);
    }
    frame(&spu);
    EQ("first value", nds_spu_channel_sample(&spu, 4), -32768);
    frame(&spu);
    EQ("second value", nds_spu_channel_sample(&spu, 4), -32767);
    frame(&spu);
    EQ("third value", nds_spu_channel_sample(&spu, 4), 23096);
    frame(&spu);
    EQ("fourth value", nds_spu_channel_sample(&spu, 4), 27191);
    frames(&spu, 19);
    EQ("frame 33 sample", nds_spu_channel_sample(&spu, 4), 0x7FFF);
    EQ("frame 33 busy", busy(&spu, 4), 1);
    frame(&spu);
    EQ("frame 34 sample", nds_spu_channel_sample(&spu, 4), 0x7FFF);
    EQ("frame 34 busy", busy(&spu, 4), 0);
    frame(&spu);
    EQ("frame 35 sample", nds_spu_channel_sample(&spu, 4), 0);
    EQ("words read", g_bus.reads, 4);
}

/* ------------------------------------------------------------------ */
/* Volume, panning, master volume                                      */
/* ------------------------------------------------------------------ */

static void test_volume(void)
{
    static NdsSpu spu;
    static const int samples[] = { 0x7FFF, -0x8000, 0x4000, -0x4000, 1, -1, 0x1234, -0x1235 };
    static const int settings[] = { 0, 1, 2, 63, 64, 65, 126, 127 };
    const int count = (int)(sizeof settings / sizeof settings[0]);

    /* GBATEK "Channel/Mixer Bit-Widths" and "7bit Volume and Panning
     * Values", through every divider and a spread of volume and pan
     * settings. The fields are rewritten under the playing channel by byte
     * and halfword writes, which must not disturb it. */
    g_section = "volume and pan";
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        unsigned wrong = 0, total = 0;

        fresh(&spu);
        start_constant(&spu, 4, samples[i], 127);
        frames(&spu, 3);
        EQ(at("sample %d playing", samples[i]), nds_spu_channel_sample(&spu, 4), samples[i]);
        for (int divider = 0; divider < 4; divider++) {
            for (int v = 0; v < count; v++) {
                for (int p = 0; p < count; p++) {
                    int64_t level = want_level(samples[i], divider, settings[v]);

                    wr16(&spu, CH(4, 0), DIVIDER(divider) | (uint32_t)settings[v]);
                    wr8(&spu, CH(4, 2), (uint32_t)settings[p]);
                    frame(&spu);
                    wrong += LEFT(&spu) != want_dac(want_side(level, settings[p], 0), 127, 0x200);
                    wrong += RIGHT(&spu) != want_dac(want_side(level, settings[p], 1), 127, 0x200);
                    total += 2;
                }
            }
        }
        EQ(at("sample %d: levels off, of %u", samples[i], total), wrong, 0);
        EQ(at("sample %d still playing", samples[i]), nds_spu_channel_sample(&spu, 4), samples[i]);
    }

    /* The same by hand for a sample of 4000h, a quarter of full scale
     * peak to peak: at full volume on one side it is 256 levels. */
    g_section = "volume and pan by hand";
    fresh(&spu);
    start_constant(&spu, 4, 0x4000, PAN(64) | 127);
    frames(&spu, 3);
    EQ("pan 64 left (\"half volume on both speakers\")", LEFT(&spu), 0x280);
    EQ("pan 64 right", RIGHT(&spu), 0x280);
    wr8(&spu, CH(4, 2), 0);
    frame(&spu);
    EQ("pan 0 left", LEFT(&spu), 0x300);
    EQ("pan 0 right", RIGHT(&spu), 0x200);
    wr8(&spu, CH(4, 2), 127);
    frame(&spu);
    EQ("pan 127 left", LEFT(&spu), 0x200);
    EQ("pan 127 right", RIGHT(&spu), 0x300);
    wr8(&spu, CH(4, 2), 126);
    frame(&spu);
    EQ("pan 126 left", LEFT(&spu), 0x204);
    EQ("pan 126 right", RIGHT(&spu), 0x2FC);
    wr8(&spu, CH(4, 2), 0);
    wr8(&spu, CH(4, 1), 1);
    frame(&spu);
    EQ("divider 1 (Div2)", LEFT(&spu), 0x280);
    wr8(&spu, CH(4, 1), 2);
    frame(&spu);
    EQ("divider 2 (Div4)", LEFT(&spu), 0x240);
    wr8(&spu, CH(4, 1), 3);
    frame(&spu);
    EQ("divider 3 (Div16)", LEFT(&spu), 0x210);
    wr8(&spu, CH(4, 1), 0);
    wr8(&spu, CH(4, 0), 64);
    frame(&spu);
    EQ("volume 64", LEFT(&spu), 0x280);
    wr8(&spu, CH(4, 0), 126);
    frame(&spu);
    EQ("volume 126", LEFT(&spu), 0x2FC);
    wr8(&spu, CH(4, 0), 1);
    frame(&spu);
    EQ("volume 1", LEFT(&spu), 0x202);
    wr8(&spu, CH(4, 0), 0);
    frame(&spu);
    EQ("volume 0", LEFT(&spu), 0x200);

    /* "Rounding Down" and "Strip fraction" are towards minus infinity: the
     * smallest negative signal still takes a whole level off, the
     * smallest positive one adds nothing. */
    fresh(&spu);
    start_constant(&spu, 4, -1, DIVIDER(3) | 1);
    frames(&spu, 3);
    EQ("-1 at the lowest volume", LEFT(&spu), 0x1FF);
    fresh(&spu);
    start_constant(&spu, 4, 1, DIVIDER(3) | 1);
    frames(&spu, 3);
    EQ("+1 at the lowest volume", LEFT(&spu), 0x200);

    /* "master.vol = data*N/128/64" */
    g_section = "master volume";
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        fresh(&spu);
        start_constant(&spu, 4, samples[i], PAN(32) | 127);
        frames(&spu, 3);
        for (int master = 0; master < 128; master++) {
            int64_t level = want_level(samples[i], 0, 127);

            wr8(&spu, SOUNDCNT, (uint32_t)master);
            frame(&spu);
            EQ(at("sample %d master %d left", samples[i], master), LEFT(&spu), want_dac(want_side(level, 32, 0), master, 0x200));
            EQ(at("sample %d master %d right", samples[i], master), RIGHT(&spu), want_dac(want_side(level, 32, 1), master, 0x200));
        }
    }
    fresh(&spu);
    start_constant(&spu, 4, 0x4000, 127);
    frames(&spu, 3);
    EQ("master 127 by hand", LEFT(&spu), 0x300);
    wr8(&spu, SOUNDCNT, 126);
    frame(&spu);
    EQ("master 126 by hand", LEFT(&spu), 0x2FC);
    wr8(&spu, SOUNDCNT, 64);
    frame(&spu);
    EQ("master 64 by hand", LEFT(&spu), 0x280);
    wr8(&spu, SOUNDCNT, 1);
    frame(&spu);
    EQ("master 1 by hand", LEFT(&spu), 0x202);
    wr8(&spu, SOUNDCNT, 0);
    frame(&spu);
    EQ("master 0 by hand", LEFT(&spu), 0x200);
}

/* ------------------------------------------------------------------ */
/* The mixer, the output selects, the "not to mixer" bits              */
/* ------------------------------------------------------------------ */

static void test_routing(void)
{
    static NdsSpu spu;
    /* Channel, sample, pan. By hand, in levels around the bias (a sample
     * of 2000h at full volume is 128 levels over both sides):
     *   channel 1  2000h, pan 32:  left 96, right 32
     *   channel 3  1000h, pan 96:  left 16, right 48
     *   channel 5  0800h, pan 64:  left 16, right 16 */
    static const struct {
        int channel, sample, pan;
    } k_voices[3] = { { 1, 0x2000, 32 }, { 3, 0x1000, 96 }, { 5, 0x0800, 64 } };
    int64_t side[3][2];

    g_section = "routing";
    fresh(&spu);
    for (int i = 0; i < 3; i++) {
        start_constant(&spu, k_voices[i].channel, k_voices[i].sample, PAN(k_voices[i].pan) | 127);
        for (int s = 0; s < 2; s++) side[i][s] = want_side(want_level(k_voices[i].sample, 0, 127), k_voices[i].pan, s);
    }
    frames(&spu, 3);

    /* GBATEK "DS Sound Control Registers": SOUNDCNT bits 8-9 "Left Output
     * from (0=Left Mixer, 1=Ch1, 2=Ch3, 3=Ch1+Ch3)", 10-11 the same for
     * the right, 12 "Output Ch1 to Mixer (0=Yes, 1=No) (both Left/Right)",
     * 13 the same for channel 3. In "DS Sound Block Diagrams" the selects
     * take channels 1 and 3 from ahead of those two switches. */
    for (int skip = 0; skip < 4; skip++) {
        for (int left = 0; left < 4; left++) {
            for (int right = 0; right < 4; right++) {
                int select[2];
                int64_t line[2];

                select[0] = left;
                select[1] = right;
                for (int s = 0; s < 2; s++) {
                    int64_t mixer = side[2][s] + (skip & 1 ? 0 : side[0][s]) + (skip & 2 ? 0 : side[1][s]);
                    line[s] = select[s] == 0 ? mixer : select[s] == 1 ? side[0][s] : select[s] == 2 ? side[1][s] : side[0][s] + side[1][s];
                }
                wr16(&spu, SOUNDCNT, 0x807Fu | ((uint32_t)left << 8) | ((uint32_t)right << 10) | ((uint32_t)skip << 12));
                frame(&spu);
                EQ(at("skip %d, left from %d", skip, left), LEFT(&spu), want_dac(line[0], 127, 0x200));
                EQ(at("skip %d, right from %d", skip, right), RIGHT(&spu), want_dac(line[1], 127, 0x200));
            }
        }
    }

    g_section = "routing by hand";
    wr16(&spu, SOUNDCNT, 0x807F);
    frame(&spu);
    EQ("mixers, left", LEFT(&spu), 0x200 + 96 + 16 + 16);
    EQ("mixers, right", RIGHT(&spu), 0x200 + 32 + 48 + 16);
    wr16(&spu, SOUNDCNT, 0x807F | 0x0100 | 0x0800);
    frame(&spu);
    EQ("left from channel 1", LEFT(&spu), 0x200 + 96);
    EQ("right from channel 3", RIGHT(&spu), 0x200 + 48);
    wr16(&spu, SOUNDCNT, 0x807F | 0x0200 | 0x0400);
    frame(&spu);
    EQ("left from channel 3", LEFT(&spu), 0x200 + 16);
    EQ("right from channel 1", RIGHT(&spu), 0x200 + 32);
    wr16(&spu, SOUNDCNT, 0x807F | 0x0300 | 0x0C00);
    frame(&spu);
    EQ("left from channels 1 and 3", LEFT(&spu), 0x200 + 96 + 16);
    EQ("right from channels 1 and 3", RIGHT(&spu), 0x200 + 32 + 48);
    wr16(&spu, SOUNDCNT, 0x807F | 0x1000);
    frame(&spu);
    EQ("channel 1 not to mixer, left", LEFT(&spu), 0x200 + 16 + 16);
    EQ("channel 1 not to mixer, right", RIGHT(&spu), 0x200 + 48 + 16);
    wr16(&spu, SOUNDCNT, 0x807F | 0x2000);
    frame(&spu);
    EQ("channel 3 not to mixer, left", LEFT(&spu), 0x200 + 96 + 16);
    EQ("channel 3 not to mixer, right", RIGHT(&spu), 0x200 + 32 + 16);
    wr16(&spu, SOUNDCNT, 0x807F | 0x3000 | 0x0100 | 0x0800);
    frame(&spu);
    EQ("neither to mixer, left from channel 1", LEFT(&spu), 0x200 + 96);
    EQ("neither to mixer, right from channel 3", RIGHT(&spu), 0x200 + 48);

    /* Sixteen channels add up. All at +7FFFh on the left they come to
     * 16 * 7FFF00h: at master volume 4 that is 255 levels (and a fraction),
     * at 127 far past the clip. */
    g_section = "sixteen channels";
    fresh(&spu);
    wr16(&spu, SOUNDBIAS, 0);
    for (int n = 0; n < NDS_SPU_CHANNELS; n++) start_constant(&spu, n, 0x7FFF, 127);
    frames(&spu, 3);
    EQ("master 127", LEFT(&spu), 0x3FF);
    wr8(&spu, SOUNDCNT, 4);
    frame(&spu);
    EQ("master 4", LEFT(&spu), 255);
    wr8(&spu, SOUNDCNT, 8);
    frame(&spu);
    EQ("master 8", LEFT(&spu), 511);
    EQ("the right mixer is empty", RIGHT(&spu), 0);
}

/* ------------------------------------------------------------------ */
/* Bias, clipping, master enable, the host sample                      */
/* ------------------------------------------------------------------ */

static void test_output_stage(void)
{
    static NdsSpu spu;
    static const int biases[] = { 0, 1, 0x155, 0x1FF, 0x200, 0x201, 0x3FE, 0x3FF };

    /* GBATEK "DS Sound Control Registers": "the signed left/right audio
     * signals are in range -200h..+1FFh (with medium level zero), the Bias
     * value is then added"; "Channel/Mixer Bit-Widths" step 9 "Clip
     * (min/max 0h..3FFh)". One channel at full volume on the left is +511
     * or -512, two are twice that. */
    g_section = "bias and clipping";
    for (int voices = 1; voices <= 2; voices++) {
        for (int negative = 0; negative < 2; negative++) {
            int sample = negative ? -0x8000 : 0x7FFF;
            int signal = voices * (negative ? -512 : 511) + (voices == 2 && !negative ? 1 : 0); /* 2 * 511.98 = 1023 */

            fresh(&spu);
            start_constant(&spu, 6, sample, 127);
            if (voices == 2) start_constant(&spu, 7, sample, 127);
            frames(&spu, 3);
            for (size_t b = 0; b < sizeof biases / sizeof biases[0]; b++) {
                int level = signal + biases[b];

                wr16(&spu, SOUNDBIAS, (uint32_t)biases[b]);
                frame(&spu);
                EQ(at("%d x %d, bias %03X, left", voices, sample, (unsigned)biases[b]), LEFT(&spu),
                   level < 0 ? 0 : level > 0x3FF ? 0x3FF : level);
                EQ(at("%d x %d, bias %03X, right", voices, sample, (unsigned)biases[b]), RIGHT(&spu), biases[b]);
            }
        }
    }
    fresh(&spu);
    start_constant(&spu, 6, 0x7FFF, 127);
    frames(&spu, 3);
    EQ("+7FFFh, bias 200h: the upper rail exactly", LEFT(&spu), 0x3FF);
    wr16(&spu, SOUNDBIAS, 0x1FF);
    frame(&spu);
    EQ("+7FFFh, bias 1FFh: one under it", LEFT(&spu), 0x3FE);
    wr16(&spu, SOUNDBIAS, 0x3FF);
    frame(&spu);
    EQ("+7FFFh, bias 3FFh: clipped", LEFT(&spu), 0x3FF);
    fresh(&spu);
    start_constant(&spu, 6, -0x8000, 127);
    frames(&spu, 3);
    EQ("-8000h, bias 200h: the lower rail exactly", LEFT(&spu), 0x000);
    wr16(&spu, SOUNDBIAS, 0x201);
    frame(&spu);
    EQ("-8000h, bias 201h: one over it", LEFT(&spu), 0x001);
    wr16(&spu, SOUNDBIAS, 0);
    frame(&spu);
    EQ("-8000h, bias 0: clipped", LEFT(&spu), 0x000);

    /* The host sample: (level - 0x200) * 64. frame() checks that on every
     * frame of every test; these are its ends and middle. */
    g_section = "host sample";
    fresh(&spu);
    start_constant(&spu, 6, 0x7FFF, 127);
    start_constant(&spu, 7, -0x8000, PAN(127) | 127);
    frames(&spu, 3);
    EQ("left level", LEFT(&spu), 0x3FF);
    EQ("right level", RIGHT(&spu), 0x000);
    EQ("left host sample at the upper rail", g_out[0], 32704);
    EQ("right host sample at the lower rail", g_out[1], -32768);
    wr8(&spu, CH(6, 0), 0);
    wr8(&spu, CH(7, 0), 0);
    frame(&spu);
    EQ("left host sample in silence", g_out[0], 0);
    EQ("right host sample in silence", g_out[1], 0);
    wr16(&spu, SOUNDBIAS, 0x201);
    frame(&spu);
    EQ("host sample one level up", g_out[0], 64);

    /* "BIAS output is always enabled, even when Master Enable
     * (SOUNDCNT.15) is off." What the channels do meanwhile GBATEK does
     * not say; they stand still, as in melonDS and DeSmuME, and go on from
     * where they were. */
    g_section = "master enable";
    fresh(&spu);
    start_ramp(&spu, 6, 127);
    frames(&spu, 10);
    EQ("sample before", nds_spu_channel_sample(&spu, 6), ramp(7));
    wr16(&spu, SOUNDCNT, 0x007F);
    for (size_t b = 0; b < sizeof biases / sizeof biases[0]; b++) {
        wr16(&spu, SOUNDBIAS, (uint32_t)biases[b]);
        frame(&spu);
        EQ(at("disabled, bias %03X, left", (unsigned)biases[b]), LEFT(&spu), biases[b]);
        EQ(at("disabled, bias %03X, right", (unsigned)biases[b]), RIGHT(&spu), biases[b]);
    }
    EQ("busy while disabled", busy(&spu, 6), 1);
    wr16(&spu, SOUNDBIAS, 0x200);
    wr16(&spu, SOUNDCNT, 0x807F);
    frame(&spu);
    EQ("sample after", nds_spu_channel_sample(&spu, 6), ramp(8));
    EQ("level after", LEFT(&spu), want_left_full(ramp(8)));
}

/* ------------------------------------------------------------------ */
/* Hold                                                                */
/* ------------------------------------------------------------------ */

/* GBATEK "DS Sound Notes", Sound Stop and Hold Flag. The sample period is
 * two frames here (SOUNDxTMR = 0xFC00), so that the first period after a
 * restart can be seen: overflow i falls in frame 2i. A 16-sample one-shot
 * shows sample k from frame 2(k + 3); its last from frame 36, when busy
 * clears, to the end of that period at frame 38. */
static void test_hold(void)
{
    static NdsSpu spu;
    const int last = pattern8(15) * 256;

    for (int hold = 0; hold < 2; hold++) {
        g_section = hold ? "hold" : "no hold";
        fresh(&spu);
        for (int i = 0; i < 16; i++) put8(DATA(0) + (uint32_t)i, pattern8(i));
        setup(&spu, 0, DATA(0), 0xFC00, 0, 4);
        wr32(&spu, CH(0, 0), START | FORMAT(0) | REPEAT(2) | (hold ? HOLD : 0) | 127);
        frames(&spu, 35);
        EQ("frame 35 sample", nds_spu_channel_sample(&spu, 0), pattern8(14) * 256);
        EQ("frame 35 busy", busy(&spu, 0), 1);
        frame(&spu);
        EQ("frame 36 sample", nds_spu_channel_sample(&spu, 0), last);
        EQ("frame 36 busy", busy(&spu, 0), 0);
        wr8(&spu, CH(0, 0), 127); /* a write to another byte of SOUNDxCNT does not cut the period short */
        frame(&spu);
        EQ("frame 37 sample: \"kept output until the END of the last sample period\"", nds_spu_channel_sample(&spu, 0), last);
        for (int f = 38; f <= 60; f++) {
            frame(&spu);
            EQ(at("frame %d sample", f), nds_spu_channel_sample(&spu, 0), hold ? last : 0);
            EQ(at("frame %d level", f), LEFT(&spu), want_left_full(hold ? last : 0));
        }
        EQ("busy", busy(&spu, 0), 0);
    }

    /* Still holding. "With Hold flag set, old output level is acually kept
     * intact during the 1st sample, but the output level drops to zero
     * during 2nd-3rd sample, before starting the new sound in 4th sample." */
    g_section = "hold across a restart";
    wr32(&spu, CH(0, 0), START | FORMAT(0) | REPEAT(2) | HOLD | 127);
    frame(&spu);
    EQ("first period", nds_spu_channel_sample(&spu, 0), last);
    for (int f = 2; f <= 5; f++) {
        frame(&spu);
        EQ(at("frame %d, second and third periods", f), nds_spu_channel_sample(&spu, 0), 0);
    }
    frame(&spu);
    EQ("fourth period", nds_spu_channel_sample(&spu, 0), pattern8(0) * 256);

    /* "...kept output infinitely, that is, until Hold gets cleared". */
    g_section = "hold cleared";
    frames(&spu, 40);
    EQ("holding again", nds_spu_channel_sample(&spu, 0), last);
    wr8(&spu, CH(0, 1), 0);
    frame(&spu);
    EQ("after clearing the hold bit", nds_spu_channel_sample(&spu, 0), 0);
    EQ("level", LEFT(&spu), 0x200);
    wr8(&spu, CH(0, 1), HOLD >> 8);
    frame(&spu);
    EQ("setting it again brings nothing back", nds_spu_channel_sample(&spu, 0), 0);

    /* The flag is for the end of a one-shot: a sound stopped by hand
     * leaves nothing behind. */
    g_section = "hold and a stop by hand";
    fresh(&spu);
    start_constant(&spu, 0, 0x1234, HOLD | 127);
    frames(&spu, 5);
    EQ("playing", nds_spu_channel_sample(&spu, 0), 0x1234);
    wr32(&spu, CH(0, 0), FORMAT(1) | REPEAT(1) | HOLD | 127);
    frame(&spu);
    EQ("stopped", nds_spu_channel_sample(&spu, 0), 0);
}

/* ------------------------------------------------------------------ */
/* Capture                                                             */
/* ------------------------------------------------------------------ */

/* A mixer's output (16.8) at the end of a frame, with the ramp on channel
 * 4 at pan 32: three quarters of it on the left, a quarter on the right,
 * so that the fractions GBATEK's rounding rule turns on all occur. */
static int64_t ramp_mixer(int frame_number, int right)
{
    return want_side(want_level(ramp(frame_number - 3), 0, 127), 32, right);
}

static void test_capture_mixer(void)
{
    static NdsSpu spu;
    unsigned writes_before;

    /* GBATEK "DS Sound Capture": unit 0 hears the left mixer, unit 1 the
     * right; bit 2 one-shot, bit 3 PCM8, bit 7 start/busy; the length is
     * in words; the rate is channel 1's (unit 0) or channel 3's (unit 1)
     * timer. With that timer at one sample a frame, sample j is the mixer
     * at the end of frame j + 1 (no start delay: melonDS; GBATEK gives
     * none). */
    g_section = "capture mixer, PCM16, one-shot";
    fresh(&spu);
    start_ramp(&spu, 4, PAN(32) | 127);
    wr16(&spu, CH(1, 8), 0xFE00);
    wr16(&spu, CH(3, 8), 0xFE00);
    for (int x = 0; x < 2; x++) {
        wr32(&spu, SNDCAPDAD(x), CAPBUF(x) | 0xF8000003u); /* 27 bits, word aligned */
        wr16(&spu, SNDCAPLEN(x), 6);
        wr8(&spu, SNDCAPCNT(x), 0x84);
    }
    /* The unit writes through a 4-word FIFO (the size is melonDS's): after
     * 7 samples memory is untouched, the 8th fills and empties it, and the
     * end of the buffer empties what is left. */
    frames(&spu, 5);
    wr16(&spu, SNDCAPCNT(0), 0x8484); /* 1 over 1: nothing starts again */
    frames(&spu, 2);
    EQ("words written after 7 samples", g_bus.writes, 0);
    EQ("unit 0 busy", rd8(&spu, SNDCAPCNT(0)), 0x84);
    /* SNDCAPxDAD reads back as written while the unit runs (melonDS,
     * DeSmuME; GBATEK only marks it R/W). */
    EQ("SNDCAP0DAD while capturing", rd32(&spu, SNDCAPDAD(0)), CAPBUF(0));
    frame(&spu);
    EQ("words written after 8 samples", g_bus.writes, 8);
    frames(&spu, 3);
    EQ("words written after 11 samples", g_bus.writes, 8);
    EQ("unit 1 busy", rd8(&spu, SNDCAPCNT(1)), 0x84);
    frame(&spu);
    EQ("words written after 12 samples", g_bus.writes, 12);
    EQ("unit 0 done", rd8(&spu, SNDCAPCNT(0)), 0x04);
    EQ("unit 1 done", rd8(&spu, SNDCAPCNT(1)), 0x04);
    for (int x = 0; x < 2; x++) {
        for (int j = 0; j < 12; j++) {
            EQ(at("unit %d sample %d", x, j), get16s(CAPBUF(x) + 2u * (uint32_t)j), want_capture(clip_mixer(ramp_mixer(j + 1, x)), 8));
        }
        EQ(at("unit %d wrote nothing past its buffer", x), get16s(CAPBUF(x) + 24u), 0);
    }
    frames(&spu, 30);
    EQ("words written once done", g_bus.writes, 12);

    /* "For PCM8 capture format, the 16.8 bits are divided by 100h". 20
     * samples: a full FIFO after 16, the last word at the end. */
    g_section = "capture mixer, PCM8, one-shot";
    fresh(&spu);
    start_ramp(&spu, 4, PAN(32) | 127);
    wr16(&spu, CH(1, 8), 0xFE00);
    wr16(&spu, CH(3, 8), 0xFE00);
    for (int x = 0; x < 2; x++) {
        wr32(&spu, SNDCAPDAD(x), CAPBUF(x));
        wr16(&spu, SNDCAPLEN(x), 5);
        wr8(&spu, SNDCAPCNT(x), 0x8C);
    }
    frames(&spu, 15);
    EQ("words written after 15 samples", g_bus.writes, 0);
    frame(&spu);
    EQ("words written after 16 samples", g_bus.writes, 8);
    frames(&spu, 4);
    EQ("words written after 20 samples", g_bus.writes, 10);
    EQ("unit 0 done", rd8(&spu, SNDCAPCNT(0)), 0x0C);
    for (int x = 0; x < 2; x++) {
        for (int j = 0; j < 20; j++) {
            EQ(at("unit %d sample %d", x, j), get8s(CAPBUF(x) + (uint32_t)j), want_capture(clip_mixer(ramp_mixer(j + 1, x)), 16));
        }
        EQ(at("unit %d wrote nothing past its buffer", x), get8s(CAPBUF(x) + 20u), 0);
    }

    /* "Capture Repeat (0=Loop, 1=One-shot)"; SNDCAPxDAD is "also used as
     * re-start address for looped capture". Four samples a lap; after ten
     * frames the buffer holds the second lap and the third is under way. */
    g_section = "capture mixer, looped";
    for (int pcm8 = 0; pcm8 < 2; pcm8++) {
        fresh(&spu);
        start_ramp(&spu, 4, PAN(32) | 127);
        wr16(&spu, CH(1, 8), 0xFE00);
        wr16(&spu, CH(3, 8), 0xFE00);
        for (int x = 0; x < 2; x++) {
            wr32(&spu, SNDCAPDAD(x), CAPBUF(x));
            wr16(&spu, SNDCAPLEN(x), pcm8 ? 1 : 2);
            wr8(&spu, SNDCAPCNT(x), pcm8 ? 0x88 : 0x80);
        }
        frames(&spu, 10);
        for (int x = 0; x < 2; x++) {
            EQ(at("%s unit %d still busy", pcm8 ? "PCM8" : "PCM16", x), rd8(&spu, SNDCAPCNT(x)), pcm8 ? 0x88 : 0x80);
            for (int j = 4; j < 8; j++) {
                int got = pcm8 ? get8s(CAPBUF(x) + (uint32_t)(j - 4)) : get16s(CAPBUF(x) + 2u * (uint32_t)(j - 4));
                EQ(at("%s unit %d sample %d", pcm8 ? "PCM8" : "PCM16", x, j), got,
                   want_capture(clip_mixer(ramp_mixer(j + 1, x)), pcm8 ? 16 : 8));
            }
        }
        EQ(at("%s words written", pcm8 ? "PCM8" : "PCM16"), g_bus.writes, pcm8 ? 4 : 8);
    }

    /* "Incoming mixer data (20.8bits) is clipped to 16.8bits (MinMax
     * -8000h..7FFFh)": two channels at a rail overflow 16 bits. */
    g_section = "capture mixer, clipped";
    for (int negative = 0; negative < 2; negative++) {
        fresh(&spu);
        start_constant(&spu, 4, negative ? -0x8000 : 0x7FFF, 127);
        start_constant(&spu, 5, negative ? -0x8000 : 0x7FFF, 127);
        wr16(&spu, CH(1, 8), 0xFE00);
        wr16(&spu, CH(3, 8), 0xFE00);
        frames(&spu, 3);
        wr32(&spu, SNDCAPDAD(0), CAPBUF(0));
        wr16(&spu, SNDCAPLEN(0), 1);
        wr8(&spu, SNDCAPCNT(0), 0x84);
        wr32(&spu, SNDCAPDAD(1), CAPBUF(1));
        wr16(&spu, SNDCAPLEN(1), 1);
        wr8(&spu, SNDCAPCNT(1), 0x8C);
        frames(&spu, 2);
        EQ(negative ? "PCM16, two at -8000h" : "PCM16, two at +7FFFh", get16s(CAPBUF(0)), negative ? -0x8000 : 0x7FFF);
        frames(&spu, 2);
        /* Unit 1 hears the right mixer, which is empty. */
        EQ("PCM8 of an empty mixer", get8s(CAPBUF(1)), 0);
        wr8(&spu, CH(4, 2), 127);
        wr8(&spu, CH(5, 2), 127);
        wr8(&spu, SNDCAPCNT(1), 0x8C);
        frames(&spu, 4);
        EQ(negative ? "PCM8, two at -8000h" : "PCM8, two at +7FFFh", get8s(CAPBUF(1)), negative ? -0x80 : 0x7F);
    }

    /* "Minimum length is 1 word (attempts to use 0 words are interpreted
     * as 1 word)." */
    g_section = "capture length 0";
    fresh(&spu);
    start_constant(&spu, 4, 0x1234, 127);
    wr16(&spu, CH(1, 8), 0xFE00);
    frames(&spu, 3);
    wr32(&spu, SNDCAPDAD(0), CAPBUF(0));
    wr16(&spu, SNDCAPLEN(0), 0);
    wr8(&spu, SNDCAPCNT(0), 0x84);
    frame(&spu);
    EQ("busy after one sample", rd8(&spu, SNDCAPCNT(0)), 0x84);
    frame(&spu);
    EQ("done after two", rd8(&spu, SNDCAPCNT(0)), 0x04);
    EQ("words written", g_bus.writes, 1);
    EQ("first sample", get16s(CAPBUF(0)), 0x1234);
    EQ("second sample", get16s(CAPBUF(0) + 2), 0x1234);

    /* "the sample frequency of Channel 1/3 is shared for Capture 0/1".
     * Unit 0 at two samples a frame takes each frame twice; unit 1 at one
     * in two frames takes every other frame. The ramp has run ten frames
     * when they start. */
    g_section = "capture rate";
    fresh(&spu);
    start_ramp(&spu, 4, PAN(64) | 127);
    wr16(&spu, CH(1, 8), 0xFF00);
    wr16(&spu, CH(3, 8), 0xFC00);
    frames(&spu, 10);
    for (int x = 0; x < 2; x++) {
        wr32(&spu, SNDCAPDAD(x), CAPBUF(x));
        wr16(&spu, SNDCAPLEN(x), 4);
        wr8(&spu, SNDCAPCNT(x), 0x84);
    }
    frames(&spu, 4);
    EQ("unit 0 done after 4 frames", rd8(&spu, SNDCAPCNT(0)), 0x04);
    frames(&spu, 11);
    EQ("unit 1 busy after 15 frames", rd8(&spu, SNDCAPCNT(1)), 0x84);
    frame(&spu);
    EQ("unit 1 done after 16 frames", rd8(&spu, SNDCAPCNT(1)), 0x04);
    for (int j = 0; j < 8; j++) {
        int64_t fast = want_side(want_level(ramp(10 + (j / 2 + 1) - 3), 0, 127), 64, 0);
        int64_t slow = want_side(want_level(ramp(10 + 2 * (j + 1) - 3), 0, 127), 64, 1);

        EQ(at("unit 0 sample %d", j), get16s(CAPBUF(0) + 2u * (uint32_t)j), want_capture(clip_mixer(fast), 8));
        EQ(at("unit 1 sample %d", j), get16s(CAPBUF(1) + 2u * (uint32_t)j), want_capture(clip_mixer(slow), 8));
    }

    /* A unit started again fills its buffer from the top; one stopped by
     * hand writes nothing more, not even what its FIFO was holding. */
    g_section = "capture restart and stop";
    writes_before = g_bus.writes;
    wr8(&spu, SNDCAPCNT(0), 0x84);
    frames(&spu, 4);
    EQ("words written by the second run", g_bus.writes - writes_before, 4);
    for (int j = 0; j < 8; j++) {
        int64_t fast = want_side(want_level(ramp(26 + (j / 2 + 1) - 3), 0, 127), 64, 0);
        EQ(at("second run sample %d", j), get16s(CAPBUF(0) + 2u * (uint32_t)j), want_capture(clip_mixer(fast), 8));
    }
    writes_before = g_bus.writes;
    wr16(&spu, SNDCAPLEN(0), 16);
    wr8(&spu, SNDCAPCNT(0), 0x80);
    frames(&spu, 5); /* ten samples: one FIFO written out, 4 bytes waiting */
    EQ("words written before the stop", g_bus.writes - writes_before, 4);
    writes_before = g_bus.writes;
    wr8(&spu, SNDCAPCNT(0), 0x00);
    EQ("stopped", rd8(&spu, SNDCAPCNT(0)), 0x00);
    frames(&spu, 20);
    EQ("words written after the stop", g_bus.writes - writes_before, 0);
    /* 55 frames have passed. The next run starts at the top of the buffer
     * again, with nothing left over in the FIFO. */
    wr16(&spu, SNDCAPLEN(0), 2);
    wr8(&spu, SNDCAPCNT(0), 0x84);
    frames(&spu, 2);
    EQ("third run done", rd8(&spu, SNDCAPCNT(0)), 0x04);
    EQ("words written by the third run", g_bus.writes - writes_before, 2);
    for (int j = 0; j < 4; j++) {
        int64_t fast = want_side(want_level(ramp(55 + (j / 2 + 1) - 3), 0, 127), 64, 0);
        EQ(at("third run sample %d", j), get16s(CAPBUF(0) + 2u * (uint32_t)j), want_capture(clip_mixer(fast), 8));
    }
}

static void test_capture_channel(void)
{
    static NdsSpu spu;
    static const int first[8] = { 0x1000, -0x1000, 0x2000, -0x2000, 0x0001, -0x0001, 0x7FFF, -0x8000 };
    static const int second[8] = { -0x0100, -0x0100, 0x0100, 0x0100, -0x0001, -0x0001, 0x0001, -0x8000 };

    /* GBATEK "DS Sound Capture": bit 1 selects channel 0 (unit 0) or
     * channel 2 (unit 1) as the source, which "DS Sound Block Diagrams"
     * takes after the channel's volume and before its panning: 16 bits
     * and a fraction, rounded like mixer data. The channel is at a quarter
     * volume, so the fractions are quarters, and panned hard to the other
     * side, so the unit's own mixer is empty. */
    for (int x = 0; x < 2; x++) {
        int a = 2 * x, b = 2 * x + 1;

        for (int pcm8 = 0; pcm8 < 2; pcm8++) {
            g_section = pcm8 ? "capture channel, PCM8" : "capture channel, PCM16";
            fresh(&spu);
            start_ramp(&spu, a, DIVIDER(1) | PAN(x ? 0 : 127) | 64);
            wr16(&spu, CH(b, 8), 0xFE00);
            wr32(&spu, SNDCAPDAD(x), CAPBUF(x));
            wr16(&spu, SNDCAPLEN(x), pcm8 ? 4 : 8);
            wr8(&spu, SNDCAPCNT(x), pcm8 ? 0x8E : 0x86);
            frames(&spu, 16);
            EQ(at("unit %d done", x), rd8(&spu, SNDCAPCNT(x)), pcm8 ? 0x0E : 0x06);
            for (int j = 0; j < 16; j++) {
                int64_t level = want_level(ramp(j + 1 - 3), 1, 64);
                int got = pcm8 ? get8s(CAPBUF(x) + (uint32_t)j) : get16s(CAPBUF(x) + 2u * (uint32_t)j);

                EQ(at("unit %d sample %d", x, j), got, want_capture(level, pcm8 ? 19 : 11));
            }
        }

        /* "Capture Bugs", 1) Both Negative Bug, addition disabled:
         * "Capture data is accidently set to -8000h if ch(a) and ch(b) are
         * both <0. Otherwise the correct capture result is returned, ie.
         * plain ch(a) data". */
        g_section = "capture channel, both negative";
        for (int silent_b = 0; silent_b < 2; silent_b++) {
            fresh(&spu);
            for (int k = 0; k < 8; k++) {
                put16(DATA(a) + 2u * (uint32_t)k, first[k]);
                put16(DATA(b) + 2u * (uint32_t)k, second[k]);
            }
            setup(&spu, a, DATA(a), 0xFE00, 0, 4);
            setup(&spu, b, DATA(b), 0xFE00, 0, 4);
            wr32(&spu, CH(a, 0), START | FORMAT(1) | REPEAT(1) | 127);
            /* At volume 0 channel b is not negative, whatever it plays. */
            wr32(&spu, CH(b, 0), START | FORMAT(1) | REPEAT(1) | (silent_b ? 0 : 127));
            wr32(&spu, SNDCAPDAD(x), CAPBUF(x));
            wr16(&spu, SNDCAPLEN(x), 8);
            wr8(&spu, SNDCAPCNT(x), 0x82);
            frames(&spu, 16);
            for (int j = 0; j < 16; j++) {
                int k = j + 1 - 3;
                int want = k < 0 ? 0 : first[k % 8];

                if (k >= 0 && !silent_b && first[k % 8] < 0 && second[k % 8] < 0) want = -0x8000;
                EQ(at("unit %d%s sample %d", x, silent_b ? ", other channel at volume 0," : "", j),
                   get16s(CAPBUF(x) + 2u * (uint32_t)j), want);
            }
        }
    }
}

/* GBATEK "DS Sound Capture", ch(a)+ch(b) Channel Addition and Capture
 * Bugs 2). For unit 0 a=0, b=1; for unit 1 a=2, b=3. */
static void test_capture_addition(void)
{
    static NdsSpu spu;
    static const struct {
        int a, b, want;
        const char *what;
    } k_sums[] = {
        { 0x1000, 0x0234, 0x1234, "a sum that fits" },
        /* "it is ANDed with FFFFh, so the sign bit is lost if the addition
         * result ch(a)+ch(b) is less/greater than -8000h/+7FFFh" */
        { 0x7000, 0x7000, -0x2000, "E000h" },
        { -0x7000, -0x7000, 0x2000, "-E000h" },
    };

    for (int x = 0; x < 2; x++) {
        int a = 2 * x, b = 2 * x + 1;
        uint32_t skip_b = 0x1000u << x;

        g_section = "capture channel addition";
        for (size_t i = 0; i < sizeof k_sums / sizeof k_sums[0]; i++) {
            fresh(&spu);
            start_constant(&spu, a, k_sums[i].a, 127);
            start_constant(&spu, b, k_sums[i].b, 127);
            /* "Addition is performed always, no matter of
             * SOUNDCNT.Bit12/13" on the capture side. */
            wr16(&spu, SOUNDCNT, 0x807Fu | (i & 1 ? skip_b : 0));
            wr32(&spu, SNDCAPDAD(x), CAPBUF(x));
            wr16(&spu, SNDCAPLEN(x), 1);
            wr8(&spu, SNDCAPCNT(x), 0x83);
            frames(&spu, 6);
            EQ(at("unit %d, %s", x, k_sums[i].what), get16s(CAPBUF(x)), k_sums[i].want);
            EQ(at("unit %d, %s, second sample", x, k_sums[i].what), get16s(CAPBUF(x) + 2), k_sums[i].want);
        }
        /* "no matter of ch(a) enable, result is plain ch(b) if ch(a) is
         * disabled." */
        fresh(&spu);
        start_constant(&spu, b, 0x0800, 127);
        wr32(&spu, SNDCAPDAD(x), CAPBUF(x));
        wr16(&spu, SNDCAPLEN(x), 1);
        wr8(&spu, SNDCAPCNT(x), 0x83);
        frames(&spu, 6);
        EQ(at("unit %d with channel %d off", x, a), get16s(CAPBUF(x)), 0x0800);

        /* The mixer side. Channel a is 1000h on the left (64 levels),
         * channel b 0800h on the right (32). Added, b comes out where a is
         * panned and nowhere else: "Output Sound Channel 1 (0=As such,
         * 1=Add to Channel 0)". */
        g_section = "channel addition to the mixer";
        fresh(&spu);
        start_constant(&spu, a, 0x1000, PAN(0) | 127);
        start_constant(&spu, b, 0x0800, PAN(127) | 127);
        wr32(&spu, SNDCAPDAD(x), CAPBUF(x));
        wr16(&spu, SNDCAPLEN(x), 0x100);
        frames(&spu, 3);
        EQ(at("unit %d off: left", x), LEFT(&spu), 0x240);
        EQ(at("unit %d off: right", x), RIGHT(&spu), 0x220);
        /* "Caution: Addition mode works only if BOTH Bit0 and Bit7 are set." */
        wr8(&spu, SNDCAPCNT(x), 0x01);
        frame(&spu);
        EQ(at("unit %d bit 0 alone: left", x), LEFT(&spu), 0x240);
        EQ(at("unit %d bit 0 alone: right", x), RIGHT(&spu), 0x220);
        wr8(&spu, SNDCAPCNT(x), 0x80);
        frame(&spu);
        EQ(at("unit %d bit 7 alone: left", x), LEFT(&spu), 0x240);
        EQ(at("unit %d bit 7 alone: right", x), RIGHT(&spu), 0x220);
        wr8(&spu, SNDCAPCNT(x), 0x81);
        frame(&spu);
        EQ(at("unit %d adding: left", x), LEFT(&spu), 0x260);
        EQ(at("unit %d adding: right", x), RIGHT(&spu), 0x200);
        /* "Ch(b) is muted if ch(b) SOUNDCNT.Bit12/13 is set to 'Ch(b) not
         * to mixer'." */
        wr16(&spu, SOUNDCNT, 0x807Fu | skip_b);
        frame(&spu);
        EQ(at("unit %d adding, channel %d not to mixer: left", x, b), LEFT(&spu), 0x240);
        EQ(at("unit %d adding, channel %d not to mixer: right", x, b), RIGHT(&spu), 0x200);
        /* "Ch(b) is muted if ch(a) is disabled." */
        wr16(&spu, SOUNDCNT, 0x807F);
        wr32(&spu, CH(a, 0), FORMAT(1) | REPEAT(1) | 127);
        frame(&spu);
        EQ(at("unit %d adding, channel %d stopped: left", x, a), LEFT(&spu), 0x200);
        EQ(at("unit %d adding, channel %d stopped: right", x, a), RIGHT(&spu), 0x200);
    }
}

/* What the FIFOs are for: a capture unit records the left mixer into a
 * 16-sample buffer, round and round, while channel 1 plays that same
 * buffer at the same rate and is the left output (the arrangement games
 * use for reverb and surround). The channel fetches each part of the
 * buffer before the unit overwrites it, so what it plays is the lap
 * before: its sample k, which it holds at the end of its frame k + 3, is
 * capture sample k - 16.
 *
 * A sound driver starts both with consecutive register writes, which can
 * fall either side of a mixer period, so the unit is also started `lead`
 * frames after the channel (before it, if negative). Capture sample j is
 * then the mixer at the end of the channel's frame lead + j + 1. */
static void test_capture_playback(void)
{
    static NdsSpu spu;

    g_section = "capture played back";
    for (int lead = -2; lead <= 1; lead++) {
        unsigned wrong = 0, wrong_level = 0;
        int ramp_frames = 40 + (lead < 0 ? -lead : 0); /* how long the ramp has run when channel 1 starts */

        fresh(&spu);
        start_ramp(&spu, 4, 127);
        setup(&spu, 1, CAPBUF(0), 0xFE00, 0, 8);
        wr32(&spu, SNDCAPDAD(0), CAPBUF(0));
        wr16(&spu, SNDCAPLEN(0), 8);
        wr16(&spu, SOUNDCNT, 0x807F | 0x0100 | 0x1000); /* left from channel 1, channel 1 not to mixer */
        frames(&spu, 40);
        if (lead < 0) {
            wr8(&spu, SNDCAPCNT(0), 0x80);
            frames(&spu, -lead);
            wr32(&spu, CH(1, 0), START | FORMAT(1) | REPEAT(1) | 127);
        } else {
            wr32(&spu, CH(1, 0), START | FORMAT(1) | REPEAT(1) | 127);
            frames(&spu, lead);
            wr8(&spu, SNDCAPCNT(0), 0x80);
        }
        for (int f = (lead > 0 ? lead : 0) + 1; f <= 400; f++) {
            /* Channel 1's sample f - 3 is capture sample f - 19: the mixer
             * of frame lead + f - 18, where the ramp has been playing for
             * ramp_frames more. Before that, the buffer's first contents. */
            int want = f < 19 ? 0 : ramp(ramp_frames + lead + f - 18 - 3);

            frame(&spu);
            if (f >= 17 && f < 40) EQ(at("lead %d frame %d channel 1", lead, f), nds_spu_channel_sample(&spu, 1), want);
            wrong += nds_spu_channel_sample(&spu, 1) != want;
            wrong_level += LEFT(&spu) != want_left_full(want) || RIGHT(&spu) != 0x200;
        }
        EQ(at("lead %d: frames where channel 1 was not one lap behind", lead), wrong, 0);
        EQ(at("lead %d: frames where the output was not channel 1", lead), wrong_level, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Saving and restoring                                                */
/* ------------------------------------------------------------------ */

/* A busy scene: every format, a capture unit and a channel playing its
 * buffer. */
static void start_scene(NdsSpu *spu)
{
    uint32_t seed = 0xC0FFEEu;

    fresh(spu);
    put32(DATA(0), 0x00100040u);
    for (int i = 4; i < 256; i++) {
        seed = seed * 1103515245u + 12345u;
        put8(DATA(0) + (uint32_t)i, (int)(seed >> 16));
        put8(DATA(2) + (uint32_t)i, (int)(seed >> 8));
    }
    setup(spu, 0, DATA(0), 0xFD80, 5, 59);
    wr32(spu, CH(0, 0), START | FORMAT(2) | REPEAT(1) | PAN(20) | 100);
    setup(spu, 2, DATA(2), 0xFE80, 2, 30);
    wr32(spu, CH(2, 0), START | FORMAT(0) | REPEAT(1) | PAN(100) | DIVIDER(1) | 90);
    start_ramp(spu, 4, PAN(64) | 80);
    wr16(spu, CH(4, 8), 0xFF30);
    wr16(spu, CH(9, 8), 0xFFA0);
    wr32(spu, CH(9, 0), START | FORMAT(3) | DUTY(2) | PAN(40) | DIVIDER(2) | 60);
    wr16(spu, CH(14, 8), 0xFE40);
    wr32(spu, CH(14, 0), START | FORMAT(3) | PAN(90) | DIVIDER(2) | 50);
    setup(spu, 1, CAPBUF(0), 0xFE00, 0, 16);
    wr32(spu, CH(1, 0), START | FORMAT(1) | REPEAT(1) | PAN(127) | 64);
    wr32(spu, SNDCAPDAD(0), CAPBUF(0));
    wr16(spu, SNDCAPLEN(0), 16);
    wr8(spu, SNDCAPCNT(0), 0x80);
    setup(spu, 6, DATA(2), 0xFE00, 0, 8);
    wr32(spu, CH(6, 0), START | FORMAT(0) | REPEAT(2) | HOLD | 127);
}

static void test_state(void)
{
    enum { BEFORE = 137, AFTER = 400 };
    static NdsSpu spu, saved;
    static uint8_t saved_ram[RAM_SIZE], end_ram[RAM_SIZE];
    static uint16_t first[AFTER][2];
    static NdsSpu end;
    unsigned wrong = 0, moving = 0;

    /* The whole state is the struct: copied out with memcpy and copied
     * back, with the memory it was working on, it carries on exactly. */
    g_section = "save and restore";
    start_scene(&spu);
    frames(&spu, BEFORE);
    memcpy(&saved, &spu, sizeof spu);
    memcpy(saved_ram, g_ram, sizeof g_ram);
    for (int f = 0; f < AFTER; f++) {
        frame(&spu);
        first[f][0] = (uint16_t)LEFT(&spu);
        first[f][1] = (uint16_t)RIGHT(&spu);
        moving += f > 0 && (first[f][0] != first[f - 1][0] || first[f][1] != first[f - 1][1]);
    }
    memcpy(&end, &spu, sizeof spu);
    memcpy(end_ram, g_ram, sizeof g_ram);
    EQ("the scene is not standing still", moving > AFTER / 2, 1);

    memset(&spu, 0x33, sizeof spu);
    memcpy(&spu, &saved, sizeof spu);
    memcpy(g_ram, saved_ram, sizeof g_ram);
    for (int f = 0; f < AFTER; f++) {
        frame(&spu);
        wrong += LEFT(&spu) != first[f][0] || RIGHT(&spu) != first[f][1];
    }
    EQ("frames that differ after a restore", wrong, 0);
    EQ("state differs after the same frames", memcmp(&end, &spu, sizeof spu) != 0, 0);
    EQ("memory differs after the same frames", memcmp(end_ram, g_ram, sizeof g_ram) != 0, 0);

    /* The scene set up with every halfword and word write handed over as
     * bytes, lowest address first, is the same scene: the start bit is in
     * a register's highest byte, so a start still finds the rest of the
     * register in place. */
    g_section = "writes split into bytes";
    start_scene(&spu);
    for (int f = 0; f < AFTER; f++) {
        frame(&spu);
        first[f][0] = (uint16_t)LEFT(&spu);
        first[f][1] = (uint16_t)RIGHT(&spu);
    }
    memcpy(&end, &spu, sizeof spu);
    memcpy(end_ram, g_ram, sizeof g_ram);
    g_split_writes = true;
    start_scene(&spu);
    g_split_writes = false;
    wrong = 0;
    for (int f = 0; f < AFTER; f++) {
        frame(&spu);
        wrong += LEFT(&spu) != first[f][0] || RIGHT(&spu) != first[f][1];
    }
    EQ("frames that differ", wrong, 0);
    EQ("state differs", memcmp(&end, &spu, sizeof spu) != 0, 0);
    EQ("memory differs", memcmp(end_ram, g_ram, sizeof g_ram) != 0, 0);
}

/* ------------------------------------------------------------------ */
/* Anything a game could write                                         */
/* ------------------------------------------------------------------ */

static struct {
    unsigned reads, writes, bad;
    /* When set, the bus has the unit's own registers at their addresses,
     * as the ARM7's bus does: a sound or a capture buffer can lie on
     * them, and then the unit's callbacks come back into the unit. */
    NdsSpu *mapped;
    unsigned reads_back, writes_back; /* how often they did */
} g_wild;

static uint32_t wild_read(void *user, uint32_t addr, int size);
static void wild_write(void *user, uint32_t addr, uint32_t value, int size);

static const NdsSpuHost g_wild_host = { wild_read, wild_write, &g_wild };

/* A bus with something at every address: here the unit gets pointed
 * anywhere. It may still only ask for whole words at word-aligned
 * addresses of 27 bits. */
static bool wild_ok(void *user, uint32_t addr, int size)
{
    return user == &g_wild && size == 4 && (addr & 3u) == 0 && addr < 0x08000000u;
}

static uint32_t wild_read(void *user, uint32_t addr, int size)
{
    if (!wild_ok(user, addr, size)) g_wild.bad++;
    g_wild.reads++;
    if (g_wild.mapped && addr >= NDS_SPU_IO_BASE && addr < NDS_SPU_IO_END) {
        g_wild.reads_back++;
        return nds_spu_read(g_wild.mapped, addr, 4);
    }
    return (addr * 2654435761u) ^ (addr >> 5);
}

static void wild_write(void *user, uint32_t addr, uint32_t value, int size)
{
    if (!wild_ok(user, addr, size)) g_wild.bad++;
    g_wild.writes++;
    if (g_wild.mapped && addr >= NDS_SPU_IO_BASE && addr < NDS_SPU_IO_END) {
        g_wild.writes_back++;
        nds_spu_write(g_wild.mapped, &g_wild_host, addr, value, 4);
    }
}

static uint32_t g_random;

static uint32_t random32(void)
{
    g_random ^= g_random << 13;
    g_random ^= g_random >> 17;
    g_random ^= g_random << 5;
    return g_random;
}

/* Random register accesses of every size at every offset, a little past
 * both ends of the register file, mixed with frames. Returns a checksum of
 * everything that came out; counts in *faults whatever broke the rules:
 * a level over 10 bits, a host sample that is not its level, more bus
 * traffic in one frame than 512 samples a unit can explain (264 words read
 * per channel with the FIFO's 32 bytes of slack, 259 written per capture
 * unit), a FIFO over its size. With `mapped` the bus has the unit's
 * registers on it and the addresses written favour them; a captured word
 * can then start channels, so the traffic rule does not apply. */
static uint32_t wild_run(uint32_t seed, int operations, bool mapped, unsigned *faults)
{
    static NdsSpu spu;
    static const int sizes[4] = { 1, 2, 4, 4 };
    const NdsSpuHost *host = &g_wild_host;
    uint32_t checksum = 0;
    int16_t out[2];

    memset(&g_wild, 0, sizeof g_wild);
    g_wild.mapped = mapped ? &spu : NULL;
    g_random = seed;
    memset(&spu, 0x5A, sizeof spu);
    nds_spu_reset(&spu);
    for (int i = 0; i < operations; i++) {
        uint32_t r = random32();
        uint32_t addr = 0x040003F8u + random32() % 0x130u;
        int size = sizes[(r >> 5) & 3u];

        if ((r & 3u) == 0) {
            for (int count = 1 + (int)((r >> 2) & 3u); count > 0; count--) {
                unsigned reads = g_wild.reads, writes = g_wild.writes;

                nds_spu_sample(&spu, host, out);
                for (int s = 0; s < 2; s++) {
                    uint32_t level = nds_spu_dac(&spu, s);

                    if (level > 0x3FFu || out[s] != ((int)level - 0x200) * 64) (*faults)++;
                    checksum = checksum * 31u + level;
                }
                if (!mapped && (g_wild.reads - reads > 16u * 264u || g_wild.writes - writes > 2u * 259u)) (*faults)++;
            }
        } else if ((r & 3u) == 1) {
            uint32_t value = nds_spu_read(&spu, addr, size);

            /* Bytes outside the register file read 0. */
            for (int b = 0; b < size; b++) {
                uint32_t byte_addr = addr + (uint32_t)b;

                if ((byte_addr < NDS_SPU_IO_BASE || byte_addr >= NDS_SPU_IO_END) && ((value >> (8 * b)) & 0xFFu) != 0) (*faults)++;
            }
            checksum = checksum * 31u + value;
        } else {
            uint32_t value = random32();

            switch ((r >> 2) & 7u) {
            case 0: value = 0; break;
            case 1: value = 0xFFFFFFFFu; break;
            case 2: value |= 0x80808080u; break; /* start and enable bits */
            case 3: value |= 0x0000FF00u; break; /* fast timers */
            case 4:
            case 5:
            case 6:
                if (mapped) value = NDS_SPU_IO_BASE + (value & 0x1FCu); /* a sound or a buffer on the registers */
                break;
            default: break;
            }
            nds_spu_write(&spu, host, addr, value, size);
        }
        for (int n = 0; n < NDS_SPU_CHANNELS; n++) {
            if (spu.channel[n].fifo_level > NDS_SPU_CHANNEL_FIFO || spu.channel[n].phase > NDS_SPU_TAIL) (*faults)++;
        }
        for (int x = 0; x < NDS_SPU_CAPTURES; x++) {
            if (spu.capture[x].fifo_level >= NDS_SPU_CAPTURE_FIFO) (*faults)++;
        }
    }
    *faults += g_wild.bad;
    return checksum * 31u + g_wild.reads * 7u + g_wild.writes;
}

/* The unit's registers used as its memory, on purpose: two capture units
 * record a loud mixer over the whole register file, word after word
 * through nds_spu_write() from inside nds_spu_sample(), and a channel
 * plays the register file back through nds_spu_read(). Whatever that
 * makes of the registers, the rules of wild_run() must hold. Returns the
 * number of times they did not. */
static unsigned registers_as_memory(uint32_t *checksum)
{
    static NdsSpu spu;
    const NdsSpuHost *host = &g_wild_host;
    unsigned faults = 0;
    int16_t out[2];

    memset(&g_wild, 0, sizeof g_wild);
    g_wild.mapped = &spu;
    nds_spu_reset(&spu);
    for (uint32_t round = 0; round < 400; round++) {
        /* Set the scene again each round: the last one wrote over it. */
        nds_spu_write(&spu, host, SOUNDCNT, 0x807F, 2);
        nds_spu_write(&spu, host, SOUNDBIAS, 0x200, 2);
        nds_spu_write(&spu, host, CH(9, 8), 0xFF55, 2);
        nds_spu_write(&spu, host, CH(9, 0), START | FORMAT(3) | DUTY(2) | PAN(40) | 127, 4);
        nds_spu_write(&spu, host, CH(15, 8), 0xFFA1, 2);
        nds_spu_write(&spu, host, CH(15, 0), START | FORMAT(3) | PAN(90) | 127, 4);
        nds_spu_write(&spu, host, CH(1, 8), 0xFF00u + (round & 0xFFu), 2);
        nds_spu_write(&spu, host, CH(3, 8), 0xFE00u + (round & 0xFFu), 2);
        for (uint32_t x = 0; x < 2; x++) {
            nds_spu_write(&spu, host, SNDCAPDAD(x), NDS_SPU_IO_BASE + 0x90u * x, 4);
            nds_spu_write(&spu, host, SNDCAPLEN(x), 0x24, 2);
            nds_spu_write(&spu, host, SNDCAPCNT(x), 0x00, 1);
            nds_spu_write(&spu, host, SNDCAPCNT(x), round & 1u ? 0x80 : 0x88, 1);
        }
        nds_spu_write(&spu, host, CH(2, 0), 0, 4);
        nds_spu_write(&spu, host, CH(2, 4), NDS_SPU_IO_BASE, 4);
        nds_spu_write(&spu, host, CH(2, 8), 0xFF80, 2);
        nds_spu_write(&spu, host, CH(2, 0xA), round & 7u, 2);
        nds_spu_write(&spu, host, CH(2, 0xC), 0x48u - (round & 7u), 4);
        nds_spu_write(&spu, host, CH(2, 0), START | FORMAT(round % 3u) | REPEAT(1) | 127, 4);
        for (int f = 0; f < 50; f++) {
            nds_spu_sample(&spu, host, out);
            for (int s = 0; s < 2; s++) {
                uint32_t level = nds_spu_dac(&spu, s);

                if (level > 0x3FFu || out[s] != ((int)level - 0x200) * 64) faults++;
                *checksum = *checksum * 31u + level;
            }
            for (int n = 0; n < NDS_SPU_CHANNELS; n++) {
                if (spu.channel[n].fifo_level > NDS_SPU_CHANNEL_FIFO || spu.channel[n].phase > NDS_SPU_TAIL) faults++;
            }
            for (int x = 0; x < NDS_SPU_CAPTURES; x++) {
                if (spu.capture[x].fifo_level >= NDS_SPU_CAPTURE_FIFO) faults++;
            }
        }
    }
    return faults + g_wild.bad;
}

/* What the run below produced. `test_nds_spu --checksum` prints it, and the
 * checksum of every frame of the other tests, for comparing one build with
 * another: every compiler must arrive at the same samples. Neither is an
 * expected value here; nothing says what they should be. */
static uint32_t g_wild_checksum;

static void test_wild(void)
{
    unsigned faults = 0, faults_again = 0;
    uint32_t first, second;

    g_section = "arbitrary register writes";
    first = wild_run(0x2545F491u, 400000, false, &faults);
    g_wild_checksum = first;
    EQ("rules broken", faults, 0);
    EQ("it read memory", g_wild.reads > 10000u, 1);
    EQ("it wrote memory", g_wild.writes > 10000u, 1);
    second = wild_run(0x2545F491u, 400000, false, &faults_again);
    EQ("rules broken the second time", faults_again, 0);
    EQ("the same accesses give the same output", second, first);

    /* The same with sounds and capture buffers lying on the unit's own
     * registers: reads of them come back through nds_spu_read() and
     * captured words through nds_spu_write(), from inside the unit. */
    g_section = "arbitrary register writes, registers on the bus";
    faults = 0;
    first = wild_run(0x9E3779B9u, 400000, true, &faults);
    g_wild_checksum = g_wild_checksum * 31u + first;
    EQ("rules broken", faults, 0);
    EQ("it read memory", g_wild.reads > 10000u, 1);
    EQ("it wrote memory", g_wild.writes > 10000u, 1);
    EQ("reads came back into the unit", g_wild.reads_back > 100u, 1);
    EQ("writes came back into the unit", g_wild.writes_back > 10u, 1);

    g_section = "registers as memory";
    EQ("rules broken", registers_as_memory(&g_wild_checksum), 0);
    EQ("reads came back into the unit", g_wild.reads_back > 10000u, 1);
    EQ("writes came back into the unit", g_wild.writes_back > 10000u, 1);
}

/* SOUNDxSAD and SNDCAPxDAD have 27 bits, and 27-bit word-aligned addresses
 * are all the bus is ever asked for: a sound or a capture buffer that runs
 * past 0x07FFFFFF goes on at 0. (What the hardware does there is not
 * documented; this is the unit's promise to the machine.) */
static void test_address_range(void)
{
    static NdsSpu spu;
    const NdsSpuHost *host = &g_wild_host;
    int16_t out[2];

    g_section = "27-bit addresses";
    memset(&g_wild, 0, sizeof g_wild);
    nds_spu_reset(&spu);
    nds_spu_write(&spu, host, SOUNDCNT, 0x807F, 2);
    nds_spu_write(&spu, host, CH(5, 4), 0x07FFFFF0u, 4);
    nds_spu_write(&spu, host, CH(5, 8), 0xFF00, 2);
    nds_spu_write(&spu, host, CH(5, 0xC), 16, 4);
    nds_spu_write(&spu, host, CH(5, 0), START | FORMAT(1) | REPEAT(1) | 127, 4);
    nds_spu_write(&spu, host, CH(1, 8), 0xFF00, 2);
    nds_spu_write(&spu, host, SNDCAPDAD(0), 0x07FFFFF8u, 4);
    nds_spu_write(&spu, host, SNDCAPLEN(0), 8, 2);
    nds_spu_write(&spu, host, SNDCAPCNT(0), 0x80, 1);
    for (int f = 0; f < 200; f++) nds_spu_sample(&spu, host, out);
    EQ("words read", g_wild.reads > 100u, 1);
    EQ("words written", g_wild.writes > 100u, 1);
    EQ("accesses over 27 bits or unaligned", g_wild.bad, 0);
}

/* The header's two allowances: a host that is NULL, or has NULL callbacks,
 * is memory that reads 0 and takes no writes; and nds_spu_sample() may be
 * told not to return the frame. */
static void test_no_host(void)
{
    static NdsSpu spu;
    static const NdsSpuHost nothing = { NULL, NULL, NULL };
    unsigned wrong = 0;
    int16_t out[2] = { 1, 1 };

    g_section = "no host";
    nds_spu_reset(&spu);
    nds_spu_write(&spu, NULL, SOUNDCNT, 0x807F, 2);
    nds_spu_write(&spu, NULL, SOUNDBIAS, 0x200, 2);
    nds_spu_write(&spu, NULL, CH(0, 8), 0xFE00, 2);
    nds_spu_write(&spu, NULL, CH(0, 0xC), 16, 4);
    nds_spu_write(&spu, NULL, CH(0, 0), START | FORMAT(1) | REPEAT(1) | 127, 4);
    nds_spu_write(&spu, &nothing, CH(1, 8), 0xFE00, 2);
    nds_spu_write(&spu, &nothing, SNDCAPLEN(0), 4, 2);
    nds_spu_write(&spu, &nothing, SNDCAPCNT(0), 0x80, 1);
    for (int f = 0; f < 60; f++) {
        nds_spu_sample(&spu, f & 1 ? NULL : &nothing, out);
        wrong += out[0] != 0 || out[1] != 0 || nds_spu_dac(&spu, 0) != 0x200;
    }
    EQ("frames that were not silence", wrong, 0);
    EQ("the channel is playing", busy(&spu, 0), 1);
    EQ("the capture unit is running", rd8(&spu, SNDCAPCNT(0)), 0x80);

    /* A PSG channel needs no memory: with no frame returned the unit still
     * advances and still keeps its levels. */
    nds_spu_write(&spu, NULL, CH(8, 8), 0xFE00, 2);
    nds_spu_write(&spu, NULL, CH(8, 0), START | FORMAT(3) | DUTY(6) | 127, 4);
    nds_spu_sample(&spu, NULL, NULL);
    EQ("first step, low", nds_spu_dac(&spu, 0), 0x000);
    nds_spu_sample(&spu, NULL, NULL);
    EQ("second step, high", nds_spu_dac(&spu, 0), 0x3FF);
    EQ("the frame that was not asked for is not written", out[0], 0);
}

int main(int argc, char **argv)
{
    test_reset();
    test_registers();
    test_psg();
    test_noise();
    test_pcm();
    test_repeat_modes();
    test_timer();
    test_adpcm();
    test_volume();
    test_routing();
    test_output_stage();
    test_hold();
    test_capture_mixer();
    test_capture_channel();
    test_capture_addition();
    test_capture_playback();
    test_state();
    test_wild();
    test_address_range();
    test_no_host();

    /* Over everything above: the host sample was always (level - 0x200) *
     * 64, and the unit only ever asked the bus for whole words at
     * word-aligned addresses, with the user pointer it was given. */
    g_section = "all tests";
    EQ("frames run", g_frames > 0, 1);
    EQ("host samples that were not (level - 0x200) * 64", g_host_sample_errors, 0);
    EQ("bus accesses that were not aligned words in RAM", g_bus_bad, 0);

    if (argc > 1 && strcmp(argv[1], "--checksum") == 0) {
        printf("output checksums %08lX %08lX over %ld frames\n", (unsigned long)g_frame_checksum, (unsigned long)g_wild_checksum,
               g_frames);
    }
    printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("no checks ran (fail closed)\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
