/**
 * gbrt_run.c - recording a played run and playing it again. See gbrt_run.h.
 */

#include "gbrt_run.h"

#include "gbrt_data_mod.h"
#include "gbrt_hash.h"
#include "gbrt_unpacked.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The sizes gbrt.c allocates. */
#define RUN_WRAM_SIZE 0x8000u
#define RUN_HRAM_SIZE 0x7Fu

#define RUN_FORMAT_VERSION 1u
#define RUN_DEFAULT_CHECKPOINT_FRAMES 60u
#define RUN_PATH_MAX 1024

typedef enum {
    RUN_IDLE = 0,
    RUN_RECORDING,
    RUN_REPLAYING,
} RunMode;

typedef struct {
    char kind;       /* 'i', 'k' or 'e' */
    uint64_t cycle;
    uint64_t frame;  /* k, e */
    uint64_t hash;   /* k, e */
    uint8_t dpad;    /* i */
    uint8_t buttons; /* i */
    bool interrupt;  /* i */
} RunLine;

static struct {
    RunMode mode;
    FILE* file;
    unsigned segment;
    uint64_t checkpoint_frames;
    uint64_t frames_seen;
    uint8_t dpad;
    uint8_t buttons;

    /* Counted in both modes. */
    uint64_t inputs;
    uint64_t checkpoints;

    /* Replay. */
    RunLine next;
    bool have_next;
    bool over;
    bool ended;          /* the record's `e` line was reached */
    bool diverged;
    uint64_t diverged_frame;
    uint64_t diverged_cycle;
    char diverged_why[160];
    uint64_t matched;
    uint64_t late;
    uint64_t max_late;

    char states_dir[RUN_PATH_MAX];
    uint64_t state_frames;
} g_run = {0};

/* ---------------------------------------------------------------------------
 * Small things
 * ------------------------------------------------------------------------- */

static void digest_hex(const uint8_t digest[32], char hex[65]) {
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
}

static void sha256_hex(const uint8_t* data, size_t size, char hex[65]) {
    uint8_t digest[32];
    gbrt_sha256(data, size, digest);
    digest_hex(digest, hex);
}

/* The replacements of the pack of unpacked data the game was given
 * (`--unpacked`): how many, and in `hex` what tells them from any others,
 * or "-" where there is none. They are written over memory the game holds,
 * so a run played with them is a run of the game with them. */
static size_t pack_replacements(const GBContext* ctx, char hex[65]) {
    uint8_t digest[32];
    const size_t count = gbrt_unpacked_replacements(ctx, digest);
    if (count) {
        digest_hex(digest, hex);
    } else {
        snprintf(hex, 65, "-");
    }
    return count;
}

static void replacements_text(char* text, size_t size, size_t count, const char* hex) {
    if (count) {
        snprintf(text, size, "%zu replacement%s, %s", count, count == 1 ? "" : "s", hex);
    } else {
        snprintf(text, size, "nothing replaced");
    }
}

static uint64_t fnv1a(uint64_t hash, const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t fnv1a_u64(uint64_t hash, uint64_t value) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (i * 8));
    return fnv1a(hash, bytes, sizeof(bytes));
}

/* The frames finished. The count in the context goes up when the loop starts
 * the next frame, and a game that is closed stops before that. */
static uint64_t run_frames(const GBContext* ctx) {
    return ctx->completed_frames + (ctx->frame_done ? 1u : 0u);
}

