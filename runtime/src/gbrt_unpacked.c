/**
 * gbrt_unpacked.c - what a game's own unpacking routine makes, kept and
 * replaced. See gbrt_unpacked.h.
 *
 * The pack (`.gbup`, written by tools/gb_data.py, little endian):
 *
 *     "GBUP"  u32 version (1)
 *     u32 size and u8[32] SHA-256 of the ROM the game was built from
 *     u16 routines  u32 entries
 *     each routine:  u8 length and that many bytes of its name
 *                    u16 bank  u16 address          where it is entered
 *                    expr bank  expr address        where its stream starts
 *                    u8 segments, each: expr address  u16 size   what it makes
 *     each entry:    u16 routine  u16 bank  u16 address   which stream
 *                    u8[32] SHA-256 of what the routine makes of it
 *                    u32 size and that many bytes to have in its place
 *
 * An expr is u8 kind and u16 value, worked out as the routine is entered:
 * 0 the value itself, 1 an 8-bit register (a b c d e h l), 2 a register
 * pair (bc de hl), 3 a byte of RAM, 4 a word of RAM, 5 the ROM bank mapped
 * at 4000-7FFF. A pack with no entries is what a harvest is made with.
 */

#include "gbrt_unpacked.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gbrt_data_use.h"
#include "gbrt_hash.h"

#define MAX_ROUTINES 64
#define MAX_SEGMENTS 8
#define MAX_PENDING 8
#define MAX_MADE 65536u          /* bytes one routine may make in all */
#define MAX_HARVEST 16384u       /* different things made that a harvest keeps */

enum { EXPR_CONST, EXPR_REG8, EXPR_REG16, EXPR_RAM8, EXPR_RAM16, EXPR_MAPPED_BANK, EXPR_KINDS };

typedef struct {
    uint8_t kind;
    uint16_t value;
} Expr;

typedef struct {
    char name[64];
    uint16_t bank;
    uint16_t address;
    Expr stream_bank;
    Expr stream_address;
    uint8_t segments;
    Expr made_address[MAX_SEGMENTS];
    uint16_t made_size[MAX_SEGMENTS];
    uint32_t made_total;
    /* What happened, for the report and the harvest. */
    uint64_t arrived;
    uint64_t returned;
    uint64_t replaced;
    uint64_t made_other;         /* a replacement was there for the stream, and for something else made of it */
    uint64_t nowhere;            /* what it makes was not all in RAM this module can reach */
} Routine;

typedef struct {
    uint16_t routine;
    uint16_t bank;
    uint16_t address;
    uint8_t made_sha256[32];
    uint32_t size;
    const uint8_t* bytes;        /* inside `pack` */
} Entry;

typedef struct {
    uint16_t routine;
    uint16_t bank;
    uint16_t address;
    uint8_t sha256[32];
    uint32_t times;
    uint8_t* bytes;
} Made;

typedef struct {
    uint16_t routine;
    uint16_t sp;                 /* the stack pointer as the routine was entered */
    uint16_t bank;
    uint16_t address;
    uint16_t made_address[MAX_SEGMENTS];
} Pending;

typedef struct {
    uint8_t* pack;               /* the file, which the entries point into */
    Routine routines[MAX_ROUTINES];
    unsigned routine_count;
    Entry* entries;
    size_t entry_count;
    Pending pending[MAX_PENDING];
    unsigned pending_count;
    bool harvesting;
    Made* made;
    size_t made_count;
    size_t made_room;
    size_t made_dropped;
    uint8_t rom_sha256[32];
    size_t rom_size;
    uint8_t replacements_sha256[32]; /* what tells the entries from any others; see hash_replacements */
    uint8_t* scratch;            /* room for the most any routine makes */
    /* The call made for the game (--unpacked-call), if one was. */
    uint8_t call_state;
    uint16_t call_sp;            /* the stack pointer the function was entered with */
    uint16_t call_routine;       /* the routine whose return ended it */
    uint64_t call_from;          /* total_cycles as it was made */
    uint64_t call_took;
} Unpacked;

static char pack_path[1024];
static char harvest_path[1024];

/* What --unpacked-call asked for. */
#define MAX_CALL_SETS 32
#define CALL_CYCLES 41943040u    /* ten seconds of the game's time, unless --unpacked-call-cycles says */
enum { SET_REG8, SET_REG16, SET_RAM, SET_CARTRIDGE, SET_IME };
enum { CALL_NONE, CALL_GOING, CALL_RETURNED, CALL_CAME_BACK, CALL_GAVE_UP };

typedef struct {
    uint8_t kind;
    uint16_t where;              /* which register, or the address */
    uint16_t value;
} CallSet;

