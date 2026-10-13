/*
 * Headless runner shared by nds_run (interpreter only) and every recompiled
 * game executable, so test tooling drives both the same way.
 *
 *   <exe> [ROM] [--bios9 FILE] [--bios7 FILE] [--frames N]
 *               [--keys FRAME:BUTTONS[:N]]... [--touch FRAME:X,Y[:N]]...
 *               [--lid FRAME[:N]] [--auto-input[=RUN]]
 *               [--dump-frame FILE.ppm] [--dump-every N] [--dump-from N]
 *               [--internal-scale N] [--dump-large FILE.ppm]
 *               [--hash-frames] [--sha-frames] [--state-hash]
 *               [--dump-audio FILE.raw] [--audio-hash]
 *               [--dump-ram FILE] [--dump-mem ADDR:SIZE:FILE]... [--regs]
 *               [--load-save FILE] [--dump-save FILE]
 *               [--interpreter] [--differential] [--differential-step]
 *               [--coverage] [--trace-out FILE]
 *               [--trace CPU:LO:HI] [--profile] [--quiet] [--headless]
 *
 * Runs are deterministic: the same ROM and options give the same frames and
 * the same state, and nothing is read from the host but the files named.
 * No save is written: the card's save memory starts empty, or as
 * --load-save FILE gives it (the file a windowed run keeps, which is not
 * touched), and --dump-save FILE writes what it holds at the end, to a file
 * of the caller's choosing.
 *
 * A recompiled game needs no ROM argument: the ROM is in the executable.
 * --interpreter ignores the recompiled program. --coverage reports the share
 * of instructions that ran as native code. --trace-out writes every
 * instruction the interpreter ran ("CPU ADDRESS a|t COUNT OPCODE" lines,
 * most-run first): with a recompiled program installed that is the code it
 * lacks, and ndsrecomp --trace takes the file as places to compile from. The
 * opcode tells overlays that load at the same address apart.
 *
 * --differential runs the recompiled program and the interpreter side by
 * side, one scheduler slice at a time, and compares both CPUs' registers,
 * cycle counters and instruction counts after every slice and memory after
 * every frame. --differential-step makes each slice one system cycle long,
 * so a divergence is reported at the instruction that caused it. Neither is
 * a hardware oracle: both machines share the same devices. They prove that
 * recompiled code executes the program exactly as the interpreter does.
 *
 * --state-hash prints a hash of both CPUs and all memory at the end, for
 * comparing two runs.
 *
 * --profile prints where the run's time went, by part of the machine. It
 * needs a runtime built with NDS_PROFILE (CMake -DNDS_PROFILE=ON), which is
 * slower: the timing calls are themselves counted.
 *
 * --dump-audio writes the sound output: signed 16-bit little-endian stereo
 * at 32,728.5 Hz (NDS_CLOCK_HZ / 1024), no header. --audio-hash prints a
 * hash of the same samples and how many frames of them there were.
 *
 * --sha-frames prints "frame N top=SHA bottom=SHA" after each frame, N
 * counting from 1: the SHA-256 of each screen's pixels as three bytes each,
 * the 5-bit red, green and blue values. tools/nds/oracle_desmume.py writes
 * the same lines, so two emulators that widen 5 bits to 8 differently can be
 * compared.
 *
 * --keys holds BUTTONS (A, B, X, Y, L, R, START, SELECT, UP, DOWN, LEFT,
 * RIGHT, joined with +) from FRAME for N frames (default 1). --touch holds
 * the stylus on the bottom screen at pixel X,Y the same way, and --lid
 * closes the lid. This is the GBA runner's --keys with the DS's extra
 * inputs; tools/nds/oracle_desmume.py takes the same options. --auto-input
 * plays the built-in input script (nds_auto_input) that ndsrecomp --profile
 * uses instead, and ignores --keys and --touch; =RUN picks a variation of it
 * (0, the default, is the one a profile starts with).
 *
 * --dump-frame writes the last frame, the top screen above the bottom one
 * (256x384), as a binary PPM. With --dump-every N it also writes every Nth
 * frame (counting from 1, and from --dump-from on) to FILE with the frame
 * number before its extension: shot.ppm gives shot_000060.ppm.
 * --internal-scale N (1 to 4) has the 3D picture drawn that many times the
 * screens' size as well (nds_set_internal_scale); the frames, hashes and
 * state above are the console's own whatever N is. --dump-large writes what
 * a display with the pixels would show, both screens N times the size, at
 * the end and with --dump-every like --dump-frame.
 * --hash-frames prints one hash per frame.
 * --dump-ram writes main RAM (4 MiB) at the end. --dump-mem writes SIZE
 * bytes from ADDR (both hex) as the ARM9 sees them, without side effects:
 * VRAM as it is mapped, palettes, OAM, the TCMs. --regs prints both CPUs'
 * registers at the end.
 *
 * --trace prints every interpreted instruction of CPU 9 or 7 whose address
 * is in [LO, HI) (hex), with its registers.
 *
 * Debug output a program writes to the no$gba ports is printed as
 * "[arm9] ..." and "[arm7] ..." lines.
 */
#include "nds_runner.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nds.h"
#include "nds_prof.h"
#include "nds_recomp.h"

typedef struct {
    uint64_t frame, frames;
    uint32_t keys;
    bool touch, lid;
    int x, y;
} Input;

#define MAX_INPUTS 1024
static Input g_inputs[MAX_INPUTS];
static int g_input_count;

typedef struct {
    uint32_t addr, size;
    const char *path;
} MemDump;

#define MAX_MEM_DUMPS 16
static MemDump g_mem_dumps[MAX_MEM_DUMPS];
static int g_mem_dump_count;

