/**
 * gbrt_entry_states.c - see gbrt_entry_states.h.
 *
 * The file, numbers little-endian:
 *
 *   "GBLIFTEN", u32 version (1)
 *   u32 page_size (256)
 *   u32 eram_size          bytes of cartridge RAM in a state
 *   u32 pages_per_state
 *   u32 state_count
 *   u32 page_count
 *   u32 function_count
 *   u32 listed             1: the functions were named, 0: they are where calls arrived
 *   u32 per                states kept of one function at most
 *   u32 zero
 *
 *   function_count times, by bank and address:
 *     u16 bank, u16 address, u32 arrivals, u32 kept
 *
 *   state_count times, by bank, address and arrival:
 *     u16 bank, u16 address       the function: bank 0 below 0x4000
 *     u16 rom_bank                mapped at 0x4000-0x7FFF
 *     u8 a, f, b, c, d, e, h, l
 *     u16 sp
 *     u16 returns_to              the word at SP
 *     u8 ime, ram_bank, ram_enabled, wram_bank, vram_bank, zero
 *     u32 arrival                 which arrival at the function this was, from 1
 *     u64 cycle                   the machine's count of cycles
 *     u32 zero
 *
 *   state_count times, in the same order: u32[pages_per_state], the pages of
 *   the state's memory in this order, the last page filled up with zeros:
 *     cartridge RAM (eram_size), work RAM (0x8000), video RAM (0x4000),
 *     the sprite table (0xA0), high RAM (0x7F), the hardware registers'
 *     stored values (0x80), the interrupt enable register (1)
 *
 *   page_count times: page_size bytes
 */

#include "gbrt_entry_states.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 256u
#define TAIL_BYTES (0xA0u + 0x7Fu + 0x80u + 1u)
#define WRAM_BYTES 0x8000u
#define VRAM_BYTES 0x4000u
#define CALLERS 8u
#define DEFAULT_PER 8u
#define FUNCTION_RECORD 12u
#define STATE_RECORD 40u

typedef struct Function {
    uint32_t offset;   /* of its first instruction in the ROM */
    uint32_t arrivals;
    uint16_t kept;
    uint16_t spread;   /* of those, kept for the count of the arrival */
    uint16_t caller[CALLERS]; /* return addresses it was seen entered with */
    uint8_t callers;
} Function;

typedef struct State {
    uint32_t function;
    uint32_t arrival;
    uint64_t cycle;
    uint16_t rom_bank;
    uint16_t sp;
    uint16_t returns_to;
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t ime, ram_bank, ram_enabled, wram_bank, vram_bank;
} State;

typedef struct EntryStates {
    char* path;
    int listed;
    unsigned per;
    size_t rom_size;
    int32_t* function_of; /* for each byte of the ROM: its function, or -1 */

    Function* functions;
    uint32_t function_count, function_room;

    State* states;
    uint32_t* page_ids;   /* pages_per_state for each state */
    uint32_t state_count, state_room;
    uint32_t pages_per_state;
    uint32_t eram_size;

    uint8_t* pages;
    uint32_t page_count, page_room;
    uint32_t* table;      /* open addressing: a page's number plus one, or 0 */
    uint32_t table_size;  /* a power of two */

    /* Without a list: what the last instruction was, if it was a call. */
    uint8_t call_pending;
    uint8_t arriving;     /* an interrupt is on its way to its vector */
    uint16_t call_target;
    uint16_t call_sp;

    int failed;           /* out of memory: nothing more is kept, and the file is not written */
} EntryStates;

static char* g_path = NULL;
static char* g_list = NULL;
static unsigned g_per = DEFAULT_PER;

static char* copy_text(const char* text) {
    if (!text || !text[0]) return NULL;
    const size_t length = strlen(text) + 1u;
    char* copy = (char*)malloc(length);
    if (copy) memcpy(copy, text, length);
    return copy;
}

void gbrt_entry_states_set_file(const char* path) {
    free(g_path);
    g_path = copy_text(path);
}

bool gbrt_entry_states_requested(void) {
    return g_path != NULL;
}

void gbrt_entry_states_set_list(const char* path) {
    free(g_list);
    g_list = copy_text(path);
}

bool gbrt_entry_states_list_requested(void) {
    return g_list != NULL;
}

bool gbrt_entry_states_set_per(const char* count) {
    char* end = NULL;
    const unsigned long value = count ? strtoul(count, &end, 10) : 0;
    if (!count || !count[0] || *end || value < 1 || value > 4096) return false;
    g_per = (unsigned)value;
    return true;
}

