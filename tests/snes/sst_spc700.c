/*
 * SingleStepTests SPC700 harness.
 *
 *   sst_spc700 DIR [--only XX] [--report N]
 *
 * DIR holds the JSON vector files fetched by tools/fetch_snes_tests.py
 * (roms/snes/sstspc700/v1, one file per opcode, 00.json .. ff.json). For
 * every test it loads the initial state into a flat 64 KiB RAM, executes
 * one instruction through the library's SPC700 core (the flat-bus
 * instantiation of snes_spc700_impl.h, the same instruction code the APU
 * runs) and compares every register, PSW, the memory the vector lists and
 * the ordered bus cycles:
 *  - "read"  [addr, value]: a read of addr returning value;
 *  - "read"  [addr, null]:  a dummy read of addr whose value the vector
 *                           does not know (the address is not in its RAM
 *                           list): the address is checked, not the value;
 *  - "write" [addr, value]: a write;
 *  - "wait"  [null, null]:  an internal (idle) cycle.
 * SLEEP (EF) and STOP (FF) vectors record the halted core's first loop
 * iterations (dummy read of PC, idle); the harness keeps stepping the
 * halted core until the vector's cycle count is reached.
 *
 * Exit status is non-zero on any mismatch, malformed test or unreadable
 * file (fail closed). No vector is excused.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_spc700.h"
#include "sst_json.h"

enum { LOG_CAP = 256 };

typedef struct {
    int64_t pc, a, x, y, sp, psw;
} Regs;

static bool read_regs(const JsDoc *doc, int32_t node, Regs *r)
{
    bool ok = true;
    r->pc = js_num(doc, node, "pc", &ok);
    r->a = js_num(doc, node, "a", &ok);
    r->x = js_num(doc, node, "x", &ok);
    r->y = js_num(doc, node, "y", &ok);
    r->sp = js_num(doc, node, "sp", &ok);
    r->psw = js_num(doc, node, "psw", &ok);
    return ok;
}

/* Apply (or clear) a "ram" list. */
static bool apply_ram(const JsDoc *doc, int32_t state, uint8_t *ram, bool clear)
{
    int32_t list = js_get(doc, state, "ram");
    if (list < 0 || doc->nodes[list].type != JS_ARR) return false;
    for (int32_t e = doc->nodes[list].child; e >= 0; e = doc->nodes[e].next) {
        int32_t a = doc->nodes[e].child;
        int32_t v = a >= 0 ? doc->nodes[a].next : -1;
        if (a < 0 || v < 0 || doc->nodes[a].type != JS_NUM || doc->nodes[v].type != JS_NUM) return false;
        ram[doc->nodes[a].num & 0xFFFF] = clear ? 0 : (uint8_t)doc->nodes[v].num;
    }
    return true;
}

typedef struct {
    int report;
    uint64_t tests, failures;
    int files_failed;
} Stats;

static bool str_is(const JsNode *n, const char *s)
{
    size_t len = strlen(s);
    return n->type == JS_STR && n->len == len && !memcmp(n->str, s, len);
}

