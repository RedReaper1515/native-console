/*
 * SingleStepTests 6502 harness for the NES CPU core.
 *
 *   sst_6502 DIR [--only XX] [--report N]
 *   sst_6502 --units
 *
 * DIR holds the nes6502 v1 files (roms/nes/sst6502/v1, 00.json to ff.json):
 * one per opcode, 10,000 vectors each, recorded for the 6502 without decimal
 * mode that the NES has. For every vector the harness loads the initial state,
 * executes one instruction through the interpreter (and so through the
 * shared handlers in nes_cpu_impl.h), and compares every register, the
 * flags, the memory the vector lists, and the ordered bus cycles: address,
 * value and direction of each. Exit status is non-zero on any mismatch, on
 * an unreadable file, and when no vectors were found (fail closed).
 *
 * The vectors say nothing about three things, so the harness checks those
 * against the hardware rules for every vector as well:
 *  - the interrupt poll the handler recorded (c->poll_at, c->poll_i; the
 *    rules are in nes_cpu.h);
 *  - c->branched and c->jammed;
 *  - the classification in nes_cpu_decode.h: the instruction's length,
 *    its control-flow class and whether it writes.
 *
 * P in the vectors is a whole byte, but the register has only six bits:
 * bit 5 and bit 4 (B) exist only in the byte the CPU pushes. Most files
 * carry them as 1 and 0, some as 1 and 1; no vector changes them. The
 * harness compares the six real flags, requires the vector to leave the
 * other two as they were, and checks the pushed byte, B included, like any
 * other bus cycle.
 *
 * One opcode is not taken from the vectors as recorded: LXA ($AB), whose
 * result depends on a constant the chip does not fix. The vectors have
 * A = X = (A | $EE) & operand; the core follows blargg's hardware-verified
 * test ROMs, where it is A = X = operand (the constant is $FF; see
 * NES_LXA_MAGIC in nes_cpu_impl.h). For every $AB vector the harness first
 * checks that the vector itself is the $EE formula, then compares the core
 * with the hardware rule: A, X, N and Z from the operand, everything else
 * (cycles, the other flags, PC) as the vector says. A vector where the two
 * rules give different results is counted as a deviation, never as a pass,
 * and the count is printed.
 *
 * --units runs checks that need no vectors: the reset and interrupt
 * sequences (which the vectors do not cover), the vector hook and NMI
 * hijacking, where the interrupt poll sits for the special cases, and a
 * jammed CPU.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes_bus.h"
#include "nes_cpu.h"
#include "nes_cpu_decode.h"
#include "sst_json.h"

enum { TX_READ, TX_WRITE };
enum { LOG_MAX = 64 };

typedef struct {
    uint8_t type;
    uint16_t addr;
    uint8_t value;
} Txn;

static uint8_t g_mem[0x10000]; /* a flat 64 KiB of RAM, as the vectors assume */
static Txn g_log[LOG_MAX];
static int g_log_n;

/* The vector hook: how often it was called, with what, and after how many
 * bus cycles. g_vector_reply, when non-zero, is returned instead of the
 * argument (the units use it to play an NMI that hijacks the sequence). */
static int g_vector_calls;
static uint16_t g_vector_arg;
static int g_vector_at;
static uint16_t g_vector_reply;

/* The units can have the bus halt one read (a DMA pulling RDY low): the
 * read with this index in the log reports c->stalled, as the machine's
 * bus does. -1: none. */
static int g_stall_read = -1;

uint8_t nes_bus_read(NesCpu *c, uint16_t addr)
{
    uint8_t v = g_mem[addr];
    if (g_log_n == g_stall_read) c->stalled = 1;
    if (g_log_n < LOG_MAX) g_log[g_log_n] = (Txn){ TX_READ, addr, v };
    g_log_n++;
    c->mdr = v;
    c->cycles += 1;
    return v;
}

void nes_bus_write(NesCpu *c, uint16_t addr, uint8_t value)
{
    if (g_log_n < LOG_MAX) g_log[g_log_n] = (Txn){ TX_WRITE, addr, value };
    g_log_n++;
    g_mem[addr] = value;
    c->mdr = value;
    c->cycles += 1;
}

uint16_t nes_bus_interrupt_vector(NesCpu *c, uint16_t vector)
{
    (void)c;
    g_vector_calls++;
    g_vector_arg = vector;
    g_vector_at = g_log_n;
    return g_vector_reply ? g_vector_reply : vector;
}

static void reset_bus_log(void)
{
    g_log_n = 0;
    g_vector_calls = 0;
    g_vector_arg = 0;
    g_vector_at = -1;
}

/* ------------------------------------------------------------------ */
/* Vectors                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t pc, s, a, x, y, p;
} Regs;

static bool read_regs(const JsDoc *doc, int32_t node, Regs *r)
{
    bool ok = true;
    r->pc = js_num(doc, node, "pc", &ok);
    r->s = js_num(doc, node, "s", &ok);
    r->a = js_num(doc, node, "a", &ok);
    r->x = js_num(doc, node, "x", &ok);
    r->y = js_num(doc, node, "y", &ok);
    r->p = js_num(doc, node, "p", &ok);
    return ok && r->pc >= 0 && r->pc <= 0xFFFF && r->s >= 0 && r->s <= 0xFF && r->a >= 0 && r->a <= 0xFF &&
           r->x >= 0 && r->x <= 0xFF && r->y >= 0 && r->y <= 0xFF && r->p >= 0 && r->p <= 0xFF;
}

/* Walk a "ram" list of [address, value] pairs: check that it is well
 * formed, load it, or zero the bytes it names. */
