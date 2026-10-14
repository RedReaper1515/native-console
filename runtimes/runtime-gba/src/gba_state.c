/*
 * Save states: the whole GbaMachine behind a small header.
 *
 * The machine struct holds all emulated state apart from the ROM, so a
 * state is its bytes as they are between frames (gba_run_frame returned).
 * Pointers and host settings in it (the ROM, the recompiled program with its
 * lookup and RAM-verification tables, coverage, the debug hook, the RTC
 * clock source) belong to the running process and are kept on load. A state
 * loads only into the same ROM and the same struct layout; the header
 * carries both.
 */
#include "gba.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_magic[8] = { 'N', 'C', 'G', 'B', 'A', 'S', 'T', 0 };

typedef struct GbaStateHeader {
    char magic[8];
    uint32_t version;
    uint32_t machine_size;  /* sizeof(GbaMachine) */
    uint32_t rom_size;
    uint8_t rom_header[32]; /* ROM 0xA0-0xBF: title, game code, maker, version, checksum */
} GbaStateHeader;

size_t gba_state_size(void)
{
    return sizeof(GbaStateHeader) + sizeof(GbaMachine);
}

static void make_header(const GbaMachine *m, GbaStateHeader *h)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, k_magic, sizeof k_magic);
    h->version = GBA_STATE_VERSION;
    h->machine_size = (uint32_t)sizeof(GbaMachine);
    h->rom_size = m->rom_size;
    if (m->rom && m->rom_size >= 0xC0) memcpy(h->rom_header, m->rom + 0xA0, sizeof h->rom_header);
}

void gba_state_save(const GbaMachine *m, uint8_t *out)
{
    GbaStateHeader h;
    make_header(m, &h);
    memcpy(out, &h, sizeof h);
    memcpy(out + sizeof h, m, sizeof *m);
}

/* States written before the link cable fields existed hold the machine
 * up to them. */
#define GBA_STATE_OLD_MACHINE_SIZE offsetof(GbaMachine, sio_at)

bool gba_state_load(GbaMachine *m, const uint8_t *in, size_t size)
{
    GbaStateHeader want, got;
    if (size < sizeof got) return false;
    make_header(m, &want);
    memcpy(&got, in, sizeof got);
    size_t machine_size = sizeof(GbaMachine);
    if (got.machine_size == GBA_STATE_OLD_MACHINE_SIZE) {
        machine_size = GBA_STATE_OLD_MACHINE_SIZE;
        want.machine_size = got.machine_size;
    }
    if (size != sizeof got + machine_size) return false;
    if (memcmp(&want, &got, sizeof want) != 0) return false;

    /* What the process owns, and what the frontend chose, survives. (The
     * machine is too large to copy whole onto the stack.) */
    uint8_t *rom = m->rom;
    uint32_t rom_size = m->rom_size, rom_mask = m->rom_mask;
    const struct GbaRcProgram *rc_program = m->rc_program;
    uint64_t *rc_verified = m->rc_verified;
    uint32_t *rc_index = m->rc_index;
    uint32_t rc_index_shift = m->rc_index_shift, rc_index_mask = m->rc_index_mask;
    uint64_t rc_ram_gen = m->rc_ram_gen;
    uint8_t rc_code_pages[sizeof m->rc_code_pages];
    memcpy(rc_code_pages, m->rc_code_pages, sizeof rc_code_pages);
    struct GbaCoverage *coverage = m->coverage;
    void (*debug_log)(struct GbaMachine *, int, const char *) = m->debug_log;
    void (*step_hook)(struct GbaMachine *) = m->step_hook;
    bool wall_clock = m->rtc.wall_clock;
    struct GbaLink *link = m->link;

    memcpy(m, in + sizeof got, machine_size);
    m->cpu.user = m;
    m->rom = rom;
    m->rom_size = rom_size;
    m->rom_mask = rom_mask;
    m->rc_program = rc_program;
    m->rc_verified = rc_verified;
    m->rc_index = rc_index;
    m->rc_index_shift = rc_index_shift;
    m->rc_index_mask = rc_index_mask;
    memcpy(m->rc_code_pages, rc_code_pages, sizeof rc_code_pages);
    m->coverage = coverage;
    m->debug_log = debug_log;
    m->step_hook = step_hook;
    m->rtc.wall_clock = wall_clock;
    m->in_native = false;
    m->in_events = false;
    /* The cable belongs to the process too; it reschedules its own slot. */
    m->link = link;
    m->sio_at = UINT64_MAX;
    m->attention = (uint8_t)(m->attention & ~GBA_ATTENTION_STALL);
    gba_schedule_sio(m, UINT64_MAX);
    if (link) gba_sio_state_loaded(m);
    /* RAM changed wholesale: a generation no block has seen makes every RAM
     * code block compare itself with RAM again before it runs. */
    m->rc_ram_gen = (rc_ram_gen > m->rc_ram_gen ? rc_ram_gen : m->rc_ram_gen) + 1;
    /* The battery save now matches the state; the file on disk keeps the
     * newer save until the game writes again. */
    m->save_dirty = false;
    /* Audio produced before the state was taken is not replayed. */
    m->apu.out_read = m->apu.out_write;
    return true;
}

bool gba_state_save_file(const GbaMachine *m, const char *path)
{
    size_t size = gba_state_size();
    uint8_t *buf = malloc(size);
    if (!buf) return false;
    gba_state_save(m, buf);

    /* Write beside the target, then replace it, so a failed write never
     * leaves a truncated state in the slot. */
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(buf, 1, size, f) == size;
    if (f && fclose(f) != 0) ok = false;
    free(buf);
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

bool gba_state_load_file(GbaMachine *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t size = gba_state_size(); /* the largest size that loads */
    uint8_t *buf = malloc(size + 1);
    /* Read one byte past the expected size to reject longer files. */
    size_t n = buf ? fread(buf, 1, size + 1, f) : 0;
    fclose(f);
    bool ok = buf && gba_state_load(m, buf, n);
    free(buf);
    return ok;
}