static struct {
    bool requested;
    uint16_t bank;
    uint16_t address;
    CallSet sets[MAX_CALL_SETS];
    unsigned set_count;
    uint64_t cycles;
} call = {false, 0, 0, {{0, 0, 0}}, 0, CALL_CYCLES};

void gbrt_unpacked_set_pack(const char* path) {
    snprintf(pack_path, sizeof(pack_path), "%s", path ? path : "");
}

void gbrt_unpacked_set_harvest(const char* path) {
    snprintf(harvest_path, sizeof(harvest_path), "%s", path ? path : "");
}

bool gbrt_unpacked_requested(void) {
    return pack_path[0] != '\0' || harvest_path[0] != '\0' || call.requested;
}

/* The frame every return is tested against: the innermost routine being
 * watched, and with none, the function --unpacked-call entered. */
static void set_watch(GBContext* ctx, const Unpacked* unpacked) {
    ctx->unpacked_watch = unpacked->pending_count
        ? (uint32_t)unpacked->pending[unpacked->pending_count - 1u].sp + 1u
        : unpacked->call_state == CALL_GOING ? (uint32_t)unpacked->call_sp + 1u : 0u;
}

void gbrt_unpacked_destroy(GBContext* ctx) {
    Unpacked* unpacked = (Unpacked*)ctx->unpacked;
    if (!unpacked) return;
    for (size_t i = 0; i < unpacked->made_count; i++) free(unpacked->made[i].bytes);
    free(unpacked->made);
    free(unpacked->entries);
    free(unpacked->scratch);
    free(unpacked->pack);
    free(unpacked);
    ctx->unpacked = NULL;
    ctx->unpacked_watch = 0u;
}

void gbrt_unpacked_reset(GBContext* ctx) {
    Unpacked* unpacked = (Unpacked*)ctx->unpacked;
    if (!unpacked) return;
    unpacked->pending_count = 0;
    set_watch(ctx, unpacked);
}

