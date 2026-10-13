/*
 * gbrt_lift.c - lifted source inside a generated game: gblift.h as the
 * runtime provides it, and the coroutine lifted code runs on.
 * gbrt_lift.h says why there is a coroutine.
 */

#if !defined(_WIN32)
/* ucontext is an XSI extension, and macOS hides it without this. */
#define _XOPEN_SOURCE 700
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#endif

#include "gbrt_lift.h"
#include "gbrt_unpacked.h"

#ifdef GBRT_ENABLE_LIFT

/* gblift.h on the runtime's own GBContext. */
#ifndef GBLIFT_USE_RUNTIME_CONTEXT
#define GBLIFT_USE_RUNTIME_CONTEXT
#endif
#include "gblift.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define GBRT_LIFT_COROUTINE 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__ANDROID__) || defined(__EMSCRIPTEN__)
/* No makecontext here. Lifted code still runs; a frame that ends inside it
 * is presented late (see gbrt_lift_yield). */
#define GBRT_LIFT_COROUTINE 0
#else
#define GBRT_LIFT_COROUTINE 1
#if defined(__APPLE__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <ucontext.h>
#endif

#define GBRT_LIFT_STACK_BYTES (4u * 1024u * 1024u)

/* One lifted function in progress. They nest: lifted code calls guest code,
 * which reaches another lifted function. */
typedef struct GBLiftActivation {
    uint16_t return_pc;   /* the caller's return address... */
    uint16_t return_sp;   /* ...and SP once it has been popped */
    struct GBLiftActivation* outer;
    const GBLiftBinding* binding;
    uint16_t return_bank; /* the ROM bank that address is in (--lift-callers only) */
    bool by_jump;         /* arrived at by a jump lifted code made as a call (lift_call_pushed) */
} GBLiftActivation;

/* Where a lifted function returned to (--lift-callers). */
typedef struct GBLiftCaller {
    uint32_t callee;      /* index of its binding, plus one; 0: an empty slot */
    uint32_t within;      /* index of the binding running around it, plus one; 0: none */
    uint16_t bank, pc;
    uint16_t within_bank, within_pc;
    uint64_t count;
} GBLiftCaller;

#define GBRT_LIFT_CALLER_SLOTS (1u << 17)
static const char* g_callers_path; /* --lift-callers: where to write them */

/* What a function is held to (--lift-signatures): bits as in gblift.h. */
typedef struct GBLiftSignature {
    uint8_t held;
    uint16_t gives;
    uint16_t keeps;
} GBLiftSignature;

typedef struct GBLiftRuntime {
    GBContext* ctx;
    const GBLiftBinding* bindings;
    size_t binding_count;
    GBLiftSignature* signatures; /* one a binding, or NULL: none loaded */
    GBLiftCaller* callers;       /* GBRT_LIFT_CALLER_SLOTS of them, or NULL: not recording */
    uint32_t callers_used;
    uint64_t callers_dropped;
    bool jumping_as_call;        /* lift_call_pushed is about to run guest code */
    uint8_t entry_bits[0x2000];  /* one bit per address: a lifted function begins here */
    bool enabled;

    /* The coroutine */
    const GBLiftBinding* task;   /* the function it was started for */
    bool active;                 /* that function has not finished */
    bool on_coroutine;           /* the coroutine is what is running now */
    bool suspended;              /* it is waiting for gbrt_lift_resume() */
    bool created;
#if GBRT_LIFT_COROUTINE && defined(_WIN32)
    LPVOID main_fiber;
    LPVOID fiber;
#elif GBRT_LIFT_COROUTINE
    ucontext_t main_context;
    ucontext_t context;
    void* stack;
#endif

    GBLiftActivation* top;
    uint32_t depth;

    /* Where guest code called from lifted code hands back: up to two
     * (pc, sp) pairs, the call's own return and the lifted function's. */
    uint16_t watch_pc[2];
    uint16_t watch_sp[2];
    uint8_t watch_count;

    GBLiftCounters counters;
} GBLiftRuntime;