static uint64_t run_hash(const GBContext* ctx) {
    const uint8_t small[] = {
        ctx->a,
        (uint8_t)((ctx->f_z ? 0x80 : 0) | (ctx->f_n ? 0x40 : 0) |
                  (ctx->f_h ? 0x20 : 0) | (ctx->f_c ? 0x10 : 0)),
        ctx->b, ctx->c, ctx->d, ctx->e, ctx->h, ctx->l,
        (uint8_t)ctx->sp, (uint8_t)(ctx->sp >> 8),
        (uint8_t)ctx->pc, (uint8_t)(ctx->pc >> 8),
        (uint8_t)ctx->rom_bank, (uint8_t)(ctx->rom_bank >> 8),
        ctx->ram_bank, ctx->wram_bank,
        ctx->rtc.s, ctx->rtc.m, ctx->rtc.h, ctx->rtc.dl, ctx->rtc.dh,
    };
    uint64_t hash = 1469598103934665603ull;
    hash = fnv1a(hash, small, sizeof(small));
    hash = fnv1a_u64(hash, ctx->total_cycles);
    hash = fnv1a_u64(hash, run_frames(ctx));
    if (ctx->wram) hash = fnv1a(hash, ctx->wram, RUN_WRAM_SIZE);
    if (ctx->hram) hash = fnv1a(hash, ctx->hram, RUN_HRAM_SIZE);
    if (ctx->eram && ctx->eram_size) hash = fnv1a(hash, ctx->eram, ctx->eram_size);
    return hash;
}

static bool cart_has_clock(const GBContext* ctx) {
    if (!ctx->rom || ctx->rom_size <= 0x147u) return false;
    return ctx->rom[0x147] == 0x0F || ctx->rom[0x147] == 0x10;
}

static const char* model_name(GBModel model) {
    switch (model) {
        case GB_MODEL_CGB: return "cgb";
        case GB_MODEL_SGB: return "sgb";
        default: return "dmg";
    }
}

static void buttons_text(uint8_t dpad, uint8_t buttons, char text[9]) {
    size_t n = 0;
    if (!(dpad & 0x04)) text[n++] = 'U';
    if (!(dpad & 0x08)) text[n++] = 'D';
    if (!(dpad & 0x02)) text[n++] = 'L';
    if (!(dpad & 0x01)) text[n++] = 'R';
    if (!(buttons & 0x01)) text[n++] = 'A';
    if (!(buttons & 0x02)) text[n++] = 'B';
    if (!(buttons & 0x08)) text[n++] = 'S';
    if (!(buttons & 0x04)) text[n++] = 'T';
    if (n == 0) text[n++] = '-';
    text[n] = '\0';
}

static bool buttons_parse(const char* text, uint8_t* dpad, uint8_t* buttons) {
    *dpad = 0xFF;
    *buttons = 0xFF;
    if (strcmp(text, "-") == 0) return true;
    for (const char* p = text; *p; p++) {
        switch (*p) {
            case 'U': *dpad &= (uint8_t)~0x04; break;
            case 'D': *dpad &= (uint8_t)~0x08; break;
            case 'L': *dpad &= (uint8_t)~0x02; break;
            case 'R': *dpad &= (uint8_t)~0x01; break;
            case 'A': *buttons &= (uint8_t)~0x01; break;
            case 'B': *buttons &= (uint8_t)~0x02; break;
            case 'S': *buttons &= (uint8_t)~0x08; break;
            case 'T': *buttons &= (uint8_t)~0x04; break;
            default: return false;
        }
    }
    return text[0] != '\0';
}

static void segment_path(char* out, size_t size, const char* dir,
                         unsigned segment, const char* extension) {
    snprintf(out, size, "%s/segment-%04u.%s", dir, segment, extension);
}