enum { RAM_CHECK, RAM_LOAD, RAM_CLEAR };

static bool walk_ram(const JsDoc *doc, int32_t state, int what)
{
    int32_t ram = js_get(doc, state, "ram");
    if (ram < 0 || doc->nodes[ram].type != JS_ARR) return false;
    for (int32_t e = doc->nodes[ram].child; e >= 0; e = doc->nodes[e].next) {
        int32_t a = doc->nodes[e].child;
        int32_t v = a >= 0 ? doc->nodes[a].next : -1;
        if (a < 0 || v < 0 || doc->nodes[a].type != JS_NUM || doc->nodes[v].type != JS_NUM) return false;
        if (doc->nodes[a].num < 0 || doc->nodes[a].num > 0xFFFF || doc->nodes[v].num < 0 || doc->nodes[v].num > 0xFF)
            return false;
        if (what != RAM_CHECK) g_mem[doc->nodes[a].num] = what == RAM_CLEAR ? 0 : (uint8_t)doc->nodes[v].num;
    }
    return true;
}

static void clear_log_memory(void)
{
    for (int k = 0; k < g_log_n && k < LOG_MAX; k++) g_mem[g_log[k].addr] = 0;
}

typedef struct {
    int report;
    uint64_t tests, failures, deviations;
    int files_failed;
} Stats;

/* LXA ($AB): replace the vector's result (constant $EE) with the hardware
 * rule's (constant $FF). Returns whether they differ; writes a reason into
 * `why` if the vector is not what this harness takes it to be. */
static bool lxa_hardware_rule(const Regs *ri, Regs *rf, uint8_t operand, char *why, size_t why_n)
{
    const uint8_t vector = (uint8_t)((ri->a | 0xEE) & operand);
    if (rf->a != vector || rf->x != vector) {
        snprintf(why, why_n, "the vector's own result (a %02X x %02X) is not (A | $EE) & operand = %02X", (unsigned)rf->a,
                 (unsigned)rf->x, vector);
        return false;
    }
    const bool differs = vector != operand;
    rf->a = rf->x = operand;
    rf->p = (rf->p & ~(int64_t)(NES_P_N | NES_P_Z)) | (operand & 0x80u) | (operand ? 0 : NES_P_Z);
    return differs;
}

/* A base for c->cycles that no poll bookkeeping could reach by accident. */
#define CYCLE_BASE UINT64_C(1000000)

static void load_cpu(NesCpu *c, const Regs *r)
{
    memset(c, 0, sizeof *c);
    c->pc = (uint16_t)r->pc;
    c->s = (uint8_t)r->s;
    c->a = (uint8_t)r->a;
    c->x = (uint8_t)r->x;
    c->y = (uint8_t)r->y;
    nes_cpu_set_p(c, (uint8_t)r->p);
    c->cycles = CYCLE_BASE;
    /* Stale bookkeeping the instruction must overwrite. */
    c->poll_at = UINT64_C(0xDEADBEEF);
    c->poll_i = 0xEE;
}

/* What the vectors cannot say: poll bookkeeping, branched, jammed, and the
 * decode table. Writes a reason into `why` on a mismatch. */