/* ---------------------------------------------------------------------------
 * Reading the pack
 * ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t at;
    bool short_of;
} Reader;

static uint32_t take(Reader* reader, unsigned bytes) {
    uint32_t value = 0;
    if (reader->size - reader->at < bytes) {
        reader->short_of = true;
        reader->at = reader->size;
        return 0;
    }
    for (unsigned i = 0; i < bytes; i++) value |= (uint32_t)reader->data[reader->at + i] << (8u * i);
    reader->at += bytes;
    return value;
}

static const uint8_t* take_bytes(Reader* reader, size_t bytes) {
    const uint8_t* here = reader->data + reader->at;
    if (reader->size - reader->at < bytes) {
        reader->short_of = true;
        reader->at = reader->size;
        return NULL;
    }
    reader->at += bytes;
    return here;
}

static bool take_expr(Reader* reader, Expr* expr) {
    expr->kind = (uint8_t)take(reader, 1);
    expr->value = (uint16_t)take(reader, 2);
    if (expr->kind >= EXPR_KINDS) return false;
    if (expr->kind == EXPR_REG8 && expr->value > 6u) return false;
    if (expr->kind == EXPR_REG16 && expr->value > 2u) return false;
    return true;
}

static bool refuse(char* message, size_t message_size, const char* why) {
    snprintf(message, message_size, "%s", why);
    return false;
}

static bool read_pack(Unpacked* unpacked, size_t size, char* message, size_t message_size) {
    Reader reader = {unpacked->pack, size, 0, false};
    const uint8_t* magic = take_bytes(&reader, 4);
    if (!magic || memcmp(magic, "GBUP", 4) != 0) return refuse(message, message_size, "it is not a pack of unpacked data");
    if (take(&reader, 4) != 1u) return refuse(message, message_size, "it is of a version this game does not read");
    const uint32_t rom_size = take(&reader, 4);
    const uint8_t* rom_sha256 = take_bytes(&reader, 32);
    if (!rom_sha256 || rom_size != unpacked->rom_size || memcmp(rom_sha256, unpacked->rom_sha256, 32) != 0) {
        return refuse(message, message_size, "it is of another ROM than the one this game was built from");
    }
    const uint32_t routines = take(&reader, 2);
    const uint32_t entries = take(&reader, 4);
    if (routines == 0 || routines > MAX_ROUTINES) return refuse(message, message_size, "it names no routine, or too many");
    for (uint32_t r = 0; r < routines; r++) {
        Routine* routine = &unpacked->routines[r];
        const uint32_t length = take(&reader, 1);
        const uint8_t* name = take_bytes(&reader, length);
        if (!name || length == 0 || length >= sizeof(routine->name)) {
            return refuse(message, message_size, "a routine in it has no name, or it is cut short");
        }
        memcpy(routine->name, name, length);
        routine->name[length] = '\0';
        for (uint32_t i = 0; i < length; i++) {
            const char c = routine->name[i];
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                               c == '_' || c == '-';
            if (!plain) return refuse(message, message_size, "a routine's name in it is not letters, digits, _ and -");
        }
        routine->bank = (uint16_t)take(&reader, 2);
        routine->address = (uint16_t)take(&reader, 2);
        if (routine->address >= 0x8000u) return refuse(message, message_size, "a routine in it is not in the ROM");
        if (!take_expr(&reader, &routine->stream_bank) || !take_expr(&reader, &routine->stream_address)) {
            return refuse(message, message_size, "it says where a stream starts in a way this game does not know");
        }
        routine->segments = (uint8_t)take(&reader, 1);
        if (routine->segments == 0 || routine->segments > MAX_SEGMENTS) {
            return refuse(message, message_size, "a routine in it makes nothing, or too many pieces");
        }
        routine->made_total = 0;
        for (unsigned s = 0; s < routine->segments; s++) {
            if (!take_expr(&reader, &routine->made_address[s])) {
                return refuse(message, message_size, "it says where a routine's output lies in a way this game does not know");
            }
            routine->made_size[s] = (uint16_t)take(&reader, 2);
            if (routine->made_size[s] == 0) return refuse(message, message_size, "a routine in it makes a piece of no bytes");
            routine->made_total += routine->made_size[s];
        }
        if (routine->made_total > MAX_MADE) return refuse(message, message_size, "a routine in it makes more than this game keeps");
    }
    if (reader.short_of) return refuse(message, message_size, "it is cut short");
    unpacked->routine_count = routines;

    unpacked->entries = entries ? (Entry*)calloc(entries, sizeof(Entry)) : NULL;
    if (entries && !unpacked->entries) return refuse(message, message_size, "out of memory");
    for (uint32_t e = 0; e < entries; e++) {
        Entry* entry = &unpacked->entries[e];
        entry->routine = (uint16_t)take(&reader, 2);
        entry->bank = (uint16_t)take(&reader, 2);
        entry->address = (uint16_t)take(&reader, 2);
        const uint8_t* sha256 = take_bytes(&reader, 32);
        entry->size = take(&reader, 4);
        entry->bytes = take_bytes(&reader, entry->size);
        if (!sha256 || !entry->bytes) return refuse(message, message_size, "it is cut short");
        memcpy(entry->made_sha256, sha256, 32);
        if (entry->routine >= routines || entry->size != unpacked->routines[entry->routine].made_total) {
            return refuse(message, message_size, "a replacement in it is not the size of what its routine makes");
        }
    }
    unpacked->entry_count = entries;
    if (reader.at != size) return refuse(message, message_size, "there is more in it than it says");
    return true;
}

static void put(uint8_t* out, size_t* at, uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) out[(*at)++] = (uint8_t)(value >> (8u * i));
}

static void put_expr(uint8_t* out, size_t* at, const Expr* expr) {
    put(out, at, expr->kind, 1);
    put(out, at, expr->value, 2);
}

static int compare_sha256(const void* one, const void* other) {
    return memcmp(one, other, 32);
}

/* What tells this pack's replacements from any others, for a recorded run to
 * keep: a hash of each replacement with its routine written out in place of
 * the routine's number (where it is entered, where its stream starts, where
 * its output lies; not its name), and a hash of those hashes, sorted. So the
 * same replacements are the same to a run whatever else the pack watches
 * and in whatever order it lists them. */
static bool hash_replacements(Unpacked* unpacked) {
    if (!unpacked->entry_count) return true;
    uint8_t* each = (uint8_t*)malloc(unpacked->entry_count * 32u);
    if (!each) return false;
    for (size_t e = 0; e < unpacked->entry_count; e++) {
        const Entry* entry = &unpacked->entries[e];
        const Routine* routine = &unpacked->routines[entry->routine];
        uint8_t said[11 + MAX_SEGMENTS * 5 + 4 + 32 + 32];
        size_t at = 0;
        put(said, &at, routine->bank, 2);
        put(said, &at, routine->address, 2);
        put_expr(said, &at, &routine->stream_bank);
        put_expr(said, &at, &routine->stream_address);
        put(said, &at, routine->segments, 1);
        for (unsigned s = 0; s < routine->segments; s++) {
            put_expr(said, &at, &routine->made_address[s]);
            put(said, &at, routine->made_size[s], 2);
        }
        put(said, &at, entry->bank, 2);
        put(said, &at, entry->address, 2);
        memcpy(said + at, entry->made_sha256, 32);
        at += 32;
        gbrt_sha256(entry->bytes, entry->size, said + at);
        at += 32;
        gbrt_sha256(said, at, each + e * 32u);
    }
    qsort(each, unpacked->entry_count, 32, compare_sha256);
    gbrt_sha256(each, unpacked->entry_count * 32u, unpacked->replacements_sha256);
    free(each);
    return true;
}

