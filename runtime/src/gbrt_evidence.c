/*
 * gbrt_evidence.c - recording the jumps a game makes that its code does not
 * spell out. gbrt_evidence.h says what for.
 */

#include "gbrt_evidence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVIDENCE_DEPTH 512u
#define EVIDENCE_RECORDS 16384u /* a power of two */

enum { FRAME_CALL = 0, FRAME_INTERRUPT = 1, FRAME_JUMP = 2 };
enum { TRANSFER_JUMP = 0, TRANSFER_RETURN = 1 };

/* One call that has not returned: where its return address is on the guest
 * stack, what it is, and who made it. */
typedef struct EvidenceFrame {
    uint16_t slot;
    uint16_t returns_to;
    uint16_t site_bank;
    uint16_t site;        /* the CALL, or the JP HL that pushed a return address first */
    uint16_t entered_bank;
    uint16_t entered;     /* where it went */
    uint8_t kind;
    /* A JP HL made with something pushed, where nothing says how much the
     * function had on the stack when it was entered: a call made by hand if
     * the target returns to what was pushed, a jump if it does not. It is
     * written down when that is known, and until then says nothing of the
     * code that runs inside it. */
    uint8_t unsure;
} EvidenceFrame;

typedef struct EvidenceRecord {
    uint8_t used;
    uint8_t kind;
    uint8_t calls;        /* a return address was on the stack for the target */
    uint8_t has_from;
    uint16_t bank;
    uint16_t at;
    uint16_t target_bank;
    uint16_t target;
    uint16_t returns_to;
    uint16_t from_bank;
    uint16_t from_site;
    uint16_t through_bank;
    uint16_t through;
    uint64_t count;
} EvidenceRecord;

typedef struct Evidence {
    char* path;
    EvidenceFrame frames[EVIDENCE_DEPTH];
    unsigned depth;
    EvidenceRecord* records;
    unsigned record_count;
    uint64_t dropped;
    uint8_t* entered;     /* a bit per ROM byte: control arrived here by a call or such a jump */
    size_t entered_bytes;
    int interpreting;
    uint16_t interpreting_at;
    /* After a state was loaded: calls were in progress that this record
     * never saw made. Their frames are on the guest stack from here up: the
     * return address of the innermost one still in progress is at this
     * address or above it. */
    int resumed;
    uint16_t resumed_sp;
} Evidence;

static char* g_evidence_path = NULL;

static void note_entered(Evidence* e, const GBContext* ctx, uint16_t bank, uint16_t address);
static void drop_frame(Evidence* e);

void gbrt_evidence_set_file(const char* path) {
    free(g_evidence_path);
    g_evidence_path = NULL;
    if (path && path[0]) {
        const size_t length = strlen(path) + 1u;
        g_evidence_path = (char*)malloc(length);
        if (g_evidence_path) memcpy(g_evidence_path, path, length);
    }
}

bool gbrt_evidence_requested(void) {
    return g_evidence_path != NULL;
}

static Evidence* evidence_of(const GBContext* ctx) {
    return ctx ? (Evidence*)ctx->evidence : NULL;
}

bool gbrt_evidence_start(GBContext* ctx) {
    if (!ctx || !g_evidence_path || ctx->evidence) return ctx != NULL;
    Evidence* e = (Evidence*)calloc(1, sizeof(Evidence));
    if (!e) return false;
    const size_t length = strlen(g_evidence_path) + 1u;
    e->path = (char*)malloc(length);
    e->records = (EvidenceRecord*)calloc(EVIDENCE_RECORDS, sizeof(EvidenceRecord));
    e->entered_bytes = (ctx->rom_size + 7u) / 8u;
    e->entered = (uint8_t*)calloc(e->entered_bytes ? e->entered_bytes : 1u, 1);
    if (!e->path || !e->records || !e->entered) {
        free(e->path);
        free(e->records);
        free(e->entered);
        free(e);
        return false;
    }
    memcpy(e->path, g_evidence_path, length);
    ctx->evidence = e;
    /* Where the machine starts counts as arrived at. */
    note_entered(e, ctx, gb_resolve_rom_bank(ctx, ctx->pc), ctx->pc);
    return true;
}