static void check_rules(int opcode, const NesCpu *cpu, const Regs *ri, const Regs *rf, int cycles, char *why,
                        size_t why_n)
{
    const NesOpInfo *info = nes_opinfo((uint8_t)opcode);
    const unsigned length = nes_insn_length((uint8_t)opcode);
    const bool initial_i = (ri->p & NES_P_I) != 0, final_i = (rf->p & NES_P_I) != 0;
    const uint16_t next = (uint16_t)(ri->pc + length);

    /* Interrupt poll. */
    uint64_t want_at;
    unsigned want_i;
    if (info->flow == NES_FLOW_BREAK || info->flow == NES_FLOW_JAM) {
        want_at = 0; /* no poll */
        want_i = 1;
    } else {
        /* Before the last cycle; a taken branch that stays in its page is
         * polled before its second cycle only. RTI has its new I by then;
         * CLI, SEI and PLP still have the old one. */
        const bool short_taken_branch = info->flow == NES_FLOW_BRANCH_COND && cycles == 3;
        want_at = CYCLE_BASE + (uint64_t)(short_taken_branch ? 1 : cycles - 1);
        want_i = opcode == 0x40 ? final_i : initial_i;
    }
    if (cpu->poll_at != want_at) {
        if (!want_at) snprintf(why, why_n, "poll_at %" PRIu64 ", but the instruction does not poll (0)", cpu->poll_at);
        else
            snprintf(why, why_n, "polled after cycle %" PRId64 ", the hardware polls after cycle %" PRId64,
                     (int64_t)(cpu->poll_at - CYCLE_BASE), (int64_t)(want_at - CYCLE_BASE));
        return;
    }
    if (cpu->poll_i != want_i) {
        snprintf(why, why_n, "poll_i %u, expected %u", cpu->poll_i, want_i);
        return;
    }
    if (cpu->cycles != CYCLE_BASE + (uint64_t)cycles) {
        snprintf(why, why_n, "c->cycles advanced by %" PRIu64 ", bus calls %d", cpu->cycles - CYCLE_BASE, cycles);
        return;
    }

    /* BRK asks the bus for its vector once, four cycles in (PC pushed, P not yet). */
    const int want_calls = info->flow == NES_FLOW_BREAK ? 1 : 0;
    if (g_vector_calls != want_calls || (want_calls && (g_vector_arg != NES_VEC_IRQ || g_vector_at != 4))) {
        snprintf(why, why_n, "vector hook called %d times (arg %04X after %d cycles)", g_vector_calls, g_vector_arg,
                 g_vector_at);
        return;
    }

    /* Control flow as the decode table classifies it. */
    bool want_branched = true;
    switch ((NesFlow)info->flow) {
    case NES_FLOW_NEXT:
        want_branched = false;
        if (rf->pc != next)
            snprintf(why, why_n, "decode: length %u, but the instruction ends at %04X", length, (unsigned)rf->pc);
        break;
    case NES_FLOW_BRANCH_COND:
        want_branched = cycles > 2;
        if (!want_branched && rf->pc != next)
            snprintf(why, why_n, "decode: length %u, but the untaken branch ends at %04X", length, (unsigned)rf->pc);
        break;
    case NES_FLOW_JAM:
        if (rf->pc != next) snprintf(why, why_n, "decode: JAM leaves pc at %04X", (unsigned)rf->pc);
        break;
    case NES_FLOW_CALL:
        if (((ri->s - rf->s) & 0xFF) != 2) snprintf(why, why_n, "decode: CALL moved s by %d", (int)(ri->s - rf->s));
        break;
    case NES_FLOW_BREAK:
        if (((ri->s - rf->s) & 0xFF) != 3) snprintf(why, why_n, "decode: BREAK moved s by %d", (int)(ri->s - rf->s));
        break;
    default: break;
    }
    if (why[0]) return;
    if ((cpu->branched != 0) != want_branched) {
        snprintf(why, why_n, "branched %u, decode flow class %u wants %d", cpu->branched, info->flow, want_branched);
        return;
    }
    if ((cpu->jammed != 0) != (info->flow == NES_FLOW_JAM)) {
        snprintf(why, why_n, "jammed %u", cpu->jammed);
        return;
    }

    /* Writes: a writing opcode writes in every vector; any other opcode
     * only pushes, which stays in page 1. */
    int writes = 0, outside_stack = 0;
    for (int k = 0; k < g_log_n; k++) {
        if (g_log[k].type != TX_WRITE) continue;
        writes++;
        if ((g_log[k].addr >> 8) != 0x01) outside_stack++;
    }
    if ((info->flags & NES_OPF_WRITES) ? writes == 0 : outside_stack != 0)
        snprintf(why, why_n, "decode: writes flag %u, but %d writes (%d outside page 1)", info->flags & NES_OPF_WRITES,
                 writes, outside_stack);
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
    for (int32_t t = doc.nodes[root].child; t >= 0; t = doc.nodes[t].next) {
        int32_t name_n = js_get(&doc, t, "name");
        int32_t init = js_get(&doc, t, "initial"), fin = js_get(&doc, t, "final"), cyc = js_get(&doc, t, "cycles");
        char name[64] = "?";
        if (name_n >= 0 && doc.nodes[name_n].type == JS_STR)
            snprintf(name, sizeof name, "%.*s", (int)doc.nodes[name_n].len, doc.nodes[name_n].str);
        Regs ri, rf;
        if (init < 0 || fin < 0 || cyc < 0 || doc.nodes[cyc].type != JS_ARR || !read_regs(&doc, init, &ri) ||
            !read_regs(&doc, fin, &rf) || !walk_ram(&doc, init, RAM_CHECK) || !walk_ram(&doc, fin, RAM_CHECK)) {
            printf("%s: malformed test %s\n", path, name);
            js_free(&doc);
            return -1;
        }
        count++;
        walk_ram(&doc, init, RAM_LOAD);

        NesCpu cpu;
        load_cpu(&cpu, &ri);
        reset_bus_log();
        char why[256] = "";

        if (g_mem[ri.pc] != opcode) snprintf(why, sizeof why, "vector's opcode byte is %02X", g_mem[ri.pc]);
        nes_cpu_step(&cpu);

        /* Bus cycles, in order. */
        int expected = 0;
        for (int32_t cn = doc.nodes[cyc].child; cn >= 0 && !why[0]; cn = doc.nodes[cn].next, expected++) {
            int32_t an = doc.nodes[cn].child;
            int32_t vn = an >= 0 ? doc.nodes[an].next : -1;
            int32_t kn = vn >= 0 ? doc.nodes[vn].next : -1;
            if (an < 0 || vn < 0 || kn < 0 || doc.nodes[an].type != JS_NUM || doc.nodes[vn].type != JS_NUM ||
                doc.nodes[kn].type != JS_STR) {
                snprintf(why, sizeof why, "malformed cycle %d", expected);
                break;
            }
            const JsNode *kind = &doc.nodes[kn];
            const bool is_write = kind->len == 5 && !memcmp(kind->str, "write", 5);
            if (!is_write && !(kind->len == 4 && !memcmp(kind->str, "read", 4))) {
                snprintf(why, sizeof why, "cycle %d: unknown kind %.*s", expected, (int)kind->len, kind->str);
                break;
            }
            if (expected >= g_log_n || expected >= LOG_MAX) {
                snprintf(why, sizeof why, "cycle %d: CPU made only %d cycles", expected, g_log_n);
                break;
            }
            const Txn *tx = &g_log[expected];
            const int want = is_write ? TX_WRITE : TX_READ;
            const uint16_t addr = (uint16_t)doc.nodes[an].num;
            const uint8_t value = (uint8_t)doc.nodes[vn].num;
            static const char *names[] = { "read", "write" };
            if (tx->type != want || tx->addr != addr || tx->value != value)
                snprintf(why, sizeof why, "cycle %d: %s %04X=%02X, expected %s %04X=%02X", expected, names[tx->type],
                         tx->addr, tx->value, names[want], addr, value);
        }
        if (!why[0] && g_log_n != expected)
            snprintf(why, sizeof why, "CPU made %d cycles, expected %d", g_log_n, expected);
        bool deviation = false;
        if (!why[0] && opcode == 0xAB) deviation = lxa_hardware_rule(&ri, &rf, g_log[1].value, why, sizeof why);

        /* Registers and the six flags. */
        if (!why[0]) {
            const uint8_t p = nes_cpu_get_p(&cpu);
            const uint8_t real = (uint8_t)~(NES_P_B | NES_P_1);
            if (cpu.pc != rf.pc) snprintf(why, sizeof why, "pc %04X, expected %04X", cpu.pc, (unsigned)rf.pc);
            else if (cpu.s != rf.s) snprintf(why, sizeof why, "s %02X, expected %02X", cpu.s, (unsigned)rf.s);
            else if (cpu.a != rf.a) snprintf(why, sizeof why, "a %02X, expected %02X", cpu.a, (unsigned)rf.a);
            else if (cpu.x != rf.x) snprintf(why, sizeof why, "x %02X, expected %02X", cpu.x, (unsigned)rf.x);
            else if (cpu.y != rf.y) snprintf(why, sizeof why, "y %02X, expected %02X", cpu.y, (unsigned)rf.y);
            else if ((p & real) != (rf.p & real))
                snprintf(why, sizeof why, "p %02X, expected %02X", p & real, (unsigned)(rf.p & real));
            else if (((ri.p ^ rf.p) & (NES_P_B | NES_P_1)) != 0)
                snprintf(why, sizeof why, "vector changes P bits 4/5 (%02X -> %02X), which the CPU does not have",
                         (unsigned)ri.p, (unsigned)rf.p);
        }
        /* Memory the vector lists. */
        if (!why[0]) {
            int32_t ram = js_get(&doc, fin, "ram");
            for (int32_t e = doc.nodes[ram].child; e >= 0 && !why[0]; e = doc.nodes[e].next) {
                const int32_t a = doc.nodes[e].child, v = doc.nodes[a].next;
                const uint16_t addr = (uint16_t)doc.nodes[a].num;
                if (g_mem[addr] != (uint8_t)doc.nodes[v].num)
                    snprintf(why, sizeof why, "ram[%04X]=%02X, expected %02X", addr, g_mem[addr],
                             (unsigned)doc.nodes[v].num);
            }
        }
        if (!why[0]) check_rules(opcode, &cpu, &ri, &rf, expected, why, sizeof why);

        if (why[0]) {
            fails++;
            if (st->report > 0) {
                st->report--;
                printf("  %s: %s\n", name, why);
            }
        } else if (deviation) {
            st->deviations++;
        }
        /* Clear every byte this test touched. */
        walk_ram(&doc, init, RAM_CLEAR);
        walk_ram(&doc, fin, RAM_CLEAR);
        clear_log_memory();
    }
    if (count == 0) {
        printf("%s: no tests\n", path);
        js_free(&doc);
        return -1;
    }
    st->tests += (uint64_t)count;
    st->failures += (uint64_t)fails;
    js_free(&doc);
    return fails;
}

