/*
 * /dev/usb/oh1/57e/305: the console's Bluetooth module, as IOS shows it.
 *
 * The module is a USB device (vendor 0x057e, product 0x0305) on the second
 * processor's own host controller, and IOS gives a game the raw endpoints
 * and nothing more. A Bluetooth controller on USB has three that matter
 * (Bluetooth Core, Host Controller Interface, USB transport):
 *
 *   control        HCI commands, as a class request (bmRequestType 0x20)
 *   interrupt 0x81 HCI events, inward
 *   bulk 0x02/0x82 ACL data, out and in
 *
 * and IOS's request for each is an ioctlv:
 *
 *   0  control    in: bmRequestType(1) bRequest(1) wValue(2) wIndex(2)
 *                     wLength(2) and a zero byte; then the data
 *   1  bulk       in: endpoint(1) length(2); then the data
 *   2  interrupt  in: endpoint(1) length(2); then the data
 *
 * The three 16-bit fields of a control request are little-endian, as they
 * are on the bus; a bulk or interrupt length is the processor's own
 * big-endian. The data is always the last vector and always counted as the
 * device's to fill, whichever way it goes. Both clients agree on all of
 * this - libogc's usb.c and lwbt's physbusif.c, and the SDK's USB library
 * that Broadcom's stack sits on - and they are the reference.
 *
 * A game's stack keeps a request waiting on each inward endpoint at all
 * times. One with nothing to deliver is held (WII_IOS_LATER) and answered
 * when there is something, which is how an event or a remote's report
 * reaches a game at a time of the module's choosing.
 *
 * Time. The kernel calls a device when a request arrives and once a field,
 * and an answer can be timed to the cycle (wii_ios_reply's delay). So a
 * waiting request is answered as soon as something is known to be ready
 * within the next field - written into its buffer now, delivered at the
 * cycle it is ready - and anything further off waits for a later look. A
 * remote reporting every five milliseconds is always inside that horizon,
 * so its reports arrive on their own clock, not the field's.
 *
 * What that costs: a waiting request that has been promised to something
 * cannot be given to something sooner that turns up afterwards, which then
 * goes next. And a change in a remote's input is seen at the next request
 * or the next field, because nothing tells this file when wii_remote_set is
 * called. wii_bt_input_changed() is here for that: one call from
 * wii_remote_set (wii.c) would close the gap.
 */

#include <stdio.h>
#include <string.h>

#include "wii.h"

enum { USB_CONTROL = 0, USB_BULK = 1, USB_INTERRUPT = 2 };
/* The plain requests libogc's USB client makes of any device. */
enum { USB_SUSPEND = 5, USB_RESUME = 6, USB_REMOVAL_HOOK = 26 };

enum { ENDPOINT_EVENTS = 0x81, ENDPOINT_ACL_OUT = 0x02, ENDPOINT_ACL_IN = 0x82 };

/* What a request still waiting is answered with when the device is closed
 * under it. */
#define USB_CANCELLED WII_IPC_EINVAL

static void unknown(GcMachine *m, const char *what, uint32_t a, uint32_t b) {
  WiiIos *ios = &wii_state(m)->ios;
  ios->unknown_commands++;
  if (m->log && ios->unknown_commands <= 32) {
    fprintf(m->log, "wii: the Bluetooth module was sent %s (%x, %x), pc %08x\n", what, a, b, m->cpu.pc);
  }
}

static void trace(GcMachine *m, const char *what, const uint8_t *data, unsigned size) {
  unsigned i;
  if (!m->trace_io || !m->log) return;
  fprintf(m->log, "wii: bt %s @%llu:", what, (unsigned long long)m->cpu.cycles);
  for (i = 0; i < size && i < 48u; i++) fprintf(m->log, " %02x", data[i]);
  if (size > 48u) fprintf(m->log, " ... (%u)", size);
  fprintf(m->log, "\n");
}

/* ------------------------------------------------------------------ */
/* Requests that wait                                                  */

static int hold(GcMachine *m, WiiBtWaiting *list, const WiiIosCall *call, const WiiIosVector *data) {
  unsigned i;
  for (i = 0; i < WII_BT_WAITING; i++) {
    if (list[i].request) continue;
    list[i].request = call->request;
    list[i].buffer = data->address;
    list[i].size = data->size;
    return 1;
  }
  /* A stack keeps one or two waiting; more than four is a stack that has
   * lost count. */
  if (m->log) fprintf(m->log, "wii: more than %u requests waiting on a Bluetooth endpoint\n", WII_BT_WAITING);
  return 0;
}

