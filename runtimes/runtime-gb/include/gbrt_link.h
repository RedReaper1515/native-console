/*
 * Game Boy / Game Boy Color link cable: two games, each in its own
 * process (two windows on one computer, or two computers), joined through
 * the serial port as by a real cable (Pokemon trades and battles, Tetris
 * and other two-player modes).
 *
 * gbrt_link.c emulates SB/SC over a message transport (GBLinkIo) and keeps
 * the two machines in lockstep; gbrt_link_net.c provides the transport over
 * TCP. Without a link the serial port behaves as it always has (a transfer
 * with the internal clock ends with $FF, as with no cable plugged in).
 *
 * Either Game Boy may drive the clock, so the lockstep is symmetric: each
 * machine counts link time in system cycles (4.19 MHz, the host's clock)
 * and runs at most LOOKAHEAD cycles past the last time the other reported.
 * A transfer started at link time T ends at T + D on both machines, where
 * D is its time on the wire but never less than MIN_DURATION > LOOKAHEAD,
 * so the other side always hears of it before it is due to end. At normal
 * clock speed that is the hardware's own timing; the CGB's fast clock and
 * double-speed transfers take a little longer than on hardware. The side
 * that drove the clock finishes once the other side's byte has arrived:
 * the byte in its SB at T if it was waiting with its external clock (SC
 * bit 7 set, bit 0 clear), else $FF.
 *
 * A machine that may not go on stalls: gb_run_cycles returns, and the
 * frontend calls gbrt_link_wait until the other catches up.
 */
#ifndef GBRT_LINK_H
#define GBRT_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GB_LINK_MAX_PLAYERS 2
#define GB_LINK_MAX_MESSAGE 32

enum {
    GB_LINK_EVENT_MESSAGE = 1, /* data[len] from player `from` */
    GB_LINK_EVENT_ROSTER,      /* this machine is player `self` (-1: not connected); `present` = players connected */
};

typedef struct GBLinkEvent {
    int kind;
    int from;
    int self;
    uint8_t present; /* bit n = player n+1 is connected, this machine included */
    uint8_t len;
    uint8_t data[GB_LINK_MAX_MESSAGE];
} GBLinkEvent;

/* A transport that delivers every message to the other player, in order. */
typedef struct GBLinkIo {
    void* ctx;
    void (*send)(void* ctx, const uint8_t* data, size_t len);
    /* Next event, waiting up to timeout_ms for one (0 = don't wait).
     * Returns 1 with *ev filled, 0 when there is none. */
    int (*poll)(void* ctx, int timeout_ms, GBLinkEvent* ev);
} GBLinkIo;

typedef struct GBLink GBLink;

/* Plug a cable in. `self` is the player number the transport already
 * knows (0 for the host), else -1 until it reports one. `local`: both
 * machines are on this computer, so a short lookahead keeps transfers at
 * hardware speed; across a network a longer one hides the latency. */
GBLink* gbrt_link_attach(GBContext* ctx, GBLinkIo io, int self, bool local);
void gbrt_link_detach(GBContext* ctx);

/* The machine may not run until the other catches up. */
bool gbrt_link_stalled(const GBContext* ctx);
/* Wait up to timeout_ms for the other player; true once it can run again. */
bool gbrt_link_wait(GBContext* ctx, int timeout_ms);
/* Handle messages while the game is not running (menus, pacing). */
void gbrt_link_idle(GBContext* ctx, int timeout_ms);

typedef struct GBLinkStatus {
    bool attached;
    int self;           /* player number - 1, or -1 */
    int players;        /* connected, this machine included */
    bool stalled;
    uint64_t transfers; /* bytes exchanged with the other player */
    int64_t time;       /* link time this machine is at */
} GBLinkStatus;
void gbrt_link_status(const GBContext* ctx, GBLinkStatus* out);

/* Identifies the game for the handshake (header title and checksums). */
void gbrt_link_game_id(const GBContext* ctx, char* out, size_t cap);

/* Runtime hooks (gbrt.c). */
void gbrt_link_event(GBContext* ctx);                       /* link_clock reached link_next */
void gbrt_link_serial_written(GBContext* ctx, uint16_t addr); /* SB or SC written, value already stored */
uint32_t gbrt_link_cycles_until_event(const GBContext* ctx); /* CPU cycles the machine may run unchecked */

/* Command line and network (generated main and the frontend's menu):
 *   --link-host            host the cable (player 1)
 *   --link-join HOST[:P]   join a host (player 2)
 *   --link-port N          port (default 47402)
 *   --link-local           host for this computer only
 *   --player N             this window's player profile (saves, settings,
 *                          controller; see platform_sdl.cpp)
 * parse_arg consumes one of these at argv[*i] and returns true. */
bool gbrt_link_parse_arg(int argc, char** argv, int* i);
int gbrt_link_player(void);
bool gbrt_link_start_from_args(GBContext* ctx);
bool gbrt_link_host(GBContext* ctx, uint16_t port, bool local_only, char* err, size_t cap);
bool gbrt_link_join(GBContext* ctx, const char* address);
void gbrt_link_stop(GBContext* ctx);
const char* gbrt_link_net_status(const GBContext* ctx); /* "" without a network link */
bool gbrt_link_net_is_host(const GBContext* ctx);
uint16_t gbrt_link_port(void);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_LINK_H */
