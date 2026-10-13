/*
 * Replays tests/n64/vectors/vr4300.vectors (tools/n64/gen_vectors.py)
 * against the interpreter on a flat physical memory: registers, HI and LO,
 * the FPU registers and FCR31, the exception taken or the PC reached, the
 * memory after, and - for plain loads and stores - the ordered bus
 * transactions, instruction fetches included.
 *
 * RDRAM is given a size of zero, so every access goes through the bus
 * functions below and is recorded; the fast path is held to the same
 * results by n64_cpu_test.c.
 *
 *   n64_vectors_test FILE
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n64_cpu_impl.h"

#define MEM_SIZE 0x100000u
static uint8_t g_mem[MEM_SIZE];

typedef struct {
    char kind;
    uint32_t addr, size;
    uint64_t value;
} Txn;
static Txn g_txn[64];
static int g_ntxn;

static void record(char kind, uint32_t addr, uint32_t size, uint64_t value)
{
    if (g_ntxn < 64) g_txn[g_ntxn++] = (Txn){ kind, addr, size, value };
}

static uint64_t rd(uint32_t pa, uint32_t size)
{
    uint64_t v = 0;
    for (uint32_t k = 0; k < size; k++) v = (v << 8) | (pa + k < MEM_SIZE ? g_mem[pa + k] : 0);
    return v;
}

static void wr(uint32_t pa, uint32_t size, uint64_t v)
{
    for (uint32_t k = 0; k < size; k++) {
        if (pa + k < MEM_SIZE) g_mem[pa + k] = (uint8_t)(v >> (8u * (size - 1u - k)));
    }
}

uint32_t n64_bus_fetch(N64Cpu *c, uint32_t pa)
{
    (void)c;
    uint32_t v = (uint32_t)rd(pa, 4);
    record('F', pa, 4, v);
    return v;
}
#define READ(bits, size) \
    uint##bits##_t n64_bus_read##bits(N64Cpu *c, uint32_t pa) \
    { \
        (void)c; \
        uint64_t v = rd(pa, size); \
        record('R', pa, size, v); \
        return (uint##bits##_t)v; \
    }
#define WRITE(bits, size) \
    void n64_bus_write##bits(N64Cpu *c, uint32_t pa, uint##bits##_t v) \
    { \
        (void)c; \
        wr(pa, size, v); \
        record('W', pa, size, v); \
    }
READ(8, 1)
READ(16, 2)
READ(32, 4)
READ(64, 8)
WRITE(8, 1)
WRITE(16, 2)
WRITE(32, 4)
WRITE(64, 8)
void n64_bus_code_written(N64Cpu *c, uint32_t pa, uint32_t size) { (void)c; (void)pa; (void)size; }
void n64_bus_timer_changed(N64Cpu *c) { (void)c; }
void n64_bus_interrupt_check(N64Cpu *c) { (void)c; }

typedef struct {
    uint64_t gpr[32], hi, lo, fpr[32];
    uint32_t fcr31;
} Regs;

static void parse_regs(char *s, Regs *r)
{
    memset(r, 0, sizeof *r);
    for (char *tok = strtok(s, " \n"); tok; tok = strtok(NULL, " \n")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        uint64_t v = strtoull(eq + 1, NULL, 16);
        if (tok[0] == 'r') r->gpr[atoi(tok + 1)] = v;
        else if (tok[0] == 'f' && tok[1] != 'c') r->fpr[atoi(tok + 1)] = v;
        else if (!strcmp(tok, "hi")) r->hi = v;
        else if (!strcmp(tok, "lo")) r->lo = v;
        else if (!strcmp(tok, "fcr31")) r->fcr31 = (uint32_t)v;
    }
}

static void parse_hex_bytes(const char *hex, uint8_t *out, size_t cap, size_t *len)
{
    size_t n = 0;
    while (hex[0] && hex[1] && hex[0] != '\n' && n < cap) {
        char b[3] = { hex[0], hex[1], 0 };
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
        hex += 2;
    }
    *len = n;
}

typedef struct {
    char name[32];
    uint32_t insns[8];
    int ninsns, count, fr;
    Regs init, final;
    int has_exc, exc;
    uint64_t pc;
    uint32_t mem_addr;
    uint8_t mem[256];
    size_t mem_len;
    int has_after;
    uint32_t after_addr;
    uint8_t after[256];
    size_t after_len;
    Txn bus[64];
    int nbus;
} Case;

static int g_deviations;

static int taken_is_error(int taken) { return taken >= 0; }

static int run_case(const Case *k, char *why, size_t cap)
{
    static N64Cpu cpu;
    N64Cpu *c = &cpu;
    memset(g_mem, 0, sizeof g_mem);
    for (int n = 0; n < k->ninsns; n++) n64_put_be32(g_mem + 0x10000u + 4u * (uint32_t)n, k->insns[n]);
    if (k->mem_len) memcpy(g_mem + k->mem_addr, k->mem, k->mem_len);
    memset(c, 0, sizeof *c);
    n64_cpu_reset(c);
    c->cp0_status = N64_SR_CU1 | (k->fr ? N64_SR_FR : 0u);
    n64_cpu_update_mode(c);
    memcpy(c->gpr, k->init.gpr, sizeof c->gpr);
    c->gpr[0] = 0;
    c->hi = k->init.hi;
    c->lo = k->init.lo;
    memcpy(c->fpr, k->init.fpr, sizeof c->fpr);
    c->fcr31 = k->init.fcr31;
    c->pc = 0xFFFFFFFF80010000ull;
    g_ntxn = 0;
    int taken = -1;
    const N64Op op0 = n64_decode(k->insns[0]);
    for (int n = 0; n < k->count; n++) {
        uint64_t before = c->cp0_status;
        (void)n64_cpu_step(c);
        if (!(before & N64_SR_EXL) && (c->cp0_status & N64_SR_EXL)) {
            taken = (int)((c->cp0_cause >> 2) & 31u);
            break;
        }
        /* The oracle counts a likely branch's nullified delay slot as an
         * instruction; the interpreter skips it in the branch's own step. */
        if (n == 0 && (n64_op_flags[op0] & N64_F_LIKELY) && !c->delay_pending) break;
    }
    Regs want = k->final;
    /* Deviation (docs/N64-PLAN.md, "Known deviations"): LWR that does not
     * load the word's top byte leaves rt's upper half alone on the VR4300
     * (ares, n64-systemtest); the R4000 model sign-extends. */
    if (op0 == N64_OP_lwr) {
        uint64_t ea = k->init.gpr[N64_RS(k->insns[0])] + (uint64_t)N64_IMM(k->insns[0]);
        unsigned rt = N64_RT(k->insns[0]);
        if ((ea & 3u) != 3u && !taken_is_error(taken)) {
            uint64_t vr = (k->init.gpr[rt] & 0xFFFFFFFF00000000ull) | (want.gpr[rt] & 0xFFFFFFFFull);
            if (vr != want.gpr[rt]) g_deviations++;
            want.gpr[rt] = vr;
        }
    }
    for (int r = 1; r < 32; r++) {
        if (c->gpr[r] != want.gpr[r]) {
            snprintf(why, cap, "r%d=%016" PRIX64 " want %016" PRIX64, r, c->gpr[r], want.gpr[r]);
            return 0;
        }
    }
    if (c->hi != k->final.hi || c->lo != k->final.lo) {
        snprintf(why, cap, "hi/lo=%016" PRIX64 "/%016" PRIX64 " want %016" PRIX64 "/%016" PRIX64, c->hi, c->lo,
                 k->final.hi, k->final.lo);
        return 0;
    }
    for (int r = 0; r < 32; r++) {
        if (c->fpr[r] != k->final.fpr[r]) {
            snprintf(why, cap, "f%d=%016" PRIX64 " want %016" PRIX64, r, c->fpr[r], k->final.fpr[r]);
            return 0;
        }
    }
    if (c->fcr31 != k->final.fcr31) {
        snprintf(why, cap, "fcr31=%08X want %08X", c->fcr31, k->final.fcr31);
        return 0;
    }
    if (k->has_exc) {
        if (taken != k->exc) {
            snprintf(why, cap, "exception %d, want %d", taken, k->exc);
            return 0;
        }
    } else {
        if (taken >= 0) {
            snprintf(why, cap, "exception %d raised, want none", taken);
            return 0;
        }
        if (c->pc != k->pc) {
            snprintf(why, cap, "pc=%016" PRIX64 " want %016" PRIX64, c->pc, k->pc);
            return 0;
        }
    }
    if (k->has_after && memcmp(g_mem + k->after_addr, k->after, k->after_len) != 0) {
        snprintf(why, cap, "memory after differs");
        return 0;
    }
    if (k->nbus) {
        if (g_ntxn != k->nbus) {
            snprintf(why, cap, "%d bus transactions, want %d", g_ntxn, k->nbus);
            return 0;
        }
        for (int n = 0; n < g_ntxn; n++) {
            const Txn *a = &g_txn[n], *b = &k->bus[n];
            if (a->kind != b->kind || a->addr != b->addr || a->size != b->size || a->value != b->value) {
                snprintf(why, cap, "bus %d: %c %08X %u %016" PRIX64 " want %c %08X %u %016" PRIX64, n, a->kind, a->addr,
                         a->size, a->value, b->kind, b->addr, b->size, b->value);
                return 0;
            }
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: n64_vectors_test FILE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    static char line[4096];
    static Case k;
    int total = 0, failed = 0, in_case = 0;
    char last_fail[32] = "";
    int same_name_failures = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (!strncmp(line, "case ", 5)) {
            memset(&k, 0, sizeof k);
            sscanf(line + 5, "%31s", k.name);
            in_case = 1;
        } else if (!strncmp(line, "insn ", 5)) {
            char *p = line + 5;
            while (*p && *p != '\n' && k.ninsns < 8) {
                k.insns[k.ninsns++] = (uint32_t)strtoul(p, &p, 16);
                while (*p == ' ') p++;
            }
        } else if (!strncmp(line, "count ", 6)) k.count = atoi(line + 6);
        else if (!strncmp(line, "fr ", 3)) k.fr = atoi(line + 3);
        else if (!strncmp(line, "init", 4)) parse_regs(line + 4, &k.init);
        else if (!strncmp(line, "final", 5)) parse_regs(line + 5, &k.final);
        else if (!strncmp(line, "exc ", 4)) {
            k.has_exc = 1;
            k.exc = atoi(line + 4);
        } else if (!strncmp(line, "pc ", 3)) k.pc = strtoull(line + 3, NULL, 16);
        else if (!strncmp(line, "mem ", 4)) {
            k.mem_addr = (uint32_t)strtoul(line + 4, NULL, 16);
            parse_hex_bytes(line + 13, k.mem, sizeof k.mem, &k.mem_len);
        } else if (!strncmp(line, "memafter ", 9)) {
            k.has_after = 1;
            k.after_addr = (uint32_t)strtoul(line + 9, NULL, 16);
            parse_hex_bytes(line + 18, k.after, sizeof k.after, &k.after_len);
        } else if (!strncmp(line, "bus ", 4) && k.nbus < 64) {
            Txn *t = &k.bus[k.nbus++];
            char kind;
            unsigned long long v;
            unsigned addr, size;
            if (sscanf(line + 4, "%c %x %u %llx", &kind, &addr, &size, &v) == 4) {
                t->kind = kind;
                t->addr = addr;
                t->size = size;
                t->value = v;
            }
        } else if (!strncmp(line, "end", 3) && in_case) {
            char why[160];
            total++;
            if (!run_case(&k, why, sizeof why)) {
                failed++;
                if (strcmp(last_fail, k.name)) {
                    same_name_failures = 0;
                    snprintf(last_fail, sizeof last_fail, "%s", k.name);
                }
                if (same_name_failures++ < 3) printf("FAIL %s [%08X]: %s\n", k.name, k.insns[0], why);
            }
            in_case = 0;
        }
    }
    fclose(f);
    printf("%d vectors, %d failed, %d listed deviations (LWR upper half)\n", total, failed, g_deviations);
    if (total == 0) return 1; /* fail closed */
    return failed ? 1 : 0;
}
