/*
 * The console's Bluetooth module and the remotes' end of the radio
 * (wii_bt.h).
 *
 * Three layers, each the least a game's own stack needs the other side to
 * be:
 *
 *   HCI    the module. A command comes in, one or more events go out:
 *          Command Complete for something done at once, Command Status
 *          then a later event for something that takes the radio. The
 *          references are the Bluetooth Core Specification (the command
 *          and event layouts are its; version 2.0 is what the module
 *          implements) and the two stacks that drive it: lwbt's hci.c for
 *          what homebrew sends and reads back, and Broadcom's BTE as
 *          doldecomp/ogws carries it (Apache-2.0) for what a retail game
 *          does - stack/btm, stack/btu, stack/hcic.
 *   L2CAP  each remote's signalling: the two HID channels it opens when it
 *          reconnects to its console, the same two when the console opens
 *          them instead, and a channel for service discovery.
 *   SDP    the remote's service records: what a console reads when it
 *          pairs with a remote it has not met.
 *
 * A remote here is one that has been registered with this console already
 * - its address is in the console's list and the module holds its link key
 * - because that is the state of every remote a player picks up. Pressing a
 * button on such a remote makes it page the console, so that is what a
 * present remote does: the module raises a Connection Request, the stack
 * accepts, and the remote opens its control and interrupt channels. The
 * other direction (inquiry, Create Connection, a PIN) works too, for
 * software that finds its remotes by looking.
 *
 * Nothing here is taken from an emulator. The link keys, the calibration
 * and the service records are written from the specifications and from
 * published descriptions of a remote; the numbers that identify the module
 * and the remote (version words, feature bits, class of device) are what
 * the hardware reports, as public dumps of both give them.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "wii.h"

WiiBluetooth *wii_bluetooth(const GcMachine *m) {
  WiiState *w = wii_state(m);
  return w ? &w->bluetooth : NULL;
}

/* HCI's error codes (Core Specification, volume 2, part D). */
enum {
  HCI_OK = 0x00,
  HCI_UNKNOWN_COMMAND = 0x01,
  HCI_NO_CONNECTION = 0x02,
  HCI_PAGE_TIMEOUT = 0x04,
  HCI_AUTH_FAILURE = 0x05,
  HCI_KEY_MISSING = 0x06,
  HCI_ALREADY_CONNECTED = 0x0b,
  HCI_DISALLOWED = 0x0c,
  HCI_INVALID_PARAMETERS = 0x12,
  HCI_REMOTE_USER_ENDED = 0x13,
  HCI_REMOTE_POWER_OFF = 0x15,
  HCI_LOCAL_HOST_ENDED = 0x16
};

/* Event codes. */
enum {
  EVT_INQUIRY_COMPLETE = 0x01,
  EVT_INQUIRY_RESULT = 0x02,
  EVT_CONNECTION_COMPLETE = 0x03,
  EVT_CONNECTION_REQUEST = 0x04,
  EVT_DISCONNECTION_COMPLETE = 0x05,
  EVT_AUTH_COMPLETE = 0x06,
  EVT_REMOTE_NAME = 0x07,
  EVT_ENCRYPTION_CHANGE = 0x08,
  EVT_CHANGE_LINK_KEY_COMPLETE = 0x09,
  EVT_MASTER_LINK_KEY_COMPLETE = 0x0a,
  EVT_REMOTE_FEATURES = 0x0b,
  EVT_REMOTE_VERSION = 0x0c,
  EVT_QOS_SETUP = 0x0d,
  EVT_COMMAND_COMPLETE = 0x0e,
  EVT_COMMAND_STATUS = 0x0f,
  EVT_ROLE_CHANGE = 0x12,
  EVT_COMPLETED_PACKETS = 0x13,
  EVT_MODE_CHANGE = 0x14,
  EVT_RETURN_LINK_KEYS = 0x15,
  EVT_PIN_REQUEST = 0x16,
  EVT_LINK_KEY_REQUEST = 0x17,
  EVT_LINK_KEY_NOTIFICATION = 0x18,
  EVT_CLOCK_OFFSET = 0x1c,
  EVT_PACKET_TYPE_CHANGED = 0x1d,
  EVT_INQUIRY_RESULT_RSSI = 0x22,
  EVT_REMOTE_EXTENDED_FEATURES = 0x23
};

/* What the hardware says it is. The module is a Broadcom BCM2045:
 * Bluetooth 2.0, and a firmware revision whose low twelve bits (167) are
 * what a retail game looks at to decide the module needs its patch. The
 * remote is a Broadcom BCM2042: Bluetooth 1.2. */
#define MODULE_HCI_VERSION 0x03u
#define MODULE_HCI_REVISION 0x40a7u
#define MODULE_LMP_VERSION 0x03u
#define MODULE_LMP_SUBVERSION 0x430eu
#define BROADCOM 0x000fu
static const uint8_t module_features[8] = {0xff, 0xff, 0x8d, 0xfe, 0x9b, 0xf9, 0x00, 0x80};
#define REMOTE_LMP_VERSION 0x02u
#define REMOTE_LMP_SUBVERSION 0x0229u
static const uint8_t remote_features[8] = {0xbc, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00};
/* Peripheral, joystick. */
static const uint8_t remote_class[3] = {0x04, 0x25, 0x00};

/* ------------------------------------------------------------------ */
/* Queues                                                              */

void wii_bt_queue_event(GcMachine *m, const uint8_t *event, unsigned size, uint64_t delay) {
  WiiBluetooth *bt = wii_bluetooth(m);
  unsigned i;
  if (size > WII_BT_EVENT_BYTES) size = WII_BT_EVENT_BYTES;
  for (i = 0; i < WII_BT_EVENT_SLOTS; i++) {
    WiiBtEvent *e = &bt->events[i];
    if (e->used) continue;
    e->used = 1;
    e->due = m->cpu.cycles + delay;
    e->order = bt->next_order++;
    e->length = (uint16_t)size;
    memcpy(e->data, event, size);
    return;
  }
  /* Nobody is reading the interrupt endpoint. */
  bt->dropped++;
  if (m->log && bt->dropped <= 8) {
    fprintf(m->log, "wii: the Bluetooth module has %u events nobody has read; one (%02x) is lost\n",
            WII_BT_EVENT_SLOTS, event[0]);
  }
}

void wii_bt_queue_acl(GcMachine *m, const uint8_t *packet, unsigned size, uint64_t delay) {
  WiiBluetooth *bt = wii_bluetooth(m);
  unsigned i;
  if (size > WII_BT_ACL_BYTES) size = WII_BT_ACL_BYTES;
  for (i = 0; i < WII_BT_ACL_SLOTS; i++) {
    WiiBtAcl *a = &bt->acl[i];
    if (a->used) continue;
    a->used = 1;
    a->due = m->cpu.cycles + delay;
    a->order = bt->next_order++;
    a->length = (uint16_t)size;
    memcpy(a->data, packet, size);
    return;
  }
  bt->dropped++;
  if (m->log && bt->dropped <= 8) {
    fprintf(m->log, "wii: the Bluetooth module has %u packets nobody has read; one is lost\n", WII_BT_ACL_SLOTS);
  }
}

/* Take back events of one kind that have not gone out yet: an inquiry or a
 * name request the stack has cancelled. */
static void forget_events(WiiBluetooth *bt, uint8_t code) {
  unsigned i;
  for (i = 0; i < WII_BT_EVENT_SLOTS; i++) {
    if (bt->events[i].used && bt->events[i].data[0] == code) bt->events[i].used = 0;
  }
}

/* An event being put together: its code, a length filled in at the end,
 * and its parameters. */
typedef struct Event {
  uint8_t data[WII_BT_EVENT_BYTES];
  unsigned size;
} Event;

static void ev_begin(Event *e, uint8_t code) {
  e->data[0] = code;
  e->size = 2;
}
static void ev8(Event *e, unsigned value) {
  if (e->size < sizeof(e->data)) e->data[e->size++] = (uint8_t)value;
}
static void ev16(Event *e, unsigned value) {
  ev8(e, value);
  ev8(e, value >> 8);
}
static void ev32(Event *e, uint32_t value) {
  ev16(e, value & 0xffffu);
  ev16(e, value >> 16);
}
static void ev_bytes(Event *e, const uint8_t *bytes, unsigned count) {
  unsigned i;
  for (i = 0; i < count; i++) ev8(e, bytes[i]);
}
static void ev_send(GcMachine *m, Event *e, uint64_t delay) {
  e->data[1] = (uint8_t)(e->size - 2u);
  wii_bt_queue_event(m, e->data, e->size, delay);
}

/* Command Complete, up to its return parameters: the module takes one
 * command at a time, so the count of commands it will take next is always
 * one. */
static void complete_begin(Event *e, uint16_t opcode, unsigned status) {
  ev_begin(e, EVT_COMMAND_COMPLETE);
  ev8(e, 1);
  ev16(e, opcode);
  ev8(e, status);
}

static void complete(GcMachine *m, uint16_t opcode, unsigned status) {
  Event e;
  complete_begin(&e, opcode, status);
  ev_send(m, &e, WII_BT_EVENT_CYCLES);
}

static void command_status(GcMachine *m, uint16_t opcode, unsigned status) {
  Event e;
  ev_begin(&e, EVT_COMMAND_STATUS);
  ev8(&e, status);
  ev8(&e, 1);
  ev16(&e, opcode);
  ev_send(m, &e, WII_BT_EVENT_CYCLES);
}

/* When the event that finishes a command which needed the radio goes out:
 * after the Command Status, by a packet's round trip. */
#define AFTER_STATUS (WII_BT_EVENT_CYCLES + WII_BT_RADIO_CYCLES)

/* ------------------------------------------------------------------ */
/* Addresses, keys, links                                              */

/* On the wire an address is least significant byte first. */
static void remote_address(unsigned index, uint8_t out[6]) {
  uint8_t a[6];
  unsigned i;
  wii_remote_address(index, a);
  for (i = 0; i < 6u; i++) out[i] = a[5u - i];
}

static void console_address(uint8_t out[6]) {
  uint8_t a[6];
  unsigned i;
  wii_console_bluetooth_address(a);
  for (i = 0; i < 6u; i++) out[i] = a[5u - i];
}

static int remote_by_address(const uint8_t *address) {
  uint8_t a[6];
  unsigned i;
  for (i = 0; i < WII_REMOTES; i++) {
    remote_address(i, a);
    if (!memcmp(a, address, 6)) return (int)i;
  }
  return -1;
}

/* The link key a remote and its console share. A real one comes out of the
 * pairing; these are made up, one per remote, and are the same in the
 * remote and in the module's memory - which is all a key has to be. */
static void remote_key(unsigned index, uint8_t key[16]) {
  unsigned i;
  for (i = 0; i < 16u; i++) key[i] = (uint8_t)(0x4e + 0x1d * i + 0x35 * index);
}

static int link_by_handle(const WiiBluetooth *bt, unsigned handle) {
  const unsigned index = (handle & 0x0fffu) - WII_BT_FIRST_HANDLE;
  if ((handle & 0x0fffu) < WII_BT_FIRST_HANDLE || index >= WII_REMOTES) return -1;
  return bt->link[index].state == WII_BT_LINK_UP ? (int)index : -1;
}

static unsigned link_handle(unsigned index) { return WII_BT_FIRST_HANDLE + index; }

static int remote_present(const GcMachine *m, unsigned index) {
  return wii_state(m)->remote_input[index].present != 0;
}