static EntryStates* states_of(const GBContext* ctx) {
    return ctx ? (EntryStates*)ctx->entry_states : NULL;
}

static void release(EntryStates* s) {
    if (!s) return;
    free(s->path);
    free(s->function_of);
    free(s->functions);
    free(s->states);
    free(s->page_ids);
    free(s->pages);
    free(s->table);
    free(s);
}

static int32_t add_function(EntryStates* s, size_t offset) {
    if (s->function_count == s->function_room) {
        const uint32_t room = s->function_room ? s->function_room * 2u : 1024u;
        Function* grown = (Function*)realloc(s->functions, (size_t)room * sizeof(Function));
        if (!grown) {
            s->failed = 1;
            return -1;
        }
        s->functions = grown;
        s->function_room = room;
    }
    Function* f = &s->functions[s->function_count];
    memset(f, 0, sizeof(*f));
    f->offset = (uint32_t)offset;
    s->function_of[offset] = (int32_t)s->function_count;
    return (int32_t)s->function_count++;
}

/* The list: one BANK:ADDRESS in hexadecimal a line; `#` starts a comment. */
static bool read_list(EntryStates* s, const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "[GBRT] Entry states: cannot read the list %s\n", path);
        return false;
    }
    char line[256];
    unsigned number = 0;
    bool good = true;
    while (good && fgets(line, sizeof(line), file)) {
        ++number;
        char* hash = strchr(line, '#');
        if (hash) *hash = '\0';
        unsigned bank = 0, address = 0;
        char rest = '\0';
        const int found = sscanf(line, " %x : %x %c", &bank, &address, &rest);
        if (found <= 0) continue; /* nothing on the line */
        const size_t offset = (size_t)bank * 0x4000u + (address & 0x3FFFu);
        if (found != 2 || address >= 0x8000u || (bank == 0) != (address < 0x4000u) ||
            offset >= s->rom_size) {
            fprintf(stderr, "[GBRT] Entry states: line %u of %s is not the BANK:ADDRESS of a place in "
                            "this ROM\n", number, path);
            good = false;
        } else if (s->function_of[offset] < 0 && add_function(s, offset) < 0) {
            good = false;
        }
    }
    fclose(file);
    return good;
}

bool gbrt_entry_states_start(GBContext* ctx) {
    if (!ctx || !g_path || ctx->entry_states) return ctx != NULL;
    EntryStates* s = (EntryStates*)calloc(1, sizeof(EntryStates));
    if (!s) return false;
    s->path = copy_text(g_path);
    s->per = g_per;
    s->rom_size = ctx->rom_size;
    s->eram_size = ctx->eram ? (uint32_t)ctx->eram_size : 0u;
    s->pages_per_state = (s->eram_size + PAGE_SIZE - 1u) / PAGE_SIZE + WRAM_BYTES / PAGE_SIZE +
                         VRAM_BYTES / PAGE_SIZE + (TAIL_BYTES + PAGE_SIZE - 1u) / PAGE_SIZE;
    s->function_of = (int32_t*)malloc((s->rom_size ? s->rom_size : 1u) * sizeof(int32_t));
    s->table_size = 1u << 16;
    s->table = (uint32_t*)calloc(s->table_size, sizeof(uint32_t));
    if (!s->path || !s->function_of || !s->table) {
        release(s);
        return false;
    }
    for (size_t i = 0; i < s->rom_size; ++i) s->function_of[i] = -1;
    if (g_list) {
        s->listed = 1;
        if (!read_list(s, g_list)) {
            release(s);
            return false;
        }
    }
    ctx->entry_states = s;
    return true;
}

void gbrt_entry_states_destroy(GBContext* ctx) {
    release(states_of(ctx));
    if (ctx) ctx->entry_states = NULL;
}

/* ---- Pages --------------------------------------------------------------- */

static uint64_t hash_page(const uint8_t* page) {
    uint64_t hash = 0x243F6A8885A308D3ull;
    for (unsigned i = 0; i < PAGE_SIZE; i += 8u) {
        uint64_t word;
        memcpy(&word, page + i, 8);
        hash = (hash ^ word) * 0x9E3779B97F4A7C15ull;
        hash ^= hash >> 29;
    }
    return hash;
}