static void release(WiiBtWaiting *list) {
  unsigned i;
  for (i = 0; i + 1u < WII_BT_WAITING; i++) list[i] = list[i + 1u];
  memset(&list[WII_BT_WAITING - 1u], 0, sizeof(list[0]));
}

/* Answer the oldest waiting request with `size` bytes, timed for `due`. */
static void answer(GcMachine *m, WiiBtWaiting *list, uint64_t *last, const uint8_t *data, unsigned size,
                   uint64_t due, const char *what) {
  const uint64_t now = m->cpu.cycles;
  uint8_t *buffer = wii_ios_memory(m, list[0].buffer, list[0].size);
  /* Never before the request's own acknowledgement, and never before the
   * answer that went ahead of it. */
  if (due < now + WII_IOS_REPLY_CYCLES) due = now + WII_IOS_REPLY_CYCLES;
  if (due < *last) due = *last;
  *last = due;
  if (size > list[0].size || !buffer) {
    /* A buffer too small for a packet: the stack asked for less than the
     * module can send. Say so rather than hand over part of one. */
    if (m->log) {
      fprintf(m->log, "wii: a Bluetooth %s of %u bytes does not fit the %u the game is waiting with\n", what,
              size, list[0].size);
    }
    size = buffer ? list[0].size : 0;
  }
  if (buffer && size) memcpy(buffer, data, size);
  trace(m, what, data, size);
  wii_ios_reply(m, list[0].request, (int32_t)size, due - now);
  release(list);
}

/* The earliest event queued, or NULL. */
static WiiBtEvent *next_event(WiiBluetooth *bt) {
  WiiBtEvent *best = NULL;
  unsigned i;
  for (i = 0; i < WII_BT_EVENT_SLOTS; i++) {
    WiiBtEvent *e = &bt->events[i];
    if (!e->used) continue;
    if (!best || e->due < best->due || (e->due == best->due && e->order < best->order)) best = e;
  }
  return best;
}

void wii_bt_pump(GcMachine *m) {
  WiiBluetooth *bt = wii_bluetooth(m);
  const uint64_t horizon = m->cpu.cycles + GC_CYCLES_PER_FIELD;
  uint8_t packet[WII_BT_ACL_BYTES];
  uint64_t when = 0;
  unsigned size;
  if (!bt) return;

  while (bt->intr[0].request) {
    WiiBtEvent *e = next_event(bt);
    if (!e || e->due > horizon) break;
    answer(m, bt->intr, &bt->intr_last, e->data, e->length, e->due, "event");
    e->used = 0;
    bt->events_sent++;
  }
  while (bt->bulk[0].request) {
    /* The soonest this request can be answered, which a remote needs to
     * know: its next report is timed from when this one really goes. */
    uint64_t soonest = m->cpu.cycles + WII_IOS_REPLY_CYCLES;
    if (soonest < bt->bulk_last) soonest = bt->bulk_last;
    size = wii_bt_next_acl(m, packet, soonest, &when, 0);
    if (!size || when > horizon) break;
    size = wii_bt_next_acl(m, packet, soonest, &when, 1);
    answer(m, bt->bulk, &bt->bulk_last, packet, size, when, "acl in");
    bt->acl_in++;
  }
}

void wii_bt_input_changed(GcMachine *m) {
  if (!wii_bluetooth(m)) return;
  wii_bt_tick(m);
  wii_bt_pump(m);
}

/* ------------------------------------------------------------------ */
/* The device                                                          */

static int32_t bt_open(WiiIosCall *call, const char *path, uint32_t mode) {
  (void)call;
  (void)path;
  (void)mode;
  return 0;
}

static void cancel(GcMachine *m, WiiBtWaiting *list) {
  while (list[0].request) {
    wii_ios_reply(m, list[0].request, USB_CANCELLED, WII_IOS_REPLY_CYCLES);
    release(list);
  }
}

/* Closing the device answers whatever was still waiting on it. The module
 * itself goes on as it was: it is powered whether or not anyone is
 * listening. */
static int32_t bt_close(WiiIosCall *call) {
  GcMachine *m = call->m;
  WiiBluetooth *bt = wii_bluetooth(m);
  cancel(m, bt->intr);
  cancel(m, bt->bulk);
  if (bt->removal_request) {
    wii_ios_reply(m, bt->removal_request, USB_CANCELLED, WII_IOS_REPLY_CYCLES);
    bt->removal_request = 0;
  }
  /* The close itself is answered after them. */
  call->delay = WII_IOS_REPLY_CYCLES + 1u;
  return 0;
}

