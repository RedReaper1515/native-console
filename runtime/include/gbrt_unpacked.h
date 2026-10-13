/**
 * gbrt_unpacked.h - what a game's own unpacking routine makes, kept and
 * replaced.
 *
 * Packed data (a compressed picture, mostly) cannot be edited where it
 * lies: the bytes in the ROM are the packer's, and writing new ones takes
 * the packer, which no game ships. The game does ship the other half. Its
 * unpacking routine turns each packed stream into the same bytes every
 * time, somewhere in RAM, and those bytes are the picture.
 *
 * So the routine is left to run, to its end, every time. Told which routine
 * it is, where its stream starts as it is entered and where its output lies
 * as it returns (`tools/gb_data.py`, DATA_FILES.md), this module does two
 * things at the routine's return:
 *
 * - `--unpacked-harvest FILE`: it keeps what the routine made, once for each
 *   different thing made from each stream, and writes it all out when the
 *   game stops. That is how the pictures are got out.
 * - `--unpacked FILE`, with a pack that has replacements in it: where what
 *   the routine made is, by its hash, exactly what a replacement was drawn
 *   from, the replacement is written over it. That is how edited pictures
 *   get in. Anything else the routine makes is left alone.
 *
 * Nothing is skipped and no time is taken or given: the routine spends the
 * cycles it always spent, and the bytes change between its last instruction
 * and its return. The ROM is not touched, so this goes with `--rom-image`
 * and with save states.
 */

#ifndef GBRT_UNPACKED_H
#define GBRT_UNPACKED_H

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ask for a pack to be loaded (`--unpacked`), and for what the routines make
 * to be written out (`--unpacked-harvest`). Before the context is started. */
void gbrt_unpacked_set_pack(const char* path);
void gbrt_unpacked_set_harvest(const char* path);
bool gbrt_unpacked_requested(void);

/* Load the pack that was asked for into `ctx`. `built_from` is the ROM the
 * game was compiled from, which the pack has to be of. False, with the
 * reason in `message`, if a pack was asked for and cannot be used, or a
 * harvest was asked for without one. True and nothing done if neither was. */
bool gbrt_unpacked_start(GBContext* ctx, const uint8_t* built_from, size_t size,
                         char* message, size_t message_size);

/* How many replacements the pack loaded into `ctx` has: 0 with no pack, and
 * with one that only says what to watch. Where there are any, `sha256` gets
 * what tells them from any others: it is the same for the same bytes put in
 * place of the same things made by routines entered, fed and writing at the
 * same places, whatever the routines are called, whatever else the pack
 * watches, and in whatever order it lists them. A recorded run is of the
 * game with these replacements and of no other (gbrt_run.h). */
size_t gbrt_unpacked_replacements(const GBContext* ctx, uint8_t sha256[32]);

/* Write the harvest. False if one was asked for and could not be written. */
bool gbrt_unpacked_write(GBContext* ctx);
/* Say on stderr how often each routine was entered, came back, and had what
 * it made replaced. */
void gbrt_unpacked_report(GBContext* ctx);
void gbrt_unpacked_destroy(GBContext* ctx);
/* The machine was replaced by a save state: a routine half way through
 * belonged to the old one. */
void gbrt_unpacked_reset(GBContext* ctx);

/* A function of the game called for it (`--unpacked-call`), so that a
 * routine the pack names unpacks a stream nobody has played as far as. The
 * argument is the function, `BANK:ADDRESS` in hexadecimal, and after it,
 * each behind a comma, what to set before it is entered, in this order:
 * `a=0D` (a b c d e h l), `de=4000` (bc de hl), `D0AA=01` (a byte of RAM,
 * or below 8000 a write to the cartridge, which is how a bank is mapped),
 * `ime=1` (interrupts on, or 0 for off). False if it is not that.
 *
 * The call is made once the game has started and any save state is loaded:
 * a return address is pushed and the function entered, in the machine as it
 * then is. The game stops as the first routine the pack names returns, when
 * what it made has been kept, so nothing depends on where the function would
 * have gone back to. It also stops if the function itself comes back first,
 * or after `cycles` of waiting (`--unpacked-call-cycles`). A game not given
 * the option does none of this. */
bool gbrt_unpacked_set_call(const char* spec);
void gbrt_unpacked_set_call_cycles(uint64_t cycles);
bool gbrt_unpacked_call_requested(void);
/* Make the call. False, with the reason in `message`, if it cannot be made. */
bool gbrt_unpacked_call_start(GBContext* ctx, char* message, size_t message_size);
/* For the loop that runs the machine: how many cycles it may run before
 * the call is looked at again, and whether the call is over. */
uint32_t gbrt_unpacked_call_slice(GBContext* ctx, uint32_t budget);
bool gbrt_unpacked_call_over(GBContext* ctx);
/* Say on stderr how the call ended. True if a routine the pack names
 * returned, which is the only end that made anything. */
bool gbrt_unpacked_call_report(GBContext* ctx);

/* Control is arriving at `target` by a call, a restart or a jump, with the
 * stack as the routine there will find it. Called where ctx->unpacked is
 * set. */
void gbrt_unpacked_arrive(GBContext* ctx, uint16_t target);
/* A return is about to pop the stack at or above where a watched routine was
 * entered. Called where ctx->unpacked_watch says so (gbrt_unpacked_returning). */
void gbrt_unpacked_return(GBContext* ctx);

/* The test every return makes: is a watched routine's frame at, or below,
 * the word about to be popped? unpacked_watch is that frame's stack pointer
 * plus one, and nought while no routine is being watched. */
static inline bool gbrt_unpacked_returning(const GBContext* ctx) {
    return ctx->unpacked_watch != 0u && (uint32_t)ctx->sp + 1u >= ctx->unpacked_watch;
}

#ifdef __cplusplus
}
#endif

#endif /* GBRT_UNPACKED_H */
