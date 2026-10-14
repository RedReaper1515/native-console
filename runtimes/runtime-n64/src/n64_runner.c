/*
 * Headless runner shared by n64_run (interpreter only) and by every
 * recompiled game executable, so test tooling drives both the same way.
 *
 *   <exe> [ROM] [--frames N] [--dump-frame FILE.ppm] [--dump-state FILE]
 *               [--dump-audio FILE.wav] [--keys FRAME:BUTTONS[:N]]...
 *               [--auto-input] [--interpreter] [--differential]
 *               [--coverage] [--trace-out FILE] [--save-dir DIR]
 *               [--isviewer] [--expect-isviewer STR] [--fail-isviewer STR]
 *               [--until-gpr REG=VALUE] [--expect-gpr REG=VALUE]...
 *               [--benchmark] [--quiet] [--dump-fb FILE.ppm [--fb-height N]]
 *               [--dump-every N] [--perf] [--dump-save FILE.sav]
 *               [--learn-out FILE.trace]
 *               [--hle-boot]
 *
 * --hle-boot skips the cartridge's IPL3 and starts from the state it
 * leaves (n64_boot.c); by default a known IPL3 runs.
 * --interpreter ignores a linked recompiled program; the summary reports
 * the share of instructions that ran as native code. --differential runs
 * the recompiled program and the interpreter side by side for --frames
 * frames and stops at the first difference: CPU registers and the cycle
 * counter after every native slice, RDRAM, DMEM, IMEM and the RSP's
 * registers at the end of every frame.
 *
 * Headless runs are deterministic (no host clock, fixed power-on state,
 * scripted input) and never write saves: --save-dir only says where to
 * read one from.
 *
 * Verdicts, fail closed: --until-gpr stops as soon as a CPU register holds
 * a value (Dillon Beliveau's n64-tests report in r30: -1 passes, any other
 * non-zero value is the failing test) and fails if --frames ends first;
 * --expect-gpr checks registers at the end; --expect-isviewer and
 * --fail-isviewer look at what the program printed through the ISViewer
 * port (libdragon and n64-systemtest print there).
 */
#include "n64_runner.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n64.h"
#include "n64_recomp.h"
#include "n64_coverage.h"
#include "n64_cpu_decode.h"

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

bool n64_dump_ppm(const N64Machine *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const int w = m->out_width ? m->out_width : 640, h = m->out_height ? m->out_height : 480;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = m->framebuffer[y * N64_FB_WIDTH + x];
            uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return true;
}

/* --dump-large: the larger picture (n64_large_frame), or the console's own
 * where there is none. */
static bool dump_large_ppm(const N64Machine *m, const char *path)
{
    int w = 0, h = 0;
    const uint32_t *frame = n64_large_frame(m, &w, &h);
    if (!frame) return n64_dump_ppm(m, path);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const size_t pitch = (size_t)N64_FB_WIDTH * (size_t)m->large_scale;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = frame[(size_t)y * pitch + (size_t)x];
            uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return true;
}

/* --dump-fb: the framebuffer at VI_ORIGIN, VI_WIDTH pixels wide, as the
 * RDP wrote it (16-bit RGBA5551 or 32-bit RGBA8888, per VI_CONTROL bits
 * 0-1), for comparison with reference screenshots. The height is
 * fb_height, or the VI's active lines (V_VIDEO) scaled by Y_SCALE. */
