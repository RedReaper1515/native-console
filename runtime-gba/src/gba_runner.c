/*
 * Headless runner shared by gba_run (interpreter only) and by every
 * recompiled game executable, so test tooling drives both the same way.
 *
 *   <exe> [ROM] [--bios FILE] [--boot-bios] [--frames N] [--until-idle]
 *               [--expect rN=VALUE]... [--expect-text STR] [--screen-text]
 *               [--dump-frame FILE.ppm] [--keys FRAME:BUTTONS[:N]]...
 *               [--auto-input] [--hash-frames] [--trace-pc LO:HI] [--quiet]
 *               [--load-state FILE] [--texture-pack FILE.gbatp]
 *               [--record-textures FILE [--record-every N]]
 *               [--link-host | --link-join HOST[:PORT]] [--link-port N]
 *               [--link-local] [--link-wait PLAYERS]
 *
 * --link-host and --link-join plug in a link cable (gba_link.h) to other
 * processes over TCP (gba_net.h), as the windowed frontend does; the host
 * is player 1. --link-wait holds frame 0 until that many players are
 * linked (at most 60 s), so scripted inputs line up across processes.
 * Frames then wait for the other players as the lockstep requires.
 *
 * --load-state starts from a save state (gba_state.c) instead of reset.
 * --texture-pack makes --dump-frame write the frame composed with that pack,
 * at its scale. --record-textures appends the video state every N frames
 * (default 10) when it changed, for authoring packs (gba_texture_pack.h).
 *
 * --trace-pc prints every interpreted instruction whose address is in
 * [LO, HI) (hex) with the cycle count it started at and the cycles since the
 * previous one printed; with --interpreter it covers all code.
 *
 * --hash-frames prints one hash of every frame the run produced, so a
 * renderer change can be checked for identical output.
 *
 * --auto-input plays the built-in input script (gba_auto_input) that
 * gbarecomp --profile uses, instead of --keys. A cartridge clock (RTC)
 * starts at --rtc-time "YYYY-MM-DD HH:MM:SS" (default 2026-01-01 12:00:00)
 * and follows emulated time.
 *
 * --until-idle stops once the CPU sits on a branch-to-self (the "idle: b
 * idle" ending of the jsmolka and Alyosha tests) or halts with no interrupt
 * enabled. Reaching the frame limit first is a failure (fail closed). mGBA
 * debug-port output is printed as "[debug] ..." lines.
 *
 * --screen-text prints the text of the tile backgrounds, reading each map
 * entry's tile number as an ASCII code (the libgba console font the
 * NanoBoyAdvance hardware tests print with is laid out that way).
 * --expect-text passes when that text contains STR.
 *
 * When a recompiled program is linked in, --interpreter ignores it; the
 * summary reports the share of instructions that ran as native code.
 * --differential runs the recompiled program and the interpreter side by
 * side for --frames frames and stops at the first difference.
 */
#include "gba_runner.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba.h"
#include "gba_link.h"
#include "gba_net.h"
#include "gba_recomp.h"
#include "gba_texture_pack.h"

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

static bool g_quiet;

static void debug_log(GbaMachine *m, int level, const char *text)
{
    (void)m;
    (void)level;
    printf("[debug] %s\n", text);
    fflush(stdout);
}

static uint32_t g_trace_lo, g_trace_hi;

static void trace_step(GbaMachine *m)
{
    static uint64_t last;
    uint32_t pc = gba_cpu_pc(&m->cpu);
    if (pc < g_trace_lo || pc >= g_trace_hi) return;
    bool thumb = (m->cpu.cpsr & GBA_CPSR_T) != 0;
    printf("[trace] %08" PRIX32 " %0*" PRIX32 " cycle %" PRIu64 " (+%" PRIu64 ")\n", pc, thumb ? 4 : 8,
           m->cpu.pipe[0], m->cpu.cycles, last ? m->cpu.cycles - last : 0);
    last = m->cpu.cycles;
}

static bool at_idle_loop(GbaMachine *m)
{
    uint32_t pc = gba_cpu_pc(&m->cpu);
    if (m->halted && !(m->ie & m->if_) && (m->ie == 0 || !(m->ime & 1u))) return true;
    if (m->cpu.cpsr & GBA_CPSR_T) return (gba_peek32(m, pc) >> ((pc & 2u) * 8u) & 0xFFFFu) == 0xE7FEu;
    return gba_peek32(m, pc) == 0xEAFFFFFEu;
}