/* ------------------------------------------------------------------ */
/* Units                                                               */
/* ------------------------------------------------------------------ */

static int g_unit_failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("  FAILED line %d: %s\n", __LINE__, #cond);                                                         \
            g_unit_failures++;                                                                                         \
        }                                                                                                              \
    } while (0)

static bool log_is(int index, int type, uint16_t addr, uint8_t value)
{
    return index < g_log_n && g_log[index].type == type && g_log[index].addr == addr && g_log[index].value == value;
}

static void unit_cpu(NesCpu *c, uint16_t pc, uint8_t s, uint8_t p)
{
    memset(c, 0, sizeof *c);
    memset(g_mem, 0, sizeof g_mem);
    c->pc = pc;
    c->s = s;
    nes_cpu_set_p(c, p);
    c->cycles = CYCLE_BASE;
    c->poll_at = UINT64_C(0xDEADBEEF);
    c->poll_i = 0xEE;
    reset_bus_log();
    g_vector_reply = 0;
    g_stall_read = -1;
}

/* Place a program at the CPU's PC. */
static void unit_code(const NesCpu *c, const uint8_t *bytes, int n)
{
    for (int i = 0; i < n; i++) g_mem[(uint16_t)(c->pc + i)] = bytes[i];
}

#define CODE(c, ...)                                                                                                   \
    do {                                                                                                               \
        const uint8_t code_[] = { __VA_ARGS__ };                                                                       \
        unit_code(c, code_, (int)sizeof code_);                                                                        \
    } while (0)

