/*
 * Headless runner shared by nes_run (interpreter only) and by every
 * recompiled game executable, so test tooling drives both the same way.
 *
 *   <exe> [ROM] [--frames N] [--until-idle] [--blargg] [--pc ADDR]
 *               [--expect-ram ADDR=VALUE]... [--expect-text STR]...
 *               [--expect-text-any STR]... [--fail-text STR] [--screen-text]
 *               [--smoke] [--dump-frame FILE.ppm] [--dump-state FILE]
 *               [--dump-audio FILE.wav] [--keys FRAME:BUTTONS[:N]]...
 *               [--auto-input] [--interpreter] [--coverage]
 *               [--trace-out FILE] [--cpu-log FILE] [--trace-last N]
 *               [--differential] [--quiet]
 *               [--save-state FILE --save-state-frame N] [--load-state FILE]
 *
 * When a recompiled program is linked in, --interpreter ignores it; the
 * summary reports the share of instructions that ran as native code.
 * --differential runs the recompiled program and the interpreter side by
 * side for --frames frames and stops at the first difference: CPU
 * registers and the cycle counter after every native slice; RAM, PRG RAM,
 * CHR RAM, nametables, palette, OAM and the cartridge's registers at the
 * end of every frame. --coverage prints where execution fell back to the
 * interpreter; --trace-out writes the entry points it saw for nesrecomp
 * --trace.
 *
 * Headless runs are deterministic: no host clock, fixed power-on memory,
 * scripted input, and battery RAM stays in memory (nothing is written next
 * to the executable).
 *
 * --blargg follows the result protocol of blargg's test ROMs: $6001-$6003
 * hold DE B0 61 once the test is running, $6000 is $80 while it runs, $81
 * when it wants the reset button pressed (done here, a tenth of a second
 * later), and the final result otherwise: 0 passes. The text at $6004 is
 * printed. Reaching the frame limit first, or never seeing the signature,
 * is a failure (fail closed).
 *
 * --until-idle stops once the program has ended: the CPU sits on a
 * jump-to-self (JMP *) with NMI off and interrupts masked, has jammed, or
 * has settled in a loop that changes
 * nothing (the registers, the stack pointer, work RAM and PRG RAM are the
 * same at the end of 120 frames running and the program counter stayed
 * within sixteen bytes). Two seconds, because a test that lines itself up
 * with the PPU waits in a delay loop one dot longer than a frame, which
 * looks the same at the end of every frame until the dots add up to an
 * instruction. Reaching the frame limit first is a failure. It only says
 * the program stopped: the verdict is what --expect-ram, --expect-text and
 * --fail-text read afterwards, so a program that stops early fails them.
 *
 * --pc ADDR starts at ADDR instead of the reset vector (nestest's
 * automation entry is $C000). --cpu-log writes one line per interpreted
 * instruction (PC, registers, cycle count) for tools/nes/nestest_compare.py.
 *
 * --save-state writes a save state (nes_state.h) once N frames have run;
 * --load-state starts from one, and the frame count carries on from the
 * state's, so --frames and --keys mean the same frames as in the run that
 * saved it. Together they check that a state resumes exactly: a run that
 * saves, and a run that loads and goes on, must end in the same
 * --dump-state as one straight run.
 *
 * --screen-text prints the first nametable, reading each tile number as an
 * ASCII code. Every --expect-text STR must be in that text at the end, and
 * at least one of the --expect-text-any strings if any is given (a test
 * that prints one of several checksums a console can produce).
 * --expect-ram checks a byte of CPU memory (work RAM or cartridge space).
 *
 * --smoke is the verdict for programs that report nothing (games, demos):
 * after --frames frames the CPU must not have jammed and the picture must
 * show at least three colours.
 */
#include "nes_runner.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes.h"
#include "nes_recomp.h"
#include "nes_state.h"

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

