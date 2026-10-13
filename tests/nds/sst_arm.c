/*
 * Per-instruction vector harness for both DS processors.
 *
 *   sst_nds_arm --cpu 7 <SingleStepTests ARM7TDMI v1 directory> [options]
 *   sst_nds_arm --cpu 9 <generated ARMv5TE vector directory> [options]
 *   options: [--filter SUBSTR] [--limit N] [--report N]
 *
 * Both directories hold .json.bin files in the SingleStepTests ARM7TDMI
 * format (documented in the upstream transcode_json.py). The harness sets
 * up the CPU from each test's initial state, runs one step, and compares
 * every register, the banked registers, CPSR and the SPSRs with the
 * recorded final state. The bus replays the recorded transactions in order
 * and fails the test on any mismatch in kind, size, address or written
 * data.
 *
 * The ARM7 runs the upstream ARM7TDMI vectors, exactly as the Game Boy
 * Advance harness does: instruction fetches and the final pipeline are
 * checked too.
 *
 * The ARM9 runs the vectors tools/nds/gen_armv5_vectors.py generates (file
 * magic D33DBAE5), listed in the directory's index.txt. Those record data
 * accesses only, so fetches are answered with filler and the pipeline is
 * not compared. The fetch logic is the same template code the ARM7 run
 * checks.
 *
 * Exit status is 0 only if every selected test passes and at least one test
 * ran.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nds_bus.h"
#include "nds_cpu.h"

#define SST_MAGIC 0xD33DBAE0u     /* upstream: fetches recorded */
#define SST_MAGIC_DATA 0xD33DBAE5u /* generated: data accesses only */
#define SST_STATE_WORDS 40

enum {
    ST_R = 0,
    ST_FIQ = 16,
    ST_SVC = 23,
    ST_ABT = 25,
    ST_IRQ = 27,
    ST_UND = 29,
    ST_CPSR = 31,
    ST_SPSR = 32,
    ST_PIPE = 37,
};

typedef struct {
    uint32_t kind, size, addr, data, cycle, access;
} SstTransaction;

typedef struct {
    uint32_t initial[SST_STATE_WORDS];
    uint32_t final[SST_STATE_WORDS];
    uint32_t transaction_count;
    const uint8_t *transactions; /* raw little-endian words */
    uint32_t opcode;
    uint32_t base_addr;
} SstTest;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static SstTransaction transaction_at(const SstTest *t, uint32_t index)
{
    const uint8_t *p = t->transactions + index * 24u;
    SstTransaction tx = { rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12), rd32(p + 16), rd32(p + 20) };
    return tx;
}

/* Parse one test starting at p; returns its total size or 0 on error. */
static uint32_t parse_test(const uint8_t *p, const uint8_t *end, SstTest *t)
{
    if (end - p < 4) return 0;
    uint32_t full = rd32(p);
    if (full < 4 || (size_t)(end - p) < full) return 0;
    const uint8_t *q = p + 4;

    for (int which = 0; which < 2; which++) {
        uint32_t size = rd32(q);
        uint32_t *dst = which ? t->final : t->initial;
        for (int i = 0; i < SST_STATE_WORDS; i++) dst[i] = rd32(q + 8 + 4 * i);
        q += size;
    }

    uint32_t tsize = rd32(q);
    t->transaction_count = rd32(q + 8);
    t->transactions = q + 12;
    q += tsize;

    t->opcode = rd32(q + 8);
    t->base_addr = rd32(q + 12);
    return full;
}

/* ------------------------------------------------------------------ */
/* Replaying bus                                                       */
/* ------------------------------------------------------------------ */

static const SstTest *g_test;
static uint32_t g_next;
static bool g_data_only;
static char g_error[512];

static void bus_fail(const char *fmt, ...)
{
    if (g_error[0]) return; /* keep the first error */
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, args);
    va_end(args);
}

static bool next_transaction(SstTransaction *out)
{
    if (g_next >= g_test->transaction_count) return false;
    *out = transaction_at(g_test, g_next++);
    return true;
}

