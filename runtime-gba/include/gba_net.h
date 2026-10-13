/*
 * TCP transport for the link cable (gba_link.h).
 *
 * One player hosts; the others join it, on the same computer
 * (127.0.0.1) or across a network. The host is player 1 (the parent);
 * joiners get player 2-4 in the order they arrive, and the host passes each
 * one's messages on to the others. A joiner must be running the same game
 * (gba_link_game_id); it keeps retrying until the host answers, so the two
 * can be started in either order, and reconnects if the host goes away.
 *
 * Frames on the wire: u8 length (of what follows), u8 origin player
 * (0xFF = control), payload. Control payloads: 'H' version + game id
 * (joiner's hello), 'R' self + players present (roster), 'N' reason text
 * (refused).
 */
#ifndef GBA_NET_H
#define GBA_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gba_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GBA_NET_DEFAULT_PORT 47400

typedef struct GbaNet GbaNet;

/* Listen on `port`: every interface, or only this computer's loopback
 * (no firewall prompt, for two windows on one PC). NULL on failure, with
 * the reason in err. */
GbaNet *gba_net_host(uint16_t port, bool loopback_only, const char *game_id, char *err, size_t cap);

/* Join "host", "host:port" or "a.b.c.d:port" (IPv4). Connects in the
 * background, from polls. */
GbaNet *gba_net_join(const char *address, const char *game_id);

GbaLinkIo gba_net_io(GbaNet *n);
int gba_net_initial_player(const GbaNet *n); /* 0 for the host, -1 for a joiner */
bool gba_net_is_host(const GbaNet *n);
/* One line for the menu ("Hosting on port 47400", "Connecting to ...",
 * "Refused: ..."). */
const char *gba_net_status(const GbaNet *n);
void gba_net_close(GbaNet *n);

/* This computer's IPv4 addresses other players can join, comma separated. */
void gba_net_local_addresses(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GBA_NET_H */
