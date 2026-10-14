/*
 * lift_check.c - run lifted functions against the original and compare.
 *
 * For each candidate the checker makes a machine state, copies it, runs the
 * original subroutine on one copy with the interpreter and the lifted C on
 * the other, and compares what is left: registers, flags, every byte of RAM,
 * the cartridge mapping, and the ordered list of writes to hardware
 * registers. It does that for many states. The states are random at first
 * and then aimed: later trials change the bytes the original was seen to
 * read.
 *
 * States can also be the game's own: a moment of its memory with registers
 * made up (--image), or the registers and memory a function was really
 * entered with, kept by a game played with --entry-states (--entries). From
 * those a function is first run exactly as the game ran it.
 *
 * A pass is evidence, not proof: it says the two agree on every state tried,
 * and the report says how much of the original those states exercised.
 */

/* For a file of entry states, which may be larger than a long counts. */
#ifndef _MSC_VER
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <sys/types.h>
#endif

#include "lift_check.h"
#include "lift_machine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETURN_SENTINEL 0xFEFEu
#define MAX_REPORTED_MISMATCHES 3
#define MAX_DIFFERENCES 8
#define GIVE_UP_AFTER 12u
#define ENOUGH_WAITING 32u

enum {
    IGNORE_A = 1 << 0, IGNORE_B = 1 << 1, IGNORE_C = 1 << 2, IGNORE_D = 1 << 3,
    IGNORE_E = 1 << 4, IGNORE_H = 1 << 5, IGNORE_L = 1 << 6,
    IGNORE_FZ = 1 << 7, IGNORE_FN = 1 << 8, IGNORE_FH = 1 << 9, IGNORE_FC = 1 << 10,
    IGNORE_IME = 1 << 11,
};

typedef struct Options {
    const char* rom_path;
    const char* image_path;
    const char* entries_path;
    const char* json_path;
    const char* only;
    const char* ignore;
    unsigned trials;
    uint64_t seed;
    uint64_t max_steps;
    int strict_stack;
    int observe;    /* also find out what each function reads and what it leaves as it came */
    unsigned observe_states; /* from this many states of a run at most; 0: from all of them */
    unsigned shard; /* with shards > 1: check only candidates whose index % shards == shard */
    unsigned shards;
} Options;

typedef struct Image {
    int present;
    GBContext regs;
    uint16_t rom_bank;
} Image;

/* One state a function was entered in (runtimes/runtime-gb/src/gbrt_entry_states.c has
 * the file). */
typedef struct EntryState {
    uint16_t bank, address; /* the function */
    uint16_t rom_bank;      /* mapped at 0x4000 */
    uint16_t sp, returns_to;
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t ime, ram_bank, ram_enabled, wram_bank, vram_bank;
    uint32_t arrival;
} EntryState;

typedef struct Entries {
    int present;
    FILE* file;
    uint32_t eram_size;
    uint32_t pages_per_state;
    uint32_t state_count;
    uint32_t page_count;
    EntryState* states;     /* by bank, address and arrival */
    uint64_t ids_at;        /* in the file: each state's page numbers */
    uint64_t pages_at;      /* in the file: the pages */
    size_t body_bytes;      /* a state's memory, put together */
} Entries;

typedef struct Differences {
    char text[MAX_DIFFERENCES][160];
    unsigned count;
    unsigned total;
} Differences;

/* The RAM and register addresses the original has read so far, in the
 * order it first read them. */
typedef struct ReadList {
    uint16_t address[0x8000];
    unsigned count;
    /* Bytes found to hold the game's idea of the mapped ROM bank. A helper
     * that calls into another bank maps this value back when it is done, so
     * a state in which it is not the function's own bank cannot happen. */
    uint16_t bank_variable[8];
    unsigned bank_variables;
    /* Bytes that held the bank mapped in one state that went wrong over it,
     * waiting to be seen holding another bank in another. */
    uint16_t bank_candidate[64];
    unsigned bank_candidates;
    uint8_t bank_candidate_value;
} ReadList;

static LiftMachine g_base;
static LiftMachine g_original;
static LiftMachine g_lifted;
static LiftMachine g_probe;
static GBContext g_probe_ctx;
static LiftWakes g_wakes;
static GBContext g_original_ctx;
static GBContext g_lifted_ctx;
static jmp_buf g_escape;

/* ---- gblift.h, as the checker provides it ------------------------------ */

static LiftMachine* machine_of(GBContext* ctx) {
    return (LiftMachine*)ctx->machine;
}

uint8_t lift_read8(GBContext* ctx, uint16_t address) {
    return lift_machine_read(machine_of(ctx), address);
}

void lift_write8(GBContext* ctx, uint16_t address, uint8_t value) {
    LiftMachine* machine = machine_of(ctx);
    if (ctx->sp < machine->min_sp) machine->min_sp = ctx->sp;
    lift_machine_write(machine, address, value);
}

uint8_t* lift_ram(GBContext* ctx, uint16_t address) {
    LiftMachine* machine = machine_of(ctx);
    /* A loop that only looks at a variable still has to end. */
    lift_machine_count_access(machine);
    if (address >= 0xC000u && address < 0xD000u) return &machine->wram[0][address - 0xC000u];
    if (address >= 0xD000u && address < 0xE000u) {
        return &machine->wram[machine->wram_bank][address - 0xD000u];
    }
    if (address >= 0xFF80u && address < 0xFFFFu) return &machine->hram[address - 0xFF80u];
    return NULL;
}

/* What the lifted function said it leaves undefined, in the state being
 * run: the bits are the IGNORE_ ones, which gblift.h's LIFT_REG_ and
 * LIFT_FLAG_ are numbered after. */
static unsigned g_undefined;

void lift_undefined(GBContext* ctx, unsigned what) {
    (void)ctx;
    g_undefined |= what & (IGNORE_IME - 1u);
}

/* The function said, in the state being run, that it keeps its own stack
 * (gblift.h). */
static int g_own_stack;

void lift_own_stack(GBContext* ctx) {
    (void)ctx;
    g_own_stack = 1;
}

void lift_tick(GBContext* ctx, uint32_t cycles) {
    LiftMachine* machine = machine_of(ctx);
    machine->cycles += cycles;
    /* A loop that only passes time still has to end. */
    lift_machine_count_access(machine);
}

void lift_advance(GBContext* ctx, uint32_t cycles) {
    machine_of(ctx)->cycles += cycles;
}

/* No interrupt is ever delivered here, so the delay EI has on hardware
 * cannot show. */
void lift_ei(GBContext* ctx) {
    ctx->ime = 1;
}

void lift_di(GBContext* ctx) {
    ctx->ime = 0;
}

void lift_halt(GBContext* ctx) {
    LiftMachine* machine = machine_of(ctx);
    lift_machine_log_event(machine, LIFT_EVENT_HALT);
    machine->cycles += 4;
    lift_machine_count_access(machine);
    /* The HALT at which the original was stopped: stop here as well. */
    if (machine->status == LIFT_PARKED && machine->escape) longjmp(*machine->escape, 1);
}

void lift_stop(GBContext* ctx) {
    LiftMachine* machine = machine_of(ctx);
    lift_machine_log_event(machine, LIFT_EVENT_STOP);
    machine->cycles += 4;
    lift_machine_count_access(machine);
}

int lift_call(GBContext* ctx, uint16_t address, uint16_t return_address) {
    lift_push16(ctx, return_address);
    return lift_call_pushed(ctx, address, return_address);
}

int lift_call_pushed(GBContext* ctx, uint16_t address, uint16_t return_address) {
    LiftMachine* machine = machine_of(ctx);
    const uint16_t frame = (uint16_t)(ctx->sp + 2u);
    ctx->pc = address;
    if (lift_cpu_run_until(machine, ctx, return_address, frame) != LIFT_OK && machine->escape) {
        longjmp(*machine->escape, 1);
    }
    /* Not back at the call site: the callee returned to our caller instead. */
    return !(ctx->pc == return_address && ctx->sp == frame);
}

void lift_ret(GBContext* ctx) {
    /* Usually this lands on the caller's return address and runs nothing. */
    lift_jump(ctx, lift_pop16(ctx));
}