size_t gbrt_unpacked_replacements(const GBContext* ctx, uint8_t sha256[32]) {
    const Unpacked* unpacked = ctx ? (const Unpacked*)ctx->unpacked : NULL;
    if (!unpacked || !unpacked->entry_count) return 0;
    if (sha256) memcpy(sha256, unpacked->replacements_sha256, 32);
    return unpacked->entry_count;
}

bool gbrt_unpacked_start(GBContext* ctx, const uint8_t* built_from, size_t size,
                         char* message, size_t message_size) {
    if (!gbrt_unpacked_requested()) return true;
    if (pack_path[0] == '\0') {
        return refuse(message, message_size, harvest_path[0] != '\0'
                      ? "--unpacked-harvest needs --unpacked FILE too, the pack that says which routines to keep the output of"
                      : "--unpacked-call needs --unpacked FILE too, the pack that says which routines to wait for");
    }
    gbrt_unpacked_destroy(ctx);
    Unpacked* unpacked = (Unpacked*)calloc(1, sizeof(Unpacked));
    if (!unpacked) return refuse(message, message_size, "out of memory");
    ctx->unpacked = unpacked;
    unpacked->rom_size = size;
    gbrt_sha256(built_from, size, unpacked->rom_sha256);

    FILE* file = fopen(pack_path, "rb");
    long length = -1;
    if (file && fseek(file, 0, SEEK_END) == 0) length = ftell(file);
    if (!file || length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file) fclose(file);
        gbrt_unpacked_destroy(ctx);
        return refuse(message, message_size, "cannot open it");
    }
    unpacked->pack = (uint8_t*)malloc(length ? (size_t)length : 1u);
    const bool whole = unpacked->pack && fread(unpacked->pack, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!whole || !read_pack(unpacked, (size_t)length, message, message_size)) {
        if (!whole) refuse(message, message_size, "cannot read it");
        gbrt_unpacked_destroy(ctx);
        return false;
    }
    if (unpacked->entry_count && gbrt_data_use_requested()) {
        /* A record of what the game does with its data is of the game as it is. */
        gbrt_unpacked_destroy(ctx);
        return refuse(message, message_size, "it has replacements in it, and --data-use records the game as it is");
    }
    uint32_t most = 1;
    for (unsigned r = 0; r < unpacked->routine_count; r++) {
        if (unpacked->routines[r].made_total > most) most = unpacked->routines[r].made_total;
    }
    unpacked->scratch = (uint8_t*)malloc(most);
    unpacked->harvesting = harvest_path[0] != '\0';
    if (!unpacked->scratch || !hash_replacements(unpacked)) {
        gbrt_unpacked_destroy(ctx);
        return refuse(message, message_size, "out of memory");
    }
    fprintf(stderr, "[GBRT] Unpacked: %u routine%s watched, %lu replacement%s%s\n", unpacked->routine_count,
            unpacked->routine_count == 1 ? "" : "s", (unsigned long)unpacked->entry_count,
            unpacked->entry_count == 1 ? "" : "s", unpacked->harvesting ? ", keeping what they make" : "");
    return true;
}

/* ---------------------------------------------------------------------------
 * The routine entered, and the routine returning
 * ------------------------------------------------------------------------- */

static bool evaluate(GBContext* ctx, const Expr* expr, uint16_t* value) {
    switch (expr->kind) {
        case EXPR_CONST: *value = expr->value; return true;
        case EXPR_REG8: {
            const uint8_t registers[7] = {ctx->a, ctx->b, ctx->c, ctx->d, ctx->e, ctx->h, ctx->l};
            *value = registers[expr->value];
            return true;
        }
        case EXPR_REG16: {
            const uint16_t pairs[3] = {ctx->bc, ctx->de, ctx->hl};
            *value = pairs[expr->value];
            return true;
        }
        case EXPR_RAM8: {
            const uint8_t* byte = gbrt_ram_byte(ctx, expr->value);
            if (!byte) return false;
            *value = *byte;
            return true;
        }
        case EXPR_RAM16: {
            const uint8_t* low = gbrt_ram_byte(ctx, expr->value);
            const uint8_t* high = gbrt_ram_byte(ctx, (uint16_t)(expr->value + 1u));
            if (!low || !high) return false;
            *value = (uint16_t)(*low | (*high << 8));
            return true;
        }
        case EXPR_MAPPED_BANK: *value = ctx->rom_bank; return true;
        default: return false;
    }
}

