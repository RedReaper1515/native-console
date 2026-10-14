/*
 * The Wii's Bluetooth: the controller inside the console, and the remotes
 * at the other end of its radio.
 *
 * A game does not ask IOS what the remotes are doing. It carries a whole
 * Bluetooth stack of its own (Broadcom's, in a retail game; lwbt, in
 * homebrew) and asks IOS for one thing only: the console's internal
 * Bluetooth module, which sits on a USB port of the second processor and is
 * reached as the device /dev/usb/oh1/57e/305. Everything above that - HCI
 * commands, L2CAP channels, the HID reports of a remote - is the game's own
 * code talking to hardware, so that hardware is what has to exist here:
 *
 *   wii_ios_bt.c   the USB device: three endpoints, and the requests a
 *                  game leaves waiting on two of them
 *   wii_bt.c       the module: HCI commands in, events out; the links to
 *                  the remotes; the remotes' side of L2CAP and SDP
 *   wii_remote.c   a remote: reports, memory, extension, camera
 *
 * There is no radio. A remote is "in range" when its input says it is
 * present (wii_remote.h), packets take a fixed time to cross, and time is
 * the machine's processor cycles, so a run is the same every time.
 *
 * Everything below is plain data with no pointers, because WiiState is
 * written whole to a save state.
 */
#ifndef WII_BT_H
#define WII_BT_H

#include <stdint.h>

#include "gc.h"
#include "wii_remote.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Figures a game can see                                              */

/* What the module says it can buffer of what a game sends (Read Buffer
 * Size): the figures a Wii's module gives. */
#define WII_BT_ACL_LENGTH 339u
#define WII_BT_ACL_PACKETS 10u
#define WII_BT_SCO_LENGTH 64u
#define WII_BT_SCO_PACKETS 0u

/* How long things take, as fractions of a second of processor cycles. An
 * event follows its command by about a USB frame; a packet crosses the
 * radio and comes back in a couple of slots' worth; a remote reports every
 * five milliseconds. Nothing correct depends on the exact figures - only on
 * their being the same every run - but they are of the order a console
 * shows, because a game's own timeouts are tuned to those. */
#define WII_BT_EVENT_CYCLES ((uint64_t)GC_CPU_HZ / 1000u)
#define WII_BT_RADIO_CYCLES ((uint64_t)GC_CPU_HZ / 400u)
#define WII_BT_REPORT_CYCLES ((uint64_t)GC_CPU_HZ / 200u)
/* A remote that was refused, or got no answer, tries again after this. */
#define WII_BT_RETRY_CYCLES ((uint64_t)GC_CPU_HZ * 2u)
/* And gives up waiting for an answer to its connection request after this
 * (the default connection accept timeout, about five seconds). */
#define WII_BT_ACCEPT_CYCLES ((uint64_t)GC_CPU_HZ * 5u)

/* ------------------------------------------------------------------ */
/* Queues                                                              */

/* What is waiting to go up an endpoint: HCI events on the interrupt
 * endpoint, ACL data on the bulk one. Each has the cycle it is ready at;
 * the earliest goes first and ties go to whichever was queued first. */
#define WII_BT_EVENT_SLOTS 32u
#define WII_BT_EVENT_BYTES 260u
#define WII_BT_ACL_SLOTS 48u
#define WII_BT_ACL_BYTES 344u

typedef struct WiiBtEvent {
  uint64_t due;
  uint32_t order;
  uint16_t length;
  uint8_t used;
  uint8_t reserved;
  uint8_t data[WII_BT_EVENT_BYTES];
} WiiBtEvent;

typedef struct WiiBtAcl {
  uint64_t due;
  uint32_t order;
  uint16_t length;
  uint8_t used;
  uint8_t reserved;
  uint8_t data[WII_BT_ACL_BYTES];
} WiiBtAcl;

/* ------------------------------------------------------------------ */
/* Links and channels                                                  */

/* One L2CAP channel between the console and a remote. */
enum {
  WII_BT_CHANNEL_CLOSED = 0,
  WII_BT_CHANNEL_ASKED = 1,  /* the remote asked for it; no answer yet */
  WII_BT_CHANNEL_CONFIG = 2, /* connected; options being agreed */
  WII_BT_CHANNEL_OPEN = 3
};