void lift_reti(GBContext* ctx) {
    ctx->ime = 1;
    lift_ret(ctx);
}

void lift_jump(GBContext* ctx, uint16_t address) {
    LiftMachine* machine = machine_of(ctx);
    ctx->pc = address;
    if (lift_cpu_run_until(machine, ctx, machine->return_pc, machine->return_sp) != LIFT_OK &&
        machine->escape) {
        longjmp(*machine->escape, 1);
    }
}

/* ---- Random states ----------------------------------------------------- */

static uint64_t next_random(uint64_t* state) {
    uint64_t value = (*state += 0x9E3779B97F4A7C15ull);
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

/* Bytes that matter to code more often than a uniform draw offers them. */
static uint8_t interesting_byte(uint64_t* rng) {
    const uint64_t r = next_random(rng);
    switch (r & 7u) {
        case 0: return 0x00;
        case 1: return 0x01;
        case 2: return 0xFF;
        case 3: return 0x80;
        case 4: return (uint8_t)((r >> 8) & 0x0Fu);
        default: return (uint8_t)(r >> 8);
    }
}

static void fill_random(uint8_t* bytes, size_t size, uint64_t* rng) {
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        const uint64_t value = next_random(rng);
        memcpy(bytes + i, &value, 8);
    }
    for (; i < size; ++i) bytes[i] = (uint8_t)next_random(rng);
}

static uint16_t ram_pointer(uint64_t* rng) {
    const uint64_t r = next_random(rng);
    switch (r & 3u) {
        case 0: return (uint16_t)(0xFF80u + ((r >> 8) % 0x70u));
        case 1: return (uint16_t)(0x8000u + ((r >> 8) % 0x1F00u));
        default: return (uint16_t)(0xC000u + ((r >> 8) % 0x1F00u));
    }
}

/* A state went wrong over the bank that was mapped. If the bank number came
 * out of memory, the bytes that held it are the game's own record of the
 * mapped bank, and from now on every state keeps them true. One state does
 * not show which bytes those are: a made-up state is full of bytes that
 * happen to hold any given value, and the number may have come from a stray
 * write and not from memory at all. A byte that held the mapped bank in two
 * such states, with two different banks, is taken. */
static void learn_bank_variable(ReadList* reads, const LiftMachine* machine) {
    const uint8_t value = machine->last_bank_value;
    if (reads->bank_candidates && value != reads->bank_candidate_value) {
        for (unsigned i = 0; i < reads->bank_candidates && reads->bank_variables < 8u; ++i) {
            const uint16_t address = reads->bank_candidate[i];
            if (lift_machine_peek(machine, address) == value) {
                reads->bank_variable[reads->bank_variables++] = address;
            }
        }
        reads->bank_candidates = 0;
    }
    if (reads->bank_candidates) return; /* the same bank again says nothing new */
    reads->bank_candidate_value = value;
    for (unsigned i = 0; i < reads->count && reads->bank_candidates < 64u; ++i) {
        const uint16_t address = reads->address[i];
        if (address >= 0xFF00u && address < 0xFF80u) continue;
        if (lift_machine_peek(machine, address) != value) continue;
        int known = 0;
        for (unsigned k = 0; k < reads->bank_variables; ++k) {
            known |= reads->bank_variable[k] == address;
        }
        if (!known) reads->bank_candidate[reads->bank_candidates++] = address;
    }
}

static void make_state(LiftMachine* machine,
                       GBContext* regs,
                       const Image* image,
                       const LiftCandidate* candidate,
                       const ReadList* reads,
                       unsigned trial,
                       uint64_t* rng) {
    machine->seed = next_random(rng);

    if (!image->present && trial % 4u != 0) {
        fill_random(&machine->wram[0][0], sizeof(machine->wram), rng);
        fill_random(&machine->vram[0][0], sizeof(machine->vram), rng);
        fill_random(machine->oam, sizeof(machine->oam), rng);
        fill_random(machine->hram, sizeof(machine->hram), rng);
        fill_random(machine->eram, machine->eram_size, rng);
        fill_random(machine->io, sizeof(machine->io), rng);
        machine->ie = (uint8_t)next_random(rng);
    }
    if (trial > 0) {
        /* Aim at what the original reads: change some of those bytes. For a
         * hardware register that is the value last written to it. */
        const unsigned seen = reads->count;
        if (seen > 0) {
            const unsigned wanted =
                (next_random(rng) & 7u) == 0 ? seen : 1u + (unsigned)(next_random(rng) % 8u);
            for (unsigned i = 0; i < seen; ++i) {
                if (next_random(rng) % seen < wanted) {
                    lift_machine_poke(machine, reads->address[i], interesting_byte(rng));
                }
            }
        }
    }

    for (unsigned i = 0; i < reads->bank_variables; ++i) {
        lift_machine_poke(machine, reads->bank_variable[i], (uint8_t)candidate->bank);
    }

    memset(regs, 0, sizeof(*regs));
    regs->a = interesting_byte(rng);
    regs->b = interesting_byte(rng);
    regs->c = interesting_byte(rng);
    regs->d = interesting_byte(rng);
    regs->e = interesting_byte(rng);
    regs->h = interesting_byte(rng);
    regs->l = interesting_byte(rng);
    if (next_random(rng) & 1u) regs->bc = ram_pointer(rng);
    if (next_random(rng) & 1u) regs->de = ram_pointer(rng);
    if (next_random(rng) & 1u) regs->hl = ram_pointer(rng);
    const uint64_t bits = next_random(rng);
    regs->f_z = (uint8_t)(bits & 1u);
    regs->f_n = (uint8_t)((bits >> 1) & 1u);
    regs->f_h = (uint8_t)((bits >> 2) & 1u);
    regs->f_c = (uint8_t)((bits >> 3) & 1u);
    regs->ime = (uint8_t)((bits >> 4) & 1u);

    uint16_t sp = 0xDFF0u;
    if (image->present && image->regs.sp >= 0xC002u && image->regs.sp <= 0xFFFEu) sp = image->regs.sp;
    if (((bits >> 5) & 3u) == 0) sp = (uint16_t)(sp - 2u * ((bits >> 8) & 15u));

    uint32_t bank = candidate->bank;
    if (bank == 0) {
        if (image->present && image->rom_bank != 0) {
            bank = image->rom_bank;
        } else if (machine->rom_bank_count > 2 && (trial & 1u)) {
            bank = 1u + (uint32_t)(next_random(rng) % (machine->rom_bank_count - 1u));
        } else {
            bank = 1;
        }
    }
    lift_machine_select_rom_bank(machine, bank);

    /* The caller's return address: somewhere no code lives. */
    sp = (uint16_t)(sp - 2u);
    lift_machine_poke(machine, sp, (uint8_t)(RETURN_SENTINEL & 0xFFu));
    lift_machine_poke(machine, (uint16_t)(sp + 1u), (uint8_t)(RETURN_SENTINEL >> 8));
    regs->sp = sp;
    regs->pc = candidate->address;
}

/* ---- States a function was entered in --------------------------------- */

#define ENTRY_PAGE 256u
#define ENTRY_TAIL (0xA0u + 0x7Fu + 0x80u + 1u)

#ifdef _MSC_VER
#define seek64(file, offset) _fseeki64((file), (__int64)(offset), SEEK_SET)
#else
#define seek64(file, offset) fseeko((file), (off_t)(offset), SEEK_SET)
#endif

/* The states kept of a function, which lie together in the file. */
static const EntryState* entries_of(const Entries* entries, const LiftCandidate* candidate,
                                    unsigned* count) {
    const uint32_t wanted = ((uint32_t)candidate->bank << 16) | candidate->address;
    uint32_t low = 0;
    uint32_t high = entries->state_count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2u;
        const EntryState* state = &entries->states[middle];
        if ((((uint32_t)state->bank << 16) | state->address) < wanted) low = middle + 1u;
        else high = middle;
    }
    uint32_t end = low;
    while (end < entries->state_count && entries->states[end].bank == candidate->bank &&
           entries->states[end].address == candidate->address) {
        ++end;
    }
    *count = end - low;
    return *count ? &entries->states[low] : NULL;
}