static GBLiftRuntime* runtime_of(const GBContext* ctx) {
    return ctx ? (GBLiftRuntime*)ctx->lift_state : NULL;
}

static void update_gate(GBLiftRuntime* rt) {
    rt->ctx->lift_gate = (uint8_t)(rt->watch_count != 0 || rt->suspended);
}

/* The registers and flags a signature speaks of, in the order of their bits. */
#define GBRT_LIFT_ITEMS 11u

static uint8_t* signature_item(GBContext* ctx, unsigned item) {
    switch (item) {
        case 0: return &ctx->a;
        case 1: return &ctx->b;
        case 2: return &ctx->c;
        case 3: return &ctx->d;
        case 4: return &ctx->e;
        case 5: return &ctx->h;
        case 6: return &ctx->l;
        case 7: return &ctx->f_z;
        case 8: return &ctx->f_n;
        case 9: return &ctx->f_h;
        default: return &ctx->f_c;
    }
}

/* The ROM bank an address of code is in, as things are mapped now. */
static uint16_t bank_of(const GBContext* ctx, uint16_t pc) {
    if (pc < 0x4000u) return 0;
    return pc < 0x8000u ? gb_resolve_rom_bank(ctx, pc) : 0xFFFFu;
}

static void note_caller(GBLiftRuntime* rt, const GBLiftActivation* activation) {
    const GBLiftActivation* outer = activation->outer;
    GBLiftCaller wanted;
    memset(&wanted, 0, sizeof(wanted));
    wanted.callee = (uint32_t)(activation->binding - rt->bindings) + 1u;
    wanted.bank = activation->return_bank;
    wanted.pc = activation->return_pc;
    /* Who was running around it matters where it was jumped to as a call:
     * it then returns into that function, which is a helper, and what is
     * read afterwards depends on who called the helper. */
    if (outer && activation->by_jump) {
        wanted.within = (uint32_t)(outer->binding - rt->bindings) + 1u;
        wanted.within_bank = outer->return_bank;
        wanted.within_pc = outer->return_pc;
    }
    uint32_t slot = (wanted.callee * 2654435761u) ^ ((uint32_t)wanted.bank << 16 | wanted.pc) ^
                    (wanted.within * 40503u) ^ ((uint32_t)wanted.within_bank << 11) ^ wanted.within_pc;
    for (uint32_t probes = 0; probes < GBRT_LIFT_CALLER_SLOTS; ++probes, ++slot) {
        GBLiftCaller* entry = &rt->callers[slot & (GBRT_LIFT_CALLER_SLOTS - 1u)];
        if (entry->callee == 0) {
            if (rt->callers_used >= GBRT_LIFT_CALLER_SLOTS / 4u * 3u) break;
            *entry = wanted;
            entry->count = 1;
            rt->callers_used++;
            return;
        }
        if (entry->callee == wanted.callee && entry->within == wanted.within && entry->bank == wanted.bank &&
            entry->pc == wanted.pc && entry->within_bank == wanted.within_bank &&
            entry->within_pc == wanted.within_pc) {
            entry->count++;
            return;
        }
    }
    rt->callers_dropped++;
}

