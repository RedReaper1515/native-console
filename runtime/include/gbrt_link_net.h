/*
 * TCP transport for the Game Boy link cable (gbrt_link.h), the same as
 * runtime-gba's gba_net.c but for two players and its own default port.
 *
 * One player hosts; the others join it, on the same computer
 * (127.0.0.1) or across a network. The host is player 1 (the parent);
 * the joiner is player 2 (a Game Boy cable joins two). A joiner must be running the same game
 * (gbrt_link_game_id: the cartridge header); it keeps retrying until the host answers, so the two
 * can be started in either order, and reconnects if the host goes away.
 *
 * Frames on the wire: u8 length (of what follows), u8 origin player
 * (0xFF = control), payload. Control payloads: 'H' version + game id
 * (joiner's hello), 'R' self + players present (roster), 'N' reason text
 * (refused).
 */
#ifndef GBRT_LINK_NET_H
#define GBRT_LINK_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbrt_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GB_NET_DEFAULT_PORT 47402

typedef struct GBNet GBNet;

/* Listen on `port`: every interface, or only this computer's loopback
 * (no firewall prompt, for two windows on one PC). NULL on failure, with
 * the reason in err. */
GBNet *gb_net_host(uint16_t port, bool loopback_only, const char *game_id, char *err, size_t cap);

/* Join "host", "host:port" or "a.b.c.d:port" (IPv4). Connects in the
 * background, from polls. */
GBNet *gb_net_join(const char *address, const char *game_id);

GBLinkIo gb_net_io(GBNet *n);
int gb_net_initial_player(const GBNet *n); /* 0 for the host, -1 for a joiner */
bool gb_net_is_host(const GBNet *n);
/* One line for the menu ("Hosting on port 47400", "Connecting to ...",
 * "Refused: ..."). */
const char *gb_net_status(const GBNet *n);
void gb_net_close(GBNet *n);

/* This computer's IPv4 addresses other players can join, comma separated. */
void gb_net_local_addresses(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_LINK_NET_H */