/* Frames the stack pointer has gone above were left some other way than by
 * the return this module watches for: a routine that never comes back. */
static void drop_left(GBContext* ctx, Unpacked* unpacked) {
    while (unpacked->pending_count && unpacked->pending[unpacked->pending_count - 1u].sp < ctx->sp) {
        unpacked->pending_count--;
    }
}

void gbrt_unpacked_arrive(GBContext* ctx, uint16_t target) {
    Unpacked* unpacked = (Unpacked*)ctx->unpacked;
    if (!unpacked) return;
    for (unsigned r = 0; r < unpacked->routine_count; r++) {
        Routine* routine = &unpacked->routines[r];
        if (routine->address != target || gb_resolve_rom_bank(ctx, target) != routine->bank) continue;
        drop_left(ctx, unpacked);
        /* A jump back to its own first instruction, or the routine calling
         * itself, is the same piece of work still going on. */
        bool going_on = false;
        for (unsigned p = 0; p < unpacked->pending_count; p++) {
            if (unpacked->pending[p].routine == r) going_on = true;
        }
        if (going_on || unpacked->pending_count == MAX_PENDING) break;
        Pending pending;
        pending.routine = (uint16_t)r;
        pending.sp = ctx->sp;
        bool known = evaluate(ctx, &routine->stream_bank, &pending.bank) &&
                     evaluate(ctx, &routine->stream_address, &pending.address);
        for (unsigned s = 0; known && s < routine->segments; s++) {
            known = evaluate(ctx, &routine->made_address[s], &pending.made_address[s]);
        }
        routine->arrived++;
        if (!known) {
            routine->nowhere++;
            break;
        }
        unpacked->pending[unpacked->pending_count++] = pending;
        set_watch(ctx, unpacked);
        break;
    }
}

/* The call --unpacked-call made is over, and the machine is to stop where it
 * is: a frame that is done is what the loop running it leaves at. */
static void call_over(GBContext* ctx, Unpacked* unpacked, uint8_t how) {
    unpacked->call_state = how;
    unpacked->call_took = ctx->total_cycles - unpacked->call_from;
    ctx->stopped = 1;
    ctx->frame_done = 1;
}

static void keep(Unpacked* unpacked, const Pending* pending, const uint8_t sha256[32], uint32_t size) {
    for (size_t i = 0; i < unpacked->made_count; i++) {
        Made* made = &unpacked->made[i];
        if (made->routine == pending->routine && made->bank == pending->bank &&
            made->address == pending->address && memcmp(made->sha256, sha256, 32) == 0) {
            made->times++;
            return;
        }
    }
    if (unpacked->made_count == MAX_HARVEST) {
        unpacked->made_dropped++;
        return;
    }
    if (unpacked->made_count == unpacked->made_room) {
        const size_t room = unpacked->made_room ? unpacked->made_room * 2u : 64u;
        Made* grown = (Made*)realloc(unpacked->made, room * sizeof(Made));
        if (!grown) {
            unpacked->made_dropped++;
            return;
        }
        unpacked->made = grown;
        unpacked->made_room = room;
    }
    Made* made = &unpacked->made[unpacked->made_count];
    made->bytes = (uint8_t*)malloc(size);
    if (!made->bytes) {
        unpacked->made_dropped++;
        return;
    }
    memcpy(made->bytes, unpacked->scratch, size);
    made->routine = pending->routine;
    made->bank = pending->bank;
    made->address = pending->address;
    memcpy(made->sha256, sha256, 32);
    made->times = 1;
    unpacked->made_count++;
}

