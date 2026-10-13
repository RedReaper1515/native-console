/*
 * Save states: the whole NesMachine (CPU, RAM, battery RAM, cartridge
 * registers, PPU, APU and the device clocks) as one block behind a header
 * that names the ROM (SHA-256) and the layout (sizeof NesMachine). A state
 * from another game or another runtime version is refused, never
 * half-applied.
 *
 * Host pointers (the ROM image, the installed program and its lookup table,
 * diagnostics hooks, the devices' owner pointers) are not part of a state:
 * loading keeps the live machine's.
 */
#include "nes_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes_sha256.h"

#define STATE_MAGIC "NCNESST1"

typedef struct {
    char magic[8];
    uint32_t machine_size;
    uint32_t rom_size;
    char rom_sha256[64];
} StateHeader;

static void make_header(const NesMachine *m, StateHeader *h)
{
    char digest[65];
    memset(h, 0, sizeof *h);
    memcpy(h->magic, STATE_MAGIC, 8);
    h->machine_size = (uint32_t)sizeof(NesMachine);
    h->rom_size = m->file_size;
    nes_sha256_hex(m->file, m->file_size, digest);
    memcpy(h->rom_sha256, digest, 64);
}

/* The pointer and diagnostics fields a state must not carry. A new pointer
 * field in NesMachine, NesPpu or NesApu belongs in this list. */
typedef struct {
    const uint8_t *file, *prg, *chr;
    const char *board;
    const struct NesRcProgram *rc_program;
    uint32_t *rc_hash;
    uint32_t rc_hash_mask;
    bool in_native;
    struct NesCoverage *coverage;
    void (*trace)(struct NesMachine *);
    void *trace_user;
} HostFields;

static void take_host(NesMachine *m, HostFields *h, bool clear)
{
    h->file = m->file;
    h->prg = m->prg;
    h->chr = m->chr;
    h->board = m->info.board;
    h->rc_program = m->rc_program;
    h->rc_hash = m->rc_hash;
    h->rc_hash_mask = m->rc_hash_mask;
    h->in_native = m->in_native;
    h->coverage = m->coverage;
    h->trace = m->trace;
    h->trace_user = m->trace_user;
    if (clear) {
        m->file = m->prg = m->chr = NULL;
        m->info.board = NULL;
        m->rc_program = NULL;
        m->rc_hash = NULL;
        m->rc_hash_mask = 0;
        m->in_native = false;
        m->coverage = NULL;
        m->trace = NULL;
        m->trace_user = NULL;
        m->cpu.user = NULL;
        m->ppu.user = NULL;
        m->apu.user = NULL;
    }
}

static void put_host(NesMachine *m, const HostFields *h)
{
    m->file = h->file;
    m->prg = h->prg;
    m->chr = h->chr;
    m->info.board = h->board;
    m->rc_program = h->rc_program;
    m->rc_hash = h->rc_hash;
    m->rc_hash_mask = h->rc_hash_mask;
    m->in_native = h->in_native;
    m->coverage = h->coverage;
    m->trace = h->trace;
    m->trace_user = h->trace_user;
    m->cpu.user = m;
    m->ppu.user = m;
    m->apu.user = m;
}

bool nes_state_save_file(NesMachine *m, const char *path)
{
    StateHeader header;
    make_header(m, &header);
    NesMachine *copy = malloc(sizeof *copy);
    if (!copy) return false;
    memcpy(copy, m, sizeof *copy);
    HostFields host;
    take_host(copy, &host, true);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(&header, sizeof header, 1, f) == 1 && fwrite(copy, sizeof *copy, 1, f) == 1;
    if (f && fclose(f) != 0) ok = false;
    free(copy);
    if (!ok) remove(path);
    return ok;
}

bool nes_state_load_file(NesMachine *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    StateHeader want, got;
    make_header(m, &want);
    NesMachine *loaded = malloc(sizeof *loaded);
    bool ok = loaded && fread(&got, sizeof got, 1, f) == 1 && memcmp(&got, &want, sizeof got) == 0 &&
              fread(loaded, sizeof *loaded, 1, f) == 1;
    fclose(f);
    if (!ok) {
        free(loaded);
        return false;
    }
    HostFields host;
    take_host(m, &host, false);
    /* The cartridge's bank generation only ever grows, so recompiled code
     * that was running when the state is loaded looks its place up again. */
    const uint64_t prg_gen = m->cart.prg_gen;
    memcpy(m, loaded, sizeof *m);
    free(loaded);
    put_host(m, &host);
    m->cart.prg_gen = prg_gen + 1u;
    m->attention |= NES_ATTENTION_REMAP;
    nes_update_attention(m);
    return true;
}