enum {
  WII_BT_CONFIG_SENT = 0x01,     /* the remote has sent its options */
  WII_BT_CONFIG_ACCEPTED = 0x02, /* and the console agreed to them */
  WII_BT_CONFIG_ANSWERED = 0x04  /* the remote agreed to the console's */
};

/* The remote's end of each channel has a fixed number: control, interrupt,
 * and two for whatever else the console opens (service discovery). */
#define WII_BT_CHANNELS 4u
#define WII_BT_CHANNEL_CONTROL 0u
#define WII_BT_CHANNEL_INTERRUPT 1u
#define WII_BT_FIRST_CID 0x0040u

typedef struct WiiBtChannel {
  uint8_t state;
  uint8_t config;     /* WII_BT_CONFIG_* */
  uint8_t request_id; /* of the remote's own request, to match its answer */
  uint8_t reserved;
  uint16_t psm;
  uint16_t host_cid; /* the console's end */
  uint16_t host_mtu; /* the largest packet the console takes on it */
} WiiBtChannel;

enum {
  WII_BT_LINK_NONE = 0,
  WII_BT_LINK_ASKING = 1, /* the remote has asked to connect */
  WII_BT_LINK_PAGED = 2,  /* the console is connecting to the remote */
  WII_BT_LINK_UP = 3
};

enum {
  WII_BT_AUTH_NONE = 0,
  WII_BT_AUTH_KEY = 1, /* waiting for the console's link key */
  WII_BT_AUTH_PIN = 2  /* waiting for a PIN */
};

/* The link to one remote. Its connection handle is fixed by which remote
 * it is. */
#define WII_BT_FIRST_HANDLE 0x0100u

typedef struct WiiBtLink {
  uint8_t state;        /* WII_BT_LINK_* */
  uint8_t remote_began; /* the remote connected to the console, not the reverse */
  uint8_t role;         /* the console's: 0 master, 1 slave */
  uint8_t mode;         /* 0 active, 2 sniff */
  uint8_t auth;         /* WII_BT_AUTH_* */
  uint8_t authenticated;
  uint8_t encrypted;
  uint8_t wants;       /* switched on and looking for the console */
  uint8_t was_present; /* the input's `present` when last looked at */
  uint8_t had_buttons; /* and whether anything was held */
  uint8_t next_id;     /* the remote's next L2CAP signal number */
  uint8_t reserved;
  uint16_t policy;
  uint16_t supervision;
  uint16_t sniff_interval;
  uint16_t flush_timeout;
  uint64_t ask_at;   /* no connection request before this cycle */
  uint64_t asked_at; /* when the last one went out */
  WiiBtChannel channel[WII_BT_CHANNELS];
} WiiBtLink;

/* ------------------------------------------------------------------ */
/* A remote's own state (wii_remote.c)                                 */

#define WII_REMOTE_EEPROM 0x1700u
/* Answers a remote owes: acknowledgements and status reports. */
#define WII_REMOTE_REPLIES 8u

typedef struct WiiRemoteReply {
  uint8_t kind;   /* 0x20 status, 0x22 acknowledgement */
  uint8_t report; /* the report being acknowledged */
  uint8_t error;
  uint8_t reserved;
} WiiRemoteReply;

typedef struct WiiRemoteState {
  uint8_t leds;       /* bit 0 the first light */
  uint8_t rumble;
  uint8_t mode;       /* the data report asked for: 0x30..0x3f */
  uint8_t continuous; /* report whether or not anything changed */
  uint8_t reporting;  /* data reports are being sent */
  uint8_t camera;     /* bit 0 its clock, bit 1 its logic */
  uint8_t speaker;    /* bit 0 enabled, bit 1 muted */
  uint8_t extension;  /* what is plugged in, as the remote last saw */
  uint8_t interleave; /* which half of the interleaved report is next */
  uint8_t last_length;
  uint8_t reply_count;
  uint8_t read_space; /* a read in progress: 0 none, 1 EEPROM, 2 registers */
  uint16_t read_left;
  uint16_t reserved;
  uint32_t read_address;
  uint8_t last_report[24]; /* the last data report sent, to see changes */
  WiiRemoteReply reply[WII_REMOTE_REPLIES];
  uint64_t next_report; /* no report before this cycle */

  /* The register spaces: speaker, extension, camera. */
  uint8_t speaker_reg[0x0a];
  uint8_t camera_reg[0x34];
  uint8_t extension_reg[0x100];
  /* The extension's cipher: on from the moment it is plugged in, and its
   * two tables, made from the sixteen key bytes at 0x40. */
  uint8_t cipher_on;
  uint8_t reserved2[3];
  uint8_t cipher_add[8];
  uint8_t cipher_xor[8];

  uint8_t eeprom[WII_REMOTE_EEPROM];
} WiiRemoteState;