void gbrt_unpacked_return(GBContext* ctx) {
    Unpacked* unpacked = (Unpacked*)ctx->unpacked;
    if (!unpacked) {
        ctx->unpacked_watch = 0u;
        return;
    }
    drop_left(ctx, unpacked);
    if (!unpacked->pending_count || unpacked->pending[unpacked->pending_count - 1u].sp != ctx->sp) {
        /* The function that was called for the game is going back itself,
         * and no routine of the pack has returned. */
        if (unpacked->call_state == CALL_GOING && !unpacked->pending_count && ctx->sp >= unpacked->call_sp) {
            call_over(ctx, unpacked, CALL_CAME_BACK);
        }
        set_watch(ctx, unpacked);
        return;
    }
    const Pending pending = unpacked->pending[--unpacked->pending_count];
    Routine* routine = &unpacked->routines[pending.routine];
    routine->returned++;
    if (unpacked->call_state == CALL_GOING) {
        unpacked->call_routine = pending.routine;
        call_over(ctx, unpacked, CALL_RETURNED);
    }
    set_watch(ctx, unpacked);

    /* What it made, as it lies in RAM now. */
    uint32_t at = 0;
    for (unsigned s = 0; s < routine->segments; s++) {
        for (uint32_t i = 0; i < routine->made_size[s]; i++) {
            const uint8_t* byte = gbrt_ram_byte(ctx, (uint16_t)(pending.made_address[s] + i));
            if (!byte) {
                routine->nowhere++;
                return;
            }
            unpacked->scratch[at++] = *byte;
        }
    }
    uint8_t sha256[32];
    gbrt_sha256(unpacked->scratch, at, sha256);
    if (unpacked->harvesting) keep(unpacked, &pending, sha256, at);

    const Entry* replacement = NULL;
    bool for_stream = false;
    for (size_t e = 0; e < unpacked->entry_count; e++) {
        const Entry* entry = &unpacked->entries[e];
        if (entry->routine != pending.routine || entry->bank != pending.bank || entry->address != pending.address) continue;
        for_stream = true;
        if (memcmp(entry->made_sha256, sha256, 32) == 0) {
            replacement = entry;
            break;
        }
    }
    if (!replacement) {
        if (for_stream) routine->made_other++;
        return;
    }
    at = 0;
    for (unsigned s = 0; s < routine->segments; s++) {
        for (uint32_t i = 0; i < routine->made_size[s]; i++) {
            uint8_t* byte = gbrt_ram_byte(ctx, (uint16_t)(pending.made_address[s] + i));
            if (byte) *byte = replacement->bytes[at];
            at++;
        }
    }
    routine->replaced++;
}

/* ---------------------------------------------------------------------------
 * A function called for the game: --unpacked-call
 * ------------------------------------------------------------------------- */

static bool hex_number(const char** at, unsigned long most, unsigned long* value) {
    const char* start = *at;
    char* end = NULL;
    const char c = (char)(*start | 0x20);
    if (!((*start >= '0' && *start <= '9') || (c >= 'a' && c <= 'f'))) return false;
    *value = strtoul(start, &end, 16);
    *at = end;
    return *value <= most;
}

bool gbrt_unpacked_set_call(const char* spec) {
    static const char* const registers[10] = {"a", "b", "c", "d", "e", "h", "l", "bc", "de", "hl"};
    const char* at = spec ? spec : "";
    unsigned long bank = 0, address = 0;
    call.requested = false;
    call.set_count = 0;
    if (!hex_number(&at, 0x1FFu, &bank) || *at != ':') return false;
    at++;
    if (!hex_number(&at, 0x7FFFu, &address)) return false;
    while (*at == ',') {
        char name[8];
        size_t length = 0;
        at++;
        while (at[length] && at[length] != '=' && at[length] != ',' && length < sizeof(name) - 1u) {
            name[length] = at[length];
            length++;
        }
        name[length] = '\0';
        if (at[length] != '=' || call.set_count == MAX_CALL_SETS) return false;
        at += length + 1u;
        CallSet* set = &call.sets[call.set_count];
        unsigned long value = 0, where = 0;
        unsigned found = 10;
        for (unsigned r = 0; r < 10; r++) {
            if (strcmp(name, registers[r]) == 0) found = r;
        }
        const char* digits = name;
        if (found < 10) {
            set->kind = found < 7 ? SET_REG8 : SET_REG16;
            set->where = (uint16_t)(found < 7 ? found : found - 7u);
            if (!hex_number(&at, found < 7 ? 0xFFu : 0xFFFFu, &value)) return false;
        } else if (strcmp(name, "ime") == 0) {
            set->kind = SET_IME;
            set->where = 0;
            if (!hex_number(&at, 1u, &value)) return false;
        } else if (length == 4 && hex_number(&digits, 0xFFFFu, &where) && *digits == '\0') {
            /* A byte at an address: of RAM, or below 8000 a write to the cartridge. */
            set->kind = where < 0x8000u ? SET_CARTRIDGE : SET_RAM;
            set->where = (uint16_t)where;
            if (!hex_number(&at, 0xFFu, &value)) return false;
        } else {
            return false;
        }
        set->value = (uint16_t)value;
        call.set_count++;
    }
    if (*at != '\0') return false;
    call.bank = (uint16_t)bank;
    call.address = (uint16_t)address;
    call.requested = true;
    return true;
}

void gbrt_unpacked_set_call_cycles(uint64_t cycles) {
    call.cycles = cycles ? cycles : CALL_CYCLES;
}

bool gbrt_unpacked_call_requested(void) {
    return call.requested;
}

