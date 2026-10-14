/*
 * Headless runner shared by snes_run (interpreter only) and by every
 * recompiled game executable, so test tooling drives both the same way.
 *
 *   <exe> [ROM] [--frames N] [--until-idle] [--expect-text STR]
 *               [--screen-text] [--expect-wram ADDR=VALUE]...
 *               [--dump-frame FILE.ppm] [--dump-state FILE]
 *               [--mode7-scale N] [--dump-large FILE.ppm]
 *               [--dump-audio FILE.wav] [--keys FRAME:BUTTONS[:N]]...
 *               [--auto-input] [--interpreter] [--coverage]
 *               [--trace-out FILE] [--differential] [--quiet]
 *
 * When a recompiled program is linked in, --interpreter ignores it; the
 * summary reports the share of instructions that ran as native code.
 * --differential runs the recompiled program and the interpreter side by
 * side for --frames frames and stops at the first difference: CPU
 * registers and the cycle counter after every native slice, WRAM, VRAM,
 * CGRAM, OAM and audio RAM at the end of every frame. --coverage prints
 * where execution fell back to the interpreter; --trace-out writes the
 * entry points it saw for snesrecomp --trace.
 *
 * Headless runs are deterministic: no host clock, fixed power-on memory,
 * scripted input, and battery RAM stays in memory (nothing is written next
 * to the executable).
 *
 * --until-idle stops once the CPU sits on a branch-to-self (BRA *, JMP *,
 * JML *) or has stopped (STP, or WAI with no interrupt that can wake it).
 * Reaching the frame limit first is a failure (fail closed).
 *
 * --screen-text prints the text of the tile backgrounds, reading each map
 * entry's tile number as an ASCII code; --expect-text passes when that
 * text contains STR. --expect-wram checks a byte of work RAM (ADDR is a
 * 24-bit address in banks 7E/7F or a low-RAM mirror).
 */
#include "snes_runner.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes.h"
#include "snes_recomp.h"

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

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool snes_dump_ppm(const SnesMachine *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const int w = m->ppu.out_width ? m->ppu.out_width : 256, h = m->ppu.out_height ? m->ppu.out_height : 224;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = m->ppu.framebuffer[y * SNES_FB_WIDTH + x];
            uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return true;
}

/* --dump-large: the larger picture (snes_ppu_large_frame), or the console's
 * own when the last frame has none. */