static void units_reset(void)
{
    NesCpu c;
    /* Power-on: a zeroed CPU. Seven reads, nothing written, S ends at $FD. */
    unit_cpu(&c, 0x0000, 0x00, 0x00);
    c.jammed = 1;
    c.branched = 1;
    g_mem[0xFFFC] = 0x34;
    g_mem[0xFFFD] = 0x12;
    g_mem[0x0100] = 0xA1;
    g_mem[0x01FF] = 0xA2;
    g_mem[0x01FE] = 0xA3;
    nes_cpu_reset(&c);
    CHECK(g_log_n == 7);
    CHECK(log_is(0, TX_READ, 0x0000, 0x00) && log_is(1, TX_READ, 0x0000, 0x00));
    CHECK(log_is(2, TX_READ, 0x0100, 0xA1) && log_is(3, TX_READ, 0x01FF, 0xA2) && log_is(4, TX_READ, 0x01FE, 0xA3));
    CHECK(log_is(5, TX_READ, 0xFFFC, 0x34) && log_is(6, TX_READ, 0xFFFD, 0x12));
    CHECK(c.pc == 0x1234 && c.s == 0xFD && c.flag_i == 1);
    CHECK(g_mem[0x0100] == 0xA1 && g_mem[0x01FF] == 0xA2 && g_mem[0x01FE] == 0xA3);
    CHECK(c.cycles == CYCLE_BASE + 7 && c.jammed == 0 && c.branched == 0);
    CHECK(c.poll_at == 0 && c.poll_i == 1);
    CHECK(g_vector_calls == 0);

    /* A warm reset keeps A, X, Y and the other flags, and moves S by 3. */
    unit_cpu(&c, 0xC123, 0x42, NES_P_N | NES_P_V | NES_P_D | NES_P_Z | NES_P_C);
    c.a = 0x11;
    c.x = 0x22;
    c.y = 0x33;
    g_mem[0xFFFC] = 0x00;
    g_mem[0xFFFD] = 0x80;
    nes_cpu_reset(&c);
    CHECK(log_is(0, TX_READ, 0xC123, 0x00) && log_is(2, TX_READ, 0x0142, 0x00) && log_is(4, TX_READ, 0x0140, 0x00));
    CHECK(c.pc == 0x8000 && c.s == 0x3F && c.a == 0x11 && c.x == 0x22 && c.y == 0x33);
    CHECK(nes_cpu_get_p(&c) == (NES_P_N | NES_P_V | NES_P_D | NES_P_Z | NES_P_C | NES_P_I | NES_P_1));
}