bool gbrt_unpacked_call_start(GBContext* ctx, char* message, size_t message_size) {
    Unpacked* unpacked = (Unpacked*)ctx->unpacked;
    if (!call.requested) return true;
    if (!unpacked) return refuse(message, message_size, "no pack is loaded that says which routines to wait for");
    for (unsigned s = 0; s < call.set_count; s++) {
        const CallSet* set = &call.sets[s];
        switch (set->kind) {
            case SET_REG8: {
                uint8_t* const registers[7] = {&ctx->a, &ctx->b, &ctx->c, &ctx->d, &ctx->e, &ctx->h, &ctx->l};
                *registers[set->where] = (uint8_t)set->value;
                break;
            }
            case SET_REG16: {
                uint16_t* const pairs[3] = {&ctx->bc, &ctx->de, &ctx->hl};
                *pairs[set->where] = set->value;
                break;
            }
            case SET_RAM: {
                uint8_t* byte = gbrt_ram_byte(ctx, set->where);
                if (!byte) {
                    snprintf(message, message_size, "%04X is not a byte of RAM this game can be given", set->where);
                    return false;
                }
                *byte = (uint8_t)set->value;
                break;
            }
            case SET_CARTRIDGE:
                gb_write8(ctx, set->where, (uint8_t)set->value);
                break;
            default:
                ctx->ime = (uint8_t)set->value;
                ctx->ime_pending = 0;
                break;
        }
    }
    const uint16_t mapped = gb_resolve_rom_bank(ctx, call.address);
    if (mapped != call.bank) {
        snprintf(message, message_size,
                 "the function is in bank %02X, and bank %02X is mapped at %04X: map its bank first, with a write to "
                 "the cartridge", call.bank, mapped, call.address);
        return false;
    }
    /* The return address is where the game was. It is never gone back to. */
    uint8_t* low = gbrt_ram_byte(ctx, (uint16_t)(ctx->sp - 2u));
    uint8_t* high = gbrt_ram_byte(ctx, (uint16_t)(ctx->sp - 1u));
    if (!low || !high) {
        snprintf(message, message_size, "the stack pointer is %04X, and a return address cannot be pushed there",
                 ctx->sp);
        return false;
    }
    *low = (uint8_t)(ctx->pc & 0xFFu);
    *high = (uint8_t)(ctx->pc >> 8);
    ctx->sp = (uint16_t)(ctx->sp - 2u);
    ctx->pc = call.address;
    ctx->halted = 0;
    ctx->halt_bug = 0;
    ctx->stop_mode_active = 0;
    unpacked->pending_count = 0;
    unpacked->call_state = CALL_GOING;
    unpacked->call_sp = ctx->sp;
    unpacked->call_from = ctx->total_cycles;
    unpacked->call_took = 0;
    set_watch(ctx, unpacked);
    /* The function may be one of the routines itself. */
    gbrt_unpacked_arrive(ctx, call.address);
    fprintf(stderr, "[GBRT] Unpacked: calling %02X:%04X for the game, and waiting %llu cycles for a routine of the "
                    "pack to return\n", call.bank, call.address, (unsigned long long)call.cycles);
    return true;
}

uint32_t gbrt_unpacked_call_slice(GBContext* ctx, uint32_t budget) {
    Unpacked* unpacked = (Unpacked*)ctx->unpacked;
    if (!unpacked || unpacked->call_state != CALL_GOING) return budget;
    /* A game with its screen off never ends a frame, so it is run a piece at
     * a time, and gbrt_unpacked_call_over looks at the cycles after each. */
    const uint64_t spent = ctx->total_cycles - unpacked->call_from;
    const uint64_t left = spent < call.cycles ? call.cycles - spent : 0u;
    const uint32_t piece = left > 0x1000000u ? 0x1000000u : left < 4u ? 4u : (uint32_t)left;
    return budget == 0u || budget > piece ? piece : budget;
}

bool gbrt_unpacked_call_over(GBContext* ctx) {
    Unpacked* unpacked = (Unpacked*)ctx->unpacked;
    if (!unpacked || unpacked->call_state == CALL_NONE) return false;
    if (unpacked->call_state == CALL_GOING && ctx->total_cycles - unpacked->call_from >= call.cycles) {
        call_over(ctx, unpacked, CALL_GAVE_UP);
    }
    return unpacked->call_state != CALL_GOING;
}