static void run_function(GBLiftRuntime* rt, GBContext* ctx, const GBLiftBinding* binding) {
    GBLiftActivation activation;
    activation.return_pc = gb_read16(ctx, ctx->sp);
    activation.return_sp = (uint16_t)(ctx->sp + 2u);
    activation.outer = rt->top;
    activation.binding = binding;
    activation.return_bank = rt->callers ? bank_of(ctx, activation.return_pc) : 0;
    activation.by_jump = rt->jumping_as_call;
    rt->jumping_as_call = false;
    rt->top = &activation;
    rt->counters.calls++;
    if (++rt->depth > rt->counters.deepest) rt->counters.deepest = rt->depth;

    const GBLiftSignature* signature = rt->signatures ? &rt->signatures[binding - rt->bindings] : NULL;
    uint8_t came[GBRT_LIFT_ITEMS];
    if (signature && signature->held) {
        for (unsigned item = 0; item < GBRT_LIFT_ITEMS; ++item) came[item] = *signature_item(ctx, item);
    }

    binding->function(ctx);

    if (rt->callers && ctx->pc == activation.return_pc && ctx->sp == activation.return_sp) {
        note_caller(rt, &activation);
    }
    /* Back with the caller, and only then: a function that went somewhere
     * else has left the registers as that code wants them. */
    if (signature && signature->held && ctx->pc == activation.return_pc && ctx->sp == activation.return_sp) {
        unsigned nobodys = 0;
        for (unsigned item = 0; item < GBRT_LIFT_ITEMS; ++item) {
            const unsigned bit = 1u << item;
            if (signature->keeps & bit) *signature_item(ctx, item) = came[item];
            else if (!(signature->gives & bit)) nobodys |= bit;
        }
        if (nobodys) lift_undefined(ctx, nobodys);
    }

    rt->top = activation.outer;
    rt->depth--;
}

/* ---- The coroutine ----------------------------------------------------- */

static void coroutine_body(GBLiftRuntime* rt);

#if GBRT_LIFT_COROUTINE && defined(_WIN32)

static VOID CALLBACK coroutine_entry(LPVOID parameter) {
    coroutine_body((GBLiftRuntime*)parameter);
}

static bool coroutine_create(GBLiftRuntime* rt) {
    if (rt->created) return true;
    rt->fiber = CreateFiberEx(64u * 1024u, GBRT_LIFT_STACK_BYTES, FIBER_FLAG_FLOAT_SWITCH,
                              coroutine_entry, rt);
    rt->created = rt->fiber != NULL;
    return rt->created;
}

static void coroutine_switch_in(GBLiftRuntime* rt) {
    rt->main_fiber = IsThreadAFiber() ? GetCurrentFiber()
                                      : ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    SwitchToFiber(rt->fiber);
}

static void coroutine_switch_out(GBLiftRuntime* rt) {
    SwitchToFiber(rt->main_fiber);
}

static void coroutine_destroy(GBLiftRuntime* rt) {
    if (rt->created) DeleteFiber(rt->fiber);
    rt->fiber = NULL;
    rt->created = false;
}

#elif GBRT_LIFT_COROUTINE

static GBLiftRuntime* g_starting;  /* makecontext takes no pointer portably */

static void coroutine_entry(void) {
    coroutine_body(g_starting);
}

static bool coroutine_create(GBLiftRuntime* rt) {
    if (rt->created) return true;
    rt->stack = malloc(GBRT_LIFT_STACK_BYTES);
    if (!rt->stack || getcontext(&rt->context) != 0) {
        free(rt->stack);
        rt->stack = NULL;
        return false;
    }
    rt->context.uc_stack.ss_sp = rt->stack;
    rt->context.uc_stack.ss_size = GBRT_LIFT_STACK_BYTES;
    rt->context.uc_link = NULL;
    g_starting = rt;
    makecontext(&rt->context, coroutine_entry, 0);
    rt->created = true;
    return true;
}

static void coroutine_switch_in(GBLiftRuntime* rt) {
    g_starting = rt;
    swapcontext(&rt->main_context, &rt->context);
}

static void coroutine_switch_out(GBLiftRuntime* rt) {
    swapcontext(&rt->context, &rt->main_context);
}

static void coroutine_destroy(GBLiftRuntime* rt) {
    free(rt->stack);
    rt->stack = NULL;
    rt->created = false;
}

#else

static bool coroutine_create(GBLiftRuntime* rt) {
    (void)rt;
    return false;
}

static void coroutine_switch_in(GBLiftRuntime* rt) {
    (void)rt;
}

static void coroutine_switch_out(GBLiftRuntime* rt) {
    (void)rt;
}

static void coroutine_destroy(GBLiftRuntime* rt) {
    (void)rt;
}

#endif