static void units_interrupt(void)
{
    NesCpu c;
    const uint8_t p = NES_P_N | NES_P_D | NES_P_C;

    /* IRQ: two discarded reads of PC, PCH, PCL, P with B clear, vector. */
    unit_cpu(&c, 0x8123, 0xFF, p);
    g_mem[0x8123] = 0xEA;
    g_mem[0xFFFE] = 0x00;
    g_mem[0xFFFF] = 0x90;
    g_mem[0xFFFA] = 0x00;
    g_mem[0xFFFB] = 0xA0;
    nes_cpu_interrupt(&c, false);
    CHECK(g_log_n == 7);
    CHECK(log_is(0, TX_READ, 0x8123, 0xEA) && log_is(1, TX_READ, 0x8123, 0xEA));
    CHECK(log_is(2, TX_WRITE, 0x01FF, 0x81) && log_is(3, TX_WRITE, 0x01FE, 0x23));
    CHECK(log_is(4, TX_WRITE, 0x01FD, p | NES_P_1));
    CHECK(log_is(5, TX_READ, 0xFFFE, 0x00) && log_is(6, TX_READ, 0xFFFF, 0x90));
    CHECK(g_vector_calls == 1 && g_vector_arg == NES_VEC_IRQ && g_vector_at == 4);
    CHECK(c.pc == 0x9000 && c.s == 0xFC && c.flag_i == 1 && c.flag_d == 1 && c.branched == 1);
    CHECK(c.cycles == CYCLE_BASE + 7);
    CHECK(c.poll_at == 0 && c.poll_i == 1); /* the sequence does not poll */

    /* NMI: the same through $FFFA; I does not keep it out. */
    unit_cpu(&c, 0x8123, 0x01, p | NES_P_I);
    g_mem[0xFFFA] = 0x00;
    g_mem[0xFFFB] = 0xA0;
    nes_cpu_interrupt(&c, true);
    CHECK(log_is(2, TX_WRITE, 0x0101, 0x81) && log_is(3, TX_WRITE, 0x0100, 0x23));
    CHECK(log_is(4, TX_WRITE, 0x01FF, p | NES_P_I | NES_P_1)); /* the stack wraps within page 1 */
    CHECK(log_is(5, TX_READ, 0xFFFA, 0x00) && log_is(6, TX_READ, 0xFFFB, 0xA0));
    CHECK(g_vector_calls == 1 && g_vector_arg == NES_VEC_NMI && g_vector_at == 4);
    CHECK(c.pc == 0xA000 && c.s == 0xFE);

    /* An NMI that arrives while an IRQ is entered takes its vector; the
     * pushed P is the IRQ's. */
    unit_cpu(&c, 0x8123, 0xFF, p);
    g_mem[0xFFFE] = 0x00;
    g_mem[0xFFFF] = 0x90;
    g_mem[0xFFFA] = 0x00;
    g_mem[0xFFFB] = 0xA0;
    g_vector_reply = NES_VEC_NMI;
    nes_cpu_interrupt(&c, false);
    CHECK(g_vector_arg == NES_VEC_IRQ);
    CHECK(log_is(4, TX_WRITE, 0x01FD, p | NES_P_1));
    CHECK(log_is(5, TX_READ, 0xFFFA, 0x00) && log_is(6, TX_READ, 0xFFFB, 0xA0));
    CHECK(c.pc == 0xA000);

    /* ...and so does one that arrives during BRK, which still pushes B set
     * and the address after its padding byte. */
    unit_cpu(&c, 0x8123, 0xFF, p);
    CODE(&c, 0x00, 0x77);
    g_mem[0xFFFE] = 0x00;
    g_mem[0xFFFF] = 0x90;
    g_mem[0xFFFA] = 0x00;
    g_mem[0xFFFB] = 0xA0;
    g_vector_reply = NES_VEC_NMI;
    nes_cpu_step(&c);
    CHECK(g_log_n == 7 && g_vector_calls == 1 && g_vector_arg == NES_VEC_IRQ && g_vector_at == 4);
    CHECK(log_is(1, TX_READ, 0x8124, 0x77));
    CHECK(log_is(2, TX_WRITE, 0x01FF, 0x81) && log_is(3, TX_WRITE, 0x01FE, 0x25));
    CHECK(log_is(4, TX_WRITE, 0x01FD, p | NES_P_1 | NES_P_B));
    CHECK(log_is(5, TX_READ, 0xFFFA, 0x00) && log_is(6, TX_READ, 0xFFFB, 0xA0));
    CHECK(c.pc == 0xA000 && c.flag_i == 1 && c.flag_d == 1 && c.branched == 1);
    CHECK(c.poll_at == 0 && c.poll_i == 1);
    g_vector_reply = 0;
}

/* Where the interrupt poll sits, and which I it sees. `at` counts the
 * instruction's completed cycles at the poll; -1 means it does not poll. */
static void poll_case(const char *what, uint16_t pc, uint8_t p, const uint8_t *code, int n, int cycles, int at,
                      unsigned poll_i, unsigned final_i)
{
    NesCpu c;
    unit_cpu(&c, pc, 0xF0, p);
    unit_code(&c, code, n);
    /* A status byte with I set for PLP and RTI to pull, and a return address. */
    g_mem[0x01F1] = NES_P_I | NES_P_1;
    g_mem[0x01F2] = 0x00;
    g_mem[0x01F3] = 0x90;
    nes_cpu_step(&c);
    const uint64_t want_at = at < 0 ? 0 : CYCLE_BASE + (uint64_t)at;
    if (g_log_n != cycles || c.poll_at != want_at || c.poll_i != poll_i || c.flag_i != final_i) {
        printf("  FAILED %s: %d cycles (want %d), poll_at %" PRIu64 " (want %" PRIu64 "), poll_i %u (want %u),"
               " I %u (want %u)\n",
               what, g_log_n, cycles, c.poll_at, want_at, c.poll_i, poll_i, c.flag_i, final_i);
        g_unit_failures++;
    }
}

#define POLL_CASE(what, pc, p, cycles, at, poll_i, final_i, ...)                                                       \
    do {                                                                                                               \
        const uint8_t code_[] = { __VA_ARGS__ };                                                                       \
        poll_case(what, pc, p, code_, (int)sizeof code_, cycles, at, poll_i, final_i);                                 \
    } while (0)