static void run_title(const GBContext* ctx, char title[17]) {
    size_t n = 0;
    if (ctx->rom && ctx->rom_size >= 0x144u) {
        for (size_t i = 0x134u; i < 0x144u; i++) {
            const uint8_t c = ctx->rom[i];
            if (c == 0) break;
            title[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    while (n > 0 && title[n - 1] == ' ') n--;
    title[n] = '\0';
}

static void write_state(const GBContext* ctx) {
    char path[RUN_PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/segment-%04u-f%09llu.state",
             g_run.states_dir, g_run.segment,
             (unsigned long long)ctx->completed_frames);
    if (!gb_context_save_state_file((GBContext*)ctx, path)) {
        fprintf(stderr, "[RUN] could not write %s\n", path);
    }
}

void gbrt_run_set_states(const char* dir, uint64_t every_frames) {
    g_run.states_dir[0] = '\0';
    g_run.state_frames = 0;
    if (dir && dir[0] && every_frames > 0) {
        snprintf(g_run.states_dir, sizeof(g_run.states_dir), "%s", dir);
        g_run.state_frames = every_frames;
    }
}

bool gbrt_run_recording(void) { return g_run.mode == RUN_RECORDING; }
bool gbrt_run_replaying(void) { return g_run.mode == RUN_REPLAYING; }
unsigned gbrt_run_segment(void) {
    return g_run.mode == RUN_IDLE ? 0u : g_run.segment;
}

/* ---------------------------------------------------------------------------
 * Recording
 * ------------------------------------------------------------------------- */

bool gbrt_run_record_start(GBContext* ctx, const char* dir) {
    if (!ctx || !dir || !dir[0] || g_run.mode != RUN_IDLE) return false;
    if (ctx->total_cycles != 0) {
        fprintf(stderr, "[RUN] a run is recorded from the game starting, "
                        "not from a save state\n");
        return false;
    }
    if (ctx->link) {
        fprintf(stderr, "[RUN] a run with another player linked cannot be "
                        "recorded: the other game is not in the record\n");
        return false;
    }

    char path[RUN_PATH_MAX + 64];
    unsigned segment = 1;
    for (;; segment++) {
        segment_path(path, sizeof(path), dir, segment, "run");
        FILE* existing = fopen(path, "rb");
        if (!existing) break;
        fclose(existing);
        if (segment >= 9999u) {
            fprintf(stderr, "[RUN] %s holds 9999 segments already\n", dir);
            return false;
        }
    }

    char save_line[160] = "save -";
    if (ctx->eram && ctx->eram_size) {
        char save_path[RUN_PATH_MAX + 64];
        segment_path(save_path, sizeof(save_path), dir, segment, "sav");
        FILE* save = fopen(save_path, "wb");
        const bool written =
            save && fwrite(ctx->eram, 1, ctx->eram_size, save) == ctx->eram_size;
        if (save && fclose(save) != 0) {
            fprintf(stderr, "[RUN] could not write %s\n", save_path);
            return false;
        }
        if (!written) {
            fprintf(stderr, "[RUN] could not write %s\n", save_path);
            return false;
        }
        bool blank = ctx->eram[0] == 0x00 || ctx->eram[0] == 0xFF;
        for (size_t i = 1; blank && i < ctx->eram_size; i++) {
            blank = ctx->eram[i] == ctx->eram[0];
        }
        char hex[65];
        sha256_hex(ctx->eram, ctx->eram_size, hex);
        snprintf(save_line, sizeof(save_line), "save segment-%04u.sav %zu %s %s",
                 segment, ctx->eram_size, hex, blank ? "blank" : "used");
    }

    FILE* file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "[RUN] could not create %s\n", path);
        return false;
    }

    char rom_hex[65];
    sha256_hex(ctx->rom, ctx->rom_size, rom_hex);
    char title[17];
    run_title(ctx, title);
    const time_t now = time(NULL);

    fprintf(file, "native-console-run %u\n", RUN_FORMAT_VERSION);
    fprintf(file, "console gb\n");
    fprintf(file, "segment %u\n", segment);
    fprintf(file, "rom %s %zu %s\n", rom_hex, ctx->rom_size, title);
    fprintf(file, "model %s %d\n", model_name(ctx->config.model),
            ctx->config.cgb_compatibility_mode ? 1 : 0);
    fprintf(file, "clock 4194304\n");
    fprintf(file, "checkpoint-frames %u\n", RUN_DEFAULT_CHECKPOINT_FRAMES);
    fprintf(file, "%s\n", save_line);
    if (cart_has_clock(ctx)) {
        fprintf(file, "rtc %u %u %u %u %u %u %u %u %u %u %u %llu %d\n",
                ctx->rtc.s, ctx->rtc.m, ctx->rtc.h, ctx->rtc.dl, ctx->rtc.dh,
                ctx->rtc.latched_s, ctx->rtc.latched_m, ctx->rtc.latched_h,
                ctx->rtc.latched_dl, ctx->rtc.latched_dh, ctx->rtc.latch_state,
                (unsigned long long)ctx->rtc.last_time, ctx->rtc.active ? 1 : 0);
    }
    fprintf(file, "data-mod %zu\n", gbrt_data_mod_entry_count(ctx));
    char pack_hex[65];
    const size_t replacements = pack_replacements(ctx, pack_hex);
    fprintf(file, "unpacked %zu %s\n", replacements, pack_hex);
    fprintf(file, "started %lld\n", (long long)(now == (time_t)-1 ? 0 : now));
    fprintf(file, "begin\n");
    if (fflush(file) != 0) {
        fprintf(stderr, "[RUN] could not write %s\n", path);
        fclose(file);
        return false;
    }

    g_run.mode = RUN_RECORDING;
    g_run.file = file;
    g_run.segment = segment;
    g_run.checkpoint_frames = RUN_DEFAULT_CHECKPOINT_FRAMES;
    g_run.frames_seen = UINT64_MAX;
    g_run.dpad = 0xFF;
    g_run.buttons = 0xFF;
    g_run.inputs = 0;
    g_run.checkpoints = 0;
    fprintf(stderr, "[RUN] recording segment %u to %s\n", segment, path);
    return true;
}

void gbrt_run_record_slice(GBContext* ctx, uint8_t dpad, uint8_t buttons,
                           bool interrupt) {
    if (g_run.mode != RUN_RECORDING || !ctx) return;
    bool wrote = false;

    if (ctx->completed_frames != g_run.frames_seen) {
        g_run.frames_seen = ctx->completed_frames;
        if (ctx->completed_frames % g_run.checkpoint_frames == 0) {
            fprintf(g_run.file, "k %llu %llu %016llx\n",
                    (unsigned long long)ctx->completed_frames,
                    (unsigned long long)ctx->total_cycles,
                    (unsigned long long)run_hash(ctx));
            g_run.checkpoints++;
            wrote = true;
        }
    }

    dpad |= 0xF0;
    buttons |= 0xF0;
    if (dpad != g_run.dpad || buttons != g_run.buttons || interrupt) {
        char text[9];
        buttons_text(dpad, buttons, text);
        fprintf(g_run.file, "i %llu %s%s\n",
                (unsigned long long)ctx->total_cycles, text,
                interrupt ? " !" : "");
        g_run.dpad = dpad;
        g_run.buttons = buttons;
        g_run.inputs++;
        wrote = true;
    }

    /* A line is on the disk as soon as it is written: what was played up to
     * a crash is kept. */
    if (wrote) fflush(g_run.file);
}

/* ---------------------------------------------------------------------------
 * Replaying
 * ------------------------------------------------------------------------- */

static void replay_read(void) {
    char line[256];
    g_run.have_next = false;
    while (fgets(line, sizeof(line), g_run.file)) {
        const size_t length = strlen(line);
        if (length == 0 || line[length - 1] != '\n') {
            /* Half a line: the recording was cut off while writing it. */
            return;
        }
        RunLine parsed = {0};
        unsigned long long a = 0, b = 0, c = 0;
        char text[64] = {0};
        char mark = 0;
        if (line[0] == 'i' && line[1] == ' ') {
            const int fields = sscanf(line + 2, "%llu %63s %c", &a, text, &mark);
            if (fields < 2 || !buttons_parse(text, &parsed.dpad, &parsed.buttons)) {
                return;
            }
            parsed.kind = 'i';
            parsed.cycle = a;
            parsed.interrupt = fields == 3 && mark == '!';
        } else if (line[0] == 'k' && line[1] == ' ') {
            if (sscanf(line + 2, "%llu %llu %llx", &a, &b, &c) != 3) return;
            parsed.kind = 'k';
            parsed.frame = a;
            parsed.cycle = b;
            parsed.hash = c;
        } else if (line[0] == 'e' && line[1] == ' ') {
            if (sscanf(line + 2, "%llu %llu %llx", &a, &b, &c) != 3) return;
            parsed.kind = 'e';
            parsed.cycle = a;
            parsed.frame = b;
            parsed.hash = c;
        } else {
            return;
        }
        g_run.next = parsed;
        g_run.have_next = true;
        return;
    }
}

static bool replay_fail(FILE* file, const char* what) {
    fprintf(stderr, "[RUN] %s\n", what);
    if (file) fclose(file);
    return false;
}

bool gbrt_run_replay_start(GBContext* ctx, const char* path) {
    if (!ctx || !path || !path[0] || g_run.mode != RUN_IDLE) return false;
    if (ctx->link) {
        fprintf(stderr, "[RUN] a run cannot be replayed with another player linked\n");
        return false;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "[RUN] could not open %s\n", path);
        return false;
    }

    char line[512];
    unsigned version = 0;
    if (!fgets(line, sizeof(line), file) ||
        sscanf(line, "native-console-run %u", &version) != 1) {
        return replay_fail(file, "not a run record");
    }
    if (version != RUN_FORMAT_VERSION) {
        return replay_fail(file, "a run record of a version this game does not read");
    }

    const bool resumed = ctx->total_cycles != 0;
    unsigned segment = 0;
    uint64_t checkpoint_frames = 0;
    bool seen_rom = false, seen_model = false, seen_save = false, begun = false;
    bool seen_pack = false;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        char word[64] = {0};
        char hex[80] = {0};
        char name[128] = {0};
        unsigned long long size = 0;
        unsigned long long number = 0;
        int flag = 0;
        if (strcmp(line, "begin") == 0) {
            begun = true;
            break;
        } else if (sscanf(line, "console %63s", word) == 1) {
            if (strcmp(word, "gb") != 0) {
                return replay_fail(file, "the record is of another console");
            }
        } else if (sscanf(line, "segment %u", &segment) == 1) {
        } else if (sscanf(line, "rom %79s %llu", hex, &size) == 2) {
            char mine[65];
            sha256_hex(ctx->rom, ctx->rom_size, mine);
            if (size != ctx->rom_size || strcmp(hex, mine) != 0) {
                fprintf(stderr, "[RUN] the record is of another ROM:\n"
                                "[RUN]   record %s (%llu bytes)\n"
                                "[RUN]   game   %s (%zu bytes)\n",
                        hex, size, mine, ctx->rom_size);
                fclose(file);
                return false;
            }
            seen_rom = true;
        } else if (sscanf(line, "model %63s %d", word, &flag) == 2) {
            if (strcmp(word, model_name(ctx->config.model)) != 0 ||
                (flag != 0) != ctx->config.cgb_compatibility_mode) {
                fprintf(stderr, "[RUN] the record was played as %s; "
                                "start the game with --model %s\n",
                        word, strcmp(word, "dmg") == 0 ? "dmg" : "cgb");
                fclose(file);
                return false;
            }
            seen_model = true;
        } else if (sscanf(line, "checkpoint-frames %llu", &number) == 1) {
            checkpoint_frames = number;
        } else if (strcmp(line, "save -") == 0) {
            if (ctx->eram_size != 0) {
                return replay_fail(file, "the record has no cartridge memory, the game does");
            }
            seen_save = true;
        } else if (sscanf(line, "save %127s %llu %79s", name, &size, hex) == 3) {
            seen_save = true;
            if (size != ctx->eram_size) {
                return replay_fail(file, "the record's cartridge memory is of another size");
            }
            if (resumed) continue;
            if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
                return replay_fail(file, "the record names a save outside its folder");
            }
            char save_path[RUN_PATH_MAX + 160];
            const char* slash = strrchr(path, '/');
            const char* backslash = strrchr(path, '\\');
            if (backslash && (!slash || backslash > slash)) slash = backslash;
            const int dir_length = slash ? (int)(slash - path) + 1 : 0;
            snprintf(save_path, sizeof(save_path), "%.*s%s", dir_length, path, name);
            FILE* save = fopen(save_path, "rb");
            uint8_t* data = (uint8_t*)malloc(ctx->eram_size);
            const bool read = save && data &&
                fread(data, 1, ctx->eram_size, save) == ctx->eram_size &&
                fgetc(save) == EOF;
            if (save) fclose(save);
            char mine[65] = {0};
            if (read) sha256_hex(data, ctx->eram_size, mine);
            if (!read || strcmp(mine, hex) != 0) {
                free(data);
                fprintf(stderr, "[RUN] %s is missing or is not the save the "
                                "segment began with\n", save_path);
                fclose(file);
                return false;
            }
            memcpy(ctx->eram, data, ctx->eram_size);
            free(data);
        } else if (strncmp(line, "rtc ", 4) == 0) {
            unsigned v[11] = {0};
            unsigned long long remainder = 0;
            int active = 0;
            if (sscanf(line, "rtc %u %u %u %u %u %u %u %u %u %u %u %llu %d",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                       &v[8], &v[9], &v[10], &remainder, &active) != 13) {
                return replay_fail(file, "the record's clock line cannot be read");
            }
            if (resumed) continue;
            ctx->rtc.s = (uint8_t)v[0];
            ctx->rtc.m = (uint8_t)v[1];
            ctx->rtc.h = (uint8_t)v[2];
            ctx->rtc.dl = (uint8_t)v[3];
            ctx->rtc.dh = (uint8_t)v[4];
            ctx->rtc.latched_s = (uint8_t)v[5];
            ctx->rtc.latched_m = (uint8_t)v[6];
            ctx->rtc.latched_h = (uint8_t)v[7];
            ctx->rtc.latched_dl = (uint8_t)v[8];
            ctx->rtc.latched_dh = (uint8_t)v[9];
            ctx->rtc.latch_state = (uint8_t)v[10];
            ctx->rtc.last_time = remainder;
            ctx->rtc.active = active != 0;
        } else if (sscanf(line, "data-mod %llu", &number) == 1) {
            if (number != gbrt_data_mod_entry_count(ctx)) {
                return replay_fail(file, "the record was played with other data mods");
            }
        } else if (strncmp(line, "unpacked ", 9) == 0) {
            if (sscanf(line, "unpacked %llu %79s", &number, hex) != 2 ||
                (number == 0) != (strcmp(hex, "-") == 0)) {
                return replay_fail(file, "the record's line about a pack of unpacked data cannot be read");
            }
            seen_pack = true;
            char mine[65];
            const size_t replacements = pack_replacements(ctx, mine);
            if (number != replacements || strcmp(hex, mine) != 0) {
                char recorded[128], given[128];
                replacements_text(recorded, sizeof(recorded), (size_t)number, hex);
                replacements_text(given, sizeof(given), replacements, mine);
                fprintf(stderr, "[RUN] %s:\n"
                                "[RUN]   record %s\n"
                                "[RUN]   replay %s\n",
                        number == 0 ? "the record was played with nothing replaced, and the pack of "
                                      "unpacked data given (--unpacked) replaces what the game unpacks"
                        : replacements == 0 ? "the record was played with a pack of unpacked data "
                                              "that replaces what the game unpacks; give the replay "
                                              "that pack (--unpacked)"
                        : "the record was played with another pack of unpacked data (--unpacked)",
                        recorded, given);
                fclose(file);
                return false;
            }
        }
        /* Any other line is from a later version of the format: left alone. */
    }
    if (!begun || !seen_rom || !seen_model || !seen_save || checkpoint_frames == 0) {
        return replay_fail(file, "the run record's header is incomplete");
    }
    if (!seen_pack) {
        /* A record from before records had the line: it cannot be told. */
        char mine[65];
        const size_t replacements = pack_replacements(ctx, mine);
        if (replacements) {
            fprintf(stderr, "[RUN] the record does not say what pack of unpacked data it was "
                            "played with; this replay has %zu replacement%s, and leaves the "
                            "record if it was played without\n",
                    replacements, replacements == 1 ? "" : "s");
        }
    }
    /* A replay never writes a save: not the player's, not the record's. */
    ctx->callbacks.save_battery_ram = NULL;
    ctx->callbacks.save_rtc_data = NULL;

    g_run.mode = RUN_REPLAYING;
    g_run.file = file;
    g_run.segment = segment;
    g_run.checkpoint_frames = checkpoint_frames;
    g_run.frames_seen = UINT64_MAX;
    g_run.dpad = 0xFF;
    g_run.buttons = 0xFF;
    g_run.inputs = 0;
    g_run.checkpoints = 0;
    g_run.matched = 0;
    g_run.late = 0;
    g_run.max_late = 0;
    g_run.over = false;
    g_run.ended = false;
    g_run.diverged = false;

    replay_read();
    /* Started from a state: what the record holds before it has happened. */
    while (g_run.have_next && g_run.next.cycle < ctx->total_cycles) {
        if (g_run.next.kind == 'i') {
            g_run.dpad = g_run.next.dpad;
            g_run.buttons = g_run.next.buttons;
        } else if (g_run.next.kind == 'e') {
            return replay_fail(file, "the state is from after the segment's end");
        }
        replay_read();
    }
    fprintf(stderr, "[RUN] replaying segment %u of %s%s\n", segment, path,
            resumed ? ", from a state" : "");
    return true;
}

