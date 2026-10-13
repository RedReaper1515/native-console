/**
 * gbrt_control.h - the game played by a program.
 *
 * `--input` presses buttons somebody wrote down before the game started. A
 * program that plays has to look first: where the player stands, whose turn
 * it is, what the menu's cursor is on. With `--control PORT` a generated
 * game connects to 127.0.0.1:PORT, where the program listens, and from then
 * on stops at the start of every frame. Stopped, it answers what it is
 * asked: the bytes at an address, a save state written or loaded. It runs
 * on when it is told which buttons are held and for how many frames, and
 * stops again at the frame after them.
 *
 * The buttons are buttons like a player's. A run recorded with
 * `--record-run` while a program plays has every one of them in its record
 * and replays with no program there (RUN_RECORDS.md). What would not be in
 * the record is refused while one is recorded or replayed: a byte written
 * into the machine, a state loaded.
 *
 * What goes over the connection, either way, is messages of one shape: a
 * letter, the length of what follows in four bytes, and that many bytes.
 * Numbers are little endian.
 *
 * From the game:
 *
 *   H  version:u32 "gb"            once, on connecting
 *   F  frame:u64 cycle:u64 flags:u8 bytes...
 *                                  the game has stopped at the start of a
 *                                  frame; `bytes` is what is watched, in
 *                                  the order it was asked for. flags: 1 a
 *                                  run is being recorded, 2 replayed
 *   M  bytes...                    the answer to `r`
 *   K                              done
 *   E  text                        not done, and why
 *   X  frame:u64 cycle:u64         the game is closing
 *
 * To the game, while it is stopped:
 *
 *   w  (address:u16 length:u16)... what every F is to carry from now on -> K
 *   r  address:u16 length:u32      read -> M
 *   p  address:u16 bytes...        write, into RAM only -> K
 *   s  path                        write a save state -> K
 *   l  path                        load a save state -> K
 *   q                              -> F, as the machine now is
 *   g  buttons:u8 frames:u32       run on -> F after that many frames, or X
 *   e                              close the game -> X
 *
 * `buttons` has a bit for each button held: 1 A, 2 B, 4 Select, 8 Start,
 * 16 Right, 32 Left, 64 Up, 128 Down. While a run is replayed the record's
 * buttons are the ones the game sees.
 *
 * A read is of what the byte holds and changes nothing: no register is
 * brought up to date for it, so the video and sound registers are as the
 * runtime last left them (gbrt_run.h says why they may lag). Cartridge
 * memory reads whether or not the game has it switched on.
 *
 * `tools/gb_control.py` is the other end, for a program written in Python.
 */

#ifndef GBRT_CONTROL_H
#define GBRT_CONTROL_H

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GBRT_CONTROL_VERSION 1u

/* The port a game started after this call connects to; 0 (the default) for
 * a game nobody controls. */
void gbrt_control_set_port(unsigned port);
bool gbrt_control_requested(void);

/* Connect and say hello, if a port was set. After the game's init and a
 * state loaded, and before a run is started, so that a game nobody answers
 * leaves no record. False, with the reason on stderr, if nobody is
 * listening. */
bool gbrt_control_start(GBContext* ctx);

/* What to do while the program is being waited for, every few
 * milliseconds: keep a window alive. It returns false when the game is to
 * close. */
void gbrt_control_set_idle(bool (*idle)(GBContext* ctx));

/* At the start of a frame. Returns at once while frames the program asked
 * for are still to run; otherwise tells the program the game has stopped
 * and answers it until it says to run on. `dpad` and `buttons` are what is
 * held from here, active low as the joypad register has them. False when
 * the game is to close: the program said so, or is gone. */
bool gbrt_control_frame(GBContext* ctx, uint8_t* dpad, uint8_t* buttons);

/* When the game stops: tell the program. False if the program went away
 * while the game was its to play. */
bool gbrt_control_finish(GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_CONTROL_H */