static void units_poll(void)
{
    /* CLI, SEI, PLP: the old I. */
    POLL_CASE("CLI", 0x8000, NES_P_I, 2, 1, 1, 0, 0x58);
    POLL_CASE("SEI", 0x8000, 0, 2, 1, 0, 1, 0x78);
    POLL_CASE("PLP sets I", 0x8000, 0, 4, 3, 0, 1, 0x28);
    /* RTI: the restored I. */
    POLL_CASE("RTI sets I", 0x8000, 0, 6, 5, 1, 1, 0x40);
    /* Two-cycle instructions: at the end of the opcode fetch. */
    POLL_CASE("NOP", 0x8000, 0, 2, 1, 0, 0, 0xEA);
    POLL_CASE("LDA #", 0x8000, NES_P_I, 2, 1, 1, 1, 0xA9, 0x01);
    /* Before the last cycle, however long. */
    POLL_CASE("LDA abs", 0x8000, 0, 4, 3, 0, 0, 0xAD, 0x00, 0x02);
    POLL_CASE("STA (zp),Y", 0x8000, 0, 6, 5, 0, 0, 0x91, 0x10);
    POLL_CASE("INC abs,X", 0x8000, 0, 7, 6, 0, 0, 0xFE, 0x00, 0x02);
    POLL_CASE("JSR", 0x8000, 0, 6, 5, 0, 0, 0x20, 0x00, 0x90);
    POLL_CASE("PHP", 0x8000, 0, 3, 2, 0, 0, 0x08);
    /* Branches (Z is clear, so BNE is taken): before the operand fetch;
     * not again before the third cycle; again before a fourth. */
    POLL_CASE("BEQ not taken", 0x80F0, 0, 2, 1, 0, 0, 0xF0, 0x02);
    POLL_CASE("BNE taken, same page", 0x80F0, 0, 3, 1, 0, 0, 0xD0, 0x02);
    POLL_CASE("BNE taken, page crossed forward", 0x80F0, 0, 4, 3, 0, 0, 0xD0, 0x20);
    POLL_CASE("BNE taken, page crossed backward", 0x8000, 0, 4, 3, 0, 0, 0xD0, 0xFD);
    /* BRK does not poll. */
    POLL_CASE("BRK", 0x8000, 0, 7, -1, 1, 1, 0x00, 0x00);
}

static void units_misc(void)
{
    NesCpu c;

    /* A JAM opcode halts; a jammed CPU reads $FFFF once per step until reset. */
    unit_cpu(&c, 0x8000, 0xFF, 0);
    CODE(&c, 0x02, 0x55);
    g_mem[0xFFFF] = 0xC0;
    g_mem[0xFFFC] = 0x00;
    g_mem[0xFFFD] = 0xC0;
    nes_cpu_step(&c);
    CHECK(c.jammed == 1 && c.pc == 0x8001 && g_log_n == 11 && c.instructions == 1);
    CHECK(c.poll_at == 0 && c.poll_i == 1);
    reset_bus_log();
    nes_cpu_step(&c);
    nes_cpu_step(&c);
    CHECK(g_log_n == 2 && log_is(0, TX_READ, 0xFFFF, 0xC0) && log_is(1, TX_READ, 0xFFFF, 0xC0));
    CHECK(c.jammed == 1 && c.pc == 0x8001 && c.instructions == 1 && c.cycles == CYCLE_BASE + 13);
    nes_cpu_reset(&c);
    CHECK(c.jammed == 0 && c.pc == 0xC000);

    /* nes_cpu_execute takes an opcode the caller fetched. */
    unit_cpu(&c, 0x8001, 0xFF, 0);
    g_mem[0x8001] = 0x80;
    g_mem[0x8002] = 0xEA;
    nes_cpu_execute(&c, 0xA9);
    CHECK(g_log_n == 1 && c.a == 0x80 && c.flag_n == 1 && c.pc == 0x8002);
    CHECK(c.instructions == 0 && c.interpreted == 0);
    nes_cpu_step(&c);
    CHECK(c.instructions == 1 && c.interpreted == 1);

    /* P: bit 5 reads as 1, B is never stored. */
    nes_cpu_set_p(&c, 0xFF);
    CHECK(nes_cpu_get_p(&c) == 0xEF);
    nes_cpu_set_p(&c, 0x00);
    CHECK(nes_cpu_get_p(&c) == 0x20);

    /* Decimal mode is absent: 0x09 + 0x01 with D set is 0x0A. */
    unit_cpu(&c, 0x8000, 0xFF, NES_P_D);
    CODE(&c, 0x69, 0x01, 0xE9, 0x01);
    c.a = 0x09;
    nes_cpu_step(&c);
    CHECK(c.a == 0x0A && c.flag_d == 1);
    c.a = 0x10;
    c.flag_c = 1;
    nes_cpu_step(&c);
    CHECK(c.a == 0x0F);

    /* The decode table: 151 official opcodes, 12 that jam, and lengths that
     * agree with the addressing modes. */
    int official = 0, jams = 0, writers = 0, beyond_zp = 0;
    for (int op = 0; op < 256; op++) {
        const NesOpInfo *info = nes_opinfo((uint8_t)op);
        CHECK(info->mnemonic && strlen(info->mnemonic) == 3);
        official += nes_op_official((uint8_t)op);
        jams += info->flow == NES_FLOW_JAM;
        writers += nes_op_writes((uint8_t)op);
        beyond_zp += nes_op_writes_beyond_zp((uint8_t)op);
        CHECK(nes_insn_length((uint8_t)op) >= 1 && nes_insn_length((uint8_t)op) <= 3);
        if (info->flow == NES_FLOW_JAM) CHECK(!nes_op_official((uint8_t)op));
    }
    CHECK(official == 151);
    CHECK(jams == 12);
    /* Official: 13 stores and 24 read-modify-write. Unofficial: 4 SAX, 42
     * read-modify-write combinations, and SHA x2, SHX, SHY, TAS. */
    CHECK(writers == 13 + 24 + 4 + 42 + 5);
    /* Without the zero-page forms: 7 stores, 12 RMW, 2 SAX, 30 combinations, 5. */
    CHECK(beyond_zp == 7 + 12 + 2 + 30 + 5);
    CHECK(nes_insn_length(0x00) == 2 && nes_insn_length(0x20) == 3 && nes_insn_length(0x6C) == 3);
    CHECK(nes_opinfo(0x4C)->flow == NES_FLOW_JUMP && nes_opinfo(0x6C)->flow == NES_FLOW_JUMP_INDIRECT);
    CHECK(nes_opinfo(0x20)->flow == NES_FLOW_CALL && nes_opinfo(0x60)->flow == NES_FLOW_RETURN);
    CHECK(nes_opinfo(0x40)->flow == NES_FLOW_RETURN && nes_opinfo(0x00)->flow == NES_FLOW_BREAK);
}