void gbrt_evidence_reset(GBContext* ctx) {
    Evidence* e = evidence_of(ctx);
    if (!e) return;
    /* What was waiting to be told a call from a jump stays a jump. */
    while (e->depth) drop_frame(e);
    e->interpreting = 0;
    e->resumed = 1;
    e->resumed_sp = ctx->sp;
}

void gbrt_evidence_destroy(GBContext* ctx) {
    Evidence* e = evidence_of(ctx);
    if (!e) return;
    free(e->path);
    free(e->records);
    free(e->entered);
    free(e);
    ctx->evidence = NULL;
}

/* The bank an address in the ROM is in as things are mapped now. */
static uint16_t bank_of(const GBContext* ctx, uint16_t address) {
    return address >= 0x8000 ? 0 : gb_resolve_rom_bank(ctx, address);
}

static void note_entered(Evidence* e, const GBContext* ctx, uint16_t bank, uint16_t address) {
    if (address >= 0x8000) return;
    const size_t offset = (size_t)bank * 0x4000u + (address & 0x3FFFu);
    if (offset < ctx->rom_size) e->entered[offset >> 3] |= (uint8_t)(1u << (offset & 7u));
}

/* A byte of the ROM as mapped now, without the bus noticing. */
static int rom_byte(const GBContext* ctx, uint16_t address) {
    if (address >= 0x8000 || !ctx->rom) return -1;
    const size_t offset = (size_t)bank_of(ctx, address) * 0x4000u + (address & 0x3FFFu);
    return offset < ctx->rom_size ? ctx->rom[offset] : -1;
}

/* The stack is RAM: reading it has no effect the pop that follows will not have. */
static uint16_t stack_word(GBContext* ctx, uint16_t address) {
    return (uint16_t)(gb_read8(ctx, address) | ((uint16_t)gb_read8(ctx, (uint16_t)(address + 1u)) << 8));
}

/* Where the instruction being run is: PC for generated code, what the
 * interpreter said for interpreted code. */
static uint16_t current_instruction(Evidence* e, const GBContext* ctx) {
    return e->interpreting ? e->interpreting_at : ctx->pc;
}

/* Whether both bytes of a word here are RAM, which reading does nothing to. */
static int stack_is_ram(uint16_t address) {
    return (address >= 0xC000 && address < 0xFDFF) || (address >= 0xFF80 && address < 0xFFFE);
}

static void record(Evidence* e, const EvidenceRecord* wanted);

/* What became of a JP HL that was held back: its target returned to what
 * was pushed for it, a call made by hand, or the frame went some other way,
 * and it is the jump it would have been written down as at once. */
static void settle(Evidence* e, const EvidenceFrame* frame, int came_back) {
    EvidenceRecord wanted;
    memset(&wanted, 0, sizeof(wanted));
    wanted.kind = TRANSFER_JUMP;
    wanted.calls = (uint8_t)(came_back != 0);
    wanted.bank = frame->site_bank;
    wanted.at = frame->site;
    wanted.target_bank = frame->entered_bank;
    wanted.target = frame->entered;
    wanted.returns_to = came_back ? frame->returns_to : 0;
    record(e, &wanted);
}

/* The innermost call is gone without having returned. */
static void drop_frame(Evidence* e) {
    const EvidenceFrame* frame = &e->frames[--e->depth];
    if (frame->unsure) settle(e, frame, 0);
}

/* The innermost call that has not returned, if it is known to be one. */
static const EvidenceFrame* known_frame(const Evidence* e) {
    return e->depth && !e->frames[e->depth - 1u].unsure ? &e->frames[e->depth - 1u] : NULL;
}

static void push_frame(Evidence* e, const EvidenceFrame* frame) {
    /* Whatever is recorded at or below this slot belongs to calls that are gone. */
    while (e->depth && e->frames[e->depth - 1u].slot <= frame->slot) drop_frame(e);
    if (e->depth < EVIDENCE_DEPTH) e->frames[e->depth++] = *frame;
}