static void coroutine_body(GBLiftRuntime* rt) {
    for (;;) {
        run_function(rt, rt->ctx, rt->task);
        rt->active = false;
        rt->on_coroutine = false;
        update_gate(rt);
        coroutine_switch_out(rt);
    }
}

/* ---- For the generated code and the scheduler --------------------------- */

void gbrt_lift_install(GBContext* ctx, const GBLiftBinding* bindings, size_t count) {
    if (!ctx) return;
    GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt) {
        rt = (GBLiftRuntime*)calloc(1, sizeof(GBLiftRuntime));
        if (!rt) return;
        rt->ctx = ctx;
        rt->enabled = true;
        ctx->lift_state = rt;
    }
    rt->bindings = bindings;
    rt->binding_count = count;
    memset(rt->entry_bits, 0, sizeof(rt->entry_bits));
    for (size_t i = 0; i < count; ++i) {
        rt->entry_bits[bindings[i].address >> 3] |= (uint8_t)(1u << (bindings[i].address & 7u));
    }
    ctx->lift_entries = count ? rt->entry_bits : NULL;
    if (g_callers_path && !rt->callers) {
        rt->callers = (GBLiftCaller*)calloc(GBRT_LIFT_CALLER_SLOTS, sizeof(GBLiftCaller));
    }
}

void gbrt_lift_set_enabled(GBContext* ctx, bool enabled) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (rt) rt->enabled = enabled;
}

bool gbrt_lift_enabled(const GBContext* ctx) {
    const GBLiftRuntime* rt = runtime_of(ctx);
    return rt && rt->enabled && rt->binding_count != 0;
}

bool gbrt_lift_enter(GBContext* ctx, const GBLiftBinding* binding) {
    GBLiftRuntime* rt = runtime_of(ctx);
    /* Single-stepping (the differential runner, a debugger) compares
     * instruction by instruction, which only the recompiled body offers. */
    if (!rt || !rt->enabled || ctx->single_step_mode) return false;

    ctx->pc = binding->address;
    if (rt->on_coroutine) {
        run_function(rt, ctx, binding);
        return true;
    }
    if (rt->active) return false;  /* a coroutine is waiting; the gate keeps us out of here */

    if (!coroutine_create(rt)) {
        /* No coroutine on this platform: run it on this stack. */
        rt->on_coroutine = true;
        run_function(rt, ctx, binding);
        rt->on_coroutine = false;
        return true;
    }
    rt->task = binding;
    rt->active = true;
    rt->on_coroutine = true;
    coroutine_switch_in(rt);
    /* Back here because the function finished, or because a frame ended
     * inside it. In the second case the dispatcher has to let go. */
    if (rt->suspended) ctx->stopped = 1;
    return true;
}

bool gbrt_lift_watch_hit(const GBContext* ctx) {
    const GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt || ctx->halted || ctx->stop_mode_active) return false;
    for (uint8_t i = 0; i < rt->watch_count; ++i) {
        if (ctx->pc == rt->watch_pc[i] && ctx->sp == rt->watch_sp[i]) return true;
    }
    return false;
}

bool gbrt_lift_gate_slow(GBContext* ctx) {
    const GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt) return false;
    if (rt->suspended) return true;
    return gbrt_lift_watch_hit(ctx);
}

static const GBLiftBinding* binding_at(const GBLiftRuntime* rt, uint16_t bank, uint16_t address) {
    for (size_t i = 0; i < rt->binding_count; ++i) {
        if (rt->bindings[i].address == address && rt->bindings[i].bank == bank) {
            return &rt->bindings[i];
        }
    }
    return NULL;
}

bool gbrt_lift_leave_interpreter(GBContext* ctx, uint32_t instructions_executed) {
    if (gbrt_lift_gate(ctx)) return true;
    const uint16_t pc = ctx->pc;
    if (!ctx->lift_entries || !(ctx->lift_entries[pc >> 3] & (1u << (pc & 7u)))) return false;
    /* The dispatcher sent the interpreter here for this very address: then
     * it has no recompiled code for it, and handing back would loop. */
    if (instructions_executed <= 1 || pc >= 0x8000 || ctx->single_step_mode) return false;
    const GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt->enabled) return false;
    const uint16_t bank = pc < 0x4000 ? 0 : gb_resolve_rom_bank(ctx, pc);
    return binding_at(rt, bank, pc) != NULL;
}