static int g_trace_cpu = -1;
static uint32_t g_trace_lo, g_trace_hi;

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = len > 0 ? malloc((size_t)len) : NULL;
    if (data && fread(data, 1, (size_t)len, f) != (size_t)len) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = data ? (size_t)len : 0;
    return data;
}

static bool parse_buttons(const char *text, uint32_t *mask)
{
    static const struct { const char *name; uint32_t bit; } names[] = {
        { "A", NDS_KEY_A }, { "B", NDS_KEY_B }, { "X", NDS_KEY_X }, { "Y", NDS_KEY_Y },
        { "L", NDS_KEY_L }, { "R", NDS_KEY_R }, { "START", NDS_KEY_START }, { "SELECT", NDS_KEY_SELECT },
        { "UP", NDS_KEY_UP }, { "DOWN", NDS_KEY_DOWN }, { "LEFT", NDS_KEY_LEFT }, { "RIGHT", NDS_KEY_RIGHT },
    };
    *mask = 0;
    while (*text) {
        size_t len = strcspn(text, "+");
        bool found = false;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            if (strlen(names[i].name) == len && !strncmp(names[i].name, text, len)) {
                *mask |= names[i].bit;
                found = true;
            }
        }
        if (!found) return false;
        text += len;
        if (*text == '+') text++;
    }
    return true;
}

/* FRAME:WHAT[:N]. Returns WHAT (copied to what) and fills frame/frames. */
static bool parse_timed(const char *arg, Input *in, char *what, size_t what_size)
{
    char *end;
    in->frame = strtoull(arg, &end, 10);
    in->frames = 1;
    what[0] = '\0';
    if (*end == ':') {
        const char *start = end + 1;
        size_t len = strcspn(start, ":");
        if (len >= what_size) return false;
        memcpy(what, start, len);
        what[len] = '\0';
        if (start[len] == ':') in->frames = strtoull(start + len + 1, NULL, 10);
    } else if (*end) {
        return false;
    }
    return in->frames > 0;
}

/* --auto-input uses nds_auto_input instead of (and ignores) --keys and
 * --touch. */
static bool g_auto_input;
static unsigned g_auto_run;

/* Where the script's taps and strokes land: the points of a low-discrepancy
 * sequence (R2, the plastic number's), so that each is somewhere new, the
 * points so far are spread evenly over the screen, and a button anywhere on
 * it is hit sooner or later. */
static void auto_point(uint64_t index, int *x, int *y)
{
    uint32_t n = (uint32_t)index + 1u;
    *x = 8 + (int)((((n * 49471u) & 0xFFFFu) * 240u) >> 16);
    *y = 8 + (int)((((n * 37345u) & 0xFFFFu) * 176u) >> 16);
}

/* One action every 30 frames from frame 300. For the first 40 slots the
 * script works through title screens, menus and text: a tap in the middle
 * of the touch screen, A, a tap somewhere else, START. Then it plays, in a
 * cycle: taps, strokes held across the screen (games steered with the
 * stylus), the D-pad and every button. START comes in pairs so that a pause
 * menu it opens is closed again. Directions and strokes are held for most
 * of a slot, everything else for six frames. `run` moves the taps and
 * strokes to other places. */
#define AUTO_TAP (1u << 30)
#define AUTO_STROKE (1u << 31)

void nds_auto_input(uint64_t frame, unsigned run, NdsAutoInput *in)
{
    static const uint32_t play[] = {
        AUTO_TAP, NDS_KEY_RIGHT, AUTO_STROKE, NDS_KEY_A, AUTO_TAP, NDS_KEY_DOWN, AUTO_STROKE, NDS_KEY_B,
        AUTO_TAP, NDS_KEY_LEFT,  AUTO_STROKE, NDS_KEY_X, AUTO_TAP, NDS_KEY_UP,   AUTO_STROKE, NDS_KEY_Y,
        NDS_KEY_R, AUTO_TAP, NDS_KEY_L, AUTO_STROKE, NDS_KEY_START, NDS_KEY_A, NDS_KEY_START, NDS_KEY_B,
    };
    const uint64_t first = 300, slot_frames = 30, menu_slots = 40;
    const uint64_t play_slots = sizeof play / sizeof play[0];
    const uint64_t place = (uint64_t)run * 1009u; /* where this run's points start in the sequence */
    in->keys = 0;
    in->touch = false;
    in->x = in->y = 0;
    if (frame < first) return;
    uint64_t slot = (frame - first) / slot_frames, at = (frame - first) % slot_frames;
    if (slot < menu_slots) {
        if (at >= 6) return;
        switch (slot % 4) {
        case 0:
            in->touch = true;
            in->x = 128;
            in->y = 96;
            break;
        case 1: in->keys = NDS_KEY_A; break;
        case 2:
            in->touch = true;
            auto_point(place + slot / 4, &in->x, &in->y);
            break;
        default: in->keys = NDS_KEY_START; break;
        }
        return;
    }
    slot -= menu_slots;
    uint32_t action = play[slot % play_slots];
    if (action == AUTO_TAP) {
        if (at >= 6) return;
        in->touch = true;
        auto_point(place + menu_slots + slot, &in->x, &in->y);
    } else if (action == AUTO_STROKE) {
        if (at >= 24) return;
        int x0, y0, x1, y1;
        auto_point(place + slot, &x0, &y0);
        auto_point(place + slot + 1, &x1, &y1);
        in->touch = true;
        in->x = x0 + (x1 - x0) * (int)at / 23;
        in->y = y0 + (y1 - y0) * (int)at / 23;
    } else {
        bool direction = (action & (NDS_KEY_RIGHT | NDS_KEY_LEFT | NDS_KEY_UP | NDS_KEY_DOWN)) != 0;
        if (at < (direction ? 24u : 6u)) in->keys = action;
    }
}