static void replay_diverge(const GBContext* ctx, const char* why) {
    if (g_run.diverged) return;
    g_run.diverged = true;
    g_run.over = true;
    g_run.diverged_frame = ctx->completed_frames;
    g_run.diverged_cycle = ctx->total_cycles;
    snprintf(g_run.diverged_why, sizeof(g_run.diverged_why), "%s", why);
}

uint32_t gbrt_run_replay_slice(GBContext* ctx, uint32_t budget,
                               uint8_t* dpad, uint8_t* buttons,
                               bool* interrupt) {
    *interrupt = false;
    if (g_run.mode != RUN_REPLAYING || !ctx) return budget;
    const uint64_t now = ctx->total_cycles;

    bool at_checkpoint = false;
    uint64_t hash = 0;
    if (ctx->completed_frames != g_run.frames_seen) {
        g_run.frames_seen = ctx->completed_frames;
        /* Before this cycle's lines are taken, so that a replay started from
         * the state takes them itself. */
        if (g_run.state_frames &&
            ctx->completed_frames % g_run.state_frames == 0) {
            write_state(ctx);
        }
        if (ctx->completed_frames % g_run.checkpoint_frames == 0) {
            at_checkpoint = true;
            hash = run_hash(ctx);
        }
    }

    while (!g_run.over && g_run.have_next && g_run.next.cycle <= now) {
        const RunLine line = g_run.next;
        char why[160];
        switch (line.kind) {
            case 'i':
                if (line.cycle < now) {
                    /* The machine could not be stopped on the cycle. */
                    g_run.late++;
                    if (now - line.cycle > g_run.max_late) {
                        g_run.max_late = now - line.cycle;
                    }
                }
                g_run.dpad = line.dpad;
                g_run.buttons = line.buttons;
                if (line.interrupt) *interrupt = true;
                g_run.inputs++;
                break;
            case 'k':
                g_run.checkpoints++;
                if (at_checkpoint && line.frame == ctx->completed_frames &&
                    line.cycle == now && line.hash == hash) {
                    g_run.matched++;
                    at_checkpoint = false;
                } else if (at_checkpoint && line.frame == ctx->completed_frames) {
                    snprintf(why, sizeof(why),
                             "frame %llu is at cycle %llu with hash %016llx; "
                             "the record has cycle %llu, hash %016llx",
                             (unsigned long long)ctx->completed_frames,
                             (unsigned long long)now, (unsigned long long)hash,
                             (unsigned long long)line.cycle,
                             (unsigned long long)line.hash);
                    replay_diverge(ctx, why);
                } else {
                    snprintf(why, sizeof(why),
                             "the record's frame %llu began at cycle %llu; "
                             "the replay is at cycle %llu in frame %llu",
                             (unsigned long long)line.frame,
                             (unsigned long long)line.cycle,
                             (unsigned long long)now,
                             (unsigned long long)ctx->completed_frames);
                    replay_diverge(ctx, why);
                }
                break;
            case 'e':
                g_run.over = true;
                g_run.ended = true;
                if (line.cycle != now || line.frame != ctx->completed_frames ||
                    line.hash != run_hash(ctx)) {
                    snprintf(why, sizeof(why),
                             "the record ends at cycle %llu in frame %llu with "
                             "hash %016llx; the replay has hash %016llx there",
                             (unsigned long long)line.cycle,
                             (unsigned long long)line.frame,
                             (unsigned long long)line.hash,
                             (unsigned long long)run_hash(ctx));
                    replay_diverge(ctx, why);
                }
                break;
            default:
                break;
        }
        if (!g_run.over) replay_read();
    }

    if (!g_run.over && !g_run.have_next) {
        /* The recording was never closed: this is as far as it goes. */
        g_run.over = true;
    }
    if (!g_run.over && at_checkpoint) {
        replay_diverge(ctx, "the replay began a frame the record has no "
                            "checkpoint for where it should have one");
    }

    *dpad = g_run.dpad;
    *buttons = g_run.buttons;
    if (g_run.over) return budget;

    const uint64_t until = g_run.next.cycle - now;
    if (until < (uint64_t)budget) return (uint32_t)until;
    return budget;
}