/* ------------------------------------------------------------------ */
/* The module                                                          */

/* Link keys the module remembers, as a console's does for the remotes
 * registered with it. */
#define WII_BT_KEYS 16u

typedef struct WiiBtKey {
  uint8_t used;
  uint8_t reserved;
  uint8_t address[6]; /* as on the wire, least significant byte first */
  uint8_t key[16];
} WiiBtKey;

/* A request waiting on an endpoint: the request block's address (zero when
 * the slot is free), where its buffer is, and how big. */
#define WII_BT_WAITING 4u

typedef struct WiiBtWaiting {
  uint32_t request, buffer, size;
} WiiBtWaiting;

typedef struct WiiBluetooth {
  /* The requests a game leaves waiting on the two inward endpoints, oldest
   * first, and one it may leave to be told the module was unplugged (it
   * never is). */
  WiiBtWaiting intr[WII_BT_WAITING];
  WiiBtWaiting bulk[WII_BT_WAITING];
  uint32_t removal_request;
  uint32_t next_order;
  /* The cycle the last answer on each endpoint was timed for: the next is
   * never earlier, so what was queued in order arrives in order. */
  uint64_t intr_last, bulk_last;

  /* What the stack has set (Write ... commands), kept as the bytes it sent
   * so that the matching Read returns them. */
  uint8_t scan_enable;
  uint8_t inquiry_mode;
  uint8_t inquiry_scan_type;
  uint8_t page_scan_type;
  uint8_t pin_type;
  uint8_t auth_enable;
  uint8_t encryption_mode;
  uint8_t afh_assessment;
  uint8_t page_scan_period;
  uint8_t page_scan_mode;
  uint8_t broadcast_retransmissions;
  uint8_t sco_flow;
  uint8_t host_flow; /* bit 0: the console counts ACL packets back */
  uint8_t iac_count;
  uint8_t class_of_device[3];
  uint8_t iac[6];
  uint8_t page_timeout[2];
  uint8_t accept_timeout[2];
  uint8_t voice[2];
  uint8_t hold_activity[2];
  uint8_t default_policy[2];
  uint8_t page_scan[4];    /* interval, window */
  uint8_t inquiry_scan[4];
  uint8_t event_mask[8];
  uint8_t name[248];
  uint16_t host_acl_length; /* Host Buffer Size */
  uint16_t host_acl_packets;
  uint16_t host_acl_out;    /* sent and not yet counted back */
  /* Set Event Filter: whether the module accepts a connection on its own
   * (and with which role), and which inquiry answers it lets through. */
  uint8_t auto_accept;      /* 0 off, 2 accept, 3 accept and become master */
  uint8_t inquiry_filter;   /* 0 all, 1 by class of device, 2 by address */
  uint8_t inquiry_match[6]; /* class and mask, or the address */

  /* An inquiry: the cycle it ends at (its events are queued when it
   * starts), and periodic inquiry mode, which starts one every so often. */
  uint64_t inquiry_end;
  uint64_t periodic_next;
  uint8_t periodic;
  uint8_t periodic_length;  /* in units of 1.28 s */
  uint8_t periodic_limit;
  uint8_t reserved1;
  uint16_t periodic_period; /* in units of 1.28 s */

  /* A connection the console is making to an address nobody answers at
   * (Create Connection): it fails when the page timeout runs out. */
  uint8_t paging;
  uint8_t reserved2;
  uint8_t paging_address[6];
  uint8_t reserved3[2];
  uint64_t paging_due;

  WiiBtKey keys[WII_BT_KEYS];
  WiiBtLink link[WII_REMOTES];
  WiiRemoteState remote[WII_REMOTES];

  WiiBtEvent events[WII_BT_EVENT_SLOTS];
  WiiBtAcl acl[WII_BT_ACL_SLOTS];

  /* For the end-of-run report and the tests. */
  uint32_t commands;
  uint32_t unknown_commands;
  uint32_t events_sent;
  uint32_t acl_in, acl_out;
  uint32_t dropped;
} WiiBluetooth;