static bool grow_table(EntryStates* s) {
    const uint32_t size = s->table_size * 2u;
    uint32_t* table = (uint32_t*)calloc(size, sizeof(uint32_t));
    if (!table) return false;
    for (uint32_t page = 0; page < s->page_count; ++page) {
        uint32_t slot = (uint32_t)hash_page(s->pages + (size_t)page * PAGE_SIZE) & (size - 1u);
        while (table[slot]) slot = (slot + 1u) & (size - 1u);
        table[slot] = page + 1u;
    }
    free(s->table);
    s->table = table;
    s->table_size = size;
    return true;
}

/* The number of the page that holds these 256 bytes, stored now if no state
 * had it before. False when there is no memory for it. */
static bool intern(EntryStates* s, const uint8_t* page, uint32_t* id) {
    uint32_t slot = (uint32_t)hash_page(page) & (s->table_size - 1u);
    while (s->table[slot]) {
        const uint32_t held = s->table[slot] - 1u;
        if (memcmp(s->pages + (size_t)held * PAGE_SIZE, page, PAGE_SIZE) == 0) {
            *id = held;
            return true;
        }
        slot = (slot + 1u) & (s->table_size - 1u);
    }
    if (s->page_count == s->page_room) {
        const uint32_t room = s->page_room ? s->page_room * 2u : 4096u;
        uint8_t* grown = (uint8_t*)realloc(s->pages, (size_t)room * PAGE_SIZE);
        if (!grown) return false;
        s->pages = grown;
        s->page_room = room;
    }
    memcpy(s->pages + (size_t)s->page_count * PAGE_SIZE, page, PAGE_SIZE);
    *id = s->page_count;
    s->table[slot] = ++s->page_count;
    if (s->page_count > s->table_size / 2u && !grow_table(s)) return false;
    return true;
}

static bool intern_bytes(EntryStates* s, const uint8_t* bytes, size_t size, uint32_t** ids) {
    size_t at = 0;
    for (; at + PAGE_SIZE <= size; at += PAGE_SIZE) {
        if (!intern(s, bytes + at, (*ids)++)) return false;
    }
    if (at < size) {
        uint8_t last[PAGE_SIZE] = {0};
        memcpy(last, bytes + at, size - at);
        if (!intern(s, last, (*ids)++)) return false;
    }
    return true;
}

/* ---- Keeping a state ------------------------------------------------------ */

static void keep(EntryStates* s, GBContext* ctx, uint32_t function, uint16_t returns_to) {
    if (s->state_count == s->state_room) {
        const uint32_t room = s->state_room ? s->state_room * 2u : 1024u;
        State* states = (State*)realloc(s->states, (size_t)room * sizeof(State));
        if (states) s->states = states;
        uint32_t* ids = states ? (uint32_t*)realloc(s->page_ids, (size_t)room * s->pages_per_state *
                                                                 sizeof(uint32_t))
                               : NULL;
        if (!ids) {
            s->failed = 1;
            return;
        }
        s->page_ids = ids;
        s->state_room = room;
    }

    uint32_t* ids = s->page_ids + (size_t)s->state_count * s->pages_per_state;
    uint8_t tail[TAIL_BYTES];
    memcpy(tail, ctx->oam, 0xA0u);
    memcpy(tail + 0xA0u, ctx->hram, 0x7Fu);
    memcpy(tail + 0xA0u + 0x7Fu, ctx->io, 0x81u); /* the registers, then interrupt enable */
    if ((s->eram_size && !intern_bytes(s, ctx->eram, s->eram_size, &ids)) ||
        !intern_bytes(s, ctx->wram, WRAM_BYTES, &ids) ||
        !intern_bytes(s, ctx->vram, VRAM_BYTES, &ids) ||
        !intern_bytes(s, tail, TAIL_BYTES, &ids)) {
        s->failed = 1;
        return;
    }

    Function* f = &s->functions[function];
    State* state = &s->states[s->state_count++];
    memset(state, 0, sizeof(*state));
    state->function = function;
    state->arrival = f->arrivals;
    state->cycle = ctx->total_cycles;
    state->rom_bank = ctx->rom_bank;
    state->sp = ctx->sp;
    state->returns_to = returns_to;
    state->a = ctx->a;
    state->f = (uint8_t)((ctx->f_z ? 0x80u : 0u) | (ctx->f_n ? 0x40u : 0u) |
                         (ctx->f_h ? 0x20u : 0u) | (ctx->f_c ? 0x10u : 0u));
    state->b = ctx->b;
    state->c = ctx->c;
    state->d = ctx->d;
    state->e = ctx->e;
    state->h = ctx->h;
    state->l = ctx->l;
    state->ime = ctx->ime ? 1u : 0u;
    state->ram_bank = ctx->ram_bank;
    state->ram_enabled = ctx->ram_enabled ? 1u : 0u;
    state->wram_bank = ctx->wram_bank;
    state->vram_bank = ctx->vram_bank;
    f->kept++;
}