static uint32_t bus_read(uint32_t addr, uint32_t size, int access)
{
    uint32_t kind = (access & NDS_ACCESS_CODE) ? 0u : 1u;
    /* Generated vectors do not record fetches. Nothing fetched during the
     * step executes within it, so any value will do. */
    if (g_data_only && kind == 0) return 0;

    SstTransaction tx;
    if (!next_transaction(&tx)) {
        bus_fail("unexpected %s%u at %08" PRIX32, kind ? "read" : "fetch", size * 8, addr);
        return 0;
    }
    if (tx.kind != kind || tx.size != size || tx.addr != addr) {
        bus_fail("%s%u at %08" PRIX32 ", expected kind %" PRIu32 " size %" PRIu32 " at %08" PRIX32,
                 kind ? "read" : "fetch", size * 8, addr, tx.kind, tx.size, tx.addr);
    }
    return tx.data;
}

static void bus_write(uint32_t addr, uint32_t size, uint32_t value)
{
    SstTransaction tx;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1u;
    if (!next_transaction(&tx)) {
        bus_fail("unexpected write%u %08" PRIX32 " at %08" PRIX32, size * 8, value, addr);
        return;
    }
    if (tx.kind != 2 || tx.size != size || tx.addr != addr ||
        (tx.data & mask) != (value & mask)) {
        bus_fail("write%u %08" PRIX32 " at %08" PRIX32 ", expected kind %" PRIu32 " size %" PRIu32
                 " %08" PRIX32 " at %08" PRIX32,
                 size * 8, value, addr, tx.kind, tx.size, tx.data, tx.addr);
    }
}

#define DEFINE_BUS(prefix)                                                              \
    uint32_t prefix##_bus_read8(NdsCpu *cpu, uint32_t addr, int access)                 \
    {                                                                                   \
        (void)cpu;                                                                      \
        return bus_read(addr, 1, access) & 0xFFu;                                       \
    }                                                                                   \
    uint32_t prefix##_bus_read16(NdsCpu *cpu, uint32_t addr, int access)                \
    {                                                                                   \
        (void)cpu;                                                                      \
        return bus_read(addr, 2, access) & 0xFFFFu;                                     \
    }                                                                                   \
    uint32_t prefix##_bus_read32(NdsCpu *cpu, uint32_t addr, int access)                \
    {                                                                                   \
        (void)cpu;                                                                      \
        return bus_read(addr, 4, access);                                               \
    }                                                                                   \
    void prefix##_bus_write8(NdsCpu *cpu, uint32_t addr, uint8_t value, int access)     \
    {                                                                                   \
        (void)cpu; (void)access;                                                        \
        bus_write(addr, 1, value);                                                      \
    }                                                                                   \
    void prefix##_bus_write16(NdsCpu *cpu, uint32_t addr, uint16_t value, int access)   \
    {                                                                                   \
        (void)cpu; (void)access;                                                        \
        bus_write(addr, 2, value);                                                      \
    }                                                                                   \
    void prefix##_bus_write32(NdsCpu *cpu, uint32_t addr, uint32_t value, int access)   \
    {                                                                                   \
        (void)cpu; (void)access;                                                        \
        bus_write(addr, 4, value);                                                      \
    }                                                                                   \
    void prefix##_bus_idle(NdsCpu *cpu, int cycles)                                     \
    {                                                                                   \
        cpu->cycles += (uint64_t)cycles;                                                \
    }

DEFINE_BUS(nds9)
DEFINE_BUS(nds7)

void nds9_bus_cp15_changed(NdsCpu *cpu)
{
    (void)cpu;
}

void nds9_bus_wait_for_interrupt(NdsCpu *cpu)
{
    (void)cpu;
}

/* ------------------------------------------------------------------ */
/* State conversion                                                    */
/* ------------------------------------------------------------------ */

/* The vectors store registers canonically: R holds the user-mode view and
 * every banked slot is live. NdsCpu keeps the current mode's registers in
 * r[] instead, so conversion goes through the current bank. */