/* The module's state inside the machine, or NULL on a GameCube. */
WiiBluetooth *wii_bluetooth(const GcMachine *m);

/* ------------------------------------------------------------------ */
/* Between the three files                                             */

/* Power on: the module as it leaves the factory, the remotes as they are
 * after their batteries go in. */
void wii_bt_reset(GcMachine *m);
/* An HCI command packet (opcode, length, parameters). */
void wii_bt_command(GcMachine *m, const uint8_t *packet, uint32_t size);
/* An ACL packet from the console (handle and flags, length, data). */
void wii_bt_acl_from_host(GcMachine *m, const uint8_t *packet, uint32_t size);
/* Whatever depends on time or on the remotes' input: connection requests,
 * a remote switched off, an inquiry running out. */
void wii_bt_tick(GcMachine *m);
/* Queue an event (code, length, parameters) or an ACL packet for the
 * console, ready `delay` cycles from now. */
void wii_bt_queue_event(GcMachine *m, const uint8_t *event, unsigned size, uint64_t delay);
void wii_bt_queue_acl(GcMachine *m, const uint8_t *packet, unsigned size, uint64_t delay);
/* The next ACL packet the module could hand over: either one that is
 * queued or a report a remote has ready. Returns zero when there is
 * nothing, else writes the packet and the cycle it would arrive at, which
 * is never before `soonest`; `take` removes it, and is what moves a
 * remote's clock on to its next report. */
unsigned wii_bt_next_acl(GcMachine *m, uint8_t *out, uint64_t soonest, uint64_t *when, int take);

/* Hand the waiting requests whatever is ready for them (wii_ios_bt.c). */
void wii_bt_pump(GcMachine *m);
/* A remote's input has just been set: look at it now rather than at the
 * next field. Nothing calls this yet (see the note in wii_ios_bt.c). */
void wii_bt_input_changed(GcMachine *m);

/* The remote's side (wii_remote.c). */
/* Write the memory a remote leaves the factory with. */
void wii_remote_factory(WiiRemoteState *r);
/* Batteries in, or a connection dropped: everything but the memory. */
void wii_remote_power_on(WiiRemoteState *r);
/* An output report from the console: its number and what follows. */
void wii_remote_output(GcMachine *m, unsigned index, const uint8_t *report, unsigned size);
/* The extension port, looked at: plugging and unplugging are noticed
 * here. */
void wii_remote_poll(GcMachine *m, unsigned index);
/* Whether the remote has an input report to send, and when it may go. */
int wii_remote_ready(GcMachine *m, unsigned index, uint64_t *when);
/* Build that report (its number and what follows) and move on. Returns
 * its length. */
unsigned wii_remote_take(GcMachine *m, unsigned index, uint64_t when, uint8_t *out);
/* The same without moving on, for looking at what would be sent. */
unsigned wii_remote_peek(GcMachine *m, unsigned index, uint8_t *out);

/* The extension's cipher (wii_extension_cipher.c): the two tables that go
 * with the sixteen key bytes a console wrote at 0x40. A byte sent from
 * register address a is (byte - add[a % 8]) ^ xor_[a % 8]. Returns non-zero
 * when the key is one a console's library makes, and zero for any other -
 * sixteen zeros among them - whose tables are made all the same. */
int wii_extension_cipher_tables(const uint8_t key[16], uint8_t add[8], uint8_t xor_[8]);

/* Little-endian fields, which is what Bluetooth puts on the wire. */
static inline uint16_t wii_le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint32_t)p[1] << 8)); }
static inline uint32_t wii_le32(const uint8_t *p) {
  return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wii_put_le16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}
static inline void wii_put_le32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

#ifdef __cplusplus
}
#endif

#endif /* WII_BT_H */