static WiiBtKey *stored_key(WiiBluetooth *bt, const uint8_t *address) {
  unsigned i;
  for (i = 0; i < WII_BT_KEYS; i++) {
    if (bt->keys[i].used && !memcmp(bt->keys[i].address, address, 6)) return &bt->keys[i];
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Power-on and reset                                                  */

/* The settings an HCI Reset returns to their defaults (Core Specification,
 * the default of each configuration parameter). */
static void default_settings(WiiBluetooth *bt) {
  static const uint8_t giac[3] = {0x33, 0x8b, 0x9e};
  bt->scan_enable = 0;
  bt->inquiry_mode = 0;
  bt->inquiry_scan_type = 0;
  bt->page_scan_type = 0;
  bt->pin_type = 0;
  bt->auth_enable = 0;
  bt->encryption_mode = 0;
  bt->afh_assessment = 1;
  bt->page_scan_period = 0;
  bt->page_scan_mode = 0;
  bt->broadcast_retransmissions = 1;
  bt->sco_flow = 0;
  bt->host_flow = 0;
  bt->iac_count = 1;
  memset(bt->class_of_device, 0, sizeof(bt->class_of_device));
  memset(bt->iac, 0, sizeof(bt->iac));
  memcpy(bt->iac, giac, 3);
  wii_put_le16(bt->page_timeout, 0x2000u);
  wii_put_le16(bt->accept_timeout, 0x1fa0u);
  wii_put_le16(bt->voice, 0x0060u);
  wii_put_le16(bt->hold_activity, 0);
  wii_put_le16(bt->default_policy, 0);
  wii_put_le16(bt->page_scan, 0x0800u);
  wii_put_le16(bt->page_scan + 2, 0x0012u);
  wii_put_le16(bt->inquiry_scan, 0x1000u);
  wii_put_le16(bt->inquiry_scan + 2, 0x0012u);
  memset(bt->event_mask, 0xff, 5);
  bt->event_mask[5] = 0x1f;
  bt->event_mask[6] = bt->event_mask[7] = 0;
  memset(bt->name, 0, sizeof(bt->name));
  bt->host_acl_length = 0;
  bt->host_acl_packets = 0;
  bt->host_acl_out = 0;
  bt->auto_accept = 0;
  bt->inquiry_filter = 0;
  bt->inquiry_end = 0;
  bt->periodic = 0;
  bt->paging = 0;
}

void wii_bt_reset(GcMachine *m) {
  WiiBluetooth *bt = wii_bluetooth(m);
  unsigned i;
  if (!bt) return;
  memset(bt, 0, sizeof(*bt));
  default_settings(bt);
  /* The module remembers the link keys of the remotes registered with the
   * console, and a retail game drops from its own list any remote the
   * module has no key for - so all four are here whether or not they are
   * switched on. */
  for (i = 0; i < WII_REMOTES; i++) {
    bt->keys[i].used = 1;
    remote_address(i, bt->keys[i].address);
    remote_key(i, bt->keys[i].key);
    wii_remote_factory(&bt->remote[i]);
    wii_remote_power_on(&bt->remote[i]);
  }
}

/* A link is gone: its channels with it, and the remote is as it is after
 * its batteries go in. */
static void link_clear(WiiBluetooth *bt, unsigned index) {
  WiiBtLink *link = &bt->link[index];
  const uint8_t wants = link->wants, was_present = link->was_present, had_buttons = link->had_buttons;
  const uint64_t ask_at = link->ask_at;
  unsigned i;
  /* Whatever was on its way to the console over this link never arrives. */
  for (i = 0; i < WII_BT_ACL_SLOTS; i++) {
    if (bt->acl[i].used && (wii_le16(bt->acl[i].data) & 0x0fffu) == link_handle(index)) bt->acl[i].used = 0;
  }
  memset(link, 0, sizeof(*link));
  link->wants = wants;
  link->was_present = was_present;
  link->had_buttons = had_buttons;
  link->ask_at = ask_at;
  wii_remote_power_on(&bt->remote[index]);
}

/* HCI Reset: the module forgets its settings and its links, and keeps what
 * it has in non-volatile memory - the link keys. The remotes are not told;
 * each finds its link dead and, after a while, looks for the console
 * again. */
static void hci_reset(GcMachine *m) {
  WiiBluetooth *bt = wii_bluetooth(m);
  unsigned i;
  for (i = 0; i < WII_REMOTES; i++) {
    const int was_up = bt->link[i].state != WII_BT_LINK_NONE;
    link_clear(bt, i);
    if (was_up) bt->link[i].ask_at = m->cpu.cycles + WII_BT_RETRY_CYCLES;
  }
  for (i = 0; i < WII_BT_EVENT_SLOTS; i++) bt->events[i].used = 0;
  for (i = 0; i < WII_BT_ACL_SLOTS; i++) bt->acl[i].used = 0;
  default_settings(bt);
}

/* ------------------------------------------------------------------ */
/* L2CAP: what a remote sends                                          */

enum {
  L2CAP_SIGNALLING = 0x0001,
  L2CAP_REJECT = 0x01,
  L2CAP_CONNECT_REQUEST = 0x02,
  L2CAP_CONNECT_RESPONSE = 0x03,
  L2CAP_CONFIG_REQUEST = 0x04,
  L2CAP_CONFIG_RESPONSE = 0x05,
  L2CAP_DISCONNECT_REQUEST = 0x06,
  L2CAP_DISCONNECT_RESPONSE = 0x07,
  L2CAP_ECHO_REQUEST = 0x08,
  L2CAP_ECHO_RESPONSE = 0x09,
  L2CAP_INFO_REQUEST = 0x0a,
  L2CAP_INFO_RESPONSE = 0x0b
};

enum { PSM_SDP = 0x0001, PSM_HID_CONTROL = 0x0011, PSM_HID_INTERRUPT = 0x0013 };

/* The largest packet a remote takes on a channel, which it says when the
 * channel is configured. */
#define REMOTE_MTU 185u

/* One L2CAP packet from a remote, in one ACL packet: nothing a remote
 * sends is too long for that. */
static void l2cap_send(GcMachine *m, unsigned index, unsigned cid, const uint8_t *payload, unsigned size,
                       uint64_t delay) {
  uint8_t packet[WII_BT_ACL_BYTES];
  if (size + 8u > sizeof(packet)) {
    if (m->log) fprintf(m->log, "wii: a remote's packet of %u bytes is too long to send\n", size);
    return;
  }
  /* The handle, and "first packet of a message". */
  wii_put_le16(packet, (uint16_t)(link_handle(index) | 0x2000u));
  wii_put_le16(packet + 2, (uint16_t)(size + 4u));
  wii_put_le16(packet + 4, (uint16_t)size);
  wii_put_le16(packet + 6, (uint16_t)cid);
  if (size) memcpy(packet + 8, payload, size);
  wii_bt_queue_acl(m, packet, size + 8u, delay);
}

static void l2cap_signal(GcMachine *m, unsigned index, unsigned code, unsigned id, const uint8_t *data,
                         unsigned size, uint64_t delay) {
  uint8_t packet[64];
  if (size + 4u > sizeof(packet)) size = sizeof(packet) - 4u;
  packet[0] = (uint8_t)code;
  packet[1] = (uint8_t)id;
  wii_put_le16(packet + 2, (uint16_t)size);
  if (size) memcpy(packet + 4, data, size);
  l2cap_send(m, index, L2CAP_SIGNALLING, packet, size + 4u, delay);
}

static unsigned next_signal_id(WiiBtLink *link) {
  link->next_id++;
  if (!link->next_id) link->next_id = 1;
  return link->next_id;
}

/* The remote asks for a channel of its own. */
static void remote_asks(GcMachine *m, unsigned index, unsigned slot, unsigned psm, uint64_t delay) {
  WiiBtLink *link = &wii_bluetooth(m)->link[index];
  WiiBtChannel *channel = &link->channel[slot];
  uint8_t data[4];
  memset(channel, 0, sizeof(*channel));
  channel->state = WII_BT_CHANNEL_ASKED;
  channel->psm = (uint16_t)psm;
  channel->request_id = (uint8_t)next_signal_id(link);
  wii_put_le16(data, (uint16_t)psm);
  wii_put_le16(data + 2, (uint16_t)(WII_BT_FIRST_CID + slot));
  l2cap_signal(m, index, L2CAP_CONNECT_REQUEST, channel->request_id, data, 4, delay);
}

/* The remote says what it wants of a channel: its packet size and nothing
 * else. */
static void remote_configures(GcMachine *m, unsigned index, unsigned slot, int with_mtu) {
  WiiBtLink *link = &wii_bluetooth(m)->link[index];
  WiiBtChannel *channel = &link->channel[slot];
  uint8_t data[8];
  channel->request_id = (uint8_t)next_signal_id(link);
  channel->config |= WII_BT_CONFIG_SENT;
  wii_put_le16(data, channel->host_cid);
  wii_put_le16(data + 2, 0);
  data[4] = 0x01; /* option: maximum transmission unit */
  data[5] = 2;
  wii_put_le16(data + 6, REMOTE_MTU);
  l2cap_signal(m, index, L2CAP_CONFIG_REQUEST, channel->request_id, data, with_mtu ? 8u : 4u,
               WII_BT_RADIO_CYCLES);
}

static void link_down(GcMachine *m, unsigned index, unsigned reason);

/* Both sides have agreed: the channel carries data. A remote that
 * connected of its own accord opens its interrupt channel once its control
 * channel is up - one after the other, which is the order both stacks
 * listen in. */
static void channel_settled(GcMachine *m, unsigned index, unsigned slot) {
  WiiBluetooth *bt = wii_bluetooth(m);
  WiiBtLink *link = &bt->link[index];
  WiiBtChannel *channel = &link->channel[slot];
  if (channel->state != WII_BT_CHANNEL_CONFIG) return;
  if ((channel->config & (WII_BT_CONFIG_ACCEPTED | WII_BT_CONFIG_ANSWERED)) !=
      (WII_BT_CONFIG_ACCEPTED | WII_BT_CONFIG_ANSWERED)) {
    return;
  }
  channel->state = WII_BT_CHANNEL_OPEN;
  if (slot == WII_BT_CHANNEL_CONTROL && link->remote_began &&
      link->channel[WII_BT_CHANNEL_INTERRUPT].state == WII_BT_CHANNEL_CLOSED) {
    remote_asks(m, index, WII_BT_CHANNEL_INTERRUPT, PSM_HID_INTERRUPT, WII_BT_RADIO_CYCLES);
  }
  if (slot == WII_BT_CHANNEL_INTERRUPT) {
    /* The remote can be heard: its first report a report's time from
     * now. */
    bt->remote[index].next_report = m->cpu.cycles + WII_BT_REPORT_CYCLES;
  }
}

static void reject(GcMachine *m, unsigned index, unsigned id, unsigned reason, unsigned local, unsigned remote) {
  uint8_t data[6];
  wii_put_le16(data, (uint16_t)reason);
  wii_put_le16(data + 2, (uint16_t)local);
  wii_put_le16(data + 4, (uint16_t)remote);
  l2cap_signal(m, index, L2CAP_REJECT, id, data, reason == 2u ? 6u : 2u, WII_BT_RADIO_CYCLES);
}

/* Which of the remote's channels a channel number is, or -1. */
static int slot_of(const WiiBtLink *link, unsigned cid) {
  const unsigned slot = cid - WII_BT_FIRST_CID;
  if (cid < WII_BT_FIRST_CID || slot >= WII_BT_CHANNELS) return -1;
  return link->channel[slot].state != WII_BT_CHANNEL_CLOSED ? (int)slot : -1;
}

/* The console asks for a channel. */
static void host_asks(GcMachine *m, unsigned index, unsigned id, const uint8_t *data, unsigned size) {
  WiiBtLink *link = &wii_bluetooth(m)->link[index];
  uint8_t answer[8];
  unsigned psm, host_cid, slot = WII_BT_CHANNELS, result = 0;
  if (size < 4u) {
    reject(m, index, id, 0, 0, 0);
    return;
  }
  psm = wii_le16(data);
  host_cid = wii_le16(data + 2);
  if (psm == PSM_HID_CONTROL) {
    slot = WII_BT_CHANNEL_CONTROL;
  } else if (psm == PSM_HID_INTERRUPT) {
    slot = WII_BT_CHANNEL_INTERRUPT;
  } else if (psm == PSM_SDP) {
    for (slot = 2; slot < WII_BT_CHANNELS && link->channel[slot].state != WII_BT_CHANNEL_CLOSED; slot++) {
    }
    if (slot == WII_BT_CHANNELS) result = 4; /* no resources */
  } else {
    result = 2; /* nothing listens on that PSM */
    if (m->log) fprintf(m->log, "wii: a game asked a remote for L2CAP PSM %04x, which a remote does not have\n", psm);
  }
  if (!result && link->channel[slot].state != WII_BT_CHANNEL_CLOSED) result = 4;

  wii_put_le16(answer, (uint16_t)(result ? 0u : WII_BT_FIRST_CID + slot));
  wii_put_le16(answer + 2, (uint16_t)host_cid);
  wii_put_le16(answer + 4, (uint16_t)result);
  wii_put_le16(answer + 6, 0);
  l2cap_signal(m, index, L2CAP_CONNECT_RESPONSE, id, answer, 8, WII_BT_RADIO_CYCLES);
  if (result) return;

  memset(&link->channel[slot], 0, sizeof(link->channel[slot]));
  link->channel[slot].state = WII_BT_CHANNEL_CONFIG;
  link->channel[slot].psm = (uint16_t)psm;
  link->channel[slot].host_cid = (uint16_t)host_cid;
  link->channel[slot].host_mtu = 672;
  remote_configures(m, index, slot, 1);
}

/* The console's options for a channel. A remote takes any packet size and
 * any flush timeout, and only the plain mode of operation. */
static void host_configures(GcMachine *m, unsigned index, unsigned id, const uint8_t *data, unsigned size) {
  WiiBtLink *link = &wii_bluetooth(m)->link[index];
  uint8_t answer[6 + 32];
  unsigned at = 4, result = 0, unknown_bytes = 0, flags;
  int slot;
  if (size < 4u) {
    reject(m, index, id, 0, 0, 0);
    return;
  }
  slot = slot_of(link, wii_le16(data));
  flags = wii_le16(data + 2);
  if (slot < 0) {
    reject(m, index, id, 2, wii_le16(data), 0);
    return;
  }
  while (at + 2u <= size) {
    const unsigned type = data[at] & 0x7fu, hint = data[at] & 0x80u, length = data[at + 1u];
    if (at + 2u + length > size) break;
    if (type == 0x01u && length == 2u) {
      link->channel[slot].host_mtu = wii_le16(data + at + 2u);
    } else if (type == 0x04u && length >= 1u && data[at + 2u] != 0u) {
      /* Retransmission or flow-control mode: a remote has neither. */
      result = 1;
    } else if (type < 0x01u || type > 0x04u) {
      /* An option nobody defined: a hint may be skipped, anything else is
       * named in the answer. */
      if (!hint && unknown_bytes + 2u + length <= 32u) {
        memcpy(answer + 6 + unknown_bytes, data + at, 2u + length);
        unknown_bytes += 2u + length;
        result = 3;
      }
    }
    at += 2u + length;
  }
  wii_put_le16(answer, link->channel[slot].host_cid);
  wii_put_le16(answer + 2, (uint16_t)(flags & 1u));
  wii_put_le16(answer + 4, (uint16_t)result);
  l2cap_signal(m, index, L2CAP_CONFIG_RESPONSE, id, answer, 6u + (result == 3u ? unknown_bytes : 0u),
               WII_BT_RADIO_CYCLES);
  /* More options follow in another request. */
  if (result || (flags & 1u)) return;
  link->channel[slot].config |= WII_BT_CONFIG_ANSWERED;
  if (!(link->channel[slot].config & WII_BT_CONFIG_SENT)) remote_configures(m, index, (unsigned)slot, 1);
  channel_settled(m, index, (unsigned)slot);
}

static void l2cap_signals(GcMachine *m, unsigned index, const uint8_t *data, unsigned size) {
  WiiBluetooth *bt = wii_bluetooth(m);
  WiiBtLink *link = &bt->link[index];
  unsigned at = 0;
  while (at + 4u <= size && link->state == WII_BT_LINK_UP) {
    const unsigned code = data[at], id = data[at + 1u], length = wii_le16(data + at + 2u);
    const uint8_t *body = data + at + 4u;
    int slot;
    if (at + 4u + length > size) break;
    switch (code) {
      case L2CAP_CONNECT_REQUEST: host_asks(m, index, id, body, length); break;

      case L2CAP_CONNECT_RESPONSE: {
        unsigned result;
        if (length < 8u) break;
        slot = slot_of(link, wii_le16(body + 2));
        result = wii_le16(body + 4);
        if (slot < 0 || link->channel[slot].state != WII_BT_CHANNEL_ASKED || link->channel[slot].request_id != id) {
          break; /* an answer to nothing the remote asked */
        }
        if (result == 1u) break; /* pending: the console is checking who this is */
        if (result != 0u) {
          /* Refused. A remote that cannot open its channels gives up and
           * switches off; this one tries again later. */
          if (m->log) {
            fprintf(m->log, "wii: the game refused remote %u's L2CAP channel %04x (result %u)\n", index,
                    link->channel[slot].psm, result);
          }
          link_down(m, index, HCI_REMOTE_USER_ENDED);
          link->ask_at = m->cpu.cycles + WII_BT_RETRY_CYCLES;
          return;
        }
        link->channel[slot].state = WII_BT_CHANNEL_CONFIG;
        link->channel[slot].host_cid = wii_le16(body);
        link->channel[slot].host_mtu = 672;
        remote_configures(m, index, (unsigned)slot, 1);
        break;
      }

      case L2CAP_CONFIG_REQUEST: host_configures(m, index, id, body, length); break;

      case L2CAP_CONFIG_RESPONSE: {
        unsigned result;
        if (length < 6u) break;
        slot = slot_of(link, wii_le16(body));
        if (slot < 0 || link->channel[slot].request_id != id) break;
        result = wii_le16(body + 4);
        if (result == 0u) {
          if (!(wii_le16(body + 2) & 1u)) link->channel[slot].config |= WII_BT_CONFIG_ACCEPTED;
          channel_settled(m, index, (unsigned)slot);
        } else if (result == 1u && !(link->channel[slot].config & WII_BT_CONFIG_ACCEPTED)) {
          /* The console did not like the packet size: ask for the default. */
          remote_configures(m, index, (unsigned)slot, 0);
        } else if (m->log) {
          fprintf(m->log, "wii: the game rejected remote %u's options for channel %04x (result %u)\n", index,
                  link->channel[slot].psm, result);
        }
        break;
      }

      case L2CAP_DISCONNECT_REQUEST: {
        if (length < 4u) break;
        slot = slot_of(link, wii_le16(body));
        if (slot < 0 || link->channel[slot].host_cid != wii_le16(body + 2)) {
          reject(m, index, id, 2, wii_le16(body), wii_le16(body + 2));
          break;
        }
        l2cap_signal(m, index, L2CAP_DISCONNECT_RESPONSE, id, body, 4, WII_BT_RADIO_CYCLES);
        memset(&link->channel[slot], 0, sizeof(link->channel[slot]));
        break;
      }

      case L2CAP_DISCONNECT_RESPONSE:
        if (length >= 4u) {
          slot = slot_of(link, wii_le16(body + 2));
          if (slot >= 0) memset(&link->channel[slot], 0, sizeof(link->channel[slot]));
        }
        break;

      case L2CAP_ECHO_REQUEST:
        l2cap_signal(m, index, L2CAP_ECHO_RESPONSE, id, body, length, WII_BT_RADIO_CYCLES);
        break;

      case L2CAP_INFO_REQUEST: {
        /* What else can you do: nothing. A Bluetooth 1.2 device has no
         * extended features and no fixed channels to list. */
        uint8_t answer[4];
        wii_put_le16(answer, length >= 2u ? wii_le16(body) : 0u);
        wii_put_le16(answer + 2, 1); /* not supported */
        l2cap_signal(m, index, L2CAP_INFO_RESPONSE, id, answer, 4, WII_BT_RADIO_CYCLES);
        break;
      }

      case L2CAP_REJECT:
        if (m->log) {
          fprintf(m->log, "wii: the game rejected an L2CAP command of remote %u (reason %u)\n", index,
                  length >= 2u ? wii_le16(body) : 0u);
        }
        break;
      case L2CAP_ECHO_RESPONSE:
      case L2CAP_INFO_RESPONSE: break;

      default: reject(m, index, id, 0, 0, 0); break;
    }
    at += 4u + length;
  }
}

/* ------------------------------------------------------------------ */
/* SDP: what a remote says it is                                       */

/* A service record is a list of attributes, each a number and a value,
 * and every value is built of "data elements": a byte giving a type and a
 * size, then the value, most significant byte first (Core Specification,
 * volume 3, part B). The records here are a remote's two - the Human
 * Interface Device service and its Device Identification - with the
 * attributes the HID profile (version 1.0, section 7.11) and the Device ID
 * profile define, and the values a remote gives: its name, Nintendo's
 * vendor number and the remote's product number, and a report descriptor
 * that lists each report's number and length and says nothing of what is
 * in them. */
typedef struct Sdp {
  uint8_t data[768];
  unsigned size;
  /* Where each attribute starts, for picking some out. */
  uint16_t attribute[40], start[41];
  unsigned attributes;
} Sdp;

static void sdp_byte(Sdp *s, unsigned value) {
  if (s->size < sizeof(s->data)) s->data[s->size++] = (uint8_t)value;
}
static void sdp_u8(Sdp *s, unsigned value) {
  sdp_byte(s, 0x08);
  sdp_byte(s, value);
}
static void sdp_u16(Sdp *s, unsigned value) {
  sdp_byte(s, 0x09);
  sdp_byte(s, value >> 8);
  sdp_byte(s, value);
}
static void sdp_u32(Sdp *s, uint32_t value) {
  sdp_byte(s, 0x0a);
  sdp_byte(s, value >> 24);
  sdp_byte(s, value >> 16);
  sdp_byte(s, value >> 8);
  sdp_byte(s, value);
}
static void sdp_uuid(Sdp *s, unsigned value) {
  sdp_byte(s, 0x19);
  sdp_byte(s, value >> 8);
  sdp_byte(s, value);
}
static void sdp_bool(Sdp *s, int value) {
  sdp_byte(s, 0x28);
  sdp_byte(s, value ? 1u : 0u);
}
static void sdp_text(Sdp *s, const uint8_t *bytes, unsigned count) {
  unsigned i;
  sdp_byte(s, 0x25);
  sdp_byte(s, count);
  for (i = 0; i < count; i++) sdp_byte(s, bytes[i]);
}
static void sdp_string(Sdp *s, const char *text) { sdp_text(s, (const uint8_t *)text, (unsigned)strlen(text)); }
/* A sequence, whose length is filled in when it closes. */
static unsigned sdp_open(Sdp *s) {
  sdp_byte(s, 0x36);
  sdp_byte(s, 0);
  sdp_byte(s, 0);
  return s->size;
}
static void sdp_close(Sdp *s, unsigned mark) {
  const unsigned length = s->size - mark;
  s->data[mark - 2u] = (uint8_t)(length >> 8);
  s->data[mark - 1u] = (uint8_t)length;
}
static void sdp_attribute(Sdp *s, unsigned id) {
  if (s->attributes < 40u) {
    s->attribute[s->attributes] = (uint16_t)id;
    s->start[s->attributes++] = (uint16_t)s->size;
  }
  sdp_u16(s, id);
}

/* The remote's reports, as its descriptor lists them: output reports
 * 0x10-0x1a and input reports 0x20-0x22 and 0x30-0x3f, each a number of
 * bytes with a vendor's usage and no meaning given. */
static unsigned remote_descriptor(uint8_t *out) {
  static const struct {
    uint8_t id, length, input;
  } reports[] = {
      {0x10, 1, 0},  {0x11, 1, 0},  {0x12, 2, 0},  {0x13, 1, 0},  {0x14, 1, 0},  {0x15, 1, 0},  {0x16, 21, 0},
      {0x17, 6, 0},  {0x18, 21, 0}, {0x19, 1, 0},  {0x1a, 1, 0},  {0x20, 6, 1},  {0x21, 21, 1}, {0x22, 4, 1},
      {0x30, 2, 1},  {0x31, 5, 1},  {0x32, 10, 1}, {0x33, 17, 1}, {0x34, 21, 1}, {0x35, 21, 1}, {0x36, 21, 1},
      {0x37, 21, 1}, {0x3d, 21, 1}, {0x3e, 21, 1}, {0x3f, 21, 1},
  };
  unsigned n = 0, i;
  out[n++] = 0x05, out[n++] = 0x01; /* usage page: generic desktop */
  out[n++] = 0x09, out[n++] = 0x05; /* usage: game pad */
  out[n++] = 0xa1, out[n++] = 0x01; /* collection: application */
  for (i = 0; i < sizeof(reports) / sizeof(reports[0]); i++) {
    out[n++] = 0x85, out[n++] = reports[i].id; /* report number */
    if (i == 0) {
      out[n++] = 0x15, out[n++] = 0x00;                   /* logical minimum 0 */
      out[n++] = 0x26, out[n++] = 0xff, out[n++] = 0x00; /* logical maximum 255 */
      out[n++] = 0x75, out[n++] = 0x08;                   /* fields of 8 bits */
    }
    out[n++] = 0x95, out[n++] = reports[i].length; /* how many */
    if (i == 0) out[n++] = 0x06, out[n++] = 0x00, out[n++] = 0xff; /* usage page: vendor's own */
    out[n++] = 0x09, out[n++] = 0x01;                                /* usage 1 */
    out[n++] = reports[i].input ? 0x81 : 0x91, out[n++] = 0x00;      /* input or output, data */
  }
  out[n++] = 0xc0; /* end of collection */
  return n;
}

#define SDP_HID_HANDLE 0x00010000u
#define SDP_ID_HANDLE 0x00010001u

static void sdp_protocols(Sdp *s, unsigned psm, unsigned protocol) {
  unsigned b, c;
  b = sdp_open(s);
  sdp_uuid(s, 0x0100); /* L2CAP */
  sdp_u16(s, psm);
  sdp_close(s, b);
  c = sdp_open(s);
  sdp_uuid(s, protocol);
  sdp_close(s, c);
}

static void sdp_record(Sdp *s, uint32_t handle) {
  uint8_t descriptor[256];
  unsigned a, b, c;
  memset(s, 0, sizeof(*s));
  if (handle == SDP_HID_HANDLE) {
    sdp_attribute(s, 0x0000); /* the record's own handle */
    sdp_u32(s, handle);
    sdp_attribute(s, 0x0001); /* service classes */
    a = sdp_open(s);
    sdp_uuid(s, 0x1124); /* Human Interface Device */
    sdp_close(s, a);
    sdp_attribute(s, 0x0004); /* how to reach it: L2CAP PSM 0x11, HID protocol */
    a = sdp_open(s);
    sdp_protocols(s, PSM_HID_CONTROL, 0x0011);
    sdp_close(s, a);
    sdp_attribute(s, 0x0005); /* browse group: the public root */
    a = sdp_open(s);
    sdp_uuid(s, 0x1002);
    sdp_close(s, a);
    sdp_attribute(s, 0x0006); /* the text below is English, UTF-8, from 0x0100 */
    a = sdp_open(s);
    sdp_u16(s, 0x656e);
    sdp_u16(s, 0x006a);
    sdp_u16(s, 0x0100);
    sdp_close(s, a);
    sdp_attribute(s, 0x0009); /* profile: HID 1.0 */
    a = sdp_open(s);
    b = sdp_open(s);
    sdp_uuid(s, 0x1124);
    sdp_u16(s, 0x0100);
    sdp_close(s, b);
    sdp_close(s, a);
    sdp_attribute(s, 0x000d); /* and a second channel: PSM 0x13 */
    a = sdp_open(s);
    b = sdp_open(s);
    sdp_protocols(s, PSM_HID_INTERRUPT, 0x0011);
    sdp_close(s, b);
    sdp_close(s, a);
    sdp_attribute(s, 0x0100);
    sdp_string(s, WII_REMOTE_NAME);
    sdp_attribute(s, 0x0101);
    sdp_string(s, WII_REMOTE_NAME);
    sdp_attribute(s, 0x0102);
    sdp_string(s, "Nintendo");
    sdp_attribute(s, 0x0200); /* device release */
    sdp_u16(s, 0x0100);
    sdp_attribute(s, 0x0201); /* HID parser version 1.11 */
    sdp_u16(s, 0x0111);
    sdp_attribute(s, 0x0202); /* subclass: joystick */
    sdp_u8(s, 0x04);
    sdp_attribute(s, 0x0203); /* country code */
    sdp_u8(s, 0x33);
    sdp_attribute(s, 0x0204); /* virtual cable */
    sdp_bool(s, 1);
    sdp_attribute(s, 0x0205); /* reconnects of its own accord */
    sdp_bool(s, 1);
    sdp_attribute(s, 0x0206); /* descriptors: one, a report descriptor */
    a = sdp_open(s);
    b = sdp_open(s);
    sdp_u8(s, 0x22);
    c = remote_descriptor(descriptor);
    sdp_text(s, descriptor, c);
    sdp_close(s, b);
    sdp_close(s, a);
    sdp_attribute(s, 0x0207); /* language of its strings: US English */
    a = sdp_open(s);
    b = sdp_open(s);
    sdp_u16(s, 0x0409);
    sdp_u16(s, 0x0100);
    sdp_close(s, b);
    sdp_close(s, a);
    sdp_attribute(s, 0x0208); /* keeps answering SDP while connected */
    sdp_bool(s, 0);
    sdp_attribute(s, 0x0209); /* runs on batteries */
    sdp_bool(s, 1);
    sdp_attribute(s, 0x020a); /* can wake the host */
    sdp_bool(s, 1);
    sdp_attribute(s, 0x020b); /* profile version */
    sdp_u16(s, 0x0100);
    sdp_attribute(s, 0x020c); /* link supervision timeout it would like */
    sdp_u16(s, 0x0c80);
    sdp_attribute(s, 0x020d); /* not connectable unless asked to be */
    sdp_bool(s, 0);
    sdp_attribute(s, 0x020e); /* not a boot-protocol device */
    sdp_bool(s, 0);
  } else {
    sdp_attribute(s, 0x0000);
    sdp_u32(s, handle);
    sdp_attribute(s, 0x0001);
    a = sdp_open(s);
    sdp_uuid(s, 0x1200); /* PnP Information */
    sdp_close(s, a);
    sdp_attribute(s, 0x0004);
    a = sdp_open(s);
    sdp_protocols(s, PSM_SDP, 0x0001);
    sdp_close(s, a);
    sdp_attribute(s, 0x0005);
    a = sdp_open(s);
    sdp_uuid(s, 0x1002);
    sdp_close(s, a);
    sdp_attribute(s, 0x0009);
    a = sdp_open(s);
    b = sdp_open(s);
    sdp_uuid(s, 0x1200);
    sdp_u16(s, 0x0100);
    sdp_close(s, b);
    sdp_close(s, a);
    sdp_attribute(s, 0x0200); /* Device ID specification 1.0 */
    sdp_u16(s, 0x0100);
    sdp_attribute(s, 0x0201); /* vendor: Nintendo */
    sdp_u16(s, 0x057e);
    sdp_attribute(s, 0x0202); /* product: the remote */
    sdp_u16(s, 0x0306);
    sdp_attribute(s, 0x0203); /* version */
    sdp_u16(s, 0x0600);
    sdp_attribute(s, 0x0204); /* this is the primary record */
    sdp_bool(s, 1);
    sdp_attribute(s, 0x0205); /* the vendor number is a USB one */
    sdp_u16(s, 0x0002);
  }
  s->start[s->attributes] = (uint16_t)s->size;
}

/* The service classes and protocols each record names, for a search. */
static int sdp_record_has(uint32_t handle, uint32_t uuid) {
  if (uuid == 0x0100u || uuid == 0x1002u) return 1;
  if (handle == SDP_HID_HANDLE) return uuid == 0x1124u || uuid == 0x0011u;
  return uuid == 0x1200u || uuid == 0x0001u;
}

/* One data element of a request: its type, where its value starts and how
 * long that is. Returns zero for one that runs off the end. */
static int sdp_element(const uint8_t *p, unsigned left, unsigned *type, unsigned *header, unsigned *length) {
  unsigned index;
  if (left < 1u) return 0;
  *type = p[0] >> 3;
  index = p[0] & 7u;
  if (index < 5u) {
    *header = 1;
    *length = *type == 0u ? 0u : 1u << index;
  } else if (index == 5u) {
    if (left < 2u) return 0;
    *header = 2;
    *length = p[1];
  } else if (index == 6u) {
    if (left < 3u) return 0;
    *header = 3;
    *length = ((unsigned)p[1] << 8) | p[2];
  } else {
    if (left < 5u) return 0;
    *header = 5;
    *length = wii_be32(p + 1);
  }
  return *header + *length <= left;
}

/* A request's list of the service classes it is looking for. Returns how
 * many bytes it took, or zero if it is not a list. */
static unsigned sdp_pattern(const uint8_t *p, unsigned left, uint32_t *uuids, unsigned *count) {
  static const uint8_t base[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb};
  unsigned type, header, length, at, end;
  *count = 0;
  if (!sdp_element(p, left, &type, &header, &length) || type != 6u) return 0;
  at = header;
  end = header + length;
  while (at < end) {
    unsigned t, h, l;
    if (!sdp_element(p + at, end - at, &t, &h, &l) || t != 3u) return 0;
    if (*count < 12u) {
      uint32_t value = 0xffffffffu;
      if (l == 2u) value = wii_be16(p + at + h);
      if (l == 4u) value = wii_be32(p + at + h);
      if (l == 16u && !memcmp(p + at + h + 4u, base, 12)) value = wii_be32(p + at + h);
      uuids[(*count)++] = value;
    }
    at += h + l;
  }
  return end;
}

/* The attributes a request wants: single numbers and ranges. */
typedef struct SdpRanges {
  uint16_t low[16], high[16];
  unsigned count;
} SdpRanges;

static unsigned sdp_ranges(const uint8_t *p, unsigned left, SdpRanges *ranges) {
  unsigned type, header, length, at, end;
  ranges->count = 0;
  if (!sdp_element(p, left, &type, &header, &length) || type != 6u) return 0;
  at = header;
  end = header + length;
  while (at < end) {
    unsigned t, h, l;
    if (!sdp_element(p + at, end - at, &t, &h, &l) || t != 1u || (l != 2u && l != 4u)) return 0;
    if (ranges->count < 16u) {
      ranges->low[ranges->count] = wii_be16(p + at + h);
      ranges->high[ranges->count] = l == 4u ? wii_be16(p + at + h + 2u) : wii_be16(p + at + h);
      ranges->count++;
    }
    at += h + l;
  }
  return end;
}

/* Append the attributes of one record that a request asked for, as a
 * sequence. */
static void sdp_pick(Sdp *out, uint32_t handle, const SdpRanges *ranges) {
  Sdp record;
  unsigned mark, i, k;
  sdp_record(&record, handle);
  mark = sdp_open(out);
  for (i = 0; i < record.attributes; i++) {
    int wanted = 0;
    for (k = 0; k < ranges->count; k++) {
      if (record.attribute[i] >= ranges->low[k] && record.attribute[i] <= ranges->high[k]) wanted = 1;
    }
    if (!wanted) continue;
    for (k = record.start[i]; k < record.start[i + 1u]; k++) sdp_byte(out, record.data[k]);
  }
  sdp_close(out, mark);
}

/* How much of an answer goes in one packet: what fits an ACL packet with
 * room for the headers around it. The rest is asked for again with the
 * "continuation state" the answer ends in, which here is simply how far
 * the last one got. */
#define SDP_CHUNK 240u

static void sdp_answer(GcMachine *m, unsigned index, unsigned slot, const uint8_t *request, unsigned size) {
  static const uint32_t handles[2] = {SDP_HID_HANDLE, SDP_ID_HANDLE};
  WiiBtChannel *channel = &wii_bluetooth(m)->link[index].channel[slot];
  uint8_t out[8 + SDP_CHUNK + 8];
  unsigned n = 5, error = 0, pdu, at, used, limit = 0, offset = 0, k;
  uint32_t uuids[12];
  unsigned uuid_count = 0;
  SdpRanges ranges;
  Sdp body;
  int match[2] = {1, 1};
  const uint8_t *p;
  unsigned left;

  if (size < 5u || 5u + wii_be16(request + 3) > size) return;
  pdu = request[0];
  p = request + 5;
  left = wii_be16(request + 3);
  at = 0;
  memset(&body, 0, sizeof(body));

  if (pdu == 0x02u || pdu == 0x06u) {
    used = sdp_pattern(p, left, uuids, &uuid_count);
    if (!used) error = 3;
    at += used;
    for (k = 0; k < uuid_count; k++) {
      if (!sdp_record_has(handles[0], uuids[k])) match[0] = 0;
      if (!sdp_record_has(handles[1], uuids[k])) match[1] = 0;
    }
  }

  if (!error && pdu == 0x02u) {
    /* Service search: the handles of the records that match. */
    unsigned count = (unsigned)match[0] + (unsigned)match[1];
    if (at + 3u > left) {
      error = 3;
    } else {
      out[0] = 0x03;
      wii_put16(out + n, (uint16_t)count);
      wii_put16(out + n + 2, (uint16_t)count);
      n += 4;
      for (k = 0; k < 2u; k++) {
        if (!match[k]) continue;
        wii_put32(out + n, handles[k]);
        n += 4;
      }
      out[n++] = 0;
    }
  } else if (!error && (pdu == 0x04u || pdu == 0x06u)) {
    /* Attributes: of one record by its handle, or of every match. */
    uint32_t handle = 0;
    if (pdu == 0x04u) {
      if (left < 4u) {
        error = 3;
      } else {
        handle = wii_be32(p);
        at = 4;
        if (handle != SDP_HID_HANDLE && handle != SDP_ID_HANDLE) error = 2;
      }
    }
    if (!error && at + 2u > left) error = 3;
    if (!error) {
      limit = wii_be16(p + at);
      at += 2;
      used = sdp_ranges(p + at, left - at, &ranges);
      if (!used) error = 3;
      at += used;
      if (!error && at < left && p[at] == 2u && at + 3u <= left) offset = wii_be16(p + at + 1u);
      if (limit > SDP_CHUNK) limit = SDP_CHUNK;
      if (channel->host_mtu >= 16u && limit > channel->host_mtu - 10u) limit = channel->host_mtu - 10u;
      if (limit < 7u) error = 3;
    }
    if (!error) {
      if (pdu == 0x04u) {
        sdp_pick(&body, handle, &ranges);
      } else {
        const unsigned mark = sdp_open(&body);
        for (k = 0; k < 2u; k++) {
          if (match[k]) sdp_pick(&body, handles[k], &ranges);
        }
        sdp_close(&body, mark);
      }
      if (offset > body.size) {
        error = 5; /* a continuation that was never given out */
      } else {
        unsigned chunk = body.size - offset;
        if (chunk > limit) chunk = limit;
        out[0] = (uint8_t)(pdu + 1u);
        wii_put16(out + n, (uint16_t)chunk);
        n += 2;
        memcpy(out + n, body.data + offset, chunk);
        n += chunk;
        if (offset + chunk < body.size) {
          out[n++] = 2;
          wii_put16(out + n, (uint16_t)(offset + chunk));
          n += 2;
        } else {
          out[n++] = 0;
        }
      }
    }
  } else if (!error) {
    error = 3;
  }

  if (error) {
    out[0] = 0x01;
    n = 5;
    wii_put16(out + n, (uint16_t)error);
    n += 2;
  }
  out[1] = request[1];
  out[2] = request[2];
  wii_put16(out + 3, (uint16_t)(n - 5u));
  l2cap_send(m, index, channel->host_cid, out, n, WII_BT_RADIO_CYCLES);
}

/* ------------------------------------------------------------------ */
/* HID: the two channels a remote's reports travel on                  */

/* The first byte of everything on them (HID profile, section 7.3): a
 * transaction type in the high half and a parameter in the low. */
enum {
  HIDP_HANDSHAKE = 0x0,
  HIDP_CONTROL = 0x1,
  HIDP_GET_REPORT = 0x4,
  HIDP_SET_REPORT = 0x5,
  HIDP_GET_PROTOCOL = 0x6,
  HIDP_SET_PROTOCOL = 0x7,
  HIDP_GET_IDLE = 0x8,
  HIDP_SET_IDLE = 0x9,
  HIDP_DATA = 0xa
};

static void hid_control(GcMachine *m, unsigned index, const uint8_t *data, unsigned size) {
  WiiBluetooth *bt = wii_bluetooth(m);
  const unsigned cid = bt->link[index].channel[WII_BT_CHANNEL_CONTROL].host_cid;
  uint8_t answer[2];
  if (!size) return;
  switch (data[0] >> 4) {
    case HIDP_SET_REPORT:
      /* An output report sent the long way round, which the first remotes
       * take and homebrew's stack uses: do it, and say it was done. */
      if ((data[0] & 3u) == 2u && size >= 2u) {
        wii_remote_output(m, index, data + 1, size - 1u);
        answer[0] = 0x00; /* handshake: successful */
      } else {
        answer[0] = 0x04; /* handshake: invalid parameter */
      }
      l2cap_send(m, index, cid, answer, 1, WII_BT_RADIO_CYCLES);
      return;
    case HIDP_DATA:
      if ((data[0] & 3u) == 2u && size >= 2u) wii_remote_output(m, index, data + 1, size - 1u);
      return;
    case HIDP_CONTROL:
      if ((data[0] & 0x0fu) == 5u) {
        /* "Virtual cable unplug": the console is forgetting this remote.
         * It switches off, and stays off until a button is pressed. */
        bt->link[index].wants = 0;
        link_down(m, index, HCI_REMOTE_USER_ENDED);
      }
      return;
    case HIDP_GET_PROTOCOL:
      answer[0] = 0xa0; /* data, of no report type */
      answer[1] = 0x01; /* the report protocol, which is the only one */
      l2cap_send(m, index, cid, answer, 2, WII_BT_RADIO_CYCLES);
      return;
    case HIDP_SET_PROTOCOL:
    case HIDP_SET_IDLE:
      answer[0] = 0x00;
      l2cap_send(m, index, cid, answer, 1, WII_BT_RADIO_CYCLES);
      return;
    case HIDP_GET_IDLE:
      answer[0] = 0xa0;
      answer[1] = 0x00;
      l2cap_send(m, index, cid, answer, 2, WII_BT_RADIO_CYCLES);
      return;
    case HIDP_HANDSHAKE: return;
    default: break;
  }
  /* Reports are not read back from a remote, and nothing else is defined. */
  answer[0] = 0x03; /* handshake: unsupported request */
  l2cap_send(m, index, cid, answer, 1, WII_BT_RADIO_CYCLES);
}

static void hid_interrupt(GcMachine *m, unsigned index, const uint8_t *data, unsigned size) {
  if (size >= 2u && data[0] == 0xa2u) {
    wii_remote_output(m, index, data + 1, size - 1u);
  } else if (m->log && size) {
    fprintf(m->log, "wii: remote %u was sent %02x on its interrupt channel, which is not an output report\n",
            index, data[0]);
  }
}

/* ------------------------------------------------------------------ */
/* ACL                                                                 */

void wii_bt_acl_from_host(GcMachine *m, const uint8_t *packet, uint32_t size) {
  WiiBluetooth *bt = wii_bluetooth(m);
  const unsigned handle = wii_le16(packet), length = wii_le16(packet + 2);
  const int index = link_by_handle(bt, handle);
  Event e;
  unsigned l2cap_length, cid;
  int slot;

  /* The module has sent it on, or thrown it away: either way its buffer is
   * free again, and the stack is told so, which is what lets it send the
   * next. */
  ev_begin(&e, EVT_COMPLETED_PACKETS);
  ev8(&e, 1);
  ev16(&e, handle & 0x0fffu);
  ev16(&e, 1);
  ev_send(m, &e, WII_BT_EVENT_CYCLES);

  if (index < 0) {
    if (m->log) fprintf(m->log, "wii: ACL data for connection %03x, which does not exist\n", handle & 0x0fffu);
    return;
  }
  if (length + 4u > size || length < 4u) return;
  if (((handle >> 12) & 3u) == 1u) {
    /* The rest of an L2CAP packet too long for one ACL packet. Nothing a
     * console says to a remote is. */
    if (m->log) fprintf(m->log, "wii: a fragmented L2CAP packet for remote %d is not reassembled\n", index);
    return;
  }
  l2cap_length = wii_le16(packet + 4);
  cid = wii_le16(packet + 6);
  if (l2cap_length + 4u > length) return;

  if (cid == L2CAP_SIGNALLING) {
    l2cap_signals(m, (unsigned)index, packet + 8, l2cap_length);
    return;
  }
  slot = slot_of(&bt->link[index], cid);
  if (slot < 0 || bt->link[index].channel[slot].state < WII_BT_CHANNEL_CONFIG) {
    if (m->log) fprintf(m->log, "wii: data for remote %d on L2CAP channel %04x, which is not open\n", index, cid);
    return;
  }
  if (slot == (int)WII_BT_CHANNEL_CONTROL) {
    hid_control(m, (unsigned)index, packet + 8, l2cap_length);
  } else if (slot == (int)WII_BT_CHANNEL_INTERRUPT) {
    hid_interrupt(m, (unsigned)index, packet + 8, l2cap_length);
  } else {
    sdp_answer(m, (unsigned)index, (unsigned)slot, packet + 8, l2cap_length);
  }
}

unsigned wii_bt_next_acl(GcMachine *m, uint8_t *out, uint64_t soonest, uint64_t *when, int take) {
  WiiBluetooth *bt = wii_bluetooth(m);
  WiiBtAcl *queued = NULL;
  int reporter = -1;
  uint64_t report_at = 0;
  unsigned i, size;

  /* A stack that counts packets back gets no more than it has room for. */
  if ((bt->host_flow & 1u) && bt->host_acl_packets && bt->host_acl_out >= bt->host_acl_packets) return 0;

  for (i = 0; i < WII_BT_ACL_SLOTS; i++) {
    WiiBtAcl *a = &bt->acl[i];
    if (!a->used) continue;
    if (!queued || a->due < queued->due || (a->due == queued->due && a->order < queued->order)) queued = a;
  }
  for (i = 0; i < WII_REMOTES; i++) {
    uint64_t at;
    if (bt->link[i].state != WII_BT_LINK_UP) continue;
    if (bt->link[i].channel[WII_BT_CHANNEL_INTERRUPT].state != WII_BT_CHANNEL_OPEN) continue;
    if (!wii_remote_ready(m, i, &at)) continue;
    if (at < soonest) at = soonest;
    if (reporter < 0 || at < report_at) {
      reporter = (int)i;
      report_at = at;
    }
  }

  if (queued && (reporter < 0 || queued->due <= report_at)) {
    size = queued->length;
    memcpy(out, queued->data, size);
    *when = queued->due < soonest ? soonest : queued->due;
    if (take) queued->used = 0;
  } else if (reporter >= 0) {
    const unsigned cid = bt->link[reporter].channel[WII_BT_CHANNEL_INTERRUPT].host_cid;
    unsigned length;
    out[8] = 0xa1; /* data, an input report */
    length = 1u + (take ? wii_remote_take(m, (unsigned)reporter, report_at, out + 9)
                        : wii_remote_peek(m, (unsigned)reporter, out + 9));
    wii_put_le16(out, (uint16_t)(link_handle((unsigned)reporter) | 0x2000u));
    wii_put_le16(out + 2, (uint16_t)(length + 4u));
    wii_put_le16(out + 4, (uint16_t)length);
    wii_put_le16(out + 6, (uint16_t)cid);
    size = length + 8u;
    *when = report_at;
  } else {
    return 0;
  }
  if (take && (bt->host_flow & 1u)) bt->host_acl_out++;
  return size;
}

/* ------------------------------------------------------------------ */
/* Links                                                               */

static void connection_complete(GcMachine *m, unsigned status, unsigned handle, const uint8_t *address,
                                uint64_t delay) {
  Event e;
  ev_begin(&e, EVT_CONNECTION_COMPLETE);
  ev8(&e, status);
  ev16(&e, handle);
  ev_bytes(&e, address, 6);
  ev8(&e, 0x01); /* an ACL link */
  ev8(&e, 0x00); /* not encrypted */
  ev_send(m, &e, delay);
}

/* The link comes up. `remote_began` is whether the remote connected to the
 * console - in which case it goes on to open its channels - or the console
 * to the remote, which then waits to be asked. */
static void link_up(GcMachine *m, unsigned index, int remote_began, unsigned role, uint64_t delay) {
  WiiBluetooth *bt = wii_bluetooth(m);
  WiiBtLink *link = &bt->link[index];
  uint8_t address[6];
  remote_address(index, address);
  link_clear(bt, index);
  link->state = WII_BT_LINK_UP;
  link->remote_began = remote_began ? 1u : 0u;
  link->role = (uint8_t)role;
  link->policy = wii_le16(bt->default_policy);
  link->supervision = 0x7d00;
  connection_complete(m, HCI_OK, link_handle(index), address, delay);
  if (remote_began) {
    /* Its control channel, a few packets' time after the link. */
    remote_asks(m, index, WII_BT_CHANNEL_CONTROL, PSM_HID_CONTROL, delay + 4u * WII_BT_RADIO_CYCLES);
  }
}

/* The link goes down, and the stack is told why. */
static void link_down(GcMachine *m, unsigned index, unsigned reason) {
  WiiBluetooth *bt = wii_bluetooth(m);
  Event e;
  if (bt->link[index].state != WII_BT_LINK_UP) return;
  link_clear(bt, index);
  ev_begin(&e, EVT_DISCONNECTION_COMPLETE);
  ev8(&e, HCI_OK);
  ev16(&e, link_handle(index));
  ev8(&e, reason);
  ev_send(m, &e, AFTER_STATUS);
}

/* An inquiry: every remote that is switched on and has no link (and is not
 * in the middle of paging the console) answers, a little apart, and the
 * inquiry ends when its time is up or it has all the answers asked for. */
static void start_inquiry(GcMachine *m, unsigned length, unsigned limit) {
  WiiBluetooth *bt = wii_bluetooth(m);
  const uint64_t unit = (uint64_t)GC_CPU_HZ * 128u / 100u; /* 1.28 s */
  uint64_t end = (uint64_t)(length ? length : 1u) * unit;
  uint64_t at = (uint64_t)GC_CPU_HZ / 10u;
  unsigned found = 0, i;
  Event e;
  for (i = 0; i < WII_REMOTES; i++) {
    uint8_t address[6];
    if (!remote_present(m, i) || bt->link[i].state != WII_BT_LINK_NONE) continue;
    if (limit && found >= limit) break;
    remote_address(i, address);
    if (bt->inquiry_filter == 1u) {
      unsigned k;
      int same = 1;
      for (k = 0; k < 3u; k++) {
        if ((remote_class[k] ^ bt->inquiry_match[k]) & bt->inquiry_match[3u + k]) same = 0;
      }
      if (!same) continue;
    }
    if (bt->inquiry_filter == 2u && memcmp(address, bt->inquiry_match, 6)) continue;
    if (at >= end) break;
    ev_begin(&e, bt->inquiry_mode ? EVT_INQUIRY_RESULT_RSSI : EVT_INQUIRY_RESULT);
    ev8(&e, 1);
    ev_bytes(&e, address, 6);
    ev8(&e, 0x01); /* page scan repetition mode R1 */
    ev8(&e, 0x00); /* reserved */
    if (!bt->inquiry_mode) ev8(&e, 0x00);
    ev_bytes(&e, remote_class, 3);
    ev16(&e, 0x0000);                      /* clock offset */
    if (bt->inquiry_mode) ev8(&e, 0xc8u); /* signal strength: -56 dBm */
    ev_send(m, &e, at);
    at += (uint64_t)GC_CPU_HZ / 10u;
    found++;
  }
  if (limit && found >= limit) end = at;
  ev_begin(&e, EVT_INQUIRY_COMPLETE);
  ev8(&e, HCI_OK);
  ev_send(m, &e, end);
  bt->inquiry_end = m->cpu.cycles + end;
}

static void stop_inquiry(WiiBluetooth *bt) {
  forget_events(bt, EVT_INQUIRY_RESULT);
  forget_events(bt, EVT_INQUIRY_RESULT_RSSI);
  forget_events(bt, EVT_INQUIRY_COMPLETE);
  bt->inquiry_end = 0;
}

void wii_bt_tick(GcMachine *m) {
  WiiBluetooth *bt = wii_bluetooth(m);
  WiiState *w = wii_state(m);
  const uint64_t now = m->cpu.cycles;
  unsigned i;
  if (!bt) return;

  for (i = 0; i < WII_REMOTES; i++) {
    WiiBtLink *link = &bt->link[i];
    const WiiRemoteInput *input = &w->remote_input[i];
    const uint8_t present = input->present ? 1u : 0u, buttons = input->buttons ? 1u : 0u;
    uint8_t address[6];

    if (present && !link->was_present) {
      /* Switched on: it looks for its console at once. */
      link->wants = 1;
      link->ask_at = now;
    }
    /* A button on a remote the console had switched off wakes it. */
    if (present && buttons && !link->had_buttons && link->state == WII_BT_LINK_NONE) link->wants = 1;
    if (!present && link->was_present) {
      link->wants = 0;
      if (link->state == WII_BT_LINK_UP) {
        link_down(m, i, HCI_REMOTE_POWER_OFF);
      } else if (link->state == WII_BT_LINK_ASKING) {
        link->state = WII_BT_LINK_NONE;
      }
    }
    link->was_present = present;
    link->had_buttons = buttons;

    if (link->state == WII_BT_LINK_ASKING && now - link->asked_at >= WII_BT_ACCEPT_CYCLES) {
      /* Nobody answered. */
      link->state = WII_BT_LINK_NONE;
      link->ask_at = now + WII_BT_RETRY_CYCLES;
    }
    /* The console can only be found while it is listening for pages. */
    if (link->state == WII_BT_LINK_NONE && link->wants && present && (bt->scan_enable & 2u) &&
        now >= link->ask_at) {
      remote_address(i, address);
      if (bt->auto_accept) {
        /* The stack has told the module to accept by itself. */
        link_up(m, i, 1, bt->auto_accept == 3u ? 0u : 1u, WII_BT_EVENT_CYCLES);
      } else {
        Event e;
        ev_begin(&e, EVT_CONNECTION_REQUEST);
        ev_bytes(&e, address, 6);
        ev_bytes(&e, remote_class, 3);
        ev8(&e, 0x01); /* an ACL link */
        ev_send(m, &e, WII_BT_EVENT_CYCLES);
        link->state = WII_BT_LINK_ASKING;
        link->asked_at = now;
      }
    }
    if (link->state == WII_BT_LINK_UP) wii_remote_poll(m, i);
  }

  if (bt->paging && now >= bt->paging_due) {
    /* Nobody answered the console's page. */
    bt->paging = 0;
    connection_complete(m, HCI_PAGE_TIMEOUT, 0, bt->paging_address, WII_BT_EVENT_CYCLES);
  }
  if (bt->periodic && now >= bt->periodic_next) {
    bt->periodic_next = now + (uint64_t)bt->periodic_period * ((uint64_t)GC_CPU_HZ * 128u / 100u);
    start_inquiry(m, bt->periodic_length, bt->periodic_limit);
  }
}

/* ------------------------------------------------------------------ */
/* HCI commands                                                        */

/* Opcodes: a group in the top six bits and a command in the rest. */
#define OP(group, command) (uint16_t)(((group) << 10) | (command))

/* Settings that are only stored: a Read returns what the last Write put
 * there. */
typedef struct Setting {
  uint16_t read, write;
  uint16_t offset;
  uint8_t size;
  uint8_t bit; /* in Read Local Supported Commands: byte * 8 + bit, of the Read; the Write is the next */
} Setting;

#define SETTING(read, write, field, bit) \
  {read, write, (uint16_t)offsetof(WiiBluetooth, field), (uint8_t)sizeof(((WiiBluetooth *)0)->field), bit}

static const Setting settings[] = {
    SETTING(OP(3, 0x09), OP(3, 0x0a), pin_type, 6 * 8 + 2),
    SETTING(OP(3, 0x14), OP(3, 0x13), name, 7 * 8 + 1),
    SETTING(OP(3, 0x15), OP(3, 0x16), accept_timeout, 7 * 8 + 2),
    SETTING(OP(3, 0x17), OP(3, 0x18), page_timeout, 7 * 8 + 4),
    SETTING(OP(3, 0x19), OP(3, 0x1a), scan_enable, 7 * 8 + 6),
    SETTING(OP(3, 0x1b), OP(3, 0x1c), page_scan, 8 * 8 + 0),
    SETTING(OP(3, 0x1d), OP(3, 0x1e), inquiry_scan, 8 * 8 + 2),
    SETTING(OP(3, 0x1f), OP(3, 0x20), auth_enable, 8 * 8 + 4),
    SETTING(OP(3, 0x21), OP(3, 0x22), encryption_mode, 8 * 8 + 6),
    SETTING(OP(3, 0x23), OP(3, 0x24), class_of_device, 9 * 8 + 0),
    SETTING(OP(3, 0x25), OP(3, 0x26), voice, 9 * 8 + 2),
    SETTING(OP(3, 0x29), OP(3, 0x2a), broadcast_retransmissions, 9 * 8 + 6),
    SETTING(OP(3, 0x2b), OP(3, 0x2c), hold_activity, 10 * 8 + 0),
    SETTING(OP(3, 0x2e), OP(3, 0x2f), sco_flow, 10 * 8 + 3),
    SETTING(OP(3, 0x3b), OP(3, 0x3c), page_scan_period, 11 * 8 + 5),
    SETTING(OP(3, 0x3d), OP(3, 0x3e), page_scan_mode, 11 * 8 + 7),
    SETTING(OP(3, 0x42), OP(3, 0x43), inquiry_scan_type, 12 * 8 + 4),
    SETTING(OP(3, 0x44), OP(3, 0x45), inquiry_mode, 12 * 8 + 6),
    SETTING(OP(3, 0x46), OP(3, 0x47), page_scan_type, 13 * 8 + 0),
    SETTING(OP(3, 0x48), OP(3, 0x49), afh_assessment, 13 * 8 + 2),
    SETTING(OP(2, 0x0e), OP(2, 0x0f), default_policy, 5 * 8 + 3),
};

/* The other commands the module has, by their bit in Read Local Supported
 * Commands (Core Specification, volume 2, part E, 6.26). The list is what
 * hci_command() below handles, so the two cannot disagree by much; the
 * unit test checks that every bit set here is a command that is not
 * answered "unknown". */
static const struct {
  uint16_t opcode;
  uint8_t bit;
} supported[] = {
    {OP(1, 0x01), 0 * 8 + 0},  {OP(1, 0x02), 0 * 8 + 1},  {OP(1, 0x03), 0 * 8 + 2},  {OP(1, 0x04), 0 * 8 + 3},
    {OP(1, 0x05), 0 * 8 + 4},  {OP(1, 0x06), 0 * 8 + 5},  {OP(1, 0x08), 0 * 8 + 7},  {OP(1, 0x09), 1 * 8 + 0},
    {OP(1, 0x0a), 1 * 8 + 1},  {OP(1, 0x0b), 1 * 8 + 2},  {OP(1, 0x0c), 1 * 8 + 3},  {OP(1, 0x0d), 1 * 8 + 4},
    {OP(1, 0x0e), 1 * 8 + 5},  {OP(1, 0x0f), 1 * 8 + 6},  {OP(1, 0x11), 1 * 8 + 7},  {OP(1, 0x13), 2 * 8 + 0},
    {OP(1, 0x15), 2 * 8 + 1},  {OP(1, 0x19), 2 * 8 + 3},  {OP(1, 0x1a), 2 * 8 + 4},  {OP(1, 0x1b), 2 * 8 + 5},
    {OP(1, 0x1c), 2 * 8 + 6},  {OP(1, 0x1d), 2 * 8 + 7},  {OP(1, 0x1f), 3 * 8 + 0},  {OP(2, 0x03), 4 * 8 + 2},
    {OP(1, 0x17), 2 * 8 + 2},  {OP(2, 0x01), 4 * 8 + 1},  {OP(2, 0x05), 4 * 8 + 4},  {OP(2, 0x06), 4 * 8 + 5},
    {OP(2, 0x04), 4 * 8 + 3},  {OP(2, 0x07), 4 * 8 + 6},  {OP(2, 0x09), 4 * 8 + 7},  {OP(2, 0x0b), 5 * 8 + 0},
    {OP(2, 0x0c), 5 * 8 + 1},  {OP(2, 0x0d), 5 * 8 + 2},  {OP(3, 0x01), 5 * 8 + 6},  {OP(3, 0x03), 5 * 8 + 7},
    {OP(3, 0x05), 6 * 8 + 0},  {OP(3, 0x08), 6 * 8 + 1},  {OP(3, 0x0d), 6 * 8 + 5},  {OP(3, 0x11), 6 * 8 + 6},
    {OP(3, 0x12), 6 * 8 + 7},  {OP(3, 0x27), 9 * 8 + 4},  {OP(3, 0x28), 9 * 8 + 5},  {OP(3, 0x2d), 10 * 8 + 2},
    {OP(3, 0x31), 10 * 8 + 5}, {OP(3, 0x33), 10 * 8 + 6}, {OP(3, 0x35), 10 * 8 + 7}, {OP(3, 0x36), 11 * 8 + 0},
    {OP(3, 0x37), 11 * 8 + 1}, {OP(3, 0x38), 11 * 8 + 2}, {OP(3, 0x39), 11 * 8 + 3}, {OP(3, 0x3a), 11 * 8 + 4},
    {OP(3, 0x3f), 12 * 8 + 1}, {OP(4, 0x01), 14 * 8 + 3}, {OP(4, 0x03), 14 * 8 + 5}, {OP(4, 0x04), 14 * 8 + 6},
    {OP(4, 0x05), 14 * 8 + 7}, {OP(4, 0x07), 15 * 8 + 0}, {OP(4, 0x09), 15 * 8 + 1}, {OP(5, 0x01), 15 * 8 + 2},
    {OP(5, 0x02), 15 * 8 + 3}, {OP(5, 0x03), 15 * 8 + 4}, {OP(5, 0x05), 15 * 8 + 5}, {OP(5, 0x06), 15 * 8 + 6},
    {OP(5, 0x07), 15 * 8 + 7},
};

static void supported_commands(uint8_t mask[64]) {
  unsigned i;
  memset(mask, 0, 64);
  for (i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
    mask[supported[i].bit >> 3] |= (uint8_t)(1u << (supported[i].bit & 7u));
  }
  for (i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
    unsigned bit = settings[i].bit;
    mask[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
    /* Write Local Name comes before Read Local Name; every other Write
     * follows its Read. */
    bit = settings[i].read == OP(3, 0x14) ? bit - 1u : bit + 1u;
    mask[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
  }
}

/* A command about one connection whose answer is Command Complete with
 * the status and the handle first. Returns the link's index, or -1 having
 * answered "no such connection". */
static int complete_for_link(GcMachine *m, Event *e, uint16_t opcode, const uint8_t *p, unsigned length) {
  const unsigned handle = length >= 2u ? wii_le16(p) : 0xffffu;
  const int index = link_by_handle(wii_bluetooth(m), handle);
  complete_begin(e, opcode, index < 0 ? HCI_NO_CONNECTION : HCI_OK);
  ev16(e, handle & 0x0fffu);
  if (index < 0) ev_send(m, e, WII_BT_EVENT_CYCLES);
  return index;
}

/* And one whose answer is Command Status, with an event to follow. */
static int status_for_link(GcMachine *m, uint16_t opcode, const uint8_t *p, unsigned length) {
  const int index = link_by_handle(wii_bluetooth(m), length >= 2u ? wii_le16(p) : 0xffffu);
  command_status(m, opcode, index < 0 ? HCI_NO_CONNECTION : HCI_OK);
  return index;
}

static void auth_complete(GcMachine *m, unsigned index, unsigned status) {
  WiiBtLink *link = &wii_bluetooth(m)->link[index];
  Event e;
  link->auth = WII_BT_AUTH_NONE;
  if (status == HCI_OK) link->authenticated = 1;
  ev_begin(&e, EVT_AUTH_COMPLETE);
  ev8(&e, status);
  ev16(&e, link_handle(index));
  ev_send(m, &e, AFTER_STATUS);
}

/* Complete with the status and an address, which is how the replies to a
 * key or PIN request are answered. */
static void complete_address(GcMachine *m, uint16_t opcode, unsigned status, const uint8_t *address) {
  Event e;
  complete_begin(&e, opcode, status);
  ev_bytes(&e, address, 6);
  ev_send(m, &e, WII_BT_EVENT_CYCLES);
}

void wii_bt_command(GcMachine *m, const uint8_t *packet, uint32_t size) {
  WiiBluetooth *bt = wii_bluetooth(m);
  const uint16_t opcode = wii_le16(packet);
  const uint8_t *p = packet + 3;
  unsigned length = packet[2];
  uint8_t address[6];
  Event e;
  unsigned i;
  int index;

  if (length + 3u > size) length = size - 3u;
  bt->commands++;

  /* The stored settings. */
  for (i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
    uint8_t *field = (uint8_t *)bt + settings[i].offset;
    if (opcode == settings[i].read) {
      complete_begin(&e, opcode, HCI_OK);
      ev_bytes(&e, field, settings[i].size);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    }
    if (opcode == settings[i].write) {
      memset(field, 0, settings[i].size);
      memcpy(field, p, length < settings[i].size ? length : settings[i].size);
      complete(m, opcode, HCI_OK);
      return;
    }
  }

  switch (opcode) {
    /* ---- Link control ---- */
    case OP(1, 0x01): /* Inquiry: access code (3), length, responses */
      if (length < 5u) break;
      if (bt->inquiry_end > m->cpu.cycles || bt->periodic) {
        command_status(m, opcode, HCI_DISALLOWED);
        return;
      }
      command_status(m, opcode, HCI_OK);
      start_inquiry(m, p[3], p[4]);
      return;
    case OP(1, 0x02): /* Inquiry Cancel */
      if (bt->inquiry_end <= m->cpu.cycles) {
        complete(m, opcode, HCI_DISALLOWED);
        return;
      }
      stop_inquiry(bt);
      complete(m, opcode, HCI_OK);
      return;
    case OP(1, 0x03): /* Periodic Inquiry Mode: longest and shortest period, access code, length, responses */
      if (length < 9u) break;
      bt->periodic = 1;
      bt->periodic_period = wii_le16(p);
      bt->periodic_length = p[7];
      bt->periodic_limit = p[8];
      bt->periodic_next = m->cpu.cycles + WII_BT_EVENT_CYCLES;
      complete(m, opcode, HCI_OK);
      return;
    case OP(1, 0x04): /* Exit Periodic Inquiry Mode */
      bt->periodic = 0;
      stop_inquiry(bt);
      complete(m, opcode, HCI_OK);
      return;

    case OP(1, 0x05): /* Create Connection: address, packet types, ..., allow role switch */
      if (length < 6u) break;
      index = remote_by_address(p);
      if (index >= 0 && bt->link[index].state == WII_BT_LINK_UP) {
        command_status(m, opcode, HCI_ALREADY_CONNECTED);
        return;
      }
      if (bt->paging) {
        command_status(m, opcode, HCI_DISALLOWED);
        return;
      }
      command_status(m, opcode, HCI_OK);
      if (index >= 0 && remote_present(m, (unsigned)index)) {
        /* The remote answers the page; the console is the master. */
        link_up(m, (unsigned)index, 0, 0, WII_BT_EVENT_CYCLES + 8u * WII_BT_RADIO_CYCLES);
      } else {
        bt->paging = 1;
        memcpy(bt->paging_address, p, 6);
        bt->paging_due = m->cpu.cycles + (uint64_t)wii_le16(bt->page_timeout) * GC_CPU_HZ / 1600u;
      }
      return;
    case OP(1, 0x08): /* Create Connection Cancel */
      if (length < 6u) break;
      if (bt->paging && !memcmp(bt->paging_address, p, 6)) {
        bt->paging = 0;
        complete_address(m, opcode, HCI_OK, p);
        connection_complete(m, HCI_NO_CONNECTION, 0, p, AFTER_STATUS);
      } else {
        index = remote_by_address(p);
        complete_address(m, opcode,
                         index >= 0 && bt->link[index].state == WII_BT_LINK_UP ? HCI_ALREADY_CONNECTED
                                                                               : HCI_NO_CONNECTION,
                         p);
      }
      return;

    case OP(1, 0x06): /* Disconnect: handle, reason */
      index = status_for_link(m, opcode, p, length);
      if (index < 0) return;
      /* The console hung up on purpose, and a remote told to go away
       * switches off: it comes back when someone presses a button. */
      bt->link[index].wants = 0;
      link_down(m, (unsigned)index, HCI_LOCAL_HOST_ENDED);
      return;

    case OP(1, 0x09): /* Accept Connection Request: address, role */
      if (length < 7u) break;
      index = remote_by_address(p);
      if (index < 0 || bt->link[index].state != WII_BT_LINK_ASKING) {
        command_status(m, opcode, HCI_DISALLOWED);
        return;
      }
      command_status(m, opcode, HCI_OK);
      if (p[6] == 0u) {
        /* "I will be the master": the two swap roles, and the stack hears
         * of that before it hears the link is up. */
        ev_begin(&e, EVT_ROLE_CHANGE);
        ev8(&e, HCI_OK);
        ev_bytes(&e, p, 6);
        ev8(&e, 0x00);
        ev_send(m, &e, AFTER_STATUS);
      }
      link_up(m, (unsigned)index, 1, p[6] ? 1u : 0u, AFTER_STATUS);
      return;
    case OP(1, 0x0a): /* Reject Connection Request: address, reason */
      if (length < 7u) break;
      index = remote_by_address(p);
      if (index < 0 || bt->link[index].state != WII_BT_LINK_ASKING) {
        command_status(m, opcode, HCI_DISALLOWED);
        return;
      }
      command_status(m, opcode, HCI_OK);
      bt->link[index].state = WII_BT_LINK_NONE;
      bt->link[index].ask_at = m->cpu.cycles + WII_BT_RETRY_CYCLES;
      connection_complete(m, p[6], 0, p, AFTER_STATUS);
      return;

    case OP(1, 0x0b): /* Link Key Request Reply: address, key */
      if (length < 22u) break;
      complete_address(m, opcode, HCI_OK, p);
      index = remote_by_address(p);
      if (index >= 0 && bt->link[index].state == WII_BT_LINK_UP && bt->link[index].auth == WII_BT_AUTH_KEY) {
        uint8_t key[16];
        remote_key((unsigned)index, key);
        if (memcmp(key, p + 6, 16)) {
          if (m->log) fprintf(m->log, "wii: the game's link key for remote %d is not the remote's\n", index);
          auth_complete(m, (unsigned)index, HCI_AUTH_FAILURE);
        } else {
          auth_complete(m, (unsigned)index, HCI_OK);
        }
      }
      return;
    case OP(1, 0x0c): /* Link Key Request Negative Reply: the stack has no key, so a PIN it is */
      if (length < 6u) break;
      complete_address(m, opcode, HCI_OK, p);
      index = remote_by_address(p);
      if (index >= 0 && bt->link[index].state == WII_BT_LINK_UP && bt->link[index].auth == WII_BT_AUTH_KEY) {
        bt->link[index].auth = WII_BT_AUTH_PIN;
        ev_begin(&e, EVT_PIN_REQUEST);
        ev_bytes(&e, p, 6);
        ev_send(m, &e, AFTER_STATUS);
      }
      return;
    case OP(1, 0x0d): /* PIN Code Request Reply: address, length, PIN */
      if (length < 23u) break;
      complete_address(m, opcode, HCI_OK, p);
      index = remote_by_address(p);
      if (index >= 0 && bt->link[index].state == WII_BT_LINK_UP && bt->link[index].auth == WII_BT_AUTH_PIN) {
        /* A remote's PIN is six bytes of address: the console's when it
         * was put in pairing mode by its sync button, its own when by
         * holding 1 and 2. This one takes either. */
        uint8_t console[6], key[16];
        console_address(console);
        if (p[6] == 6u && (!memcmp(p + 7, console, 6) || !memcmp(p + 7, p, 6))) {
          remote_key((unsigned)index, key);
          ev_begin(&e, EVT_LINK_KEY_NOTIFICATION);
          ev_bytes(&e, p, 6);
          ev_bytes(&e, key, 16);
          ev8(&e, 0x00); /* a combination key */
          ev_send(m, &e, AFTER_STATUS);
          auth_complete(m, (unsigned)index, HCI_OK);
        } else {
          auth_complete(m, (unsigned)index, HCI_AUTH_FAILURE);
        }
      }
      return;
    case OP(1, 0x0e): /* PIN Code Request Negative Reply */
      if (length < 6u) break;
      complete_address(m, opcode, HCI_OK, p);
      index = remote_by_address(p);
      if (index >= 0 && bt->link[index].state == WII_BT_LINK_UP && bt->link[index].auth == WII_BT_AUTH_PIN) {
        auth_complete(m, (unsigned)index, HCI_KEY_MISSING);
      }
      return;

    case OP(1, 0x0f): /* Change Connection Packet Type: handle, types */
      index = status_for_link(m, opcode, p, length);
      if (index < 0 || length < 4u) return;
      ev_begin(&e, EVT_PACKET_TYPE_CHANGED);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev16(&e, wii_le16(p + 2));
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(1, 0x11): /* Authentication Requested: the remote asks for the link key */
      index = status_for_link(m, opcode, p, length);
      if (index < 0) return;
      bt->link[index].auth = WII_BT_AUTH_KEY;
      remote_address((unsigned)index, address);
      ev_begin(&e, EVT_LINK_KEY_REQUEST);
      ev_bytes(&e, address, 6);
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(1, 0x13): /* Set Connection Encryption: handle, on or off */
      index = status_for_link(m, opcode, p, length);
      if (index < 0 || length < 3u) return;
      bt->link[index].encrypted = p[2] ? 1u : 0u;
      ev_begin(&e, EVT_ENCRYPTION_CHANGE);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev8(&e, p[2] ? 1u : 0u);
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(1, 0x15): /* Change Connection Link Key */
      index = status_for_link(m, opcode, p, length);
      if (index < 0) return;
      ev_begin(&e, EVT_CHANGE_LINK_KEY_COMPLETE);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev_send(m, &e, AFTER_STATUS);
      return;

    case OP(1, 0x19): { /* Remote Name Request: address, ... */
      uint8_t name[248];
      if (length < 6u) break;
      command_status(m, opcode, HCI_OK);
      index = remote_by_address(p);
      memset(name, 0, sizeof(name));
      ev_begin(&e, EVT_REMOTE_NAME);
      if (index >= 0 && remote_present(m, (unsigned)index)) {
        memcpy(name, WII_REMOTE_NAME, sizeof(WII_REMOTE_NAME));
        ev8(&e, HCI_OK);
        ev_bytes(&e, p, 6);
        ev_bytes(&e, name, sizeof(name));
        ev_send(m, &e, WII_BT_EVENT_CYCLES + 8u * WII_BT_RADIO_CYCLES);
      } else {
        ev8(&e, HCI_PAGE_TIMEOUT);
        ev_bytes(&e, p, 6);
        ev_bytes(&e, name, sizeof(name));
        ev_send(m, &e, (uint64_t)wii_le16(bt->page_timeout) * GC_CPU_HZ / 1600u);
      }
      return;
    }
    case OP(1, 0x1a): /* Remote Name Request Cancel */
      if (length < 6u) break;
      forget_events(bt, EVT_REMOTE_NAME);
      complete_address(m, opcode, HCI_OK, p);
      return;
    case OP(1, 0x1b): /* Read Remote Supported Features */
      index = status_for_link(m, opcode, p, length);
      if (index < 0) return;
      ev_begin(&e, EVT_REMOTE_FEATURES);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev_bytes(&e, remote_features, 8);
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(1, 0x1c): /* Read Remote Extended Features: handle, page */
      index = status_for_link(m, opcode, p, length);
      if (index < 0) return;
      ev_begin(&e, EVT_REMOTE_EXTENDED_FEATURES);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev8(&e, length >= 3u ? p[2] : 0u);
      ev8(&e, 0); /* the highest page there is */
      if (length >= 3u && p[2] == 0u) {
        ev_bytes(&e, remote_features, 8);
      } else {
        ev32(&e, 0);
        ev32(&e, 0);
      }
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(1, 0x1d): /* Read Remote Version Information */
      index = status_for_link(m, opcode, p, length);
      if (index < 0) return;
      ev_begin(&e, EVT_REMOTE_VERSION);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev8(&e, REMOTE_LMP_VERSION);
      ev16(&e, BROADCOM);
      ev16(&e, REMOTE_LMP_SUBVERSION);
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(1, 0x1f): /* Read Clock Offset */
      index = status_for_link(m, opcode, p, length);
      if (index < 0) return;
      ev_begin(&e, EVT_CLOCK_OFFSET);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev16(&e, 0x0000);
      ev_send(m, &e, AFTER_STATUS);
      return;

    case OP(1, 0x17): /* Master Link Key: which key the piconet uses */
      if (length < 1u) break;
      command_status(m, opcode, HCI_OK);
      for (i = 0; i < WII_REMOTES; i++) {
        if (bt->link[i].state != WII_BT_LINK_UP) continue;
        ev_begin(&e, EVT_MASTER_LINK_KEY_COMPLETE);
        ev8(&e, HCI_OK);
        ev16(&e, link_handle(i));
        ev8(&e, p[0]);
        ev_send(m, &e, AFTER_STATUS);
      }
      return;

    /* ---- Link policy ---- */
    case OP(2, 0x01): /* Hold Mode: handle, longest and shortest interval */
    case OP(2, 0x05): /* Park State: the same */
    case OP(2, 0x06): /* Exit Park State */
      /* Modes a console does not put a remote in. The link says it has
       * changed and carries on exactly as before. */
      index = status_for_link(m, opcode, p, length);
      if (index < 0) return;
      ev_begin(&e, EVT_MODE_CHANGE);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev8(&e, opcode == OP(2, 0x01) ? 1u : opcode == OP(2, 0x05) ? 3u : 0u);
      ev16(&e, opcode != OP(2, 0x06) && length >= 4u ? wii_le16(p + 2) : 0u);
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(2, 0x03): /* Sniff Mode: handle, longest and shortest interval, attempt, timeout */
      index = status_for_link(m, opcode, p, length);
      if (index < 0 || length < 4u) return;
      bt->link[index].mode = 2;
      bt->link[index].sniff_interval = wii_le16(p + 2);
      ev_begin(&e, EVT_MODE_CHANGE);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev8(&e, 2);
      ev16(&e, bt->link[index].sniff_interval);
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(2, 0x04): /* Exit Sniff Mode */
      index = status_for_link(m, opcode, p, length);
      if (index < 0) return;
      bt->link[index].mode = 0;
      ev_begin(&e, EVT_MODE_CHANGE);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev8(&e, 0);
      ev16(&e, 0);
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(2, 0x07): /* QoS Setup: handle, flags, service type, four rates */
      index = status_for_link(m, opcode, p, length);
      if (index < 0 || length < 20u) return;
      ev_begin(&e, EVT_QOS_SETUP);
      ev8(&e, HCI_OK);
      ev16(&e, link_handle((unsigned)index));
      ev_bytes(&e, p + 2, 18); /* what was asked for is what it gets */
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(2, 0x09): /* Role Discovery */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      ev8(&e, bt->link[index].role);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(2, 0x0b): /* Switch Role: address, role */
      if (length < 7u) break;
      index = remote_by_address(p);
      if (index < 0 || bt->link[index].state != WII_BT_LINK_UP) {
        command_status(m, opcode, HCI_NO_CONNECTION);
        return;
      }
      command_status(m, opcode, HCI_OK);
      bt->link[index].role = p[6] ? 1u : 0u;
      ev_begin(&e, EVT_ROLE_CHANGE);
      ev8(&e, HCI_OK);
      ev_bytes(&e, p, 6);
      ev8(&e, bt->link[index].role);
      ev_send(m, &e, AFTER_STATUS);
      return;
    case OP(2, 0x0c): /* Read Link Policy Settings */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      ev16(&e, bt->link[index].policy);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(2, 0x0d): /* Write Link Policy Settings: handle, settings */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      if (length >= 4u) bt->link[index].policy = wii_le16(p + 2);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;

    /* ---- Controller and baseband ---- */
    case OP(3, 0x01): /* Set Event Mask */
      memcpy(bt->event_mask, p, length < 8u ? length : 8u);
      complete(m, opcode, HCI_OK);
      return;
    case OP(3, 0x03): /* Reset */
      hci_reset(m);
      complete(m, opcode, HCI_OK);
      return;
    case OP(3, 0x05): /* Set Event Filter: type, condition type, condition */
      if (length < 1u) break;
      if (p[0] == 0u) {
        bt->auto_accept = 0;
        bt->inquiry_filter = 0;
      } else if (p[0] == 1u && length >= 2u) {
        /* Which inquiry answers to pass on: all, a class, an address. */
        bt->inquiry_filter = p[1] <= 2u ? p[1] : 0u;
        memset(bt->inquiry_match, 0, sizeof(bt->inquiry_match));
        if (p[1] && length >= 8u) memcpy(bt->inquiry_match, p + 2, 6);
      } else if (p[0] == 2u && length >= 3u) {
        /* What to do with a connection request: the last byte says ask
         * (1), accept (2) or accept and switch roles (3). A remote matches
         * any condition a Wii's software would set. */
        const unsigned accept = p[length - 1u];
        bt->auto_accept = accept >= 2u && accept <= 3u ? (uint8_t)accept : 0u;
      }
      complete(m, opcode, HCI_OK);
      return;
    case OP(3, 0x08): /* Flush */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index >= 0) ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;

    case OP(3, 0x0d): { /* Read Stored Link Key: address, all or one */
      unsigned count = 0, in_event = 0;
      if (length < 7u) break;
      ev_begin(&e, EVT_RETURN_LINK_KEYS);
      ev8(&e, 0);
      for (i = 0; i < WII_BT_KEYS; i++) {
        if (!bt->keys[i].used) continue;
        if (!p[6] && memcmp(bt->keys[i].address, p, 6)) continue;
        ev_bytes(&e, bt->keys[i].address, 6);
        ev_bytes(&e, bt->keys[i].key, 16);
        count++;
        /* Eleven keys are all one event has room for. */
        if (++in_event == 11u) {
          e.data[2] = (uint8_t)in_event;
          ev_send(m, &e, WII_BT_EVENT_CYCLES);
          ev_begin(&e, EVT_RETURN_LINK_KEYS);
          ev8(&e, 0);
          in_event = 0;
        }
      }
      if (in_event) {
        e.data[2] = (uint8_t)in_event;
        ev_send(m, &e, WII_BT_EVENT_CYCLES);
      }
      complete_begin(&e, opcode, HCI_OK);
      ev16(&e, WII_BT_KEYS);
      ev16(&e, count);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    }
    case OP(3, 0x11): { /* Write Stored Link Key: how many, then address and key each */
      unsigned written = 0, k;
      if (length < 1u) break;
      for (k = 0; k < p[0] && 1u + (k + 1u) * 22u <= length; k++) {
        const uint8_t *entry = p + 1u + k * 22u;
        WiiBtKey *key = stored_key(bt, entry);
        for (i = 0; !key && i < WII_BT_KEYS; i++) {
          if (!bt->keys[i].used) key = &bt->keys[i];
        }
        if (!key) break;
        key->used = 1;
        memcpy(key->address, entry, 6);
        memcpy(key->key, entry + 6, 16);
        written++;
      }
      complete_begin(&e, opcode, HCI_OK);
      ev8(&e, written);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    }
    case OP(3, 0x12): { /* Delete Stored Link Key: address, all or one */
      unsigned deleted = 0;
      if (length < 7u) break;
      for (i = 0; i < WII_BT_KEYS; i++) {
        if (!bt->keys[i].used) continue;
        if (!p[6] && memcmp(bt->keys[i].address, p, 6)) continue;
        bt->keys[i].used = 0;
        deleted++;
      }
      complete_begin(&e, opcode, HCI_OK);
      ev16(&e, deleted);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    }

    case OP(3, 0x27): /* Read Automatic Flush Timeout */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      ev16(&e, bt->link[index].flush_timeout);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(3, 0x28): /* Write Automatic Flush Timeout */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      if (length >= 4u) bt->link[index].flush_timeout = wii_le16(p + 2);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(3, 0x2d): /* Read Transmit Power Level */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      ev8(&e, 0);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(3, 0x31): /* Set Host Controller To Host Flow Control */
      bt->host_flow = length ? p[0] : 0u;
      bt->host_acl_out = 0;
      complete(m, opcode, HCI_OK);
      return;
    case OP(3, 0x33): /* Host Buffer Size: ACL length, SCO length, ACL packets, SCO packets */
      if (length < 7u) break;
      bt->host_acl_length = wii_le16(p);
      bt->host_acl_packets = wii_le16(p + 3);
      complete(m, opcode, HCI_OK);
      return;
    case OP(3, 0x35): /* Host Number Of Completed Packets: no event answers this one */
      for (i = 0; length >= 1u && i < p[0] && 1u + (i + 1u) * 4u <= length; i++) {
        const unsigned count = wii_le16(p + 3u + i * 4u);
        bt->host_acl_out = (uint16_t)(bt->host_acl_out > count ? bt->host_acl_out - count : 0u);
      }
      return;
    case OP(3, 0x36): /* Read Link Supervision Timeout */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      ev16(&e, bt->link[index].supervision);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(3, 0x37): /* Write Link Supervision Timeout */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      if (length >= 4u) bt->link[index].supervision = wii_le16(p + 2);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(3, 0x38): /* Read Number Of Supported IAC */
      complete_begin(&e, opcode, HCI_OK);
      ev8(&e, 2);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(3, 0x39): /* Read Current IAC LAP */
      complete_begin(&e, opcode, HCI_OK);
      ev8(&e, bt->iac_count);
      ev_bytes(&e, bt->iac, 3u * bt->iac_count);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(3, 0x3a): /* Write Current IAC LAP: how many, then three bytes each */
      if (length < 1u || p[0] < 1u || p[0] > 2u || length < 1u + 3u * p[0]) {
        complete(m, opcode, HCI_INVALID_PARAMETERS);
        return;
      }
      bt->iac_count = p[0];
      memcpy(bt->iac, p + 1, 3u * p[0]);
      complete(m, opcode, HCI_OK);
      return;
    case OP(3, 0x3f): /* Set AFH Host Channel Classification */
      complete(m, opcode, HCI_OK);
      return;

    /* ---- Information ---- */
    case OP(4, 0x01): /* Read Local Version Information */
      complete_begin(&e, opcode, HCI_OK);
      ev8(&e, MODULE_HCI_VERSION);
      ev16(&e, MODULE_HCI_REVISION);
      ev8(&e, MODULE_LMP_VERSION);
      ev16(&e, BROADCOM);
      ev16(&e, MODULE_LMP_SUBVERSION);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(4, 0x02): { /* Read Local Supported Commands */
      uint8_t mask[64];
      supported_commands(mask);
      complete_begin(&e, opcode, HCI_OK);
      ev_bytes(&e, mask, 64);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    }
    case OP(4, 0x03): /* Read Local Supported Features */
      complete_begin(&e, opcode, HCI_OK);
      ev_bytes(&e, module_features, 8);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(4, 0x04): /* Read Local Extended Features: page */
      complete_begin(&e, opcode, HCI_OK);
      ev8(&e, length ? p[0] : 0u);
      ev8(&e, 0);
      if (!length || p[0] == 0u) {
        ev_bytes(&e, module_features, 8);
      } else {
        ev32(&e, 0);
        ev32(&e, 0);
      }
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(4, 0x05): /* Read Buffer Size */
      complete_begin(&e, opcode, HCI_OK);
      ev16(&e, WII_BT_ACL_LENGTH);
      ev8(&e, WII_BT_SCO_LENGTH);
      ev16(&e, WII_BT_ACL_PACKETS);
      ev16(&e, WII_BT_SCO_PACKETS);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(4, 0x07): /* Read Country Code */
      complete_begin(&e, opcode, HCI_OK);
      ev8(&e, 0);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(4, 0x09): /* Read BD_ADDR */
      console_address(address);
      complete_address(m, opcode, HCI_OK, address);
      return;

    /* ---- Status ---- */
    case OP(5, 0x01): /* Read Failed Contact Counter */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      ev16(&e, 0);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(5, 0x02): /* Reset Failed Contact Counter */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index >= 0) ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(5, 0x03): /* Get Link Quality: as good as it gets */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      ev8(&e, 0xff);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(5, 0x05): /* Read RSSI: inside the range the module likes */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      ev8(&e, 0);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(5, 0x06): /* Read AFH Channel Map: hopping, every channel in use */
      index = complete_for_link(m, &e, opcode, p, length);
      if (index < 0) return;
      ev8(&e, 1);
      for (i = 0; i < 9u; i++) ev8(&e, 0xff);
      ev8(&e, 0x7f);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    case OP(5, 0x07): { /* Read Clock: which, handle. The clock ticks 3,200 times a second. */
      const unsigned handle = length >= 3u ? wii_le16(p + 1) : 0u;
      complete_begin(&e, opcode, HCI_OK);
      ev16(&e, handle & 0x0fffu);
      ev32(&e, (uint32_t)(m->cpu.cycles * 3200u / GC_CPU_HZ) & 0x0fffffffu);
      ev16(&e, 0);
      ev_send(m, &e, WII_BT_EVENT_CYCLES);
      return;
    }

    /* ---- Broadcom's own ---- */
    case 0xfc4c: /* write to the module's memory: a retail game's firmware patch, in pieces */
    case 0xfc4f: /* and the list of where the patch hooks in, or its removal */
    case 0xfc57: /* a link's priority over the others */
      /* The patch fixes faults in the real module's firmware. There is no
       * firmware here to have them, so it is taken and dropped. */
      complete(m, opcode, HCI_OK);
      return;

    default: break;
  }

  /* Not a command this module has - or one too short to be it. A game
   * leaning on it will misbehave, so it is said out loud. */
  bt->unknown_commands++;
  wii_state(m)->ios.unknown_commands++;
  if (m->log && bt->unknown_commands <= 32) {
    fprintf(m->log, "wii: Bluetooth command %04x (group %02x, command %03x, %u bytes) is not implemented\n",
            opcode, opcode >> 10, opcode & 0x3ffu, length);
  }
  complete(m, opcode, HCI_UNKNOWN_COMMAND);
}