static int32_t bt_ioctl(WiiIosCall *call, uint32_t command, uint8_t *in, uint32_t in_size, uint8_t *out,
                        uint32_t out_size) {
  WiiBluetooth *bt = wii_bluetooth(call->m);
  (void)in;
  (void)in_size;
  (void)out;
  (void)out_size;
  switch (command) {
    case USB_SUSPEND:
    case USB_RESUME: return 0;
    case USB_REMOVAL_HOOK:
      /* Answered when the device is unplugged, which the console's own
       * module never is. */
      if (bt->removal_request) return WII_IPC_EEXIST;
      bt->removal_request = call->request;
      return WII_IOS_LATER;
    default: break;
  }
  unknown(call->m, "an ioctl it does not have", command, in_size);
  return WII_IPC_EINVAL;
}

static int32_t bt_ioctlv(WiiIosCall *call, uint32_t command, unsigned in_count, unsigned io_count,
                         const WiiIosVector *v) {
  GcMachine *m = call->m;
  WiiBluetooth *bt = wii_bluetooth(m);
  const uint8_t *field[6];
  unsigned i;

  if (command == USB_CONTROL) {
    const uint8_t *data;
    uint32_t length;
    if (in_count != 6u || io_count != 1u) {
      unknown(m, "a control request of the wrong shape", in_count, io_count);
      return WII_IPC_EINVAL;
    }
    for (i = 0; i < 6u; i++) field[i] = wii_ios_buffer(m, &v[i]);
    if (!field[0] || !field[1] || !field[4] || v[4].size < 2u) return WII_IPC_EINVAL;
    length = wii_le16(field[4]);
    data = wii_ios_buffer(m, &v[6]);
    /* The one class request a Bluetooth controller has: here is an HCI
     * command. */
    if (field[0][0] != 0x20u || field[1][0] != 0u) {
      unknown(m, "a control request that is not an HCI command", field[0][0], field[1][0]);
      return WII_IPC_EINVAL;
    }
    if (length > v[6].size || (length && !data) || length < 3u) {
      unknown(m, "an HCI command shorter than its header", length, v[6].size);
      return WII_IPC_EINVAL;
    }
    trace(m, "command", data, length);
    wii_bt_command(m, data, length);
    wii_bt_tick(m);
    wii_bt_pump(m);
    return (int32_t)length;
  }

  if (command == USB_BULK || command == USB_INTERRUPT) {
    const uint8_t *endpoint;
    if (in_count != 2u || io_count != 1u) {
      unknown(m, "a transfer of the wrong shape", in_count, io_count);
      return WII_IPC_EINVAL;
    }
    endpoint = wii_ios_buffer(m, &v[0]);
    if (!endpoint) return WII_IPC_EINVAL;

    if (command == USB_INTERRUPT && endpoint[0] == ENDPOINT_EVENTS) {
      if (!hold(m, bt->intr, call, &v[2])) return WII_IPC_EQUEUEFULL;
      wii_bt_tick(m);
      wii_bt_pump(m);
      return WII_IOS_LATER;
    }
    if (command == USB_BULK && endpoint[0] == ENDPOINT_ACL_IN) {
      if (!hold(m, bt->bulk, call, &v[2])) return WII_IPC_EQUEUEFULL;
      wii_bt_tick(m);
      wii_bt_pump(m);
      return WII_IOS_LATER;
    }
    if (command == USB_BULK && endpoint[0] == ENDPOINT_ACL_OUT) {
      const uint8_t *data = wii_ios_buffer(m, &v[2]);
      if (!data || v[2].size < 4u) {
        unknown(m, "an ACL packet shorter than its header", v[2].size, 0);
        return WII_IPC_EINVAL;
      }
      trace(m, "acl out", data, v[2].size);
      bt->acl_out++;
      wii_bt_acl_from_host(m, data, v[2].size);
      wii_bt_tick(m);
      wii_bt_pump(m);
      return (int32_t)v[2].size;
    }
    unknown(m, "a transfer on an endpoint it does not have", command, endpoint[0]);
    return WII_IPC_EINVAL;
  }

  unknown(m, "an ioctlv it does not have", command, in_count);
  return WII_IPC_EINVAL;
}

/* Once a field: the remotes' input may have changed, and whatever was too
 * far off to promise at the last look may be near enough now. */
static void bt_field(GcMachine *m) {
  wii_bt_tick(m);
  wii_bt_pump(m);
}

static void bt_reset(GcMachine *m) { wii_bt_reset(m); }

const WiiIosDevice wii_ios_bluetooth = {
    "/dev/usb/oh1/57e/305", 0, bt_open, bt_close, NULL, NULL, NULL, bt_ioctl, bt_ioctlv, bt_field, bt_reset,
};