bool gbrt_lift_yield(GBContext* ctx) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt || !rt->on_coroutine || !rt->created) return false;
    rt->counters.yields++;
    rt->suspended = true;
    rt->on_coroutine = false;
    update_gate(rt);
    coroutine_switch_out(rt);
    /* gbrt_lift_resume() has put the flags back. */
    return true;
}

bool gbrt_lift_resume(GBContext* ctx) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt || !rt->suspended) return false;
    rt->suspended = false;
    rt->on_coroutine = true;
    update_gate(rt);
    coroutine_switch_in(rt);
    if (rt->suspended) ctx->stopped = 1;
    return true;
}

void gbrt_lift_reset(GBContext* ctx) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt || rt->on_coroutine) return;
    if (rt->active) {
        /* The stack of lifted functions it holds belongs to a guest state
         * that no longer exists. The return addresses on the new guest
         * stack lead into the recompiled bodies, which take over. */
        coroutine_destroy(rt);
        rt->counters.abandoned++;
    }
    rt->active = false;
    rt->suspended = false;
    rt->top = NULL;
    rt->depth = 0;
    rt->watch_count = 0;
    update_gate(rt);
}

void gbrt_lift_set_callers_file(const char* path) {
    g_callers_path = path;
}

bool gbrt_lift_callers_requested(void) {
    return g_callers_path != NULL;
}

bool gbrt_lift_write_callers(const GBContext* ctx) {
    const GBLiftRuntime* rt = runtime_of(ctx);
    if (!g_callers_path) return true;
    if (!rt || !rt->callers) {
        fprintf(stderr, "[LIFT] --lift-callers: nothing was noted (no lifted functions, or no memory)\n");
        return false;
    }
    FILE* file = fopen(g_callers_path, "w");
    if (!file) {
        fprintf(stderr, "[LIFT] cannot write the callers to %s\n", g_callers_path);
        return false;
    }
    fprintf(file, "# gbrt.lift-callers 1: CALLEE SITE WITHIN WITHIN_SITE COUNT\n");
    for (uint32_t i = 0; i < GBRT_LIFT_CALLER_SLOTS; ++i) {
        const GBLiftCaller* entry = &rt->callers[i];
        if (entry->callee == 0) continue;
        const GBLiftBinding* callee = &rt->bindings[entry->callee - 1u];
        fprintf(file, "%04X:%04X %04X:%04X ", callee->bank, callee->address, entry->bank, entry->pc);
        if (entry->within) {
            const GBLiftBinding* within = &rt->bindings[entry->within - 1u];
            fprintf(file, "%04X:%04X %04X:%04X", within->bank, within->address, entry->within_bank,
                    entry->within_pc);
        } else {
            fprintf(file, "- -");
        }
        fprintf(file, " %llu\n", (unsigned long long)entry->count);
    }
    if (rt->callers_dropped) {
        fprintf(file, "# %llu returns were not noted: the table was full\n",
                (unsigned long long)rt->callers_dropped);
    }
    const bool good = fclose(file) == 0;
    fprintf(stderr, "[LIFT] callers: %u pairings of a function and where it returned to%s\n", rt->callers_used,
            rt->callers_dropped ? ", and some not noted: the table was full" : "");
    return good;
}

static const char* g_signatures_path;

void gbrt_lift_set_signatures_file(const char* path) {
    g_signatures_path = path;
}

bool gbrt_lift_signatures_requested(void) {
    return g_signatures_path != NULL;
}