static bool dump_large_ppm(const SnesMachine *m, const char *path)
{
    int w = 0, h = 0;
    const uint32_t *frame = snes_ppu_large_frame(&m->ppu, &w, &h);
    if (!frame) return snes_dump_ppm(m, path);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        uint8_t rgb[3] = { (uint8_t)(frame[i] >> 16), (uint8_t)(frame[i] >> 8), (uint8_t)frame[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

/* Stable layout: a tag and length before each block, little-endian. */
static void dump_block(FILE *f, const char *tag, const void *data, uint32_t size)
{
    uint8_t len[4];
    put_le32(len, size);
    fwrite(tag, 1, 4, f);
    fwrite(len, 1, 4, f);
    fwrite(data, 1, size, f);
}

/* A CPU's registers and clock: 16 bytes of registers, then the cycle count. */
static void cpu_regs(const SnesCpu *c, uint8_t regs[24])
{
    memset(regs, 0, 24);
    regs[0] = (uint8_t)c->a; regs[1] = (uint8_t)(c->a >> 8);
    regs[2] = (uint8_t)c->x; regs[3] = (uint8_t)(c->x >> 8);
    regs[4] = (uint8_t)c->y; regs[5] = (uint8_t)(c->y >> 8);
    regs[6] = (uint8_t)c->s; regs[7] = (uint8_t)(c->s >> 8);
    regs[8] = (uint8_t)c->d; regs[9] = (uint8_t)(c->d >> 8);
    regs[10] = (uint8_t)c->pc; regs[11] = (uint8_t)(c->pc >> 8);
    regs[12] = c->pbr;
    regs[13] = c->dbr;
    regs[14] = snes_cpu_get_p(c);
    regs[15] = c->flag_e;
    put_le32(regs + 16, (uint32_t)c->cycles);
    put_le32(regs + 20, (uint32_t)(c->cycles >> 32));
}

bool snes_dump_state(SnesMachine *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t regs[32];
    memset(regs, 0, sizeof regs);
    cpu_regs(&m->cpu, regs);
    put_le32(regs + 24, (uint32_t)m->frame_count);
    regs[28] = (uint8_t)m->vcounter;
    regs[29] = (uint8_t)(m->vcounter >> 8);
    dump_block(f, "CPU ", regs, sizeof regs);
    dump_block(f, "WRAM", m->wram, SNES_WRAM_SIZE);
    dump_block(f, "VRAM", snes_ppu_vram(&m->ppu), 0x10000u);
    dump_block(f, "CGRM", snes_ppu_cgram(&m->ppu), 0x200u);
    dump_block(f, "OAM ", snes_ppu_oam(&m->ppu), 0x220u);
    dump_block(f, "SRAM", m->sram, m->cart.sram_size);
    dump_block(f, "ARAM", snes_apu_ram(&m->apu), 0x10000u);
    if (m->sa1.present) {
        uint8_t sa1[24];
        cpu_regs(&m->sa1.cpu, sa1);
        dump_block(f, "SA1 ", sa1, sizeof sa1);
        dump_block(f, "IRAM", m->sa1.iram, sizeof m->sa1.iram);
    }
    fclose(f);
    return true;
}

void snes_screen_text(SnesMachine *m, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    const uint8_t *vram = snes_ppu_vram(&m->ppu);
    uint8_t mode = snes_ppu_reg(&m->ppu, 0x05) & 7u;
    if (mode == 7) return;
    uint8_t tm = snes_ppu_reg(&m->ppu, 0x2C);
    if (!(tm & 0x0Fu)) tm = 1; /* nothing enabled: look at BG1 */
    for (int bg = 0; bg < 4; bg++) {
        if (!(tm & (1u << bg))) continue;
        uint32_t base = (uint32_t)(snes_ppu_reg(&m->ppu, (uint8_t)(0x07 + bg)) & 0xFCu) << 8; /* word address */
        for (int y = 0; y < 28; y++) {
            char line[33];
            int end = 0;
            for (int x = 0; x < 32; x++) {
                uint32_t word = (base + (uint32_t)(y * 32 + x)) & 0x7FFFu;
                uint32_t tile = (vram[word * 2u] | (vram[word * 2u + 1u] << 8)) & 0x3FFu;
                char ch = (tile >= 0x20 && tile < 0x7F) ? (char)tile : ' ';
                line[x] = ch;
                if (ch != ' ') end = x + 1;
            }
            line[end] = '\0';
            if (end && len + (size_t)end + 2 < cap) len += (size_t)snprintf(out + len, cap - len, "%s\n", line);
        }
    }
}

/* --keys FRAME:BUTTONS[:DURATION]: hold BUTTONS (A+B+START...) from FRAME
 * for DURATION frames (default 6). */
typedef struct {
    uint64_t frame, duration;
    uint16_t keys;
} KeyPress;

static bool parse_keys(const char *spec, KeyPress *out)
{
    static const struct { const char *name; uint16_t bit; } names[] = {
        { "A", SNES_BTN_A }, { "B", SNES_BTN_B }, { "X", SNES_BTN_X }, { "Y", SNES_BTN_Y },
        { "L", SNES_BTN_L }, { "R", SNES_BTN_R }, { "SELECT", SNES_BTN_SELECT }, { "START", SNES_BTN_START },
        { "UP", SNES_BTN_UP }, { "DOWN", SNES_BTN_DOWN }, { "LEFT", SNES_BTN_LEFT }, { "RIGHT", SNES_BTN_RIGHT },
    };
    char buf[128];
    snprintf(buf, sizeof buf, "%s", spec);
    char *colon = strchr(buf, ':');
    if (!colon) return false;
    *colon = '\0';
    out->frame = strtoull(buf, NULL, 10);
    char *keys = colon + 1;
    char *colon2 = strchr(keys, ':');
    out->duration = 6;
    if (colon2) {
        *colon2 = '\0';
        out->duration = strtoull(colon2 + 1, NULL, 10);
    }
    out->keys = 0;
    for (char *tok = keys; tok && *tok;) {
        char *plus = strchr(tok, '+');
        if (plus) *plus = '\0';
        bool found = false;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            if (!strcmp(tok, names[i].name)) {
                out->keys |= names[i].bit;
                found = true;
            }
        }
        if (!found) return false;
        tok = plus ? plus + 1 : NULL;
    }
    return true;
}

static bool g_auto_input;
static bool g_trace_apu;

static uint16_t keys_at(const KeyPress *presses, int count, uint64_t frame)
{
    if (g_auto_input) return snes_auto_input(frame);
    uint16_t keys = 0;
    for (int i = 0; i < count; i++) {
        if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].duration) keys |= presses[i].keys;
    }
    return keys;
}