/* Control is at the first instruction of a function. A state is kept the
 * first time it comes with a return address not seen for the function
 * before, which is another caller and likely another kind of use, and for
 * half of what a function may have, at its first, second, fourth, eighth...
 * arrival, which spreads them over the run. */
static void arrived(EntryStates* s, GBContext* ctx, uint32_t function) {
    Function* f = &s->functions[function];
    if (f->arrivals != UINT32_MAX) f->arrivals++;
    if (f->kept >= s->per || s->failed) return;

    const uint16_t returns_to = (uint16_t)(gbrt_peek8(ctx, ctx->sp) |
                                           ((uint16_t)gbrt_peek8(ctx, (uint16_t)(ctx->sp + 1u)) << 8));
    int new_caller = 1;
    for (unsigned i = 0; i < f->callers; ++i) {
        if (f->caller[i] == returns_to) new_caller = 0;
    }
    if (new_caller) {
        if (f->callers < CALLERS) f->caller[f->callers++] = returns_to;
        else new_caller = 0; /* more callers than are remembered: no longer told apart */
    }
    const int spread = (f->arrivals & (f->arrivals - 1u)) == 0 && f->spread < (s->per + 1u) / 2u;
    if (!new_caller && !spread) return;
    if (!new_caller) f->spread++;
    keep(s, ctx, function, returns_to);
}

void gbrt_entry_states_interrupt(GBContext* ctx) {
    EntryStates* s = states_of(ctx);
    if (s) s->arriving = 1;
}

void gbrt_entry_states_before(GBContext* ctx) {
    EntryStates* s = states_of(ctx);
    if (!s) return;
    const uint16_t pc = ctx->pc;
    if (pc >= 0x8000u) {
        /* Code in RAM is no function of the ROM, and calls nothing we follow. */
        s->call_pending = 0;
        s->arriving = 0;
        return;
    }
    const size_t offset = (size_t)gb_resolve_rom_bank(ctx, pc) * 0x4000u + (pc & 0x3FFFu);
    if (offset >= s->rom_size) return;
    int32_t function = s->function_of[offset];

    if (!s->listed) {
        const int called = s->arriving ||
                           (s->call_pending && pc == s->call_target && ctx->sp == s->call_sp);
        s->arriving = 0;
        s->call_pending = 0;
        const uint8_t opcode = ctx->rom[offset];
        if ((opcode & 0xC7u) == 0xC7u) {
            s->call_pending = 1; /* RST */
            s->call_target = opcode & 0x38u;
            s->call_sp = (uint16_t)(ctx->sp - 2u);
        } else if ((opcode == 0xCDu || (opcode & 0xE7u) == 0xC4u) && (pc & 0x3FFFu) < 0x3FFEu &&
                   offset + 2u < s->rom_size) {
            s->call_pending = 1; /* CALL nn, CALL cc, nn: taken if control is there next */
            s->call_target = (uint16_t)(ctx->rom[offset + 1u] | (ctx->rom[offset + 2u] << 8));
            s->call_sp = (uint16_t)(ctx->sp - 2u);
        }
        if (function < 0 && called && !s->failed) function = add_function(s, offset);
    }
    if (function >= 0) arrived(s, ctx, (uint32_t)function);
}

/* ---- The file -------------------------------------------------------------- */

static const EntryStates* g_sorting;