static void record(Evidence* e, const EvidenceRecord* wanted) {
    uint32_t hash = 2166136261u;
    const uint16_t words[] = {wanted->bank, wanted->at, wanted->target_bank, wanted->target,
                              wanted->returns_to, wanted->from_bank, wanted->from_site,
                              wanted->through_bank, wanted->through,
                              (uint16_t)(wanted->kind | (wanted->calls << 4) | (wanted->has_from << 5))};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        hash = (hash ^ words[i]) * 16777619u;
    }
    for (unsigned probe = 0; probe < EVIDENCE_RECORDS; ++probe) {
        EvidenceRecord* slot = &e->records[(hash + probe) & (EVIDENCE_RECORDS - 1u)];
        if (!slot->used) {
            if (e->record_count >= EVIDENCE_RECORDS - EVIDENCE_RECORDS / 4u) break;
            *slot = *wanted;
            slot->used = 1;
            slot->count = 1;
            e->record_count++;
            return;
        }
        if (slot->kind == wanted->kind && slot->calls == wanted->calls &&
            slot->has_from == wanted->has_from && slot->bank == wanted->bank && slot->at == wanted->at &&
            slot->target_bank == wanted->target_bank && slot->target == wanted->target &&
            slot->returns_to == wanted->returns_to && slot->from_bank == wanted->from_bank &&
            slot->from_site == wanted->from_site && slot->through_bank == wanted->through_bank &&
            slot->through == wanted->through) {
            slot->count++;
            return;
        }
    }
    e->dropped++;
}

/* A transfer the code did not spell out, made from inside the innermost call
 * that has not returned. */
static void record_transfer(Evidence* e, const GBContext* ctx, uint8_t kind, uint16_t at,
                            uint16_t target, int calls, uint16_t returns_to) {
    EvidenceRecord wanted;
    memset(&wanted, 0, sizeof(wanted));
    wanted.kind = kind;
    wanted.calls = (uint8_t)(calls != 0);
    wanted.bank = bank_of(ctx, at);
    wanted.at = at;
    wanted.target_bank = bank_of(ctx, target);
    wanted.target = target;
    wanted.returns_to = returns_to;
    const EvidenceFrame* frame = known_frame(e);
    if (frame && frame->kind != FRAME_INTERRUPT) {
        wanted.has_from = 1;
        wanted.from_bank = frame->site_bank;
        wanted.from_site = frame->site;
        wanted.through_bank = frame->entered_bank;
        wanted.through = frame->entered;
    }
    record(e, &wanted);
    note_entered(e, ctx, wanted.target_bank, target);
}

void gbrt_evidence_call(GBContext* ctx, uint16_t target, uint16_t return_address, uint8_t length) {
    Evidence* e = evidence_of(ctx);
    EvidenceFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.slot = ctx->sp;
    frame.returns_to = return_address;
    frame.site = (uint16_t)(return_address - length);
    frame.site_bank = bank_of(ctx, frame.site);
    frame.entered = target;
    frame.entered_bank = bank_of(ctx, target);
    frame.kind = FRAME_CALL;
    push_frame(e, &frame);
    note_entered(e, ctx, frame.entered_bank, target);
    e->interpreting = 0;
}

void gbrt_evidence_interrupt(GBContext* ctx, uint16_t vector, uint16_t return_address) {
    Evidence* e = evidence_of(ctx);
    EvidenceFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.slot = ctx->sp;
    frame.returns_to = return_address;
    frame.entered = vector;
    frame.kind = FRAME_INTERRUPT;
    push_frame(e, &frame);
    note_entered(e, ctx, 0, vector);
    e->interpreting = 0;
}