static void load_state(NdsCpu *cpu, const uint32_t *s)
{
    memcpy(cpu->r, &s[ST_R], sizeof cpu->r);
    memcpy(cpu->bank_r8_r12[0], &s[ST_R + 8], 5 * sizeof(uint32_t));
    memcpy(cpu->bank_r8_r12[1], &s[ST_FIQ], 5 * sizeof(uint32_t));
    cpu->bank_r13[NDS_BANK_NONE] = s[ST_R + 13];
    cpu->bank_r14[NDS_BANK_NONE] = s[ST_R + 14];
    cpu->bank_r13[NDS_BANK_FIQ] = s[ST_FIQ + 5];
    cpu->bank_r14[NDS_BANK_FIQ] = s[ST_FIQ + 6];
    cpu->bank_r13[NDS_BANK_SVC] = s[ST_SVC];
    cpu->bank_r14[NDS_BANK_SVC] = s[ST_SVC + 1];
    cpu->bank_r13[NDS_BANK_ABT] = s[ST_ABT];
    cpu->bank_r14[NDS_BANK_ABT] = s[ST_ABT + 1];
    cpu->bank_r13[NDS_BANK_IRQ] = s[ST_IRQ];
    cpu->bank_r14[NDS_BANK_IRQ] = s[ST_IRQ + 1];
    cpu->bank_r13[NDS_BANK_UND] = s[ST_UND];
    cpu->bank_r14[NDS_BANK_UND] = s[ST_UND + 1];
    uint32_t cpsr = s[ST_CPSR];
    int bank = nds_cpu_bank_for_mode(cpsr);
    if (bank == NDS_BANK_FIQ) memcpy(&cpu->r[8], cpu->bank_r8_r12[1], 5 * sizeof(uint32_t));
    cpu->r[13] = cpu->bank_r13[bank];
    cpu->r[14] = cpu->bank_r14[bank];
    cpu->cpsr = cpsr & ~NDS_CPSR_FLAGS_MASK;
    cpu->flag_n = (uint8_t)((cpsr >> 31) & 1u);
    cpu->flag_z = (uint8_t)((cpsr >> 30) & 1u);
    cpu->flag_c = (uint8_t)((cpsr >> 29) & 1u);
    cpu->flag_v = (uint8_t)((cpsr >> 28) & 1u);
    cpu->spsr[NDS_BANK_NONE] = 0;
    for (int i = 0; i < 5; i++) cpu->spsr[NDS_BANK_FIQ + i] = s[ST_SPSR + i];
    cpu->pipe[0] = s[ST_PIPE];
    cpu->pipe[1] = s[ST_PIPE + 1];
}

static void save_state(const NdsCpu *cpu_in, uint32_t *s)
{
    /* Flush the visible registers into their bank slots first. */
    NdsCpu flushed = *cpu_in;
    NdsCpu *cpu = &flushed;
    int bank = nds_cpu_bank_for_mode(cpu->cpsr);
    if (bank == NDS_BANK_FIQ) memcpy(cpu->bank_r8_r12[1], &cpu->r[8], 5 * sizeof(uint32_t));
    else memcpy(cpu->bank_r8_r12[0], &cpu->r[8], 5 * sizeof(uint32_t));
    cpu->bank_r13[bank] = cpu->r[13];
    cpu->bank_r14[bank] = cpu->r[14];

    memset(s, 0, SST_STATE_WORDS * sizeof(uint32_t));
    memcpy(&s[ST_R], cpu->r, sizeof cpu->r);
    memcpy(&s[ST_R + 8], cpu->bank_r8_r12[0], 5 * sizeof(uint32_t));
    s[ST_R + 13] = cpu->bank_r13[NDS_BANK_NONE];
    s[ST_R + 14] = cpu->bank_r14[NDS_BANK_NONE];
    memcpy(&s[ST_FIQ], cpu->bank_r8_r12[1], 5 * sizeof(uint32_t));
    s[ST_FIQ + 5] = cpu->bank_r13[NDS_BANK_FIQ];
    s[ST_FIQ + 6] = cpu->bank_r14[NDS_BANK_FIQ];
    s[ST_SVC] = cpu->bank_r13[NDS_BANK_SVC];
    s[ST_SVC + 1] = cpu->bank_r14[NDS_BANK_SVC];
    s[ST_ABT] = cpu->bank_r13[NDS_BANK_ABT];
    s[ST_ABT + 1] = cpu->bank_r14[NDS_BANK_ABT];
    s[ST_IRQ] = cpu->bank_r13[NDS_BANK_IRQ];
    s[ST_IRQ + 1] = cpu->bank_r14[NDS_BANK_IRQ];
    s[ST_UND] = cpu->bank_r13[NDS_BANK_UND];
    s[ST_UND + 1] = cpu->bank_r14[NDS_BANK_UND];
    s[ST_CPSR] = nds_cpu_get_cpsr(cpu);
    for (int i = 0; i < 5; i++) s[ST_SPSR + i] = cpu->spsr[NDS_BANK_FIQ + i];
    s[ST_PIPE] = cpu->pipe[0];
    s[ST_PIPE + 1] = cpu->pipe[1];
}