static int by_offset(const void* left, const void* right) {
    const Function* a = &g_sorting->functions[*(const uint32_t*)left];
    const Function* b = &g_sorting->functions[*(const uint32_t*)right];
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

static int by_function(const void* left, const void* right) {
    const State* a = &g_sorting->states[*(const uint32_t*)left];
    const State* b = &g_sorting->states[*(const uint32_t*)right];
    const uint32_t a_offset = g_sorting->functions[a->function].offset;
    const uint32_t b_offset = g_sorting->functions[b->function].offset;
    if (a_offset != b_offset) return a_offset < b_offset ? -1 : 1;
    return a->arrival < b->arrival ? -1 : a->arrival > b->arrival;
}

static void put16(uint8_t* at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t* at, uint32_t value) {
    put16(at, value);
    put16(at + 2, value >> 16);
}

static void put_place(uint8_t* at, uint32_t offset) {
    const uint32_t bank = offset / 0x4000u;
    put16(at, bank);
    put16(at + 2, bank ? 0x4000u + offset % 0x4000u : offset);
}

bool gbrt_entry_states_write(GBContext* ctx) {
    EntryStates* s = states_of(ctx);
    if (!s) return true;
    if (s->failed) {
        fprintf(stderr, "[GBRT] Entry states: out of memory while recording; %s not written\n", s->path);
        return false;
    }
    uint32_t* function_order = (uint32_t*)malloc((s->function_count ? s->function_count : 1u) * sizeof(uint32_t));
    uint32_t* state_order = (uint32_t*)malloc((s->state_count ? s->state_count : 1u) * sizeof(uint32_t));
    FILE* file = function_order && state_order ? fopen(s->path, "wb") : NULL;
    if (!file) {
        fprintf(stderr, "[GBRT] Entry states: cannot write %s\n", s->path);
        free(function_order);
        free(state_order);
        return false;
    }
    for (uint32_t i = 0; i < s->function_count; ++i) function_order[i] = i;
    for (uint32_t i = 0; i < s->state_count; ++i) state_order[i] = i;
    g_sorting = s;
    qsort(function_order, s->function_count, sizeof(uint32_t), by_offset);
    qsort(state_order, s->state_count, sizeof(uint32_t), by_function);

    uint8_t header[48] = {0};
    memcpy(header, "GBLIFTEN", 8);
    put32(header + 8, 1u);
    put32(header + 12, PAGE_SIZE);
    put32(header + 16, s->eram_size);
    put32(header + 20, s->pages_per_state);
    put32(header + 24, s->state_count);
    put32(header + 28, s->page_count);
    put32(header + 32, s->function_count);
    put32(header + 36, s->listed ? 1u : 0u);
    put32(header + 40, s->per);
    bool good = fwrite(header, 1, sizeof(header), file) == sizeof(header);

    uint32_t never = 0;
    for (uint32_t i = 0; good && i < s->function_count; ++i) {
        const Function* f = &s->functions[function_order[i]];
        uint8_t record[FUNCTION_RECORD];
        put_place(record, f->offset);
        put32(record + 4, f->arrivals);
        put32(record + 8, f->kept);
        if (!f->arrivals) never++;
        good = fwrite(record, 1, sizeof(record), file) == sizeof(record);
    }
    for (uint32_t i = 0; good && i < s->state_count; ++i) {
        const State* state = &s->states[state_order[i]];
        uint8_t record[STATE_RECORD] = {0};
        put_place(record, s->functions[state->function].offset);
        put16(record + 4, state->rom_bank);
        record[6] = state->a;
        record[7] = state->f;
        record[8] = state->b;
        record[9] = state->c;
        record[10] = state->d;
        record[11] = state->e;
        record[12] = state->h;
        record[13] = state->l;
        put16(record + 14, state->sp);
        put16(record + 16, state->returns_to);
        record[18] = state->ime;
        record[19] = state->ram_bank;
        record[20] = state->ram_enabled;
        record[21] = state->wram_bank;
        record[22] = state->vram_bank;
        put32(record + 24, state->arrival);
        put32(record + 28, (uint32_t)state->cycle);
        put32(record + 32, (uint32_t)(state->cycle >> 32));
        good = fwrite(record, 1, sizeof(record), file) == sizeof(record);
    }
    /* On a little-endian machine the numbers lie in memory as the file has
     * them; written one at a time all the same, so that it holds anywhere. */
    for (uint32_t i = 0; good && i < s->state_count; ++i) {
        const uint32_t* ids = s->page_ids + (size_t)state_order[i] * s->pages_per_state;
        for (uint32_t page = 0; good && page < s->pages_per_state; ++page) {
            uint8_t number[4];
            put32(number, ids[page]);
            good = fwrite(number, 1, 4, file) == 4;
        }
    }
    if (good && s->page_count) {
        good = fwrite(s->pages, PAGE_SIZE, s->page_count, file) == s->page_count;
    }
    if (fclose(file) != 0) good = false;
    free(function_order);
    free(state_order);
    if (!good) {
        fprintf(stderr, "[GBRT] Entry states: could not write %s\n", s->path);
        return false;
    }
    if (s->listed) {
        printf("[GBRT] Entry states: %u states of %u functions, %u of the %u asked for never arrived at, "
               "%u pages: %s\n",
               s->state_count, s->function_count - never, never, s->function_count, s->page_count, s->path);
    } else {
        printf("[GBRT] Entry states: %u states of the %u functions calls arrived at, %u pages: %s\n",
               s->state_count, s->function_count, s->page_count, s->path);
    }
    return true;
}