bool gbrt_lift_load_signatures(GBContext* ctx) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (!g_signatures_path) return true;
    if (!rt) {
        fprintf(stderr, "[LIFT] --lift-signatures: this game has no lifted functions\n");
        return false;
    }
    FILE* file = fopen(g_signatures_path, "r");
    if (!file) {
        fprintf(stderr, "[LIFT] cannot read the signatures in %s\n", g_signatures_path);
        return false;
    }
    GBLiftSignature* signatures = (GBLiftSignature*)calloc(rt->binding_count ? rt->binding_count : 1u,
                                                           sizeof(GBLiftSignature));
    if (!signatures) {
        fclose(file);
        return false;
    }
    char line[128];
    unsigned number = 0, held = 0, unbound = 0;
    bool good = true;
    while (good && fgets(line, sizeof(line), file)) {
        unsigned bank, address, takes, gives, keeps;
        number++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (sscanf(line, "%4x:%4x %3x %3x %3x", &bank, &address, &takes, &gives, &keeps) != 5 ||
            (gives & keeps) != 0 || ((gives | keeps) & ~0x7FFu) != 0) {
            fprintf(stderr, "[LIFT] %s:%u is no signature (BBBB:AAAA TAKES GIVES KEEPS, hexadecimal)\n",
                    g_signatures_path, number);
            good = false;
            break;
        }
        const GBLiftBinding* binding = NULL;
        for (size_t i = 0; i < rt->binding_count; ++i) {
            if (rt->bindings[i].address == address && rt->bindings[i].bank == bank) {
                binding = &rt->bindings[i];
                break;
            }
        }
        if (!binding) {
            unbound++; /* a function the game runs the ROM's code for */
            continue;
        }
        GBLiftSignature* signature = &signatures[binding - rt->bindings];
        signature->held = 1;
        signature->gives = (uint16_t)gives;
        signature->keeps = (uint16_t)keeps;
        held++;
    }
    fclose(file);
    if (!good) {
        free(signatures);
        return false;
    }
    free(rt->signatures);
    rt->signatures = signatures;
    fprintf(stderr, "[LIFT] signatures: %u of %llu lifted functions held to one (%u more in the file are not "
                    "lifted functions of this game)\n",
            held, (unsigned long long)rt->binding_count, unbound);
    return true;
}

void gbrt_lift_destroy(GBContext* ctx) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt) return;
    if (!rt->on_coroutine) coroutine_destroy(rt);
    free(rt->signatures);
    free(rt->callers);
    ctx->lift_state = NULL;
    ctx->lift_entries = NULL;
    ctx->lift_gate = 0;
    free(rt);
}

void gbrt_lift_note_idle(GBContext* ctx, uint32_t cycles) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (rt) rt->counters.idle_cycles += cycles;
}

const GBLiftCounters* gbrt_lift_counters(const GBContext* ctx) {
    const GBLiftRuntime* rt = runtime_of(ctx);
    return rt ? &rt->counters : NULL;
}

void gbrt_lift_report(const GBContext* ctx) {
    const GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt) return;
    /* Of the time the CPU was running (not waiting in HALT or STOP), how
     * much passed in lifted code. Time a lifted function does not count
     * (source written without lift_tick) is not in either number. */
    const uint64_t total = ctx->total_cycles;
    const uint64_t running = total > rt->counters.idle_cycles ? total - rt->counters.idle_cycles : 0;
    const unsigned permille =
        running ? (unsigned)((rt->counters.lifted_cycles * 1000u) / running) : 0u;
    fprintf(stderr,
            "[LIFT] functions=%llu enabled=%d calls=%llu guest_calls=%llu frame_waits=%llu "
            "deepest=%u abandoned=%llu lifted_cycles=%llu running_cycles=%llu lifted_share=%u.%u%%\n",
            (unsigned long long)rt->binding_count, rt->enabled ? 1 : 0,
            (unsigned long long)rt->counters.calls,
            (unsigned long long)rt->counters.guest_calls,
            (unsigned long long)rt->counters.yields, rt->counters.deepest,
            (unsigned long long)rt->counters.abandoned,
            (unsigned long long)rt->counters.lifted_cycles, (unsigned long long)running,
            permille / 10u, permille % 10u);
}

/* ---- gblift.h ----------------------------------------------------------- */