/* The memory of `count` states from `first` on, each put together from its
 * pages. Null if the file cannot be read. */
static uint8_t* entry_bodies(const Entries* entries, const EntryState* first, unsigned count) {
    uint8_t* bodies = (uint8_t*)malloc((size_t)count * entries->body_bytes);
    uint8_t* ids = (uint8_t*)malloc((size_t)entries->pages_per_state * 4u);
    int good = bodies && ids;
    const uint64_t index = (uint64_t)(first - entries->states);
    for (unsigned i = 0; good && i < count; ++i) {
        good = seek64(entries->file, entries->ids_at + (index + i) * entries->pages_per_state * 4u) == 0 &&
               fread(ids, 4, entries->pages_per_state, entries->file) == entries->pages_per_state;
        uint8_t* body = bodies + (size_t)i * entries->body_bytes;
        for (uint32_t page = 0; good && page < entries->pages_per_state; ++page) {
            const uint32_t id = (uint32_t)ids[page * 4u] | ((uint32_t)ids[page * 4u + 1u] << 8) |
                                ((uint32_t)ids[page * 4u + 2u] << 16) | ((uint32_t)ids[page * 4u + 3u] << 24);
            good = id < entries->page_count &&
                   seek64(entries->file, entries->pages_at + (uint64_t)id * ENTRY_PAGE) == 0 &&
                   fread(body + (size_t)page * ENTRY_PAGE, 1, ENTRY_PAGE, entries->file) == ENTRY_PAGE;
        }
    }
    free(ids);
    if (!good) {
        free(bodies);
        return NULL;
    }
    return bodies;
}

/* A state from one the function was entered in. Round 0 is that state as it
 * was: its registers, its memory, what was mapped, and its own return
 * address on the stack. Later rounds change some of the bytes the original
 * was seen to read, as states from an image do, and keep the registers. */
static void entry_state(LiftMachine* machine,
                        GBContext* regs,
                        const Entries* entries,
                        const EntryState* state,
                        const uint8_t* body,
                        const LiftCandidate* candidate,
                        const ReadList* reads,
                        unsigned round,
                        uint64_t* rng) {
    machine->seed = next_random(rng);

    const size_t eram_bytes = ((size_t)entries->eram_size + ENTRY_PAGE - 1u) / ENTRY_PAGE * ENTRY_PAGE;
    memcpy(machine->eram, body,
           entries->eram_size < machine->eram_size ? entries->eram_size : machine->eram_size);
    const uint8_t* p = body + eram_bytes;
    memcpy(machine->wram, p, 0x8000u);
    p += 0x8000u;
    memcpy(machine->vram, p, 0x4000u);
    p += 0x4000u;
    memcpy(machine->oam, p, 0xA0u);
    p += 0xA0u;
    memcpy(machine->hram, p, 0x7Fu);
    p += 0x7Fu;
    memcpy(machine->io, p, 0x80u);
    p += 0x80u;
    machine->ie = *p;

    machine->ram_enable = state->ram_enabled;
    machine->ram_bank = state->ram_bank;
    machine->bank_high = state->ram_bank;
    machine->wram_bank = (state->wram_bank & 7u) ? (state->wram_bank & 7u) : 1u;
    machine->vram_bank = state->vram_bank & 1u;

    if (round > 0) {
        const unsigned seen = reads->count;
        if (seen > 0) {
            const unsigned wanted =
                (next_random(rng) & 7u) == 0 ? seen : 1u + (unsigned)(next_random(rng) % 8u);
            for (unsigned i = 0; i < seen; ++i) {
                if (next_random(rng) % seen < wanted) {
                    lift_machine_poke(machine, reads->address[i], interesting_byte(rng));
                }
            }
        }
        for (unsigned i = 0; i < reads->bank_variables; ++i) {
            lift_machine_poke(machine, reads->bank_variable[i], (uint8_t)state->rom_bank);
        }
    }

    memset(regs, 0, sizeof(*regs));
    regs->a = state->a;
    regs->b = state->b;
    regs->c = state->c;
    regs->d = state->d;
    regs->e = state->e;
    regs->h = state->h;
    regs->l = state->l;
    regs->f_z = (uint8_t)((state->f >> 7) & 1u);
    regs->f_n = (uint8_t)((state->f >> 6) & 1u);
    regs->f_h = (uint8_t)((state->f >> 5) & 1u);
    regs->f_c = (uint8_t)((state->f >> 4) & 1u);
    regs->ime = state->ime;

    lift_machine_select_rom_bank(machine, state->rom_bank);

    /* Where it goes back to is the game's, whatever a later round changed. */
    lift_machine_poke(machine, state->sp, (uint8_t)(state->returns_to & 0xFFu));
    lift_machine_poke(machine, (uint16_t)(state->sp + 1u), (uint8_t)(state->returns_to >> 8));
    regs->sp = state->sp;
    regs->pc = candidate->address;
}

/* ---- Comparison -------------------------------------------------------- */

/* Stretches of RAM not compared: bytes that are registers in all but name
 * (a helper parks A in one for a moment, a far call keeps its caller's
 * registers in six), which source written by hand has no reason to fill. */
#define MAX_IGNORED_RAM 64
static struct { uint16_t first, last; } g_ignored_ram[MAX_IGNORED_RAM];
static unsigned g_ignored_ram_count;

/* "CF12" or "CF12-CF17": addresses in RAM, hexadecimal, four digits each. */
static int parse_ignored_ram(const char* text, size_t length) {
    unsigned first = 0, last = 0;
    int used = 0;
    if (length == 4 && sscanf(text, "%4x%n", &first, &used) == 1 && used == 4) {
        last = first;
    } else if (!(length == 9 && sscanf(text, "%4x-%4x%n", &first, &last, &used) == 2 && used == 9)) {
        return 0;
    }
    if (first < 0x8000u || last < first) return 0;
    if (g_ignored_ram_count >= MAX_IGNORED_RAM) {
        fprintf(stderr, "lift_check: more than %d stretches of RAM in an ignore list\n", MAX_IGNORED_RAM);
        exit(2);
    }
    g_ignored_ram[g_ignored_ram_count].first = (uint16_t)first;
    g_ignored_ram[g_ignored_ram_count].last = (uint16_t)last;
    g_ignored_ram_count++;
    return 1;
}

static unsigned parse_ignore(const char* text) {
    static const struct { const char* name; unsigned bit; } kNames[] = {
        {"a", IGNORE_A}, {"b", IGNORE_B}, {"c", IGNORE_C}, {"d", IGNORE_D},
        {"e", IGNORE_E}, {"h", IGNORE_H}, {"l", IGNORE_L},
        {"fz", IGNORE_FZ}, {"fn", IGNORE_FN}, {"fh", IGNORE_FH}, {"fc", IGNORE_FC},
        {"ime", IGNORE_IME},
    };
    unsigned mask = 0;
    if (!text) return 0;
    while (*text) {
        size_t length = strcspn(text, ", ");
        int known = 0;
        for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
            if (strlen(kNames[i].name) == length && strncmp(kNames[i].name, text, length) == 0) {
                mask |= kNames[i].bit;
                known = 1;
            }
        }
        if (!known && length > 0) known = parse_ignored_ram(text, length);
        if (!known && length > 0) {
            fprintf(stderr, "lift_check: unknown name in ignore list: %.*s\n", (int)length, text);
            exit(2);
        }
        text += length;
        while (*text == ',' || *text == ' ') ++text;
    }
    return mask;
}

static void differ(Differences* out, const char* format, ...) {
    out->total++;
    if (out->count >= MAX_DIFFERENCES) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(out->text[out->count], sizeof(out->text[0]), format, arguments);
    va_end(arguments);
    out->count++;
}

