/**
 * gbrt_data_use.h - what a game was seen to do with the bytes of its ROM
 * that are not code.
 *
 * A ROM is code and data, and nothing in it says what the data is: which
 * bytes are pictures, which are text, which are music, which are addresses.
 * The game knows, and shows it: bytes it copies to the tile area of video
 * memory are pictures, bytes that end up in the tile map are text or a map,
 * bytes written to the sound registers are music, and two bytes it jumps
 * through are the address of code.
 *
 * `--data-use FILE` has a game run in the interpreter and follow every byte
 * it reads from the ROM as data: through the registers, through RAM and the
 * stack, to wherever it is written or used. The byte itself may arrive (a
 * copy), or a value worked out from it (decompression, a letter plus an
 * offset); the record says which. It also says which instruction read the
 * byte first.
 *
 * What is followed is one byte at a time, and where two bytes meet in an
 * instruction the result carries one of them. That is enough to say what a
 * region is for; it is not a proof of where every bit went. And it is a
 * record of one run: what the game did not do in it is not there.
 *
 * `--data-use-follow BANK:ADDRESS` adds, for one instruction that reads, which
 * functions were running when it read. A routine that unpacks reads its packed
 * bytes through one instruction, and none of them arrives anywhere as itself,
 * so the record says only that they were read. What says more is the calls
 * that were in progress: the function whose every call reads one run of bytes
 * from its first to its last is the one that unpacks, and how it was entered
 * says where it is told to read and what it leaves in RAM. The record gets a
 * `followed` section with that; `tools/gb_data.py unpackers` reads it.
 *
 * `--data-use-pointers` adds which two bytes of the ROM made each address the
 * game read its ROM through. Data that is to be moved has to be found again
 * by everything that reads it, and what reads it is told where it is by a
 * pointer: two bytes in a table, or the operand of an instruction. A value is
 * followed as one of those bytes for as long as only plain numbers are added
 * to it, which is what stepping along and indexing do. The record gets a
 * `pointers` section: the pairs of bytes that made addresses, with the lowest
 * and the highest byte read through each, for every byte that was read
 * whether it was reached through such a pair, and which bytes the game
 * compared or reckoned with: a pointer that is a number to the game as well
 * cannot be turned to another place. `tools/gb_data.py pointers` reads it.
 *
 * A tile is colour numbers, and what colour a number is the game's palettes
 * say, which are set while it runs. So the recorder also looks at the screen
 * once a frame: for the tiles that lie in video memory as the ROM has them it
 * notes which layer drew each, and through which palette. The record's
 * `shown` has that, and `tools/gb_data.py tiles` draws a picture in it.
 *
 * The same look says what a tile map's bytes are. For each ROM byte that
 * lies in a cell of a tile map that was on the screen, `shown` has how it
 * lay to the bytes before it in the ROM, which is how wide a screen or a
 * block of tiles is, and which ROM tile its number drew.
 * `tools/gb_data.py maps` makes grids of tile numbers of that.
 *
 * `--data-use-tiles` adds where in a tile each byte landed that reached the
 * tile area as it is. A picture of such bytes has to be told which of them
 * is the first of a tile and whether a tile is 16 of them or 8, and where a
 * byte landed against where the byte before it did says both. The record
 * gets a `tile_runs` section with that; `tools/gb_data.py uses` and `tiles`
 * read it. The rest of the record is what it is without the option.
 */

#ifndef GBRT_DATA_USE_H
#define GBRT_DATA_USE_H

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ask for a record, to be written to `path`. Before the context is made. */
void gbrt_data_use_set_file(const char* path);
bool gbrt_data_use_requested(void);

/* Ask for the calls around one reading instruction to be followed as well:
 * `where` is BANK:ADDRESS in hexadecimal, as `gb_data.py uses` lists the
 * instructions that read. Before the context is made; a few can be asked for.
 * False if `where` is not that, or too many were asked for. It needs a record
 * to be written into: gbrt_data_use_start fails if none was asked for. */
bool gbrt_data_use_follow(const char* where);
bool gbrt_data_use_follow_requested(void);

/* Ask for where tiles land to be recorded as well. Before the context is
 * made. It needs a record too: gbrt_data_use_start fails if none was asked
 * for. */
void gbrt_data_use_tiles(void);
bool gbrt_data_use_tiles_requested(void);

/* Start recording in `ctx` if a record was asked for. The game then runs in
 * the interpreter. False if it was asked for and cannot be kept. */
bool gbrt_data_use_start(GBContext* ctx);

/* Write the record. False if it was asked for and could not be written. */
bool gbrt_data_use_write(GBContext* ctx);
void gbrt_data_use_destroy(GBContext* ctx);

/* The interpreter is about to execute the instruction at ctx->pc. */
void gbrt_data_use_before(GBContext* ctx);

/* An interrupt has pushed where to come back to: that is no data. */
void gbrt_data_use_interrupt(GBContext* ctx);

/* The hardware is copying `count` bytes from `source` to `target` by
 * itself: the sprite table from a page of memory, or a block into video
 * memory. Called where the runtime starts the copy, whatever code asked. */
void gbrt_data_use_copied(GBContext* ctx, uint16_t source, uint16_t target, unsigned count);

/* Ask for the record to say as well which two bytes of the ROM made each
 * address the ROM was read through. Before the context is made. It needs a
 * record to be written into: gbrt_data_use_start fails if none was asked for. */
void gbrt_data_use_pointers_set(bool wanted);
bool gbrt_data_use_pointers_requested(void);

/* The instruction at ctx->pc is about to run: in the interpreter, or in one
 * of its stubs for code in RAM, which gbrt_data_use_before is not told of. */
void gbrt_data_use_pointers_before(GBContext* ctx);

/* The PPU has drawn a whole frame. The recorder looks at it: which tiles
 * drew a pixel, in the background or as a sprite, and through which
 * palette, and which cells of the tile maps were on the screen. For the
 * bytes of those tiles and cells that lie in video memory as the ROM has
 * them, the record's `shown` says so. Nothing of the game is changed. */
void gbrt_data_use_frame(GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_DATA_USE_H */