static void apply_inputs(NdsMachine *m, uint64_t frame)
{
    if (g_auto_input) {
        NdsAutoInput in;
        nds_auto_input(frame, g_auto_run, &in);
        nds_set_keys(m, in.keys);
        nds_set_touch(m, in.touch, in.x, in.y);
        nds_set_lid(m, false);
        return;
    }
    uint32_t keys = 0;
    bool touch = false, lid = false;
    int x = 0, y = 0;
    for (int i = 0; i < g_input_count; i++) {
        const Input *in = &g_inputs[i];
        if (frame < in->frame || frame >= in->frame + in->frames) continue;
        keys |= in->keys;
        if (in->touch) {
            touch = true;
            x = in->x;
            y = in->y;
        }
        lid |= in->lid;
    }
    nds_set_keys(m, keys);
    nds_set_touch(m, touch, x, y);
    nds_set_lid(m, lid);
}

static uint64_t hash_frame(const NdsMachine *m)
{
    uint64_t h = 1469598103934665603ull;
    for (int screen = 0; screen < 2; screen++) {
        for (int i = 0; i < NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT; i++) {
            uint32_t pixel = m->framebuffer[screen][i];
            for (int b = 0; b < 3; b++) {
                h ^= (pixel >> (8 * b)) & 0xFFu;
                h *= 1099511628211ull;
            }
        }
    }
    return h;
}

/* SHA-256 (FIPS 180-4), for --sha-frames. */
static uint32_t rotr32(uint32_t v, unsigned n)
{
    return (v >> n) | (v << (32u - n));
}

static void sha256_block(uint32_t h[8], const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
        0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
        0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
        0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
        0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
        0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
        0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
        0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
    };
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | ((uint32_t)block[i * 4 + 2] << 8) |
               block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(s[4], 6) ^ rotr32(s[4], 11) ^ rotr32(s[4], 25);
        uint32_t ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
        uint32_t t1 = s[7] + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(s[0], 2) ^ rotr32(s[0], 13) ^ rotr32(s[0], 22);
        uint32_t maj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
        uint32_t t2 = s0 + maj;
        memmove(s + 1, s, 7 * sizeof s[0]);
        s[4] += t1;
        s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += s[i];
}

/* One screen as the oracle hashes it: three bytes a pixel, 5 bits each. */
static void sha_screen(const uint32_t *pixels, char hex[65])
{
    uint32_t h[8] = { 0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u };
    uint8_t block[64];
    size_t fill = 0;
    const uint64_t length = (uint64_t)NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 3u;
    for (int i = 0; i < NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT; i++) {
        for (int shift = 16; shift >= 0; shift -= 8) {
            block[fill++] = (uint8_t)(((pixels[i] >> shift) & 0xFFu) >> 3);
            if (fill == 64) {
                sha256_block(h, block);
                fill = 0;
            }
        }
    }
    block[fill++] = 0x80;
    if (fill > 56) {
        memset(block + fill, 0, 64 - fill);
        sha256_block(h, block);
        fill = 0;
    }
    memset(block + fill, 0, 56 - fill);
    for (int i = 0; i < 8; i++) block[56 + i] = (uint8_t)((length * 8u) >> (56 - 8 * i));
    sha256_block(h, block);
    for (int i = 0; i < 8; i++) snprintf(hex + i * 8, 9, "%08" PRIx32, h[i]);
}

static bool write_ppm(const NdsMachine *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT * 2);
    for (int screen = 0; screen < 2; screen++) {
        for (int i = 0; i < NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT; i++) {
            uint32_t pixel = m->framebuffer[screen][i];
            uint8_t rgb[3] = { (uint8_t)(pixel >> 16), (uint8_t)(pixel >> 8), (uint8_t)pixel };
            fwrite(rgb, 1, 3, f);
        }
    }
    return fclose(f) == 0;
}

/* --dump-large: both screens as a display with the pixels would show them at
 * the internal resolution, the top above the bottom. A screen with no
 * larger frame (nds_fine_frame) is its own pixels repeated. */