static int run_file(const char *path, uint8_t *ram, Stats *st)
{
    JsDoc doc;
    int32_t root = js_load(&doc, path);
    if (root < 0 || doc.nodes[root].type != JS_ARR) {
        printf("%s: cannot read or parse\n", path);
        js_free(&doc);
        return -1;
    }
    static const char *names[] = { "read", "write", "idle" };
    int fails = 0, count = 0;
    SnesSpc700Txn log[LOG_CAP];
    for (int32_t t = doc.nodes[root].child; t >= 0; t = doc.nodes[t].next) {
        int32_t name_n = js_get(&doc, t, "name");
        int32_t init = js_get(&doc, t, "initial"), fin = js_get(&doc, t, "final"), cyc = js_get(&doc, t, "cycles");
        char name[64] = "?";
        if (name_n >= 0) snprintf(name, sizeof name, "%.*s", (int)doc.nodes[name_n].len, doc.nodes[name_n].str);
        Regs ri, rf;
        if (init < 0 || fin < 0 || cyc < 0 || doc.nodes[cyc].type != JS_ARR || !read_regs(&doc, init, &ri) ||
            !read_regs(&doc, fin, &rf) || !apply_ram(&doc, init, ram, false)) {
            printf("%s: malformed test %s\n", path, name);
            js_free(&doc);
            return -1;
        }
        count++;

        int expected = 0;
        for (int32_t cn = doc.nodes[cyc].child; cn >= 0; cn = doc.nodes[cn].next) expected++;

        SnesSpc700 cpu;
        memset(&cpu, 0, sizeof cpu);
        cpu.pc = (uint16_t)ri.pc;
        cpu.a = (uint8_t)ri.a;
        cpu.x = (uint8_t)ri.x;
        cpu.y = (uint8_t)ri.y;
        cpu.sp = (uint8_t)ri.sp;
        snes_spc700_set_psw(&cpu, (uint8_t)ri.psw);

        SnesSpc700FlatBus bus;
        memset(&bus, 0, sizeof bus);
        bus.ram = ram;
        bus.log = log;
        bus.log_cap = LOG_CAP;
        snes_spc700_step_flat(&cpu, &bus);
        /* A halted core keeps cycling (dummy read of PC, idle). */
        while (cpu.halt && bus.log_n < (uint32_t)expected && bus.log_n < LOG_CAP) snes_spc700_step_flat(&cpu, &bus);
        const int got = (int)(bus.log_n < LOG_CAP ? bus.log_n : LOG_CAP);

        char why[256] = "";
        int i = 0;
        for (int32_t cn = doc.nodes[cyc].child; cn >= 0 && !why[0]; cn = doc.nodes[cn].next, i++) {
            int32_t an = doc.nodes[cn].child;
            int32_t vn = an >= 0 ? doc.nodes[an].next : -1;
            int32_t kn = vn >= 0 ? doc.nodes[vn].next : -1;
            if (an < 0 || vn < 0 || kn < 0) {
                snprintf(why, sizeof why, "malformed cycle %d", i);
                break;
            }
            const JsNode *kind = &doc.nodes[kn];
            int want;
            if (str_is(kind, "read")) want = SNES_SPC700_TX_READ;
            else if (str_is(kind, "write")) want = SNES_SPC700_TX_WRITE;
            else if (str_is(kind, "wait")) want = SNES_SPC700_TX_IDLE;
            else {
                snprintf(why, sizeof why, "cycle %d: unknown kind %.*s", i, (int)kind->len, kind->str ? kind->str : "");
                break;
            }
            const bool has_addr = doc.nodes[an].type == JS_NUM;
            const bool has_value = doc.nodes[vn].type == JS_NUM;
            if (want != SNES_SPC700_TX_IDLE && !has_addr) {
                snprintf(why, sizeof why, "malformed cycle %d: %s without an address", i, names[want]);
                break;
            }
            if (want == SNES_SPC700_TX_WRITE && !has_value) {
                snprintf(why, sizeof why, "malformed cycle %d: write without a value", i);
                break;
            }
            const uint16_t addr = has_addr ? (uint16_t)doc.nodes[an].num : 0;
            const uint8_t value = has_value ? (uint8_t)doc.nodes[vn].num : 0;
            if (i >= got) {
                snprintf(why, sizeof why, "cycle %d: CPU made only %d cycles", i, got);
                break;
            }
            const SnesSpc700Txn *tx = &log[i];
            if (tx->type != want) {
                snprintf(why, sizeof why, "cycle %d: %s %04X, expected %s %04X", i, names[tx->type], tx->addr,
                         names[want], addr);
            } else if (want != SNES_SPC700_TX_IDLE && tx->addr != addr) {
                snprintf(why, sizeof why, "cycle %d: %s %04X, expected %s %04X", i, names[want], tx->addr, names[want],
                         addr);
            } else if (want != SNES_SPC700_TX_IDLE && has_value && tx->value != value) {
                snprintf(why, sizeof why, "cycle %d: %s %04X=%02X, expected %04X=%02X", i, names[want], tx->addr,
                         tx->value, addr, value);
            }
        }
        if (!why[0] && (uint32_t)expected != bus.log_n)
            snprintf(why, sizeof why, "CPU made %" PRIu32 " cycles, expected %d", bus.log_n, expected);

        if (!why[0]) {
            uint8_t psw = snes_spc700_get_psw(&cpu);
            if (cpu.pc != rf.pc) snprintf(why, sizeof why, "pc %04X, expected %04X", cpu.pc, (unsigned)rf.pc);
            else if (cpu.a != rf.a) snprintf(why, sizeof why, "a %02X, expected %02X", cpu.a, (unsigned)rf.a);
            else if (cpu.x != rf.x) snprintf(why, sizeof why, "x %02X, expected %02X", cpu.x, (unsigned)rf.x);
            else if (cpu.y != rf.y) snprintf(why, sizeof why, "y %02X, expected %02X", cpu.y, (unsigned)rf.y);
            else if (cpu.sp != rf.sp) snprintf(why, sizeof why, "sp %02X, expected %02X", cpu.sp, (unsigned)rf.sp);
            else if (psw != rf.psw) snprintf(why, sizeof why, "psw %02X, expected %02X", psw, (unsigned)rf.psw);
        }
        if (!why[0]) {
            int32_t list = js_get(&doc, fin, "ram");
            if (list < 0 || doc.nodes[list].type != JS_ARR) snprintf(why, sizeof why, "final state has no ram list");
            for (int32_t e = list >= 0 ? doc.nodes[list].child : -1; e >= 0 && !why[0]; e = doc.nodes[e].next) {
                int32_t a = doc.nodes[e].child;
                int32_t v = a >= 0 ? doc.nodes[a].next : -1;
                if (a < 0 || v < 0) {
                    snprintf(why, sizeof why, "malformed final ram entry");
                    break;
                }
                uint16_t addr = (uint16_t)doc.nodes[a].num;
                if (ram[addr] != (uint8_t)doc.nodes[v].num)
                    snprintf(why, sizeof why, "ram[%04X]=%02X, expected %02X", addr, ram[addr],
                             (unsigned)doc.nodes[v].num);
            }
        }

        if (why[0]) {
            fails++;
            if (st->report > 0) {
                st->report--;
                printf("  %s: %s\n", name, why);
            }
        }
        /* Clear every byte this test touched. */
        apply_ram(&doc, init, ram, true);
        apply_ram(&doc, fin, ram, true);
        for (int k = 0; k < got; k++)
            if (log[k].type == SNES_SPC700_TX_WRITE) ram[log[k].addr] = 0;
    }
    st->tests += (uint64_t)count;
    st->failures += (uint64_t)fails;
    js_free(&doc);
    if (count == 0) {
        printf("%s: no tests\n", path);
        return -1;
    }
    return fails;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    int only = -1;
    Stats st = { 5, 0, 0, 0 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) only = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) st.report = atoi(argv[++i]);
        else if (!dir) dir = argv[i];
        else {
            fprintf(stderr, "usage: sst_spc700 DIR [--only XX] [--report N]\n");
            return 2;
        }
    }
    if (!dir) {
        fprintf(stderr, "usage: sst_spc700 DIR [--only XX] [--report N]\n");
        return 2;
    }
    uint8_t *ram = (uint8_t *)calloc(1, 0x10000);
    if (!ram) return 2;

    int bad_files = 0, files = 0;
    for (int op = 0; op < 256; op++) {
        if (only >= 0 && op != only) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%02x.json", dir, op);
        int f = run_file(path, ram, &st);
        files++;
        if (f < 0) {
            bad_files++;
            continue;
        }
        if (f > 0) {
            st.files_failed++;
            printf("%02x: %d failures\n", op, f);
        }
    }
    printf("%" PRIu64 " tests, %" PRIu64 " passed, %" PRIu64 " failures, %d/%d opcode files with failures, %d unreadable files\n",
           st.tests, st.tests - st.failures, st.failures, st.files_failed, files, bad_files);
    free(ram);
    if (bad_files || st.tests == 0) return 1;
    return st.failures ? 1 : 0;
}