static void compare_region(Differences* out,
                           const char* name,
                           const uint8_t* original,
                           const uint8_t* lifted,
                           size_t size,
                           unsigned base,
                           unsigned bank,
                           int banked) {
    if (memcmp(original, lifted, size) == 0) return;
    unsigned shown = 0;
    for (size_t i = 0; i < size; ++i) {
        if (original[i] == lifted[i]) continue;
        if (shown++ >= 3) {
            out->total++;
            continue;
        }
        if (banked) {
            differ(out, "%s %04X (bank %u): original %02X, lifted %02X", name,
                   (unsigned)(base + i), bank, original[i], lifted[i]);
        } else {
            differ(out, "%s %04X: original %02X, lifted %02X", name, (unsigned)(base + i),
                   original[i], lifted[i]);
        }
    }
}

static void describe_entry(char* text, size_t size, uint32_t address, uint8_t value) {
    if (address == LIFT_EVENT_HALT) snprintf(text, size, "HALT");
    else if (address == LIFT_EVENT_STOP) snprintf(text, size, "STOP");
    else snprintf(text, size, "%04X=%02X", (unsigned)address, value);
}

static void compare(Differences* out,
                    LiftMachine* original,
                    const GBContext* a,
                    LiftMachine* lifted,
                    const GBContext* b,
                    unsigned ignore,
                    int strict_stack) {
    out->count = 0;
    out->total = 0;

    if (original->status == LIFT_PARKED) {
        /* Stopped at a wait, not at the return. What is compared is what a
         * handler would find there: memory, the stack pointer, whether
         * interrupts are on, and what the hardware was told. The registers
         * are the code's own business until it returns. */
        if (lifted->status == LIFT_OK) {
            differ(out, "returned where the original was still waiting, at its HALT #%u",
                   (unsigned)original->halts + 1u);
        } else if (lifted->status != LIFT_PARKED) {
            differ(out, "lifted code stopped: %s", lift_status_name(lifted->status));
        }
        ignore |= IGNORE_A | IGNORE_B | IGNORE_C | IGNORE_D | IGNORE_E | IGNORE_H | IGNORE_L |
                  IGNORE_FZ | IGNORE_FN | IGNORE_FH | IGNORE_FC;
    } else {
        if (lifted->status != LIFT_OK) {
            differ(out, "lifted code stopped: %s", lift_status_name(lifted->status));
        }
        if (b->pc != a->pc) {
            differ(out, "did not return to the caller: pc %04X (end with lift_ret or lift_jump)", b->pc);
        }
    }
    if (b->sp != a->sp) differ(out, "sp: original %04X, lifted %04X", a->sp, b->sp);

#define COMPARE_REGISTER(field, bit, label)                                             \
    if (!(ignore & (bit)) && a->field != b->field) {                                    \
        differ(out, "%s: original %02X, lifted %02X", label, a->field, b->field);      \
    }
    COMPARE_REGISTER(a, IGNORE_A, "a")
    COMPARE_REGISTER(b, IGNORE_B, "b")
    COMPARE_REGISTER(c, IGNORE_C, "c")
    COMPARE_REGISTER(d, IGNORE_D, "d")
    COMPARE_REGISTER(e, IGNORE_E, "e")
    COMPARE_REGISTER(h, IGNORE_H, "h")
    COMPARE_REGISTER(l, IGNORE_L, "l")
#undef COMPARE_REGISTER
#define COMPARE_FLAG(field, bit, label)                                                 \
    if (!(ignore & (bit)) && (a->field != 0) != (b->field != 0)) {                      \
        differ(out, "flag %s: original %u, lifted %u", label, a->field != 0, b->field != 0); \
    }
    COMPARE_FLAG(f_z, IGNORE_FZ, "z")
    COMPARE_FLAG(f_n, IGNORE_FN, "n")
    COMPARE_FLAG(f_h, IGNORE_FH, "h")
    COMPARE_FLAG(f_c, IGNORE_FC, "c")
    COMPARE_FLAG(ime, IGNORE_IME, "ime")
#undef COMPARE_FLAG

    /* Hardware writes, in order. */
    const uint32_t entries = original->io_log_count < lifted->io_log_count
                                 ? original->io_log_count
                                 : lifted->io_log_count;
    for (uint32_t i = 0; i < entries; ++i) {
        const LiftIoEntry* x = &original->io_log[i];
        const LiftIoEntry* y = &lifted->io_log[i];
        if (x->address == y->address && x->value == y->value) continue;
        char left[24];
        char right[24];
        describe_entry(left, sizeof(left), x->address, x->value);
        describe_entry(right, sizeof(right), y->address, y->value);
        differ(out, "hardware write #%u: original %s, lifted %s", (unsigned)i + 1u, left, right);
        break;
    }
    if (original->io_log_count != lifted->io_log_count) {
        differ(out, "hardware writes: original made %u, lifted made %u",
               (unsigned)original->io_log_count, (unsigned)lifted->io_log_count);
    }

    if (original->rom_bank != lifted->rom_bank || original->rom_bank0 != lifted->rom_bank0) {
        differ(out, "ROM bank left mapped: original %u, lifted %u", (unsigned)original->rom_bank,
               (unsigned)lifted->rom_bank);
    }
    if (original->ram_bank != lifted->ram_bank || original->ram_enable != lifted->ram_enable) {
        differ(out, "cartridge RAM mapping: original bank %u enable %u, lifted bank %u enable %u",
               (unsigned)original->ram_bank, original->ram_enable, (unsigned)lifted->ram_bank,
               lifted->ram_enable);
    }
    if (original->wram_bank != lifted->wram_bank || original->vram_bank != lifted->vram_bank) {
        differ(out, "WRAM/VRAM bank: original %u/%u, lifted %u/%u", original->wram_bank,
               original->vram_bank, lifted->wram_bank, lifted->vram_bank);
    }

    /* Below the stack pointer is scratch: pushes that were popped again.
     * Readable source has no reason to reproduce it. */
    if (!strict_stack && out->total == 0) {
        const uint16_t low = original->min_sp < lifted->min_sp ? original->min_sp : lifted->min_sp;
        if (low < a->sp && (unsigned)(a->sp - low) <= 0x400u) {
            for (uint16_t address = low; address < a->sp; ++address) {
                lift_machine_poke(lifted, address, lift_machine_peek(original, address));
            }
        }
    }

    /* RAM the function was told it need not leave as the original does. */
    for (unsigned i = 0; i < g_ignored_ram_count; ++i) {
        for (unsigned address = g_ignored_ram[i].first; address <= g_ignored_ram[i].last; ++address) {
            lift_machine_poke(lifted, (uint16_t)address, lift_machine_peek(original, (uint16_t)address));
        }
    }

    compare_region(out, "wram", original->wram[0], lifted->wram[0], 0x1000, 0xC000, 0, 0);
    for (unsigned bank = 1; bank < 8; ++bank) {
        compare_region(out, "wram", original->wram[bank], lifted->wram[bank], 0x1000, 0xD000, bank,
                       original->cgb);
    }
    compare_region(out, "hram", original->hram, lifted->hram, sizeof(original->hram), 0xFF80, 0, 0);
    compare_region(out, "vram", original->vram[0], lifted->vram[0], 0x2000, 0x8000, 0, original->cgb);
    compare_region(out, "vram", original->vram[1], lifted->vram[1], 0x2000, 0x8000, 1, 1);
    compare_region(out, "oam", original->oam, lifted->oam, sizeof(original->oam), 0xFE00, 0, 0);
    compare_region(out, "unusable", original->unusable, lifted->unusable,
                   sizeof(original->unusable), 0xFEA0, 0, 0);
    compare_region(out, "hardware register", original->io, lifted->io, sizeof(original->io),
                   0xFF00, 0, 0);
    if (original->ie != lifted->ie) {
        differ(out, "IE: original %02X, lifted %02X", original->ie, lifted->ie);
    }
    if (original->eram_size) {
        for (uint32_t bank = 0; bank * 0x2000u < original->eram_size; ++bank) {
            const uint32_t left = original->eram_size - bank * 0x2000u;
            compare_region(out, "cartridge ram", original->eram + bank * 0x2000u,
                           lifted->eram + bank * 0x2000u, left < 0x2000u ? left : 0x2000u, 0xA000,
                           bank, 1);
        }
    }
}