/* One press every 30 frames from frame 300: START then three A presses
 * (title screens, file select, dialogue) for 80 presses, then a cycle of
 * movement and the other buttons. START comes in pairs so a pause menu it
 * opens is closed again. Directions are held for most of a slot. */
uint16_t snes_auto_input(uint64_t frame)
{
    static const uint16_t play[] = {
        SNES_BTN_A, SNES_BTN_RIGHT, SNES_BTN_B, SNES_BTN_DOWN, SNES_BTN_Y, SNES_BTN_LEFT, SNES_BTN_A, SNES_BTN_UP,
        SNES_BTN_R, SNES_BTN_RIGHT, SNES_BTN_X, SNES_BTN_DOWN, SNES_BTN_L, SNES_BTN_LEFT, SNES_BTN_B, SNES_BTN_UP,
        SNES_BTN_START, SNES_BTN_A, SNES_BTN_START, SNES_BTN_Y,
    };
    const uint64_t first = 300, slot_frames = 30, menu_slots = 80;
    if (frame < first) return 0;
    uint64_t slot = (frame - first) / slot_frames, at = (frame - first) % slot_frames;
    if (slot < menu_slots) return at < 6 ? (slot % 4 == 0 ? SNES_BTN_START : SNES_BTN_A) : 0;
    uint16_t key = play[(slot - menu_slots) % (sizeof play / sizeof play[0])];
    bool direction = (key & (SNES_BTN_RIGHT | SNES_BTN_LEFT | SNES_BTN_UP | SNES_BTN_DOWN)) != 0;
    return at < (direction ? 24u : 6u) ? key : 0;
}

/* The CPU loops on itself or has stopped for good. */
static bool at_idle_loop(SnesMachine *m)
{
    const SnesCpu *c = &m->cpu;
    if (c->stp) return true;
    if (c->wai && !(m->nmitimen & 0xB0u)) return true;
    uint32_t pc = snes_cpu_pc24(c);
    uint8_t op = snes_peek(m, pc);
    uint8_t b1 = snes_peek(m, (pc & 0xFF0000u) | ((pc + 1u) & 0xFFFFu));
    uint8_t b2 = snes_peek(m, (pc & 0xFF0000u) | ((pc + 2u) & 0xFFFFu));
    if (op == 0x80 && b1 == 0xFE) return true;                                   /* BRA * */
    if (op == 0x4C && (uint16_t)(b1 | (b2 << 8)) == c->pc) return true;          /* JMP * */
    if (op == 0x5C && (uint16_t)(b1 | (b2 << 8)) == c->pc &&
        snes_peek(m, (pc & 0xFF0000u) | ((pc + 3u) & 0xFFFFu)) == c->pbr) return true; /* JML * */
    return false;
}

/* --dump-audio: 16-bit stereo WAV at SNES_APU_RATE; sizes patched on close.
 * Fields are stored one by one (MSVC 19.51 /O2 once miscompiled a loop of
 * shifted stores). */