bool gbrt_unpacked_call_report(GBContext* ctx) {
    const Unpacked* unpacked = (const Unpacked*)ctx->unpacked;
    if (!unpacked || unpacked->call_state == CALL_NONE) return true;
    const unsigned long long took = (unsigned long long)unpacked->call_took;
    switch (unpacked->call_state) {
        case CALL_RETURNED:
            fprintf(stderr, "[UNPACKED] call of %02X:%04X: %s returned after %llu cycles\n", call.bank, call.address,
                    unpacked->routines[unpacked->call_routine].name, took);
            return true;
        case CALL_CAME_BACK:
            fprintf(stderr, "[UNPACKED] call of %02X:%04X: it came back after %llu cycles, and no routine of the "
                            "pack had returned\n", call.bank, call.address, took);
            return false;
        case CALL_GAVE_UP:
            fprintf(stderr, "[UNPACKED] call of %02X:%04X: no routine of the pack returned in the %llu cycles it "
                            "was given\n", call.bank, call.address, (unsigned long long)call.cycles);
            return false;
        default:
            fprintf(stderr, "[UNPACKED] call of %02X:%04X: no routine of the pack had returned when the game "
                            "stopped\n", call.bank, call.address);
            return false;
    }
}

/* ---------------------------------------------------------------------------
 * The report and the harvest
 * ------------------------------------------------------------------------- */

void gbrt_unpacked_report(GBContext* ctx) {
    const Unpacked* unpacked = (const Unpacked*)ctx->unpacked;
    if (!unpacked) {
        fprintf(stderr, "[UNPACKED] no pack was loaded\n");
        return;
    }
    for (unsigned r = 0; r < unpacked->routine_count; r++) {
        const Routine* routine = &unpacked->routines[r];
        fprintf(stderr,
                "[UNPACKED] %s at %02X:%04X: entered=%llu returned=%llu replaced=%llu made_something_else=%llu "
                "not_in_reach=%llu\n",
                routine->name, routine->bank, routine->address, (unsigned long long)routine->arrived,
                (unsigned long long)routine->returned, (unsigned long long)routine->replaced,
                (unsigned long long)routine->made_other, (unsigned long long)routine->nowhere);
    }
    if (unpacked->harvesting) {
        fprintf(stderr, "[UNPACKED] kept %lu different things made%s\n", (unsigned long)unpacked->made_count,
                unpacked->made_dropped ? ", and dropped some for want of room" : "");
    }
}

static void write_hex(FILE* out, const uint8_t* bytes, size_t count) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; i++) {
        fputc(digits[bytes[i] >> 4], out);
        fputc(digits[bytes[i] & 15u], out);
    }
}

bool gbrt_unpacked_write(GBContext* ctx) {
    const Unpacked* unpacked = (const Unpacked*)ctx->unpacked;
    if (!unpacked || !unpacked->harvesting) return true;
    FILE* out = fopen(harvest_path, "wb");
    if (!out) {
        fprintf(stderr, "[GBRT] Cannot write what was unpacked to %s\n", harvest_path);
        return false;
    }
    fprintf(out, "{\"schema\": \"gbrt.unpacked-harvest\", \"version\": 1,\n \"rom\": {\"size\": %lu, \"sha256\": \"",
            (unsigned long)unpacked->rom_size);
    write_hex(out, unpacked->rom_sha256, 32);
    /* What a routine makes of a stream the image changed is not the game's own. */
    fprintf(out, "\"},\n \"image_changed\": %lu, \"dropped\": %lu,\n \"unpackers\": [",
            (unsigned long)gbrt_rom_image_changed(), (unsigned long)unpacked->made_dropped);
    for (unsigned r = 0; r < unpacked->routine_count; r++) {
        const Routine* routine = &unpacked->routines[r];
        fprintf(out, "%s\n  {\"name\": \"%s\", \"bank\": %u, \"address\": %u, \"entered\": %llu, \"returned\": %llu, "
                     "\"not_in_reach\": %llu}",
                r ? "," : "", routine->name, routine->bank, routine->address, (unsigned long long)routine->arrived,
                (unsigned long long)routine->returned, (unsigned long long)routine->nowhere);
    }
    fprintf(out, "],\n \"made\": [");
    for (size_t i = 0; i < unpacked->made_count; i++) {
        const Made* made = &unpacked->made[i];
        const Routine* routine = &unpacked->routines[made->routine];
        fprintf(out, "%s\n  {\"unpacker\": \"%s\", \"bank\": %u, \"address\": %u, \"times\": %lu, \"sha256\": \"",
                i ? "," : "", routine->name, made->bank, made->address, (unsigned long)made->times);
        write_hex(out, made->sha256, 32);
        fprintf(out, "\", \"bytes\": \"");
        write_hex(out, made->bytes, routine->made_total);
        fprintf(out, "\"}");
    }
    fprintf(out, "]}\n");
    const bool written = !ferror(out);
    if (fclose(out) != 0 || !written) {
        fprintf(stderr, "[GBRT] Cannot write what was unpacked to %s\n", harvest_path);
        return false;
    }
    return true;
}
