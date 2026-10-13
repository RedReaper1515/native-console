/*
 * gbrt_lift.h - running lifted source inside a generated game.
 *
 * LIFTING.md describes lifted source: C written against gblift.h that does
 * what one subroutine of the game does. A game generated with
 * `gbrecomp --lift` carries such source. Where the recompiled code reaches a
 * lifted subroutine's first instruction it calls gbrt_lift_enter() and the C
 * runs in place of the recompiled body, which stays in the executable as the
 * fallback.
 *
 * Lifted C is an ordinary function and cannot return to the scheduler half
 * way through the way recompiled code does, yet a subroutine it calls may
 * wait for the next frame. So lifted code runs on a coroutine: when the frame
 * ends inside one of its callees the coroutine is left where it is, the main
 * loop presents the frame as usual, and gb_run_cycles() goes back into the
 * coroutine instead of dispatching.
 */

#ifndef GBRT_LIFT_H
#define GBRT_LIFT_H

#include "gbrt.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef GBRT_ENABLE_LIFT

typedef struct GBLiftBinding {
    uint16_t bank;                    /* ROM bank of the subroutine, 0 for the home bank */
    uint16_t address;                 /* its entry address */
    void (*function)(GBContext* ctx); /* the lifted source */
    const char* name;
} GBLiftBinding;

typedef struct GBLiftCounters {
    uint64_t calls;         /* lifted functions entered */
    uint64_t guest_calls;   /* times lifted code ran guest code: lift_call, an interrupt, HALT */
    uint64_t yields;        /* times a frame ended inside lifted code */
    uint64_t abandoned;     /* coroutines dropped by a state load */
    uint64_t lifted_cycles; /* guest time that passed in lifted code itself */
    uint64_t idle_cycles;   /* guest time the CPU spent in HALT or STOP */
    uint32_t deepest;       /* most lifted functions active at once */
} GBLiftCounters;

/* Give a context its lifted functions. The table must outlive the context. */
void gbrt_lift_install(GBContext* ctx, const GBLiftBinding* bindings, size_t count);

/* Signatures (--lift-signatures FILE, LIFTING.md): what each lifted function
 * gives back and what it leaves as it came. With a file loaded, a function
 * that returns to its caller has every register and flag it keeps put back
 * as it came, and every one it neither keeps nor gives set to a value that
 * is nobody's (lift_undefined). A game whose signatures are right plays as
 * it does without them; one that reads a register a signature calls
 * undefined goes another way, which a run compared with the original shows.
 * The file is a line a function, `BBBB:AAAA TAKES GIVES KEEPS`, all
 * hexadecimal, the last three as the LIFT_REG_ and LIFT_FLAG_ bits of
 * gblift.h. `set_file` before the functions are installed; `load` after,
 * false (with a message) if the file was asked for and cannot be used. */
void gbrt_lift_set_signatures_file(const char* path);
bool gbrt_lift_signatures_requested(void);
bool gbrt_lift_load_signatures(GBContext* ctx);

/* Callers (--lift-callers FILE, LIFTING.md): where each lifted function
 * returned to while the game was played. Assembly does not list who calls a
 * function that is reached through a table or an address in data; a played
 * game does. Noted when a function is back with its caller: the address it
 * returned to with the ROM bank that was mapped there when it was called,
 * and, where a lifted function was running around it, that function and
 * where it in turn was to return (so that a function called through a
 * bank-switching helper is known by who called the helper). The game is not
 * changed by it. A line a pairing in the file:
 * `CALLEE SITE WITHIN WITHIN_SITE COUNT`, each of the first four BBBB:AAAA
 * in hexadecimal, WITHIN and WITHIN_SITE `-` where there is none. `set_file`
 * before the game runs; `write` when it stops. */
void gbrt_lift_set_callers_file(const char* path);
bool gbrt_lift_callers_requested(void);
bool gbrt_lift_write_callers(const GBContext* ctx);

/* Off: every subroutine runs its recompiled body, as if nothing were lifted. */
void gbrt_lift_set_enabled(GBContext* ctx, bool enabled);
bool gbrt_lift_enabled(const GBContext* ctx);

/*
 * Generated code calls this where a lifted subroutine begins. True: the
 * lifted code has run, or is waiting on its coroutine; either way return to
 * the dispatcher. False: run the recompiled body.
 */
bool gbrt_lift_enter(GBContext* ctx, const GBLiftBinding* binding);

/* For the dispatcher loop: true when it must stop dispatching, because guest
 * code that lifted code called has come back to it, or because the coroutine
 * owns the machine. */
bool gbrt_lift_gate_slow(GBContext* ctx);
static inline bool gbrt_lift_gate(GBContext* ctx) {
    return ctx->lift_gate && gbrt_lift_gate_slow(ctx);
}

/* For the interpreter: true when it must hand back to the dispatcher, for
 * the reasons above or because it has reached a lifted subroutine. */
bool gbrt_lift_leave_interpreter(GBContext* ctx, uint32_t instructions_executed);

/* For gb_run_cycles(): go back into a waiting coroutine. False if none waits. */
bool gbrt_lift_resume(GBContext* ctx);

/* Drop a waiting coroutine: the guest state it belonged to is gone (a state
 * was loaded, the machine was reset). Execution carries on in recompiled code. */
void gbrt_lift_reset(GBContext* ctx);
void gbrt_lift_destroy(GBContext* ctx);

/* For the scheduler: the CPU sat in HALT or STOP for this long. */
void gbrt_lift_note_idle(GBContext* ctx, uint32_t cycles);

const GBLiftCounters* gbrt_lift_counters(const GBContext* ctx);
void gbrt_lift_report(const GBContext* ctx);

/* ---- Between gbrt.c and gbrt_lift.c ------------------------------------ */

/* Run the scheduler on the coroutine until gbrt_lift_watch_hit(). In gbrt.c,
 * beside gb_run_cycles(), whose turn it shares. */
void gbrt_lift_run_guest(GBContext* ctx);
/* True where gb_run_cycles() would stop: the frame is done, the slice is
 * spent, the link cable is waiting. */
bool gbrt_lift_should_yield(GBContext* ctx);
bool gbrt_lift_watch_hit(const GBContext* ctx);
/* Leave the coroutine for the main loop. False if there is no coroutine to
 * leave, in which case the caller carries on. */
bool gbrt_lift_yield(GBContext* ctx);

#endif /* GBRT_ENABLE_LIFT */

#ifdef __cplusplus
}
#endif

#endif /* GBRT_LIFT_H */