/* SHA, SHX, SHY and TAS store a register ANDed with the operand's high
 * byte plus one, unless RDY was low in the cycle before the write: then the
 * AND drops out (SHY stores Y like STY). The vectors have no halted cycle;
 * AccuracyCoin tests this on hardware with a DMC DMA ("Unofficial
 * Instructions: SH*", error codes 7 to C). A halt anywhere else changes
 * nothing. */
static void units_sh_stall(void)
{
    static const struct {
        uint8_t opcode;
        int length, dummy; /* bytes; the log index of the read before the write */
    } ops[] = { { 0x9C, 3, 3 }, { 0x9E, 3, 3 }, { 0x9F, 3, 3 }, { 0x9B, 3, 3 }, { 0x93, 2, 4 } };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        for (int stall = -1; stall <= ops[i].dummy; stall++) {
            NesCpu c;
            unit_cpu(&c, 0x8000, 0xFD, NES_P_I);
            c.a = 0xF5;
            c.x = ops[i].opcode == 0x9C ? 0x00 : 0xA5;
            c.y = ops[i].opcode == 0x9C ? 0xA5 : 0x00;
            if (ops[i].length == 3) {
                CODE(&c, ops[i].opcode, 0x00, 0x05); /* $0500, index 0 */
            } else {
                CODE(&c, ops[i].opcode, 0x10); /* ($10),Y -> $0500 */
                g_mem[0x10] = 0x00;
                g_mem[0x11] = 0x05;
            }
            g_stall_read = stall;
            nes_cpu_step(&c);
            const uint8_t reg = ops[i].opcode == 0x9C ? c.y : ops[i].opcode == 0x9E ? c.x : (uint8_t)(c.a & c.x);
            const uint8_t want = stall == ops[i].dummy ? reg : (uint8_t)(reg & 0x06u);
            CHECK(g_mem[0x0500] == want);
            CHECK(g_log_n == ops[i].dummy + 2 && g_log[g_log_n - 1].type == TX_WRITE && g_log[g_log_n - 1].addr == 0x0500);
            if (ops[i].opcode == 0x9B) CHECK(c.s == (uint8_t)(c.a & c.x));
        }
    }
}

static int run_units(void)
{
    units_reset();
    units_interrupt();
    units_poll();
    units_misc();
    units_sh_stall();
    printf("CPU units: %d failures\n", g_unit_failures);
    return g_unit_failures ? 1 : 0;
}

/* ------------------------------------------------------------------ */

static int usage(void)
{
    fprintf(stderr, "usage: sst_6502 DIR [--only XX] [--report N]\n       sst_6502 --units\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    int only = -1;
    bool units = false;
    Stats st = { 5, 0, 0, 0 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) only = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) st.report = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--units")) units = true;
        else if (!dir && argv[i][0] != '-') dir = argv[i];
        else return usage();
    }
    if (units) return dir ? usage() : run_units();
    if (!dir || only > 0xFF) return usage();

    int bad_files = 0;
    for (int op = 0; op < 256; op++) {
        if (only >= 0 && op != only) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%02x.json", dir, op);
        int f = run_file(path, op, &st);
        if (f < 0) {
            bad_files++;
            continue;
        }
        if (f > 0) {
            st.files_failed++;
            printf("%02x: %d failures\n", op, f);
        }
    }
    printf("%" PRIu64 " tests, %" PRIu64 " failures, %" PRIu64
           " hardware deviations (LXA, checked against the hardware rule), %d files with failures, %d unreadable files\n",
           st.tests, st.failures, st.deviations, st.files_failed, bad_files);
    if (bad_files || st.tests == 0) return 1;
    return st.failures ? 1 : 0;
}
