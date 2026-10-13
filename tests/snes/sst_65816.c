/*
 * SingleStepTests 65816 harness.
 *
 *   sst_65816 DIR [--only XX] [--mode e|n] [--report N]
 *
 * DIR holds the v1/*.json files fetched by tools/fetch_snes_tests.py
 * (roms/snes/sst65816/v1). For every opcode and both modes it loads the
 * initial state, executes one instruction through the interpreter (and so
 * through the shared handlers in snes_cpu_impl.h), and compares every
 * register, flag, the memory the vector lists, and the ordered bus cycles:
 * reads and writes by address and value, and internal (idle) cycles by
 * position. Exit status is non-zero on any mismatch or unreadable file
 * (fail closed).
 *
 * Two vector shapes need care, both documented in docs/SNES-PLAN.md:
 *  - MVN/MVP vectors stop after exactly 100 bus cycles, in the middle of an
 *    iteration. The harness runs whole iterations, then checks that the
 *    remaining cycles are the next iteration's opening fetches.
 *  - WAI/STP vectors end with an address-less cycle: the CPU has stopped.
 *
 * Where the vectors disagree with the hardware, the harness checks the
 * hardware behaviour at the disputed bus cycle instead and counts the
 * vector as a deviation (never as a pass). Both cases are confirmed by
 * gilyon's cputest, which was verified on a real SNES (tests 0027 and
 * 0277), and match ares:
 *  - (dp,X) in emulation mode: when the pointer's low byte is the last byte
 *    of a page, its high byte comes from the start of the same page (the
 *    +1 wraps within the page), whatever D's low byte. The vectors read it
 *    from the next page (gilyon tests 0024 and 0027 show the wrap).
 *  - JSR (a,X) in emulation mode pushes with a 16-bit stack pointer, like
 *    the other 65816-only instructions. The vectors wrap it within page 1.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_cpu.h"
#include "snes_bus.h"
#include "sst_json.h"

enum { TX_READ, TX_WRITE, TX_IDLE };

typedef struct {
    uint8_t type;
    uint32_t addr;
    uint8_t value;
    uint8_t old; /* memory before a write, to undo it */
} Txn;

static uint8_t *g_mem; /* 16 MiB flat address space, as the vectors assume */
static Txn g_log[1024];
static int g_log_n;

uint8_t snes_bus_read(SnesCpu *c, uint32_t addr)
{
    uint8_t v = g_mem[addr & 0xFFFFFFu];
    if (g_log_n < 1024) g_log[g_log_n++] = (Txn){ TX_READ, addr, v, 0 };
    c->mdr = v;
    c->cycles += 1;
    return v;
}

void snes_bus_write(SnesCpu *c, uint32_t addr, uint8_t value)
{
    if (g_log_n < 1024) g_log[g_log_n++] = (Txn){ TX_WRITE, addr, value, g_mem[addr & 0xFFFFFFu] };
    g_mem[addr & 0xFFFFFFu] = value;
    c->mdr = value;
    c->cycles += 1;
}

void snes_bus_idle(SnesCpu *c)
{
    if (g_log_n < 1024) g_log[g_log_n++] = (Txn){ TX_IDLE, 0, 0, 0 };
    c->cycles += 1;
}

/* SA-1 only; the vectors are S-CPU (id 0) behaviour. */
void snes_bus_idle_jump(SnesCpu *c) { (void)c; }

typedef struct {
    int64_t pc, s, p, a, x, y, dbr, d, pbr, e;
} Regs;

static bool read_regs(const JsDoc *doc, int32_t node, Regs *r)
{
    bool ok = true;
    r->pc = js_num(doc, node, "pc", &ok);
    r->s = js_num(doc, node, "s", &ok);
    r->p = js_num(doc, node, "p", &ok);
    r->a = js_num(doc, node, "a", &ok);
    r->x = js_num(doc, node, "x", &ok);
    r->y = js_num(doc, node, "y", &ok);
    r->dbr = js_num(doc, node, "dbr", &ok);
    r->d = js_num(doc, node, "d", &ok);
    r->pbr = js_num(doc, node, "pbr", &ok);
    r->e = js_num(doc, node, "e", &ok);
    return ok;
}