static bool write_large_ppm(const NdsMachine *m, const char *path)
{
    const int scale = nds_internal_scale(m);
    const int wide = NDS_SCREEN_WIDTH * scale, high = NDS_SCREEN_HEIGHT * scale;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t *row = malloc((size_t)wide * 3u);
    if (!row) {
        fclose(f);
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", wide, high * 2);
    for (int screen = 0; screen < 2; screen++) {
        const uint32_t *fine = nds_fine_frame(m, screen, NULL, NULL);
        for (int y = 0; y < high; y++) {
            for (int x = 0; x < wide; x++) {
                uint32_t pixel = fine ? fine[(size_t)y * (size_t)wide + (size_t)x]
                                      : m->framebuffer[screen][(y / scale) * NDS_SCREEN_WIDTH + x / scale];
                row[x * 3] = (uint8_t)(pixel >> 16);
                row[x * 3 + 1] = (uint8_t)(pixel >> 8);
                row[x * 3 + 2] = (uint8_t)pixel;
            }
            fwrite(row, 1, (size_t)wide * 3u, f);
        }
    }
    free(row);
    return fclose(f) == 0;
}

static void print_regs(const char *name, const NdsCpu *cpu)
{
    printf("%s", name);
    for (int i = 0; i < 16; i++) printf(" r%d=%08" PRIX32, i, cpu->r[i]);
    printf(" cpsr=%08" PRIX32 "\n", nds_cpu_get_cpsr(cpu));
}

static void trace_hook(NdsMachine *m, int cpu)
{
    if (cpu != g_trace_cpu) return;
    const NdsCpu *c = cpu == NDS_ARM9 ? &m->arm9 : &m->arm7;
    uint32_t pc = nds_cpu_pc(c);
    if (pc < g_trace_lo || pc >= g_trace_hi) return;
    printf("[trace%d] %08" PRIX32 " %0*" PRIX32 " cyc=%" PRIu64, cpu == NDS_ARM9 ? 9 : 7, pc,
           nds_cpu_thumb(c) ? 4 : 8, c->pipe[0], nds_cpu_now(m, cpu));
    for (int i = 0; i < 15; i++) printf(" %08" PRIX32, c->r[i]);
    printf(" %08" PRIX32 "\n", nds_cpu_get_cpsr(c));
}

static void debug_log(NdsMachine *m, int cpu, const char *text)
{
    /* Programs print a character at a time; start a line when the last
     * one ended. */
    static bool at_line_start[2] = { true, true };
    (void)m;
    for (const char *p = text; *p; p++) {
        if (at_line_start[cpu]) printf("[arm%d] ", cpu == NDS_ARM9 ? 9 : 7);
        putchar(*p);
        at_line_start[cpu] = *p == '\n';
    }
}

static int usage(const char *exe)
{
    fprintf(stderr,
            "usage: %s [ROM] [--bios9 FILE] [--bios7 FILE] [--frames N]\n"
            "          [--keys FRAME:BUTTONS[:N]]... [--touch FRAME:X,Y[:N]]... [--lid FRAME[:N]]\n"
            "          [--dump-frame FILE.ppm] [--dump-every N] [--dump-from N]\n"
            "          [--internal-scale N] [--dump-large FILE.ppm]\n"
            "          [--hash-frames] [--sha-frames] [--state-hash] [--dump-audio FILE.raw] [--audio-hash]\n"
            "          [--dump-ram FILE] [--dump-mem ADDR:SIZE:FILE]... [--regs]\n"
            "          [--load-save FILE] [--dump-save FILE]\n"
            "          [--auto-input[=RUN]] [--interpreter] [--differential] [--differential-step] [--coverage]\n"
            "          [--trace-out FILE] [--trace CPU:LO:HI] [--profile] [--quiet]\n",
            exe);
    return 2;
}

/* ------------------------------------------------------------------ */
/* What the interpreter ran (--trace-out)                              */
/* ------------------------------------------------------------------ */

/* Open-addressing table of (CPU, address | Thumb, opcode) -> times
 * interpreted. The opcode is part of the key: overlays put different code
 * at one address. */
typedef struct {
    uint64_t key; /* cpu << 32 | address | thumb, plus 1 << 63 so that 0 is "empty" */
    uint64_t count;
    uint32_t opcode;
} Visit;

static Visit *g_visits;
static size_t g_visit_slots, g_visit_count;

static void visit_insert(uint64_t key, uint32_t opcode, uint64_t count)
{
    size_t h = (size_t)(((key ^ ((uint64_t)opcode << 21)) * 0x9E3779B97F4A7C15ull) >> 32) & (g_visit_slots - 1);
    while (g_visits[h].key && (g_visits[h].key != key || g_visits[h].opcode != opcode)) {
        h = (h + 1) & (g_visit_slots - 1);
    }
    if (!g_visits[h].key) {
        g_visits[h].key = key;
        g_visits[h].opcode = opcode;
        g_visit_count++;
    }
    g_visits[h].count += count;
}

static void visits_clear(void)
{
    free(g_visits);
    g_visits = NULL;
    g_visit_slots = g_visit_count = 0;
}

static void visit_hook(NdsMachine *m, int cpu)
{
    const NdsCpu *c = cpu == NDS_ARM9 ? &m->arm9 : &m->arm7;
    if (!g_visits || g_visit_count * 2 >= g_visit_slots) {
        Visit *old = g_visits;
        size_t old_slots = g_visit_slots;
        g_visit_slots = old_slots ? old_slots * 2 : (size_t)1 << 16;
        g_visits = calloc(g_visit_slots, sizeof *g_visits);
        g_visit_count = 0;
        if (!g_visits) {
            fprintf(stderr, "out of memory for --trace-out\n");
            exit(1);
        }
        for (size_t i = 0; i < old_slots; i++) {
            if (old[i].key) visit_insert(old[i].key, old[i].opcode, old[i].count);
        }
        free(old);
    }
    visit_insert(((uint64_t)1 << 63) | ((uint64_t)cpu << 32) | nds_cpu_pc(c) | (nds_cpu_thumb(c) ? 1u : 0u),
                 nds_cpu_thumb(c) ? c->pipe[0] & 0xFFFFu : c->pipe[0], 1);
}

static int by_count(const void *a, const void *b)
{
    const Visit *x = a, *y = b;
    if (x->count != y->count) return x->count < y->count ? 1 : -1;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->opcode < y->opcode ? -1 : x->opcode > y->opcode;
}

static bool write_visits(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    size_t n = 0;
    for (size_t i = 0; i < g_visit_slots; i++) {
        if (g_visits[i].key) g_visits[n++] = g_visits[i];
    }
    if (n) qsort(g_visits, n, sizeof *g_visits, by_count);
    for (size_t i = 0; i < n; i++) {
        uint32_t key = (uint32_t)g_visits[i].key;
        fprintf(f, "%d %08" PRIX32 " %c %" PRIu64 " %0*" PRIX32 "\n", ((g_visits[i].key >> 32) & 1u) ? 7 : 9,
                key & ~1u, (key & 1u) ? 't' : 'a', g_visits[i].count, (key & 1u) ? 4 : 8, g_visits[i].opcode);
    }
    return fclose(f) == 0;
}

bool nds_profile_rom(const uint8_t *rom, size_t size, uint64_t frames, const char *trace_path,
                     uint64_t instructions[2])
{
    NdsMachine *m = nds_create();
    if (!m) return false;
    if (!nds_load_rom(m, rom, size)) {
        nds_destroy(m);
        return false;
    }
    visits_clear();
    uint64_t ran[2] = { 0, 0 };
    for (unsigned run = 0; run < NDS_PROFILE_RUNS; run++) {
        /* The last run takes what the division leaves over. */
        uint64_t run_frames = frames / NDS_PROFILE_RUNS;
        if (run == NDS_PROFILE_RUNS - 1) run_frames = frames - run_frames * (NDS_PROFILE_RUNS - 1);
        nds_reset(m);
        m->step_hook = visit_hook;
        for (uint64_t frame = 0; frame < run_frames; frame++) {
            NdsAutoInput in;
            nds_auto_input(frame, run, &in);
            nds_set_keys(m, in.keys);
            nds_set_touch(m, in.touch, in.x, in.y);
            nds_run_frame(m);
            /* Nobody listens: keep the sound ring from filling up. */
            int16_t audio[1024 * 2];
            while (nds_audio_read(m, audio, 1024) != 0) {}
        }
        ran[0] += m->instructions[NDS_ARM9];
        ran[1] += m->instructions[NDS_ARM7];
    }
    bool ok = write_visits(trace_path);
    if (instructions) {
        instructions[0] = ran[0];
        instructions[1] = ran[1];
    }
    visits_clear();
    nds_destroy(m);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Machine set-up and comparison                                       */
/* ------------------------------------------------------------------ */

static NdsMachine *create_machine(const char *rom_path, const char *bios9_path, const char *bios7_path,
                                  const NdsRcProgram *program, bool use_program)
{
    NdsMachine *m = nds_create();
    if (!m) {
        fprintf(stderr, "out of memory\n");
        return NULL;
    }
    size_t size = 0;
    if (rom_path) {
        uint8_t *rom = read_file(rom_path, &size);
        bool ok = rom && nds_load_rom(m, rom, size);
        free(rom);
        if (!ok) {
            fprintf(stderr, "cannot load ROM %s\n", rom_path);
            nds_destroy(m);
            return NULL;
        }
    } else {
        const uint8_t *rom = program && program->embedded_rom ? program->embedded_rom(&size) : NULL;
        if (!rom || !nds_load_rom(m, rom, size)) {
            fprintf(stderr, "no ROM given and none is embedded in this program\n");
            nds_destroy(m);
            return NULL;
        }
    }
    const char *bios_paths[2] = { bios9_path, bios7_path };
    for (int cpu = 0; cpu < 2; cpu++) {
        if (!bios_paths[cpu]) continue;
        uint8_t *bios = read_file(bios_paths[cpu], &size);
        bool ok = bios && (cpu == NDS_ARM9 ? nds_load_bios9(m, bios, size) : nds_load_bios7(m, bios, size));
        free(bios);
        if (!ok) {
            fprintf(stderr, "cannot load BIOS %s\n", bios_paths[cpu]);
            nds_destroy(m);
            return NULL;
        }
    }
    if (use_program && program && !nds_rc_install(m, program)) {
        fprintf(stderr, "out of memory\n");
        nds_destroy(m);
        return NULL;
    }
    m->debug_log = debug_log;
    nds_reset(m);
    return m;
}

/* Everything of a CPU that recompiled code and the interpreter must agree
 * on. The prefetched opcodes are not part of it: native code does not keep
 * them. */
static bool same_cpu(const NdsCpu *a, const NdsCpu *b)
{
    return memcmp(a->r, b->r, sizeof a->r) == 0 && nds_cpu_get_cpsr(a) == nds_cpu_get_cpsr(b) &&
           a->cycles == b->cycles && memcmp(a->bank_r8_r12, b->bank_r8_r12, sizeof a->bank_r8_r12) == 0 &&
           memcmp(a->bank_r13, b->bank_r13, sizeof a->bank_r13) == 0 &&
           memcmp(a->bank_r14, b->bank_r14, sizeof a->bank_r14) == 0 &&
           memcmp(a->spsr, b->spsr, sizeof a->spsr) == 0 && memcmp(&a->cp15, &b->cp15, sizeof a->cp15) == 0;
}

static const char *differing_memory(const NdsMachine *a, const NdsMachine *b)
{
#define REGION(field) \
    if (memcmp(a->field, b->field, sizeof a->field) != 0) return #field
    REGION(main_ram);
    REGION(shared_wram);
    REGION(arm7_wram);
    REGION(itcm);
    REGION(dtcm);
    REGION(vram);
    REGION(palette);
    REGION(oam);
    REGION(io9);
    REGION(engine);
    if (memcmp(&a->spu, &b->spu, sizeof a->spu) != 0) return "spu";
    if (memcmp(&a->gx, &b->gx, sizeof a->gx) != 0) return "gx";
    REGION(io7);
    REGION(framebuffer);
#undef REGION
    return NULL;
}

static uint64_t hash_bytes(uint64_t h, const void *data, size_t size)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t hash_state(const NdsMachine *m)
{
    uint64_t h = 1469598103934665603ull;
    const NdsCpu *cpus[2] = { &m->arm9, &m->arm7 };
    for (int i = 0; i < 2; i++) {
        uint32_t cpsr = nds_cpu_get_cpsr(cpus[i]);
        h = hash_bytes(h, cpus[i]->r, sizeof cpus[i]->r);
        h = hash_bytes(h, &cpsr, sizeof cpsr);
        h = hash_bytes(h, &cpus[i]->cycles, sizeof cpus[i]->cycles);
        h = hash_bytes(h, cpus[i]->bank_r13, sizeof cpus[i]->bank_r13);
        h = hash_bytes(h, cpus[i]->bank_r14, sizeof cpus[i]->bank_r14);
        h = hash_bytes(h, cpus[i]->spsr, sizeof cpus[i]->spsr);
    }
    h = hash_bytes(h, m->main_ram, sizeof m->main_ram);
    h = hash_bytes(h, m->shared_wram, sizeof m->shared_wram);
    h = hash_bytes(h, m->arm7_wram, sizeof m->arm7_wram);
    h = hash_bytes(h, m->itcm, sizeof m->itcm);
    h = hash_bytes(h, m->dtcm, sizeof m->dtcm);
    h = hash_bytes(h, m->vram, sizeof m->vram);
    h = hash_bytes(h, m->palette, sizeof m->palette);
    h = hash_bytes(h, m->oam, sizeof m->oam);
    h = hash_bytes(h, m->instructions, sizeof m->instructions);
    return h;
}

static void print_side(const char *tag, const NdsMachine *m)
{
    const NdsCpu *cpus[2] = { &m->arm9, &m->arm7 };
    printf("  %s: now=%" PRIu64 "\n", tag, m->now);
    for (int i = 0; i < 2; i++) {
        const NdsCpu *c = cpus[i];
        printf("    arm%d pc=%08" PRIX32 " cpsr=%08" PRIX32 " cycles=%" PRIu64 " instructions=%" PRIu64 "\n     ",
               i == NDS_ARM9 ? 9 : 7, nds_cpu_pc(c), nds_cpu_get_cpsr(c), c->cycles, m->instructions[i]);
        for (int r = 0; r < 16; r++) printf(" %08" PRIX32, c->r[r]);
        printf("\n");
    }
}

static double native_share(const NdsMachine *m, int cpu)
{
    if (!m->instructions[cpu]) return 0.0;
    return 100.0 * (double)(m->instructions[cpu] - m->interpreted[cpu]) / (double)m->instructions[cpu];
}

/* Recompiled machine `a` against interpreter `b`, slice by slice. */
static int run_differential(NdsMachine *a, NdsMachine *b, uint64_t frames, bool single_step)
{
    uint64_t frame = 0, slices = 0;
    apply_inputs(a, 0);
    apply_inputs(b, 0);
    while (frame < frames) {
        uint32_t pc9 = nds_cpu_pc(&a->arm9), pc7 = nds_cpu_pc(&a->arm7);
        uint64_t limit = a->now + (single_step ? 1u : NDS_CYCLES_PER_LINE);
        a->frame_ready = false;
        nds_run_slice(a, limit);
        nds_run_slice(b, limit);
        slices++;

        const char *what = NULL;
        if (a->now != b->now) what = "time";
        else if (!same_cpu(&a->arm9, &b->arm9) || a->instructions[NDS_ARM9] != b->instructions[NDS_ARM9]) what = "arm9";
        else if (!same_cpu(&a->arm7, &b->arm7) || a->instructions[NDS_ARM7] != b->instructions[NDS_ARM7]) what = "arm7";
        if (!what && a->frame_ready) {
            frame++;
            what = differing_memory(a, b);
            /* Both machines are at the same instruction here: change input. */
            apply_inputs(a, frame);
            apply_inputs(b, frame);
        }
        if (what) {
            printf("DIVERGED (%s) after slice %" PRIu64 " (frame %" PRIu64 "); the slice began at arm9 %08" PRIX32
                   ", arm7 %08" PRIX32 "\n",
                   what, slices, frame, pc9, pc7);
            print_side("recompiled ", a);
            print_side("interpreter", b);
            return 1;
        }
    }
    printf("differential: %" PRIu64 " frames, %" PRIu64 " slices, arm9 %" PRIu64 " instructions (%.1f%% native), arm7 %"
           PRIu64 " instructions (%.1f%% native), no divergence\nPASS\n",
           frame, slices, a->instructions[NDS_ARM9], native_share(a, NDS_ARM9), a->instructions[NDS_ARM7],
           native_share(a, NDS_ARM7));
    return 0;
}

int nds_runner_main(int argc, char **argv, const struct NdsRcProgram *program, const char *default_rom)
{
    const char *rom_path = default_rom;
    const char *bios9_path = NULL, *bios7_path = NULL;
    const char *dump_frame = NULL, *dump_ram = NULL, *dump_large = NULL;
    int internal_scale = 1;
    const char *trace_out = NULL, *dump_audio = NULL, *load_save = NULL, *dump_save = NULL;
    bool audio_hash = false, profile = false;
    uint64_t frames = 60, dump_every = 0, dump_from = 0;
    bool hash_frames = false, sha_frames = false, regs = false, quiet = false, state_hash = false;
    bool interpreter_only = false, differential = false, single_step = false, coverage = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        char what[64];
        if (!strcmp(a, "--frames") && next) frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--bios9") && next) bios9_path = argv[++i];
        else if (!strcmp(a, "--bios7") && next) bios7_path = argv[++i];
        else if (!strcmp(a, "--dump-frame") && next) dump_frame = argv[++i];
        else if (!strcmp(a, "--dump-large") && next) dump_large = argv[++i];
        else if (!strcmp(a, "--internal-scale") && next) {
            internal_scale = atoi(argv[++i]);
            if (internal_scale < 1 || internal_scale > NDS_INTERNAL_SCALE_MAX) {
                fprintf(stderr, "--internal-scale takes 1 to %d, not %s\n", NDS_INTERNAL_SCALE_MAX, argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--dump-every") && next) dump_every = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--dump-from") && next) dump_from = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--dump-ram") && next) dump_ram = argv[++i];
        else if (!strcmp(a, "--load-save") && next) load_save = argv[++i];
        else if (!strcmp(a, "--dump-save") && next) dump_save = argv[++i];
        else if (!strcmp(a, "--dump-mem") && next && g_mem_dump_count < MAX_MEM_DUMPS) {
            /* ADDR:SIZE:FILE; the file name may itself hold colons. */
            MemDump *d = &g_mem_dumps[g_mem_dump_count];
            char *end;
            d->addr = (uint32_t)strtoul(argv[++i], &end, 16);
            bool ok = *end == ':';
            if (ok) {
                d->size = (uint32_t)strtoul(end + 1, &end, 16);
                ok = *end == ':' && end[1] && d->size > 0 && d->size <= 0x1000000u;
            }
            if (!ok) {
                fprintf(stderr, "bad --dump-mem %s (want ADDR:SIZE:FILE, hex)\n", argv[i]);
                return 2;
            }
            d->path = end + 1;
            g_mem_dump_count++;
        }
        else if (!strcmp(a, "--hash-frames")) hash_frames = true;
        else if (!strcmp(a, "--sha-frames")) sha_frames = true;
        else if (!strcmp(a, "--dump-audio") && next) dump_audio = argv[++i];
        else if (!strcmp(a, "--audio-hash")) audio_hash = true;
        else if (!strcmp(a, "--profile")) profile = true;
        else if (!strcmp(a, "--state-hash")) state_hash = true;
        else if (!strcmp(a, "--interpreter")) interpreter_only = true;
        else if (!strcmp(a, "--differential")) differential = true;
        else if (!strcmp(a, "--differential-step")) differential = single_step = true;
        else if (!strcmp(a, "--coverage")) coverage = true;
        else if (!strcmp(a, "--auto-input") || !strncmp(a, "--auto-input=", 13)) {
            g_auto_input = true;
            g_auto_run = a[12] ? (unsigned)strtoul(a + 13, NULL, 10) : 0;
        }
        else if (!strcmp(a, "--trace-out") && next) trace_out = argv[++i];
        else if (!strcmp(a, "--regs")) regs = true;
        else if (!strcmp(a, "--quiet")) quiet = true;
        else if (!strcmp(a, "--headless")) { /* what this runner always is; a game's window stays shut for it */ }
        else if ((!strcmp(a, "--keys") || !strcmp(a, "--touch") || !strcmp(a, "--lid")) && next &&
                 g_input_count < MAX_INPUTS) {
            Input *in = &g_inputs[g_input_count];
            memset(in, 0, sizeof *in);
            bool ok = parse_timed(argv[++i], in, what, sizeof what);
            if (ok && !strcmp(a, "--keys")) ok = parse_buttons(what, &in->keys) && in->keys;
            else if (ok && !strcmp(a, "--touch")) {
                char *comma;
                in->touch = true;
                in->x = (int)strtol(what, &comma, 10);
                ok = *comma == ',' && comma != what;
                if (ok) in->y = (int)strtol(comma + 1, NULL, 10);
            } else if (ok) {
                /* --lid FRAME[:N]: the middle field is the duration. */
                in->lid = true;
                if (what[0]) in->frames = strtoull(what, NULL, 10);
                ok = in->frames > 0;
            }
            if (!ok) {
                fprintf(stderr, "bad %s %s\n", a, argv[i]);
                return 2;
            }
            g_input_count++;
        } else if (!strcmp(a, "--trace") && next) {
            char *lo, *hi;
            unsigned long cpu = strtoul(argv[++i], &lo, 10);
            bool ok = *lo == ':' && (cpu == 9 || cpu == 7);
            if (ok) {
                g_trace_lo = (uint32_t)strtoul(lo + 1, &hi, 16);
                ok = *hi == ':';
                if (ok) g_trace_hi = (uint32_t)strtoul(hi + 1, NULL, 16);
            }
            if (!ok) {
                fprintf(stderr, "bad --trace %s (want 9:LO:HI or 7:LO:HI, hex)\n", argv[i]);
                return 2;
            }
            g_trace_cpu = cpu == 9 ? NDS_ARM9 : NDS_ARM7;
        } else if (a[0] != '-') rom_path = a;
        else return usage(argv[0]);
    }
    if (!rom_path && !(program && program->embedded_rom)) return usage(argv[0]);

    NdsMachine *m = create_machine(rom_path, bios9_path, bios7_path, program, !interpreter_only);
    if (!m) return 1;
    nds_set_internal_scale(m, internal_scale);
    size_t save_size = 0;
    uint8_t *save = load_save ? read_file(load_save, &save_size) : NULL;
    if (load_save && (!save || !nds_load_save(m, save, save_size))) {
        fprintf(stderr, "cannot use %s as this game's save\n", load_save);
        free(save);
        nds_destroy(m);
        return 1;
    }

    if (differential) {
        if (!program || interpreter_only) {
            fprintf(stderr, "--differential needs a recompiled program\n");
            nds_destroy(m);
            return 2;
        }
        NdsMachine *reference = create_machine(rom_path, bios9_path, bios7_path, program, false);
        if (!reference || (save && !nds_load_save(reference, save, save_size))) {
            if (reference) nds_destroy(reference);
            free(save);
            nds_destroy(m);
            return 1;
        }
        free(save);
        reference->debug_log = NULL; /* the same text as the first machine's */
        int result = run_differential(m, reference, frames, single_step);
        nds_destroy(reference);
        nds_destroy(m);
        return result;
    }

    free(save);
    if (g_trace_cpu >= 0) m->step_hook = trace_hook;
    else if (trace_out) m->step_hook = visit_hook;

    FILE *audio_file = dump_audio ? fopen(dump_audio, "wb") : NULL;
    if (dump_audio && !audio_file) {
        fprintf(stderr, "cannot write %s\n", dump_audio);
        nds_destroy(m);
        return 1;
    }
    uint64_t audio_frames = 0, audio_fnv = 1469598103934665603ull;
    if (profile) {
#ifdef NDS_PROFILE
        nds_prof_reset();
#else
        fprintf(stderr, "--profile needs a runtime built with NDS_PROFILE\n");
        nds_destroy(m);
        return 2;
#endif
    }

    for (uint64_t frame = 0; frame < frames; frame++) {
        apply_inputs(m, frame);
        nds_run_frame(m);
        /* Collect the frame's sound (about 546 stereo frames of it). */
        int16_t audio[1024 * 2];
        size_t got;
        while ((got = nds_audio_read(m, audio, 1024)) != 0) {
            uint8_t bytes[1024 * 4];
            for (size_t i = 0; i < got * 2; i++) {
                bytes[i * 2] = (uint8_t)((uint16_t)audio[i] & 0xFFu);
                bytes[i * 2 + 1] = (uint8_t)((uint16_t)audio[i] >> 8);
            }
            audio_fnv = hash_bytes(audio_fnv, bytes, got * 4);
            audio_frames += got;
            if (audio_file) fwrite(bytes, 1, got * 4, audio_file);
        }
        if (hash_frames) printf("frame %" PRIu64 " %016" PRIX64 "\n", frame, hash_frame(m));
        for (int large = 0; large < 2; large++) {
            const char *base = large ? dump_large : dump_frame;
            if (!base || !dump_every || frame + 1 < dump_from || (frame + 1) % dump_every != 0) continue;
            /* FILE with the frame number before its extension. */
            char path[1024];
            const char *dot = strrchr(base, '.');
            const char *slash = strpbrk(base, "/\\") ? base + strlen(base) : base;
            while (slash > base && slash[-1] != '/' && slash[-1] != '\\') slash--;
            if (!dot || dot < slash) dot = base + strlen(base);
            snprintf(path, sizeof path, "%.*s_%06" PRIu64 "%s", (int)(dot - base), base, frame + 1, dot);
            if (!(large ? write_large_ppm(m, path) : write_ppm(m, path))) {
                fprintf(stderr, "cannot write %s\n", path);
                nds_destroy(m);
                return 1;
            }
        }
        if (sha_frames) {
            char top[65], bottom[65];
            sha_screen(m->framebuffer[0], top);
            sha_screen(m->framebuffer[1], bottom);
            printf("frame %" PRIu64 " top=%s bottom=%s\n", frame + 1, top, bottom);
        }
    }

    int status = 0;
    if (dump_frame && !write_ppm(m, dump_frame)) {
        fprintf(stderr, "cannot write %s\n", dump_frame);
        status = 1;
    }
    if (dump_large && !write_large_ppm(m, dump_large)) {
        fprintf(stderr, "cannot write %s\n", dump_large);
        status = 1;
    }
    if (dump_ram) {
        FILE *f = fopen(dump_ram, "wb");
        if (!f || fwrite(m->main_ram, 1, sizeof m->main_ram, f) != sizeof m->main_ram) status = 1;
        if (f && fclose(f) != 0) status = 1;
        if (status) fprintf(stderr, "cannot write %s\n", dump_ram);
    }
    for (int i = 0; i < g_mem_dump_count; i++) {
        const MemDump *d = &g_mem_dumps[i];
        FILE *f = fopen(d->path, "wb");
        bool ok = f != NULL;
        for (uint32_t at = 0; ok && at < d->size; at++) ok = fputc(nds9_peek8(m, d->addr + at), f) != EOF;
        if (f && fclose(f) != 0) ok = false;
        if (!ok) {
            fprintf(stderr, "cannot write %s\n", d->path);
            status = 1;
        }
    }
    if (dump_save) {
        uint32_t size = 0;
        const uint8_t *data = nds_save_memory(m, &size);
        FILE *f = fopen(dump_save, "wb");
        bool ok = f && (!size || fwrite(data, 1, size, f) == size);
        if (f && fclose(f) != 0) ok = false;
        if (!ok) {
            fprintf(stderr, "cannot write %s\n", dump_save);
            status = 1;
        }
    }
    if (regs) {
        print_regs("arm9", &m->arm9);
        print_regs("arm7", &m->arm7);
    }
    if (trace_out && !write_visits(trace_out)) {
        fprintf(stderr, "cannot write %s\n", trace_out);
        status = 1;
    }
    if (audio_file && fclose(audio_file) != 0) {
        fprintf(stderr, "cannot write %s\n", dump_audio);
        status = 1;
    }
#ifdef NDS_PROFILE
    if (profile) nds_prof_report();
#endif
    if (audio_hash) printf("audio %016" PRIX64 " (%" PRIu64 " frames)\n", audio_fnv, audio_frames);
    if (state_hash) printf("state %016" PRIX64 "\n", hash_state(m));
    if (coverage) {
        printf("native: arm9 %.2f%% (%" PRIu64 " of %" PRIu64 " instructions interpreted), arm7 %.2f%% (%" PRIu64
               " of %" PRIu64 ")\n",
               native_share(m, NDS_ARM9), m->interpreted[NDS_ARM9], m->instructions[NDS_ARM9],
               native_share(m, NDS_ARM7), m->interpreted[NDS_ARM7], m->instructions[NDS_ARM7]);
        printf("native calls: arm9 %" PRIu64 ", arm7 %" PRIu64 "; code compared with memory %" PRIu64 " times\n",
               m->rc_calls[NDS_ARM9], m->rc_calls[NDS_ARM7], m->rc_compared);
    }
    if (!quiet) {
        printf("%" PRIu64 " frames, arm9 %" PRIu64 " instructions, arm7 %" PRIu64 " instructions, frame hash %016" PRIX64 "\n",
               frames, m->instructions[NDS_ARM9], m->instructions[NDS_ARM7], hash_frame(m));
    }
    nds_destroy(m);
    return status;
}
