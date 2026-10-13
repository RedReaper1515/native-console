/**
 * gbrt_trace_back.h - which instruction wrote a byte of RAM, and what the
 * value it wrote was made of.
 *
 * `--data-use` follows the bytes of a ROM forward, to the screen and the
 * speaker. What it cannot find is the data and the code that only ever steer
 * a game: a formula, the tables it looks numbers up in, the constants inside
 * its instructions. Those are found the other way round: watch a variable,
 * and ask of each write to it where the value came from.
 *
 * `--trace-back FILE --trace-back-of ADDRESS[-ADDRESS]` has a game run in the
 * interpreter and keep, for every store to RAM, an event: the instruction
 * that stored, the address, the value, and the sources the value was made
 * of. A source is a byte of the ROM read as data, a constant in an
 * instruction, a hardware register that was read, an earlier event, or RAM as
 * it was when the trace began. Each source says how it took part: as a
 * value (it was copied or reckoned into the result), as an address (the
 * value was loaded through it, which is how the variable that picked a
 * table's entry is kept), or as a decision (a conditional jump, call or
 * return tested it earlier in the same call, so it had a say in whether the
 * store was made at all, and not in what was stored).
 *
 * Each time a watched byte is written the record gets the event, the calls
 * that were in progress, and the events its sources lead back to, followed
 * to a depth and a number of nodes. `tools/gb_trace_back.py` prints that as
 * a tree.
 *
 * The limits are real and the record states them. A set of sources holds
 * eight: what does not fit is dropped, decisions first, and the event says
 * that something was. Events live in a ring: one that has been written over
 * is "forgotten". Decisions over-approximate (everything a call does after a
 * comparison is taken to depend on it) and under-approximate (a call does
 * not inherit its caller's). And it is a record of one run.
 *
 * The trace only reads: a game ends in the same state with it as without.
 */

#ifndef GBRT_TRACE_BACK_H
#define GBRT_TRACE_BACK_H

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ask for a record, to be written to `path`. Before the context is made. */
void gbrt_trace_back_set_file(const char* path);
bool gbrt_trace_back_requested(void);

/* An option of the record, by the part of its flag after `--trace-back-`:
 *   of      ADDRESS or ADDRESS-ADDRESS, hexadecimal: bytes to watch (a few ranges)
 *   depth   how many sources deep a tree is followed
 *   nodes   how many events one tree may take
 *   trees   how many trees the record holds
 *   each    how many trees one writing instruction, with one set of calls around it, gets
 *   memory  megabytes of events remembered, or kilobytes with a k after the number
 * False if `name` is none of those or `value` is not what it takes. */
bool gbrt_trace_back_option(const char* name, const char* value);

/* Whether any byte was asked to be watched. */
bool gbrt_trace_back_watching(void);

/* Start tracing in `ctx` if a record was asked for. The game then runs in
 * the interpreter. False if it was asked for and cannot be kept. */
bool gbrt_trace_back_start(GBContext* ctx);

/* Write the record. False if it was asked for and could not be written. */
bool gbrt_trace_back_write(GBContext* ctx);
void gbrt_trace_back_destroy(GBContext* ctx);

/* A save state was loaded: what the trace knew of the machine is gone. The
 * record so far stays. */
void gbrt_trace_back_reset(GBContext* ctx);

/* The instruction at ctx->pc is about to run: in the interpreter, or in one
 * of its stubs for code in RAM. Called once for every instruction. */
void gbrt_trace_back_before(GBContext* ctx);

/* An interrupt has pushed where to come back to (ctx->pc): the stack pointer
 * is already two lower. */
void gbrt_trace_back_interrupt(GBContext* ctx);

/* The hardware is copying `count` bytes from `source` to `target` by itself:
 * the sprite table from a page of memory, or a block into video memory. */
void gbrt_trace_back_copied(GBContext* ctx, uint16_t source, uint16_t target, unsigned count);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_TRACE_BACK_H */