static void wav_header(FILE *f, uint32_t frames)
{
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
    put_le32(h + 4, 36u + frames * 4u);
    put_le32(h + 24, SNES_APU_RATE);
    put_le32(h + 28, SNES_APU_RATE * 4u);
    put_le32(h + 40, frames * 4u);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

static uint32_t drain_audio(SnesMachine *m, FILE *f)
{
    static int16_t buf[4096 * 2];
    uint32_t total = 0;
    size_t n;
    while ((n = snes_apu_read_samples(&m->apu, buf, 4096)) > 0) {
        if (f) fwrite(buf, 4, n, f);
        total += (uint32_t)n;
    }
    return total;
}

static SnesMachine *create_machine(const char *rom_path, const struct SnesRcProgram *program,
                                   const uint8_t *(*embedded)(size_t *))
{
    (void)program;
    SnesMachine *m = snes_create();
    if (!m) return NULL;
    size_t size = 0;
    if (!rom_path) {
        const uint8_t *data = embedded ? embedded(&size) : NULL;
        if (!data || !snes_load_rom(m, data, size)) {
            fprintf(stderr, "no ROM given and none is embedded in this program\n");
            snes_destroy(m);
            return NULL;
        }
    } else {
        uint8_t *rom = read_file(rom_path, &size);
        if (!rom || !snes_load_rom(m, rom, size)) {
            fprintf(stderr, "cannot load ROM %s (missing, or no SNES header found)\n", rom_path);
            free(rom);
            snes_destroy(m);
            return NULL;
        }
        free(rom);
    }
    snes_power(m);
    return m;
}

typedef struct {
    uint32_t addr;
    uint8_t value;
} WramExpect;

/* --trace-last N: a ring of the last N interpreted instructions, printed
 * at exit (development aid). */
typedef struct {
    uint32_t pc;
    uint16_t a, x, y, s;
    uint8_t p, e;
    uint16_t v;
} TraceEntry;
static TraceEntry *g_trace;
static uint32_t g_trace_len, g_trace_pos;

static void trace_hook(SnesMachine *m)
{
    const SnesCpu *c = &m->cpu;
    TraceEntry *t = &g_trace[g_trace_pos++ % g_trace_len];
    t->pc = snes_cpu_pc24(c);
    t->a = c->a;
    t->x = c->x;
    t->y = c->y;
    t->s = c->s;
    t->p = snes_cpu_get_p(c);
    t->e = c->flag_e;
    t->v = m->vcounter;
}

/* --apu-console: text a test prints through the APU ports with blargg's
 * protocol (port 0 = 'C', port 1 = the character). */
static bool g_apu_console;
static char g_console[1u << 16];
static size_t g_console_len;

/* --trace-apu: the S-CPU's port accesses, with repeated reads collapsed. */
static void apu_port_hook(SnesMachine *m, unsigned port, uint8_t value, bool write)
{
    static unsigned last_port = 99;
    static uint8_t last_value;
    static bool last_write;
    static unsigned repeats;
    if (g_apu_console && !write && port == 1 && !last_write && last_port == 0 && last_value == 0x43 &&
        g_console_len + 1 < sizeof g_console) {
        g_console[g_console_len++] = (char)((value >= 0x20 && value < 0x7F) || value == '\n' ? value : '.');
        g_console[g_console_len] = '\0';
    }
    if (!g_trace_apu) {
        last_port = port;
        last_value = value;
        last_write = write;
        return;
    }
    if (!write && !last_write && port == last_port && value == last_value) {
        repeats++;
        return;
    }
    if (repeats) printf("    (x%u)\n", repeats);
    repeats = 0;
    printf("  f%llu v%u %s $214%u %s %02X\n", (unsigned long long)m->frame_count, m->vcounter, write ? "W" : "R", port,
           write ? "<-" : "->", value);
    last_port = port;
    last_value = value;
    last_write = write;
}

static void trace_print(SnesMachine *m)
{
    uint32_t n = g_trace_pos < g_trace_len ? g_trace_pos : g_trace_len;
    for (uint32_t i = 0; i < n; i++) {
        const TraceEntry *t = &g_trace[(g_trace_pos - n + i) % g_trace_len];
        printf("  %02X:%04X %02X  a=%04X x=%04X y=%04X s=%04X p=%02X e=%d v=%d\n", (unsigned)(t->pc >> 16),
               (unsigned)(t->pc & 0xFFFFu), snes_peek(m, t->pc), t->a, t->x, t->y, t->s, t->p, t->e, t->v);
    }
}

bool snes_profile_rom(const uint8_t *rom, size_t size, uint64_t frames, const char *trace_path,
                      uint64_t *instructions)
{
    SnesMachine *m = snes_create();
    if (!m || !snes_load_rom(m, rom, size)) {
        snes_destroy(m);
        return false;
    }
    snes_power(m);
    snes_coverage_enable(m);
    for (uint64_t frame = 0; frame < frames; frame++) {
        snes_set_buttons(m, 0, snes_auto_input(frame));
        snes_run_frame(m);
        drain_audio(m, NULL);
    }
    bool ok = snes_coverage_write_trace(m, trace_path);
    if (instructions) *instructions = m->cpu.instructions;
    snes_destroy(m);
    return ok;
}

static void print_cpu(const char *tag, const SnesCpu *c)
{
    printf("  %-11s pc=%02X:%04X a=%04X x=%04X y=%04X s=%04X d=%04X dbr=%02X p=%02X e=%d cycles=%" PRIu64
           " instructions=%" PRIu64 "\n",
           tag, c->pbr, c->pc, c->a, c->x, c->y, c->s, c->d, c->dbr, snes_cpu_get_p(c), c->flag_e, c->cycles,
           c->instructions);
}

static void print_state(const char *tag, const SnesMachine *m)
{
    print_cpu(tag, &m->cpu);
    if (m->sa1.present) print_cpu("  SA-1", &m->sa1.cpu);
}

static bool same_cpu(const SnesCpu *a, const SnesCpu *b)
{
    return a->a == b->a && a->x == b->x && a->y == b->y && a->s == b->s && a->d == b->d && a->pc == b->pc &&
           a->dbr == b->dbr && a->pbr == b->pbr && snes_cpu_get_p(a) == snes_cpu_get_p(b) && a->flag_e == b->flag_e &&
           a->cycles == b->cycles && a->instructions == b->instructions && a->wai == b->wai && a->stp == b->stp;
}

/* Recompiled machine `a` against interpreter `b`, in lockstep: after every
 * run-loop slice of `a`, `b` catches up to the same instruction count and
 * the CPU state and cycle counter must match exactly. Memory is compared at
 * the end of every frame. Differential mode is not a hardware oracle: both
 * machines share the same devices; it proves the recompiled code executes
 * the program exactly as the interpreter does. */
static int run_differential(SnesMachine *a, SnesMachine *b, uint64_t frames, const KeyPress *presses, int press_count)
{
    if (!b) return 2;
    uint64_t slices = 0, frame = 0;
    uint32_t last_pc = 0;
    snes_set_buttons(a, 0, keys_at(presses, press_count, 0));
    snes_set_buttons(b, 0, keys_at(presses, press_count, 0));
    while (frame < frames) {
        last_pc = snes_cpu_pc24(&a->cpu);
        a->frame_ready = false;
        snes_run_slice(a, UINT64_MAX);
        slices++;
        while (b->cpu.instructions < a->cpu.instructions) snes_run_slice(b, UINT64_MAX);
        while (b->cpu.instructions == a->cpu.instructions && b->cpu.cycles < a->cpu.cycles && (b->cpu.wai || b->cpu.stp))
            snes_run_slice(b, a->cpu.cycles);
        bool same = same_cpu(&a->cpu, &b->cpu) && (!a->sa1.present || same_cpu(&a->sa1.cpu, &b->sa1.cpu));
        if (a->frame_ready) {
            frame++;
            snes_sync_apu(a);
            snes_sync_apu(b);
            same = same && memcmp(a->wram, b->wram, sizeof a->wram) == 0 &&
                   memcmp(snes_ppu_vram(&a->ppu), snes_ppu_vram(&b->ppu), 0x10000u) == 0 &&
                   memcmp(snes_ppu_cgram(&a->ppu), snes_ppu_cgram(&b->ppu), 0x200u) == 0 &&
                   memcmp(snes_ppu_oam(&a->ppu), snes_ppu_oam(&b->ppu), 0x220u) == 0 &&
                   memcmp(snes_apu_ram(&a->apu), snes_apu_ram(&b->apu), 0x10000u) == 0 &&
                   memcmp(a->sram, b->sram, sizeof a->sram) == 0 &&
                   memcmp(a->sa1.iram, b->sa1.iram, sizeof a->sa1.iram) == 0;
            drain_audio(a, NULL);
            drain_audio(b, NULL);
            snes_set_buttons(a, 0, keys_at(presses, press_count, frame));
            snes_set_buttons(b, 0, keys_at(presses, press_count, frame));
        }
        if (!same) {
            printf("DIVERGED after slice %" PRIu64 " (frame %" PRIu64 "), slice entered at %02X:%04X\n", slices, frame,
                   (unsigned)(last_pc >> 16), (unsigned)(last_pc & 0xFFFFu));
            print_state("recompiled", a);
            print_state("interpreter", b);
            snes_destroy(b);
            return 1;
        }
    }
    const SnesCpu *c = &a->cpu;
    printf("differential: %" PRIu64 " frames, %" PRIu64 " instructions (%.1f%% native)", frame, c->instructions,
           c->instructions ? 100.0 * (double)(c->instructions - c->interpreted) / (double)c->instructions : 0.0);
    if (a->sa1.present) {
        const SnesCpu *s = &a->sa1.cpu;
        printf(", SA-1 %" PRIu64 " (%.1f%% native)", s->instructions,
               s->instructions ? 100.0 * (double)(s->instructions - s->interpreted) / (double)s->instructions : 0.0);
    }
    printf(", no divergence\nPASS\n");
    snes_destroy(b);
    return 0;
}

int snes_runner_main(int argc, char **argv, const struct SnesRcProgram *program, const char *default_rom)
{
    const char *rom_path = NULL, *frame_path = NULL, *expect_text = NULL, *state_path = NULL, *audio_path = NULL;
    const char *trace_path = NULL, *fail_text = NULL, *large_path = NULL;
    int mode7_scale = 1;
    bool until_idle = false, show_text = false, quiet = false, differential = false, coverage = false;
    uint64_t max_frames = 600;
    KeyPress presses[64];
    int press_count = 0;
    WramExpect wexpect[32];
    int wexpect_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--frames") && i + 1 < argc) max_frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--until-idle")) until_idle = true;
        else if (!strcmp(a, "--dump-frame") && i + 1 < argc) frame_path = argv[++i];
        else if (!strcmp(a, "--dump-state") && i + 1 < argc) state_path = argv[++i];
        else if (!strcmp(a, "--dump-large") && i + 1 < argc) large_path = argv[++i];
        else if (!strcmp(a, "--mode7-scale") && i + 1 < argc) mode7_scale = atoi(argv[++i]);
        else if (!strcmp(a, "--dump-audio") && i + 1 < argc) audio_path = argv[++i];
        else if (!strcmp(a, "--quiet")) quiet = true;
        else if (!strcmp(a, "--screen-text")) show_text = true;
        else if (!strcmp(a, "--interpreter")) program = NULL;
        else if (!strcmp(a, "--differential")) differential = true;
        else if (!strcmp(a, "--coverage")) coverage = true;
        else if (!strcmp(a, "--trace-out") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(a, "--auto-input")) g_auto_input = true;
        else if (!strcmp(a, "--trace-last") && i + 1 < argc) g_trace_len = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--trace-apu")) g_trace_apu = true;
        else if (!strcmp(a, "--apu-console")) g_apu_console = true;
        else if (!strcmp(a, "--fail-text") && i + 1 < argc) fail_text = argv[++i];
        else if (!strcmp(a, "--keys") && i + 1 < argc && press_count < 64) {
            if (!parse_keys(argv[++i], &presses[press_count++])) {
                fprintf(stderr, "bad --keys %s (want FRAME:BUTTON[+BUTTON...][:DURATION])\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--expect-text") && i + 1 < argc) expect_text = argv[++i];
        else if (!strcmp(a, "--expect-wram") && i + 1 < argc && wexpect_count < 32) {
            const char *e = argv[++i];
            const char *eq = strchr(e, '=');
            if (!eq) {
                fprintf(stderr, "bad --expect-wram %s (want ADDR=VALUE)\n", e);
                return 2;
            }
            wexpect[wexpect_count].addr = (uint32_t)strtoul(e, NULL, 16);
            wexpect[wexpect_count].value = (uint8_t)strtoul(eq + 1, NULL, 0);
            wexpect_count++;
        } else if (a[0] != '-' && !rom_path) rom_path = a;
        else {
            fprintf(stderr, "unknown argument %s\n", a);
            return 2;
        }
    }
    if (!rom_path) rom_path = default_rom;
    const uint8_t *(*embedded)(size_t *) = program ? program->embedded_rom : NULL;
    if (!rom_path && !embedded) {
        fprintf(stderr, "usage: snes_run ROM [--frames N] [--until-idle] [--expect-text STR] [--screen-text]\n"
                        "                [--expect-wram ADDR=VALUE]... [--dump-frame FILE.ppm] [--dump-state FILE]\n"
                        "                [--dump-audio FILE.wav] [--keys FRAME:BUTTONS[:N]]... [--auto-input]\n"
                        "                [--interpreter] [--quiet]\n");
        return 2;
    }

    SnesMachine *m = create_machine(rom_path, program, embedded);
    if (m && mode7_scale != 1 && (mode7_scale < 1 || !snes_ppu_set_mode7_scale(&m->ppu, mode7_scale))) {
        fprintf(stderr, "--mode7-scale wants 1 to %d\n", SNES_MODE7_SCALE_MAX);
        snes_destroy(m);
        return 2;
    }
    if (!m) return 2;
    if (program && !snes_rc_install(m, program)) {
        fprintf(stderr, "%s does not match the ROM this program was built from; running interpreted\n",
                rom_path ? rom_path : "(embedded)");
    }
    if (coverage || trace_path) snes_coverage_enable(m);
    if (differential) {
        if (!m->rc_program) {
            fprintf(stderr, "--differential needs a recompiled program\n");
            snes_destroy(m);
            return 2;
        }
        SnesMachine *b = create_machine(rom_path, program, embedded);
        int status = run_differential(m, b, max_frames, presses, press_count);
        snes_destroy(m);
        return status;
    }
    if (g_trace_len) {
        g_trace = calloc(g_trace_len, sizeof *g_trace);
        if (g_trace) m->trace = trace_hook;
    }
    if (g_trace_apu || g_apu_console) m->apu_port_trace = apu_port_hook;
    if (!quiet && m->cart.coprocessor)
        fprintf(stderr, "note: cartridge uses a %s coprocessor, which is not emulated\n", m->cart.coprocessor);

    FILE *audio = NULL;
    if (audio_path) {
        audio = fopen(audio_path, "wb");
        if (!audio) fprintf(stderr, "cannot write %s\n", audio_path);
        else wav_header(audio, 0);
    }
    uint32_t audio_frames = 0;

    static char text[4 * 28 * 34 + 1];
    bool idle = false;
    int64_t failed_at = -1;
    uint64_t frame = 0;
    for (; frame < max_frames; frame++) {
        if (press_count || g_auto_input) snes_set_buttons(m, 0, keys_at(presses, press_count, frame));
        snes_run_frame(m);
        audio_frames += drain_audio(m, audio);
        /* --fail-text: tests that page through results must never show it. */
        if (fail_text) {
            snes_screen_text(m, text, sizeof text);
            if (strstr(text, fail_text) || strstr(g_console, fail_text)) {
                failed_at = (int64_t)frame;
                if (show_text) printf("[screen at frame %" PRIu64 "]\n%s[/screen]\n", frame, text);
                break;
            }
        }
        if (until_idle && at_idle_loop(m)) {
            idle = true;
            snes_run_frame(m); /* one more frame so the result is on screen */
            frame++;
            break;
        }
    }

    const SnesCpu *c = &m->cpu;
    if (!quiet) {
        printf("rom=%s title=\"%s\" frames=%" PRIu64 " instructions=%" PRIu64 " cycles=%" PRIu64 "\n",
               rom_path ? rom_path : "(embedded)", m->cart.title, frame, c->instructions, c->cycles);
        printf("pc=%02X:%04X a=%04X x=%04X y=%04X s=%04X d=%04X dbr=%02X p=%02X e=%d\n", c->pbr, c->pc, c->a, c->x,
               c->y, c->s, c->d, c->dbr, snes_cpu_get_p(c), c->flag_e);
        const SnesSpc700 *smp = &m->apu.smp;
        printf("apu: pc=%04X a=%02X x=%02X y=%02X sp=%02X psw=%02X halt=%u ports cpu->apu %02X %02X %02X %02X"
               " apu->cpu %02X %02X %02X %02X\n",
               smp->pc, smp->a, smp->x, smp->y, smp->sp, snes_spc700_get_psw(smp), smp->halt, m->apu.cpu_to_apu[0],
               m->apu.cpu_to_apu[1], m->apu.cpu_to_apu[2], m->apu.cpu_to_apu[3], m->apu.apu_to_cpu[0],
               m->apu.apu_to_cpu[1], m->apu.apu_to_cpu[2], m->apu.apu_to_cpu[3]);
        if (m->sa1.present) {
            const SnesCpu *s = &m->sa1.cpu;
            printf("sa1: pc=%02X:%04X a=%04X x=%04X y=%04X s=%04X p=%02X e=%d instructions=%" PRIu64
                   " reset=%d wai=%d\n",
                   s->pbr, s->pc, s->a, s->x, s->y, s->s, snes_cpu_get_p(s), s->flag_e, s->instructions,
                   m->sa1.sa1_resb, s->wai);
        }
        if (m->rc_program && c->instructions) {
            uint64_t native = c->instructions - c->interpreted;
            printf("native=%.1f%% (%" PRIu64 " of %" PRIu64 " instructions)", 100.0 * (double)native / (double)c->instructions,
                   native, c->instructions);
            const SnesCpu *s = &m->sa1.cpu;
            if (m->sa1.present && s->instructions)
                printf(", SA-1 native=%.1f%%", 100.0 * (double)(s->instructions - s->interpreted) / (double)s->instructions);
            printf("\n");
        }
    }
    if (coverage) snes_coverage_report(m);
    if (trace_path && !snes_coverage_write_trace(m, trace_path)) fprintf(stderr, "cannot write %s\n", trace_path);
    if (audio) {
        audio_frames += drain_audio(m, audio);
        wav_header(audio, audio_frames);
        fclose(audio);
    }
    if (g_trace) {
        printf("[trace]\n");
        trace_print(m);
        free(g_trace);
        g_trace = NULL;
    }
    if (frame_path && !snes_dump_ppm(m, frame_path)) fprintf(stderr, "cannot write %s\n", frame_path);
    if (large_path && !dump_large_ppm(m, large_path)) fprintf(stderr, "cannot write %s\n", large_path);
    if (state_path && !snes_dump_state(m, state_path)) fprintf(stderr, "cannot write %s\n", state_path);

    snes_screen_text(m, text, sizeof text);
    if (show_text) printf("[screen]\n%s[/screen]\n", text);
    if (g_apu_console) printf("[console]\n%s\n[/console]\n", g_console);

    int status = 0;
    if (failed_at >= 0) {
        printf("FAIL: \"%s\" appeared at frame %" PRId64 "\n", fail_text, failed_at);
        status = 1;
    }
    if (expect_text && !strstr(text, expect_text) && !strstr(g_console, expect_text)) {
        printf("FAIL: screen does not contain \"%s\"\n", expect_text);
        status = 1;
    }
    if (until_idle && !idle) {
        printf("FAIL: not idle after %" PRIu64 " frames\n", frame);
        status = 1;
    }
    for (int i = 0; i < wexpect_count; i++) {
        uint32_t a = wexpect[i].addr;
        uint8_t got = ((a >> 16) == 0x7E || (a >> 16) == 0x7F) ? m->wram[a & 0x1FFFFu] : m->wram[a & 0x1FFFu];
        if (got != wexpect[i].value) {
            printf("FAIL: wram[%06" PRIX32 "]=%02X, expected %02X\n", a, got, wexpect[i].value);
            status = 1;
        }
    }
    if (status == 0 && (until_idle || expect_text || wexpect_count || fail_text)) printf("PASS\n");
    snes_destroy(m);
    return status;
}