/* FNV-1a over the framebuffer, chained from frame to frame. */
static uint64_t hash_frame(const GbaMachine *m, uint64_t hash)
{
    for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) {
        hash = (hash ^ m->framebuffer[i]) * 0x100000001B3ull;
    }
    return hash;
}

static bool dump_ppm(const GbaMachine *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);
    for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) {
        uint32_t p = m->framebuffer[i];
        uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

/* --dump-large: the larger picture (gba_ppu_large_frame), or the console's
 * own when the last frame has none. */
static bool dump_large_ppm(const GbaMachine *m, const char *path)
{
    int w = 0, h = 0;
    const uint32_t *frame = gba_ppu_large_frame(m, &w, &h);
    if (!frame) return dump_ppm(m, path);
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

/* --dump-frame with a texture pack: the composed frame at the pack's scale. */
static bool dump_composed_ppm(const char *path)
{
    uint32_t s = gba_texture_pack_scale();
    uint32_t w = GBA_SCREEN_WIDTH * s, h = GBA_SCREEN_HEIGHT * s;
    uint32_t *px = malloc((size_t)w * h * sizeof *px);
    FILE *f = px ? fopen(path, "wb") : NULL;
    bool ok = f && gba_texture_pack_compose(px);
    if (ok) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
            fwrite(rgb, 1, 3, f);
        }
    }
    if (f) fclose(f);
    free(px);
    return ok;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* --dump-audio: the APU output as a 16-bit stereo WAV at GBA_APU_RATE. The
 * header is written with zero sizes and patched when the file is closed.
 * The fields are stored one by one: MSVC 19.51 /O2 miscompiled a loop of
 * "field >> (8 * i)" stores here (its AVX2 path shifted one lane with
 * psrld instead of vpsrlvd, zeroing the data size). */
static void wav_header(FILE *f, uint32_t frames)
{
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
    put_le32(h + 4, 36u + frames * 4u);
    put_le32(h + 24, GBA_APU_RATE);
    put_le32(h + 28, GBA_APU_RATE * 4u);
    put_le32(h + 40, frames * 4u);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

static FILE *open_wav(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) fprintf(stderr, "cannot write %s\n", path);
    else wav_header(f, 0);
    return f;
}

static uint32_t drain_audio(GbaMachine *m, FILE *f)
{
    static int16_t buf[4096 * 2];
    uint32_t total = 0;
    size_t n;
    while ((n = gba_apu_read_samples(m, buf, 4096)) > 0) {
        if (f) fwrite(buf, 4, n, f);
        total += (uint32_t)n;
    }
    return total;
}

/* Text of the enabled tile backgrounds, 30x20 characters per BG. */
static void screen_text(GbaMachine *m, char *out, size_t cap)
{
    size_t len = 0;
    uint16_t dispcnt = (uint16_t)(m->io[0] | (m->io[1] << 8));
    out[0] = '\0';
    if ((dispcnt & 7u) > 2) return;
    for (int bg = 0; bg < 4; bg++) {
        if (!(dispcnt & (0x100u << bg))) continue;
        uint16_t cnt = (uint16_t)(m->io[8 + 2 * bg] | (m->io[9 + 2 * bg] << 8));
        uint32_t base = ((cnt >> 8) & 31u) * 0x800u;
        for (int y = 0; y < 20; y++) {
            char line[31];
            int end = 0;
            for (int x = 0; x < 30; x++) {
                uint32_t off = base + (uint32_t)(y * 32 + x) * 2u;
                uint32_t tile = (m->vram[off] | (m->vram[off + 1] << 8)) & 0x3FFu;
                char ch = (tile >= 0x20 && tile < 0x7F) ? (char)tile : ' ';
                line[x] = ch;
                if (ch != ' ') end = x + 1;
            }
            line[end] = '\0';
            if (end && len + (size_t)end + 2 < cap) len += (size_t)snprintf(out + len, cap - len, "%s\n", line);
        }
    }
}

typedef struct {
    int reg;
    uint32_t value;
} Expectation;

/* --keys FRAME:BUTTONS[:DURATION]: hold BUTTONS (A+B+START...) from FRAME
 * for DURATION frames (default 6). */
typedef struct {
    uint64_t frame, duration;
    uint16_t keys;
} KeyPress;

static bool parse_keys(const char *spec, KeyPress *out)
{
    static const struct { const char *name; uint16_t bit; } names[] = {
        { "A", GBA_KEY_A }, { "B", GBA_KEY_B }, { "SELECT", GBA_KEY_SELECT }, { "START", GBA_KEY_START },
        { "RIGHT", GBA_KEY_RIGHT }, { "LEFT", GBA_KEY_LEFT }, { "UP", GBA_KEY_UP }, { "DOWN", GBA_KEY_DOWN },
        { "R", GBA_KEY_R }, { "L", GBA_KEY_L },
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

/* --auto-input uses gba_auto_input instead of (and ignores) --keys. */
static bool g_auto_input;

static uint16_t keys_at(const KeyPress *presses, int count, uint64_t frame)
{
    if (g_auto_input) return gba_auto_input(frame);
    uint16_t keys = 0;
    for (int i = 0; i < count; i++) {
        if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].duration) keys |= presses[i].keys;
    }
    return keys;
}

/* One press every 30 frames from frame 300: START then three A presses
 * (title screens, file select, name entry, dialogue) for 80 presses, then a
 * cycle of movement and the other buttons. START comes in pairs so a pause
 * menu it opens is closed again. Directions are held for most of a slot. */
uint16_t gba_auto_input(uint64_t frame)
{
    static const uint16_t play[] = {
        GBA_KEY_A, GBA_KEY_RIGHT, GBA_KEY_A, GBA_KEY_DOWN, GBA_KEY_B, GBA_KEY_LEFT, GBA_KEY_A, GBA_KEY_UP,
        GBA_KEY_R, GBA_KEY_RIGHT, GBA_KEY_B, GBA_KEY_DOWN, GBA_KEY_L, GBA_KEY_LEFT, GBA_KEY_A, GBA_KEY_UP,
        GBA_KEY_START, GBA_KEY_A, GBA_KEY_START, GBA_KEY_B,
    };
    const uint64_t first = 300, slot_frames = 30, menu_slots = 80;
    if (frame < first) return 0;
    uint64_t slot = (frame - first) / slot_frames, at = (frame - first) % slot_frames;
    if (slot < menu_slots) return at < 6 ? (slot % 4 == 0 ? GBA_KEY_START : GBA_KEY_A) : 0;
    uint16_t key = play[(slot - menu_slots) % (sizeof play / sizeof play[0])];
    bool direction = key & (GBA_KEY_RIGHT | GBA_KEY_LEFT | GBA_KEY_UP | GBA_KEY_DOWN);
    return at < (direction ? 24u : 6u) ? key : 0;
}

bool gba_profile_rom(const uint8_t *rom, size_t size, uint64_t frames, const char *trace_path,
                     uint64_t *instructions)
{
    GbaMachine *m = gba_create();
    if (!gba_load_rom(m, rom, size)) {
        gba_destroy(m);
        return false;
    }
    gba_reset(m, true);
    gba_coverage_enable(m);
    for (uint64_t frame = 0; frame < frames; frame++) {
        gba_set_keys(m, gba_auto_input(frame));
        gba_run_frame(m);
    }
    bool ok = gba_coverage_write_trace(m, trace_path);
    if (instructions) *instructions = m->instructions;
    gba_destroy(m);
    return ok;
}

/* --rtc-time: where the cartridge clock starts (it then follows emulated
 * time, so headless runs are reproducible). */
static bool g_rtc_start_set;
static int64_t g_rtc_start;

/* rom_path NULL: use the ROM compiled into the program. */
static GbaMachine *create_machine(const char *rom_path, const char *bios_path, bool boot_bios,
                                  const GbaRcProgram *program)
{
    GbaMachine *m = gba_create();
    size_t size = 0;
    if (!rom_path) {
        const uint8_t *embedded = program && program->embedded_rom ? program->embedded_rom(&size) : NULL;
        if (!embedded || !gba_load_rom(m, embedded, size)) {
            fprintf(stderr, "no ROM given and none is embedded in this program\n");
            gba_destroy(m);
            return NULL;
        }
    } else {
        uint8_t *rom = read_file(rom_path, &size);
        if (!rom || !gba_load_rom(m, rom, size)) {
            fprintf(stderr, "cannot load ROM %s\n", rom_path);
            free(rom);
            gba_destroy(m);
            return NULL;
        }
        free(rom);
    }
    if (bios_path) {
        uint8_t *bios = read_file(bios_path, &size);
        if (!bios || !gba_load_bios(m, bios, size)) {
            fprintf(stderr, "cannot load BIOS %s\n", bios_path);
            free(bios);
            gba_destroy(m);
            return NULL;
        }
        free(bios);
    }
    gba_reset(m, !boot_bios);
    if (g_rtc_start_set) m->rtc.start = g_rtc_start;
    return m;
}

static void print_state(const char *tag, const GbaMachine *m)
{
    const GbaCpu *c = &m->cpu;
    printf("  %-11s pc=%08" PRIX32 " cpsr=%08" PRIX32 " cycles=%" PRIu64 " instructions=%" PRIu64 "\n", tag,
           gba_cpu_pc(c), gba_cpu_get_cpsr(c), c->cycles, m->instructions);
    for (int r = 0; r < 16; r++) printf("%s%08" PRIX32 "%s", r % 8 == 0 ? "    " : " ", c->r[r], r % 8 == 7 ? "\n" : "");
}

/* Recompiled machine `a` against interpreter `b`, in lockstep: after every
 * run-loop slice of `a`, `b` catches up to the same instruction count and
 * the CPU state and cycle counter must match exactly. Memory is compared at
 * the end of every frame. Differential mode is not a hardware oracle: both
 * machines share the same devices; it proves the recompiled code executes
 * the program exactly as the interpreter does. */
static int run_differential(GbaMachine *a, GbaMachine *b, uint64_t frames, const KeyPress *presses, int press_count)
{
    if (!b) return 2;
    if (press_count || g_auto_input) {
        gba_set_keys(a, keys_at(presses, press_count, 0));
        gba_set_keys(b, keys_at(presses, press_count, 0));
    }
    uint64_t slices = 0, frame = 0;
    uint32_t last_pc = 0;
    while (frame < frames) {
        last_pc = gba_cpu_pc(&a->cpu);
        a->frame_ready = false;
        gba_run_slice(a, UINT64_MAX);
        slices++;
        while (b->instructions < a->instructions) gba_run_slice(b, UINT64_MAX);
        while (b->instructions == a->instructions && b->cpu.cycles < a->cpu.cycles && (b->halted || b->stopped)) {
            gba_run_slice(b, a->cpu.cycles);
        }
        bool same = b->instructions == a->instructions && b->cpu.cycles == a->cpu.cycles &&
                    gba_cpu_get_cpsr(&a->cpu) == gba_cpu_get_cpsr(&b->cpu) &&
                    memcmp(a->cpu.r, b->cpu.r, sizeof a->cpu.r) == 0;
        if (a->frame_ready) {
            frame++;
            same = same && memcmp(a->iwram, b->iwram, sizeof a->iwram) == 0 &&
                   memcmp(a->ewram, b->ewram, sizeof a->ewram) == 0 &&
                   memcmp(a->vram, b->vram, sizeof a->vram) == 0 &&
                   memcmp(a->io, b->io, sizeof a->io) == 0;
            /* Both machines are at the same instruction here: change input. */
            if (press_count || g_auto_input) {
                gba_set_keys(a, keys_at(presses, press_count, frame));
                gba_set_keys(b, keys_at(presses, press_count, frame));
            }
        }
        if (!same) {
            printf("DIVERGED after slice %" PRIu64 " (frame %" PRIu64 "), slice entered at %08" PRIX32 "\n",
                   slices, frame, last_pc);
            print_state("recompiled", a);
            print_state("interpreter", b);
            gba_destroy(b);
            return 1;
        }
    }
    printf("differential: %" PRIu64 " frames, %" PRIu64 " instructions (%.1f%% native), no divergence\nPASS\n",
           frame, a->instructions,
           a->instructions ? 100.0 * (double)(a->instructions - a->interpreted_instructions) / (double)a->instructions : 0.0);
    gba_destroy(b);
    return 0;
}

/* A frame, waiting for the other players when the link cable stalls it. */
static void run_linked_frame(GbaMachine *m)
{
    gba_run_frame(m);
    while (!m->frame_ready && m->link) {
        if (!gba_link_wait(m->link, 30000)) {
            fprintf(stderr, "link: no word from the other players for 30 s; unplugging\n");
            gba_link_detach(m);
        }
        gba_run_frame(m);
    }
    if (m->link) gba_link_frame_done(m->link);
}

int gba_runner_main(int argc, char **argv, const GbaRcProgram *program, const char *default_rom)
{
    const char *rom_path = NULL, *bios_path = NULL, *frame_path = NULL, *expect_text = NULL, *trace_path = NULL;
    const char *large_path = NULL;
    int affine_scale = 1;
    const char *audio_path = NULL, *state_path = NULL, *pack_path = NULL, *record_path = NULL;
    uint64_t record_every = 10;
    bool coverage = false, hash_frames = false;
    uint64_t frame_hash = 0xCBF29CE484222325ull;
    bool boot_bios = false, until_idle = false, show_text = false, interpreter_only = false, differential = false;
    uint64_t max_frames = 600;
    const char *link_join = NULL;
    bool link_host = false, link_local = false;
    int link_port = GBA_NET_DEFAULT_PORT, link_wait = 0;
    Expectation expect[16];
    KeyPress presses[512];
    int press_count = 0;
    int expect_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--bios") && i + 1 < argc) bios_path = argv[++i];
        else if (!strcmp(a, "--boot-bios")) boot_bios = true;
        else if (!strcmp(a, "--frames") && i + 1 < argc) max_frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--until-idle")) until_idle = true;
        else if (!strcmp(a, "--dump-frame") && i + 1 < argc) frame_path = argv[++i];
        else if (!strcmp(a, "--dump-large") && i + 1 < argc) large_path = argv[++i];
        else if (!strcmp(a, "--affine-scale") && i + 1 < argc) affine_scale = atoi(argv[++i]);
        else if (!strcmp(a, "--dump-audio") && i + 1 < argc) audio_path = argv[++i];
        else if (!strcmp(a, "--quiet")) g_quiet = true;
        else if (!strcmp(a, "--load-state") && i + 1 < argc) state_path = argv[++i];
        else if (!strcmp(a, "--texture-pack") && i + 1 < argc) pack_path = argv[++i];
        else if (!strcmp(a, "--record-textures") && i + 1 < argc) record_path = argv[++i];
        else if (!strcmp(a, "--record-every") && i + 1 < argc) record_every = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--screen-text")) show_text = true;
        else if (!strcmp(a, "--interpreter")) interpreter_only = true;
        else if (!strcmp(a, "--differential")) differential = true;
        else if (!strcmp(a, "--coverage")) coverage = true;
        else if (!strcmp(a, "--hash-frames")) hash_frames = true;
        else if (!strcmp(a, "--trace-pc") && i + 1 < argc) {
            char *end = NULL;
            g_trace_lo = (uint32_t)strtoul(argv[++i], &end, 16);
            g_trace_hi = (end && *end == ':') ? (uint32_t)strtoul(end + 1, NULL, 16) : 0;
            if (g_trace_hi <= g_trace_lo) {
                fprintf(stderr, "bad --trace-pc %s (want LO:HI in hex)\n", argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--auto-input")) g_auto_input = true;
        else if (!strcmp(a, "--link-host")) link_host = true;
        else if (!strcmp(a, "--link-join") && i + 1 < argc) link_join = argv[++i];
        else if (!strcmp(a, "--link-port") && i + 1 < argc) link_port = atoi(argv[++i]);
        else if (!strcmp(a, "--link-local")) link_local = true;
        else if (!strcmp(a, "--link-wait") && i + 1 < argc) link_wait = atoi(argv[++i]);
        else if (!strcmp(a, "--rtc-time") && i + 1 < argc) {
            if (!gba_rtc_parse_time(argv[++i], &g_rtc_start)) {
                fprintf(stderr, "bad --rtc-time %s (want YYYY-MM-DD[ HH:MM[:SS]], 2000-2099)\n", argv[i]);
                return 2;
            }
            g_rtc_start_set = true;
        }
        else if (!strcmp(a, "--trace-out") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(a, "--keys") && i + 1 < argc && press_count < 512) {
            if (!parse_keys(argv[++i], &presses[press_count++])) {
                fprintf(stderr, "bad --keys %s (want FRAME:BUTTON[+BUTTON...][:DURATION])\n", argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--expect-text") && i + 1 < argc) expect_text = argv[++i];
        else if (!strcmp(a, "--expect") && i + 1 < argc && expect_count < 16) {
            const char *e = argv[++i];
            int reg;
            char *eq = strchr(e, '=');
            if ((e[0] != 'r' && e[0] != 'R') || !eq) {
                fprintf(stderr, "bad --expect %s (want rN=VALUE)\n", e);
                return 2;
            }
            reg = atoi(e + 1);
            expect[expect_count].reg = reg;
            expect[expect_count].value = (uint32_t)strtoul(eq + 1, NULL, 0);
            expect_count++;
        } else if (a[0] != '-' && !rom_path) rom_path = a;
        else {
            fprintf(stderr, "unknown argument %s\n", a);
            return 2;
        }
    }
    if (!rom_path) rom_path = default_rom;
    bool embedded = !rom_path && program && program->embedded_rom;
    if (!rom_path && !embedded) {
        fprintf(stderr, "usage: gba_run ROM [--bios FILE] [--boot-bios] [--frames N] [--until-idle]\n"
                        "               [--expect rN=VALUE]... [--expect-text STR] [--screen-text]\n"
                        "               [--dump-frame FILE.ppm] [--interpreter] [--coverage]\n"
                        "               [--trace-out FILE] [--differential] [--keys FRAME:BUTTONS[:N]]...\n"
                        "               [--auto-input] [--hash-frames] [--quiet]\n"
                        "               [--load-state FILE] [--texture-pack FILE.gbatp]\n"
                        "               [--record-textures FILE [--record-every N]]\n");
        return 2;
    }

    if (boot_bios && !bios_path) {
        fprintf(stderr, "--boot-bios needs --bios\n");
        return 2;
    }
    GbaMachine *m = create_machine(rom_path, bios_path, boot_bios, program);
    if (m && affine_scale != 1 && (affine_scale < 1 || !gba_ppu_set_affine_scale(m, affine_scale))) {
        fprintf(stderr, "--affine-scale wants 1 to %d\n", GBA_AFFINE_SCALE_MAX);
        gba_destroy(m);
        return 2;
    }
    if (!m) return 2;
    if (!rom_path) rom_path = "(embedded)";
    m->debug_log = debug_log;
    if (g_trace_hi) m->step_hook = trace_step;
    if (program && !interpreter_only && !gba_rc_install(m, program)) {
        fprintf(stderr, "%s does not match the ROM this program was built from; running interpreted\n", rom_path);
    }
    if (coverage || trace_path) gba_coverage_enable(m);
    if (state_path && !gba_state_load_file(m, state_path)) {
        fprintf(stderr, "cannot load state %s\n", state_path);
        return 2;
    }
    if (pack_path) {
        GbaTexturePackStatus st = gba_texture_pack_load_file(m, pack_path);
        if (st != GBA_TEXTURE_PACK_OK) {
            fprintf(stderr, "texture pack %s: %s\n", pack_path, gba_texture_pack_status_string(st));
            return 2;
        }
    }
    FILE *record = record_path ? fopen(record_path, "ab") : NULL;
    uint64_t record_hash = 0;
    if (record_path && !record) {
        fprintf(stderr, "cannot write %s\n", record_path);
        return 2;
    }
    if (record_every < 1) record_every = 1;
    if (differential) {
        if (!m->rc_program) {
            fprintf(stderr, "--differential needs a recompiled program\n");
            return 2;
        }
        int status = run_differential(m, create_machine(embedded ? NULL : rom_path, bios_path, boot_bios, program),
                                      max_frames, presses, press_count);
        gba_destroy(m);
        return status;
    }

    GbaNet *net = NULL;
    if (link_host || link_join) {
        char game[64], err[200];
        gba_link_game_id(m, game, sizeof game);
        net = link_host ? gba_net_host((uint16_t)link_port, link_local, game, err, sizeof err) : gba_net_join(link_join, game);
        if (!net) {
            fprintf(stderr, "link: %s\n", err);
            return 2;
        }
        gba_link_attach(m, gba_net_io(net), gba_net_initial_player(net));
        if (link_wait > 1) {
            GbaLinkStatus ls;
            gba_link_status(m->link, &ls);
            for (int ms = 0; ms < 60000 && (ls.players < link_wait || (ls.self > 0 && !gba_link_follows(m->link)));
                 ms += 10) {
                gba_link_idle(m->link, 10);
                gba_link_status(m->link, &ls);
            }
            if (!g_quiet) printf("link: %s\n", gba_net_status(net));
            if (ls.players < link_wait) {
                fprintf(stderr, "link: only %d of %d players after 60 s\n", ls.players, link_wait);
                gba_link_detach(m);
                gba_net_close(net);
                return 2;
            }
        }
    }

    FILE *audio = audio_path ? open_wav(audio_path) : NULL;
    uint32_t audio_frames = 0;

    bool idle = false;
    uint64_t frame = 0;
    for (; frame < max_frames; frame++) {
        if (press_count || g_auto_input) gba_set_keys(m, keys_at(presses, press_count, frame));
        run_linked_frame(m);
        if (record && m->frame_count % record_every == 0) gba_texture_capture(m, record, &record_hash);
        /* The ring holds about a second; drain it every frame. */
        audio_frames += drain_audio(m, audio);
        if (hash_frames) frame_hash = hash_frame(m, frame_hash);
        if (until_idle && at_idle_loop(m)) {
            idle = true;
            /* One more frame so the result screen is drawn. */
            run_linked_frame(m);
            frame++;
            break;
        }
    }

    const GbaCpu *c = &m->cpu;
    if (!g_quiet) {
        printf("rom=%s frames=%" PRIu64 " instructions=%" PRIu64 " cycles=%" PRIu64 "\n", rom_path, frame,
               m->instructions, c->cycles);
        if (m->rc_program && m->instructions) {
            uint64_t native = m->instructions - m->interpreted_instructions;
            printf("native=%.1f%% (%" PRIu64 " of %" PRIu64 " instructions)\n",
                   100.0 * (double)native / (double)m->instructions, native, m->instructions);
        }
        printf("pc=%08" PRIX32 " cpsr=%08" PRIX32 " %s\n", gba_cpu_pc(c), gba_cpu_get_cpsr(c),
               (c->cpsr & GBA_CPSR_T) ? "thumb" : "arm");
        for (int r = 0; r < 16; r++) {
            printf("r%-2d=%08" PRIX32 "%s", r, c->r[r], (r % 4 == 3) ? "\n" : "  ");
        }
    }
    if (hash_frames) printf("frame_hash=%016" PRIX64 "\n", frame_hash);
    if (m->link) {
        GbaLinkStatus ls;
        gba_link_status(m->link, &ls);
        if (!g_quiet) printf("link: player %d of %d, %" PRIu64 " transfers\n", ls.self + 1, ls.players, ls.transfers);
        gba_link_detach(m);
    }
    gba_net_close(net);
    if (audio) {
        audio_frames += drain_audio(m, audio);
        wav_header(audio, audio_frames);
        fclose(audio);
    }
    if (record) fclose(record);
    if (frame_path && !(pack_path ? dump_composed_ppm(frame_path) : dump_ppm(m, frame_path)))
        fprintf(stderr, "cannot write %s\n", frame_path);
    if (large_path && !dump_large_ppm(m, large_path)) fprintf(stderr, "cannot write %s\n", large_path);
    if (pack_path && !g_quiet) {
        GbaTexturePackStats ts = gba_texture_pack_stats();
        printf("texture pack: %u tiles matched, %u recoloured, %u without an image (last frame)\n",
               ts.tiles_matched, ts.tiles_recoloured, ts.tiles_missing);
    }
    if (coverage) gba_coverage_report(m);
    if (audio) gba_apu_report(m);
    if (trace_path && !gba_coverage_write_trace(m, trace_path)) fprintf(stderr, "cannot write %s\n", trace_path);

    static char text[20 * 4 * 32 + 1];
    screen_text(m, text, sizeof text);
    if (show_text) printf("[screen]\n%s[/screen]\n", text);

    int status = 0;
    if (expect_text && !strstr(text, expect_text)) {
        printf("FAIL: screen does not contain \"%s\"\n", expect_text);
        status = 1;
    }
    if (until_idle && !idle) {
        printf("FAIL: not idle after %" PRIu64 " frames\n", frame);
        status = 1;
    }
    for (int i = 0; i < expect_count; i++) {
        uint32_t got = c->r[expect[i].reg & 15];
        if (got != expect[i].value) {
            printf("FAIL: r%d=%08" PRIX32 ", expected %08" PRIX32 "\n", expect[i].reg, got, expect[i].value);
            status = 1;
        }
    }
    if (status == 0 && (until_idle || expect_count || expect_text)) printf("PASS\n");
    gba_destroy(m);
    return status;
}
