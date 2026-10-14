/*
 * gbrt_evidence.h - what the game was seen to do with the jumps its code
 * does not spell out.
 *
 * A JP HL, or a RET that pops an address the code put there itself, goes
 * wherever a register or the stack says. Reading the ROM cannot always tell
 * where that is: a map's script is an address copied out of its header into
 * RAM when the map loads. Playing the game can. With `--evidence FILE` a
 * generated game records every such jump it makes: the instruction, where it
 * went, what the target will return to, and the CALL that entered the
 * function it happened in. `tools/gb_lift.py edges --evidence FILE` turns
 * the record into entry points and call edges (docs/LIFTING.md).
 *
 * The recorder keeps a shadow of the call stack, fed from the helpers every
 * CALL, RST, RET and JP HL goes through, whether the code running is
 * generated or interpreted, so recording costs a few instructions per call
 * and the game runs at its usual speed. Lifted source does not go through
 * those helpers; a game recording evidence runs its recompiled bodies.
 *
 * It records what happened in the run and nothing more: code the player did
 * not reach is not in it.
 */

#ifndef GBRT_EVIDENCE_H
#define GBRT_EVIDENCE_H

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The file a context created after this call writes its evidence to; NULL
 * (the default) records nothing. */
void gbrt_evidence_set_file(const char* path);
bool gbrt_evidence_requested(void);

/* Start recording, if a file was set. False if that fails. */
bool gbrt_evidence_start(GBContext* ctx);
/* The guest's stack is not what it was (a state was loaded, the machine was
 * reset): forget who called whom. What was recorded stays. */
void gbrt_evidence_reset(GBContext* ctx);
/* Write the file. True if nothing was being recorded or it was written. */
bool gbrt_evidence_write(GBContext* ctx);
void gbrt_evidence_destroy(GBContext* ctx);

/* ---- For the runtime: called only while ctx->evidence is set ----------- */

/* A CALL or RST has pushed `return_address` and is going to `target`.
 * `length` is the instruction's, so that return_address - length is it. */
void gbrt_evidence_call(GBContext* ctx, uint16_t target, uint16_t return_address, uint8_t length);
/* An interrupt has pushed `return_address` and is going to `vector`. */
void gbrt_evidence_interrupt(GBContext* ctx, uint16_t vector, uint16_t return_address);
/* A RET, RETI or taken RET cc is about to pop. */
void gbrt_evidence_return(GBContext* ctx);
/* A jump to `target` is about to be made and HL holds the same address. */
void gbrt_evidence_jump(GBContext* ctx, uint16_t target);
/* The interpreter is about to run the instruction at `address`: it advances
 * PC before it acts, where generated code leaves PC on the instruction. */
void gbrt_evidence_interpreting(GBContext* ctx, uint16_t address);
void gbrt_evidence_left_interpreter(GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_EVIDENCE_H */
