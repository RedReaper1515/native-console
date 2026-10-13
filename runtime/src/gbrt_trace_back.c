/**
 * gbrt_trace_back.c - which instruction wrote a byte of RAM, and what the
 * value it wrote was made of. gbrt_trace_back.h says what for.
 */

#include "gbrt_trace_back.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The limits, and what happens past each: */
#define TB_SOURCES 8u        /* sources one value has: more are dropped, decisions first, and the event says so */
#define TB_CALLS 256u        /* calls inside one another: deeper ones are not told apart from the 256th */
#define TB_RANGES 16u        /* ranges of addresses watched: more are refused */
#define TB_SITES 1024u       /* writing instructions, each with its calls, kept for the record: more are counted */
#define TB_SITE_CALLS 24u    /* calls written out for one of them, the innermost: the record says how many there were */

/* ---------------------------------------------------------------------------
 * A source: one thing a value was made of, in 64 bits. The top three say what
 * kind it is, the next two how it took part, and the rest which one it is.
 * ------------------------------------------------------------------------- */

typedef uint64_t Src;

enum {
    K_ROM = 1,      /* a byte of the ROM read as data: its offset */
    K_CONSTANT,     /* the operand of an instruction: its offset in the ROM, or its address and value in RAM */
    K_HARDWARE,     /* a hardware register that was read: the register, and the instruction that read it */
    K_EVENT,        /* an earlier store: the event's id */
    K_START,        /* a byte of RAM, or a register, as it was when the trace began */
    K_UNSEEN,       /* a byte of RAM that is not what the last store the trace saw left there */
};

enum {
    ROLE_VALUE = 0,     /* copied or reckoned into the result */
    ROLE_ADDRESS = 1,   /* the value was loaded through it */
    ROLE_DECISION = 2,  /* a conditional tested it earlier in the call */
};

#define SRC(kind, payload) (((uint64_t)(kind) << 61) | (uint64_t)(payload))
#define SRC_KIND(s) ((unsigned)((s) >> 61))
#define SRC_ROLE(s) ((unsigned)(((s) >> 59) & 3u))
#define SRC_KEY(s) ((s) & ~((uint64_t)3u << 59))
#define SRC_PAYLOAD(s) ((s) & (((uint64_t)1u << 59) - 1u))
#define SRC_AS(s, role) (SRC_KEY(s) | ((uint64_t)(role) << 59))

#define CONSTANT_WIDE ((uint64_t)1u << 58)      /* two bytes, the low one first */
#define CONSTANT_IN_RAM ((uint64_t)1u << 57)    /* of an instruction in RAM: address, and value << 16 */
#define START_REGISTER ((uint64_t)1u << 58)     /* a register, not a byte: its number below */

#define MORE_VALUES 1u      /* a value or an address was dropped for want of room */
#define MORE_DECISIONS 2u   /* a decision was */

/* What a value was made of: at most TB_SOURCES, the values first, then the
 * addresses, then the decisions, so that what is dropped is the weakest. */
typedef struct {
    Src s[TB_SOURCES];
    uint8_t n;
    uint8_t more;
} Set;

/* The sets the machine's registers hold. B to L and A are numbered as the
 * opcodes number them, and the place of (HL) among them holds the zero flag
 * with N and H, which are followed as one thing. */
enum { RB, RC, RD, RE, RH, RL, RF, RA, RCY, RSP, REG_COUNT };
static const char* const kRegisterNames[REG_COUNT] = {"b", "c", "d", "e", "h", "l", "f", "a", "carry", "sp"};

/* What made an event. */
enum {
    WHAT_STORE,             /* an instruction stored the value */
    WHAT_PUSH,              /* PUSH put it on the stack */
    WHAT_RETURN_ADDRESS,    /* CALL or RST pushed where to come back to */
    WHAT_INTERRUPT,         /* an interrupt did */
    WHAT_OAM_COPY,          /* the hardware copied it into the sprite table (FF46) */
    WHAT_BLOCK_COPY,        /* the hardware copied it into video memory (HDMA) */
    WHAT_KINDS
};
static const char* const kWhatNames[WHAT_KINDS] = {
    "store", "push", "return_address", "interrupt", "oam_copy", "block_copy",
};

/* An event is two words and then its sources, in a ring of words:
 *   word 0: pc << 48 | bank << 32 | address << 16 | value << 8 | sources
 *   word 1: frame << 32 | cell << 8 | what << 4 | more
 * Its id is where it begins in the stream of words ever written, plus one,
 * so an id says by itself whether its event is still there. */
#define EVENT_PC(w) ((unsigned)((w)[0] >> 48))
#define EVENT_BANK(w) ((unsigned)(((w)[0] >> 32) & 0xFFFFu))
#define EVENT_ADDRESS(w) ((unsigned)(((w)[0] >> 16) & 0xFFFFu))
#define EVENT_VALUE(w) ((unsigned)(((w)[0] >> 8) & 0xFFu))
#define EVENT_SOURCES(w) ((unsigned)((w)[0] & 0xFFu))
#define EVENT_FRAME(w) ((unsigned long)((w)[1] >> 32))
#define EVENT_CELL(w) ((unsigned long)(((w)[1] >> 8) & 0xFFFFFFu))
#define EVENT_WHAT(w) ((unsigned)(((w)[1] >> 4) & 0xFu))
#define EVENT_MORE(w) ((unsigned)((w)[1] & 0xFu))

/* A call in progress. It ends when the stack pointer passes `sp`, where its
 * return address is: so a function it jumps to is still this call. */
typedef struct {
    uint16_t sp;
    uint16_t returns_to;
    uint16_t entry, entry_bank;
    uint16_t from, from_bank;
    uint8_t interrupt;
    Set decisions;      /* what its conditionals tested, the latest first */
    uint16_t low[TB_SOURCES], high[TB_SOURCES];     /* ...and the addresses each still has a say at */
} Frame;

typedef struct {
    uint16_t entry, entry_bank, from, from_bank, returns_to;
    uint8_t interrupt;
} Call;

/* One writing instruction with one set of calls around it. */
typedef struct {
    uint32_t cell, hash;
    uint16_t address, pc, bank;
    uint16_t call_count;        /* calls kept below, outermost first */
    uint16_t calls_in_all;      /* calls that were in progress */
    uint8_t first_value, last_value;
    uint64_t count, first_frame, last_frame;
    uint32_t trees;
    Call calls[TB_SITE_CALLS];
} Site;

typedef struct {
    uint32_t site;
    uint8_t value;
    uint64_t root, frame;
    uint32_t nodes, cut_depth, cut_nodes, forgotten;
} Tree;

/* An event kept for the record: copied out of the ring when a tree reached
 * it, since the ring may forget it before the record is written. */
typedef struct {
    uint64_t id;        /* 0: no entry */
    uint32_t at;        /* where its words are in `words`, or FORGOTTEN */
    uint32_t stamp;     /* the last tree that reached it */
} Kept;
#define FORGOTTEN 0xFFFFFFFFu

#define NO_CELL 0xFFFFFFFFu

typedef struct {
    Set reg[REG_COUNT];
    Frame frames[TB_CALLS + 1u];    /* frames[0] is the code no call was seen to enter */
    unsigned depth;
    uint64_t too_deep;
    uint8_t away;                   /* LD SP,HL took the stack pointer above the calls: they are not over */
    unsigned away_depth;
    uint8_t vector_pending;         /* an interrupt was taken: where it went shows at the next instruction */
    uint8_t page_known;             /* FF46 is being written by the instruction that is running */
    Set page;                       /* ...with a value made of these */
    uint16_t pc, bank;              /* the instruction that is running */

    /* A cell is a byte of memory that holds what is written to it, whatever
     * bank it is in: video memory, cartridge RAM, work RAM, the sprite
     * table, the hardware registers and high RAM. */
    uint32_t vram_base, eram_base, wram_base, oam_base, io_base, hram_base, cell_count;
    uint64_t* cells;                /* per cell: the id of the event that wrote it last, or 0 */

    uint64_t* ring;
    uint64_t ring_words;
    uint64_t head;                  /* words ever written */
    uint64_t events, instructions;
    uint32_t state_loads;

    uint8_t watched[0x10000u / 8u];
    uint64_t watched_writes;
    Site* sites;
    uint32_t site_count;
    uint64_t sites_dropped;         /* watched writes that no site had room for */
    Tree* trees;
    uint32_t tree_count;
    uint64_t trees_skipped;         /* watched writes that got no tree */

    Kept* kept;
    uint32_t kept_room;             /* a power of two */
    uint32_t kept_count;
    uint64_t* order;                /* the ids kept, in the order they were */
    uint64_t* words;
    uint32_t words_count, words_room;
    uint32_t stamp;
    uint64_t* queue;
    uint16_t* queue_depth;
    uint8_t failed;                 /* there was no memory for something: the record says so */
} TraceBack;