static bool dump_fb(const N64Machine *m, const char *path, int fb_height)
{
    const uint32_t ctrl = m->vi_regs[0], origin = m->vi_regs[1] & 0xffffffu, w = m->vi_regs[2] & 0xfffu;
    const uint32_t bpp = (ctrl & 3u) == 3u ? 4u : 2u;
    int h = fb_height;
    if (h <= 0) {
        uint32_t lines = (((m->vi_regs[10] & 0x3ffu) - ((m->vi_regs[10] >> 16) & 0x3ffu)) & 0x3ffu) >> 1;
        h = (int)((lines * (m->vi_regs[13] & 0xfffu)) >> 10);
    }
    if (!w || h <= 0) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %d\n255\n", w, h);
    for (uint32_t i = 0; i < w * (uint32_t)h; i++) {
        uint32_t a = (origin + i * bpp) & 0xffffffu, p = 0;
        uint8_t rgb[3];
        for (uint32_t k = 0; k < bpp; k++)
            p = (p << 8) | (a + k < m->rdram_size ? m->rdram[a + k] : 0);
        if (bpp == 4) {
            rgb[0] = (uint8_t)(p >> 24), rgb[1] = (uint8_t)(p >> 16), rgb[2] = (uint8_t)(p >> 8);
        } else {
            uint32_t r = (p >> 11) & 31, g = (p >> 6) & 31, b = (p >> 1) & 31;
            rgb[0] = (uint8_t)(r << 3 | r >> 2), rgb[1] = (uint8_t)(g << 3 | g >> 2), rgb[2] = (uint8_t)(b << 3 | b >> 2);
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

static void dump_block(FILE *f, const char *tag, const void *data, uint32_t size)
{
    uint8_t len[4];
    n64_put_le32(len, size);
    fwrite(tag, 1, 4, f);
    fwrite(len, 1, 4, f);
    fwrite(data, 1, size, f);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    n64_put_le32(p, (uint32_t)v);
    n64_put_le32(p + 4, (uint32_t)(v >> 32));
}

/* The CPU's architectural state in a stable little-endian layout. */
static void cpu_block(const N64Cpu *c, uint8_t *out, size_t *len)
{
    size_t n = 0;
    for (int i = 0; i < 32; i++, n += 8) put_le64(out + n, c->gpr[i]);
    put_le64(out + n, c->hi), n += 8;
    put_le64(out + n, c->lo), n += 8;
    put_le64(out + n, c->pc), n += 8;
    for (int i = 0; i < 32; i++, n += 8) put_le64(out + n, c->fpr[i]);
    n64_put_le32(out + n, c->fcr31), n += 4;
    n64_put_le32(out + n, c->cp0_status), n += 4;
    n64_put_le32(out + n, c->cp0_cause), n += 4;
    put_le64(out + n, c->cp0_epc), n += 8;
    put_le64(out + n, c->cycles), n += 8;
    put_le64(out + n, c->instructions), n += 8;
    *len = n;
}

bool n64_dump_state(N64Machine *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t regs[640];
    size_t len;
    cpu_block(&m->cpu, regs, &len);
    dump_block(f, "CPU ", regs, (uint32_t)len);
    dump_block(f, "RDRM", m->rdram, m->rdram_size);
    dump_block(f, "DMEM", m->rsp.dmem, sizeof m->rsp.dmem);
    dump_block(f, "IMEM", m->rsp.imem, sizeof m->rsp.imem);
    uint8_t rsp[32 * 4 + 8];
    for (int i = 0; i < 32; i++) n64_put_le32(rsp + 4 * i, m->rsp.r[i]);
    n64_put_le32(rsp + 128, m->rsp.pc);
    n64_put_le32(rsp + 132, m->rsp.status);
    dump_block(f, "RSP ", rsp, sizeof rsp);
    dump_block(f, "EEPR", m->eeprom, sizeof m->eeprom);
    dump_block(f, "SRAM", m->sram, 0x20000u);
    dump_block(f, "MPAK", m->mempak, sizeof m->mempak);
    uint8_t frame[8];
    put_le64(frame, m->frame_count);
    dump_block(f, "FRAM", frame, sizeof frame);
    fclose(f);
    return true;
}

/* ---- input ---- */

typedef struct {
    uint64_t frame, duration;
    uint16_t buttons;
    int8_t x, y;
} KeyPress;

static bool parse_keys(const char *spec, KeyPress *out)
{
    static const struct {
        const char *name;
        uint16_t bit;
        int8_t x, y;
    } names[] = {
        { "A", N64_BTN_A, 0, 0 },         { "B", N64_BTN_B, 0, 0 },          { "Z", N64_BTN_Z, 0, 0 },
        { "START", N64_BTN_START, 0, 0 }, { "L", N64_BTN_L, 0, 0 },          { "R", N64_BTN_R, 0, 0 },
        { "DUP", N64_BTN_DUP, 0, 0 },     { "DDOWN", N64_BTN_DDOWN, 0, 0 },  { "DLEFT", N64_BTN_DLEFT, 0, 0 },
        { "DRIGHT", N64_BTN_DRIGHT, 0, 0 }, { "CUP", N64_BTN_CUP, 0, 0 },    { "CDOWN", N64_BTN_CDOWN, 0, 0 },
        { "CLEFT", N64_BTN_CLEFT, 0, 0 }, { "CRIGHT", N64_BTN_CRIGHT, 0, 0 }, { "UP", 0, 0, 80 },
        { "DOWN", 0, 0, -80 },            { "LEFT", 0, -80, 0 },             { "RIGHT", 0, 80, 0 },
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
    out->buttons = 0;
    out->x = out->y = 0;
    for (char *tok = keys; tok && *tok;) {
        char *plus = strchr(tok, '+');
        if (plus) *plus = '\0';
        bool found = false;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            if (!strcmp(tok, names[i].name)) {
                out->buttons |= names[i].bit;
                if (names[i].x) out->x = names[i].x;
                if (names[i].y) out->y = names[i].y;
                found = true;
            }
        }
        if (!found) return false;
        tok = plus ? plus + 1 : NULL;
    }
    return true;
}

uint16_t n64_auto_input(uint64_t frame, int8_t *x, int8_t *y)
{
    static const struct {
        uint16_t buttons;
        int8_t x, y;
    } play[] = {
        { N64_BTN_A, 0, 0 },  { 0, 80, 0 },       { N64_BTN_B, 0, 0 },     { 0, 0, -80 },
        { N64_BTN_Z, 0, 0 },  { 0, -80, 0 },      { N64_BTN_A, 0, 0 },     { 0, 0, 80 },
        { N64_BTN_R, 0, 0 },  { N64_BTN_CUP, 0, 0 }, { N64_BTN_START, 0, 0 }, { N64_BTN_A, 0, 0 },
        { N64_BTN_START, 0, 0 }, { N64_BTN_CLEFT, 0, 0 }, { N64_BTN_L, 0, 0 }, { 0, 50, 50 },
    };
    *x = *y = 0;
    const uint64_t first = 300, slot_frames = 30, menu_slots = 80;
    if (frame < first) return 0;
    uint64_t slot = (frame - first) / slot_frames, at = (frame - first) % slot_frames;
    if (slot < menu_slots) return at < 6 ? (slot % 4 == 0 ? N64_BTN_START : N64_BTN_A) : 0;
    const size_t n = sizeof play / sizeof play[0];
    size_t k = (size_t)((slot - menu_slots) % n);
    bool stick = play[k].x || play[k].y;
    if (at >= (stick ? 24u : 6u)) return 0;
    *x = play[k].x;
    *y = play[k].y;
    return play[k].buttons;
}

static bool g_auto_input;

/* --trace-last N: a ring of the last N interpreted instructions, printed at
 * exit (development aid). */
typedef struct {
    uint64_t pc;
    uint32_t insn;
    uint64_t cycles;
} TraceEntry;
static TraceEntry *g_trace;
static uint32_t g_trace_len, g_trace_pos;

static void trace_hook(N64Machine *m)
{
    TraceEntry *t = &g_trace[g_trace_pos++ % g_trace_len];
    uint32_t pa;
    t->pc = m->cpu.pc;
    t->insn = n64_cpu_translate_quiet(&m->cpu, m->cpu.pc, &pa) ? (uint32_t)n64_peek(m, pa) << 24 |
                                                                     (uint32_t)n64_peek(m, pa + 1) << 16 |
                                                                     (uint32_t)n64_peek(m, pa + 2) << 8 |
                                                                     n64_peek(m, pa + 3)
                                                               : 0;
    t->cycles = m->cpu.cycles;
}

static void trace_print(void)
{
    uint32_t n = g_trace_pos < g_trace_len ? g_trace_pos : g_trace_len;
    for (uint32_t i = 0; i < n; i++) {
        const TraceEntry *t = &g_trace[(g_trace_pos - n + i) % g_trace_len];
        printf("  %016" PRIX64 ": %08X %-10s cycle %" PRIu64 "\n", t->pc, t->insn, n64_op_names[n64_decode(t->insn)],
               t->cycles);
    }
}

static void apply_input(N64Machine *m, const KeyPress *presses, int count, uint64_t frame)
{
    int8_t x = 0, y = 0;
    uint16_t buttons = 0;
    if (g_auto_input) {
        buttons = n64_auto_input(frame, &x, &y);
    } else {
        for (int i = 0; i < count; i++) {
            if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].duration) {
                buttons |= presses[i].buttons;
                if (presses[i].x) x = presses[i].x;
                if (presses[i].y) y = presses[i].y;
            }
        }
    }
    n64_set_buttons(m, 0, buttons, x, y);
}

/* ---- audio dump ---- */

static void wav_header(FILE *f, uint32_t frames)
{
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
    n64_put_le32(h + 4, 36u + frames * 4u);
    n64_put_le32(h + 24, N64_AUDIO_RATE);
    n64_put_le32(h + 28, N64_AUDIO_RATE * 4u);
    n64_put_le32(h + 40, frames * 4u);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

static uint32_t drain_audio(N64Machine *m, FILE *f)
{
    static int16_t buf[4096 * 2];
    uint32_t total = 0;
    size_t n;
    while ((n = n64_read_audio(m, buf, 4096)) > 0) {
        if (f) {
            uint8_t le[4096 * 4];
            for (size_t i = 0; i < 2 * n; i++) {
                le[2 * i] = (uint8_t)buf[i];
                le[2 * i + 1] = (uint8_t)((uint16_t)buf[i] >> 8);
            }
            fwrite(le, 4, n, f);
        }
        total += (uint32_t)n;
    }
    return total;
}

/* ---- machines ---- */

/* <dir>/<stem of name>.sav: the save a game executable keeps, named after
 * the ROM file, or after the executable when the ROM is embedded (the hub
 * keeps <game id>.sav next to <game id>.exe). */
static void save_path_for(char *out, size_t cap, const char *dir, const char *name)
{
    const char *leaf = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') leaf = p + 1;
    }
    char stem[256];
    snprintf(stem, sizeof stem, "%s", leaf);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    const size_t n = strlen(dir);
    const bool slash = n && dir[n - 1] != '/' && dir[n - 1] != '\\';
    snprintf(out, cap, "%s%s%s.sav", dir, slash ? "/" : "", stem);
}

static bool g_hle_boot;
static bool g_vi_raw; /* --vi-raw: scan out without the VI's filters */
static int g_internal_scale = 1; /* --internal-scale: 1, 2 or 4 */

static N64Machine *create_machine(const char *rom_path, const uint8_t *(*embedded)(size_t *), const char *save_dir,
                                  const char *exe_path)
{
    N64Machine *m = n64_create();
    if (!m) return NULL;
    size_t size = 0;
    bool ok;
    if (!rom_path) {
        const uint8_t *data = embedded ? embedded(&size) : NULL;
        ok = data && n64_load_rom(m, data, size);
        if (!ok) fprintf(stderr, "no ROM given and none is embedded in this program\n");
    } else {
        uint8_t *rom = read_file(rom_path, &size);
        ok = rom && n64_load_rom(m, rom, size);
        if (!ok) fprintf(stderr, "cannot load ROM %s (missing, or too small to be a cartridge)\n", rom_path);
        free(rom);
    }
    if (!ok) {
        n64_destroy(m);
        return NULL;
    }
    if (save_dir) {
        /* Read the save a game executable would use; never write it. */
        char path[4096];
        save_path_for(path, sizeof path, save_dir, rom_path ? rom_path : exe_path);
        size_t ssize = 0;
        uint8_t *save = read_file(path, &ssize);
        if (save) n64_save_import(m, save, ssize);
        free(save);
    }
    m->hle_boot = g_hle_boot;
    m->vi_raw = g_vi_raw;
    if (g_internal_scale > 1 && !n64_set_internal_scale(m, g_internal_scale))
        fprintf(stderr, "no memory for an internal scale of %d; the console's own picture\n", g_internal_scale);
    n64_power(m);
    return m;
}

static bool parse_gpr(const char *spec, int *reg, uint64_t *value)
{
    const char *eq = strchr(spec, '=');
    if (!eq || spec[0] != 'r') return false;
    *reg = atoi(spec + 1);
    if (*reg < 0 || *reg > 31) return false;
    *value = (uint64_t)strtoll(eq + 1, NULL, 0);
    if (eq[1] != '-') *value = strtoull(eq + 1, NULL, 0);
    return true;
}

static void print_cpu(const char *tag, const N64Cpu *c)
{
    printf("  %-11s pc=%016" PRIX64 " cycles=%" PRIu64 " instructions=%" PRIu64 " status=%08X cause=%08X fcr31=%08X"
           " delay=%u\n",
           tag, c->pc, c->cycles, c->instructions, c->cp0_status, c->cp0_cause, c->fcr31, c->delay_pending);
    for (int i = 0; i < 32; i += 4)
        printf("    r%-2d=%016" PRIX64 " r%-2d=%016" PRIX64 " r%-2d=%016" PRIX64 " r%-2d=%016" PRIX64 "\n", i, c->gpr[i],
               i + 1, c->gpr[i + 1], i + 2, c->gpr[i + 2], i + 3, c->gpr[i + 3]);
}

static bool same_cpu(const N64Cpu *a, const N64Cpu *b)
{
    return memcmp(a->gpr, b->gpr, sizeof a->gpr) == 0 && a->hi == b->hi && a->lo == b->lo && a->pc == b->pc &&
           memcmp(a->fpr, b->fpr, sizeof a->fpr) == 0 && a->fcr31 == b->fcr31 && a->cp0_status == b->cp0_status &&
           a->cp0_cause == b->cp0_cause && a->cycles == b->cycles && a->instructions == b->instructions &&
           a->delay_pending == b->delay_pending;
}

static int run_differential(N64Machine *a, N64Machine *b, uint64_t frames, const KeyPress *presses, int count)
{
    if (!b) return 2;
    uint64_t slices = 0, frame = 0;
    uint64_t last_pc = 0;
    apply_input(a, presses, count, 0);
    apply_input(b, presses, count, 0);
    while (frame < frames) {
        last_pc = a->cpu.pc;
        a->frame_ready = false;
        const uint64_t before = a->cpu.instructions;
        n64_run_slice(a, UINT64_MAX);
        slices++;
        /* The interpreter runs to the same instruction; a slice that retired
         * none took an interrupt, which the interpreter's next slice takes
         * the same way. */
        b->stop_instructions = a->cpu.instructions;
        if (a->cpu.instructions == before) n64_run_slice(b, UINT64_MAX);
        while (b->cpu.instructions < a->cpu.instructions) n64_run_slice(b, UINT64_MAX);
        b->stop_instructions = UINT64_MAX;
        /* Native code that leaves through a call or a return does not take
         * the events due at that sync point; the next slice's start does.
         * The interpreter took them before it stopped. Take them here too,
         * so the two are compared in the same state. */
        if (a->cpu.cycles >= a->next_event) n64_process_events(a);
        if (b->cpu.cycles >= b->next_event) n64_process_events(b);
        bool same = same_cpu(&a->cpu, &b->cpu);
        if (a->frame_ready) {
            frame++;
            n64_sync_rsp(a);
            n64_sync_rsp(b);
            same = same && memcmp(a->rdram, b->rdram, a->rdram_size) == 0 &&
                   memcmp(a->rsp.dmem, b->rsp.dmem, sizeof a->rsp.dmem) == 0 &&
                   memcmp(a->rsp.imem, b->rsp.imem, sizeof a->rsp.imem) == 0 &&
                   memcmp(a->rsp.r, b->rsp.r, sizeof a->rsp.r) == 0 && a->rsp.pc == b->rsp.pc;
            drain_audio(a, NULL);
            drain_audio(b, NULL);
            apply_input(a, presses, count, frame);
            apply_input(b, presses, count, frame);
        }
        if (!same) {
            printf("DIVERGED after slice %" PRIu64 " (frame %" PRIu64 "), slice entered at %016" PRIX64 "\n", slices,
                   frame, last_pc);
            print_cpu("recompiled", &a->cpu);
            print_cpu("interpreter", &b->cpu);
            n64_destroy(b);
            return 1;
        }
    }
    const N64Cpu *c = &a->cpu;
    const uint64_t ran = c->instructions - c->idled;
    printf("differential: %" PRIu64 " frames, %" PRIu64 " instructions (%.1f%% native), no divergence\nPASS\n", frame,
           c->instructions, ran ? 100.0 * (double)(ran - c->interpreted) / (double)ran : 0.0);
    n64_destroy(b);
    return 0;
}

bool n64_profile_rom(const uint8_t *rom, size_t size, uint64_t frames, const char *trace_path, uint64_t *instructions)
{
    N64Machine *m = n64_create();
    if (!m || !n64_load_rom(m, rom, size)) {
        n64_destroy(m);
        return false;
    }
    n64_power(m);
    m->audio_off = true;
    n64_coverage_enable(m);
    for (uint64_t frame = 0; frame < frames; frame++) {
        int8_t x, y;
        uint16_t b = n64_auto_input(frame, &x, &y);
        n64_set_buttons(m, 0, b, x, y);
        n64_run_frame(m);
    }
    bool ok = n64_coverage_write_trace(m, trace_path);
    if (instructions) *instructions = m->cpu.instructions;
    n64_destroy(m);
    return ok;
}

int n64_runner_main(int argc, char **argv, const struct N64RcProgram *program, const char *default_rom)
{
    const char *rom_path = NULL, *frame_path = NULL, *state_path = NULL, *audio_path = NULL, *trace_path = NULL;
    const char *save_dir = NULL, *expect_isv = NULL, *fail_isv = NULL, *fb_path = NULL, *save_out = NULL;
    const char *learn_path = NULL, *large_path = NULL;
    int fb_height = 0;
    uint64_t dump_every = 0;
    bool quiet = false, differential = false, coverage = false, show_isv = false, benchmark = false, perf = false;
    uint64_t max_frames = 600;
    KeyPress presses[64];
    int press_count = 0;
    int until_reg = -1, expect_count = 0;
    bool interpret = false;
    uint64_t until_value = 0;
    int expect_reg[16];
    uint64_t expect_value[16];

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--frames") && i + 1 < argc) max_frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--dump-frame") && i + 1 < argc) frame_path = argv[++i];
        else if (!strcmp(a, "--dump-state") && i + 1 < argc) state_path = argv[++i];
        else if (!strcmp(a, "--dump-fb") && i + 1 < argc) fb_path = argv[++i];
        else if (!strcmp(a, "--fb-height") && i + 1 < argc) fb_height = atoi(argv[++i]);
        else if (!strcmp(a, "--dump-every") && i + 1 < argc) dump_every = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--dump-audio") && i + 1 < argc) audio_path = argv[++i];
        else if (!strcmp(a, "--trace-out") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(a, "--save-dir") && i + 1 < argc) save_dir = argv[++i];
        else if (!strcmp(a, "--dump-save") && i + 1 < argc) save_out = argv[++i];
        else if (!strcmp(a, "--learn-out") && i + 1 < argc) learn_path = argv[++i];
        else if (!strcmp(a, "--quiet")) quiet = true;
        else if (!strcmp(a, "--hle-boot")) g_hle_boot = true;
        else if (!strcmp(a, "--vi-raw")) g_vi_raw = true;
        else if (!strcmp(a, "--dump-large") && i + 1 < argc) large_path = argv[++i];
        else if (!strcmp(a, "--internal-scale") && i + 1 < argc) {
            g_internal_scale = atoi(argv[++i]);
            if (g_internal_scale != 1 && g_internal_scale != 2 && g_internal_scale != 4) {
                fprintf(stderr, "--internal-scale takes 1, 2 or 4, not %s\n", argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--headless")) { /* selects this runner over the window; nothing else */ }
        else if (!strcmp(a, "--interpreter")) interpret = true;
        else if (!strcmp(a, "--differential")) differential = true;
        else if (!strcmp(a, "--coverage")) coverage = true;
        else if (!strcmp(a, "--auto-input")) g_auto_input = true;
        else if (!strcmp(a, "--benchmark")) benchmark = true;
        else if (!strcmp(a, "--perf")) perf = true;
        else if (!strcmp(a, "--isviewer")) show_isv = true;
        else if (!strcmp(a, "--trace-last") && i + 1 < argc) g_trace_len = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--expect-isviewer") && i + 1 < argc) expect_isv = argv[++i];
        else if (!strcmp(a, "--fail-isviewer") && i + 1 < argc) fail_isv = argv[++i];
        else if (!strcmp(a, "--until-gpr") && i + 1 < argc) {
            if (!parse_gpr(argv[++i], &until_reg, &until_value)) {
                fprintf(stderr, "bad --until-gpr %s (want rN=VALUE)\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--expect-gpr") && i + 1 < argc && expect_count < 16) {
            if (!parse_gpr(argv[++i], &expect_reg[expect_count], &expect_value[expect_count])) {
                fprintf(stderr, "bad --expect-gpr %s (want rN=VALUE)\n", argv[i]);
                return 2;
            }
            expect_count++;
        } else if (!strcmp(a, "--keys") && i + 1 < argc && press_count < 64) {
            if (!parse_keys(argv[++i], &presses[press_count++])) {
                fprintf(stderr, "bad --keys %s (want FRAME:BUTTON[+BUTTON...][:DURATION])\n", argv[i]);
                return 2;
            }
        } else if (a[0] != '-' && !rom_path) rom_path = a;
        else {
            fprintf(stderr, "unknown argument %s\n", a);
            return 2;
        }
    }
    if (!rom_path) rom_path = default_rom;
    /* --interpreter still runs the ROM compiled into this executable. */
    const uint8_t *(*embedded)(size_t *) = program ? program->embedded_rom : NULL;
    if (interpret) program = NULL;
    if (!rom_path && !embedded) {
        fprintf(stderr, "usage: n64_run ROM [--frames N] [--dump-frame FILE.ppm] [--dump-state FILE]\n"
                        "               [--dump-audio FILE.wav] [--keys FRAME:BUTTONS[:N]]... [--auto-input]\n"
                        "               [--until-gpr rN=VALUE] [--expect-gpr rN=VALUE]... [--isviewer]\n"
                        "               [--expect-isviewer STR] [--fail-isviewer STR] [--interpreter] [--quiet]\n");
        return 2;
    }

    N64Machine *m = create_machine(rom_path, embedded, save_dir, argv[0]);
    if (!m) return 2;
    if (program && !n64_rc_install(m, program)) {
        fprintf(stderr, "%s does not match the ROM this program was built from; running interpreted\n",
                rom_path ? rom_path : "(embedded)");
    }
    if (benchmark) {
        m->audio_off = true;
        m->video_off = true;
    }
    if (coverage || trace_path) n64_coverage_enable(m);
    /* --learn-out FILE: what a game's window does while it is played. The
     * interpreter notes the code it had to run, without slowing compiled
     * code, and the run is added to FILE (n64_coverage_merge_trace). */
    else if (learn_path) n64_coverage_enable_light(m);
    if (g_trace_len) {
        g_trace = calloc(g_trace_len, sizeof *g_trace);
        if (g_trace) m->trace = trace_hook;
    }
    if (differential) {
        if (!m->rc_program) {
            fprintf(stderr, "--differential needs a recompiled program\n");
            n64_destroy(m);
            return 2;
        }
        N64Machine *b = create_machine(rom_path, embedded, save_dir, argv[0]);
        int status = run_differential(m, b, max_frames, presses, press_count);
        n64_destroy(m);
        return status;
    }

    FILE *audio = NULL;
    if (audio_path) {
        audio = fopen(audio_path, "wb");
        if (!audio) fprintf(stderr, "cannot write %s\n", audio_path);
        else wav_header(audio, 0);
    }
    uint32_t audio_frames = 0;
    bool reached = false, failed_isv = false;
    uint64_t frame = 0;
    m->perf_on = perf;
    const uint64_t perf_start = n64_host_ticks(), rsp_start = m->rsp.instructions, cpu_start = m->cpu.instructions;
    for (; frame < max_frames; frame++) {
        apply_input(m, presses, press_count, frame);
        if (until_reg >= 0) {
            m->frame_ready = false;
            while (!m->frame_ready) {
                n64_run_slice(m, UINT64_MAX);
                if (m->cpu.gpr[until_reg] == until_value) break;
            }
            if (m->cpu.gpr[until_reg] == until_value) {
                reached = true;
                frame++;
                break;
            }
        } else {
            n64_run_frame(m);
        }
        audio_frames += drain_audio(m, audio);
        if (dump_every && frame_path && (frame + 1u) % dump_every == 0) {
            /* --dump-frame out.ppm --dump-every N also writes out_00000N.ppm, ... */
            char path[4096];
            const char *dot = strrchr(frame_path, '.');
            const int stem = dot ? (int)(dot - frame_path) : (int)strlen(frame_path);
            snprintf(path, sizeof path, "%.*s_%06" PRIu64 "%s", stem, frame_path, frame + 1u, dot ? dot : "");
            if (!n64_dump_ppm(m, path)) fprintf(stderr, "cannot write %s\n", path);
        }
        if (dump_every && large_path && (frame + 1u) % dump_every == 0) {
            char path[4096];
            const char *dot = strrchr(large_path, '.');
            const int stem = dot ? (int)(dot - large_path) : (int)strlen(large_path);
            snprintf(path, sizeof path, "%.*s_%06" PRIu64 "%s", stem, large_path, frame + 1u, dot ? dot : "");
            if (!dump_large_ppm(m, path)) fprintf(stderr, "cannot write %s\n", path);
        }
        if (fail_isv && m->isv_log && strstr(m->isv_log, fail_isv)) {
            failed_isv = true;
            frame++;
            break;
        }
        if (expect_isv && m->isv_log && strstr(m->isv_log, expect_isv)) {
            frame++;
            break;
        }
    }

    const N64Cpu *c = &m->cpu;
    if (!quiet) {
        printf("rom=%s title=\"%s\" id=%.4s cic=%u frames=%" PRIu64 " instructions=%" PRIu64 " cycles=%" PRIu64 "\n",
               rom_path ? rom_path : "(embedded)", m->cart.title, m->cart.id, m->cart.cic, frame, c->instructions,
               c->cycles);
        printf("pc=%016" PRIX64 " status=%08X cause=%08X epc=%016" PRIX64 " badvaddr=%016" PRIX64
               " rsp_pc=%03X rsp_status=%04X\n",
               c->pc, c->cp0_status, c->cp0_cause, c->cp0_epc, c->cp0_bad_vaddr, m->rsp.pc, m->rsp.status);
        printf("vi: ctrl=%08X origin=%06X width=%u v_intr=%u v_sync=%u h_sync=%08X h_video=%08X v_video=%08X"
               " x_scale=%08X y_scale=%08X\n",
               m->vi_regs[0], m->vi_regs[1], m->vi_regs[2], m->vi_regs[3], m->vi_regs[6], m->vi_regs[7], m->vi_regs[9],
               m->vi_regs[10], m->vi_regs[12], m->vi_regs[13]);
        if (m->rc_program && c->instructions > c->idled) {
            /* Of the instructions that ran: idle-loop turns taken at once
             * were neither compiled nor interpreted. */
            const uint64_t ran = c->instructions - c->idled, native = ran - c->interpreted;
            printf("native=%.1f%% (%" PRIu64 " of %" PRIu64 " instructions run; %" PRIu64 " more idled)\n",
                   100.0 * (double)native / (double)ran, native, ran, c->idled);
        }
    }
    if (perf && frame) {
        /* Where the host's time went. The RSP's figure excludes the RDP
         * commands it sent; the CPU's is what is left. */
        const double hz = (double)n64_host_ticks_per_second(), n = (double)frame;
        const double total = (double)(n64_host_ticks() - perf_start) / hz;
        const double rdp = (double)m->perf_rdp / hz, vi = (double)m->perf_vi / hz;
        const double rsp = (double)(m->perf_rsp - m->perf_rdp_in_rsp) / hz;
        const double cpu = total - rsp - rdp - vi;
        printf("perf: %.2f s for %" PRIu64 " frames = %.1f frames/s (%.2f ms a frame)\n", total, frame, n / total,
               1000.0 * total / n);
        printf("perf: cpu %.2f s (%.0f%%, %.0f instructions a frame, idle turns left out), rsp %.2f s (%.0f%%, %.0f"
               " instructions a frame), rdp %.2f s (%.0f%%), vi %.2f s (%.0f%%)\n",
               cpu, 100.0 * cpu / total, (double)(c->instructions - c->idled - cpu_start) / n, rsp, 100.0 * rsp / total,
               (double)(m->rsp.instructions - rsp_start) / n, rdp, 100.0 * rdp / total, vi, 100.0 * vi / total);
    }
    if (coverage) n64_coverage_report(m);
    if (trace_path && !n64_coverage_write_trace(m, trace_path)) fprintf(stderr, "cannot write %s\n", trace_path);
    if (learn_path && !coverage && !trace_path && !n64_coverage_merge_trace(m, learn_path))
        fprintf(stderr, "cannot write %s\n", learn_path);
    if (audio) {
        audio_frames += drain_audio(m, audio);
        wav_header(audio, audio_frames);
        fclose(audio);
    }
    if (frame_path && !n64_dump_ppm(m, frame_path)) fprintf(stderr, "cannot write %s\n", frame_path);
    if (large_path && !dump_large_ppm(m, large_path)) fprintf(stderr, "cannot write %s\n", large_path);
    if (fb_path && !dump_fb(m, fb_path, fb_height)) fprintf(stderr, "cannot write %s\n", fb_path);
    if (state_path && !n64_dump_state(m, state_path)) fprintf(stderr, "cannot write %s\n", state_path);
    if (save_out) {
        /* The battery memory as a game executable would save it, to a file
         * the caller names: the save in --save-dir is still never written. */
        const size_t size = n64_save_size(m);
        uint8_t *image = malloc(size ? size : 1u);
        FILE *out = image ? fopen(save_out, "wb") : NULL;
        if (out) {
            n64_save_export(m, image);
            if (fwrite(image, 1, size, out) != size) fprintf(stderr, "cannot write %s\n", save_out);
            fclose(out);
        } else {
            fprintf(stderr, "cannot write %s\n", save_out);
        }
        free(image);
    }
    if (show_isv) printf("[isviewer]\n%s\n[/isviewer]\n", m->isv_log ? m->isv_log : "");
    if (g_trace) {
        printf("[trace]\n");
        trace_print();
        free(g_trace);
        g_trace = NULL;
    }

    int status = 0;
    if (until_reg >= 0 && !reached) {
        printf("FAIL: r%d never became %016" PRIX64 " (r%d=%016" PRIX64 ")\n", until_reg, until_value, until_reg,
               c->gpr[until_reg]);
        status = 1;
    }
    for (int i = 0; i < expect_count; i++) {
        if (c->gpr[expect_reg[i]] != expect_value[i]) {
            printf("FAIL: r%d=%016" PRIX64 ", expected %016" PRIX64 "\n", expect_reg[i], c->gpr[expect_reg[i]],
                   expect_value[i]);
            status = 1;
        }
    }
    if (failed_isv) {
        printf("FAIL: the program printed \"%s\"\n", fail_isv);
        status = 1;
    }
    if (expect_isv && !(m->isv_log && strstr(m->isv_log, expect_isv))) {
        printf("FAIL: the program never printed \"%s\"\n", expect_isv);
        status = 1;
    }
    if (status == 0 && (until_reg >= 0 || expect_count || expect_isv || fail_isv)) printf("PASS\n");
    n64_destroy(m);
    return status;
}