/* Run guest code from ctx->pc until it is back at (pc, sp), or back with the
 * caller of the lifted function that is running. */
static void run_guest(GBLiftRuntime* rt, GBContext* ctx, uint16_t pc, uint16_t sp) {
    const uint8_t saved_count = rt->watch_count;
    const uint16_t saved_pc[2] = {rt->watch_pc[0], rt->watch_pc[1]};
    const uint16_t saved_sp[2] = {rt->watch_sp[0], rt->watch_sp[1]};

    rt->watch_pc[0] = pc;
    rt->watch_sp[0] = sp;
    rt->watch_count = 1;
    if (rt->top) {
        rt->watch_pc[1] = rt->top->return_pc;
        rt->watch_sp[1] = rt->top->return_sp;
        rt->watch_count = 2;
    }
    update_gate(rt);
    rt->counters.guest_calls++;

    gbrt_lift_run_guest(ctx);

    rt->watch_pc[0] = saved_pc[0];
    rt->watch_pc[1] = saved_pc[1];
    rt->watch_sp[0] = saved_sp[0];
    rt->watch_sp[1] = saved_sp[1];
    rt->watch_count = saved_count;
    update_gate(rt);
}

uint8_t lift_read8(GBContext* ctx, uint16_t address) {
    return gb_read8(ctx, address);
}

void lift_write8(GBContext* ctx, uint16_t address, uint8_t value) {
    gb_write8(ctx, address, value);
}

uint8_t* lift_ram(GBContext* ctx, uint16_t address) {
    if (address >= 0xC000u && address < 0xD000u) return &ctx->wram[address - 0xC000u];
    if (address >= 0xD000u && address < 0xE000u) {
        return &ctx->wram[((uint32_t)ctx->wram_bank * 0x1000u) + (address - 0xD000u)];
    }
    if (address >= 0xFF80u && address < 0xFFFFu) return &ctx->hram[address - 0xFF80u];
    return NULL;
}

/* A value that is nobody's: not what the register held on the way in, which
 * is often what a caller would have wanted, and not zero. */
void lift_undefined(GBContext* ctx, unsigned what) {
    if (what & LIFT_REG_A) ctx->a = 0xA5u;
    if (what & LIFT_REG_B) ctx->b = 0xA5u;
    if (what & LIFT_REG_C) ctx->c = 0xA5u;
    if (what & LIFT_REG_D) ctx->d = 0xA5u;
    if (what & LIFT_REG_E) ctx->e = 0xA5u;
    if (what & LIFT_REG_H) ctx->h = 0xA5u;
    if (what & LIFT_REG_L) ctx->l = 0xA5u;
    if (what & LIFT_FLAG_Z) ctx->f_z = 0;
    if (what & LIFT_FLAG_N) ctx->f_n = 1;
    if (what & LIFT_FLAG_H) ctx->f_h = 1;
    if (what & LIFT_FLAG_C) ctx->f_c = 1;
}

/* What a function says of itself for the checker (gblift.h). The game has
 * nothing to do with it. */
void lift_own_stack(GBContext* ctx) {
    (void)ctx;
}

static void service(GBContext* ctx);

static int call_with_return_pushed(GBContext* ctx, uint16_t address, uint16_t return_address, bool by_jump) {
    GBLiftRuntime* rt = runtime_of(ctx);
    const uint16_t frame = (uint16_t)(ctx->sp + 2u);
    ctx->pc = address;
    /* Lifted source calls, jumps and returns here and not through the
     * helpers of gbrt.c, so what watches those is told here as well. */
    if (ctx->unpacked) gbrt_unpacked_arrive(ctx, address);
    rt->jumping_as_call = by_jump;
    run_guest(rt, ctx, return_address, frame);
    rt->jumping_as_call = false;
    if (!(ctx->pc == return_address && ctx->sp == frame)) return 1;
    /* The callee's last instruction may have ended the frame or raised an
     * interrupt. Recompiled code would be back with the scheduler now. */
    if (ctx->stopped) service(ctx);
    return 0;
}