static const char *field_name(int i, char *buf, size_t n)
{
    if (i < 16) snprintf(buf, n, "R%d", i);
    else if (i < 23) snprintf(buf, n, "R%d_fiq", 8 + (i - 16));
    else if (i < 31) {
        static const char *modes[] = { "svc", "abt", "irq", "und" };
        snprintf(buf, n, "R%d_%s", 13 + ((i - 23) & 1), modes[(i - 23) / 2]);
    } else if (i == 31) snprintf(buf, n, "CPSR");
    else if (i >= ST_PIPE) snprintf(buf, n, "pipe[%d]", i - ST_PIPE);
    else {
        static const char *modes[] = { "fiq", "svc", "abt", "irq", "und" };
        snprintf(buf, n, "SPSR_%s", modes[i - 32]);
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

/* The upstream ARM7TDMI files. */
static const char *const k_arm7_files[] = {
    "arm_b_bl", "arm_bx", "arm_cdp", "arm_data_proc_immediate",
    "arm_data_proc_immediate_shift", "arm_data_proc_register_shift",
    "arm_ldm_stm", "arm_ldr_str_immediate_offset",
    "arm_ldr_str_register_offset", "arm_ldrh_strh", "arm_ldrsb_ldrsh",
    "arm_mcr_mrc", "arm_mrs", "arm_msr_imm", "arm_msr_reg", "arm_mul_mla",
    "arm_mull_mlal", "arm_stc_ldc", "arm_swi", "arm_swp",
    "thumb_add_cmp_mov_hi", "thumb_add_sp_or_pc", "thumb_add_sub",
    "thumb_add_sub_sp", "thumb_b", "thumb_bcc", "thumb_bl_blx_prefix",
    "thumb_bl_suffix", "thumb_bx", "thumb_data_proc", "thumb_ldm_stm",
    "thumb_ldr_pc_rel", "thumb_ldr_str_imm_offset",
    "thumb_ldr_str_reg_offset", "thumb_ldr_str_sp_rel",
    "thumb_ldrb_strb_imm_offset", "thumb_ldrh_strh_imm_offset",
    "thumb_ldrh_strh_reg_offset", "thumb_ldrsb_strb_reg_offset",
    "thumb_ldrsh_ldrsb_reg_offset", "thumb_lsl_lsr_asr",
    "thumb_mov_cmp_add_sub", "thumb_push_pop", "thumb_swi",
    "thumb_undefined_bcc",
};

#define MAX_FILES 256
static char g_index_names[MAX_FILES][64];

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = len > 0 ? malloc((size_t)len) : NULL;
    if (data && fread(data, 1, (size_t)len, f) != (size_t)len) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = data ? (size_t)len : 0;
    return data;
}

/* Reads index.txt (one file stem per line) into g_index_names. */
static int read_index(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/index.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int count = 0;
    char line[128];
    while (count < MAX_FILES && fgets(line, sizeof line, f)) {
        size_t len = strcspn(line, "\r\n");
        line[len] = '\0';
        if (len == 0 || len >= sizeof g_index_names[0]) continue;
        memcpy(g_index_names[count++], line, len + 1);
    }
    fclose(f);
    return count;
}

/* Returns true if the test passed; prints a report when asked. */
static bool run_test(int which_cpu, const SstTest *t, bool report, const char *file, uint32_t index)
{
    NdsCpu cpu;
    memset(&cpu, 0, sizeof cpu);
    if (which_cpu == 9) nds9_cpu_reset(&cpu);
    else nds7_cpu_reset(&cpu);
    load_state(&cpu, t->initial);
    g_test = t;
    g_next = 0;
    g_error[0] = '\0';

    if (which_cpu == 9) nds9_cpu_step(&cpu);
    else nds7_cpu_step(&cpu);

    SstTransaction leftover;
    if (!g_error[0] && next_transaction(&leftover)) {
        static const char *kinds[] = { "fetch", "read", "write" };
        bus_fail("missing %s%u at %08" PRIX32, kinds[leftover.kind < 3 ? leftover.kind : 1],
                 leftover.size * 8, leftover.addr);
    }

    uint32_t got[SST_STATE_WORDS];
    save_state(&cpu, got);
    int compared = g_data_only ? ST_PIPE : ST_PIPE + 2;
    bool ok = !g_error[0];
    for (int i = 0; i < compared; i++) {
        if (got[i] != t->final[i]) ok = false;
    }

    if (!ok && report) {
        bool thumb = (t->initial[ST_CPSR] & NDS_CPSR_T) != 0;
        printf("  FAIL %s #%" PRIu32 ": %s %0*" PRIX32 " at %08" PRIX32 ", CPSR %08" PRIX32 "\n",
               file, index, thumb ? "thumb" : "arm", thumb ? 4 : 8, t->opcode, t->base_addr,
               t->initial[ST_CPSR]);
        if (g_error[0]) printf("    bus: %s\n", g_error);
        for (int i = 0; i < compared; i++) {
            if (got[i] != t->final[i]) {
                char name[32];
                printf("    %-9s initial %08" PRIX32 " expected %08" PRIX32 " got %08" PRIX32 "\n",
                       field_name(i, name, sizeof name), t->initial[i], t->final[i], got[i]);
            }
        }
    }
    return ok;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    const char *filter = NULL;
    int which_cpu = 0;
    uint32_t limit = UINT32_MAX;
    uint32_t report_max = 3;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cpu") && i + 1 < argc) which_cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report_max = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (argv[i][0] != '-' && !dir) dir = argv[i];
        else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!dir || (which_cpu != 7 && which_cpu != 9)) {
        fprintf(stderr, "usage: %s --cpu 7|9 <vector directory> [--filter S] [--limit N] [--report N]\n", argv[0]);
        return 2;
    }

    const char *const *names = k_arm7_files;
    size_t name_count = sizeof k_arm7_files / sizeof k_arm7_files[0];
    const char *index_names[MAX_FILES];
    uint32_t want_magic = SST_MAGIC;
    if (which_cpu == 9) {
        int count = read_index(dir);
        if (count == 0) {
            printf("no index.txt in %s: run tools/nds/gen_armv5_vectors.py (fail closed)\n", dir);
            return 1;
        }
        for (int i = 0; i < count; i++) index_names[i] = g_index_names[i];
        names = index_names;
        name_count = (size_t)count;
        want_magic = SST_MAGIC_DATA;
    }
    g_data_only = want_magic == SST_MAGIC_DATA;

    uint64_t total = 0, total_failed = 0;
    int files_run = 0, files_failed = 0;

    for (size_t f = 0; f < name_count; f++) {
        if (filter && !strstr(names[f], filter)) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.json.bin", dir, names[f]);
        size_t size = 0;
        uint8_t *data = read_file(path, &size);
        if (!data || size < 8 || rd32(data) != want_magic) {
            printf("%-34s MISSING or invalid (%s)\n", names[f], path);
            free(data);
            files_failed++;
            continue;
        }

        uint32_t count = rd32(data + 4);
        const uint8_t *p = data + 8;
        const uint8_t *end = data + size;
        uint32_t ran = 0, failed = 0;
        for (uint32_t i = 0; i < count && ran < limit; i++) {
            SstTest t;
            uint32_t consumed = parse_test(p, end, &t);
            if (!consumed) {
                printf("%-34s truncated at test %" PRIu32 "\n", names[f], i);
                failed++;
                break;
            }
            p += consumed;
            ran++;
            if (!run_test(which_cpu, &t, failed < report_max, names[f], i)) failed++;
        }
        free(data);

        files_run++;
        total += ran;
        total_failed += failed;
        if (failed) files_failed++;
        printf("%-34s %6" PRIu32 "/%-6" PRIu32 " %s\n", names[f], ran - failed, ran,
               failed ? "FAIL" : "ok");
        fflush(stdout);
    }

    printf("\nARM%d: %d files, %" PRIu64 " tests, %" PRIu64 " failed\n", which_cpu, files_run, total,
           total_failed);
    if (files_run == 0 || total == 0) {
        printf("no tests ran (fail closed)\n");
        return 1;
    }
    return (total_failed || files_failed) ? 1 : 0;
}