void gbrt_evidence_return(GBContext* ctx) {
    Evidence* e = evidence_of(ctx);
    const uint16_t at = current_instruction(e, ctx);
    const uint16_t slot = ctx->sp;
    const uint16_t target = stack_word(ctx, slot);
    e->interpreting = 0;

    while (e->depth && e->frames[e->depth - 1u].slot < slot) drop_frame(e);
    if (e->depth && e->frames[e->depth - 1u].slot == slot) {
        const EvidenceFrame* frame = &e->frames[--e->depth];
        const int back = frame->returns_to == target;
        /* A JP HL that was held back, and this is its target returning to
         * what was pushed for it: a call made by hand after all. */
        if (frame->unsure) settle(e, frame, back);
        if (back) return; /* back where the call came from */
        /* The callee changed its own return address: it returns somewhere
         * else in its caller, past arguments that follow the CALL perhaps. */
    }
    /* With no call on record and the stack no lower than a loaded state
     * left it, this is a call from before the state coming back: a return
     * like any other, whose CALL the record did not see. The next such
     * frame is above it. */
    if (!e->depth && e->resumed && slot >= e->resumed_sp) {
        e->resumed_sp = (uint16_t)(slot + 2u);
        return;
    }
    /* A RET to an address no CALL pushed: a jump. If something is on the
     * stack below what this function was entered with, the target has that
     * to return to. */
    const uint16_t after = (uint16_t)(slot + 2u);
    const EvidenceFrame* inside = known_frame(e);
    const int calls = inside ? after < inside->slot : 0;
    const uint16_t returns_to = calls ? stack_word(ctx, after) : (inside ? inside->returns_to : 0);
    record_transfer(e, ctx, TRANSFER_RETURN, at, target, calls, returns_to);
    if (calls) {
        EvidenceFrame frame;
        memset(&frame, 0, sizeof(frame));
        frame.slot = after;
        frame.returns_to = returns_to;
        frame.site = at;
        frame.site_bank = bank_of(ctx, at);
        frame.entered = target;
        frame.entered_bank = bank_of(ctx, target);
        frame.kind = FRAME_JUMP;
        push_frame(e, &frame);
    }
}

void gbrt_evidence_jump(GBContext* ctx, uint16_t target) {
    Evidence* e = evidence_of(ctx);
    const uint16_t at = current_instruction(e, ctx);
    e->interpreting = 0;
    /* Only a JP HL: a jump the code spells out can happen to go where HL
     * points. Code in RAM cannot be looked at this way and is taken on trust. */
    const int opcode = rom_byte(ctx, at);
    if (opcode >= 0 && opcode != 0xE9) return;

    const uint16_t slot = ctx->sp;
    while (e->depth && e->frames[e->depth - 1u].slot < slot) drop_frame(e);
    /* With the stack as the function was entered, the target returns for
     * it: a jump. With more on it, the code pushed somewhere for the target
     * to come back to: a call made by hand.
     *
     * With no call on record after a state was loaded, the calls from
     * before the state are what is in progress, and the innermost has its
     * return address at resumed_sp or above: below that, something was
     * pushed since. But this may as well be the code at the bottom of the
     * stack, which is in no call, pushing a register for its target to pop,
     * and so may a jump made inside one that is not known for a call yet. */
    const EvidenceFrame* inside = known_frame(e);
    const int pushed = e->depth ? slot < e->frames[e->depth - 1u].slot
                                : e->resumed && slot < e->resumed_sp;
    EvidenceFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.slot = slot;
    frame.site = at;
    frame.site_bank = bank_of(ctx, at);
    frame.entered = target;
    frame.entered_bank = bank_of(ctx, target);
    frame.kind = FRAME_JUMP;
    if (pushed && !inside && stack_is_ram(slot) && e->depth < EVIDENCE_DEPTH) {
        /* So it is not written down yet: the target returning to what was
         * pushed, at this slot, is what tells the call (gbrt_evidence_return).
         * Where it went is arrived at either way. */
        frame.returns_to = stack_word(ctx, slot);
        frame.unsure = 1;
        e->frames[e->depth++] = frame;
        note_entered(e, ctx, frame.entered_bank, target);
        return;
    }
    const int calls = inside && pushed;
    const uint16_t returns_to = calls ? stack_word(ctx, slot) : (inside ? inside->returns_to : 0);
    record_transfer(e, ctx, TRANSFER_JUMP, at, target, calls, returns_to);
    if (calls) {
        frame.returns_to = returns_to;
        push_frame(e, &frame);
    }
}

