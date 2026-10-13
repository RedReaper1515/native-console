/**
 * gbrt_executed.h - which bytes of the ROM a played game executed.
 *
 * Nothing in a ROM says which of its bytes are instructions. The analysis
 * decodes what it can reach and what looks like code, and some of what it
 * decodes is a picture, a map or a table that happens to read as
 * instructions. Whether the processor ever went there, only the game shows.
 *
 * `--executed FILE` has a game run in the interpreter and note every byte
 * of the ROM that was part of an instruction it executed: the first byte of
 * the instruction, and the bytes after it. Together with what a game was
 * seen to read as data (`--data-use`), that sorts what the analysis calls a
 * function three ways: it ran, it was read and never ran, or the game did
 * neither with it. `tools/gb_lift.py executed` does the sorting.
 *
 * It is a record of what was played: a byte not in it was not executed in
 * that run, which is not to say that it never is.
 *
 * The file, numbers little-endian:
 *
 *   "GBEXECUT", u32 version (1)
 *   u32 rom_size
 *   u16 global_checksum    the ROM's, from its header
 *   u16 zero
 *   u64 instructions       executed from the ROM while recording
 *   rom_size bytes, one for each byte of the ROM:
 *     bit 0  an executed instruction began here
 *     bit 1  it was a later byte of an executed instruction
 *
 * The recorder only reads. A game recorded ends as the same game does
 * recorded with `--data-use`, which runs in the interpreter too.
 */

#ifndef GBRT_EXECUTED_H
#define GBRT_EXECUTED_H

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GBRT_EXECUTED_BEGAN 0x01u
#define GBRT_EXECUTED_WITHIN 0x02u

/* Ask for the map, to be written to `path`. Before the context is made. */
void gbrt_executed_set_file(const char* path);
bool gbrt_executed_requested(void);

/* Start recording in `ctx` if a map was asked for. The game then runs in
 * the interpreter. False if it was asked for and cannot be kept. */
bool gbrt_executed_start(GBContext* ctx);

/* Write the file. False if it was asked for and could not be written. */
bool gbrt_executed_write(GBContext* ctx);
void gbrt_executed_destroy(GBContext* ctx);

/* The interpreter is about to execute the instruction at ctx->pc. */
void gbrt_executed_before(GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_EXECUTED_H */