bool gbrt_run_replay_over(void) {
    return g_run.mode == RUN_REPLAYING && g_run.over;
}

/* ---------------------------------------------------------------------------
 * The end
 * ------------------------------------------------------------------------- */

bool gbrt_run_finish(GBContext* ctx) {
    bool ok = true;
    if (g_run.mode == RUN_RECORDING && ctx) {
        char save_hex[65] = "-";
        if (ctx->eram && ctx->eram_size) {
            sha256_hex(ctx->eram, ctx->eram_size, save_hex);
        }
        fprintf(g_run.file, "e %llu %llu %016llx %s\n",
                (unsigned long long)ctx->total_cycles,
                (unsigned long long)run_frames(ctx),
                (unsigned long long)run_hash(ctx), save_hex);
        ok = fflush(g_run.file) == 0;
        fprintf(stderr,
                "[RUN] recorded segment %u: %llu inputs, %llu checkpoints, "
                "%llu frames\n",
                g_run.segment, (unsigned long long)g_run.inputs,
                (unsigned long long)g_run.checkpoints,
                (unsigned long long)run_frames(ctx));
    } else if (g_run.mode == RUN_REPLAYING && ctx) {
        const char* end = g_run.diverged ? "diverged"
                        : g_run.ended ? "matched"
                        : g_run.over ? "unclosed"
                        : "not-reached";
        if (g_run.diverged) {
            fprintf(stderr, "[RUN] the replay left the record in frame %llu, "
                            "at cycle %llu: %s\n",
                    (unsigned long long)g_run.diverged_frame,
                    (unsigned long long)g_run.diverged_cycle,
                    g_run.diverged_why);
            ok = false;
        }
        /* One line a tool can read. */
        printf("[RUN] result segment=%u end=%s inputs=%llu late=%llu "
               "max_late=%llu checkpoints=%llu matched=%llu frames=%llu "
               "cycles=%llu\n",
               g_run.segment, end, (unsigned long long)g_run.inputs,
               (unsigned long long)g_run.late,
               (unsigned long long)g_run.max_late,
               (unsigned long long)g_run.checkpoints,
               (unsigned long long)g_run.matched,
               (unsigned long long)run_frames(ctx),
               (unsigned long long)ctx->total_cycles);
        fflush(stdout);
    }
    if (g_run.file) fclose(g_run.file);
    g_run.file = NULL;
    g_run.mode = RUN_IDLE;
    return ok;
}