bool nes_dump_ppm(const NesMachine *m, const char *path)
{
    static uint32_t rgb[NES_WIDTH * NES_HEIGHT];
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    nes_frame_rgb(m, rgb);
    fprintf(f, "P6\n%d %d\n255\n", NES_WIDTH, NES_HEIGHT);
    for (int i = 0; i < NES_WIDTH * NES_HEIGHT; i++) {
        uint8_t px[3] = { (uint8_t)(rgb[i] >> 16), (uint8_t)(rgb[i] >> 8), (uint8_t)rgb[i] };
        fwrite(px, 1, 3, f);
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

/* The cartridge's registers and mapping, field by field (the struct itself
 * has padding and host byte order). */
static uint32_t cart_bytes(const NesCart *k, uint8_t *out)
{
    uint32_t n = 0;
    for (int i = 0; i < 4; i++, n += 4) put_le32(out + n, k->prg_page[i]);
    for (int i = 0; i < 8; i++, n += 4) put_le32(out + n, k->chr_page[i]);
    for (int i = 0; i < 4; i++, n += 4) put_le32(out + n, k->nt_page[i]);
    put_le32(out + n, k->prg_ram_page);
    n += 4;
    memcpy(out + n, k->reg, sizeof k->reg);
    n += (uint32_t)sizeof k->reg;
    const uint8_t rest[] = { k->prg_ram_enable, k->prg_ram_protect, k->chr_writable, k->irq, k->shift,
                             k->shift_count, k->bank_select, k->irq_latch, k->irq_counter, k->irq_reload,
                             k->irq_enable, k->a12, k->latch[0], k->latch[1] };
    memcpy(out + n, rest, sizeof rest);
    return n + (uint32_t)sizeof rest;
}

bool nes_dump_state(NesMachine *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    nes_sync(m);
    const NesCpu *c = &m->cpu;
    uint8_t regs[24];
    memset(regs, 0, sizeof regs);
    regs[0] = c->a;
    regs[1] = c->x;
    regs[2] = c->y;
    regs[3] = c->s;
    regs[4] = (uint8_t)c->pc;
    regs[5] = (uint8_t)(c->pc >> 8);
    regs[6] = nes_cpu_get_p(c);
    regs[7] = c->jammed;
    put_le32(regs + 8, (uint32_t)c->cycles);
    put_le32(regs + 12, (uint32_t)(c->cycles >> 32));
    put_le32(regs + 16, (uint32_t)m->frame_count);
    put_le32(regs + 20, (uint32_t)m->ppu_dots);
    dump_block(f, "CPU ", regs, sizeof regs);
    dump_block(f, "RAM ", m->ram, NES_RAM_SIZE);
    dump_block(f, "PRAM", m->prg_ram, m->info.prg_ram_size);
    dump_block(f, "CRAM", m->chr_ram, m->info.chr_ram_size);
    dump_block(f, "NTRM", m->nt_ram, NES_NT_RAM_SIZE);
    dump_block(f, "PAL ", m->ppu.palette, sizeof m->ppu.palette);
    dump_block(f, "OAM ", m->ppu.oam, sizeof m->ppu.oam);
    uint8_t cart[160];
    dump_block(f, "CART", cart, cart_bytes(&m->cart, cart));
    fclose(f);
    return true;
}

void nes_screen_text(NesMachine *m, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    const uint8_t *nt = &m->nt_ram[m->cart.nt_page[0]];
    for (int y = 0; y < 30; y++) {
        char line[33];
        int end = 0;
        for (int x = 0; x < 32; x++) {
            uint8_t tile = nt[y * 32 + x];
            char ch = (tile >= 0x20 && tile < 0x7F) ? (char)tile : ' ';
            line[x] = ch;
            if (ch != ' ') end = x + 1;
        }
        line[end] = '\0';
        if (end && len + (size_t)end + 2 < cap) len += (size_t)snprintf(out + len, cap - len, "%s\n", line);
    }
}

/* --keys FRAME:BUTTONS[:DURATION]: hold BUTTONS (A+B+START...) from FRAME
 * for DURATION frames (default 6). */
typedef struct {
    uint64_t frame, duration;
    uint8_t keys;
} KeyPress;

#define MAX_KEY_PRESSES 2048

static bool parse_keys(const char *spec, KeyPress *out)
{
    static const struct { const char *name; uint8_t bit; } names[] = {
        { "A", NES_BTN_A }, { "B", NES_BTN_B }, { "SELECT", NES_BTN_SELECT }, { "START", NES_BTN_START },
        { "UP", NES_BTN_UP }, { "DOWN", NES_BTN_DOWN }, { "LEFT", NES_BTN_LEFT }, { "RIGHT", NES_BTN_RIGHT },
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

static uint8_t keys_at(const KeyPress *presses, int count, uint64_t frame)
{
    if (g_auto_input) return nes_auto_input(frame);
    uint8_t keys = 0;
    for (int i = 0; i < count; i++) {
        if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].duration) keys |= presses[i].keys;
    }
    return keys;
}

/* One press every 30 frames from frame 120: START then three A presses
 * (title screens, file select, dialogue) for 80 presses, then a cycle of
 * movement and the other buttons. START comes in pairs so a pause menu it
 * opens is closed again. Directions are held for most of a slot. */
uint8_t nes_auto_input(uint64_t frame)
{
    static const uint8_t play[] = {
        NES_BTN_A, NES_BTN_RIGHT, NES_BTN_B, NES_BTN_DOWN, NES_BTN_A, NES_BTN_LEFT, NES_BTN_A, NES_BTN_UP,
        NES_BTN_B, NES_BTN_RIGHT, NES_BTN_A, NES_BTN_DOWN, NES_BTN_SELECT, NES_BTN_LEFT, NES_BTN_B, NES_BTN_UP,
        NES_BTN_START, NES_BTN_A, NES_BTN_START, NES_BTN_A,
    };
    const uint64_t first = 120, slot_frames = 30, menu_slots = 80;
    if (frame < first) return 0;
    uint64_t slot = (frame - first) / slot_frames, at = (frame - first) % slot_frames;
    if (slot < menu_slots) return at < 6 ? (slot % 4 == 0 ? NES_BTN_START : NES_BTN_A) : 0;
    uint8_t key = play[(slot - menu_slots) % (sizeof play / sizeof play[0])];
    bool direction = (key & (NES_BTN_RIGHT | NES_BTN_LEFT | NES_BTN_UP | NES_BTN_DOWN)) != 0;
    return at < (direction ? 24u : 6u) ? key : 0;
}

/* The CPU has jammed, or jumps to itself with nothing left to get it out:
 * NMI disabled and interrupts masked. A jump-to-self with an interrupt
 * enabled is how a program waits for one (blargg's vbl_clear_time does its
 * whole test from the NMI handler); that only counts once it has settled. */
static bool at_idle_loop(NesMachine *m)
{
    const NesCpu *c = &m->cpu;
    if (c->jammed) return true;
    const uint16_t pc = c->pc;
    if (nes_peek(m, pc) != 0x4C ||
        (uint16_t)(nes_peek(m, (uint16_t)(pc + 1u)) | (nes_peek(m, (uint16_t)(pc + 2u)) << 8)) != pc)
        return false;
    return !(m->ppu.ctrl & 0x80u) && c->flag_i;
}

/* --until-idle's second way to stop: a loop of several instructions that
 * changes nothing (blargg's later shells end in SEI / LDA #0 / STA $2000 /
 * JMP). Sampled at the end of each frame. */
#define SETTLED_FRAMES 120
#define SETTLED_PC_SPAN 16u

typedef struct {
    uint64_t memory; /* FNV-1a of work RAM and PRG RAM */
    uint16_t pc_low, pc_high;
    uint8_t a, x, y, s, p;
    int frames; /* consecutive frames with the same sample */
} Settled;

static uint64_t fnv1a(uint64_t h, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

static bool settled(NesMachine *m, Settled *t)
{
    const NesCpu *c = &m->cpu;
    uint64_t memory = fnv1a(14695981039346656037ull, m->ram, sizeof m->ram);
    memory = fnv1a(memory, m->prg_ram, m->info.prg_ram_size);
    const uint8_t p = nes_cpu_get_p(c);
    const uint16_t low = c->pc < t->pc_low ? c->pc : t->pc_low, high = c->pc > t->pc_high ? c->pc : t->pc_high;
    if (t->frames && memory == t->memory && c->a == t->a && c->x == t->x && c->y == t->y && c->s == t->s &&
        p == t->p && (uint16_t)(high - low) < SETTLED_PC_SPAN) {
        t->pc_low = low;
        t->pc_high = high;
        t->frames++;
    } else {
        t->memory = memory;
        t->pc_low = t->pc_high = c->pc;
        t->a = c->a;
        t->x = c->x;
        t->y = c->y;
        t->s = c->s;
        t->p = p;
        t->frames = 1;
    }
    return t->frames >= SETTLED_FRAMES;
}

/* --dump-audio: 16-bit mono WAV at the APU's output rate; sizes patched on
 * close. Fields are stored one by one (MSVC 19.51 /O2 once miscompiled a
 * loop of shifted stores). */
#define RUNNER_AUDIO_RATE 48000u

static void wav_header(FILE *f, uint32_t samples)
{
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
    put_le32(h + 4, 36u + samples * 2u);
    put_le32(h + 24, RUNNER_AUDIO_RATE);
    put_le32(h + 28, RUNNER_AUDIO_RATE * 2u);
    put_le32(h + 40, samples * 2u);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

static uint32_t drain_audio(NesMachine *m, FILE *f)
{
    static int16_t buf[4096];
    uint32_t total = 0;
    size_t n;
    nes_sync(m);
    while ((n = nes_apu_read_samples(&m->apu, buf, 4096)) > 0) {
        if (f) {
            for (size_t i = 0; i < n; i++) {
                uint8_t le[2] = { (uint8_t)buf[i], (uint8_t)((uint16_t)buf[i] >> 8) };
                fwrite(le, 1, 2, f);
            }
        }
        total += (uint32_t)n;
    }
    return total;
}

static NesMachine *create_machine(const char *rom_path, const uint8_t *(*embedded)(size_t *))
{
    NesMachine *m = nes_create();
    if (!m) return NULL;
    size_t size = 0;
    uint8_t *file = NULL;
    const uint8_t *data = NULL;
    if (rom_path) data = file = read_file(rom_path, &size);
    else if (embedded) data = embedded(&size);
    if (!data || !nes_load_rom(m, data, size)) {
        if (!rom_path) fprintf(stderr, "no ROM given and none is embedded in this program\n");
        else if (data && m->info.prg_size && !m->info.board)
            fprintf(stderr, "cannot load ROM %s: mapper %u is not supported\n", rom_path, (unsigned)m->info.mapper);
        else fprintf(stderr, "cannot load ROM %s (missing, or not an iNES image)\n", rom_path);
        free(file);
        nes_destroy(m);
        return NULL;
    }
    free(file);
    nes_power(m);
    return m;
}

typedef struct {
    uint16_t addr;
    uint8_t value;
} RamExpect;

/* --cpu-log / --trace-last: the interpreter calls this before every
 * instruction. */
typedef struct {
    uint16_t pc;
    uint8_t a, x, y, s, p;
    uint64_t cycles;
} TraceEntry;
static TraceEntry *g_trace;
static uint32_t g_trace_len, g_trace_pos;
static FILE *g_cpu_log;

static void trace_hook(NesMachine *m)
{
    const NesCpu *c = &m->cpu;
    if (g_cpu_log) {
        fprintf(g_cpu_log, "%04X A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%" PRIu64 "\n", c->pc, c->a, c->x, c->y,
                nes_cpu_get_p(c), c->s, c->cycles);
    }
    if (g_trace) {
        TraceEntry *t = &g_trace[g_trace_pos++ % g_trace_len];
        t->pc = c->pc;
        t->a = c->a;
        t->x = c->x;
        t->y = c->y;
        t->s = c->s;
        t->p = nes_cpu_get_p(c);
        t->cycles = c->cycles;
    }
}

static void trace_print(NesMachine *m)
{
    uint32_t n = g_trace_pos < g_trace_len ? g_trace_pos : g_trace_len;
    for (uint32_t i = 0; i < n; i++) {
        const TraceEntry *t = &g_trace[(g_trace_pos - n + i) % g_trace_len];
        printf("  %04X %02X  a=%02X x=%02X y=%02X s=%02X p=%02X cyc=%" PRIu64 "\n", t->pc, nes_peek(m, t->pc), t->a,
               t->x, t->y, t->s, t->p, t->cycles);
    }
}

bool nes_profile_rom(const uint8_t *rom, size_t size, uint64_t frames, const char *trace_path, uint64_t *instructions)
{
    NesMachine *m = nes_create();
    if (!m || !nes_load_rom(m, rom, size)) {
        nes_destroy(m);
        return false;
    }
    nes_power(m);
    nes_coverage_enable(m);
    for (uint64_t frame = 0; frame < frames; frame++) {
        nes_set_buttons(m, 0, nes_auto_input(frame));
        nes_run_frame(m);
        drain_audio(m, NULL);
    }
    bool ok = nes_coverage_write_trace(m, trace_path);
    if (instructions) *instructions = m->cpu.instructions;
    nes_destroy(m);
    return ok;
}

static void print_state(const char *tag, const NesMachine *m)
{
    const NesCpu *c = &m->cpu;
    printf("  %-11s pc=%04X a=%02X x=%02X y=%02X s=%02X p=%02X cycles=%" PRIu64 " instructions=%" PRIu64 "\n", tag,
           c->pc, c->a, c->x, c->y, c->s, nes_cpu_get_p(c), c->cycles, c->instructions);
}

static bool same_cpu(const NesCpu *a, const NesCpu *b)
{
    return a->a == b->a && a->x == b->x && a->y == b->y && a->s == b->s && a->pc == b->pc &&
           nes_cpu_get_p(a) == nes_cpu_get_p(b) && a->cycles == b->cycles && a->instructions == b->instructions &&
           a->jammed == b->jammed;
}

static bool same_memory(NesMachine *a, NesMachine *b)
{
    uint8_t ca[160], cb[160];
    const uint32_t na = cart_bytes(&a->cart, ca), nb = cart_bytes(&b->cart, cb);
    return memcmp(a->ram, b->ram, sizeof a->ram) == 0 && memcmp(a->prg_ram, b->prg_ram, sizeof a->prg_ram) == 0 &&
           memcmp(a->chr_ram, b->chr_ram, sizeof a->chr_ram) == 0 &&
           memcmp(a->nt_ram, b->nt_ram, sizeof a->nt_ram) == 0 &&
           memcmp(a->ppu.palette, b->ppu.palette, sizeof a->ppu.palette) == 0 &&
           memcmp(a->ppu.oam, b->ppu.oam, sizeof a->ppu.oam) == 0 && na == nb && memcmp(ca, cb, na) == 0 &&
           memcmp(a->ppu.frame, b->ppu.frame, sizeof a->ppu.frame) == 0;
}

/* Recompiled machine `a` against interpreter `b`, in lockstep: after every
 * run-loop slice of `a`, `b` catches up to the same instruction count and
 * the CPU state and cycle counter must match exactly. Memory and the
 * picture are compared at the end of every frame. Differential mode is not
 * a hardware oracle: both machines share the same devices; it proves the
 * recompiled code executes the program exactly as the interpreter does. */
static int run_differential(NesMachine *a, NesMachine *b, uint64_t frames, const KeyPress *presses, int press_count)
{
    if (!b) return 2;
    uint64_t slices = 0, frame = 0;
    nes_set_buttons(a, 0, keys_at(presses, press_count, 0));
    nes_set_buttons(b, 0, keys_at(presses, press_count, 0));
    while (frame < frames) {
        const uint16_t last_pc = a->cpu.pc;
        const uint32_t last_key = nes_rc_key_at(a, last_pc);
        a->frame_ready = false;
        nes_run_slice(a);
        slices++;
        while (b->cpu.instructions < a->cpu.instructions) nes_run_slice(b);
        while (b->cpu.jammed && b->cpu.cycles < a->cpu.cycles) nes_run_slice(b);
        bool same = same_cpu(&a->cpu, &b->cpu);
        if (a->frame_ready) {
            frame++;
            nes_sync(a);
            nes_sync(b);
            same = same && same_memory(a, b);
            drain_audio(a, NULL);
            drain_audio(b, NULL);
            nes_set_buttons(a, 0, keys_at(presses, press_count, frame));
            nes_set_buttons(b, 0, keys_at(presses, press_count, frame));
        }
        if (!same) {
            printf("DIVERGED after slice %" PRIu64 " (frame %" PRIu64 "), slice entered at %04X (PRG page %02" PRIX32
                   ")\n",
                   slices, frame, last_pc, last_key == NES_RC_NO_KEY ? 0xFFu : NES_RC_KEY_PAGE(last_key));
            print_state("recompiled", a);
            print_state("interpreter", b);
            nes_destroy(b);
            return 1;
        }
    }
    const NesCpu *c = &a->cpu;
    printf("differential: %" PRIu64 " frames, %" PRIu64 " instructions (%.1f%% native), no divergence\nPASS\n", frame,
           c->instructions,
           c->instructions ? 100.0 * (double)(c->instructions - c->interpreted) / (double)c->instructions : 0.0);
    nes_destroy(b);
    return 0;
}

/* blargg's result protocol at $6000 (see the top of this file). Returns the
 * status byte, or -1 while the signature is missing. */
static int blargg_status(NesMachine *m)
{
    if (nes_peek(m, 0x6001) != 0xDE || nes_peek(m, 0x6002) != 0xB0 || nes_peek(m, 0x6003) != 0x61) return -1;
    return nes_peek(m, 0x6000);
}

static void blargg_text(NesMachine *m, char *out, size_t cap)
{
    size_t n = 0;
    for (uint16_t a = 0x6004; a < 0x7FFF && n + 1 < cap; a++) {
        uint8_t ch = nes_peek(m, a);
        if (!ch) break;
        out[n++] = (ch == '\n' || (ch >= 0x20 && ch < 0x7F)) ? (char)ch : '.';
    }
    out[n] = '\0';
}

int nes_runner_main(int argc, char **argv, const struct NesRcProgram *program, const char *default_rom)
{
    const char *rom_path = NULL, *frame_path = NULL, *state_path = NULL, *audio_path = NULL;
    const char *expect_text[8], *expect_any[8];
    int expect_text_count = 0, expect_any_count = 0;
    const char *trace_path = NULL, *fail_text = NULL, *cpu_log_path = NULL;
    uint64_t max_frames = 600;
    bool until_idle = false, quiet = false, show_text = false, differential = false, coverage = false, blargg = false;
    bool smoke = false;
    long start_pc = -1;
    static KeyPress presses[MAX_KEY_PRESSES];
    int press_count = 0;
    static RamExpect expects[32];
    int expect_count = 0;
    const char *save_state_path = NULL, *load_state_path = NULL;
    uint64_t save_state_frame = 0;
    /* --interpreter drops the program but still runs its embedded ROM. */
    const uint8_t *(*embedded)(size_t *) = program ? program->embedded_rom : NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--frames") && i + 1 < argc) max_frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--until-idle")) until_idle = true;
        else if (!strcmp(a, "--save-state") && i + 1 < argc) save_state_path = argv[++i];
        else if (!strcmp(a, "--save-state-frame") && i + 1 < argc) save_state_frame = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--load-state") && i + 1 < argc) load_state_path = argv[++i];
        else if (!strcmp(a, "--blargg")) blargg = true;
        else if (!strcmp(a, "--smoke")) smoke = true;
        else if (!strcmp(a, "--pc") && i + 1 < argc) start_pc = strtol(argv[++i], NULL, 16);
        else if (!strcmp(a, "--dump-frame") && i + 1 < argc) frame_path = argv[++i];
        else if (!strcmp(a, "--dump-state") && i + 1 < argc) state_path = argv[++i];
        else if (!strcmp(a, "--dump-audio") && i + 1 < argc) audio_path = argv[++i];
        else if (!strcmp(a, "--quiet")) quiet = true;
        else if (!strcmp(a, "--screen-text")) show_text = true;
        else if (!strcmp(a, "--interpreter")) program = NULL;
        else if (!strcmp(a, "--differential")) differential = true;
        else if (!strcmp(a, "--coverage")) coverage = true;
        else if (!strcmp(a, "--trace-out") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(a, "--cpu-log") && i + 1 < argc) cpu_log_path = argv[++i];
        else if (!strcmp(a, "--auto-input")) g_auto_input = true;
        else if (!strcmp(a, "--trace-last") && i + 1 < argc) g_trace_len = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--fail-text") && i + 1 < argc) fail_text = argv[++i];
        else if (!strcmp(a, "--expect-text") && i + 1 < argc && expect_text_count < 8)
            expect_text[expect_text_count++] = argv[++i];
        else if (!strcmp(a, "--expect-text-any") && i + 1 < argc && expect_any_count < 8)
            expect_any[expect_any_count++] = argv[++i];
        else if (!strcmp(a, "--headless")) continue; /* the frontend's way to ask for this runner */
        else if (!strcmp(a, "--keys") && i + 1 < argc) {
            if (press_count >= MAX_KEY_PRESSES) {
                fprintf(stderr, "too many --keys (at most %d)\n", MAX_KEY_PRESSES);
                return 2;
            }
            if (!parse_keys(argv[++i], &presses[press_count++])) {
                fprintf(stderr, "bad --keys %s (want FRAME:BUTTON+BUTTON[:FRAMES])\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--expect-ram") && i + 1 < argc && expect_count < 32) {
            const char *e = argv[++i];
            const char *eq = strchr(e, '=');
            if (!eq) {
                fprintf(stderr, "bad --expect-ram %s (want ADDR=VALUE)\n", e);
                return 2;
            }
            expects[expect_count].addr = (uint16_t)strtoul(e, NULL, 16);
            expects[expect_count].value = (uint8_t)strtoul(eq + 1, NULL, 0);
            expect_count++;
        } else if (a[0] != '-' && !rom_path) rom_path = a;
        else {
            fprintf(stderr, "unknown argument %s\n", a);
            return 2;
        }
    }
    if (!rom_path) rom_path = default_rom;
    if (!rom_path && !embedded) {
        fprintf(stderr, "usage: nes_run ROM [--frames N] [--until-idle] [--blargg] [--pc ADDR]\n"
                        "               [--expect-ram ADDR=VALUE]... [--expect-text STR]...\n"
                        "               [--expect-text-any STR]... [--fail-text STR] [--screen-text]\n"
                        "               [--dump-frame FILE.ppm] [--dump-state FILE] [--dump-audio FILE.wav]\n"
                        "               [--keys FRAME:BUTTONS[:N]]... [--auto-input] [--interpreter] [--quiet]\n");
        return 2;
    }

    NesMachine *m = create_machine(rom_path, embedded);
    if (!m) return 2;
    if (program && !nes_rc_install(m, program)) {
        fprintf(stderr, "%s does not match the ROM this program was built from; running interpreted\n",
                rom_path ? rom_path : "(embedded)");
    }
    if (start_pc >= 0) m->cpu.pc = (uint16_t)start_pc;
    if (load_state_path && !nes_state_load_file(m, load_state_path)) {
        fprintf(stderr, "cannot load state %s (missing, another ROM, or another runtime version)\n", load_state_path);
        nes_destroy(m);
        return 2;
    }
    if (coverage || trace_path) nes_coverage_enable(m);
    if (differential) {
        if (!m->rc_program) {
            fprintf(stderr, "--differential needs a recompiled program\n");
            nes_destroy(m);
            return 2;
        }
        NesMachine *b = create_machine(rom_path, embedded);
        if (b && start_pc >= 0) b->cpu.pc = (uint16_t)start_pc;
        int status = run_differential(m, b, max_frames, presses, press_count);
        nes_destroy(m);
        return status;
    }
    if (cpu_log_path) {
        g_cpu_log = fopen(cpu_log_path, "w");
        if (!g_cpu_log) fprintf(stderr, "cannot write %s\n", cpu_log_path);
    }
    if (g_trace_len) g_trace = calloc(g_trace_len, sizeof *g_trace);
    if (g_trace || g_cpu_log) m->trace = trace_hook;

    FILE *audio = NULL;
    if (audio_path) {
        audio = fopen(audio_path, "wb");
        if (!audio) fprintf(stderr, "cannot write %s\n", audio_path);
        else wav_header(audio, 0);
    }
    uint32_t audio_samples = 0;

    static char text[30 * 34 + 1];
    static char message[0x2000];
    bool idle = false;
    Settled settle = { 0 };
    int blargg_result = -1;
    int64_t failed_at = -1, reset_at = -1;
    bool state_saved = false;
    /* A loaded state carries on at its own frame. */
    uint64_t frame = load_state_path ? m->frame_count : 0;
    for (; frame < max_frames; frame++) {
        if (save_state_path && !state_saved && frame == save_state_frame) {
            state_saved = nes_state_save_file(m, save_state_path);
            if (!state_saved) break;
        }
        if (press_count || g_auto_input) nes_set_buttons(m, 0, keys_at(presses, press_count, frame));
        nes_run_frame(m);
        audio_samples += drain_audio(m, audio);
        if (blargg) {
            const int status = blargg_status(m);
            if (status == 0x81) {
                /* The test asks for the reset button, at least 100 ms from now. */
                if (reset_at < 0) reset_at = (int64_t)frame + 8;
                else if ((int64_t)frame >= reset_at) {
                    nes_reset(m);
                    reset_at = -1;
                }
            } else {
                /* $81 is still in memory while the test starts again after
                 * the reset: that is not a second request. Only an
                 * unbroken stretch of $81 counts as the delay, or the next
                 * real request would be answered at once, before the test
                 * has finished preparing for it (apu_reset/4017_written
                 * writes $4017 after asking). */
                reset_at = -1;
                if (status >= 0 && status != 0x80) {
                    blargg_result = status;
                    frame++;
                    break;
                }
            }
        }
        /* --fail-text: tests that page through results must never show it. */
        if (fail_text) {
            nes_screen_text(m, text, sizeof text);
            if (strstr(text, fail_text)) {
                failed_at = (int64_t)frame;
                if (show_text) printf("[screen at frame %" PRIu64 "]\n%s[/screen]\n", frame, text);
                break;
            }
        }
        if (until_idle && (at_idle_loop(m) || settled(m, &settle))) {
            idle = true;
            nes_run_frame(m); /* one more frame so the result is on screen */
            frame++;
            break;
        }
    }

    const NesCpu *c = &m->cpu;
    if (!quiet) {
        printf("rom=%s board=%s frames=%" PRIu64 " instructions=%" PRIu64 " cycles=%" PRIu64 "\n",
               rom_path ? rom_path : "(embedded)", m->info.board, frame, c->instructions, c->cycles);
        printf("pc=%04X a=%02X x=%02X y=%02X s=%02X p=%02X\n", c->pc, c->a, c->x, c->y, c->s, nes_cpu_get_p(c));
        if (m->rc_program && c->instructions) {
            uint64_t native = c->instructions - c->interpreted;
            printf("native=%.1f%% (%" PRIu64 " of %" PRIu64 " instructions)\n",
                   100.0 * (double)native / (double)c->instructions, native, c->instructions);
        }
    }
    if (coverage) nes_coverage_report(m);
    if (trace_path && !nes_coverage_write_trace(m, trace_path)) fprintf(stderr, "cannot write %s\n", trace_path);
    if (audio) {
        audio_samples += drain_audio(m, audio);
        wav_header(audio, audio_samples);
        fclose(audio);
    }
    if (g_cpu_log) {
        fclose(g_cpu_log);
        g_cpu_log = NULL;
    }
    if (g_trace) {
        printf("[trace]\n");
        trace_print(m);
        free(g_trace);
        g_trace = NULL;
    }
    if (frame_path && !nes_dump_ppm(m, frame_path)) fprintf(stderr, "cannot write %s\n", frame_path);
    if (state_path && !nes_dump_state(m, state_path)) fprintf(stderr, "cannot write %s\n", state_path);

    nes_screen_text(m, text, sizeof text);
    if (show_text) printf("[screen]\n%s[/screen]\n", text);

    int status = 0;
    if (blargg) {
        blargg_text(m, message, sizeof message);
        if (message[0] && !quiet) printf("[blargg]\n%s\n[/blargg]\n", message);
        if (blargg_result < 0) {
            printf("FAIL: no result at $6000 after %" PRIu64 " frames (status %d)\n", frame, blargg_status(m));
            status = 1;
        } else if (blargg_result != 0) {
            printf("FAIL: result code %d\n", blargg_result);
            status = 1;
        }
    }
    if (failed_at >= 0) {
        printf("FAIL: \"%s\" appeared at frame %" PRId64 "\n", fail_text, failed_at);
        status = 1;
    }
    for (int i = 0; i < expect_text_count; i++) {
        if (!strstr(text, expect_text[i])) {
            printf("FAIL: screen does not contain \"%s\"\n", expect_text[i]);
            status = 1;
        }
    }
    if (expect_any_count) {
        bool any = false;
        for (int i = 0; i < expect_any_count; i++) any = any || strstr(text, expect_any[i]) != NULL;
        if (!any) {
            printf("FAIL: screen contains none of the %d expected texts\n", expect_any_count);
            status = 1;
        }
    }
    if (until_idle && !idle) {
        printf("FAIL: not idle after %" PRIu64 " frames\n", frame);
        status = 1;
    }
    if (smoke) {
        bool seen[512] = { false };
        int colours = 0;
        for (size_t i = 0; i < (size_t)NES_WIDTH * NES_HEIGHT; i++) {
            const uint16_t px = m->ppu.frame[i] & 0x1FFu;
            if (!seen[px]) {
                seen[px] = true;
                colours++;
            }
        }
        if (c->jammed) {
            printf("FAIL: the CPU jammed at %04X\n", c->pc);
            status = 1;
        } else if (colours < 3) {
            printf("FAIL: the picture has %d colour%s\n", colours, colours == 1 ? "" : "s");
            status = 1;
        }
    }
    if (save_state_path && !state_saved) {
        printf("FAIL: no state written to %s (frame %" PRIu64 " not reached, or the file cannot be written)\n",
               save_state_path, save_state_frame);
        status = 1;
    }
    for (int i = 0; i < expect_count; i++) {
        uint8_t got = nes_peek(m, expects[i].addr);
        if (got != expects[i].value) {
            printf("FAIL: [%04X]=%02X, expected %02X\n", expects[i].addr, got, expects[i].value);
            status = 1;
        }
    }
    if (status == 0 &&
        (until_idle || expect_text_count || expect_any_count || expect_count || fail_text || blargg || smoke))
        printf("PASS\n");
    nes_destroy(m);
    return status;
}