/* Apply (or undo, with value 0) a "ram" list. */
static bool apply_ram(const JsDoc *doc, int32_t state, bool clear)
{
    int32_t ram = js_get(doc, state, "ram");
    if (ram < 0) return false;
    for (int32_t e = doc->nodes[ram].child; e >= 0; e = doc->nodes[e].next) {
        int32_t a = doc->nodes[e].child;
        int32_t v = a >= 0 ? doc->nodes[a].next : -1;
        if (a < 0 || v < 0) return false;
        g_mem[doc->nodes[a].num & 0xFFFFFF] = clear ? 0 : (uint8_t)doc->nodes[v].num;
    }
    return true;
}

typedef struct {
    int report;
    uint64_t tests, failures, deviations;
    int files_failed;
} Stats;

/* Hardware overrides (see the header comment). Returns true when cycle
 * `index` of this vector is one the vectors get wrong; *hw_addr is then
 * the address the hardware uses. */
static bool hardware_override(int opcode, const Regs *ri, const Txn *log, int index, uint32_t sst_addr, uint32_t *hw_addr)
{
    if (!ri->e) return false;
    const bool dp_x = (opcode & 0x1F) == 0x01; /* 01 21 41 ... E1: (dp,X) */
    if (dp_x) {
        uint32_t dp = log[1].value, x = (uint32_t)ri->x & 0xFFu, d = (uint32_t)ri->d;
        uint32_t lo = (d & 0xFFu) ? (d + dp + x) & 0xFFFFu : d | ((dp + x) & 0xFFu);
        int hi_cycle = (d & 0xFFu) ? 5 : 4; /* one more internal cycle when DL != 0 */
        if (index == hi_cycle && (lo & 0xFFu) == 0xFFu && sst_addr == ((lo + 1u) & 0xFFFFu)) {
            *hw_addr = lo & 0xFF00u;
            return true;
        }
    }
    if (opcode == 0xFC && (ri->s & 0xFF) == 0x00 && index == 3 && sst_addr == 0x01FF) {
        *hw_addr = 0x00FF;
        return true;
    }
    return false;
}

static void load_cpu(SnesCpu *c, const Regs *r)
{
    memset(c, 0, sizeof *c);
    c->flag_e = (uint8_t)(r->e & 1);
    c->pc = (uint16_t)r->pc;
    c->s = (uint16_t)r->s;
    c->a = (uint16_t)r->a;
    c->x = (uint16_t)r->x;
    c->y = (uint16_t)r->y;
    c->d = (uint16_t)r->d;
    c->dbr = (uint8_t)r->dbr;
    c->pbr = (uint8_t)r->pbr;
    /* snes_cpu_set_p enforces the hardware invariants: M = X = 1 in
     * emulation mode, X/Y high bytes zero with 8-bit indexes. */
    snes_cpu_set_p(c, (uint8_t)r->p);
    if (c->flag_e) c->s = (uint16_t)(0x0100u | (c->s & 0xFFu));
}

static void describe(const char *name, const char *what)
{
    printf("  %s: %s\n", name, what);
}