static char* g_path = NULL;
static struct { uint16_t first, last; } g_ranges[TB_RANGES];
static unsigned g_range_count = 0;
static unsigned long g_depth = 256u;
static unsigned long g_nodes = 4096u;
static unsigned long g_trees = 64u;
static unsigned long g_each = 1u;
static unsigned long g_kilobytes = 128u * 1024u;

void gbrt_trace_back_set_file(const char* path) {
    free(g_path);
    g_path = NULL;
    if (path && path[0]) {
        const size_t length = strlen(path) + 1u;
        g_path = (char*)malloc(length);
        if (g_path) memcpy(g_path, path, length);
    }
}

bool gbrt_trace_back_requested(void) {
    return g_path != NULL;
}

bool gbrt_trace_back_watching(void) {
    return g_range_count != 0;
}

static bool number(const char* text, int base, unsigned long least, unsigned long most, unsigned long* out,
                   const char** rest) {
    char* end = NULL;
    if (!text || !text[0] || text[0] == '-' || text[0] == '+' || text[0] == ' ') return false;
    const unsigned long value = strtoul(text, &end, base);
    if (end == text || value < least || value > most) return false;
    if (rest) {
        *rest = end;
    } else if (*end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

bool gbrt_trace_back_option(const char* name, const char* value) {
    if (!name || !value) return false;
    if (strcmp(name, "of") == 0) {
        unsigned long first = 0, last = 0;
        const char* rest = NULL;
        if (!number(value, 16, 0x8000u, 0xFFFFu, &first, &rest)) return false;
        last = first;
        if (*rest == '-') {
            if (!number(rest + 1, 16, first, 0xFFFFu, &last, NULL)) return false;
        } else if (*rest != '\0') {
            return false;
        }
        if (g_range_count >= TB_RANGES) return false;
        g_ranges[g_range_count].first = (uint16_t)first;
        g_ranges[g_range_count].last = (uint16_t)last;
        g_range_count++;
        return true;
    }
    if (strcmp(name, "depth") == 0) return number(value, 10, 1u, 60000u, &g_depth, NULL);
    if (strcmp(name, "nodes") == 0) return number(value, 10, 1u, 1000000u, &g_nodes, NULL);
    if (strcmp(name, "trees") == 0) return number(value, 10, 1u, 100000u, &g_trees, NULL);
    if (strcmp(name, "each") == 0) return number(value, 10, 1u, 100000u, &g_each, NULL);
    if (strcmp(name, "memory") == 0) {
        /* Megabytes, or kilobytes with a k after the number. */
        unsigned long amount = 0;
        const char* rest = NULL;
        if (!number(value, 10, 1u, 16u * 1024u * 1024u, &amount, &rest)) return false;
        if ((*rest == 'k' || *rest == 'K') && rest[1] == '\0') {
            if (amount < 16u) return false;
        } else if (*rest == '\0' || ((*rest == 'm' || *rest == 'M') && rest[1] == '\0')) {
            if (amount > 16u * 1024u) return false;
            amount *= 1024u;
        } else {
            return false;
        }
        g_kilobytes = amount;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Sets of sources.
 * ------------------------------------------------------------------------- */

static void set_none(Set* set) {
    set->n = 0;
    set->more = 0;
}

static void dropped(Set* set, unsigned role) {
    set->more |= role == ROLE_DECISION ? MORE_DECISIONS : MORE_VALUES;
}

/* Add a source. One that is there already stays once, in the stronger of its
 * two roles. With no room left the weakest goes: the last, or the new one. */
static void set_add(Set* set, Src src) {
    const uint64_t key = SRC_KEY(src);
    const unsigned role = SRC_ROLE(src);
    for (unsigned i = 0; i < set->n; ++i) {
        if (SRC_KEY(set->s[i]) != key) continue;
        if (SRC_ROLE(set->s[i]) <= role) return;
        memmove(&set->s[i], &set->s[i + 1u], (size_t)(set->n - i - 1u) * sizeof(Src));
        set->n--;
        break;
    }
    unsigned at = set->n;
    while (at > 0 && SRC_ROLE(set->s[at - 1u]) > role) --at;
    if (set->n == TB_SOURCES) {
        if (at == TB_SOURCES) {
            dropped(set, role);
            return;
        }
        dropped(set, SRC_ROLE(set->s[TB_SOURCES - 1u]));
        set->n--;
    }
    memmove(&set->s[at + 1u], &set->s[at], (size_t)(set->n - at) * sizeof(Src));
    set->s[at] = src;
    set->n++;
}

static void set_one(Set* set, Src src) {
    set->s[0] = src;
    set->n = 1;
    set->more = 0;
}

/* Add all of `from`, each in its own role or in `least` if that is weaker:
 * what an address was made of took part as an address, whatever it was to
 * the address. `from` must not be `set`. */
static void set_union(Set* set, const Set* from, unsigned least) {
    for (unsigned i = 0; i < from->n; ++i) {
        const unsigned role = SRC_ROLE(from->s[i]);
        set_add(set, SRC_AS(from->s[i], role > least ? role : least));
    }
    if (from->more) set->more |= least == ROLE_DECISION ? MORE_DECISIONS : from->more;
}

/* ---------------------------------------------------------------------------
 * Memory, as cells, and the ring of events.
 * ------------------------------------------------------------------------- */

static uint32_t cell_of(const TraceBack* t, const GBContext* ctx, uint16_t address) {
    if (address < 0x8000u) return NO_CELL;
    if (address < 0xA000u) return t->vram_base + (uint32_t)(ctx->vram_bank & 1u) * 0x2000u + (address - 0x8000u);
    if (address < 0xC000u) {
        if (!ctx->eram || !ctx->eram_size) return NO_CELL;
        return t->eram_base +
               (uint32_t)(((uint32_t)ctx->ram_bank * 0x2000u + (uint32_t)(address - 0xA000u)) % ctx->eram_size);
    }
    if (address < 0xD000u) return t->wram_base + (address - 0xC000u);
    if (address < 0xE000u) {
        const uint32_t index = (uint32_t)ctx->wram_bank * 0x1000u + (address - 0xD000u);
        return index < 0x8000u ? t->wram_base + index : NO_CELL;
    }
    if (address < 0xFE00u) return cell_of(t, ctx, (uint16_t)(address - 0x2000u));
    if (address < 0xFEA0u) return t->oam_base + (address - 0xFE00u);
    if (address < 0xFF00u) return NO_CELL;
    if (address < 0xFF80u) return t->io_base + (address - 0xFF00u);
    if (address < 0xFFFFu) return t->hram_base + (address - 0xFF80u);
    return t->io_base + 0x80u;
}

static bool is_hardware(const TraceBack* t, uint32_t cell) {
    return cell >= t->io_base && cell < t->hram_base;
}

/* An echo of work RAM is the byte it echoes. */
static uint16_t plain_address(uint16_t address) {
    return address >= 0xE000u && address < 0xFE00u ? (uint16_t)(address - 0x2000u) : address;
}

static const uint64_t* event_at(const TraceBack* t, uint64_t id) {
    if (!id) return NULL;
    const uint64_t begins = id - 1u;
    if (begins + t->ring_words < t->head) return NULL;      /* written over */
    return &t->ring[begins % t->ring_words];
}

static uint64_t event_new(TraceBack* t, const GBContext* ctx, uint16_t address, uint32_t cell, uint8_t value,
                          unsigned what, const Set* sources) {
    const unsigned n = sources ? sources->n : 0u;
    const unsigned length = 2u + n;
    uint64_t at = t->head % t->ring_words;
    if (at + length > t->ring_words) {
        /* An event does not go round the end: the rest of the ring is left. */
        t->head += t->ring_words - at;
        at = 0;
    }
    uint64_t* w = &t->ring[at];
    w[0] = ((uint64_t)t->pc << 48) | ((uint64_t)t->bank << 32) | ((uint64_t)address << 16) |
           ((uint64_t)value << 8) | n;
    w[1] = ((uint64_t)(uint32_t)ctx->completed_frames << 32) | ((uint64_t)cell << 8) | ((uint64_t)what << 4) |
           (sources ? sources->more : 0u);
    if (n) memcpy(&w[2], sources->s, (size_t)n * sizeof(Src));
    const uint64_t id = t->head + 1u;
    t->head += length;
    t->events++;
    return id;
}

/* ---------------------------------------------------------------------------
 * What the record keeps: events copied out of the ring, sites, trees.
 * ------------------------------------------------------------------------- */

static Kept* kept_slot(const TraceBack* t, uint64_t id) {
    uint32_t at = (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 40) & (t->kept_room - 1u);
    while (t->kept[at].id && t->kept[at].id != id) at = (at + 1u) & (t->kept_room - 1u);
    return &t->kept[at];
}

static Kept* kept_find(const TraceBack* t, uint64_t id) {
    Kept* slot = kept_slot(t, id);
    return slot->id ? slot : NULL;
}

/* Keep the event `id`: its words if the ring still has them, and otherwise
 * that it is forgotten. NULL if there is no memory. */
static Kept* keep(TraceBack* t, uint64_t id) {
    if ((t->kept_count + 1u) * 2u > t->kept_room) {
        const uint32_t room = t->kept_room * 2u;
        Kept* old = t->kept;
        Kept* grown = (Kept*)calloc(room, sizeof(Kept));
        uint64_t* order = (uint64_t*)realloc(t->order, (size_t)(room / 2u) * sizeof(uint64_t));
        if (order) t->order = order;
        if (!grown || !order) {
            free(grown);
            t->failed = 1;
            return NULL;
        }
        const uint32_t old_room = t->kept_room;
        t->kept = grown;
        t->kept_room = room;
        for (uint32_t i = 0; i < old_room; ++i) {
            if (old[i].id) *kept_slot(t, old[i].id) = old[i];
        }
        free(old);
    }
    const uint64_t* w = event_at(t, id);
    uint32_t at = FORGOTTEN;
    if (w) {
        const unsigned length = 2u + EVENT_SOURCES(w);
        if (t->words_count + length > t->words_room) {
            uint32_t room = t->words_room * 2u;
            uint64_t* grown = (uint64_t*)realloc(t->words, (size_t)room * sizeof(uint64_t));
            if (!grown) {
                t->failed = 1;
                return NULL;
            }
            t->words = grown;
            t->words_room = room;
        }
        at = t->words_count;
        memcpy(&t->words[at], w, (size_t)length * sizeof(uint64_t));
        t->words_count += length;
    }
    Kept* slot = kept_slot(t, id);
    slot->id = id;
    slot->at = at;
    slot->stamp = 0;
    t->order[t->kept_count++] = id;
    return slot;
}

/* Follow the sources of the event `root` back through the events they name,
 * the nearest first, and keep what is reached. */
static void capture(TraceBack* t, uint32_t site, uint64_t root, uint8_t value, uint64_t frame) {
    Tree* tree = &t->trees[t->tree_count];
    memset(tree, 0, sizeof(*tree));
    tree->site = site;
    tree->root = root;
    tree->value = value;
    tree->frame = frame;
    const uint32_t stamp = ++t->stamp;
    Kept* first = kept_find(t, root);
    if (!first) first = keep(t, root);
    if (!first) return;
    first->stamp = stamp;
    uint32_t head = 0, tail = 0;
    t->queue[tail] = root;
    t->queue_depth[tail++] = 0;
    while (head < tail) {
        const uint64_t id = t->queue[head];
        const unsigned depth = t->queue_depth[head++];
        const Kept* node = kept_find(t, id);
        if (!node || node->at == FORGOTTEN) {
            tree->forgotten++;
            continue;
        }
        Src sources[TB_SOURCES];
        const unsigned n = EVENT_SOURCES(&t->words[node->at]);
        memcpy(sources, &t->words[node->at + 2u], (size_t)n * sizeof(Src));    /* keep() below may move `words` */
        for (unsigned i = 0; i < n; ++i) {
            if (SRC_KIND(sources[i]) != K_EVENT) continue;
            const uint64_t next = SRC_PAYLOAD(sources[i]);
            Kept* there = kept_find(t, next);
            if (there && there->stamp == stamp) continue;
            if (depth + 1u > g_depth) {
                tree->cut_depth++;
                continue;
            }
            if (tail >= g_nodes) {
                tree->cut_nodes++;
                continue;
            }
            if (!there) there = keep(t, next);
            if (!there) return;
            there->stamp = stamp;
            t->queue[tail] = next;
            t->queue_depth[tail++] = (uint16_t)(depth + 1u);
        }
    }
    tree->nodes = tail;
    t->tree_count++;
    t->sites[site].trees++;
}

/* A watched byte was written by the event `id`. */
static void watched_write(TraceBack* t, const GBContext* ctx, uint16_t address, uint32_t cell, uint8_t value,
                          uint64_t id) {
    t->watched_writes++;
    /* The calls that count are the ones made since the innermost interrupt
     * was taken: where it broke in is chance. */
    unsigned lowest = 1u;
    for (unsigned depth = t->depth; depth >= 1u; --depth) {
        if (t->frames[depth].interrupt) {
            lowest = depth;
            break;
        }
    }
    uint32_t hash = 2166136261u;
    for (unsigned depth = lowest; depth <= t->depth; ++depth) {
        const Frame* frame = &t->frames[depth];
        hash = (hash ^ frame->entry) * 16777619u;
        hash = (hash ^ frame->entry_bank) * 16777619u;
        hash = (hash ^ frame->returns_to) * 16777619u;
        hash = (hash ^ frame->interrupt) * 16777619u;
    }
    Site* site = NULL;
    for (uint32_t i = 0; i < t->site_count; ++i) {
        Site* seen = &t->sites[i];
        if (seen->cell == cell && seen->pc == t->pc && seen->bank == t->bank && seen->hash == hash &&
            seen->address == address) {
            site = seen;
            break;
        }
    }
    if (!site) {
        if (t->site_count >= TB_SITES) {
            t->sites_dropped++;
            return;
        }
        site = &t->sites[t->site_count++];
        memset(site, 0, sizeof(*site));
        site->cell = cell;
        site->hash = hash;
        site->address = address;
        site->pc = t->pc;
        site->bank = t->bank;
        site->first_value = value;
        site->first_frame = ctx->completed_frames;
        site->calls_in_all = (uint16_t)(t->depth - lowest + 1u);
        unsigned from = lowest;
        if (t->depth - lowest + 1u > TB_SITE_CALLS) from = t->depth - TB_SITE_CALLS + 1u;
        for (unsigned depth = from; depth <= t->depth; ++depth) {
            const Frame* frame = &t->frames[depth];
            Call* call = &site->calls[site->call_count++];
            call->entry = frame->entry;
            call->entry_bank = frame->entry_bank;
            call->from = frame->from;
            call->from_bank = frame->from_bank;
            call->returns_to = frame->returns_to;
            call->interrupt = frame->interrupt;
        }
        if (t->depth < lowest) site->calls_in_all = 0;
    }
    site->count++;
    site->last_value = value;
    site->last_frame = ctx->completed_frames;
    if (site->trees >= g_each || t->tree_count >= g_trees) {
        t->trees_skipped++;
        return;
    }
    capture(t, (uint32_t)(site - t->sites), id, value, ctx->completed_frames);
}

/* ---------------------------------------------------------------------------
 * The calls in progress, and what their conditionals tested.
 * ------------------------------------------------------------------------- */

static Frame* current(TraceBack* t) {
    return &t->frames[t->depth];
}

/* The calls the stack pointer has passed are over. */
static void settle(TraceBack* t, uint16_t sp) {
    while (t->depth && sp > t->frames[t->depth].sp && (!t->away || t->depth > t->away_depth)) t->depth--;
}

static void enter(TraceBack* t, const GBContext* ctx, bool interrupt, uint16_t sp, uint16_t returns_to,
                  uint16_t entry) {
    if (t->depth >= TB_CALLS) {
        t->too_deep++;
        return;
    }
    Frame* frame = &t->frames[++t->depth];
    frame->sp = sp;
    frame->returns_to = returns_to;
    frame->entry = entry;
    frame->entry_bank = entry < 0x8000u ? gb_resolve_rom_bank(ctx, entry) : 0u;
    frame->from = t->pc;
    frame->from_bank = t->bank;
    frame->interrupt = interrupt ? 1u : 0u;
    set_none(&frame->decisions);
}

/* A conditional tested a flag made of `tested`, and the code from `low` to
 * `high` runs, or runs again, because of how the test came out: while the
 * call that is running is there, what it stores and reckons depends on those.
 * The latest are kept. Only what the flag was made of counts, not what had
 * decided that: a decision of a decision would spread what one function
 * tested into every function its registers reach. Nor does a constant the
 * tested value was only loaded through, the `wVariable` of `ld hl, wVariable`:
 * the variable is there as the event that wrote it. With `any_constant`
 * false no constant counts: a jump through an address an instruction spells
 * out is decided by nothing. */
static void decide(TraceBack* t, const Set* tested, uint16_t low, uint16_t high, bool any_constant) {
    Frame* frame = current(t);
    Set* decisions = &frame->decisions;
    for (unsigned i = tested->n; i-- > 0;) {
        if (SRC_ROLE(tested->s[i]) == ROLE_DECISION) continue;
        if (SRC_KIND(tested->s[i]) == K_CONSTANT && (!any_constant || SRC_ROLE(tested->s[i]) == ROLE_ADDRESS)) continue;
        const Src src = SRC_AS(tested->s[i], ROLE_DECISION);
        uint16_t from = low, to = high;
        unsigned at = 0;
        while (at < decisions->n && decisions->s[at] != src) ++at;
        if (at < decisions->n) {
            /* Tested before: it has a say wherever either test gave it one. */
            if (frame->low[at] < from) from = frame->low[at];
            if (frame->high[at] > to) to = frame->high[at];
        } else if (decisions->n == TB_SOURCES) {
            at = TB_SOURCES - 1u;
            decisions->more |= MORE_DECISIONS;
        } else {
            decisions->n++;
        }
        for (; at > 0; --at) {
            decisions->s[at] = decisions->s[at - 1u];
            frame->low[at] = frame->low[at - 1u];
            frame->high[at] = frame->high[at - 1u];
        }
        decisions->s[0] = src;
        frame->low[0] = from;
        frame->high[0] = to;
    }
    if (tested->more & MORE_VALUES) decisions->more |= MORE_DECISIONS;
}

/* The call that is running has come to `pc`: what was decided for other
 * code has no say here. */
static void expire(TraceBack* t, uint16_t pc) {
    Frame* frame = current(t);
    Set* decisions = &frame->decisions;
    unsigned kept = 0;
    for (unsigned i = 0; i < decisions->n; ++i) {
        if (pc < frame->low[i] || pc > frame->high[i]) continue;
        decisions->s[kept] = decisions->s[i];
        frame->low[kept] = frame->low[i];
        frame->high[kept] = frame->high[i];
        kept++;
    }
    decisions->n = (uint8_t)kept;
    if (!kept) decisions->more = 0;
}

static bool condition(const GBContext* ctx, uint8_t opcode);

/* JR cc or JP cc at `pc`, to `target`. What depends on the test is the code
 * the jump guards:
 *   not taken, forward    the code up to the target, which a taken jump skips
 *   taken, backward       the loop from the target to here, which runs again
 *   taken, forward        what is at the target, where nothing runs on into
 *                         it: the code before it ends in a jump over it (an
 *                         else, as far as that jump goes) or in a return
 *   not taken, backward   nothing: what follows a loop follows it anyway
 * A jump over code that runs on into the target decides nothing at the
 * target. That leaves out what a skipped store would have changed, which is
 * a dependence too, and one this cannot see. */
static void branch(TraceBack* t, GBContext* ctx, uint8_t opcode, const Set* tested, uint16_t pc, uint16_t target) {
    uint16_t low, high;
    if (!condition(ctx, opcode)) {
        if (target <= pc) return;
        low = pc;
        high = (uint16_t)(target - 1u);
    } else if (target <= pc) {
        low = target;
        high = pc;
    } else {
        const uint8_t last = gbrt_peek8(ctx, (uint16_t)(target - 1u));
        const uint8_t second = gbrt_peek8(ctx, (uint16_t)(target - 2u));
        const uint8_t third = gbrt_peek8(ctx, (uint16_t)(target - 3u));
        uint16_t over = 0;
        if (second == 0x18u) {
            over = (uint16_t)(target + (int8_t)last);       /* JR e */
        } else if (third == 0xC3u) {
            over = (uint16_t)(second | (last << 8));        /* JP nn */
        } else if (last != 0xC9u && last != 0xD9u && last != 0xE9u) {
            return;                                         /* neither RET, RETI nor JP HL */
        }
        low = target;
        high = over > target ? (uint16_t)(over - 1u) : 0xFFFFu;
    }
    decide(t, tested, low, high, true);
}

/* A register is given a value made of `from`, by the call that is running:
 * so what that call has decided on had a say in it too. */
static void assign(TraceBack* t, Set* to, const Set* from) {
    Set made = *from;
    set_union(&made, &current(t)->decisions, ROLE_DECISION);
    *to = made;
}

/* ---------------------------------------------------------------------------
 * Loads and stores.
 * ------------------------------------------------------------------------- */

static Src constant(const GBContext* ctx, uint16_t address, bool wide) {
    if (address < 0x8000u) {
        const uint64_t offset = (uint64_t)gb_resolve_rom_bank(ctx, address) * 0x4000u + (address & 0x3FFFu);
        return SRC(K_CONSTANT, offset | (wide ? CONSTANT_WIDE : 0u));
    }
    uint64_t value = gbrt_peek8((GBContext*)ctx, address);
    if (wide) value |= (uint64_t)gbrt_peek8((GBContext*)ctx, (uint16_t)(address + 1u)) << 8;
    return SRC(K_CONSTANT, CONSTANT_IN_RAM | (wide ? CONSTANT_WIDE : 0u) | address | (value << 16));
}

/* What the byte at `address` is made of, read through an address made of
 * `high` and `low` (either may be NULL: the address was spelled out). */
static void load(TraceBack* t, GBContext* ctx, uint16_t address, const Set* high, const Set* low, Set* out) {
    set_none(out);
    if (address < 0x8000u) {
        const size_t offset = (size_t)gb_resolve_rom_bank(ctx, address) * 0x4000u + (address & 0x3FFFu);
        if (offset < ctx->rom_size) set_add(out, SRC(K_ROM, offset));
    } else {
        const uint32_t cell = cell_of(t, ctx, address);
        if (cell != NO_CELL) {
            const uint64_t id = t->cells[cell];
            const uint64_t* event = event_at(t, id);
            const uint64_t where = (uint64_t)plain_address(address) | ((uint64_t)cell << 16);
            if (is_hardware(t, cell)) {
                /* What a register reads as is the hardware's to say. Where it
                 * holds what was written to it, that write is a source too. */
                set_add(out, SRC(K_HARDWARE, (uint64_t)address | ((uint64_t)t->pc << 16) | ((uint64_t)t->bank << 32)));
                if (event && EVENT_VALUE(event) == gbrt_peek8(ctx, address)) set_add(out, SRC(K_EVENT, id));
            } else if (!id) {
                set_add(out, SRC(K_START, where));
            } else if (event && EVENT_VALUE(event) != gbrt_peek8(ctx, address)) {
                /* Something that is no instruction changed it: a write the
                 * hardware dropped, or one made from outside the game. */
                set_add(out, SRC(K_UNSEEN, where));
            } else {
                set_add(out, SRC(K_EVENT, id));
            }
        }
    }
    if (high) set_union(out, high, ROLE_ADDRESS);
    if (low) set_union(out, low, ROLE_ADDRESS);
}

/* The byte at `address` is given `value`, made of `sources` (NULL: nothing). */
static void store(TraceBack* t, GBContext* ctx, uint16_t address, uint8_t value, const Set* sources, unsigned what) {
    if (address == 0xFF46u && what == WHAT_STORE) {
        /* The sprite table is about to be copied from the page this names. */
        t->page_known = 1;
        if (sources) {
            t->page = *sources;
        } else {
            set_none(&t->page);
        }
    }
    const uint32_t cell = cell_of(t, ctx, address);
    if (cell == NO_CELL) return;
    Set made;
    if (sources) {
        made = *sources;
    } else {
        set_none(&made);
    }
    if (what == WHAT_STORE || what == WHAT_PUSH) set_union(&made, &current(t)->decisions, ROLE_DECISION);
    const uint64_t id = event_new(t, ctx, plain_address(address), cell, value, what, &made);
    t->cells[cell] = id;
    const uint16_t plain = plain_address(address);
    if (t->watched[plain >> 3] & (1u << (plain & 7u))) watched_write(t, ctx, plain, cell, value, id);
}

void gbrt_trace_back_copied(GBContext* ctx, uint16_t source, uint16_t target, unsigned count) {
    TraceBack* t = (TraceBack*)ctx->trace_back;
    if (!t) return;
    const bool sprites = target >= 0xFE00u;
    for (unsigned i = 0; i < count; ++i) {
        uint16_t from = (uint16_t)(source + i);
        if (from >= 0xE000u) from = (uint16_t)(from - 0x2000u);    /* the copy sees work RAM there */
        Set made;
        load(t, ctx, from, sprites && t->page_known ? &t->page : NULL, NULL, &made);
        store(t, ctx, (uint16_t)(target + i), gbrt_peek8(ctx, from), &made, sprites ? WHAT_OAM_COPY : WHAT_BLOCK_COPY);
    }
}

void gbrt_trace_back_interrupt(GBContext* ctx) {
    TraceBack* t = (TraceBack*)ctx->trace_back;
    if (!t) return;
    /* The game is still where it will come back to. */
    t->pc = ctx->pc;
    t->bank = ctx->pc < 0x8000u ? gb_resolve_rom_bank(ctx, ctx->pc) : 0u;
    settle(t, (uint16_t)(ctx->sp + 2u));
    store(t, ctx, (uint16_t)(ctx->sp + 1u), (uint8_t)(ctx->pc >> 8), NULL, WHAT_INTERRUPT);
    store(t, ctx, ctx->sp, (uint8_t)ctx->pc, NULL, WHAT_INTERRUPT);
    const unsigned before = t->depth;
    enter(t, ctx, true, ctx->sp, ctx->pc, 0u);
    t->vector_pending = t->depth != before;
}

void gbrt_trace_back_reset(GBContext* ctx) {
    TraceBack* t = ctx ? (TraceBack*)ctx->trace_back : NULL;
    if (!t) return;
    memset(t->cells, 0, (size_t)t->cell_count * sizeof(uint64_t));
    for (unsigned i = 0; i < REG_COUNT; ++i) set_one(&t->reg[i], SRC(K_START, START_REGISTER | i));
    t->depth = 0;
    set_none(&t->frames[0].decisions);
    t->away = 0;
    t->vector_pending = 0;
    t->page_known = 0;
    t->state_loads++;
}

/* ---------------------------------------------------------------------------
 * An instruction at a time.
 * ------------------------------------------------------------------------- */

static bool condition(const GBContext* ctx, uint8_t opcode) {
    switch ((opcode >> 3) & 3u) {
        case 0: return !ctx->f_z;
        case 1: return ctx->f_z != 0;
        case 2: return !ctx->f_c;
        default: return ctx->f_c != 0;
    }
}

/* The flag a conditional tests: Z for the first two conditions, carry for the others. */
static const Set* tested(const TraceBack* t, uint8_t opcode) {
    return &t->reg[((opcode >> 3) & 3u) < 2u ? RF : RCY];
}

static uint8_t register_value(const GBContext* ctx, unsigned which) {
    switch (which) {
        case RB: return ctx->b;
        case RC: return ctx->c;
        case RD: return ctx->d;
        case RE: return ctx->e;
        case RH: return ctx->h;
        case RL: return ctx->l;
        default: return ctx->a;
    }
}

/* ADD, ADC, SUB, SBC, AND, XOR, OR, CP of A with something made of `operand`. */
static void arithmetic(TraceBack* t, unsigned kind, const Set* operand, bool with_itself) {
    Set* reg = t->reg;
    Set result, carry;
    set_none(&carry);
    if (with_itself && (kind == 2u || kind == 5u || kind == 7u)) {
        set_none(&result);                      /* SUB A, XOR A, CP A: nought, whatever A was */
    } else if (with_itself && kind == 3u) {
        result = reg[RCY];                      /* SBC A,A: the carry, in every bit */
    } else {
        result = reg[RA];
        if (!with_itself) set_union(&result, operand, ROLE_VALUE);
        if (kind == 1u || kind == 3u) set_union(&result, &reg[RCY], ROLE_VALUE);
    }
    if (kind <= 3u || kind == 7u) carry = result;
    /* CP keeps nothing, and AND A and OR A leave A what it was. */
    if (kind != 7u && !(with_itself && (kind == 4u || kind == 6u))) assign(t, &reg[RA], &result);
    assign(t, &reg[RF], &result);
    assign(t, &reg[RCY], &carry);
}

static void call(TraceBack* t, GBContext* ctx, uint16_t target, uint16_t returns_to) {
    store(t, ctx, (uint16_t)(ctx->sp - 1u), (uint8_t)(returns_to >> 8), NULL, WHAT_RETURN_ADDRESS);
    store(t, ctx, (uint16_t)(ctx->sp - 2u), (uint8_t)returns_to, NULL, WHAT_RETURN_ADDRESS);
    enter(t, ctx, false, (uint16_t)(ctx->sp - 2u), returns_to, target);
}

static void push(TraceBack* t, GBContext* ctx, uint8_t high, const Set* high_of, uint8_t low, const Set* low_of) {
    store(t, ctx, (uint16_t)(ctx->sp - 1u), high, high_of, WHAT_PUSH);
    store(t, ctx, (uint16_t)(ctx->sp - 2u), low, low_of, WHAT_PUSH);
}

/* What POP takes off the stack. With the stack pointer in the ROM it is a
 * read like any other, through whatever pointed the stack pointer there. */
static void pop(TraceBack* t, GBContext* ctx, Set* high, Set* low) {
    const Set* through = ctx->sp < 0x8000u ? &t->reg[RSP] : NULL;
    load(t, ctx, ctx->sp, through, NULL, low);
    load(t, ctx, (uint16_t)(ctx->sp + 1u), through, NULL, high);
}

static uint8_t shifted(const GBContext* ctx, unsigned kind, uint8_t value) {
    const unsigned carry = ctx->f_c ? 1u : 0u;
    switch (kind) {
        case 0: return (uint8_t)((value << 1) | (value >> 7));
        case 1: return (uint8_t)((value >> 1) | (value << 7));
        case 2: return (uint8_t)((value << 1) | carry);
        case 3: return (uint8_t)((value >> 1) | (carry << 7));
        case 4: return (uint8_t)(value << 1);
        case 5: return (uint8_t)((value >> 1) | (value & 0x80u));
        case 6: return (uint8_t)((value << 4) | (value >> 4));
        default: return (uint8_t)(value >> 1);
    }
}

static void prefixed(TraceBack* t, GBContext* ctx, uint16_t pc) {
    Set* reg = t->reg;
    const uint8_t second = gbrt_peek8(ctx, (uint16_t)(pc + 1u));
    const unsigned which = second & 7u;
    const unsigned group = second >> 6;
    const unsigned bit = (second >> 3) & 7u;
    Set before, after, none;
    set_none(&none);
    uint8_t value = 0;
    if (which == 6u) {
        load(t, ctx, ctx->hl, &reg[RH], &reg[RL], &before);
        value = gbrt_peek8(ctx, ctx->hl);
    } else {
        before = reg[which];
    }
    if (group == 1u) {                              /* BIT: only the flag */
        assign(t, &reg[RF], &before);
        return;
    }
    after = before;
    if (group == 0u) {
        /* RL and RR take the carry in; all but SWAP put a bit out into it. */
        if (bit == 2u || bit == 3u) set_union(&after, &reg[RCY], ROLE_VALUE);
        value = shifted(ctx, bit, value);
    } else if (group == 2u) {
        value = (uint8_t)(value & ~(1u << bit));
    } else {
        value = (uint8_t)(value | (1u << bit));
    }
    if (which == 6u) {
        store(t, ctx, ctx->hl, value, &after, WHAT_STORE);
    } else {
        assign(t, &reg[which], &after);
    }
    if (group == 0u) {
        assign(t, &reg[RF], &after);
        assign(t, &reg[RCY], bit == 6u ? &none : &before);
    }
}

void gbrt_trace_back_before(GBContext* ctx) {
    TraceBack* t = (TraceBack*)ctx->trace_back;
    if (!t) return;
    Set* reg = t->reg;
    const uint16_t pc = ctx->pc;
    t->instructions++;
    t->pc = pc;
    t->bank = pc < 0x8000u ? gb_resolve_rom_bank(ctx, pc) : 0u;
    t->page_known = 0;
    settle(t, ctx->sp);
    if (t->vector_pending) {
        /* An interrupt was taken, and this is where it went. */
        if (t->depth && t->frames[t->depth].interrupt && !t->frames[t->depth].entry) {
            t->frames[t->depth].entry = pc;
            t->frames[t->depth].entry_bank = t->bank;
        }
        t->vector_pending = 0;
    }
    if (current(t)->decisions.n) expire(t, pc);
    const uint8_t opcode = gbrt_peek8(ctx, pc);
    const unsigned low3 = opcode & 7u;
    const unsigned mid3 = (opcode >> 3) & 7u;
    const uint16_t next = (uint16_t)(pc + 1u);
    Set made, other, none;
    set_none(&none);

    /* LD r,r' and LD r,(HL), LD (HL),r */
    if (opcode >= 0x40u && opcode < 0x80u) {
        if (opcode == 0x76u) return;
        if (low3 == 6u) {
            load(t, ctx, ctx->hl, &reg[RH], &reg[RL], &made);
            assign(t, &reg[mid3], &made);
        } else if (mid3 == 6u) {
            store(t, ctx, ctx->hl, register_value(ctx, low3), &reg[low3], WHAT_STORE);
        } else if (mid3 != low3) {
            assign(t, &reg[mid3], &reg[low3]);
        }
        return;
    }
    /* Arithmetic on A with a register or (HL) */
    if (opcode >= 0x80u && opcode < 0xC0u) {
        if (low3 == 6u) {
            load(t, ctx, ctx->hl, &reg[RH], &reg[RL], &made);
        } else {
            made = reg[low3];
        }
        arithmetic(t, mid3, &made, low3 == 7u);
        return;
    }
    switch (opcode) {
        /* LD r,n and LD (HL),n */
        case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E:
            set_one(&made, constant(ctx, next, false));
            assign(t, &reg[mid3], &made);
            return;
        case 0x36:
            set_one(&made, constant(ctx, next, false));
            store(t, ctx, ctx->hl, gbrt_peek8(ctx, next), &made, WHAT_STORE);
            return;
        /* INC r, DEC r: the value stepped is still made of what it was */
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x3C:
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x3D:
            assign(t, &reg[mid3], &reg[mid3]);
            assign(t, &reg[RF], &reg[mid3]);
            return;
        case 0x34: case 0x35:
            load(t, ctx, ctx->hl, &reg[RH], &reg[RL], &made);
            store(t, ctx, ctx->hl, (uint8_t)(gbrt_peek8(ctx, ctx->hl) + (opcode == 0x34u ? 1u : 0xFFu)), &made,
                  WHAT_STORE);
            assign(t, &reg[RF], &made);
            return;
        /* A through memory */
        case 0x0A:
            load(t, ctx, ctx->bc, &reg[RB], &reg[RC], &made);
            assign(t, &reg[RA], &made);
            return;
        case 0x1A:
            load(t, ctx, ctx->de, &reg[RD], &reg[RE], &made);
            assign(t, &reg[RA], &made);
            return;
        case 0x2A: case 0x3A:
            load(t, ctx, ctx->hl, &reg[RH], &reg[RL], &made);
            assign(t, &reg[RA], &made);
            return;
        case 0x02: store(t, ctx, ctx->bc, ctx->a, &reg[RA], WHAT_STORE); return;
        case 0x12: store(t, ctx, ctx->de, ctx->a, &reg[RA], WHAT_STORE); return;
        case 0x22: case 0x32: store(t, ctx, ctx->hl, ctx->a, &reg[RA], WHAT_STORE); return;
        case 0xFA: case 0xEA: {
            const uint16_t address = (uint16_t)(gbrt_peek8(ctx, next) | (gbrt_peek8(ctx, (uint16_t)(pc + 2u)) << 8));
            if (opcode == 0xFAu) {
                load(t, ctx, address, NULL, NULL, &made);
                assign(t, &reg[RA], &made);
            } else {
                store(t, ctx, address, ctx->a, &reg[RA], WHAT_STORE);
            }
            return;
        }
        case 0xF0:
            load(t, ctx, (uint16_t)(0xFF00u | gbrt_peek8(ctx, next)), NULL, NULL, &made);
            assign(t, &reg[RA], &made);
            return;
        case 0xE0: store(t, ctx, (uint16_t)(0xFF00u | gbrt_peek8(ctx, next)), ctx->a, &reg[RA], WHAT_STORE); return;
        case 0xF2:
            load(t, ctx, (uint16_t)(0xFF00u | ctx->c), NULL, &reg[RC], &made);
            assign(t, &reg[RA], &made);
            return;
        case 0xE2: store(t, ctx, (uint16_t)(0xFF00u | ctx->c), ctx->a, &reg[RA], WHAT_STORE); return;
        /* A by itself */
        case 0x07: case 0x0F:                       /* RLCA, RRCA: a bit of A into the carry */
            made = reg[RA];
            assign(t, &reg[RCY], &made);
            assign(t, &reg[RF], &none);
            assign(t, &reg[RA], &made);
            return;
        case 0x17: case 0x1F:                       /* RLA, RRA: and the carry into A */
            made = reg[RA];
            other = reg[RA];
            set_union(&made, &reg[RCY], ROLE_VALUE);
            assign(t, &reg[RCY], &other);
            assign(t, &reg[RF], &none);
            assign(t, &reg[RA], &made);
            return;
        case 0x27:                                  /* DAA: A put right by the flags the last sum left */
            made = reg[RA];
            set_union(&made, &reg[RCY], ROLE_VALUE);
            set_union(&made, &reg[RF], ROLE_VALUE);
            assign(t, &reg[RA], &made);
            assign(t, &reg[RCY], &made);
            assign(t, &reg[RF], &made);
            return;
        case 0x2F: assign(t, &reg[RA], &reg[RA]); return;       /* CPL */
        case 0x37: assign(t, &reg[RCY], &none); return;         /* SCF */
        case 0x3F: assign(t, &reg[RCY], &reg[RCY]); return;     /* CCF */
        /* A with a constant */
        case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
            set_one(&made, constant(ctx, next, false));
            arithmetic(t, mid3, &made, false);
            return;
        /* Pairs */
        case 0x01: case 0x11: case 0x21:
            set_one(&made, constant(ctx, next, true));
            assign(t, &reg[mid3 & 6u], &made);
            assign(t, &reg[(mid3 & 6u) + 1u], &made);
            return;
        case 0x31:
            set_one(&made, constant(ctx, next, true));
            assign(t, &reg[RSP], &made);
            t->away = 0;
            return;
        case 0xF9:
            /* The stack pointer is pointed somewhere by hand. Above the calls
             * in progress, they are not over: a game that reads with POP comes
             * back to its stack the same way. */
            made = reg[RH];
            set_union(&made, &reg[RL], ROLE_VALUE);
            assign(t, &reg[RSP], &made);
            if (!t->away) t->away_depth = t->depth;
            t->away = t->away_depth && ctx->hl > t->frames[t->away_depth].sp;
            return;
        case 0x08: {
            const uint16_t address = (uint16_t)(gbrt_peek8(ctx, next) | (gbrt_peek8(ctx, (uint16_t)(pc + 2u)) << 8));
            store(t, ctx, address, (uint8_t)ctx->sp, &reg[RSP], WHAT_STORE);
            store(t, ctx, (uint16_t)(address + 1u), (uint8_t)(ctx->sp >> 8), &reg[RSP], WHAT_STORE);
            return;
        }
        case 0x09: case 0x19: case 0x29: case 0x39: {
            /* ADD HL,rr: the low half is the two low halves, and the high half
             * all four, since the low sum carries into it. */
            Set low = reg[RL];
            Set high = reg[RH];
            if (opcode == 0x39u) {
                set_union(&low, &reg[RSP], ROLE_VALUE);
            } else if (opcode != 0x29u) {
                set_union(&low, &reg[(mid3 & 6u) + 1u], ROLE_VALUE);
                set_union(&high, &reg[mid3 & 6u], ROLE_VALUE);
            }
            set_union(&high, &low, ROLE_VALUE);
            assign(t, &reg[RL], &low);
            assign(t, &reg[RH], &high);
            assign(t, &reg[RCY], &high);
            return;
        }
        case 0xE8: case 0xF8:                       /* ADD SP,e and LD HL,SP+e */
            made = reg[RSP];
            set_add(&made, constant(ctx, next, false));
            if (opcode == 0xE8u) {
                assign(t, &reg[RSP], &made);
            } else {
                assign(t, &reg[RH], &made);
                assign(t, &reg[RL], &made);
            }
            assign(t, &reg[RF], &none);
            assign(t, &reg[RCY], &made);
            return;
        /* The stack */
        case 0xC5: push(t, ctx, ctx->b, &reg[RB], ctx->c, &reg[RC]); return;
        case 0xD5: push(t, ctx, ctx->d, &reg[RD], ctx->e, &reg[RE]); return;
        case 0xE5: push(t, ctx, ctx->h, &reg[RH], ctx->l, &reg[RL]); return;
        case 0xF5: {
            const uint8_t flags = (uint8_t)((ctx->f_z ? 0x80u : 0u) | (ctx->f_n ? 0x40u : 0u) |
                                            (ctx->f_h ? 0x20u : 0u) | (ctx->f_c ? 0x10u : 0u));
            made = reg[RF];
            set_union(&made, &reg[RCY], ROLE_VALUE);
            push(t, ctx, ctx->a, &reg[RA], flags, &made);
            return;
        }
        case 0xC1: case 0xD1: case 0xE1:
            pop(t, ctx, &made, &other);
            assign(t, &reg[mid3 & 6u], &made);
            assign(t, &reg[(mid3 & 6u) + 1u], &other);
            return;
        case 0xF1:
            pop(t, ctx, &made, &other);
            assign(t, &reg[RA], &made);
            assign(t, &reg[RF], &other);
            assign(t, &reg[RCY], &other);
            return;
        /* Where the code goes on is decided by what a conditional tests, and
         * by what an address jumped through was made of: all that the call
         * does from there on. */
        case 0xE9:
            made = reg[RH];
            set_union(&made, &reg[RL], ROLE_VALUE);
            decide(t, &made, 0u, 0xFFFFu, false);
            return;
        case 0x20: case 0x28: case 0x30: case 0x38:
            made = *tested(t, opcode);
            branch(t, ctx, opcode, &made, pc, (uint16_t)(pc + 2u + (int8_t)gbrt_peek8(ctx, next)));
            return;
        case 0xC2: case 0xCA: case 0xD2: case 0xDA:
            made = *tested(t, opcode);
            branch(t, ctx, opcode, &made, pc,
                   (uint16_t)(gbrt_peek8(ctx, next) | (gbrt_peek8(ctx, (uint16_t)(pc + 2u)) << 8)));
            return;
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:
            /* A return not made: the rest of the function runs because of it. */
            made = *tested(t, opcode);
            if (!condition(ctx, opcode)) decide(t, &made, pc, 0xFFFFu, true);
            return;
        case 0xC4: case 0xCC: case 0xD4: case 0xDC:
            if (!condition(ctx, opcode)) return;
            /* A call made on a condition: all the function called does depends on it. */
            made = *tested(t, opcode);
            call(t, ctx, (uint16_t)(gbrt_peek8(ctx, next) | (gbrt_peek8(ctx, (uint16_t)(pc + 2u)) << 8)),
                 (uint16_t)(pc + 3u));
            if (t->frames[t->depth].sp == (uint16_t)(ctx->sp - 2u)) decide(t, &made, 0u, 0xFFFFu, true);
            return;
        case 0xCD:
            call(t, ctx, (uint16_t)(gbrt_peek8(ctx, next) | (gbrt_peek8(ctx, (uint16_t)(pc + 2u)) << 8)),
                 (uint16_t)(pc + 3u));
            return;
        case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
            call(t, ctx, (uint16_t)(opcode & 0x38u), next);
            return;
        case 0xCB:
            prefixed(t, ctx, pc);
            return;
        default:
            /* INC rr and DEC rr step a pair that is still what it was made
             * of; returns, jumps and the rest move no data. */
            return;
    }
}

/* ---------------------------------------------------------------------------
 * Starting, and writing the record.
 * ------------------------------------------------------------------------- */

void gbrt_trace_back_destroy(GBContext* ctx) {
    TraceBack* t = ctx ? (TraceBack*)ctx->trace_back : NULL;
    if (!t) return;
    free(t->cells);
    free(t->ring);
    free(t->sites);
    free(t->trees);
    free(t->kept);
    free(t->order);
    free(t->words);
    free(t->queue);
    free(t->queue_depth);
    free(t);
    ctx->trace_back = NULL;
}

bool gbrt_trace_back_start(GBContext* ctx) {
    if (!g_path) return g_range_count == 0;
    if (!ctx || ctx->trace_back) return true;
    if (!ctx->rom || ctx->rom_size == 0 || !g_range_count) return false;
    TraceBack* t = (TraceBack*)calloc(1, sizeof(TraceBack));
    if (!t) return false;
    ctx->trace_back = t;
    t->vram_base = 0;
    t->eram_base = t->vram_base + 0x4000u;
    t->wram_base = t->eram_base + (uint32_t)(ctx->eram ? ctx->eram_size : 0u);
    t->oam_base = t->wram_base + 0x8000u;
    t->io_base = t->oam_base + 0xA0u;
    t->hram_base = t->io_base + 0x81u;
    t->cell_count = t->hram_base + 0x7Fu;
    t->ring_words = (uint64_t)g_kilobytes * (1024u / 8u);
    t->kept_room = 1024u;
    t->words_room = 8192u;
    t->cells = (uint64_t*)calloc(t->cell_count, sizeof(uint64_t));
    t->ring = (uint64_t*)calloc((size_t)t->ring_words, sizeof(uint64_t));
    t->sites = (Site*)calloc(TB_SITES, sizeof(Site));
    t->trees = (Tree*)calloc(g_trees, sizeof(Tree));
    t->kept = (Kept*)calloc(t->kept_room, sizeof(Kept));
    t->order = (uint64_t*)calloc(t->kept_room / 2u, sizeof(uint64_t));
    t->words = (uint64_t*)calloc(t->words_room, sizeof(uint64_t));
    t->queue = (uint64_t*)calloc(g_nodes, sizeof(uint64_t));
    t->queue_depth = (uint16_t*)calloc(g_nodes, sizeof(uint16_t));
    if (!t->cells || !t->ring || !t->sites || !t->trees || !t->kept || !t->order || !t->words || !t->queue ||
        !t->queue_depth) {
        gbrt_trace_back_destroy(ctx);
        return false;
    }
    for (unsigned i = 0; i < g_range_count; ++i) {
        for (unsigned address = g_ranges[i].first; address <= g_ranges[i].last; ++address) {
            const uint16_t plain = plain_address((uint16_t)address);
            t->watched[plain >> 3] |= (uint8_t)(1u << (plain & 7u));
        }
    }
    gbrt_trace_back_reset(ctx);
    t->state_loads = 0;
    return true;
}

static const char* const kRoleNames[3] = {"value", "address", "decision"};

static void write_source(FILE* out, const GBContext* ctx, Src src) {
    const uint64_t payload = SRC_PAYLOAD(src);
    const char* role = kRoleNames[SRC_ROLE(src) < 3u ? SRC_ROLE(src) : 0u];
    switch (SRC_KIND(src)) {
        case K_ROM: {
            const size_t offset = (size_t)payload;
            fprintf(out, "[\"rom\", \"%s\", %lu, %u]", role, (unsigned long)offset,
                    offset < ctx->rom_size ? (unsigned)ctx->rom[offset] : 0u);
            break;
        }
        case K_CONSTANT: {
            const unsigned size = (payload & CONSTANT_WIDE) ? 2u : 1u;
            if (payload & CONSTANT_IN_RAM) {
                fprintf(out, "[\"constant_in_ram\", \"%s\", %u, %u, %u]", role, (unsigned)(payload & 0xFFFFu), size,
                        (unsigned)((payload >> 16) & 0xFFFFu));
            } else {
                const size_t offset = (size_t)(payload & 0xFFFFFFFFu);
                unsigned value = offset < ctx->rom_size ? ctx->rom[offset] : 0u;
                if (size == 2u && offset + 1u < ctx->rom_size) value |= (unsigned)ctx->rom[offset + 1u] << 8;
                fprintf(out, "[\"constant\", \"%s\", %lu, %u, %u]", role, (unsigned long)offset, size, value);
            }
            break;
        }
        case K_HARDWARE:
            fprintf(out, "[\"hardware\", \"%s\", %u, %u, %u]", role, (unsigned)(payload & 0xFFFFu),
                    (unsigned)((payload >> 32) & 0xFFFFu), (unsigned)((payload >> 16) & 0xFFFFu));
            break;
        case K_EVENT:
            fprintf(out, "[\"ram\", \"%s\", %llu]", role, (unsigned long long)payload);
            break;
        case K_START:
            if (payload & START_REGISTER) {
                const unsigned which = (unsigned)(payload & 0xFFu);
                fprintf(out, "[\"start_register\", \"%s\", \"%s\"]", role,
                        which < REG_COUNT ? kRegisterNames[which] : "?");
            } else {
                fprintf(out, "[\"start\", \"%s\", %u, %lu]", role, (unsigned)(payload & 0xFFFFu),
                        (unsigned long)(payload >> 16));
            }
            break;
        case K_UNSEEN:
            fprintf(out, "[\"unseen\", \"%s\", %u, %lu]", role, (unsigned)(payload & 0xFFFFu),
                    (unsigned long)(payload >> 16));
            break;
        default:
            fprintf(out, "[\"unknown\", \"%s\"]", role);
            break;
    }
}

bool gbrt_trace_back_write(GBContext* ctx) {
    TraceBack* t = ctx ? (TraceBack*)ctx->trace_back : NULL;
    if (!g_path) return true;
    if (!t) return false;
    FILE* out = fopen(g_path, "w");
    if (!out) return false;
    const unsigned checksum = ctx->rom_size > 0x14Fu ? (unsigned)((ctx->rom[0x14E] << 8) | ctx->rom[0x14F]) : 0u;
    fprintf(out, "{\n \"schema\": \"gbrt.trace-back\",\n \"version\": 1,\n");
    fprintf(out, " \"rom\": {\"size\": %lu, \"global_checksum\": %u},\n", (unsigned long)ctx->rom_size, checksum);
    fprintf(out, " \"instructions\": %llu,\n \"events\": %llu,\n", (unsigned long long)t->instructions,
            (unsigned long long)t->events);
    const uint64_t remembered = t->head < t->ring_words ? t->head : t->ring_words;
    fprintf(out, " \"memory\": {\"kilobytes\": %lu, \"words\": %llu, \"words_written\": %llu, "
                 "\"forgetting\": %s},\n",
            g_kilobytes, (unsigned long long)t->ring_words, (unsigned long long)t->head,
            remembered < t->head ? "true" : "false");
    fprintf(out, " \"limits\": {\"sources\": %u, \"calls\": %u, \"depth\": %lu, \"nodes\": %lu, \"trees\": %lu, "
                 "\"each\": %lu, \"sites\": %u, \"site_calls\": %u},\n",
            TB_SOURCES, TB_CALLS, g_depth, g_nodes, g_trees, g_each, TB_SITES, TB_SITE_CALLS);
    fprintf(out, " \"state_loads\": %u,\n \"calls_too_deep\": %llu,\n \"out_of_memory\": %s,\n", t->state_loads,
            (unsigned long long)t->too_deep, t->failed ? "true" : "false");
    fprintf(out, " \"watched\": [");
    for (unsigned i = 0; i < g_range_count; ++i) {
        fprintf(out, "%s{\"first\": %u, \"last\": %u}", i ? ", " : "", (unsigned)g_ranges[i].first,
                (unsigned)g_ranges[i].last);
    }
    fprintf(out, "],\n \"watched_writes\": %llu,\n \"writes_without_site\": %llu,\n \"writes_without_tree\": %llu,\n",
            (unsigned long long)t->watched_writes, (unsigned long long)t->sites_dropped,
            (unsigned long long)t->trees_skipped);
    fprintf(out, " \"writes\": [");
    for (uint32_t i = 0; i < t->site_count; ++i) {
        const Site* site = &t->sites[i];
        fprintf(out, "%s\n  {\"address\": %u, \"cell\": %lu, \"at\": [%u, %u], \"count\": %llu, "
                     "\"first_frame\": %llu, \"last_frame\": %llu, \"first_value\": %u, \"last_value\": %u,\n"
                     "   \"calls_in_progress\": %u, \"calls\": [",
                i ? "," : "", (unsigned)site->address, (unsigned long)site->cell, (unsigned)site->bank,
                (unsigned)site->pc, (unsigned long long)site->count, (unsigned long long)site->first_frame,
                (unsigned long long)site->last_frame, (unsigned)site->first_value, (unsigned)site->last_value,
                (unsigned)site->calls_in_all);
        for (unsigned c = 0; c < site->call_count; ++c) {
            const Call* call = &site->calls[c];
            fprintf(out, "%s\n    {\"entered\": [%u, %u], \"from\": [%u, %u], \"returns_to\": %u, \"interrupt\": %s}",
                    c ? "," : "", (unsigned)call->entry_bank, (unsigned)call->entry, (unsigned)call->from_bank,
                    (unsigned)call->from, (unsigned)call->returns_to, call->interrupt ? "true" : "false");
        }
        fprintf(out, "],\n   \"trees\": [");
        bool first = true;
        for (uint32_t k = 0; k < t->tree_count; ++k) {
            const Tree* tree = &t->trees[k];
            if (tree->site != i) continue;
            fprintf(out, "%s\n    {\"event\": %llu, \"frame\": %llu, \"value\": %u, \"nodes\": %u, "
                         "\"cut_by_depth\": %u, \"cut_by_nodes\": %u, \"forgotten\": %u}",
                    first ? "" : ",", (unsigned long long)tree->root, (unsigned long long)tree->frame,
                    (unsigned)tree->value, tree->nodes, tree->cut_depth, tree->cut_nodes, tree->forgotten);
            first = false;
        }
        fprintf(out, "]}");
    }
    fprintf(out, "\n ],\n \"events_kept\": [");
    for (uint32_t i = 0; i < t->kept_count; ++i) {
        const uint64_t id = t->order[i];
        const Kept* node = kept_find(t, id);
        if (!node || node->at == FORGOTTEN) {
            fprintf(out, "%s\n  {\"id\": %llu, \"forgotten\": true}", i ? "," : "", (unsigned long long)id);
            continue;
        }
        const uint64_t* w = &t->words[node->at];
        fprintf(out, "%s\n  {\"id\": %llu, \"what\": \"%s\", \"at\": [%u, %u], \"address\": %u, \"cell\": %lu, "
                     "\"value\": %u, \"frame\": %lu, \"sources\": [",
                i ? "," : "", (unsigned long long)id,
                EVENT_WHAT(w) < WHAT_KINDS ? kWhatNames[EVENT_WHAT(w)] : "?", EVENT_BANK(w), EVENT_PC(w),
                EVENT_ADDRESS(w), EVENT_CELL(w), EVENT_VALUE(w), EVENT_FRAME(w));
        for (unsigned s = 0; s < EVENT_SOURCES(w); ++s) {
            if (s) fprintf(out, ", ");
            write_source(out, ctx, w[2u + s]);
        }
        fprintf(out, "]");
        if (EVENT_MORE(w) & MORE_VALUES) fprintf(out, ", \"more\": true");
        if (EVENT_MORE(w) & MORE_DECISIONS) fprintf(out, ", \"more_decisions\": true");
        fprintf(out, "}");
    }
    fprintf(out, "\n ]\n}\n");
    const bool ok = ferror(out) == 0;
    const bool closed = fclose(out) == 0;
    fprintf(stderr, "[GBRT] trace back: %llu instructions, %llu events, %llu watched writes, %u trees in %s\n",
            (unsigned long long)t->instructions, (unsigned long long)t->events,
            (unsigned long long)t->watched_writes, t->tree_count, g_path);
    return ok && closed;
}
