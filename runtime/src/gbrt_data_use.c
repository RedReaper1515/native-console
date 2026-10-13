/**
 * gbrt_data_use.c - following the bytes a game reads from its ROM as data
 * to where it uses them. gbrt_data_use.h says what for.
 */

#include "gbrt_data_use.h"

#include "gbrt_texture_pack.h"
#include "ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What a byte, or a value worked out from it, was used for. */
enum {
    USE_TILES = 0x01,    /* written to tile data, 8000-97FF */
    USE_TILE_MAP = 0x02, /* written to a tile map, 9800-9FFF */
    USE_OAM = 0x04,      /* written to the sprite table */
    USE_SOUND = 0x08,    /* written to a sound register or the wave table */
    USE_PALETTE = 0x10,  /* written to a palette register */
    USE_BANK = 0x20,     /* written to the cartridge: a bank number, mostly */
    USE_JUMP = 0x40,     /* half of an address the game jumped to */
    USE_POINTER = 0x80,  /* half of an address the game read or wrote through */
};

static const char* const kUseNames[8] = {
    "tiles", "tile_map", "oam", "sound", "palette", "bank", "jump", "pointer",
};

/* A tag says which ROM byte a value came from: its offset plus one, with
 * the top bit set once the value is no longer the byte itself. 0: none. */
#define TAG_COMPUTED 0x80000000u

/* Following one reading instruction (--data-use-follow): the calls that were
 * in progress while it read. The limits, and what happens past each:        */
#define FOLLOW_READERS 4u     /* instructions followed at once: more are refused */
#define FOLLOW_DEPTH 256u     /* calls inside one another: deeper ones are not followed, and counted */
#define FOLLOW_PIECES 4u      /* runs of a reader's bytes kept for one call: more make it "several" */
#define FOLLOW_STREAMS 8u     /* runs one reader can be part way through: the stalest is forgotten */
#define FOLLOW_PENDING 64u    /* calls waiting to hear whether their stream went on: more are "undecided" */
#define FOLLOW_FUNCTIONS 256u /* functions kept for one reader: more are counted */
#define FOLLOW_SAMPLES 16u    /* calls of one function written out whole: more are counted */
#define FOLLOW_HELD 16u       /* words of RAM noted as holding where a stream starts: more are flagged */
#define FOLLOW_INSIDE 8u      /* functions noted as the one a function was called in: more are counted */
#define FOLLOW_RUNS 32u       /* runs of RAM written, kept for one sample: the smallest are dropped, and counted */

/* How a function was entered. */
enum { ENTERED_CALL, ENTERED_JUMP, ENTERED_RELATIVE, ENTERED_FALL, ENTERED_INTERRUPT, ENTERED_KINDS };
static const char* const kEnteredNames[ENTERED_KINDS] = {"call", "jump", "relative", "fall", "interrupt"};

/* What a call that read through the followed instruction read. */
enum {
    READ_WHOLE,     /* one run of bytes from its first, and nothing read on from its end afterwards */
    READ_JOINED,    /* one run, which went on from where an earlier call stopped, or which a later one went on from */
    READ_SEVERAL,   /* more than one run */
    READ_LEFT,      /* it did not come back to where it was called from */
    READ_UNDECIDED, /* one run from its first byte, and the record ended before anything showed whether it went on */
    READ_UNFINISHED, /* still in progress when the record ended */
    READ_KINDS
};
static const char* const kReadNames[READ_KINDS] = {"whole", "joined", "several", "left", "undecided", "unfinished"};

/* A run of ROM bytes a call read through the followed instruction. */
typedef struct {
    uint32_t stream;        /* which run of the reader's it is part of: a number */
    uint32_t first;         /* ROM offset of the first byte this call read of it */
    uint32_t end;           /* one past the last */
    uint16_t address;       /* the address that first byte was read at */
    uint8_t from_start;     /* that byte was the first anything read of the run */
} FollowPiece;

typedef struct {
    FollowPiece pieces[FOLLOW_PIECES];
    uint8_t count;
    uint8_t more;           /* runs that did not fit */
    uint8_t distance;       /* calls between this one and the instruction: 0 if it ran it itself */
    uint8_t searched;       /* RAM was searched for the start of the first run begun inside it */
    uint8_t held_count;
    uint8_t held_more;      /* more words held it than there was room for */
    uint16_t held[FOLLOW_HELD]; /* words of RAM that held that address when this call was entered */
} FollowRead;

/* A call in progress. It ends when the stack pointer passes `sp`, where its
 * return address is: so a function it jumps to is still this call, and one
 * that throws its return address away is over. */
typedef struct {
    uint16_t sp;
    uint16_t returns_to;
    uint16_t entry, entry_bank;
    uint16_t from, from_bank;   /* the instruction that called, or jumped */
    uint16_t bc, de, hl;        /* the registers as it was entered */
    uint16_t mapped_bank;       /* the ROM bank at 4000-7FFF then */
    uint32_t seq;               /* how many stores had been made then */
    uint8_t a;
    uint8_t kind;
    uint8_t level;              /* interrupts being served, one inside another */
    uint8_t arrived;            /* for an interrupt: how the instruction it broke in before was come to, */
    uint16_t last_pc, last_bank; /* and from where */
    FollowRead read[FOLLOW_READERS];
} FollowFrame;

typedef struct {
    uint16_t address, size;
    uint8_t bank;
} FollowRun;

/* One call, kept whole. */
typedef struct {
    uint16_t from, from_bank, bc, de, hl, sp, mapped_bank;
    uint8_t a, kind, read, piece_count, more, held_count, held_more;
    FollowPiece pieces[FOLLOW_PIECES];
    uint16_t held[FOLLOW_HELD];
    uint16_t run_count;
    uint32_t runs_dropped, bytes_dropped;
    FollowRun runs[FOLLOW_RUNS];
} FollowSample;

/* What one function's calls read through one followed instruction. */
typedef struct {
    uint16_t bank, address;
    uint32_t read[READ_KINDS];
    uint32_t samples_dropped, inside_dropped;
    uint8_t distance, inside_count, sample_count;
    struct { uint16_t bank, address; uint32_t times; } inside[FOLLOW_INSIDE];
    FollowSample samples[FOLLOW_SAMPLES];
} FollowStats;

typedef struct {
    uint32_t where;             /* bank << 16 | address, plus one: as `reader` below */
    uint64_t bytes;
    uint32_t streams, next_stream, clock;
    struct { uint32_t id, end, used; } open[FOLLOW_STREAMS];
    struct { uint32_t stream; FollowStats* stats; FollowSample* sample; uint8_t level; } pending[FOLLOW_PENDING];
    unsigned pending_count;
    FollowStats* functions[FOLLOW_FUNCTIONS];
    unsigned function_count;
    uint32_t functions_dropped;
} FollowReader;

/* A function: an address something called. */
typedef struct {
    uint16_t bank, address;
    uint32_t by[ENTERED_KINDS];
} FollowFunction;

typedef struct {
    unsigned reader_count;
    FollowReader readers[FOLLOW_READERS];
    FollowFrame frames[FOLLOW_DEPTH];
    unsigned depth;
    uint64_t too_deep;
    uint32_t* function_at;      /* per ROM byte, then per address from 8000: its place in `functions`, plus one */
    FollowFunction* functions;
    uint32_t function_count, function_room;
    uint32_t seq;               /* stores made so far */
    uint32_t* changed;          /* per address from 8000: `seq` at the last store to it */
    uint8_t* level_at;          /* ...the interrupt level it was made at, or FOLLOW_PUSHED */
    uint8_t* bank_at;           /* ...and the bank that was mapped there */
    uint8_t level;
    uint8_t arrived;            /* how the instruction about to run was come to */
    uint8_t awaiting_vector;    /* an interrupt was taken: 1, and 2 if it has a frame */
    uint8_t finished;
    uint16_t last_pc, last_bank;
} Follow;

#define FOLLOW_PUSHED 0xFFu     /* the stack wrote it: no store of the game's */

/* What the screen showed of the ROM's bytes, looked at once a frame. An
 * entry is one thing seen of one byte, and counts the frames it was seen in. */
enum {
    SHOWN_BACKGROUND,   /* a byte of a tile the background or the window drew, through the palette in `detail` */
    SHOWN_OBJECTS,      /* ...that a sprite drew */
    /* A byte in a tile map, in a cell that was on the screen. These count cells, not frames: */
    SHOWN_CELL,         /* the cells it was in */
    SHOWN_LEFT,         /* ...of those, the ones whose cell to the left was on the screen too */
    SHOWN_AFTER,        /* ......and held the byte before this one in the ROM: they are in one row */
    SHOWN_ABOVE,        /* ...and the ones whose cell above was on the screen */
    SHOWN_UNDER,        /* ......and held the ROM byte `detail` before this one: a row is `detail` tiles */
    SHOWN_STOOD,        /* ...and the tile its number drew: the ROM tile at `detail` >> 8, of `detail` & 0xFF bytes */
};
#define SHOWN_CELLS 2048u       /* the cells of the two tile maps */
#define SHOWN_COLOUR 0x80u      /* `detail` is the four colours of a Game Boy Color palette, 16 bits each, and
                                 * not the value of a palette register */
#define SHOWN_KEYS 0x80000u     /* objects or not, the bank, the tile and the palette of a pixel: 19 bits */

typedef struct {
    uint32_t offset1;       /* the ROM byte, plus one. 0: no entry */
    uint32_t frames;
    uint32_t stamp;         /* the last frame that counted, so that a frame counts once */
    uint8_t what;
    uint8_t planes;         /* 1 where the byte was the low bits of a row of pixels, 2 the high bits */
    uint64_t detail;
} ShownEntry;

typedef struct {
    ShownEntry* entries;
    size_t room;            /* a power of two */
    size_t count;
    uint32_t frames;        /* frames looked at */
    uint8_t failed;         /* there was no memory for more: the record says so */
    uint32_t* tags;         /* what `mem` holds of video memory, for each of its two banks */
    uint32_t* own_sources;  /* what drew each pixel, where nothing else has the PPU note it */
    uint8_t* drawn;         /* in the frame being looked at: a bit for each of SHOWN_KEYS that drew a pixel */
    uint32_t* list;         /* ...and those, one after another */
    uint8_t* cells;         /* ...and for each cell of the tile maps, whether it was on the screen */
    uint16_t* cell_tile;    /* ......the bank and the tile that drew it */
    uint16_t* cell_list;    /* ......and those cells, one after another */
    unsigned cell_count;
} Shown;

/* Where in a tile a byte landed (--data-use-tiles). A byte that reaches the
 * tile area as it is lands at some row of some tile, and the byte after it
 * in the ROM lands at the next address if the tiles are 2 bits a pixel, or
 * two on if they are 1 bit a pixel and the game makes the other half of
 * each row itself. That is what a picture of them has to be told. */
enum {
    TILE_PUT = 0x001,          /* it reached the tile area as it is */
    TILE_NEXT = 0x002,         /* ...at the address after the one the byte before it in the ROM was at */
    TILE_SKIP = 0x004,         /* ...or two after it */
    TILE_BEGUN = 0x008,        /* ...or neither: a copy began with it */
    TILE_BEGUN_TOLD = 0x010,   /* ...one that read it at an address the ROM has as a word */
    TILE_BEGUN_WORKED = 0x020, /* ...or at one worked out from what the ROM has */
    TILE_MIXED = 0x040,        /* it landed at more than one place in a tile */
    TILE_READ_TOLD = 0x080,    /* the address it was last read at: a word of the ROM as it is */
    TILE_READ_WORKED = 0x100,  /* ...or one worked out */
};

typedef struct {
    uint16_t* at;           /* per ROM byte: where in the tile area it last landed, or 0 */
    uint16_t* how;          /* per ROM byte: TILE_* */
} TileUse;

typedef struct {
    size_t rom_size;
    uint32_t reg[8];        /* b c d e h l - a, as the opcodes number them */
    uint32_t sp[2];         /* what the halves of the stack pointer came from: high, low */
    uint32_t sp_set;        /* the instruction that last set the stack pointer: as `reader` below */
    uint32_t read_as;       /* while the stack is read out of the ROM, that instruction: the reader of the bytes */
    uint32_t* mem;          /* for every address from 8000 up */
    uint8_t* direct;        /* per ROM byte: what the byte itself was used for */
    uint8_t* computed;      /* ...and what a value worked out from it was */
    uint8_t* reads;         /* how many instructions read it: 0, 1, or 2 for more */
    uint32_t* reader;       /* the first of them: bank << 16 | address, plus one */
    uint64_t instructions;
    Follow* follow;         /* NULL unless an instruction is followed */
    struct Pointers* pointers; /* NULL unless the pointers were asked for */
    Shown* shown;           /* what the screen showed */
    TileUse* tiles;         /* NULL unless where tiles land is recorded */
} DataUse;

/* --data-use-pointers, which is at the end of this file. */
static bool pointers_create(DataUse* use);
static void pointers_destroy(DataUse* use);
static void pointers_copied(DataUse* use, GBContext* ctx, uint16_t source, uint16_t target, unsigned count);
static void pointers_interrupt(DataUse* use, const GBContext* ctx);
static void pointers_write(FILE* out, const DataUse* use, const GBContext* ctx);

static char* g_path = NULL;
static uint32_t g_follow[FOLLOW_READERS];   /* bank << 16 | address */
static unsigned g_follow_count = 0;
static bool g_pointers = false;
static bool g_tiles = false;

void gbrt_data_use_set_file(const char* path) {
    free(g_path);
    g_path = NULL;
    if (path && path[0]) {
        const size_t length = strlen(path) + 1u;
        g_path = (char*)malloc(length);
        if (g_path) memcpy(g_path, path, length);
    }
}

bool gbrt_data_use_requested(void) {
    return g_path != NULL;
}

bool gbrt_data_use_follow(const char* where) {
    if (!where) return false;
    char* end = NULL;
    const unsigned long bank = strtoul(where, &end, 16);
    if (end == where || *end != ':') return false;
    const char* second = end + 1;
    const unsigned long address = strtoul(second, &end, 16);
    if (end == second || *end != '\0' || bank > 0x1FFu || address > 0xFFFFu) return false;
    const uint32_t wanted = ((uint32_t)bank << 16) | (uint32_t)address;
    for (unsigned i = 0; i < g_follow_count; ++i) {
        if (g_follow[i] == wanted) return true;
    }
    if (g_follow_count >= FOLLOW_READERS) return false;
    g_follow[g_follow_count++] = wanted;
    return true;
}

bool gbrt_data_use_follow_requested(void) {
    return g_follow_count != 0;
}

void gbrt_data_use_tiles(void) {
    g_tiles = true;
}

bool gbrt_data_use_tiles_requested(void) {
    return g_tiles;
}

static void tiles_destroy(TileUse* tiles) {
    if (!tiles) return;
    free(tiles->at);
    free(tiles->how);
    free(tiles);
}