void gbrt_evidence_interpreting(GBContext* ctx, uint16_t address) {
    Evidence* e = evidence_of(ctx);
    e->interpreting = 1;
    e->interpreting_at = address;
}

void gbrt_evidence_left_interpreter(GBContext* ctx) {
    Evidence* e = evidence_of(ctx);
    e->interpreting = 0;
}

bool gbrt_evidence_write(GBContext* ctx) {
    Evidence* e = evidence_of(ctx);
    if (!e) return true;
    /* A jump still held back, whose target has not returned: the jump. */
    unsigned kept = 0;
    for (unsigned i = 0; i < e->depth; ++i) {
        if (e->frames[i].unsure) settle(e, &e->frames[i], 0);
        else e->frames[kept++] = e->frames[i];
    }
    e->depth = kept;
    FILE* out = fopen(e->path, "w");
    if (!out) {
        fprintf(stderr, "[EVIDENCE] cannot write %s\n", e->path);
        return false;
    }
    fprintf(out, "{\n  \"schema\": \"gbrt.evidence\",\n  \"version\": 1,\n");
    fprintf(out, "  \"rom\": {\"size\": %llu, \"global_checksum\": %u},\n",
            (unsigned long long)ctx->rom_size,
            ctx->rom_size > 0x14F ? (unsigned)((ctx->rom[0x14E] << 8) | ctx->rom[0x14F]) : 0u);
    fprintf(out, "  \"frames\": %llu,\n", (unsigned long long)ctx->completed_frames);
    fprintf(out, "  \"dropped\": %llu,\n", (unsigned long long)e->dropped);
    fprintf(out, "  \"transfers\": [");
    int first = 1;
    for (unsigned i = 0; i < EVIDENCE_RECORDS; ++i) {
        const EvidenceRecord* r = &e->records[i];
        if (!r->used) continue;
        fprintf(out, "%s\n    {\"kind\": \"%s\", \"bank\": %u, \"at\": %u, \"target_bank\": %u, "
                     "\"target\": %u, \"calls\": %s, \"returns_to\": %u, ",
                first ? "" : ",", r->kind == TRANSFER_JUMP ? "jump" : "return", (unsigned)r->bank,
                (unsigned)r->at, (unsigned)r->target_bank, (unsigned)r->target,
                r->calls ? "true" : "false", (unsigned)r->returns_to);
        if (r->has_from) {
            fprintf(out, "\"from\": {\"bank\": %u, \"site\": %u, \"entered_bank\": %u, \"entered\": %u}, ",
                    (unsigned)r->from_bank, (unsigned)r->from_site, (unsigned)r->through_bank,
                    (unsigned)r->through);
        } else {
            fprintf(out, "\"from\": null, ");
        }
        fprintf(out, "\"count\": %llu}", (unsigned long long)r->count);
        first = 0;
    }
    fprintf(out, "%s],\n  \"entered\": [", first ? "" : "\n  ");
    first = 1;
    for (size_t offset = 0; offset < ctx->rom_size; ++offset) {
        if (!(e->entered[offset >> 3] & (1u << (offset & 7u)))) continue;
        const unsigned bank = (unsigned)(offset / 0x4000u);
        const unsigned address = (unsigned)(offset & 0x3FFFu) + (bank ? 0x4000u : 0u);
        fprintf(out, "%s[%u, %u]", first ? "" : ", ", bank, address);
        first = 0;
    }
    fprintf(out, "]\n}\n");
    const bool ok = ferror(out) == 0;
    fclose(out);
    fprintf(stderr, "[EVIDENCE] %u different jumps the code does not spell out, written to %s\n",
            e->record_count, e->path);
    return ok;
}