/* ---- What a function does with each register (--observe) --------------- *
 * Assembly does not say what a function takes and what it gives back. This
 * finds out from the original itself. A register or flag is READ if giving
 * it another value on the way in changes anything else that comes out: a
 * byte of memory, another register, what the hardware was told, whether it
 * returns at all, or how long it took. The time is part of it because a
 * function held to its signature is given another value for what it does
 * not take, and its time is compared: a flag that only decides between two
 * ways to the same end (a jump to the instruction after it) would be left
 * out, and the function reported as taking another time than the original
 * in every state. One that is only pushed and popped again, or never
 * touched, comes out as it went in whatever it held, and is not read. It is
 * KEPT if it came out as it went in from every state tried. One that came
 * out as it went in from some state and changed in another WENT THROUGH:
 * what it holds afterwards is, that time, what it held before, so whoever
 * reads it afterwards has it from the caller. It is evidence
 * from the states there were, like a match: the signature made of it is
 * then held against every state a later check tries, and against the game. */

#define ITEM_COUNT 11u
#define OBSERVE_STATES 160u /* unless --observe-states says otherwise */

typedef struct Observed {
    unsigned states;
    unsigned reads;   /* a bit an item: IGNORE_A ... IGNORE_FC */
    unsigned changed; /* came out other than it went in, in some state */
    unsigned through; /* came out as it went in, whatever it held, in some state */
} Observed;

static uint8_t* item_of(GBContext* ctx, unsigned item) {
    switch (item) {
        case 0: return &ctx->a;
        case 1: return &ctx->b;
        case 2: return &ctx->c;
        case 3: return &ctx->d;
        case 4: return &ctx->e;
        case 5: return &ctx->h;
        case 6: return &ctx->l;
        case 7: return &ctx->f_z;
        case 8: return &ctx->f_n;
        case 9: return &ctx->f_h;
        default: return &ctx->f_c;
    }
}

static uint8_t item_value(const GBContext* ctx, unsigned item) {
    const uint8_t value = *item_of((GBContext*)ctx, item);
    return item >= 7u ? (uint8_t)(value != 0) : value;
}

/* The same item with a value it did not have, another kind each turn: code
 * mostly asks of a register whether it is zero, whether its top bit is set,
 * whether it is odd, or what it is. */
static uint8_t item_other(uint8_t value, unsigned item, unsigned turn) {
    if (item >= 7u) return (uint8_t)!value;
    switch (turn & 3u) {
        case 0: return (uint8_t)(value ^ 0x5Au);
        case 1: return (uint8_t)(value ? 0u : 0x01u);
        case 2: return (uint8_t)(value ^ 0x80u);
        default: return (uint8_t)(value ^ 0x01u);
    }
}

/* The original has just returned from `entry`, and g_lifted is still the
 * state it started in. `trial` picks the other value, as it does where a
 * function held to a signature has what it does not take changed: what is
 * found here from a state is what holds there from the same state. */
static void observe(const GBContext* entry, const Options* options, Observed* seen, unsigned trial) {
    seen->states++;
    for (unsigned item = 0; item < ITEM_COUNT; ++item) {
        const unsigned bit = 1u << item;
        const uint8_t went_in = item_value(entry, item);
        const int kept = item_value(&g_original_ctx, item) == went_in;
        if (!kept) seen->changed |= bit;
        if (seen->reads & bit) continue; /* known already; save the run */

        memcpy(&g_probe, &g_lifted, sizeof(g_lifted));
        g_probe_ctx = *entry;
        g_probe_ctx.machine = &g_probe;
        const uint8_t other = item_other(went_in, item, trial + item);
        *item_of(&g_probe_ctx, item) = other;
        g_probe.step_limit = options->max_steps;
        const LiftStatus status =
            lift_cpu_run_until(&g_probe, &g_probe_ctx, g_original.return_pc, g_original.return_sp);
        /* It went through untouched both times: its own value coming out
         * differently is not the function reading it. */
        const unsigned through =
            status == LIFT_OK && kept && item_value(&g_probe_ctx, item) == other ? bit : 0u;
        Differences differences;
        compare(&differences, &g_original, &g_original_ctx, &g_probe, &g_probe_ctx, through, 0);
        /* compare() does not look at the time: the same end by another way
         * is the function reading it all the same. */
        if (differences.total != 0 || g_probe.cycles != g_original.cycles) seen->reads |= bit;
        else if (through) seen->through |= bit;
    }
}

/* ---- A signature held against the source -------------------------------- */

static void scramble_untaken(const LiftCandidate* candidate, GBContext* ctx, unsigned trial) {
    for (unsigned item = 0; item < ITEM_COUNT; ++item) {
        if (candidate->takes & (1u << item)) continue;
        *item_of(ctx, item) = item_other(item_value(ctx, item), item, trial + item);
    }
}

static void settle_result(const LiftCandidate* candidate, const GBContext* entry, GBContext* ctx) {
    for (unsigned item = 0; item < ITEM_COUNT; ++item) {
        const unsigned bit = 1u << item;
        if (candidate->keeps & bit) *item_of(ctx, item) = item_value(entry, item);
        else if (!(candidate->gives & bit)) g_undefined |= bit;
    }
}

/* ---- Files ------------------------------------------------------------- */

static uint8_t* read_file(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        fclose(file);
        return NULL;
    }
    uint8_t* data = (uint8_t*)malloc((size_t)length);
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * A memory image (written by tools/gb_lift.py from a save state):
 *   "GBLIFTIM", u32 version (1)
 *   u16 af, bc, de, hl, sp, pc
 *   u16 rom_bank, u8 ram_bank, u8 wram_bank, u8 vram_bank, u8 ime, u8[2] zero
 *   u32 eram_size, eram bytes
 *   wram 0x8000, vram 0x4000, oam 0xA0, hram 0x7F, io 0x80, ie 1
 * Numbers are little-endian.
 */
static int load_image(const char* path, LiftMachine* machine, Image* image) {
    size_t size = 0;
    uint8_t* data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "lift_check: cannot read %s\n", path);
        return 0;
    }
    const size_t header = 8 + 4 + 12 + 8 + 4;
    if (size < header || memcmp(data, "GBLIFTIM", 8) != 0 || le32(data + 8) != 1u) {
        fprintf(stderr, "lift_check: %s is not a version 1 memory image\n", path);
        free(data);
        return 0;
    }
    const uint8_t* p = data + 12;
    image->present = 1;
    image->regs.af = le16(p);
    image->regs.bc = le16(p + 2);
    image->regs.de = le16(p + 4);
    image->regs.hl = le16(p + 6);
    image->regs.sp = le16(p + 8);
    image->regs.pc = le16(p + 10);
    image->rom_bank = le16(p + 12);
    machine->ram_bank = p[14];
    machine->bank_high = p[14];
    machine->wram_bank = p[15] ? p[15] : 1;
    machine->vram_bank = p[16] & 1u;
    const uint32_t eram_size = le32(p + 20);
    const size_t body = (size_t)eram_size + 0x8000u + 0x4000u + 0xA0u + 0x7Fu + 0x80u + 1u;
    if (size != header + body) {
        fprintf(stderr, "lift_check: %s has the wrong size\n", path);
        free(data);
        return 0;
    }
    p = data + header;
    memcpy(machine->eram, p, eram_size < LIFT_ERAM_MAX ? eram_size : LIFT_ERAM_MAX);
    p += eram_size;
    memcpy(machine->wram, p, 0x8000u);
    p += 0x8000u;
    memcpy(machine->vram, p, 0x4000u);
    p += 0x4000u;
    memcpy(machine->oam, p, 0xA0u);
    p += 0xA0u;
    memcpy(machine->hram, p, 0x7Fu);
    p += 0x7Fu;
    memcpy(machine->io, p, 0x80u);
    p += 0x80u;
    machine->ie = *p;
    free(data);
    return 1;
}

/* The states of a game played with --entry-states. The states' records are
 * read now; their memory is read a function at a time (entry_bodies). */
