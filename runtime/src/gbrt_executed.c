/**
 * gbrt_executed.c - see gbrt_executed.h.
 */

#include "gbrt_executed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Executed {
    char* path;
    uint8_t* map; /* a byte for each byte of the ROM */
    size_t rom_size;
    uint64_t instructions;
} Executed;

static char* g_path = NULL;

void gbrt_executed_set_file(const char* path) {
    free(g_path);
    g_path = NULL;
    if (path && path[0]) {
        const size_t length = strlen(path) + 1u;
        g_path = (char*)malloc(length);
        if (g_path) memcpy(g_path, path, length);
    }
}

bool gbrt_executed_requested(void) {
    return g_path != NULL;
}

static Executed* executed_of(const GBContext* ctx) {
    return ctx ? (Executed*)ctx->executed : NULL;
}

bool gbrt_executed_start(GBContext* ctx) {
    if (!ctx || !g_path || ctx->executed) return ctx != NULL;
    Executed* e = (Executed*)calloc(1, sizeof(Executed));
    if (!e) return false;
    const size_t length = strlen(g_path) + 1u;
    e->path = (char*)malloc(length);
    e->rom_size = ctx->rom_size;
    e->map = (uint8_t*)calloc(e->rom_size ? e->rom_size : 1u, 1);
    if (!e->path || !e->map) {
        free(e->path);
        free(e->map);
        free(e);
        return false;
    }
    memcpy(e->path, g_path, length);
    ctx->executed = e;
    return true;
}

void gbrt_executed_destroy(GBContext* ctx) {
    Executed* e = executed_of(ctx);
    if (!e) return;
    free(e->path);
    free(e->map);
    free(e);
    ctx->executed = NULL;
}

/* How many bytes the instruction that begins with `opcode` has. */
static unsigned length_of(uint8_t opcode) {
    switch (opcode) {
        case 0x01: case 0x11: case 0x21: case 0x31: /* ld rr, nn */
        case 0x08:                                  /* ld [nn], sp */
        case 0xC2: case 0xC3: case 0xCA: case 0xD2: case 0xDA: /* jp */
        case 0xC4: case 0xCC: case 0xCD: case 0xD4: case 0xDC: /* call */
        case 0xEA: case 0xFA:                       /* ld [nn], a / ld a, [nn] */
            return 3;
        case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E: /* ld r, n */
        case 0x10:                                  /* stop */
        case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: /* jr */
        case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE: /* op a, n */
        case 0xE0: case 0xF0:                       /* ldh */
        case 0xE8: case 0xF8:                       /* add sp, n / ld hl, sp+n */
        case 0xCB:                                  /* the second page of instructions */
            return 2;
        default:
            return 1;
    }
}

void gbrt_executed_before(GBContext* ctx) {
    Executed* e = executed_of(ctx);
    if (!e || ctx->pc >= 0x8000u || !ctx->rom) return;
    /* Each byte where it is mapped: an instruction can run across from the
     * fixed half of the ROM into the bank beside it. */
    const size_t first = (size_t)gb_resolve_rom_bank(ctx, ctx->pc) * 0x4000u + (ctx->pc & 0x3FFFu);
    if (first >= e->rom_size) return;
    e->map[first] |= GBRT_EXECUTED_BEGAN;
    e->instructions++;
    const unsigned length = length_of(ctx->rom[first]);
    for (unsigned i = 1; i < length; ++i) {
        const uint16_t address = (uint16_t)(ctx->pc + i);
        if (address >= 0x8000u) break;
        const size_t offset = (size_t)gb_resolve_rom_bank(ctx, address) * 0x4000u + (address & 0x3FFFu);
        if (offset < e->rom_size) e->map[offset] |= GBRT_EXECUTED_WITHIN;
    }
}

bool gbrt_executed_write(GBContext* ctx) {
    Executed* e = executed_of(ctx);
    if (!e) return true;
    FILE* file = fopen(e->path, "wb");
    if (!file) {
        fprintf(stderr, "[GBRT] Executed bytes: cannot write %s\n", e->path);
        return false;
    }
    uint8_t header[20] = {0};
    memcpy(header, "GBEXECUT", 8);
    header[8] = 1;
    for (unsigned i = 0; i < 4; ++i) header[12 + i] = (uint8_t)((uint64_t)e->rom_size >> (8u * i));
    if (e->rom_size > 0x14Fu) {
        header[16] = ctx->rom[0x14F]; /* the checksum is kept high byte first; here low first */
        header[17] = ctx->rom[0x14E];
    }
    bool good = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    uint8_t count[8];
    for (unsigned i = 0; i < 8; ++i) count[i] = (uint8_t)(e->instructions >> (8u * i));
    good = good && fwrite(count, 1, sizeof(count), file) == sizeof(count);
    good = good && fwrite(e->map, 1, e->rom_size, file) == e->rom_size;
    if (fclose(file) != 0) good = false;
    if (!good) {
        fprintf(stderr, "[GBRT] Executed bytes: could not write %s\n", e->path);
        return false;
    }
    size_t began = 0, bytes = 0;
    for (size_t i = 0; i < e->rom_size; ++i) {
        began += (e->map[i] & GBRT_EXECUTED_BEGAN) != 0;
        bytes += e->map[i] != 0;
    }
    printf("[GBRT] Executed bytes: %llu instructions run from the ROM, %llu different ones in %llu of its "
           "bytes: %s\n",
           (unsigned long long)e->instructions, (unsigned long long)began, (unsigned long long)bytes, e->path);
    return true;
}