static TileUse* tiles_create(size_t rom_size) {
    TileUse* tiles = (TileUse*)calloc(1, sizeof(TileUse));
    if (!tiles) return NULL;
    tiles->at = (uint16_t*)calloc(rom_size, sizeof(uint16_t));
    tiles->how = (uint16_t*)calloc(rom_size, sizeof(uint16_t));
    if (!tiles->at || !tiles->how) {
        tiles_destroy(tiles);
        return NULL;
    }
    return tiles;
}

static void follow_destroy(Follow* follow) {
    if (!follow) return;
    for (unsigned r = 0; r < follow->reader_count; ++r) {
        for (unsigned i = 0; i < follow->readers[r].function_count; ++i) free(follow->readers[r].functions[i]);
    }
    free(follow->function_at);
    free(follow->functions);
    free(follow->changed);
    free(follow->level_at);
    free(follow->bank_at);
    free(follow);
}

static Follow* follow_create(size_t rom_size) {
    Follow* follow = (Follow*)calloc(1, sizeof(Follow));
    if (!follow) return NULL;
    follow->reader_count = g_follow_count;
    for (unsigned r = 0; r < g_follow_count; ++r) follow->readers[r].where = g_follow[r] + 1u;
    follow->function_at = (uint32_t*)calloc(rom_size + 0x8000u, sizeof(uint32_t));
    follow->changed = (uint32_t*)calloc(0x8000u, sizeof(uint32_t));
    follow->level_at = (uint8_t*)calloc(0x8000u, 1);
    follow->bank_at = (uint8_t*)calloc(0x8000u, 1);
    follow->arrived = ENTERED_FALL;
    if (!follow->function_at || !follow->changed || !follow->level_at || !follow->bank_at) {
        follow_destroy(follow);
        return NULL;
    }
    return follow;
}

static void shown_destroy(Shown* shown) {
    if (!shown) return;
    if (gbrt_pixel_sources == shown->own_sources) gbrt_pixel_sources = NULL;
    free(shown->entries);
    free(shown->tags);
    free(shown->own_sources);
    free(shown->drawn);
    free(shown->list);
    free(shown->cells);
    free(shown->cell_tile);
    free(shown->cell_list);
    free(shown);
}

static Shown* shown_create(void) {
    Shown* shown = (Shown*)calloc(1, sizeof(Shown));
    if (!shown) return NULL;
    shown->room = 1024u;
    shown->entries = (ShownEntry*)calloc(shown->room, sizeof(ShownEntry));
    shown->tags = (uint32_t*)calloc(2u * VRAM_SIZE, sizeof(uint32_t));
    shown->own_sources = (uint32_t*)calloc(GB_FRAMEBUFFER_SIZE, sizeof(uint32_t));
    shown->drawn = (uint8_t*)calloc(SHOWN_KEYS / 8u, 1);
    shown->list = (uint32_t*)calloc(GB_FRAMEBUFFER_SIZE, sizeof(uint32_t));
    shown->cells = (uint8_t*)calloc(SHOWN_CELLS, 1);
    shown->cell_tile = (uint16_t*)calloc(SHOWN_CELLS, sizeof(uint16_t));
    shown->cell_list = (uint16_t*)calloc(SHOWN_CELLS, sizeof(uint16_t));
    if (!shown->entries || !shown->tags || !shown->own_sources || !shown->drawn || !shown->list || !shown->cells ||
        !shown->cell_tile || !shown->cell_list) {
        shown_destroy(shown);
        return NULL;
    }
    /* The PPU notes what drew each pixel where it is given somewhere to: a
     * texture pack's, if there is one, and otherwise here. */
    if (!gbrt_pixel_sources) gbrt_pixel_sources = shown->own_sources;
    return shown;
}

void gbrt_data_use_destroy(GBContext* ctx) {
    DataUse* use = ctx ? (DataUse*)ctx->data_use : NULL;
    if (!use) return;
    shown_destroy(use->shown);
    follow_destroy(use->follow);
    pointers_destroy(use);
    tiles_destroy(use->tiles);
    free(use->mem);
    free(use->direct);
    free(use->computed);
    free(use->reads);
    free(use->reader);
    free(use);
    ctx->data_use = NULL;
}