static int load_entries(const char* path, Entries* entries) {
    FILE* file = fopen(path, "rb");
    uint8_t header[48];
    if (!file || fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "GBLIFTEN", 8) != 0 || le32(header + 8) != 1u || le32(header + 12) != ENTRY_PAGE) {
        fprintf(stderr, "lift_check: %s is not a version 1 file of entry states\n", path);
        if (file) fclose(file);
        return 0;
    }
    entries->eram_size = le32(header + 16);
    entries->pages_per_state = le32(header + 20);
    entries->state_count = le32(header + 24);
    entries->page_count = le32(header + 28);
    const uint32_t function_count = le32(header + 32);
    const uint32_t expected = (entries->eram_size + ENTRY_PAGE - 1u) / ENTRY_PAGE + 0x8000u / ENTRY_PAGE +
                              0x4000u / ENTRY_PAGE + (ENTRY_TAIL + ENTRY_PAGE - 1u) / ENTRY_PAGE;
    if (entries->pages_per_state != expected) {
        fprintf(stderr, "lift_check: %s does not have the memory a state is made of\n", path);
        fclose(file);
        return 0;
    }
    entries->body_bytes = (size_t)entries->pages_per_state * ENTRY_PAGE;
    const uint64_t states_at = sizeof(header) + (uint64_t)function_count * 12u;
    entries->ids_at = states_at + (uint64_t)entries->state_count * 40u;
    entries->pages_at = entries->ids_at + (uint64_t)entries->state_count * entries->pages_per_state * 4u;
    entries->states = (EntryState*)calloc(entries->state_count ? entries->state_count : 1u, sizeof(EntryState));
    int good = entries->states != NULL && seek64(file, states_at) == 0;
    for (uint32_t i = 0; good && i < entries->state_count; ++i) {
        uint8_t record[40];
        good = fread(record, 1, sizeof(record), file) == sizeof(record);
        EntryState* state = &entries->states[i];
        state->bank = le16(record);
        state->address = le16(record + 2);
        state->rom_bank = le16(record + 4);
        state->a = record[6];
        state->f = record[7];
        state->b = record[8];
        state->c = record[9];
        state->d = record[10];
        state->e = record[11];
        state->h = record[12];
        state->l = record[13];
        state->sp = le16(record + 14);
        state->returns_to = le16(record + 16);
        state->ime = record[18];
        state->ram_bank = record[19];
        state->ram_enabled = record[20];
        state->wram_bank = record[21];
        state->vram_bank = record[22];
        state->arrival = le32(record + 24);
    }
    if (!good) {
        fprintf(stderr, "lift_check: %s ends before its states do\n", path);
        fclose(file);
        return 0;
    }
    entries->file = file;
    entries->present = 1;
    return 1;
}

static void json_text(FILE* out, const char* text) {
    fputc('"', out);
    for (; *text; ++text) {
        const unsigned char ch = (unsigned char)*text;
        if (ch == '"' || ch == '\\') {
            fputc('\\', out);
            fputc(ch, out);
        } else if (ch < 0x20) {
            fprintf(out, "\\u%04x", ch);
        } else {
            fputc(ch, out);
        }
    }
    fputc('"', out);
}

/* ---- One candidate ----------------------------------------------------- */