int lift_call_pushed(GBContext* ctx, uint16_t address, uint16_t return_address) {
    return call_with_return_pushed(ctx, address, return_address, true);
}

int lift_call(GBContext* ctx, uint16_t address, uint16_t return_address) {
    gb_push16(ctx, return_address);
    return call_with_return_pushed(ctx, address, return_address, false);
}

/* The dispatcher carries on wherever PC points once the lifted function has
 * returned, so leaving the function is no more than setting PC. */
void lift_jump(GBContext* ctx, uint16_t address) {
    if (ctx->unpacked) gbrt_unpacked_arrive(ctx, address);
    ctx->pc = address;
}

void lift_ret(GBContext* ctx) {
    if (gbrt_unpacked_returning(ctx)) gbrt_unpacked_return(ctx);
    ctx->pc = gb_pop16(ctx);
}

/* As gbrt_timed_reti() ends: IME on, and a source that is already pending
 * is taken before the instruction at the restored PC. */
void lift_reti(GBContext* ctx) {
    if (gbrt_unpacked_returning(ctx)) gbrt_unpacked_return(ctx);
    ctx->pc = gb_pop16(ctx);
    ctx->ime = 1;
    ctx->ime_pending = 0;
    if (ctx->io[0x0F] & ctx->io[0x80] & 0x1Fu) ctx->stopped = 1;
}

/* What the scheduler does between two instructions, for lifted code: let a
 * finished frame out, and take a pending interrupt. */
static void service(GBContext* ctx) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt || !rt->on_coroutine) return;
    for (;;) {
        /* Without a coroutine there is nowhere to go; the frame is late. */
        if (gbrt_lift_should_yield(ctx)) (void)gbrt_lift_yield(ctx);
        if (!(ctx->ime && (ctx->io[0x0F] & ctx->io[0x80] & 0x1Fu))) break;
        /* The handler returns to the address it finds on the stack, which
         * is ctx->pc: where the original would be, if the lifted code keeps
         * it up to date, and the function's first instruction if not. */
        const uint16_t resume = ctx->pc;
        const uint16_t sp = ctx->sp;
        gb_handle_interrupts(ctx);
        run_guest(rt, ctx, resume, sp);
        /* The handler's RETI may have found the next one waiting. */
    }
    if (!gbrt_lift_should_yield(ctx)) ctx->stopped = 0;
}

void lift_tick(GBContext* ctx, uint32_t cycles) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (rt) rt->counters.lifted_cycles += cycles;
    gb_tick(ctx, cycles);
    if (ctx->stopped) service(ctx);
}

void lift_advance(GBContext* ctx, uint32_t cycles) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (rt) rt->counters.lifted_cycles += cycles;
    gb_tick(ctx, cycles);
}

void lift_ei(GBContext* ctx) {
    ctx->ime_pending = 1;
}

void lift_di(GBContext* ctx) {
    ctx->ime = 0;
    ctx->ime_pending = 0;
}

/* HALT and STOP hand the machine to the scheduler until the CPU runs again,
 * and then until the interrupt that woke it has been handled. */
static void wait_for_wake(GBContext* ctx) {
    GBLiftRuntime* rt = runtime_of(ctx);
    if (!rt || !rt->on_coroutine) return;
    run_guest(rt, ctx, ctx->pc, ctx->sp);
}

void lift_halt(GBContext* ctx) {
    gbrt_execute_halt(ctx, ctx->pc, 4);
    /* The HALT bug repeats the next opcode byte, which lifted code has no
     * way to do; the next instruction simply runs once. */
    ctx->halt_bug = 0;
    wait_for_wake(ctx);
    if (ctx->stopped) service(ctx);
}

void lift_stop(GBContext* ctx) {
    gb_stop(ctx);
    gb_tick(ctx, 4);
    wait_for_wake(ctx);
    service(ctx);
}

#else

/* Keeps the translation unit from being empty when lifting is not built. */
typedef int gbrt_lift_unused;

#endif /* GBRT_ENABLE_LIFT */
