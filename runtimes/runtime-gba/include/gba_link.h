/*
 * Link cable: two to four GBAs running the same game in multi-player mode
 * (SIOCNT mode 2, the mode Four Swords, Mario Kart, Pokemon trades and
 * multiboot use), each in its own process, on one computer or several.
 *
 * gba_link.c emulates the serial port and keeps the machines in lockstep
 * over a message transport (GbaLinkIo); gba_net.c provides one over TCP.
 * Without a link attached the serial registers stay plain storage, as they
 * always were.
 *
 * Lockstep: every machine counts "link time" in CPU cycles, the parent's
 * (player 1's) clock. Only the parent starts transfers, and a transfer
 * started at link time T completes at T + D (D = its duration on the wire,
 * several thousand cycles). So a child may run at most LOOKAHEAD cycles
 * (less than any D) past the last time the parent reported: whatever the
 * parent starts, the child hears of it before the transfer is due to end.
 * The parent in turn completes a transfer only once every child's word for
 * it has arrived (a child sends the SIOMLT_SEND value it had at T), and
 * never runs more than two frames ahead of the slowest child. When a
 * machine may not go on, it stalls: the run loop returns (GBA_ATTENTION_STALL)
 * and the frontend calls gba_link_wait until the others catch up.
 *
 * The parent is paced by its own frontend (audio clock); children run as
 * fast as the parent lets them, so they are paced by it.
 */
#ifndef GBA_LINK_H
#define GBA_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gba.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GBA_LINK_MAX_PLAYERS 4
#define GBA_LINK_MAX_MESSAGE 32

enum {
    GBA_LINK_EVENT_MESSAGE = 1, /* data[len] from player `from` */
    GBA_LINK_EVENT_ROSTER,      /* this machine is player `self` (-1: not connected); `present` = players connected */
};

typedef struct GbaLinkEvent {
    int kind;
    int from;
    int self;
    uint8_t present; /* bit n = player n+1 is connected, this machine included */
    uint8_t len;
    uint8_t data[GBA_LINK_MAX_MESSAGE];
} GbaLinkEvent;

/* A transport that delivers every message to every other player, in the
 * order each player sent them. */
typedef struct GbaLinkIo {
    void *ctx;
    void (*send)(void *ctx, const uint8_t *data, size_t len);
    /* Next event, waiting up to timeout_ms for one (0 = don't wait).
     * Returns 1 with *ev filled, 0 when there is none. */
    int (*poll)(void *ctx, int timeout_ms, GbaLinkEvent *ev);
} GbaLinkIo;

typedef struct GbaLink GbaLink;

/* Plug a cable into the machine. `self` is the player number the transport
 * already knows (0 for the host), else -1 until it reports one. */
GbaLink *gba_link_attach(GbaMachine *m, GbaLinkIo io, int self);
/* Unplug and free it; the serial registers are plain storage again. */
void gba_link_detach(GbaMachine *m);

/* While the machine is stalled (m->attention & GBA_ATTENTION_STALL), wait
 * up to timeout_ms for the other players; true once it can run again. */
bool gba_link_wait(GbaLink *l, int timeout_ms);
/* Handle messages while the machine is not running (the frontend's frame
 * pacing, a paused menu): the host relays for the others. Waits up to
 * timeout_ms for the first one. */
void gba_link_idle(GbaLink *l, int timeout_ms);
/* Report how far this machine got (called after each frame). */
void gba_link_frame_done(GbaLink *l);

/* A child with its parent connected: paced by the parent, not by a clock. */
bool gba_link_follows(const GbaLink *l);

typedef struct GbaLinkStatus {
    int self;          /* player number - 1, or -1 */
    int players;       /* connected, this machine included */
    uint8_t present;
    bool stalled;
    int waiting_for;   /* player number - 1 the stall waits on, or -1 */
    uint64_t transfers; /* completed multi-player transfers */
    int64_t time;       /* link time (the parent's cycle count) this machine is at */
} GbaLinkStatus;
void gba_link_status(const GbaLink *l, GbaLinkStatus *out);

/* Identifies the game for the connection handshake: machines only link
 * when this matches (title, game code, version, header checksum, size). */
void gba_link_game_id(const GbaMachine *m, char *out, size_t cap);

/* Duration of one multi-player transfer, in cycles (SIOCNT baud bits). */
uint32_t gba_link_transfer_cycles(int baud, int players);

#ifdef __cplusplus
}
#endif

#endif /* GBA_LINK_H */