static int check_candidate(const Options* options,
                           const Image* image,
                           const Entries* entries,
                           const LiftCandidate* candidate,
                           FILE* out) {
    uint8_t* read_seen = (uint8_t*)calloc(0x10000u, 1);
    uint8_t* executed = (uint8_t*)calloc(g_base.rom_size, 1);
    ReadList* reads = (ReadList*)calloc(1, sizeof(ReadList));
    if (!read_seen || !executed || !reads) {
        fprintf(stderr, "lift_check: out of memory\n");
        exit(2);
    }
    g_ignored_ram_count = 0;
    const unsigned ignore = parse_ignore(candidate->ignore) | parse_ignore(options->ignore);

    unsigned passed = 0;
    unsigned mismatched = 0;
    unsigned skipped[LIFT_STATUS_COUNT] = {0};
    unsigned cycles_equal = 0;
    unsigned cycles_differ = 0;
    unsigned cycles_untracked = 0;
    unsigned cycles_apart = 0;
    unsigned ran_ram_code = 0;
    unsigned reported = 0;
    unsigned gave_up_after = 0;
    unsigned waited = 0;
    unsigned stopped_waiting = 0;
    uint16_t stack_low = 0xFFFF;
    uint16_t stack_high = 0;
    Observed seen = {0, 0, 0, 0};

    /* With a file of entry states, a function is tried from the states kept
     * of it and from nothing else: first each as it was, then each again
     * with some of what the function reads changed. One no state was kept
     * of is not tried. */
    unsigned own_count = 0;
    const EntryState* own = entries->present ? entries_of(entries, candidate, &own_count) : NULL;
    uint8_t* bodies = own_count ? entry_bodies(entries, own, own_count) : NULL;
    if (own_count && !bodies) {
        fprintf(stderr, "lift_check: cannot read the states of %02X:%04X\n", candidate->bank,
                candidate->address);
        exit(2);
    }
    unsigned trials = options->trials;
    if (entries->present) trials = own_count ? (trials < own_count ? own_count : trials) : 0u;
    unsigned as_recorded[3] = {0, 0, 0}; /* of the states as they were: matched, mismatched, thrown away */

    fprintf(out, "    {\"function\": \"gbfn:v1:%04x:%04x\", \"name\": ", candidate->bank,
            candidate->address);
    json_text(out, candidate->name ? candidate->name : "");
    fprintf(out, ", \"mismatches\": [");

    for (unsigned trial = 0; trial < trials; ++trial) {
        /* The states are the function's own: where it stands in the list
         * has no part in them, so it is tried from the same ones whatever
         * else the export holds. */
        uint64_t rng = options->seed ^ ((uint64_t)candidate->bank << 48) ^
                       ((uint64_t)candidate->address << 32) ^ trial;
        GBContext entry;

        memcpy(&g_original, &g_base, sizeof(g_base));
        uint16_t return_pc = RETURN_SENTINEL;
        const int recorded = own_count && trial < own_count; /* a state as the game had it */
        if (own_count) {
            const unsigned which = trial % own_count;
            entry_state(&g_original, &entry, entries, &own[which], bodies + (size_t)which * entries->body_bytes,
                        candidate, reads, trial / own_count, &rng);
            return_pc = own[which].returns_to;
            g_original.return_is_code = 1;
        } else {
            make_state(&g_original, &entry, image, candidate, reads, trial, &rng);
        }
        g_original.step_limit = options->max_steps;
        g_original.return_pc = return_pc;
        g_original.return_sp = (uint16_t)(entry.sp + 2u);
        /* The original runs first and decides what each HALT changes; the
         * lifted code is shown the same changes at the same HALTs. */
        lift_machine_start_waits(&g_original, &g_wakes, 1);
        memcpy(&g_lifted, &g_original, sizeof(g_original));
        lift_machine_start_waits(&g_lifted, &g_wakes, 0);
        if (candidate->bank != 0) {
            /* It lives in the switchable area: watch for another bank there. */
            g_original.own_bank = candidate->bank;
            g_original.own_low = (uint16_t)(0x4000u + (candidate->cover_low & 0x3FFFu));
            g_original.own_high = (uint16_t)(0x4000u + (candidate->cover_high & 0x3FFFu));
        }
        g_original.frame_top = entry.sp;
        const unsigned entry_bank = (unsigned)g_original.rom_bank;
        if (entry.sp < stack_low) stack_low = entry.sp;
        if ((uint16_t)(entry.sp + 1u) > stack_high) stack_high = (uint16_t)(entry.sp + 1u);

        g_original.read_seen = read_seen;
        g_original.read_list = reads->address;
        g_original.read_count = &reads->count;
        g_original.executed = executed;
        g_original_ctx = entry;
        g_original_ctx.machine = &g_original;
        const LiftStatus status = lift_cpu_run_until(&g_original, &g_original_ctx, return_pc,
                                                     g_original.return_sp);
        if (g_original.min_sp < stack_low) stack_low = g_original.min_sp;
        if (status == LIFT_FOREIGN_BANK) learn_bank_variable(reads, &g_original);
        /* Stopped at a wait with the budget nearly spent: the lifted code is
         * run to the same HALT and the two are compared there. */
        const int parked = status == LIFT_PARKED;
        if (status != LIFT_OK && !parked) {
            skipped[status]++;
            if (recorded) as_recorded[2]++;
            /* Code that returns from no state at all is not something this
             * can check; do not spend the whole budget finding that out.
             * Give it longer once a state has failed over the bank that was
             * mapped: the bank variable is being learnt, and a state that
             * points a write at the mapper is bad luck, not bad code. And
             * give it longer where something is known to call it: that is
             * code, and what stops it is mostly a pointer out of made-up
             * memory aiming a copy at the cartridge. */
            unsigned patience =
                candidate->called || skipped[LIFT_FOREIGN_BANK] + skipped[LIFT_BANK_PULLED] != 0
                    ? 8u * GIVE_UP_AFTER
                    : GIVE_UP_AFTER;
            /* Every state the game had it in is tried before it is given up on. */
            if (patience < own_count) patience = own_count;
            if (passed + mismatched == 0 && trial + 1 >= patience) {
                gave_up_after = trial + 1;
                break;
            }
            continue;
        }
        if (g_original.executed_ram) ran_ram_code++;
        /* Not from a state in which it used what it pushed as data: what
         * comes out of that is the pointer's doing. */
        if (options->observe && !parked && !g_original.frame_used &&
            (options->observe_states == 0 || seen.states < options->observe_states)) {
            observe(&entry, options, &seen, trial);
        }

        g_lifted_ctx = entry;
        g_lifted_ctx.machine = &g_lifted;
        /* Held to its signature, but not from a state in which the original
         * used what it pushed as data: a fill that runs over its own saved
         * registers does not keep them, and no line says otherwise. There
         * the two are compared as they are. */
        const int held = candidate->held && !g_original.frame_used;
        if (held) scramble_untaken(candidate, &g_lifted_ctx, trial);
        g_lifted.escape = &g_escape;
        /* The lifted code gets several times the work the original needed. */
        g_lifted.bus_limit = (g_original.bus_accesses + g_original.steps) * 8u + 100000u;
        if (parked) {
            g_lifted.park_set = 1;
            g_lifted.park_halt = g_original.halts;
        }
        g_undefined = 0;
        g_own_stack = 0;
        if (setjmp(g_escape) == 0) {
            candidate->function(&g_lifted_ctx);
        } else if (g_lifted.status == LIFT_OK) {
            g_lifted.status = LIFT_TIMEOUT;
        }
        g_lifted.escape = NULL;
        /* Back with the caller: what it keeps is as it came, and what it
         * does not give is nobody's. */
        if (held && !parked && g_lifted.status == LIFT_OK && g_lifted_ctx.pc == return_pc &&
            g_lifted_ctx.sp == g_original.return_sp) {
            settle_result(candidate, &entry, &g_lifted_ctx);
        }
        if (held && parked && g_lifted_ctx.sp == g_original_ctx.sp) {
            /* Stopped at a wait, with what it has pushed still on the stack:
             * the registers it saved to put back, which are the ones it does
             * not take, and were given another value on the way in. */
            for (unsigned address = g_original_ctx.sp; address < entry.sp; ++address) {
                lift_machine_poke(&g_lifted, (uint16_t)address, lift_machine_peek(&g_original, (uint16_t)address));
            }
        }
        if (g_own_stack && parked && g_lifted.status == LIFT_PARKED) {
            /* Stopped at a wait, in a function that keeps its own stack: how
             * far down it is and what it has there are its own business. A
             * handler that came in here would push and pop, and look at
             * neither. Below where the function was entered the two are
             * made alike, as far down as either got, and the stack pointer
             * with them. */
            uint16_t low = g_original.min_sp < g_lifted.min_sp ? g_original.min_sp : g_lifted.min_sp;
            if (g_original_ctx.sp < low) low = g_original_ctx.sp;
            if (g_lifted_ctx.sp < low) low = g_lifted_ctx.sp;
            if (low < entry.sp && (unsigned)(entry.sp - low) <= 0x400u) {
                for (unsigned address = low; address < entry.sp; ++address) {
                    lift_machine_poke(&g_lifted, (uint16_t)address,
                                      lift_machine_peek(&g_original, (uint16_t)address));
                }
            }
            g_lifted_ctx.sp = g_original_ctx.sp;
        }

        Differences differences;
        compare(&differences, &g_original, &g_original_ctx, &g_lifted, &g_lifted_ctx, ignore | g_undefined,
                options->strict_stack);
        /* The original looked at, or wrote over, what it had pushed: a copy
         * or a search whose made-up pointer takes in the stack. The source
         * it is compared with does not push what the original pushes: its
         * lowest stack pointer is another, or it says so itself
         * (lift_own_stack). It is source written as C, which has no such
         * bytes to agree about. Source that keeps the original's stack is
         * held to this state like any other. */
        const int no_such_bytes =
            g_original.frame_used && (g_own_stack || g_lifted.min_sp != g_original.min_sp);
        if (differences.total != 0 && no_such_bytes) {
            /* The state says nothing of it. */
            skipped[LIFT_OWN_STACK]++;
            if (recorded) as_recorded[2]++;
            continue;
        }
        if (differences.total == 0) {
            passed++;
            if (recorded) as_recorded[0]++;
            if (g_wakes.change_count) waited++;
            if (g_lifted.cycles == g_original.cycles) cycles_equal++;
            else if (g_lifted.cycles == 0) cycles_untracked++;
            /* The two came out alike and took another time over it: a search
             * that goes one way or the other over a byte and ends the same.
             * The byte was one the original had pushed, so the state says as
             * little of the time as it would have of a register. */
            else if (no_such_bytes) cycles_apart++;
            else cycles_differ++;
            if (parked && ++stopped_waiting == passed && stopped_waiting >= ENOUGH_WAITING) {
                /* It has returned from no state and is not going to: a main
                 * loop. More states would be more of the same. */
                gave_up_after = trial + 1;
                break;
            }
            continue;
        }
        mismatched++;
        if (recorded) as_recorded[1]++;
        if (reported < MAX_REPORTED_MISMATCHES) {
            fprintf(out, "%s\n      {\"trial\": %u, ", reported ? "," : "", trial);
            if (own_count) {
                /* Which state of the game's it was, and whether as it was. */
                fprintf(out, "\"arrival\": %u, \"as_recorded\": %s, ",
                        (unsigned)own[trial % own_count].arrival, recorded ? "true" : "false");
            }
            fprintf(out, "\"entry\": {\"a\": %u, \"b\": %u, \"c\": %u, "
                         "\"d\": %u, \"e\": %u, \"h\": %u, \"l\": %u, \"sp\": %u, \"fz\": %u, "
                         "\"fn\": %u, \"fh\": %u, \"fc\": %u, \"rom_bank\": %u}, "
                         "\"difference_count\": %u, \"differences\": [",
                    entry.a, entry.b, entry.c, entry.d, entry.e,
                    entry.h, entry.l, entry.sp, entry.f_z, entry.f_n, entry.f_h, entry.f_c,
                    entry_bank,
                    differences.total);
            for (unsigned i = 0; i < differences.count; ++i) {
                fprintf(out, "%s", i ? ", " : "");
                json_text(out, differences.text[i]);
            }
            fprintf(out, "]}");
            reported++;
        }
    }
    fprintf(out, "%s],\n", reported ? "\n    " : "");

    unsigned skipped_total = 0;
    for (unsigned i = 0; i < LIFT_STATUS_COUNT; ++i) skipped_total += skipped[i];
    /* More states thrown away because the source keeps no stack than states
     * that agreed: the ones that agreed may be the ones where what was on
     * the stack happened not to matter. That is no verdict. */
    const char* verdict = mismatched ? "mismatch"
                          : (passed && passed >= skipped[LIFT_OWN_STACK] ? "match" : "inconclusive");
    fprintf(out, "     \"verdict\": \"%s\", \"trials\": %u, \"matched\": %u, \"mismatched\": %u, "
                 "\"skipped\": %u,\n",
            verdict, gave_up_after ? gave_up_after : trials, passed, mismatched,
            skipped_total);
    if (entries->present) {
        fprintf(out, "     \"entry_states\": {\"kept\": %u, \"matched\": %u, \"mismatched\": %u, "
                     "\"thrown_away\": %u},\n",
                own_count, as_recorded[0], as_recorded[1], as_recorded[2]);
    }
    fprintf(out, "     \"skipped_because\": {");
    {
        int first = 1;
        for (unsigned i = 1; i < LIFT_STATUS_COUNT; ++i) {
            if (!skipped[i]) continue;
            fprintf(out, "%s\"%s\": %u", first ? "" : ", ", lift_status_name((LiftStatus)i),
                    skipped[i]);
            first = 0;
        }
    }
    fprintf(out, "},\n");
    fprintf(out, "     \"cycles\": {\"equal\": %u, \"different\": %u, \"not_counted\": %u, "
                 "\"not_compared\": %u},\n",
            cycles_equal, cycles_differ, cycles_untracked, cycles_apart);
    fprintf(out, "     \"ran_code_in_ram\": %u,\n", ran_ram_code);
    if (options->observe) {
        fprintf(out, "     \"observed\": {\"states\": %u, \"reads\": %u, \"changed\": %u, \"through\": %u},\n",
                seen.states, seen.reads, seen.changed, seen.through);
    }
    fprintf(out, "     \"waited\": %u, \"stopped_waiting\": %u,\n", waited, stopped_waiting);

    fprintf(out, "     \"executed\": [");
    {
        int first = 1;
        for (uint32_t offset = candidate->cover_low;
             offset <= candidate->cover_high && offset < g_base.rom_size; ++offset) {
            if (!executed[offset]) continue;
            fprintf(out, "%s%u", first ? "" : ", ", (unsigned)offset);
            first = 0;
        }
    }
    fprintf(out, "],\n");

    fprintf(out, "     \"memory_read\": [");
    {
        int first = 1;
        unsigned listed = 0;
        for (unsigned address = 0x8000; address < 0x10000; ++address) {
            if (!read_seen[address]) continue;
            if (address >= stack_low && address <= stack_high) continue;
            if (listed++ >= 256) break;
            fprintf(out, "%s%u", first ? "" : ", ", address);
            first = 0;
        }
    }
    fprintf(out, "]}");

    free(read_seen);
    free(executed);
    free(reads);
    free(bodies);
    return mismatched ? 1 : (passed ? 0 : 3);
}