static int run_file(const char *path, int opcode, Stats *st)
{
    JsDoc doc;
    int32_t root = js_load(&doc, path);
    if (root < 0 || doc.nodes[root].type != JS_ARR) {
        printf("%s: cannot read or parse\n", path);
        js_free(&doc);
        return -1;
    }
    int fails = 0, count = 0;
    const bool block_move = opcode == 0x44 || opcode == 0x54;
    for (int32_t t = doc.nodes[root].child; t >= 0; t = doc.nodes[t].next) {
        int32_t name_n = js_get(&doc, t, "name");
        int32_t init = js_get(&doc, t, "initial"), fin = js_get(&doc, t, "final"), cyc = js_get(&doc, t, "cycles");
        char name[64] = "?";
        if (name_n >= 0) snprintf(name, sizeof name, "%.*s", (int)doc.nodes[name_n].len, doc.nodes[name_n].str);
        Regs ri, rf;
        if (init < 0 || fin < 0 || cyc < 0 || !read_regs(&doc, init, &ri) || !read_regs(&doc, fin, &rf) ||
            !apply_ram(&doc, init, false)) {
            printf("%s: malformed test %s\n", path, name);
            js_free(&doc);
            return -1;
        }
        count++;

        /* Expected cycles. */
        int expected = 0;
        bool halted_marker = false;
        for (int32_t cn = doc.nodes[cyc].child; cn >= 0; cn = doc.nodes[cn].next) expected++;

        SnesCpu cpu;
        load_cpu(&cpu, &ri);
        g_log_n = 0;
        char why[256] = "";

        if (block_move) {
            /* Whole iterations while they fit in the vector's cycles. */
            for (;;) {
                int before = g_log_n;
                SnesCpu save = cpu;
                snes_cpu_step(&cpu);
                if (g_log_n > expected) {
                    /* This iteration does not fit: undo it (the partial
                     * fetches are checked below). */
                    for (int k = g_log_n - 1; k >= before; k--) {
                        if (g_log[k].type == TX_WRITE) g_mem[g_log[k].addr & 0xFFFFFFu] = g_log[k].old;
                    }
                    cpu = save;
                    g_log_n = before;
                    break;
                }
                if (g_log_n == expected) break;
            }
        } else {
            snes_cpu_step(&cpu);
        }

        /* Compare cycles. */
        bool deviation = false;
        int i = 0;
        for (int32_t cn = doc.nodes[cyc].child; cn >= 0 && !why[0]; cn = doc.nodes[cn].next, i++) {
            int32_t an = doc.nodes[cn].child;
            int32_t vn = an >= 0 ? doc.nodes[an].next : -1;
            int32_t fn = vn >= 0 ? doc.nodes[vn].next : -1;
            if (an < 0 || vn < 0 || fn < 0) {
                snprintf(why, sizeof why, "malformed cycle %d", i);
                break;
            }
            const JsNode *flags = &doc.nodes[fn];
            if (doc.nodes[an].type == JS_NULL) {
                /* Address-less cycle: WAI/STP has stopped the CPU. */
                if (!cpu.wai && !cpu.stp) snprintf(why, sizeof why, "cycle %d: vector says halted, CPU is running", i);
                halted_marker = true;
                continue;
            }
            uint32_t addr = (uint32_t)doc.nodes[an].num;
            bool is_write = flags->len >= 4 && flags->str[3] == 'w';
            bool has_value = doc.nodes[vn].type == JS_NUM;
            uint8_t value = has_value ? (uint8_t)doc.nodes[vn].num : 0;
            if (i >= g_log_n) {
                if (block_move && !is_write && has_value && i - g_log_n < 3) {
                    /* Opening fetches of the next MVN/MVP iteration. */
                    uint32_t fetch = ((uint32_t)cpu.pbr << 16) | (uint16_t)(cpu.pc + (i - g_log_n));
                    if (addr != fetch || g_mem[addr] != value)
                        snprintf(why, sizeof why, "cycle %d: partial iteration fetch %06X=%02X, expected %06X", i, addr,
                                 value, fetch);
                    continue;
                }
                snprintf(why, sizeof why, "cycle %d: CPU made only %d cycles", i, g_log_n);
                break;
            }
            const Txn *tx = &g_log[i];
            int want = is_write ? TX_WRITE : (has_value ? TX_READ : TX_IDLE);
            static const char *names[] = { "read", "write", "idle" };
            uint32_t hw_addr;
            if (hardware_override(opcode, &ri, g_log, i, addr, &hw_addr)) {
                if (tx->type != want || tx->addr != hw_addr)
                    snprintf(why, sizeof why, "cycle %d: %s %06X, hardware does %s %06X", i, names[tx->type], tx->addr,
                             names[want], hw_addr);
                else deviation = true;
                break;
            }
            if (tx->type != want) {
                snprintf(why, sizeof why, "cycle %d: %s %06X, expected %s %06X", i, names[tx->type], tx->addr,
                         names[want], addr);
            } else if (want != TX_IDLE && (tx->addr != addr || tx->value != value)) {
                snprintf(why, sizeof why, "cycle %d: %s %06X=%02X, expected %06X=%02X", i, names[want], tx->addr,
                         tx->value, addr, value);
            }
        }
        if (deviation && !why[0]) {
            st->deviations++;
            apply_ram(&doc, init, true);
            apply_ram(&doc, fin, true);
            for (int k = 0; k < g_log_n; k++) g_mem[g_log[k].addr & 0xFFFFFFu] = 0;
            continue;
        }
        int partial = block_move && expected > g_log_n ? expected - g_log_n : 0;
        if (!why[0] && !halted_marker && g_log_n != expected - partial)
            snprintf(why, sizeof why, "CPU made %d cycles, expected %d", g_log_n, expected);
        if (!why[0] && halted_marker && g_log_n != expected - 1)
            snprintf(why, sizeof why, "CPU made %d cycles before halting, expected %d", g_log_n, expected - 1);

        /* Registers. */
        if (!why[0]) {
            uint16_t pc = (uint16_t)(cpu.pc + partial);
            uint8_t p = snes_cpu_get_p(&cpu);
            if (pc != rf.pc) snprintf(why, sizeof why, "pc %04X, expected %04X", pc, (unsigned)rf.pc);
            else if (cpu.s != rf.s) snprintf(why, sizeof why, "s %04X, expected %04X", cpu.s, (unsigned)rf.s);
            else if (p != rf.p) snprintf(why, sizeof why, "p %02X, expected %02X", p, (unsigned)rf.p);
            else if (cpu.a != rf.a) snprintf(why, sizeof why, "a %04X, expected %04X", cpu.a, (unsigned)rf.a);
            else if (cpu.x != rf.x) snprintf(why, sizeof why, "x %04X, expected %04X", cpu.x, (unsigned)rf.x);
            else if (cpu.y != rf.y) snprintf(why, sizeof why, "y %04X, expected %04X", cpu.y, (unsigned)rf.y);
            else if (cpu.dbr != rf.dbr) snprintf(why, sizeof why, "dbr %02X, expected %02X", cpu.dbr, (unsigned)rf.dbr);
            else if (cpu.d != rf.d) snprintf(why, sizeof why, "d %04X, expected %04X", cpu.d, (unsigned)rf.d);
            else if (cpu.pbr != rf.pbr) snprintf(why, sizeof why, "pbr %02X, expected %02X", cpu.pbr, (unsigned)rf.pbr);
            else if (cpu.flag_e != rf.e) snprintf(why, sizeof why, "e %d, expected %d", cpu.flag_e, (int)rf.e);
        }
        /* Memory the vector lists. */
        if (!why[0]) {
            int32_t ram = js_get(&doc, fin, "ram");
            for (int32_t e = ram >= 0 ? doc.nodes[ram].child : -1; e >= 0 && !why[0]; e = doc.nodes[e].next) {
                int32_t a = doc.nodes[e].child;
                int32_t v = doc.nodes[a].next;
                uint32_t addr = (uint32_t)doc.nodes[a].num & 0xFFFFFFu;
                if (g_mem[addr] != (uint8_t)doc.nodes[v].num)
                    snprintf(why, sizeof why, "ram[%06X]=%02X, expected %02X", addr, g_mem[addr],
                             (unsigned)doc.nodes[v].num);
            }
        }

        if (why[0]) {
            fails++;
            if (st->report > 0) {
                st->report--;
                describe(name, why);
            }
        }
        /* Clear every byte this test touched. */
        apply_ram(&doc, init, true);
        apply_ram(&doc, fin, true);
        for (int k = 0; k < g_log_n; k++) g_mem[g_log[k].addr & 0xFFFFFFu] = 0;
    }
    st->tests += (uint64_t)count;
    st->failures += (uint64_t)fails;
    js_free(&doc);
    return fails;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    int only = -1;
    char only_mode = 0;
    Stats st = { 5, 0, 0, 0, 0 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) only = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) only_mode = argv[++i][0];
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) st.report = atoi(argv[++i]);
        else if (!dir) dir = argv[i];
        else {
            fprintf(stderr, "usage: sst_65816 DIR [--only XX] [--mode e|n] [--report N]\n");
            return 2;
        }
    }
    if (!dir) {
        fprintf(stderr, "usage: sst_65816 DIR [--only XX] [--mode e|n] [--report N]\n");
        return 2;
    }
    g_mem = (uint8_t *)calloc(1, 1u << 24);
    if (!g_mem) return 2;

    int bad_files = 0;
    for (int op = 0; op < 256; op++) {
        if (only >= 0 && op != only) continue;
        for (int m = 0; m < 2; m++) {
            char mode = m ? 'n' : 'e';
            if (only_mode && mode != only_mode) continue;
            char path[1024];
            snprintf(path, sizeof path, "%s/%02x.%c.json", dir, op, mode);
            int f = run_file(path, op, &st);
            if (f < 0) {
                bad_files++;
                continue;
            }
            if (f > 0) {
                st.files_failed++;
                printf("%02x.%c: %d failures\n", op, mode, f);
            }
        }
    }
    printf("%" PRIu64 " tests, %" PRIu64 " failures, %" PRIu64
           " hardware deviations (checked against the hardware rule), %d files with failures, %d unreadable files\n",
           st.tests, st.failures, st.deviations, st.files_failed, bad_files);
    free(g_mem);
    if (bad_files || st.tests == 0) return 1;
    return st.failures ? 1 : 0;
}
