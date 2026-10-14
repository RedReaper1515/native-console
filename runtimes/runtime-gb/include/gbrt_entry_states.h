/**
 * gbrt_entry_states.h - the machine as it was each time a function was
 * entered, for the lift checker.
 *
 * The checker (`tools/gb_lift.py check`, docs/LIFTING.md) runs a function of the
 * ROM and the C written for it from the same state and compares where they
 * end. The states it makes up are random registers and random memory, or a
 * moment of the game's memory with random registers, and a good part of a
 * game's functions get through none of those: a copy whose made-up pointer
 * takes in the cartridge's registers, a routine that wants a pointer to
 * something real in a register.
 *
 * `--entry-states FILE` has a game run in the interpreter and keep the
 * registers and the whole of memory as they were when control arrived at
 * the first instruction of a function: the first time, the second, the
 * fourth, the eighth and so on, and any time it arrived with a return
 * address on the stack not seen for it before, up to a number for each
 * function (`--entry-states-per`, eight unless told). `gb_lift.py check
 * --entries FILE` starts states from those.
 *
 * Which addresses are functions the game does not know. `--entry-states-of
 * LIST` names them, one `BANK:ADDRESS` in hexadecimal a line, which is what
 * `gb_lift.py entries` writes from an export. Without a list, the places a
 * CALL, a RST or an interrupt arrived at are taken, which finds no function
 * that is only ever jumped to.
 *
 * Memory is kept in pages of 256 bytes and a page is stored once however
 * many states have it, so a state costs its registers, a number for each
 * page, and the pages it is the first to have. The format is at the top of
 * gbrt_entry_states.c; `lift/checkers/checker-gb/lift_check.c` reads it.
 *
 * The recorder only reads. A game recorded ends as the same game does
 * recorded with `--data-use`, which runs in the interpreter too.
 */

#ifndef GBRT_ENTRY_STATES_H
#define GBRT_ENTRY_STATES_H

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ask for the states, to be written to `path`. Before the context is made. */
void gbrt_entry_states_set_file(const char* path);
bool gbrt_entry_states_requested(void);

/* The file that lists the functions. Before the context is made; it is read
 * when recording starts, and gbrt_entry_states_start fails if it cannot be. */
void gbrt_entry_states_set_list(const char* path);
bool gbrt_entry_states_list_requested(void);

/* How many states to keep of one function at most. False if `count` is not
 * a number from 1 to 4096. */
bool gbrt_entry_states_set_per(const char* count);

/* Start recording in `ctx` if states were asked for. The game then runs in
 * the interpreter. False if they were asked for and cannot be kept. */
bool gbrt_entry_states_start(GBContext* ctx);

/* Write the file. False if it was asked for and could not be written. */
bool gbrt_entry_states_write(GBContext* ctx);
void gbrt_entry_states_destroy(GBContext* ctx);

/* The interpreter is about to execute the instruction at ctx->pc. */
void gbrt_entry_states_before(GBContext* ctx);

/* An interrupt has pushed where to come back to and is going to its vector:
 * the next instruction is the handler's first. */
void gbrt_entry_states_interrupt(GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_ENTRY_STATES_H */