static void usage(void) {
    fprintf(stderr,
            "usage: lift_check --rom FILE [options]\n"
            "  --image FILE      start every trial from this memory image\n"
            "  --entries FILE    start every trial from a state the function was entered in, kept by\n"
            "                    a game played with --entry-states; a function none was kept of is\n"
            "                    not tried\n"
            "  --trials N        states to try per function (default 500)\n"
            "  --seed N          seed for the states (default 1)\n"
            "  --max-steps N     instructions the original may run per trial (default 200000)\n"
            "  --only BB:AAAA    check only this function (hexadecimal bank and address)\n"
            "  --ignore LIST     registers and flags not to compare: a b c d e h l fz fn fh fc ime,\n"
            "                    and RAM as AAAA or AAAA-BBBB (hexadecimal)\n"
            "  --strict-stack    also compare the bytes below the stack pointer\n"
            "  --observe         also report what each function reads and what it changes\n"
            "  --observe-states N  from N states of a run at most (default 160; 0: all)\n"
            "  --shard I/N       check only every Nth function, starting at the Ith\n"
            "  --json FILE       write the report here instead of standard output\n"
            "exit status: 0 every function matched, 1 a mismatch, 3 nothing could be compared\n");
}

int main(int argc, char** argv) {
    Options options;
    memset(&options, 0, sizeof(options));
    options.trials = 500;
    options.seed = 1;
    options.max_steps = 200000;
    options.observe_states = OBSERVE_STATES;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(arg, "--observe") == 0) {
            options.observe = 1;
            continue;
        }
        if (strcmp(arg, "--observe-states") == 0 && i + 1 < argc) {
            options.observe_states = (unsigned)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (strcmp(arg, "--strict-stack") == 0) {
            options.strict_stack = 1;
            continue;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage();
            return 0;
        }
        if (!value) {
            usage();
            return 2;
        }
        if (strcmp(arg, "--rom") == 0) options.rom_path = value;
        else if (strcmp(arg, "--image") == 0) options.image_path = value;
        else if (strcmp(arg, "--entries") == 0) options.entries_path = value;
        else if (strcmp(arg, "--json") == 0) options.json_path = value;
        else if (strcmp(arg, "--only") == 0) options.only = value;
        else if (strcmp(arg, "--ignore") == 0) options.ignore = value;
        else if (strcmp(arg, "--trials") == 0) options.trials = (unsigned)strtoul(value, NULL, 0);
        else if (strcmp(arg, "--seed") == 0) options.seed = strtoull(value, NULL, 0);
        else if (strcmp(arg, "--max-steps") == 0) options.max_steps = strtoull(value, NULL, 0);
        else if (strcmp(arg, "--shard") == 0) {
            if (sscanf(value, "%u/%u", &options.shard, &options.shards) != 2 ||
                options.shards == 0 || options.shard >= options.shards) {
                fprintf(stderr, "lift_check: --shard wants I/N with I below N\n");
                return 2;
            }
        }
        else {
            usage();
            return 2;
        }
        ++i;
    }
    if (!options.rom_path || options.trials == 0) {
        usage();
        return 2;
    }
    if (options.image_path && options.entries_path) {
        fprintf(stderr, "lift_check: --image and --entries are two kinds of state: one run for each\n");
        return 2;
    }

    size_t rom_size = 0;
    uint8_t* rom = read_file(options.rom_path, &rom_size);
    if (!rom || rom_size < 0x150) {
        fprintf(stderr, "lift_check: cannot read ROM %s\n", options.rom_path);
        return 2;
    }
    lift_machine_init(&g_base, rom, rom_size);

    Image image;
    memset(&image, 0, sizeof(image));
    if (options.image_path && !load_image(options.image_path, &g_base, &image)) return 2;
    Entries entries;
    memset(&entries, 0, sizeof(entries));
    if (options.entries_path && !load_entries(options.entries_path, &entries)) return 2;

    unsigned only_bank = 0;
    unsigned only_address = 0;
    if (options.only && sscanf(options.only, "%x:%x", &only_bank, &only_address) != 2) {
        fprintf(stderr, "lift_check: --only wants BB:AAAA in hexadecimal\n");
        return 2;
    }

    FILE* out = stdout;
    if (options.json_path) {
        out = fopen(options.json_path, "w");
        if (!out) {
            fprintf(stderr, "lift_check: cannot write %s\n", options.json_path);
            return 2;
        }
    }

    int worst = 0;
    int any = 0;
    fprintf(out, "{\n  \"schema\": \"gbrecomp.lift-check\",\n  \"version\": 1,\n");
    fprintf(out, "  \"trials\": %u,\n  \"seed\": %llu,\n  \"image\": %s,\n  \"entries\": %s,\n"
                 "  \"results\": [\n",
            options.trials, (unsigned long long)options.seed, image.present ? "true" : "false",
            entries.present ? "true" : "false");
    for (unsigned i = 0; i < lift_candidate_count; ++i) {
        const LiftCandidate* candidate = &lift_candidates[i];
        if (options.only &&
            (candidate->bank != only_bank || candidate->address != only_address)) {
            continue;
        }
        if (options.shards > 1 && i % options.shards != options.shard) continue;
        if (any) fprintf(out, ",\n");
        const int result = check_candidate(&options, &image, &entries, candidate, out);
        if (result == 1 || (result == 3 && worst == 0)) worst = result;
        any = 1;
    }
    fprintf(out, "\n  ]\n}\n");
    if (out != stdout) fclose(out);
    free(rom);
    if (!any && options.shards <= 1) {
        fprintf(stderr, "lift_check: no function to check\n");
        return 2;
    }
    return worst;
}