bool gbrt_data_use_start(GBContext* ctx) {
    /* Following writes into the record: without one there is nothing to start. */
    if (!g_path) return g_follow_count == 0 && !g_pointers && !g_tiles;
    if (!ctx || ctx->data_use) return true;
    if (!ctx->rom || ctx->rom_size == 0) return false;
    DataUse* use = (DataUse*)calloc(1, sizeof(DataUse));
    if (!use) return false;
    use->rom_size = ctx->rom_size;
    use->mem = (uint32_t*)calloc(0x8000u, sizeof(uint32_t));
    use->direct = (uint8_t*)calloc(use->rom_size, 1);
    use->computed = (uint8_t*)calloc(use->rom_size, 1);
    use->reads = (uint8_t*)calloc(use->rom_size, 1);
    use->reader = (uint32_t*)calloc(use->rom_size, sizeof(uint32_t));
    if (g_follow_count) use->follow = follow_create(use->rom_size);
    const bool pointers_made = !g_pointers || pointers_create(use);
    use->shown = shown_create();
    if (g_tiles) use->tiles = tiles_create(use->rom_size);
    ctx->data_use = use;
    if (!use->mem || !use->direct || !use->computed || !use->reads || !use->reader ||
        (g_follow_count && !use->follow) || !pointers_made || !use->shown || (g_tiles && !use->tiles)) {
        gbrt_data_use_destroy(ctx);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Following one reading instruction: the calls in progress while it read.
 * ------------------------------------------------------------------------- */

static uint16_t follow_bank_of(const GBContext* ctx, uint16_t address) {
    return address >= 0x8000u ? 0u : gb_resolve_rom_bank(ctx, address);
}

/* A byte of work RAM or high RAM, as it is, without the bus noticing. */
static uint8_t follow_ram(const GBContext* ctx, uint16_t address) {
    if (address >= 0xC000u && address < 0xD000u) return ctx->wram[address - 0xC000u];
    if (address >= 0xD000u && address < 0xE000u) {
        return ctx->wram[(size_t)ctx->wram_bank * 0x1000u + (address - 0xD000u)];
    }
    if (address >= 0xFF80u && address < 0xFFFFu) return ctx->hram[address - 0xFF80u];
    return 0xFFu;
}

static uint16_t follow_ram_word(const GBContext* ctx, uint16_t address) {
    return (uint16_t)(follow_ram(ctx, address) | (follow_ram(ctx, (uint16_t)(address + 1u)) << 8));
}

static uint32_t* follow_function_at(const DataUse* use, uint16_t bank, uint16_t address) {
    if (address >= 0x8000u) return &use->follow->function_at[use->rom_size + (address - 0x8000u)];
    const size_t offset = (size_t)bank * 0x4000u + (address & 0x3FFFu);
    return offset < use->rom_size ? &use->follow->function_at[offset] : NULL;
}

/* Count an entry to the function at `address`. An address becomes a function
 * by being called: `make` says this entry is one. */
static void follow_entered(DataUse* use, uint16_t bank, uint16_t address, unsigned kind, bool make) {
    Follow* follow = use->follow;
    uint32_t* at = follow_function_at(use, bank, address);
    if (!at) return;
    if (!*at) {
        if (!make) return;
        if (follow->function_count == follow->function_room) {
            const uint32_t room = follow->function_room ? follow->function_room * 2u : 1024u;
            FollowFunction* grown = (FollowFunction*)realloc(follow->functions, (size_t)room * sizeof(FollowFunction));
            if (!grown) return;
            follow->functions = grown;
            follow->function_room = room;
        }
        FollowFunction* function = &follow->functions[follow->function_count];
        memset(function, 0, sizeof(*function));
        function->bank = bank;
        function->address = address;
        *at = ++follow->function_count;
    }
    follow->functions[*at - 1u].by[kind]++;
}

static FollowStats* follow_stats(FollowReader* reader, uint16_t bank, uint16_t address) {
    for (unsigned i = 0; i < reader->function_count; ++i) {
        if (reader->functions[i]->bank == bank && reader->functions[i]->address == address) return reader->functions[i];
    }
    if (reader->function_count >= FOLLOW_FUNCTIONS) return NULL;
    FollowStats* stats = (FollowStats*)calloc(1, sizeof(FollowStats));
    if (!stats) return NULL;
    stats->bank = bank;
    stats->address = address;
    stats->distance = 0xFFu;
    reader->functions[reader->function_count++] = stats;
    return stats;
}

static FollowPiece* follow_piece(FollowRead* read, uint32_t stream) {
    for (unsigned i = 0; i < read->count; ++i) {
        if (read->pieces[i].stream == stream) return &read->pieces[i];
    }
    return NULL;
}

/* The runs of RAM that stores were made to since `frame` was entered, by it
 * and what it called: not by the stack, and not by an interrupt that came
 * in between. Past FOLLOW_RUNS of them the smallest go. */
static void follow_wrote(const DataUse* use, const FollowFrame* frame, FollowSample* sample) {
    const Follow* follow = use->follow;
    unsigned address = 0x8000u;
    while (address < 0xFFFFu) {
        const unsigned index = address - 0x8000u;
        const unsigned area_end = address < 0xA000u ? 0xA000u : address < 0xC000u ? 0xC000u :
                                  address < 0xE000u ? 0xE000u : 0xFFFFu;
        if ((address >= 0xE000u && address < 0xFF80u) || follow->changed[index] <= frame->seq ||
            follow->level_at[index] != frame->level) {
            ++address;
            continue;
        }
        unsigned end = address + 1u;
        while (end < area_end && follow->changed[end - 0x8000u] > frame->seq &&
               follow->level_at[end - 0x8000u] == frame->level &&
               follow->bank_at[end - 0x8000u] == follow->bank_at[index]) {
            ++end;
        }
        const unsigned size = end - address;
        if (sample->run_count == FOLLOW_RUNS) {
            unsigned smallest = 0;
            for (unsigned i = 1; i < FOLLOW_RUNS; ++i) {
                if (sample->runs[i].size < sample->runs[smallest].size) smallest = i;
            }
            sample->runs_dropped++;
            if (sample->runs[smallest].size >= size) {
                sample->bytes_dropped += size;
                address = end;
                continue;
            }
            sample->bytes_dropped += sample->runs[smallest].size;
            memmove(&sample->runs[smallest], &sample->runs[smallest + 1u],
                    (FOLLOW_RUNS - 1u - smallest) * sizeof(FollowRun));
            sample->run_count--;
        }
        FollowRun* run = &sample->runs[sample->run_count++];
        run->address = (uint16_t)address;
        run->size = (uint16_t)size;
        run->bank = follow->bank_at[index];
        address = end;
    }
}

/* The call on top is over. `how` is READ_WHOLE if it came back to where it
 * was called from, READ_LEFT if the stack pointer passed its return address
 * some other way, READ_UNFINISHED if the record ended first. What it read
 * goes to the count of its function, and on to the call it was made in. */
static void follow_end(DataUse* use, unsigned how) {
    Follow* follow = use->follow;
    FollowFrame* frame = &follow->frames[follow->depth - 1u];
    FollowFrame* outer = follow->depth > 1u ? frame - 1 : NULL;
    for (unsigned r = 0; r < follow->reader_count; ++r) {
        FollowRead* read = &frame->read[r];
        if (!read->count) continue;
        FollowReader* reader = &follow->readers[r];
        FollowStats* stats = follow_stats(reader, frame->entry_bank, frame->entry);
        if (!stats) {
            reader->functions_dropped++;
        } else {
            if (read->distance < stats->distance) stats->distance = read->distance;
            const FollowPiece* piece = &read->pieces[0];
            unsigned said = how;
            bool waits = false;
            if (how == READ_WHOLE) {
                if (read->count > 1u || read->more) {
                    said = READ_SEVERAL;
                } else {
                    /* One run. Whole if it began the run and nothing has read on since; and
                     * nothing may read on later, which the next read of the reader's tells. */
                    said = READ_JOINED;
                    for (unsigned i = 0; i < FOLLOW_STREAMS && piece->from_start; ++i) {
                        if (reader->open[i].id == piece->stream && reader->open[i].end == piece->end) waits = true;
                    }
                }
            }
            FollowSample* sample = NULL;
            if (how != READ_UNFINISHED) {
                if (outer) {
                    unsigned i = 0;
                    while (i < stats->inside_count && (stats->inside[i].bank != outer->entry_bank ||
                                                       stats->inside[i].address != outer->entry)) {
                        ++i;
                    }
                    if (i < stats->inside_count) {
                        stats->inside[i].times++;
                    } else if (i < FOLLOW_INSIDE) {
                        stats->inside[i].bank = outer->entry_bank;
                        stats->inside[i].address = outer->entry;
                        stats->inside[i].times = 1;
                        stats->inside_count++;
                    } else {
                        stats->inside_dropped++;
                    }
                }
                /* A sample of each stream it was seen to start on, as far as there is room. */
                bool have = stats->sample_count >= FOLLOW_SAMPLES;
                for (unsigned i = 0; i < stats->sample_count && !have; ++i) {
                    have = stats->samples[i].pieces[0].first == piece->first;
                }
                if (have) {
                    stats->samples_dropped++;
                } else {
                    sample = &stats->samples[stats->sample_count++];
                    memset(sample, 0, sizeof(*sample));
                    sample->from = frame->from;
                    sample->from_bank = frame->from_bank;
                    sample->a = frame->a;
                    sample->bc = frame->bc;
                    sample->de = frame->de;
                    sample->hl = frame->hl;
                    sample->sp = frame->sp;
                    sample->mapped_bank = frame->mapped_bank;
                    sample->kind = frame->kind;
                    sample->piece_count = read->count;
                    sample->more = read->more;
                    memcpy(sample->pieces, read->pieces, sizeof(sample->pieces));
                    sample->held_count = read->held_count;
                    sample->held_more = read->held_more;
                    memcpy(sample->held, read->held, sizeof(sample->held));
                    follow_wrote(use, frame, sample);
                }
            }
            if (waits && reader->pending_count < FOLLOW_PENDING) {
                reader->pending[reader->pending_count].stream = piece->stream;
                reader->pending[reader->pending_count].stats = stats;
                reader->pending[reader->pending_count].sample = sample;
                reader->pending[reader->pending_count].level = frame->level;
                reader->pending_count++;
                if (sample) sample->read = READ_UNDECIDED;
            } else {
                if (waits) said = READ_UNDECIDED;
                stats->read[said]++;
                if (sample) sample->read = (uint8_t)said;
            }
        }
        /* What an interrupt's routine read, the code it interrupted did not. */
        if (outer && frame->kind != ENTERED_INTERRUPT) {
            FollowRead* into = &outer->read[r];
            for (unsigned i = 0; i < read->count; ++i) {
                FollowPiece* have = follow_piece(into, read->pieces[i].stream);
                if (have) {
                    if (read->pieces[i].end > have->end) have->end = read->pieces[i].end;
                } else if (into->count < FOLLOW_PIECES) {
                    into->pieces[into->count++] = read->pieces[i];
                } else {
                    into->more = 1;
                }
            }
            if (read->more) into->more = 1;
            if (read->distance + 1u < into->distance) into->distance = (uint8_t)(read->distance + 1u);
        }
    }
    if (frame->kind == ENTERED_INTERRUPT) {
        if (follow->level) follow->level--;
        /* The instruction the interrupt came in before is still to run, come to as it was. */
        follow->arrived = frame->arrived;
        follow->last_pc = frame->last_pc;
        follow->last_bank = frame->last_bank;
    }
    follow->depth--;
}

/* End the calls whose return address the stack pointer has passed. One came
 * back if the stack pointer is just past it and the game is where it said. */
static void follow_settle(DataUse* use, uint16_t sp, uint16_t pc) {
    Follow* follow = use->follow;
    /* A stack pointer in the ROM is no stack: the game is copying with it,
     * and the calls are where they were when it gets it back. */
    if (sp < 0x8000u) return;
    while (follow->depth) {
        const FollowFrame* frame = &follow->frames[follow->depth - 1u];
        const uint16_t above = (uint16_t)(sp - frame->sp);
        if (above == 0u || above >= 0x8000u) break;
        follow_end(use, above == 2u && frame->returns_to == pc ? READ_WHOLE : READ_LEFT);
    }
}

static FollowFrame* follow_push(DataUse* use, GBContext* ctx, unsigned kind, uint16_t entry, uint16_t sp,
                                uint16_t returns_to) {
    Follow* follow = use->follow;
    if (follow->depth >= FOLLOW_DEPTH) {
        follow->too_deep++;
        return NULL;
    }
    FollowFrame* frame = &follow->frames[follow->depth++];
    frame->sp = sp;
    frame->returns_to = returns_to;
    frame->entry = entry;
    frame->entry_bank = follow_bank_of(ctx, entry);
    frame->from = ctx->pc;
    frame->from_bank = follow_bank_of(ctx, ctx->pc);
    frame->a = ctx->a;
    frame->bc = ctx->bc;
    frame->de = ctx->de;
    frame->hl = ctx->hl;
    frame->mapped_bank = gb_resolve_rom_bank(ctx, 0x4000u);
    frame->seq = follow->seq;
    frame->kind = (uint8_t)kind;
    if (kind == ENTERED_INTERRUPT) follow->level++;
    frame->level = follow->level;
    frame->arrived = follow->arrived;
    frame->last_pc = follow->last_pc;
    frame->last_bank = follow->last_bank;
    for (unsigned r = 0; r < follow->reader_count; ++r) {
        frame->read[r].count = 0;
        frame->read[r].more = 0;
        frame->read[r].distance = 0xFFu;
        frame->read[r].searched = 0;
        frame->read[r].held_count = 0;
        frame->read[r].held_more = 0;
    }
    return frame;
}

/* A store the stack made: the bytes changed, and no instruction wrote them. */
static void follow_pushed(Follow* follow, uint16_t address) {
    if (address < 0x8000u) return;
    follow->changed[address - 0x8000u] = ++follow->seq;
    follow->level_at[address - 0x8000u] = FOLLOW_PUSHED;
}

/* A store an instruction made. */
static void follow_stored(DataUse* use, const GBContext* ctx, uint16_t address) {
    Follow* follow = use->follow;
    if (address < 0x8000u) return;
    const unsigned index = address - 0x8000u;
    follow->changed[index] = ++follow->seq;
    follow->level_at[index] = follow->level;
    /* Where a bank can be chosen: video memory, cartridge RAM, and the upper
     * half of work RAM on a Game Boy Color, whose bank 1 is the one a Game
     * Boy has there and is written 0 like the lower half. */
    follow->bank_at[index] = address < 0xA000u ? ctx->vram_bank :
                             address < 0xC000u ? ctx->ram_bank :
                             (address >= 0xD000u && address < 0xE000u && ctx->wram_bank > 1u) ? ctx->wram_bank : 0u;
}

/* The followed instruction `r` is reading the ROM byte at `offset`, mapped at `address`. */
static void follow_read(DataUse* use, GBContext* ctx, unsigned r, uint32_t offset, uint16_t address) {
    Follow* follow = use->follow;
    FollowReader* reader = &follow->readers[r];
    reader->bytes++;
    /* It goes on with a run it was part way through if this is the byte
     * after the last it read of one, or that byte again. Otherwise this is
     * the first byte of a new run. */
    int which = -1;
    for (unsigned i = 0; i < FOLLOW_STREAMS && which < 0; ++i) {
        if (reader->open[i].id && reader->open[i].end == offset) which = (int)i;
    }
    for (unsigned i = 0; i < FOLLOW_STREAMS && which < 0; ++i) {
        if (reader->open[i].id && reader->open[i].end == offset + 1u) which = (int)i;
    }
    const bool starts = which < 0;
    if (!starts) {
        /* Calls that ended having read this run to its end had not: it goes on. */
        unsigned kept = 0;
        for (unsigned i = 0; i < reader->pending_count; ++i) {
            if (reader->pending[i].stream != reader->open[which].id) {
                reader->pending[kept++] = reader->pending[i];
                continue;
            }
            reader->pending[i].stats->read[READ_JOINED]++;
            if (reader->pending[i].sample) reader->pending[i].sample->read = READ_JOINED;
        }
        reader->pending_count = kept;
        if (reader->open[which].end == offset) reader->open[which].end++;
    } else {
        /* The reader has gone elsewhere: what was read before it did was read
         * whole. Not what the code an interrupt broke into was reading, nor
         * what an interrupt's routine read before: those go on by themselves. */
        unsigned kept = 0;
        for (unsigned i = 0; i < reader->pending_count; ++i) {
            if (reader->pending[i].level != follow->level) {
                reader->pending[kept++] = reader->pending[i];
                continue;
            }
            reader->pending[i].stats->read[READ_WHOLE]++;
            if (reader->pending[i].sample) reader->pending[i].sample->read = READ_WHOLE;
        }
        reader->pending_count = kept;
        which = 0;
        for (unsigned i = 0; i < FOLLOW_STREAMS; ++i) {
            if (!reader->open[i].id) {
                which = (int)i;
                break;
            }
            if (reader->open[i].used < reader->open[which].used) which = (int)i;
        }
        reader->open[which].id = ++reader->next_stream;
        reader->open[which].end = offset + 1u;
        reader->streams++;
        /* Which words of RAM hold the address it starts at, and for each call
         * in progress, which of them have not been stored to since it was
         * entered: those held the address when it was. */
        for (unsigned at = 0xC000u; at < 0xFFFEu; ++at) {
            if (at == 0xDFFFu) at = 0xFF80u;
            if (follow_ram_word(ctx, (uint16_t)at) != address) continue;
            for (unsigned depth = 0; depth < follow->depth; ++depth) {
                const FollowFrame* frame = &follow->frames[depth];
                FollowRead* read = &follow->frames[depth].read[r];
                if (read->searched || follow->changed[at - 0x8000u] > frame->seq ||
                    follow->changed[at + 1u - 0x8000u] > frame->seq) {
                    continue;
                }
                if (read->held_count < FOLLOW_HELD) {
                    read->held[read->held_count++] = (uint16_t)at;
                } else {
                    read->held_more = 1;
                }
            }
        }
        for (unsigned depth = 0; depth < follow->depth; ++depth) follow->frames[depth].read[r].searched = 1;
    }
    reader->open[which].used = ++reader->clock;
    if (!follow->depth) return;
    FollowRead* read = &follow->frames[follow->depth - 1u].read[r];
    read->distance = 0;
    FollowPiece* piece = follow_piece(read, reader->open[which].id);
    if (piece) {
        if (piece->end < reader->open[which].end) piece->end = reader->open[which].end;
    } else if (read->count < FOLLOW_PIECES) {
        piece = &read->pieces[read->count++];
        piece->stream = reader->open[which].id;
        piece->first = offset;
        piece->end = offset + 1u;
        piece->address = address;
        piece->from_start = starts;
    } else {
        read->more = 1;
    }
}

/* Whether a call of the function at `pc` is in progress with the stack as it is now. */
static bool follow_in_progress(const Follow* follow, uint16_t sp, uint16_t bank, uint16_t pc) {
    for (unsigned depth = follow->depth; depth && follow->frames[depth - 1u].sp == sp; --depth) {
        if (follow->frames[depth - 1u].entry == pc && follow->frames[depth - 1u].entry_bank == bank) return true;
    }
    return false;
}

static bool condition(const GBContext* ctx, uint8_t opcode);

/* The instruction at ctx->pc is about to run: keep the shadow of the call stack. */
static void follow_before(DataUse* use, GBContext* ctx, uint8_t opcode) {
    Follow* follow = use->follow;
    const uint16_t pc = ctx->pc;
    const uint16_t bank = follow_bank_of(ctx, pc);
    follow_settle(use, ctx->sp, pc);
    if (follow->awaiting_vector) {
        /* An interrupt was taken, and this is where it went. */
        if (follow->awaiting_vector == 2u && follow->depth) {
            follow->frames[follow->depth - 1u].entry = pc;
            follow->frames[follow->depth - 1u].entry_bank = bank;
        }
        follow->awaiting_vector = 0;
        follow_entered(use, bank, pc, ENTERED_INTERRUPT, true);
    } else {
        /* The first instruction of a function, come to other than by a call:
         * by a jump, or by running on into it. It is entered all the same,
         * and what is on top of the stack is where it will return to. */
        const uint32_t* at = follow->arrived == ENTERED_CALL ? NULL : follow_function_at(use, bank, pc);
        if (at && *at && !follow_in_progress(follow, ctx->sp, bank, pc)) {
            follow->functions[*at - 1u].by[follow->arrived]++;
            FollowFrame* frame = follow_push(use, ctx, follow->arrived, pc, ctx->sp, follow_ram_word(ctx, ctx->sp));
            if (frame) {
                frame->from = follow->last_pc;
                frame->from_bank = follow->last_bank;
            }
        }
    }
    follow->arrived = ENTERED_FALL;
    follow->last_pc = pc;
    follow->last_bank = bank;
    switch (opcode) {
        case 0xC4: case 0xCC: case 0xD4: case 0xDC:
            if (!condition(ctx, opcode)) break;
            /* fall through */
        case 0xCD: {
            const uint16_t target = (uint16_t)(gb_read8(ctx, (uint16_t)(pc + 1u)) |
                                               (gb_read8(ctx, (uint16_t)(pc + 2u)) << 8));
            follow_entered(use, follow_bank_of(ctx, target), target, ENTERED_CALL, true);
            follow_push(use, ctx, ENTERED_CALL, target, (uint16_t)(ctx->sp - 2u), (uint16_t)(pc + 3u));
            follow->arrived = ENTERED_CALL;
            break;
        }
        case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF: {
            const uint16_t target = (uint16_t)(opcode & 0x38u);
            follow_entered(use, follow_bank_of(ctx, target), target, ENTERED_CALL, true);
            follow_push(use, ctx, ENTERED_CALL, target, (uint16_t)(ctx->sp - 2u), (uint16_t)(pc + 1u));
            follow->arrived = ENTERED_CALL;
            break;
        }
        case 0xC2: case 0xCA: case 0xD2: case 0xDA:
            if (!condition(ctx, opcode)) break;
            /* fall through */
        case 0xC3: case 0xE9:
            follow->arrived = ENTERED_JUMP;
            break;
        case 0x20: case 0x28: case 0x30: case 0x38:
            if (!condition(ctx, opcode)) break;
            /* fall through */
        case 0x18:
            follow->arrived = ENTERED_RELATIVE;
            break;
        default:
            break;
    }
}

static void mark(DataUse* use, uint32_t tag, uint8_t what) {
    if (!tag) return;
    const size_t offset = (size_t)(tag & ~TAG_COMPUTED) - 1u;
    if (offset >= use->rom_size) return;
    if (tag & TAG_COMPUTED) {
        use->computed[offset] |= what;
    } else {
        use->direct[offset] |= what;
    }
}

static uint32_t worked_out(uint32_t tag) {
    return tag ? (tag | TAG_COMPUTED) : 0u;
}

/* What two values make together: it carries the second, the one just
 * brought to the first, if that came from the ROM. */
static uint32_t together(uint32_t first, uint32_t second) {
    return worked_out(second ? second : first);
}

/* The ROM byte at `offset` is being read at `address`, whose halves came
 * from `high` and `low`. How the game came by that address is what tells
 * the start of one picture from the rest of another: it was told it, if the
 * two halves are bytes of the ROM that spell exactly this address, as a word
 * of a table does; it worked it out, if a half was made from something in
 * the ROM, or the word has been stepped on from. Otherwise nothing is known:
 * a constant of the code, or a number that was in RAM before the record. */
static void tile_read(DataUse* use, const GBContext* ctx, size_t offset, uint16_t address, uint32_t high,
                      uint32_t low) {
    uint16_t how = (uint16_t)(use->tiles->how[offset] & ~(TILE_READ_TOLD | TILE_READ_WORKED));
    if ((high | low) & TAG_COMPUTED) {
        how |= TILE_READ_WORKED;
    } else if (high && low && (size_t)high - 1u < use->rom_size && (size_t)low - 1u < use->rom_size) {
        const uint16_t word = (uint16_t)(ctx->rom[low - 1u] | (ctx->rom[high - 1u] << 8));
        how |= word == address ? TILE_READ_TOLD : TILE_READ_WORKED;
    }
    use->tiles->how[offset] = how;
}

/* The tag of the byte at `address`, read through an address whose halves
 * came from `high` and `low`. */
static uint32_t load(DataUse* use, GBContext* ctx, uint16_t address, uint32_t high, uint32_t low) {
    mark(use, high, USE_POINTER);
    mark(use, low, USE_POINTER);
    if (address >= 0x8000u) return use->mem[address - 0x8000u];
    const size_t offset = (size_t)gb_resolve_rom_bank(ctx, address) * 0x4000u + (address & 0x3FFFu);
    if (offset >= use->rom_size) return 0u;
    const uint32_t reader = use->read_as ? use->read_as
                                         : (((uint32_t)gb_resolve_rom_bank(ctx, ctx->pc) << 16) | ctx->pc) + 1u;
    if (use->reads[offset] == 0) {
        use->reads[offset] = 1;
        use->reader[offset] = reader;
    } else if (use->reader[offset] != reader) {
        use->reads[offset] = 2;
    }
    if (use->follow) {
        for (unsigned r = 0; r < use->follow->reader_count; ++r) {
            if (use->follow->readers[r].where == reader) follow_read(use, ctx, r, (uint32_t)offset, address);
        }
    }
    if (use->tiles) tile_read(use, ctx, offset, address, high, low);
    return (uint32_t)offset + 1u;
}

/* A ROM byte has reached the tile area as it is, at `address`. Where it
 * landed is weighed against where the byte before it in the ROM last did:
 * right after it, the two are neighbours in a tile of 2 bits a pixel; two
 * after it, in one of 1 bit a pixel. Otherwise a copy begins here, and how
 * the game came by the address it read the byte at is kept with it. */
static void tile_put(DataUse* use, uint32_t tag, uint16_t address) {
    TileUse* tiles = use->tiles;
    if (!tag || (tag & TAG_COMPUTED)) return;
    const size_t offset = (size_t)tag - 1u;
    if (offset >= use->rom_size) return;
    const uint16_t before = tiles->at[offset];
    /* The same byte again at the next address: both halves of a row made from one byte. */
    if (before && address == (uint16_t)(before + 1u)) return;
    uint16_t how = tiles->how[offset];
    if (before && ((before ^ address) & 15u)) how |= TILE_MIXED;
    const uint16_t last = (offset & 0x3FFFu) ? tiles->at[offset - 1u] : 0u;   /* no further back than its bank */
    if (last && address == (uint16_t)(last + 1u)) {
        how |= TILE_NEXT;
    } else if (last && address == (uint16_t)(last + 2u)) {
        how |= TILE_SKIP;
    } else {
        how |= TILE_BEGUN;
        if (how & TILE_READ_TOLD) how |= TILE_BEGUN_TOLD;
        if (how & TILE_READ_WORKED) how |= TILE_BEGUN_WORKED;
    }
    tiles->how[offset] = (uint16_t)(how | TILE_PUT);
    tiles->at[offset] = address;
}

static void put(DataUse* use, uint16_t address, uint32_t tag) {
    uint8_t what = 0;
    if (address < 0x8000u) {
        what = USE_BANK;
    } else if (address < 0x9800u) {
        what = USE_TILES;
    } else if (address < 0xA000u) {
        what = USE_TILE_MAP;
    } else if (address >= 0xFE00u && address < 0xFEA0u) {
        what = USE_OAM;
    } else if (address >= 0xFF10u && address < 0xFF40u) {
        what = USE_SOUND;
    } else if ((address >= 0xFF47u && address <= 0xFF49u) || address == 0xFF69u || address == 0xFF6Bu) {
        what = USE_PALETTE;
    }
    if (what) mark(use, tag, what);
    if (what == USE_TILES && use->tiles) tile_put(use, tag, address);
    if (address >= 0x8000u) use->mem[address - 0x8000u] = tag;
}

/* The same for video memory, by its bank as well: `mem` goes by address
 * alone, and the screen draws from both banks at once. */
static void shown_put(DataUse* use, const GBContext* ctx, uint16_t address, uint32_t tag) {
    if (address < 0x8000u || address >= 0xA000u) return;
    use->shown->tags[(size_t)(ctx->vram_bank & 1u) * VRAM_SIZE + (address - 0x8000u)] = tag;
}

void gbrt_data_use_copied(GBContext* ctx, uint16_t source, uint16_t target, unsigned count) {
    DataUse* use = (DataUse*)ctx->data_use;
    if (!use) return;
    if (use->pointers) pointers_copied(use, ctx, source, target, count);
    for (unsigned i = 0; i < count; ++i) {
        const uint16_t from = (uint16_t)(source + i);
        uint32_t tag;
        if (from >= 0x8000u) {
            tag = use->mem[from - 0x8000u];
        } else {
            const size_t offset = (size_t)gb_resolve_rom_bank(ctx, from) * 0x4000u + (from & 0x3FFFu);
            tag = offset < use->rom_size ? (uint32_t)offset + 1u : 0u;
            if (tag && use->reads[offset] == 0) {
                use->reads[offset] = 1;
                use->reader[offset] = (((uint32_t)gb_resolve_rom_bank(ctx, ctx->pc) << 16) | ctx->pc) + 1u;
            }
            if (tag && use->tiles) tile_read(use, ctx, offset, from, 0u, 0u);
        }
        put(use, (uint16_t)(target + i), tag);
        shown_put(use, ctx, (uint16_t)(target + i), tag);
    }
}

/* A store an instruction makes, of a value tagged `tag`. */
static void put_stored(DataUse* use, const GBContext* ctx, uint16_t address, uint32_t tag) {
    if (use->follow) follow_stored(use, ctx, address);
    put(use, address, tag);
    shown_put(use, ctx, address, tag);
}

/* A store of a value tagged `tag`, through an address from `high` and `low`. */
static void store(DataUse* use, const GBContext* ctx, uint16_t address, uint32_t tag, uint32_t high, uint32_t low) {
    mark(use, high, USE_POINTER);
    mark(use, low, USE_POINTER);
    put_stored(use, ctx, address, tag);
}

void gbrt_data_use_interrupt(GBContext* ctx) {
    DataUse* use = (DataUse*)ctx->data_use;
    if (!use) return;
    for (unsigned i = 0; i < 2u; ++i) {
        const uint16_t address = (uint16_t)(ctx->sp + i);
        if (address >= 0x8000u) use->mem[address - 0x8000u] = 0u;
    }
    if (use->pointers) pointers_interrupt(use, ctx);
    if (use->follow) {
        /* The game is still where it will come back to, and the stack pointer
         * two below where it was. Where the interrupt goes shows when the
         * next instruction runs. */
        Follow* follow = use->follow;
        follow_settle(use, (uint16_t)(ctx->sp + 2u), ctx->pc);
        follow_pushed(follow, ctx->sp);
        follow_pushed(follow, (uint16_t)(ctx->sp + 1u));
        follow->awaiting_vector = follow_push(use, ctx, ENTERED_INTERRUPT, 0u, ctx->sp, ctx->pc) ? 2u : 1u;
    }
}

static void push(DataUse* use, GBContext* ctx, uint32_t high, uint32_t low) {
    const uint16_t top = (uint16_t)(ctx->sp - 1u);
    const uint16_t below = (uint16_t)(ctx->sp - 2u);
    if (top >= 0x8000u) use->mem[top - 0x8000u] = high;
    if (below >= 0x8000u) use->mem[below - 0x8000u] = low;
    if (use->follow) {
        follow_pushed(use->follow, top);
        follow_pushed(use->follow, below);
    }
}

/* What a POP or a RET takes off the stack. With the stack pointer in the ROM
 * it is a read like any other: a game that copies with POP, the fastest read
 * there is, reads its pictures this way. Such a copy is often a row of POPs,
 * each reading two bytes of every so many, so the bytes are put down to the
 * one instruction that pointed the stack pointer there. */
static uint32_t stacked(DataUse* use, GBContext* ctx, uint16_t address) {
    if (address >= 0x8000u) return use->mem[address - 0x8000u];
    use->read_as = use->sp_set;
    const uint32_t tag = load(use, ctx, address, use->sp[0], use->sp[1]);
    use->read_as = 0u;
    return tag;
}

static bool condition(const GBContext* ctx, uint8_t opcode) {
    switch ((opcode >> 3) & 3u) {
        case 0: return !ctx->f_z;
        case 1: return ctx->f_z != 0;
        case 2: return !ctx->f_c;
        default: return ctx->f_c != 0;
    }
}

void gbrt_data_use_before(GBContext* ctx) {
    DataUse* use = (DataUse*)ctx->data_use;
    if (!use) return;
    use->instructions++;
    uint32_t* reg = use->reg;
    const uint16_t pc = ctx->pc;
    const uint8_t opcode = gb_read8(ctx, pc);
    const uint8_t low3 = opcode & 7u;
    const uint8_t mid3 = (opcode >> 3) & 7u;
    if (use->follow) follow_before(use, ctx, opcode);

    /* Where the stack pointer is pointed: LD SP,nn, LD SP,HL and ADD SP,e */
    if (opcode == 0x31u || opcode == 0xF9u || opcode == 0xE8u) {
        if (opcode == 0x31u) {
            use->sp[0] = use->sp[1] = 0u;
        } else if (opcode == 0xF9u) {
            use->sp[0] = reg[4];
            use->sp[1] = reg[5];
        } else {
            use->sp[0] = worked_out(use->sp[0]);
            use->sp[1] = worked_out(use->sp[1]);
        }
        use->sp_set = (((uint32_t)gb_resolve_rom_bank(ctx, pc) << 16) | pc) + 1u;
    }

    /* LD r,r' and LD r,(HL), LD (HL),r */
    if (opcode >= 0x40u && opcode < 0x80u) {
        if (opcode == 0x76u) return;
        const uint32_t tag = low3 == 6u ? load(use, ctx, ctx->hl, reg[4], reg[5]) : reg[low3];
        if (mid3 == 6u) {
            store(use, ctx, ctx->hl, tag, reg[4], reg[5]);
        } else {
            reg[mid3] = tag;
        }
        return;
    }
    /* ALU A,r */
    if (opcode >= 0x80u && opcode < 0xC0u) {
        const uint32_t tag = low3 == 6u ? load(use, ctx, ctx->hl, reg[4], reg[5]) : reg[low3];
        if (mid3 == 7u) return;                                   /* CP: nothing is kept */
        if (low3 == 7u && (mid3 == 2u || mid3 == 5u)) {           /* SUB A, XOR A: zero */
            reg[7] = 0u;
        } else if (low3 == 7u && (mid3 == 4u || mid3 == 6u)) {    /* AND A, OR A: as it was */
        } else {
            reg[7] = together(reg[7], tag);
        }
        return;
    }
    switch (opcode) {
        /* LD r,n */
        case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E:
            reg[mid3] = 0u;
            return;
        case 0x36:
            store(use, ctx, ctx->hl, 0u, reg[4], reg[5]);
            return;
        /* INC r, DEC r */
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x3C:
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x3D:
            reg[mid3] = worked_out(reg[mid3]);
            return;
        case 0x34: case 0x35: {
            const uint32_t tag = load(use, ctx, ctx->hl, reg[4], reg[5]);
            put_stored(use, ctx, ctx->hl, worked_out(tag));
            return;
        }
        /* A through memory */
        case 0x0A: reg[7] = load(use, ctx, ctx->bc, reg[0], reg[1]); return;
        case 0x1A: reg[7] = load(use, ctx, ctx->de, reg[2], reg[3]); return;
        case 0x2A: case 0x3A: reg[7] = load(use, ctx, ctx->hl, reg[4], reg[5]); return;
        case 0x02: store(use, ctx, ctx->bc, reg[7], reg[0], reg[1]); return;
        case 0x12: store(use, ctx, ctx->de, reg[7], reg[2], reg[3]); return;
        case 0x22: case 0x32: store(use, ctx, ctx->hl, reg[7], reg[4], reg[5]); return;
        case 0xFA: case 0xEA: {
            const uint16_t address = (uint16_t)(gb_read8(ctx, (uint16_t)(pc + 1u)) |
                                                (gb_read8(ctx, (uint16_t)(pc + 2u)) << 8));
            if (opcode == 0xFA) {
                reg[7] = load(use, ctx, address, 0u, 0u);
            } else {
                store(use, ctx, address, reg[7], 0u, 0u);
            }
            return;
        }
        case 0xF0: reg[7] = load(use, ctx, (uint16_t)(0xFF00u | gb_read8(ctx, (uint16_t)(pc + 1u))), 0u, 0u); return;
        case 0xE0: store(use, ctx, (uint16_t)(0xFF00u | gb_read8(ctx, (uint16_t)(pc + 1u))), reg[7], 0u, 0u); return;
        case 0xF2: reg[7] = load(use, ctx, (uint16_t)(0xFF00u | ctx->c), 0u, reg[1]); return;
        case 0xE2: store(use, ctx, (uint16_t)(0xFF00u | ctx->c), reg[7], 0u, reg[1]); return;
        /* A by itself, or with a constant */
        case 0x07: case 0x0F: case 0x17: case 0x1F: case 0x27: case 0x2F:
        case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6:
            reg[7] = worked_out(reg[7]);
            return;
        /* Pairs. A pair stepped along is still the address it was. */
        case 0x01: reg[0] = reg[1] = 0u; return;
        case 0x11: reg[2] = reg[3] = 0u; return;
        case 0x21: reg[4] = reg[5] = 0u; return;
        case 0x09: reg[4] = together(reg[4], reg[0]); reg[5] = together(reg[5], reg[1]); return;
        case 0x19: reg[4] = together(reg[4], reg[2]); reg[5] = together(reg[5], reg[3]); return;
        case 0x29: case 0x39: reg[4] = worked_out(reg[4]); reg[5] = worked_out(reg[5]); return;
        case 0xF8: reg[4] = reg[5] = 0u; return;
        case 0x08: {
            const uint16_t address = (uint16_t)(gb_read8(ctx, (uint16_t)(pc + 1u)) |
                                                (gb_read8(ctx, (uint16_t)(pc + 2u)) << 8));
            put_stored(use, ctx, address, 0u);
            put_stored(use, ctx, (uint16_t)(address + 1u), 0u);
            return;
        }
        /* The stack */
        case 0xC5: push(use, ctx, reg[0], reg[1]); return;
        case 0xD5: push(use, ctx, reg[2], reg[3]); return;
        case 0xE5: push(use, ctx, reg[4], reg[5]); return;
        case 0xF5: push(use, ctx, reg[7], 0u); return;
        case 0xC1: reg[1] = stacked(use, ctx, ctx->sp); reg[0] = stacked(use, ctx, (uint16_t)(ctx->sp + 1u)); return;
        case 0xD1: reg[3] = stacked(use, ctx, ctx->sp); reg[2] = stacked(use, ctx, (uint16_t)(ctx->sp + 1u)); return;
        case 0xE1: reg[5] = stacked(use, ctx, ctx->sp); reg[4] = stacked(use, ctx, (uint16_t)(ctx->sp + 1u)); return;
        case 0xF1: reg[7] = stacked(use, ctx, (uint16_t)(ctx->sp + 1u)); return;
        /* Leaving for an address that came out of the ROM */
        case 0xE9:
            mark(use, reg[4], USE_JUMP);
            mark(use, reg[5], USE_JUMP);
            return;
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:
            if (!condition(ctx, opcode)) return;
            /* fall through */
        case 0xC9: case 0xD9:
            mark(use, stacked(use, ctx, ctx->sp), USE_JUMP);
            mark(use, stacked(use, ctx, (uint16_t)(ctx->sp + 1u)), USE_JUMP);
            return;
        /* A call pushes where to come back to, which is no data. */
        case 0xC4: case 0xCC: case 0xD4: case 0xDC:
            if (!condition(ctx, opcode)) return;
            /* fall through */
        case 0xCD:
        case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
            push(use, ctx, 0u, 0u);
            return;
        case 0xCB: {
            const uint8_t second = gb_read8(ctx, (uint16_t)(pc + 1u));
            const uint8_t which = second & 7u;
            const bool only_tests = (second >> 6) == 1u;             /* BIT */
            if (which == 6u) {
                const uint32_t tag = load(use, ctx, ctx->hl, reg[4], reg[5]);
                if (!only_tests) put_stored(use, ctx, ctx->hl, worked_out(tag));
            } else if (!only_tests) {
                reg[which] = worked_out(reg[which]);
            }
            return;
        }
        default:
            return;
    }
}

/* ---------------------------------------------------------------------------
 * What the screen showed: looked at once a frame, when the frame is whole.
 * ------------------------------------------------------------------------- */

static size_t shown_slot(const Shown* shown, uint32_t offset1, uint8_t what, uint64_t detail) {
    uint64_t hash = ((uint64_t)offset1 << 8 | what) * 0x9E3779B97F4A7C15ull;
    hash ^= (detail + (hash >> 29)) * 0xBF58476D1CE4E5B9ull;
    size_t at = (size_t)(hash >> 20) & (shown->room - 1u);
    while (shown->entries[at].offset1 &&
           (shown->entries[at].offset1 != offset1 || shown->entries[at].what != what ||
            shown->entries[at].detail != detail)) {
        at = (at + 1u) & (shown->room - 1u);
    }
    return at;
}

/* The entry for one thing seen of the ROM byte at `offset`, made if it is not there. */
static ShownEntry* shown_entry(Shown* shown, uint32_t offset, uint8_t what, uint64_t detail) {
    if (shown->count * 2u >= shown->room) {
        const size_t room = shown->room;
        ShownEntry* old = shown->entries;
        ShownEntry* grown = (ShownEntry*)calloc(room * 2u, sizeof(ShownEntry));
        if (!grown) {
            shown->failed = 1;
            return NULL;
        }
        shown->entries = grown;
        shown->room = room * 2u;
        for (size_t i = 0; i < room; ++i) {
            if (old[i].offset1) grown[shown_slot(shown, old[i].offset1, old[i].what, old[i].detail)] = old[i];
        }
        free(old);
    }
    ShownEntry* entry = &shown->entries[shown_slot(shown, offset + 1u, what, detail)];
    if (!entry->offset1) {
        entry->offset1 = offset + 1u;
        entry->what = what;
        entry->detail = detail;
        shown->count++;
    }
    return entry;
}

/* The ROM byte at `offset` was seen so in the frame being looked at. */
static ShownEntry* shown_seen(Shown* shown, uint32_t offset, uint8_t what, uint64_t detail) {
    ShownEntry* entry = shown_entry(shown, offset, what, detail);
    if (entry && entry->stamp != shown->frames) {
        entry->stamp = shown->frames;
        entry->frames++;
    }
    return entry;
}

/* ...or once more, for what is counted by the cell. */
static void shown_count(Shown* shown, uint32_t offset, uint8_t what, uint64_t detail) {
    ShownEntry* entry = shown_entry(shown, offset, what, detail);
    if (entry) entry->frames++;
}

/* The ROM byte that lies in video memory at `at` (a place in `tags`) as it
 * is, or false. A write the hardware dropped left a tag and no byte: what
 * is there has to be what the ROM has. */
static bool shown_rom_byte(const DataUse* use, const GBContext* ctx, size_t at, uint32_t* offset) {
    const uint32_t tag = use->shown->tags[at];
    if (!tag || (tag & TAG_COMPUTED) || tag > use->rom_size || ctx->rom[tag - 1u] != ctx->vram[at]) return false;
    *offset = tag - 1u;
    return true;
}

/* The ROM tile that lies in video memory at `at` as it is: where its first
 * byte is in the ROM, and 16 if its sixteen bytes follow one another there,
 * 8 if each of eight was written twice, as a tile of one bit a pixel is. 0
 * if it is neither. */
static unsigned shown_rom_tile(const DataUse* use, const GBContext* ctx, size_t at, uint32_t* first) {
    uint32_t offsets[TILE_SIZE];
    bool whole = true;
    bool doubled = true;
    for (unsigned byte = 0; byte < TILE_SIZE; ++byte) {
        if (!shown_rom_byte(use, ctx, at + byte, &offsets[byte])) return 0u;
        if (offsets[byte] != offsets[0] + byte) whole = false;
        if (offsets[byte] != offsets[0] + byte / 2u) doubled = false;
    }
    *first = offsets[0];
    return whole ? TILE_SIZE : doubled ? TILE_SIZE / 2u : 0u;
}

/* A pixel of the background or the window: note the cell of the tile map it
 * was drawn from. The cell is worked out from where the pixel is and from
 * the scroll and window registers as they are now, at the frame's end; a
 * game that changed them while the frame was drawn drew this pixel from
 * some other cell, so the cell is kept only if its tile number gives the
 * tile the PPU says it drew. */
static void shown_cell(Shown* shown, const GBContext* ctx, const GBPPU* ppu, unsigned pixel, uint32_t source) {
    const unsigned x = pixel % GB_SCREEN_WIDTH;
    const unsigned y = pixel / GB_SCREEN_WIDTH;
    unsigned column, row, map;
    if (GB_PIXEL_SOURCE_LAYER(source) == GB_PIXEL_LAYER_WINDOW) {
        if (x + 7u < ppu->wx || y < ppu->wy) return;
        column = ((x + 7u - ppu->wx) >> 3) & 31u;
        row = ((y - ppu->wy) >> 3) & 31u;
        map = (ppu->lcdc & LCDC_WINDOW_TILEMAP) ? 1u : 0u;
    } else {
        column = ((x + ppu->scx) >> 3) & 31u;
        row = ((y + ppu->scy) >> 3) & 31u;
        map = (ppu->lcdc & LCDC_BG_TILEMAP) ? 1u : 0u;
    }
    const unsigned cell = map * 1024u + row * 32u + column;
    if (shown->cells[cell]) return;
    const unsigned number = ctx->vram[0x1800u + cell];
    const unsigned tile = (ppu->lcdc & LCDC_TILE_DATA) || number >= 128u ? number : 256u + number;
    if (tile != GB_PIXEL_SOURCE_SLOT(source)) return;
    shown->cells[cell] = 1;
    shown->cell_tile[cell] = (uint16_t)((GB_PIXEL_SOURCE_BANK(source) << 9) | tile);
    shown->cell_list[shown->cell_count++] = (uint16_t)cell;
}

/* The cells that were on the screen: for the ROM byte in each, the tile its
 * number stood for, and how it lay to the bytes before it in the ROM. The
 * byte just before it in the cell to the left says the two are in one row;
 * the byte in the cell above says how many bytes a row is, which is how
 * wide a screen or a block of tiles is. Each is counted against the cells
 * where it could be seen at all: at the screen's edge a neighbour is out of
 * sight, and that says nothing. */
static void shown_cells(DataUse* use, const GBContext* ctx) {
    Shown* shown = use->shown;
    for (unsigned n = 0; n < shown->cell_count; ++n) {
        const unsigned cell = shown->cell_list[n];
        uint32_t offset, first;
        if (!shown_rom_byte(use, ctx, 0x1800u + cell, &offset)) continue;
        shown_count(shown, offset, SHOWN_CELL, 0u);
        const size_t tile = (size_t)(shown->cell_tile[cell] >> 9) * VRAM_SIZE +
                            (size_t)(shown->cell_tile[cell] & 0x1FFu) * TILE_SIZE;
        const unsigned bytes = shown_rom_tile(use, ctx, tile, &first);
        if (bytes) shown_count(shown, offset, SHOWN_STOOD, ((uint64_t)first << 8) | bytes);
        const unsigned map = cell & ~1023u;
        const unsigned row = (cell >> 5) & 31u;
        const unsigned column = cell & 31u;
        const unsigned left = map + row * 32u + ((column + 31u) & 31u);
        const unsigned above = map + ((row + 31u) & 31u) * 32u + column;
        uint32_t there;
        if (shown->cells[left]) {
            shown_count(shown, offset, SHOWN_LEFT, 0u);
            if (shown_rom_byte(use, ctx, 0x1800u + left, &there) && there + 1u == offset) {
                shown_count(shown, offset, SHOWN_AFTER, 0u);
            }
        }
        if (shown->cells[above]) {
            shown_count(shown, offset, SHOWN_ABOVE, 0u);
            if (shown_rom_byte(use, ctx, 0x1800u + above, &there) && there < offset && offset - there <= 32u) {
                shown_count(shown, offset, SHOWN_UNDER, offset - there);
            }
        }
    }
    for (unsigned n = 0; n < shown->cell_count; ++n) shown->cells[shown->cell_list[n]] = 0;
    shown->cell_count = 0;
}

void gbrt_data_use_frame(GBContext* ctx) {
    DataUse* use = ctx ? (DataUse*)ctx->data_use : NULL;
    Shown* shown = use ? use->shown : NULL;
    if (!shown || !ctx->ppu || !ctx->vram || !ctx->rom) return;
    if (!gbrt_pixel_sources) {
        /* Whatever had the PPU note what drew each pixel has stopped: from the next frame on, here. */
        gbrt_pixel_sources = shown->own_sources;
        return;
    }
    const GBPPU* ppu = (const GBPPU*)ctx->ppu;
    const uint32_t* sources = gbrt_pixel_sources;
    const bool colour = ctx->config.model == GB_MODEL_CGB && !ctx->config.cgb_compatibility_mode;
    shown->frames++;
    /* Each tile that drew a pixel, with the layer it was in and the palette it was drawn through, once. */
    unsigned count = 0;
    uint32_t last = SHOWN_KEYS;
    for (unsigned i = 0; i < GB_FRAMEBUFFER_SIZE; ++i) {
        const uint32_t source = sources[i];
        const unsigned layer = GB_PIXEL_SOURCE_LAYER(source);
        if (layer == GB_PIXEL_LAYER_NONE || GB_PIXEL_SOURCE_SLOT(source) >= TILES_PER_BANK) continue;
        if (layer != GB_PIXEL_LAYER_OBJ) shown_cell(shown, ctx, ppu, i, source);
        const uint32_t key = ((uint32_t)(layer == GB_PIXEL_LAYER_OBJ) << 18) | (GB_PIXEL_SOURCE_BANK(source) << 17) |
                             (GB_PIXEL_SOURCE_SLOT(source) << 8) | GB_PIXEL_SOURCE_PALETTE(source);
        if (key == last) continue;
        last = key;
        if (shown->drawn[key >> 3] & (1u << (key & 7u))) continue;
        shown->drawn[key >> 3] |= (uint8_t)(1u << (key & 7u));
        shown->list[count++] = key;
    }
    for (unsigned n = 0; n < count; ++n) {
        const uint32_t key = shown->list[n];
        shown->drawn[key >> 3] = 0;
        const bool objects = (key >> 18) != 0;
        const size_t tile = (size_t)((key >> 17) & 1u) * VRAM_SIZE + (size_t)((key >> 8) & 0x1FFu) * TILE_SIZE;
        uint8_t what = objects ? SHOWN_OBJECTS : SHOWN_BACKGROUND;
        uint64_t detail = key & 0xFFu;
        if (colour) {
            /* The palette's number says nothing by itself: its four colours as they are now. */
            const uint8_t* ram = (objects ? ppu->obj_palette_ram : ppu->bg_palette_ram) + (key & 7u) * 8u;
            detail = 0;
            for (unsigned c = 0; c < 4u; ++c) {
                detail |= (uint64_t)((ram[c * 2u] | (ram[c * 2u + 1u] << 8)) & 0x7FFFu) << (16u * c);
            }
            what |= SHOWN_COLOUR;
        }
        for (unsigned byte = 0; byte < TILE_SIZE; ++byte) {
            uint32_t offset;
            if (!shown_rom_byte(use, ctx, tile + byte, &offset)) continue;
            ShownEntry* entry = shown_seen(shown, offset, what, detail);
            if (entry) entry->planes |= (uint8_t)((byte & 1u) ? 2u : 1u);
        }
    }
    shown_cells(use, ctx);
}

static void write_uses(FILE* out, uint8_t bits) {
    bool first = true;
    fputc('[', out);
    for (unsigned i = 0; i < 8u; ++i) {
        if (!(bits & (1u << i))) continue;
        fprintf(out, "%s\"%s\"", first ? "" : ", ", kUseNames[i]);
        first = false;
    }
    fputc(']', out);
}

static void write_place(FILE* out, uint16_t bank, uint16_t address) {
    fprintf(out, "{\"bank\": %u, \"address\": %u}", (unsigned)bank, (unsigned)address);
}

/* The names of those of `count` values that are `wanted`, as a list. */
static void write_matching(FILE* out, const char* const* names, const unsigned* values, unsigned count,
                           unsigned wanted) {
    bool first = true;
    fputc('[', out);
    for (unsigned i = 0; i < count; ++i) {
        if (values[i] != wanted) continue;
        fprintf(out, "%s\"%s\"", first ? "" : ", ", names[i]);
        first = false;
    }
    fputc(']', out);
}

static void follow_write_sample(FILE* out, const FollowSample* sample) {
    static const char* const kPairs[3] = {"bc", "de", "hl"};
    static const char* const kRegisters[7] = {"a", "b", "c", "d", "e", "h", "l"};
    const FollowPiece* first = &sample->pieces[0];
    const unsigned pairs[3] = {sample->bc, sample->de, sample->hl};
    const unsigned registers[7] = {sample->a, sample->bc >> 8, sample->bc & 0xFFu, sample->de >> 8,
                                   sample->de & 0xFFu, sample->hl >> 8, sample->hl & 0xFFu};
    fprintf(out, "\n      {\"by\": \"%s\", \"from\": ", kEnteredNames[sample->kind]);
    write_place(out, sample->from_bank, sample->from);
    fprintf(out, ", \"registers\": {\"a\": %u, \"bc\": %u, \"de\": %u, \"hl\": %u, \"sp\": %u}, \"mapped_bank\": %u,",
            (unsigned)sample->a, (unsigned)sample->bc, (unsigned)sample->de, (unsigned)sample->hl,
            (unsigned)sample->sp, (unsigned)sample->mapped_bank);
    fprintf(out, "\n       \"read\": \"%s\", \"streams\": [", kReadNames[sample->read]);
    for (unsigned i = 0; i < sample->piece_count; ++i) {
        const FollowPiece* piece = &sample->pieces[i];
        fprintf(out, "%s{\"offset\": %lu, \"size\": %lu, \"bank\": %lu, \"address\": %u, \"from_start\": %s}",
                i ? ", " : "", (unsigned long)piece->first, (unsigned long)(piece->end - piece->first),
                (unsigned long)(piece->first / 0x4000u), (unsigned)piece->address,
                piece->from_start ? "true" : "false");
    }
    fprintf(out, "], \"streams_dropped\": %s,", sample->more ? "true" : "false");
    /* Where the first of them starts, and its bank: what held those as the function was entered. */
    fprintf(out, "\n       \"held\": {\"registers\": ");
    write_matching(out, kPairs, pairs, 3u, first->address);
    fprintf(out, ", \"ram16\": [");
    for (unsigned i = 0; i < sample->held_count; ++i) fprintf(out, "%s%u", i ? ", " : "", (unsigned)sample->held[i]);
    fprintf(out, "], \"ram16_dropped\": %s}, \"bank_in\": ", sample->held_more ? "true" : "false");
    write_matching(out, kRegisters, registers, 7u, (unsigned)(first->first / 0x4000u));
    fprintf(out, ",\n       \"wrote\": [");
    for (unsigned i = 0; i < sample->run_count; ++i) {
        const FollowRun* run = &sample->runs[i];
        fprintf(out, "%s{\"area\": \"%s\", \"bank\": %u, \"address\": %u, \"size\": %u}", i ? ", " : "",
                run->address < 0xA000u ? "vram" : run->address < 0xC000u ? "cart_ram" :
                run->address < 0xE000u ? "wram" : "hram",
                (unsigned)run->bank, (unsigned)run->address, (unsigned)run->size);
    }
    fprintf(out, "], \"wrote_dropped\": {\"runs\": %lu, \"bytes\": %lu}}", (unsigned long)sample->runs_dropped,
            (unsigned long)sample->bytes_dropped);
}

/* The record is over: the calls still in progress never ended, and what
 * waited to hear whether its stream went on never heard. */
static void follow_finish(DataUse* use) {
    Follow* follow = use->follow;
    if (follow->finished) return;
    follow->finished = 1;
    while (follow->depth) follow_end(use, READ_UNFINISHED);
    for (unsigned r = 0; r < follow->reader_count; ++r) {
        FollowReader* reader = &follow->readers[r];
        for (unsigned i = 0; i < reader->pending_count; ++i) reader->pending[i].stats->read[READ_UNDECIDED]++;
        reader->pending_count = 0;
    }
}

static void follow_write(FILE* out, DataUse* use) {
    Follow* follow = use->follow;
    follow_finish(use);
    fprintf(out, ",\n \"followed\": {\n  \"too_deep\": %llu,\n  \"readers\": [",
            (unsigned long long)follow->too_deep);
    for (unsigned r = 0; r < follow->reader_count; ++r) {
        const FollowReader* reader = &follow->readers[r];
        const uint32_t where = reader->where - 1u;
        fprintf(out, "%s\n   {\"bank\": %u, \"address\": %u, \"bytes\": %llu, \"streams\": %lu, "
                     "\"functions_dropped\": %lu, \"functions\": [",
                r ? "," : "", (unsigned)(where >> 16), (unsigned)(where & 0xFFFFu), (unsigned long long)reader->bytes,
                (unsigned long)reader->streams, (unsigned long)reader->functions_dropped);
        for (unsigned f = 0; f < reader->function_count; ++f) {
            const FollowStats* stats = reader->functions[f];
            const uint32_t* at = follow_function_at(use, stats->bank, stats->address);
            const FollowFunction* function = at && *at ? &follow->functions[*at - 1u] : NULL;
            unsigned long entered = 0, read = 0;
            for (unsigned kind = 0; kind < ENTERED_KINDS && function; ++kind) entered += function->by[kind];
            for (unsigned kind = 0; kind < READ_KINDS; ++kind) read += kind == READ_UNFINISHED ? 0u : stats->read[kind];
            fprintf(out, "%s\n    {\"bank\": %u, \"address\": %u, \"entered\": %lu, \"by\": {", f ? "," : "",
                    (unsigned)stats->bank, (unsigned)stats->address, entered);
            for (unsigned kind = 0; kind < ENTERED_KINDS; ++kind) {
                fprintf(out, "%s\"%s\": %lu", kind ? ", " : "", kEnteredNames[kind],
                        (unsigned long)(function ? function->by[kind] : 0u));
            }
            fprintf(out, "},\n     \"read\": %lu", read);
            for (unsigned kind = 0; kind < READ_KINDS; ++kind) {
                fprintf(out, ", \"%s\": %lu", kReadNames[kind], (unsigned long)stats->read[kind]);
            }
            fprintf(out, ", \"distance\": %u,\n     \"inside\": [", (unsigned)stats->distance);
            for (unsigned i = 0; i < stats->inside_count; ++i) {
                fprintf(out, "%s{\"bank\": %u, \"address\": %u, \"times\": %lu}", i ? ", " : "",
                        (unsigned)stats->inside[i].bank, (unsigned)stats->inside[i].address,
                        (unsigned long)stats->inside[i].times);
            }
            fprintf(out, "], \"inside_dropped\": %lu, \"samples_dropped\": %lu, \"samples\": [",
                    (unsigned long)stats->inside_dropped, (unsigned long)stats->samples_dropped);
            for (unsigned i = 0; i < stats->sample_count; ++i) {
                if (i) fputc(',', out);
                follow_write_sample(out, &stats->samples[i]);
            }
            fprintf(out, "%s]}", stats->sample_count ? "\n     " : "");
        }
        fprintf(out, "%s]}", reader->function_count ? "\n   " : "");
    }
    fprintf(out, "%s]\n }", follow->reader_count ? "\n  " : "");
}

static bool shown_of_map(const ShownEntry* entry) {
    return (entry->what & 0x7Fu) >= SHOWN_CELL;
}

/* What was seen of tiles before what was seen of tile maps; then by the ROM
 * byte, and for one byte the most seen first, the kinds of a tile map's each
 * together. */
static int shown_order(const void* first, const void* second) {
    const ShownEntry* a = (const ShownEntry*)first;
    const ShownEntry* b = (const ShownEntry*)second;
    if (shown_of_map(a) != shown_of_map(b)) return shown_of_map(b) ? -1 : 1;
    if (a->offset1 != b->offset1) return a->offset1 < b->offset1 ? -1 : 1;
    if (shown_of_map(a) && a->what != b->what) return a->what < b->what ? -1 : 1;
    if (a->frames != b->frames) return a->frames > b->frames ? -1 : 1;
    if (a->what != b->what) return a->what < b->what ? -1 : 1;
    if (a->detail != b->detail) return a->detail < b->detail ? -1 : 1;
    return 0;
}

/* How many entries from `at` on are of the same ROM byte, and into `planes`
 * which bits of a row of pixels the byte was seen as. */
static size_t shown_group(const ShownEntry* sorted, size_t count, size_t at, uint8_t* planes) {
    size_t end = at;
    *planes = 0;
    while (end < count && sorted[end].offset1 == sorted[at].offset1) *planes |= sorted[end++].planes;
    return end - at;
}

/* The tiles the screen drew, as runs of ROM bytes that were drawn alike:
 * by which layer, through which palette, in how many frames. */
static void shown_write_tiles(FILE* out, const ShownEntry* sorted, size_t count) {
    bool first = true;
    size_t at = 0;
    while (at < count) {
        uint8_t planes, next_planes;
        const size_t group = shown_group(sorted, count, at, &planes);
        /* A byte that was both the low and the high bits of its pixels is of a tile
         * of one bit a pixel, and is not run together with bytes that were not. */
        const bool both = planes == 3u;
        size_t end = at + group;
        size_t size = 1;
        while (end < count && sorted[end].offset1 == sorted[at].offset1 + size &&
               (sorted[end].offset1 - 1u) % 0x4000u != 0u &&
               shown_group(sorted, count, end, &next_planes) == group && (next_planes == 3u) == both) {
            bool alike = true;
            for (size_t i = 0; i < group && alike; ++i) {
                alike = sorted[end + i].what == sorted[at + i].what && sorted[end + i].detail == sorted[at + i].detail &&
                        sorted[end + i].frames == sorted[at + i].frames;
            }
            if (!alike) break;
            if (next_planes != planes) planes = 0;
            end += group;
            ++size;
        }
        fprintf(out, "%s\n   {\"offset\": %lu, \"size\": %lu, ", first ? "" : ",",
                (unsigned long)(sorted[at].offset1 - 1u), (unsigned long)size);
        /* Which bits every byte of the run was, where that says something: one byte of two bits a pixel is either. */
        if (both || (planes && size > 1u)) fprintf(out, "\"planes\": %u, ", (unsigned)planes);
        fprintf(out, "\"as\": [");
        for (size_t i = 0; i < group; ++i) {
            const ShownEntry* entry = &sorted[at + i];
            fprintf(out, "%s{\"by\": \"%s\", ", i ? ", " : "",
                    (entry->what & 0x7Fu) == SHOWN_OBJECTS ? "objects" : "background");
            if (entry->what & SHOWN_COLOUR) {
                fprintf(out, "\"colours\": [%u, %u, %u, %u]", (unsigned)(entry->detail & 0xFFFFu),
                        (unsigned)((entry->detail >> 16) & 0xFFFFu), (unsigned)((entry->detail >> 32) & 0xFFFFu),
                        (unsigned)((entry->detail >> 48) & 0xFFFFu));
            } else {
                fprintf(out, "\"palette\": %u", (unsigned)entry->detail);
            }
            fprintf(out, ", \"frames\": %lu}", (unsigned long)entry->frames);
        }
        fprintf(out, "]}");
        first = false;
        at = end;
    }
    fprintf(out, "%s]", first ? "" : "\n  ");
}

/* The bytes of tile maps the screen showed, a line for each: in how many
 * cells; in how many of those the cell to the left was on the screen, and
 * held the byte before it; in how many the cell above was, and how many
 * bytes before it that held, how often; and which ROM tiles its number drew. */
static void shown_write_maps(FILE* out, const ShownEntry* sorted, size_t count) {
    bool first = true;
    size_t at = 0;
    while (at < count) {
        size_t end = at;
        unsigned long counts[SHOWN_STOOD + 1u] = {0};
        while (end < count && sorted[end].offset1 == sorted[at].offset1) {
            if (sorted[end].what < SHOWN_UNDER) counts[sorted[end].what] = sorted[end].frames;
            ++end;
        }
        fprintf(out, "%s\n   {\"offset\": %lu, \"cells\": %lu, \"left\": %lu, \"after\": %lu, \"above\": %lu, "
                     "\"under\": [", first ? "" : ",", (unsigned long)(sorted[at].offset1 - 1u), counts[SHOWN_CELL],
                counts[SHOWN_LEFT], counts[SHOWN_AFTER], counts[SHOWN_ABOVE]);
        bool comma = false;
        for (size_t i = at; i < end; ++i) {
            if (sorted[i].what != SHOWN_UNDER) continue;
            fprintf(out, "%s[%u, %lu]", comma ? ", " : "", (unsigned)sorted[i].detail, (unsigned long)sorted[i].frames);
            comma = true;
        }
        fprintf(out, "], \"tiles\": [");
        comma = false;
        for (size_t i = at; i < end; ++i) {
            if (sorted[i].what != SHOWN_STOOD) continue;
            fprintf(out, "%s[%lu, %u, %lu]", comma ? ", " : "", (unsigned long)(sorted[i].detail >> 8),
                    (unsigned)(sorted[i].detail & 0xFFu), (unsigned long)sorted[i].frames);
            comma = true;
        }
        fprintf(out, "]}");
        first = false;
        at = end;
    }
    fprintf(out, "%s]", first ? "" : "\n  ");
}

static void shown_write(FILE* out, const DataUse* use) {
    const Shown* shown = use->shown;
    ShownEntry* sorted = (ShownEntry*)malloc((shown->count ? shown->count : 1u) * sizeof(ShownEntry));
    size_t count = 0;
    for (size_t i = 0; sorted && i < shown->room; ++i) {
        if (shown->entries[i].offset1) sorted[count++] = shown->entries[i];
    }
    if (sorted) qsort(sorted, count, sizeof(ShownEntry), shown_order);
    fprintf(out, ",\n \"shown\": {\n  \"frames\": %lu,\n  \"dropped\": %s,\n  \"tiles\": [", (unsigned long)shown->frames,
            shown->failed || !sorted ? "true" : "false");
    size_t tiles = 0;
    while (tiles < count && !shown_of_map(&sorted[tiles])) ++tiles;
    shown_write_tiles(out, sorted, tiles);
    fprintf(out, ",\n  \"maps\": [");
    shown_write_maps(out, sorted ? sorted + tiles : NULL, count - tiles);
    fprintf(out, "\n }");
    free(sorted);
}

/* How many addresses on from the byte before it the byte at `offset` landed,
 * 1 or 2, or 0 if it did not follow on from it. */
static unsigned tile_step(const TileUse* tiles, size_t offset) {
    const uint16_t how = tiles->how[offset];
    if (!(offset & 0x3FFFu) || (how & (TILE_PUT | TILE_MIXED)) != TILE_PUT ||
        (tiles->how[offset - 1u] & (TILE_PUT | TILE_MIXED)) != TILE_PUT) {
        return 0u;
    }
    if ((how & (TILE_NEXT | TILE_SKIP)) == TILE_NEXT) return 1u;
    return (how & (TILE_NEXT | TILE_SKIP)) == TILE_SKIP ? 2u : 0u;
}

/* The bytes that reached the tile area as they are, in runs: a byte a copy
 * began with and the bytes that followed on from it, each the same number of
 * addresses after the one before. `to` is where the first landed, and
 * `begun` how a copy that began with it came by its address: told, if any
 * was, since that is one picture's start whatever else went on from the
 * picture before; else worked out, if any did. A byte a copy was told to
 * begin with begins a run even where another copy ran on through it: a game
 * may copy more than one picture's worth. Bytes that landed at more than
 * one place in a tile are runs of their own. */
static void tiles_write(FILE* out, const DataUse* use) {
    const TileUse* tiles = use->tiles;
    bool first = true;
    size_t offset = 0;
    fprintf(out, ",\n \"tile_runs\": [");
    while (offset < use->rom_size) {
        const uint16_t how = tiles->how[offset];
        if (!(how & TILE_PUT)) {
            ++offset;
            continue;
        }
        size_t end = offset + 1u;
        const size_t bank_end = (offset / 0x4000u + 1u) * 0x4000u;
        const unsigned bank = (unsigned)(offset / 0x4000u);
        unsigned step = 0;
        if (how & TILE_MIXED) {
            while (end < use->rom_size && end < bank_end &&
                   (tiles->how[end] & (TILE_PUT | TILE_MIXED)) == (TILE_PUT | TILE_MIXED)) {
                ++end;
            }
        } else {
            for (; end < use->rom_size && end < bank_end; ++end) {
                const unsigned next = tile_step(tiles, end);
                if (!next || (step && next != step) || (tiles->how[end] & TILE_BEGUN_TOLD)) break;
                step = next;
            }
        }
        fprintf(out, "%s\n  {\"offset\": %lu, \"size\": %lu, \"bank\": %u, \"address\": %u, ", first ? "" : ",",
                (unsigned long)offset, (unsigned long)(end - offset), bank,
                (unsigned)(bank ? 0x4000u + (offset & 0x3FFFu) : offset));
        if (how & TILE_MIXED) {
            fprintf(out, "\"mixed\": true}");
        } else {
            fprintf(out, "\"step\": %u, \"to\": %u, \"begun\": \"%s\"}", step, (unsigned)tiles->at[offset],
                    (how & TILE_BEGUN_TOLD) ? "told" : (how & TILE_BEGUN_WORKED) ? "worked_out" : "unknown");
        }
        first = false;
        offset = end;
    }
    fprintf(out, "\n ]");
}

bool gbrt_data_use_write(GBContext* ctx) {
    DataUse* use = ctx ? (DataUse*)ctx->data_use : NULL;
    if (!g_path) return true;
    if (!use) return false;
    FILE* out = fopen(g_path, "w");
    if (!out) return false;
    const unsigned checksum = use->rom_size > 0x14Fu ? (unsigned)((ctx->rom[0x14E] << 8) | ctx->rom[0x14F]) : 0u;
    fprintf(out, "{\n \"schema\": \"gbrt.data-use\",\n \"version\": 1,\n");
    fprintf(out, " \"rom\": {\"size\": %lu, \"global_checksum\": %u},\n", (unsigned long)use->rom_size, checksum);
    fprintf(out, " \"instructions\": %llu,\n", (unsigned long long)use->instructions);
    fprintf(out, " \"ranges\": [");
    bool first = true;
    size_t offset = 0;
    while (offset < use->rom_size) {
        if (!use->reads[offset]) {
            ++offset;
            continue;
        }
        /* A run of bytes read by the same instruction and used alike, in one bank. */
        size_t end = offset + 1u;
        const size_t bank_end = (offset / 0x4000u + 1u) * 0x4000u;
        while (end < use->rom_size && end < bank_end && use->reads[end] == use->reads[offset] &&
               use->reader[end] == use->reader[offset] && use->direct[end] == use->direct[offset] &&
               use->computed[end] == use->computed[offset]) {
            ++end;
        }
        const unsigned bank = (unsigned)(offset / 0x4000u);
        const uint32_t reader = use->reader[offset] - 1u;
        fprintf(out, "%s\n  {\"offset\": %lu, \"size\": %lu, \"bank\": %u, \"address\": %u, \"as_it_is\": ",
                first ? "" : ",", (unsigned long)offset, (unsigned long)(end - offset), bank,
                (unsigned)(bank ? 0x4000u + (offset & 0x3FFFu) : offset));
        write_uses(out, use->direct[offset]);
        fprintf(out, ", \"worked_out\": ");
        write_uses(out, use->computed[offset]);
        fprintf(out, ", \"read_at\": [%u, %u], \"one_reader\": %s}", (unsigned)(reader >> 16),
                (unsigned)(reader & 0xFFFFu), use->reads[offset] == 1 ? "true" : "false");
        first = false;
        offset = end;
    }
    fprintf(out, "\n ]");
    if (use->follow) follow_write(out, use);
    shown_write(out, use);
    /* What an option adds comes after what every record has, each section whole: a record made with
     * one is the record made without it and that section. */
    if (use->pointers) pointers_write(out, use, ctx);
    if (use->tiles) tiles_write(out, use);
    fprintf(out, "\n}\n");
    const bool ok = ferror(out) == 0;
    return fclose(out) == 0 && ok;
}

/* ---------------------------------------------------------------------------
 * --data-use-pointers: which two bytes of the ROM made the address a byte of
 * the ROM was read through.
 *
 * An origin says which ROM byte a value is, give or take a plain number: the
 * byte's offset plus one. It is kept apart from the tags above because it
 * means something else. A tag follows a value to where it is used, through
 * any arithmetic. An origin lasts only while the value is still that byte
 * with a number added that no byte of the ROM had a part in, which is what
 * an address stepped along or indexed is. Two such values that make an
 * address are a pointer, and what is read through it is what it leads to.
 * ------------------------------------------------------------------------- */

/* The byte was an instruction's operand, and not read as data: `ld hl, nn`. */
#define ORIGIN_OPERAND 0x80000000u

/* How a byte of the ROM was reached. */
enum {
    REACHED_DATA = 0x01,    /* through an address made of two bytes read from the ROM as data */
    REACHED_OPERAND = 0x02, /* through one made of the two bytes of an instruction's operand */
    REACHED_OTHER = 0x04,   /* through one that is neither, or by a copy of the hardware's own */
    REACHED_ALL = 0x07,
    /* And, kept with those: the byte was worked with as a number. It was compared, or put
     * through arithmetic that an address stepped along or indexed is not put through, or what
     * it made was jumped to. A byte of a pointer that is also that cannot be turned to another
     * place. An operand is marked like a byte read as data: lifted source that names an
     * operand turns it as a pointer in data is turned (LIFTING.md, "Data the source names"),
     * and must not turn one that was a number, or the start of code the game runs on into. */
    WORKED_AS_NUMBER = 0x80,
};
static const char* const kReachedNames[3] = {"data", "operand", "other"};

/* Two bytes that made addresses, and what was read through those. */
typedef struct {
    uint32_t low, high;     /* their origins: of the low half of the address, and of the high */
    uint32_t bank;          /* the bank of what was read */
    uint32_t first, last;   /* the lowest and the highest ROM offset read */
    int32_t least, most;    /* the address read less the address the two bytes spell, at its least and most */
    uint64_t reads;
} PointerLink;

struct Pointers {
    uint32_t reg[8];        /* the origin of b c d e h l - a */
    uint32_t* mem;          /* and of every byte from 8000 up */
    uint8_t* reached;       /* per ROM byte: how it was reached, and WORKED_AS_NUMBER */
    PointerLink* links;
    uint32_t link_count, link_room;
    uint32_t* slots;        /* a hash table of them: a link's place in `links`, plus one */
    uint32_t slot_count;
    uint32_t recent;        /* the link read through last, plus one */
};

void gbrt_data_use_pointers_set(bool wanted) {
    g_pointers = wanted;
}

bool gbrt_data_use_pointers_requested(void) {
    return g_pointers;
}

static bool pointers_create(DataUse* use) {
    struct Pointers* p = (struct Pointers*)calloc(1, sizeof(struct Pointers));
    if (!p) return false;
    use->pointers = p;
    p->mem = (uint32_t*)calloc(0x8000u, sizeof(uint32_t));
    p->reached = (uint8_t*)calloc(use->rom_size, 1);
    return p->mem && p->reached;
}

static void pointers_destroy(DataUse* use) {
    struct Pointers* p = use->pointers;
    if (!p) return;
    free(p->mem);
    free(p->reached);
    free(p->links);
    free(p->slots);
    free(p);
    use->pointers = NULL;
}

/* Where the origin of the byte at an address from 8000 up is kept. E000-FDFF is C000-DDFF again. */
static uint32_t* pointers_at(struct Pointers* p, uint16_t address) {
    if (address >= 0xE000u && address < 0xFE00u) address = (uint16_t)(address - 0x2000u);
    return &p->mem[address - 0x8000u];
}

static void pointers_put(struct Pointers* p, uint16_t address, uint32_t origin) {
    if (address < 0x8000u) {
        /* A write to the cartridge, other than the choice of a ROM bank, may map another bank
         * of its RAM or take the RAM away: what was at those addresses is not what is there. */
        if (address < 0x2000u || address >= 0x4000u) memset(&p->mem[0x2000u], 0, 0x2000u * sizeof(uint32_t));
        return;
    }
    /* And so may these two, of video memory and of work RAM, on a Game Boy Color. */
    if (address == 0xFF4Fu) memset(&p->mem[0], 0, 0x2000u * sizeof(uint32_t));
    if (address == 0xFF70u) memset(&p->mem[0x5000u], 0, 0x1000u * sizeof(uint32_t));
    *pointers_at(p, address) = origin;
}

static uint32_t pointers_hash(const PointerLink* link) {
    uint32_t hash = link->low * 2654435761u;
    hash ^= link->high + 0x9E3779B9u + (hash << 6) + (hash >> 2);
    hash ^= link->bank + 0x9E3779B9u + (hash << 6) + (hash >> 2);
    return hash;
}

static bool pointers_same(const PointerLink* link, const PointerLink* other) {
    return link->low == other->low && link->high == other->high && link->bank == other->bank;
}

/* The table again, with room for `count` places: a power of two. */
static bool pointers_rehash(struct Pointers* p, uint32_t count) {
    uint32_t* slots = (uint32_t*)calloc(count, sizeof(uint32_t));
    if (!slots) return false;
    for (uint32_t i = 0; i < p->link_count; ++i) {
        uint32_t at = pointers_hash(&p->links[i]) & (count - 1u);
        while (slots[at]) at = (at + 1u) & (count - 1u);
        slots[at] = i + 1u;
    }
    free(p->slots);
    p->slots = slots;
    p->slot_count = count;
    p->recent = 0;
    return true;
}

/* The link of those two bytes with that bank, made if it is new. NULL if there is no room for it. */
static PointerLink* pointers_link(struct Pointers* p, uint32_t low, uint32_t high, uint32_t bank) {
    PointerLink wanted;
    wanted.low = low;
    wanted.high = high;
    wanted.bank = bank;
    if (p->recent && pointers_same(&p->links[p->recent - 1u], &wanted)) return &p->links[p->recent - 1u];
    if ((p->link_count + 1u) * 2u > p->slot_count &&
        !pointers_rehash(p, p->slot_count ? p->slot_count * 2u : 1024u)) {
        return NULL;
    }
    uint32_t at = pointers_hash(&wanted) & (p->slot_count - 1u);
    while (p->slots[at]) {
        if (pointers_same(&p->links[p->slots[at] - 1u], &wanted)) {
            p->recent = p->slots[at];
            return &p->links[p->recent - 1u];
        }
        at = (at + 1u) & (p->slot_count - 1u);
    }
    if (p->link_count == p->link_room) {
        const uint32_t room = p->link_room ? p->link_room * 2u : 256u;
        PointerLink* grown = (PointerLink*)realloc(p->links, (size_t)room * sizeof(PointerLink));
        if (!grown) return NULL;
        p->links = grown;
        p->link_room = room;
    }
    PointerLink* link = &p->links[p->link_count];
    memset(link, 0, sizeof(*link));
    link->low = low;
    link->high = high;
    link->bank = bank;
    p->slots[at] = p->recent = ++p->link_count;
    return link;
}

/* The origin of the byte at `address`, read through an address whose halves
 * have the origins `high` and `low`. A byte of the ROM is its own origin. */
static uint32_t pointers_read(DataUse* use, GBContext* ctx, uint16_t address, uint32_t high, uint32_t low) {
    struct Pointers* p = use->pointers;
    if (address >= 0x8000u) return *pointers_at(p, address);
    const size_t offset = (size_t)gb_resolve_rom_bank(ctx, address) * 0x4000u + (address & 0x3FFFu);
    if (offset >= use->rom_size) return 0u;
    uint8_t how = REACHED_OTHER;
    /* A pointer is two bytes read as data, or the two bytes of one operand, low then high. */
    const bool operand = (low & ORIGIN_OPERAND) != 0u;
    if (low && high && low != high && ((low ^ high) & ORIGIN_OPERAND) == 0u && (!operand || high == low + 1u)) {
        const size_t low_at = (size_t)(low & ~ORIGIN_OPERAND) - 1u;
        const size_t high_at = (size_t)(high & ~ORIGIN_OPERAND) - 1u;
        PointerLink* link = low_at < use->rom_size && high_at < use->rom_size ?
                            pointers_link(p, low, high, (uint32_t)(offset / 0x4000u)) : NULL;
        if (link) {
            const uint16_t spelt = (uint16_t)(ctx->rom[low_at] | (ctx->rom[high_at] << 8));
            const int32_t past = (int16_t)(uint16_t)(address - spelt);
            if (!link->reads || offset < link->first) link->first = (uint32_t)offset;
            if (!link->reads || offset > link->last) link->last = (uint32_t)offset;
            if (!link->reads || past < link->least) link->least = past;
            if (!link->reads || past > link->most) link->most = past;
            link->reads++;
            how = operand ? REACHED_OPERAND : REACHED_DATA;
        }
    }
    p->reached[offset] |= how;
    return (uint32_t)offset + 1u;
}

/* The origin of an operand byte. Code that was copied into RAM has, in each
 * of its bytes, the ROM byte it was copied from. */
static uint32_t pointers_operand(DataUse* use, const GBContext* ctx, uint16_t address) {
    uint32_t origin;
    if (address >= 0x8000u) {
        origin = *pointers_at(use->pointers, address);
    } else {
        const size_t offset = (size_t)gb_resolve_rom_bank(ctx, address) * 0x4000u + (address & 0x3FFFu);
        origin = offset < use->rom_size ? (uint32_t)offset + 1u : 0u;
    }
    return origin ? origin | ORIGIN_OPERAND : 0u;
}

/* A value that was a byte of the ROM, read as data or an operand, is no
 * longer that byte moved by a plain number, or was compared, or was jumped
 * to: the byte was a number to the game. */
static void pointers_number(DataUse* use, uint32_t origin) {
    if (!origin) return;
    const size_t offset = (size_t)(origin & ~ORIGIN_OPERAND) - 1u;
    if (offset < use->rom_size) use->pointers->reached[offset] |= WORKED_AS_NUMBER;
}

/* What an operand made was jumped to. A byte read as data that is jumped
 * through is known by its tags (the use `jump`), as it always was; an
 * operand has none, so it is marked here. */
static void pointers_jumped(DataUse* use, uint32_t origin) {
    if (origin & ORIGIN_OPERAND) pointers_number(use, origin);
}

/* What a sum is. A plain number added leaves a byte what it was, whichever
 * of the two it is. Two bytes read as data, added, are neither of them. An
 * operand is a number as often as an address (`ld de, 20`): of a byte and an
 * operand, or of two operands, the one that was added to stays. The one that
 * does not stay was a number. */
static uint32_t pointers_sum(DataUse* use, uint32_t to, uint32_t added) {
    uint32_t stays = to;
    if (!to || !added) {
        stays = to ? to : added;
    } else if (!((to | added) & ORIGIN_OPERAND)) {
        stays = 0u;
    }
    if (to != stays) pointers_number(use, to);
    if (added != stays) pointers_number(use, added);
    return stays;
}

/* A difference: what a number was taken from stays, and what was taken was a number. */
static uint32_t pointers_less(DataUse* use, uint32_t from, uint32_t taken) {
    uint32_t stays = from;
    if (from && taken && !((from | taken) & ORIGIN_OPERAND)) stays = 0u;
    if (from != stays) pointers_number(use, from);
    pointers_number(use, taken);
    return stays;
}

static void pointers_push(struct Pointers* p, const GBContext* ctx, uint32_t high, uint32_t low) {
    const uint16_t top = (uint16_t)(ctx->sp - 1u);
    const uint16_t below = (uint16_t)(ctx->sp - 2u);
    if (top >= 0x8000u) *pointers_at(p, top) = high;
    if (below >= 0x8000u) *pointers_at(p, below) = low;
}

static uint32_t pointers_stacked(struct Pointers* p, uint16_t address) {
    return address >= 0x8000u ? *pointers_at(p, address) : 0u;
}

static void pointers_interrupt(DataUse* use, const GBContext* ctx) {
    /* Where to come back to has been pushed: no byte of the ROM. */
    for (unsigned i = 0; i < 2u; ++i) {
        const uint16_t address = (uint16_t)(ctx->sp + i);
        if (address >= 0x8000u) *pointers_at(use->pointers, address) = 0u;
    }
}

static void pointers_copied(DataUse* use, GBContext* ctx, uint16_t source, uint16_t target, unsigned count) {
    struct Pointers* p = use->pointers;
    for (unsigned i = 0; i < count; ++i) {
        const uint16_t from = (uint16_t)(source + i);
        const uint16_t to = (uint16_t)(target + i);
        uint32_t origin = 0u;
        if (from >= 0x8000u) {
            origin = *pointers_at(p, from);
        } else {
            const size_t offset = (size_t)gb_resolve_rom_bank(ctx, from) * 0x4000u + (from & 0x3FFFu);
            if (offset < use->rom_size) {
                p->reached[offset] |= REACHED_OTHER;
                origin = (uint32_t)offset + 1u;
            }
        }
        if (to >= 0x8000u) *pointers_at(p, to) = origin;
    }
}

static void pointers_decode(DataUse* use, GBContext* ctx, uint16_t pc, uint8_t opcode) {
    struct Pointers* p = use->pointers;
    uint32_t* reg = p->reg;
    const uint8_t low3 = opcode & 7u;
    const uint8_t mid3 = (opcode >> 3) & 7u;

    /* LD r,r' and LD r,(HL), LD (HL),r */
    if (opcode >= 0x40u && opcode < 0x80u) {
        if (opcode == 0x76u) return;
        const uint32_t origin = low3 == 6u ? pointers_read(use, ctx, ctx->hl, reg[4], reg[5]) : reg[low3];
        if (mid3 == 6u) {
            pointers_put(p, ctx->hl, origin);
        } else {
            reg[mid3] = origin;
        }
        return;
    }
    /* ALU A,r */
    if (opcode >= 0x80u && opcode < 0xC0u) {
        const uint32_t origin = low3 == 6u ? pointers_read(use, ctx, ctx->hl, reg[4], reg[5]) : reg[low3];
        if (mid3 == 7u) {                                         /* CP: nothing is kept, and both were numbers */
            if (low3 != 7u) {
                pointers_number(use, reg[7]);
                pointers_number(use, origin);
            }
            return;
        }
        if (mid3 < 2u) {                                          /* ADD, ADC: A and itself is twice A */
            if (low3 == 7u) pointers_number(use, reg[7]);
            reg[7] = low3 == 7u ? 0u : pointers_sum(use, reg[7], origin);
        } else if (mid3 < 4u) {                                   /* SUB, SBC: A less itself is nothing */
            /* Taking a register from a byte is how two are compared, and how far apart
             * two addresses are is reckoned: the byte was a number, whatever comes of it.
             * A constant taken (SUB n) is a step back along an address. */
            if (low3 != 7u) pointers_number(use, reg[7]);
            reg[7] = low3 == 7u ? 0u : pointers_less(use, reg[7], origin);
        } else if (!(low3 == 7u && mid3 != 5u)) {                 /* AND, XOR, OR: but AND A and OR A change nothing */
            /* The two halves of an address are put together with OR to see whether
             * there is an address at all: that makes no number of them. */
            if (low3 != 7u && mid3 != 6u) {
                pointers_number(use, reg[7]);
                pointers_number(use, origin);
            }
            reg[7] = 0u;
        }
        return;
    }
    switch (opcode) {
        /* LD r,n: a number */
        case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E:
            reg[mid3] = 0u;
            return;
        case 0x36:
            pointers_put(p, ctx->hl, 0u);
            return;
        /* INC (HL), DEC (HL): read, and one more or less is still what it was. INC r and DEC r likewise. */
        case 0x34: case 0x35:
            pointers_read(use, ctx, ctx->hl, reg[4], reg[5]);
            return;
        /* A through memory */
        case 0x0A: reg[7] = pointers_read(use, ctx, ctx->bc, reg[0], reg[1]); return;
        case 0x1A: reg[7] = pointers_read(use, ctx, ctx->de, reg[2], reg[3]); return;
        case 0x2A: case 0x3A: reg[7] = pointers_read(use, ctx, ctx->hl, reg[4], reg[5]); return;
        case 0x02: pointers_put(p, ctx->bc, reg[7]); return;
        case 0x12: pointers_put(p, ctx->de, reg[7]); return;
        case 0x22: case 0x32: pointers_put(p, ctx->hl, reg[7]); return;
        case 0xFA: case 0xEA: {
            const uint16_t address = (uint16_t)(gb_read8(ctx, (uint16_t)(pc + 1u)) |
                                                (gb_read8(ctx, (uint16_t)(pc + 2u)) << 8));
            if (opcode == 0xFA) {
                reg[7] = pointers_read(use, ctx, address, pointers_operand(use, ctx, (uint16_t)(pc + 2u)),
                                       pointers_operand(use, ctx, (uint16_t)(pc + 1u)));
            } else {
                pointers_put(p, address, reg[7]);
            }
            return;
        }
        case 0xF0: reg[7] = *pointers_at(p, (uint16_t)(0xFF00u | gb_read8(ctx, (uint16_t)(pc + 1u)))); return;
        case 0xE0: pointers_put(p, (uint16_t)(0xFF00u | gb_read8(ctx, (uint16_t)(pc + 1u))), reg[7]); return;
        case 0xF2: reg[7] = *pointers_at(p, (uint16_t)(0xFF00u | ctx->c)); return;
        case 0xE2: pointers_put(p, (uint16_t)(0xFF00u | ctx->c), reg[7]); return;
        /* A turned about, or cut with a constant, is no address any more. A
         * constant added to it or taken from it (C6 CE D6 DE) leaves it one. */
        case 0x07: case 0x0F: case 0x17: case 0x1F: case 0x27: case 0x2F:
        case 0xE6: case 0xEE: case 0xF6:
            pointers_number(use, reg[7]);
            reg[7] = 0u;
            return;
        case 0xFE:                                                /* CP n */
            pointers_number(use, reg[7]);
            return;
        /* Pairs. An operand may be an address. A pair stepped along is still the address it was. */
        case 0x01:
            reg[1] = pointers_operand(use, ctx, (uint16_t)(pc + 1u));
            reg[0] = pointers_operand(use, ctx, (uint16_t)(pc + 2u));
            return;
        case 0x11:
            reg[3] = pointers_operand(use, ctx, (uint16_t)(pc + 1u));
            reg[2] = pointers_operand(use, ctx, (uint16_t)(pc + 2u));
            return;
        case 0x21:
            reg[5] = pointers_operand(use, ctx, (uint16_t)(pc + 1u));
            reg[4] = pointers_operand(use, ctx, (uint16_t)(pc + 2u));
            return;
        case 0x09:
            reg[4] = pointers_sum(use, reg[4], reg[0]);
            reg[5] = pointers_sum(use, reg[5], reg[1]);
            return;
        case 0x19:
            reg[4] = pointers_sum(use, reg[4], reg[2]);
            reg[5] = pointers_sum(use, reg[5], reg[3]);
            return;
        case 0x29: case 0x39:                                     /* twice HL, or HL and the stack pointer */
            pointers_number(use, reg[4]);
            pointers_number(use, reg[5]);
            reg[4] = reg[5] = 0u;
            return;
        case 0xF8: reg[4] = reg[5] = 0u; return;
        case 0x08: {
            const uint16_t address = (uint16_t)(gb_read8(ctx, (uint16_t)(pc + 1u)) |
                                                (gb_read8(ctx, (uint16_t)(pc + 2u)) << 8));
            pointers_put(p, address, 0u);
            pointers_put(p, (uint16_t)(address + 1u), 0u);
            return;
        }
        /* A jump through HL, and a return to an address an operand made (`push hl` / `ret`):
         * what the operand made is where code is, and the game goes on from there. It cannot
         * be somewhere else. */
        case 0xE9:
            pointers_jumped(use, reg[4]);
            pointers_jumped(use, reg[5]);
            return;
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:
            if (!condition(ctx, opcode)) return;
            /* fall through */
        case 0xC9: case 0xD9:
            pointers_jumped(use, pointers_stacked(p, ctx->sp));
            pointers_jumped(use, pointers_stacked(p, (uint16_t)(ctx->sp + 1u)));
            return;
        /* The stack */
        case 0xC5: pointers_push(p, ctx, reg[0], reg[1]); return;
        case 0xD5: pointers_push(p, ctx, reg[2], reg[3]); return;
        case 0xE5: pointers_push(p, ctx, reg[4], reg[5]); return;
        case 0xF5: pointers_push(p, ctx, reg[7], 0u); return;
        case 0xC1: reg[1] = pointers_stacked(p, ctx->sp); reg[0] = pointers_stacked(p, (uint16_t)(ctx->sp + 1u)); return;
        case 0xD1: reg[3] = pointers_stacked(p, ctx->sp); reg[2] = pointers_stacked(p, (uint16_t)(ctx->sp + 1u)); return;
        case 0xE1: reg[5] = pointers_stacked(p, ctx->sp); reg[4] = pointers_stacked(p, (uint16_t)(ctx->sp + 1u)); return;
        case 0xF1: reg[7] = pointers_stacked(p, (uint16_t)(ctx->sp + 1u)); return;
        /* A call pushes where to come back to. */
        case 0xC4: case 0xCC: case 0xD4: case 0xDC:
            if (!condition(ctx, opcode)) return;
            /* fall through */
        case 0xCD:
        case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
            pointers_push(p, ctx, 0u, 0u);
            return;
        case 0xCB: {
            const uint8_t second = gb_read8(ctx, (uint16_t)(pc + 1u));
            const uint8_t which = second & 7u;
            const bool only_tests = (second >> 6) == 1u;             /* BIT */
            /* Shifted, or with a bit of it looked at or changed: a number. */
            if (which == 6u) {
                pointers_number(use, pointers_read(use, ctx, ctx->hl, reg[4], reg[5]));
                if (!only_tests) pointers_put(p, ctx->hl, 0u);
            } else {
                pointers_number(use, reg[which]);
                if (!only_tests) reg[which] = 0u;
            }
            return;
        }
        default:
            return;
    }
}

void gbrt_data_use_pointers_before(GBContext* ctx) {
    DataUse* use = (DataUse*)ctx->data_use;
    if (!use || !use->pointers) return;
    const uint16_t pc = ctx->pc;
    pointers_decode(use, ctx, pc, gb_read8(ctx, pc));
    /* Code in high RAM may be run several instructions at a time by a stub of
     * the runtime's, of which this is told the first: the wait after the
     * sprite table's copy is started, which counts down in A. */
    if (pc >= 0xFF80u) use->pointers->reg[7] = 0u;
}

static int pointers_in_order(const void* first, const void* second) {
    const PointerLink* a = (const PointerLink*)first;
    const PointerLink* b = (const PointerLink*)second;
    const uint32_t ours[4] = {a->low & ~ORIGIN_OPERAND, a->high & ~ORIGIN_OPERAND, a->low & ORIGIN_OPERAND, a->bank};
    const uint32_t theirs[4] = {b->low & ~ORIGIN_OPERAND, b->high & ~ORIGIN_OPERAND, b->low & ORIGIN_OPERAND, b->bank};
    for (unsigned i = 0; i < 4u; ++i) {
        if (ours[i] != theirs[i]) return ours[i] < theirs[i] ? -1 : 1;
    }
    return 0;
}

static void pointers_write(FILE* out, const DataUse* use, const GBContext* ctx) {
    const struct Pointers* p = use->pointers;
    (void)ctx;
    /* In the order the bytes lie in the ROM, where there is room to put them in it. */
    PointerLink* sorted = p->link_count ? (PointerLink*)malloc((size_t)p->link_count * sizeof(PointerLink)) : NULL;
    if (sorted) {
        memcpy(sorted, p->links, (size_t)p->link_count * sizeof(PointerLink));
        qsort(sorted, p->link_count, sizeof(PointerLink), pointers_in_order);
    }
    fprintf(out, ",\n \"pointers\": {\n  \"links\": [");
    for (uint32_t i = 0; i < p->link_count; ++i) {
        const PointerLink* link = sorted ? &sorted[i] : &p->links[i];
        fprintf(out, "%s\n   {\"low\": %lu, \"high\": %lu, \"operand\": %s, \"bank\": %lu, \"first\": %lu, "
                     "\"last\": %lu, \"least\": %ld, \"most\": %ld, \"reads\": %llu}",
                i ? "," : "", (unsigned long)((link->low & ~ORIGIN_OPERAND) - 1u),
                (unsigned long)((link->high & ~ORIGIN_OPERAND) - 1u), (link->low & ORIGIN_OPERAND) ? "true" : "false",
                (unsigned long)link->bank, (unsigned long)link->first, (unsigned long)link->last, (long)link->least,
                (long)link->most, (unsigned long long)link->reads);
    }
    free(sorted);
    fprintf(out, "%s],\n  \"reached\": [", p->link_count ? "\n  " : "");
    bool first = true;
    size_t offset = 0;
    while (offset < use->rom_size) {
        if (!p->reached[offset]) {
            ++offset;
            continue;
        }
        /* A run of bytes reached alike, in one bank. */
        size_t end = offset + 1u;
        const size_t bank_end = (offset / 0x4000u + 1u) * 0x4000u;
        const uint8_t how = p->reached[offset] & REACHED_ALL;
        while (end < use->rom_size && end < bank_end && (p->reached[end] & REACHED_ALL) == how) ++end;
        if (how) {
            fprintf(out, "%s\n   {\"offset\": %lu, \"size\": %lu, \"through\": [", first ? "" : ",",
                    (unsigned long)offset, (unsigned long)(end - offset));
            bool one = false;
            for (unsigned i = 0; i < 3u; ++i) {
                if (!(how & (1u << i))) continue;
                fprintf(out, "%s\"%s\"", one ? ", " : "", kReachedNames[i]);
                one = true;
            }
            fprintf(out, "]}");
            first = false;
        }
        offset = end;
    }
    /* The bytes that were numbers to the game, in runs. */
    fprintf(out, "%s],\n  \"numbers\": [", first ? "" : "\n  ");
    first = true;
    for (offset = 0; offset < use->rom_size; ++offset) {
        if (!(p->reached[offset] & WORKED_AS_NUMBER)) continue;
        size_t end = offset + 1u;
        while (end < use->rom_size && (p->reached[end] & WORKED_AS_NUMBER)) ++end;
        fprintf(out, "%s\n   {\"offset\": %lu, \"size\": %lu}", first ? "" : ",", (unsigned long)offset,
                (unsigned long)(end - offset));
        first = false;
        offset = end;
    }
    fprintf(out, "%s]\n }", first ? "" : "\n  ");
}
