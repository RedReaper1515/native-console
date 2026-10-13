/*
 * The console's Bluetooth module and the remotes behind it, driven the way
 * a game drives them: this test is the game's Bluetooth stack. It opens
 * /dev/usb/oh1/57e/305 through the real mailbox, keeps a request waiting
 * on each inward endpoint as lwbt and Broadcom's stack both do, sends HCI
 * commands and ACL packets, and checks every byte that comes back and the
 * cycle it came back at.
 *
 * No guest code runs: time is moved by hand, to the next thing IOS owes
 * and to each field's end, so a failure names a packet rather than a
 * symptom.
 *
 * What the expected bytes are checked against is the specifications
 * (Bluetooth Core for HCI, L2CAP and SDP; the HID profile) and the
 * published descriptions of a remote - not this implementation's own
 * output. Where a figure is this implementation's choice (the calibration,
 * the room the camera's dots assume) the test works the expected value
 * out again its own way.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_bus.h"
#include "wii.h"

static int failures;
static int checks;

#define CHECK(condition, ...)                     \
  do {                                            \
    checks++;                                     \
    if (!(condition)) {                           \
      failures++;                                 \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
    }                                             \
  } while (0)

#define HW(offset) (0xcd000000u + (offset))

/* ------------------------------------------------------------------ */
/* The machine and the mailbox                                         */

static GcMachine *m;
static uint32_t arena;

typedef struct Packet {
  uint64_t cycle;
  unsigned size;
  int taken;
  uint8_t data[352];
} Packet;

#define MAX_EVENTS 1024
#define MAX_ACL 8192
static Packet events[MAX_EVENTS];
static Packet acl[MAX_ACL];
static unsigned event_count, event_read, acl_count;
static unsigned completed_packets;

/* A request handed to IOS and not yet answered. */
typedef struct Request {
  uint32_t block, buffer;
  int kind; /* 0 answered at once, 1 waiting for an event, 2 waiting for ACL data */
  int done;
  int32_t result;
} Request;
#define MAX_REQUESTS 16
static Request requests[MAX_REQUESTS];
static int acked;
static int fd = -1;
static int listening;
static Request *waiting_event, *waiting_acl;

/* Everything that crosses the endpoints, and when: two runs that are the
 * same run have the same sum. */
static uint64_t traffic_hash;
static unsigned traffic_count;
static uint64_t patience;

static void note(unsigned kind, const uint8_t *data, unsigned size) {
  uint64_t h = traffic_hash ^ kind;
  unsigned i;
  for (i = 0; i < 8u; i++) h = (h ^ ((m->cpu.cycles >> (8u * i)) & 0xffu)) * 0x100000001b3ull;
  for (i = 0; i < size; i++) h = (h ^ data[i]) * 0x100000001b3ull;
  traffic_hash = h;
  traffic_count++;
}

static uint8_t *ram(uint32_t address, uint32_t size) { return gc_machine_ram(m, address, size); }

static uint32_t alloc(uint32_t size) {
  const uint32_t at = arena;
  arena += (size + 31u) & ~31u;
  memset(ram(at, size ? size : 1u), 0, size);
  return at;
}

static uint32_t ctrl(void) { return gc_bus_read32(&m->cpu, HW(0x004)); }

/* Take whatever the mailbox is holding up. */
static void service(void) {
  for (;;) {
    const uint32_t c = ctrl();
    if (c & 0x02u) {
      acked = 1;
      gc_bus_write32(&m->cpu, HW(0x004), 0x02u);
      continue;
    }
    if (c & 0x04u) {
      const uint32_t block = gc_bus_read32(&m->cpu, HW(0x008));
      unsigned i;
      for (i = 0; i < MAX_REQUESTS; i++) {
        Request *r = &requests[i];
        if (r->block != block || r->done) continue;
        r->done = 1;
        r->result = (int32_t)wii_be32(ram(block + 4u, 4));
        if (r->kind == 1 && r->result > 0 && event_count < MAX_EVENTS) {
          Packet *p = &events[event_count++];
          p->cycle = m->cpu.cycles;
          p->size = (unsigned)r->result;
          p->taken = 0;
          memcpy(p->data, ram(r->buffer, p->size), p->size);
          note(1, p->data, p->size);
        }
        if (r->kind == 2 && r->result > 0 && acl_count < MAX_ACL) {
          Packet *p = &acl[acl_count++];
          p->cycle = m->cpu.cycles;
          p->size = (unsigned)r->result;
          p->taken = 0;
          memcpy(p->data, ram(r->buffer, p->size), p->size);
          note(2, p->data, p->size);
        }
        if (r == waiting_event) waiting_event = NULL;
        if (r == waiting_acl) waiting_acl = NULL;
        break;
      }
      gc_bus_write32(&m->cpu, HW(0x004), 0x04u);
      gc_bus_write32(&m->cpu, HW(0x004), 0x08u);
      continue;
    }
    break;
  }
}

/* Move time to the next thing that can happen: something IOS owes, or the
 * end of the field - at which the machine itself calls every device's
 * once-a-field function, as it does when a game is running. */
static void step(void) {
  const uint64_t now = m->cpu.cycles;
  uint64_t next = wii_machine_next_event(m, m->next_field_cycles);
  if (next < now) next = now;
  if (next > m->next_field_cycles) next = m->next_field_cycles;
  m->cpu.cycles = next;
  gc_machine_sync(m);
  wii_machine_advance(m);
  service();
}

static Request *new_request(void) {
  unsigned i;
  for (i = 0; i < MAX_REQUESTS; i++) {
    if (!requests[i].block || requests[i].done) {
      memset(&requests[i], 0, sizeof(requests[i]));
      return &requests[i];
    }
  }
  printf("FAIL: the test has too many requests outstanding\n");
  failures++;
  return &requests[0];
}

/* Hand a request block over and wait for IOS to take it. */
static void hand_over(Request *r) {
  unsigned guard = 0;
  gc_bus_write32(&m->cpu, HW(0x000), r->block);
  acked = 0;
  gc_bus_write32(&m->cpu, HW(0x004), 0x01u);
  while (!acked && guard++ < 100000u) step();
}

static Request *request(uint32_t type, uint32_t descriptor, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                        int kind, uint32_t buffer) {
  Request *r = new_request();
  uint8_t *block;
  r->block = alloc(0x40);
  r->buffer = buffer;
  r->kind = kind;
  block = ram(r->block, 0x40);
  wii_put32(block, type);
  wii_put32(block + 8, descriptor);
  wii_put32(block + 0x0c, a0);
  wii_put32(block + 0x10, a1);
  wii_put32(block + 0x14, a2);
  wii_put32(block + 0x18, a3);
  hand_over(r);
  return r;
}

static int32_t finish(Request *r) {
  unsigned guard = 0;
  while (!r->done && guard++ < 1000000u) step();
  return r->done ? r->result : (int32_t)0x7fffffff;
}

static Request *ioctlv(uint32_t command, unsigned in_count, unsigned io_count, const uint32_t *address,
                       const uint32_t *size, int kind) {
  const unsigned total = in_count + io_count;
  const uint32_t table = alloc(total * 8u);
  unsigned i;
  for (i = 0; i < total; i++) {
    wii_put32(ram(table + i * 8u, 8), address[i]);
    wii_put32(ram(table + i * 8u + 4u, 4), size[i]);
  }
  return request(WII_IOS_IOCTLV, (uint32_t)fd, command, in_count, io_count, table, kind, address[total - 1u]);
}

static uint32_t put_bytes(const uint8_t *bytes, unsigned size) {
  const uint32_t at = alloc(size ? size : 1u);
  if (size) memcpy(ram(at, size), bytes, size);
  return at;
}

/* A request left on an inward endpoint. */
static Request *wait_on(unsigned endpoint) {
  uint32_t address[3], size[3];
  uint8_t e = (uint8_t)endpoint, length[2];
  wii_put16(length, 1024);
  address[0] = put_bytes(&e, 1);
  size[0] = 1;
  address[1] = put_bytes(length, 2);
  size[1] = 2;
  address[2] = alloc(1024);
  size[2] = 1024;
  return ioctlv(endpoint == 0x81u ? 2u : 1u, 2, 1, address, size, endpoint == 0x81u ? 1 : 2);
}

/* What a stack does whenever it has a moment: keep a request waiting on
 * each inward endpoint. */
static void host_poll(void) {
  if (!listening) return;
  if (!waiting_event) waiting_event = wait_on(0x81);
  if (!waiting_acl) waiting_acl = wait_on(0x82);
}

static void run(uint64_t cycles) {
  const uint64_t until = m->cpu.cycles + cycles;
  while (m->cpu.cycles < until) {
    host_poll();
    step();
  }
}

/* Run to the end of this field and of `fields - 1` more. */
static void run_fields(unsigned fields) {
  const uint64_t until = m->field + fields;
  while (m->field < until) {
    host_poll();
    step();
  }
}

static void fresh(void) {
  if (m) gc_machine_destroy(m);
  m = gc_machine_create();
  m->log = NULL;
  gc_install_boot_state(m);
  arena = 0x13300000u;
  memset(requests, 0, sizeof(requests));
  event_count = event_read = acl_count = 0;
  completed_packets = 0;
  waiting_event = waiting_acl = NULL;
  listening = 0;
  fd = -1;
  traffic_hash = 0xcbf29ce484222325ull;
  traffic_count = 0;
  patience = (uint64_t)GC_CPU_HZ / 4u;
}

static int32_t open_device(void) {
  const uint32_t path = alloc(64);
  strcpy((char *)ram(path, 64), "/dev/usb/oh1/57e/305");
  return finish(request(WII_IOS_OPEN, 0, path, 0, 0, 0, 0, 0));
}

static void power_up(void) {
  fresh();
  fd = open_device();
  listening = 1;
  host_poll();
}

/* ------------------------------------------------------------------ */
/* HCI                                                                 */

static int32_t hci(unsigned opcode, const uint8_t *params, unsigned length) {
  uint8_t packet[260], type = 0x20, zero = 0, word[2];
  uint32_t address[7], size[7];
  wii_put_le16(packet, (uint16_t)opcode);
  packet[2] = (uint8_t)length;
  if (length) memcpy(packet + 3, params, length);
  address[0] = put_bytes(&type, 1);
  address[1] = put_bytes(&zero, 1);
  wii_put_le16(word, 0);
  address[2] = put_bytes(word, 2);
  address[3] = put_bytes(word, 2);
  wii_put_le16(word, (uint16_t)(length + 3u));
  address[4] = put_bytes(word, 2);
  address[5] = put_bytes(&zero, 1);
  address[6] = put_bytes(packet, length + 3u);
  size[0] = size[1] = size[5] = 1;
  size[2] = size[3] = size[4] = 2;
  size[6] = length + 3u;
  note(3, packet, length + 3u);
  return finish(ioctlv(0, 6, 1, address, size, 0));
}

/* The next event that is not the module counting packets back (those are
 * added up on the side), or NULL when none comes. */
static Packet *event_next(void) {
  const uint64_t deadline = m->cpu.cycles + patience;
  for (;;) {
    while (event_read < event_count) {
      Packet *p = &events[event_read++];
      if (p->data[0] == 0x13u) {
        unsigned i;
        for (i = 0; i < p->data[2]; i++) completed_packets += wii_le16(p->data + 5u + i * 4u);
        continue;
      }
      return p;
    }
    if (m->cpu.cycles >= deadline) return NULL;
    host_poll();
    step();
  }
}

static unsigned hex(const char *text, uint8_t *out) {
  unsigned n = 0, value;
  int used;
  while (sscanf(text, " %2x%n", &value, &used) == 1) {
    out[n++] = (uint8_t)value;
    text += used;
  }
  return n;
}

static void show(const uint8_t *data, unsigned size) {
  unsigned i;
  for (i = 0; i < size && i < 64u; i++) printf(" %02x", data[i]);
}

static int same(const Packet *p, const char *text, const char *what, int line) {
  uint8_t want[352];
  const unsigned n = hex(text, want);
  checks++;
  if (p && p->size == n && !memcmp(p->data, want, n)) return 1;
  failures++;
  printf("FAIL %s:%d: %s: wanted %s, got", __FILE__, line, what, text);
  if (p) {
    show(p->data, p->size);
  } else {
    printf(" nothing");
  }
  printf("\n");
  return 0;
}
#define EVENT(text, what) same(event_next(), text, what, __LINE__)

/* A command whose whole answer is one event. */
#define COMMAND(opcode, params, answer, what)        \
  do {                                               \
    uint8_t bytes_[256];                             \
    const unsigned n_ = hex(params, bytes_);         \
    hci(opcode, bytes_, n_);                         \
    same(event_next(), answer, what, __LINE__);      \
  } while (0)

/* ------------------------------------------------------------------ */
/* ACL and L2CAP                                                       */

static void acl_send(unsigned handle, unsigned cid, const uint8_t *payload, unsigned size) {
  uint8_t packet[352], endpoint = 0x02, length[2];
  uint32_t address[3], sizes[3];
  wii_put_le16(packet, (uint16_t)(handle | 0x2000u));
  wii_put_le16(packet + 2, (uint16_t)(size + 4u));
  wii_put_le16(packet + 4, (uint16_t)size);
  wii_put_le16(packet + 6, (uint16_t)cid);
  memcpy(packet + 8, payload, size);
  wii_put16(length, (uint16_t)(size + 8u));
  address[0] = put_bytes(&endpoint, 1);
  sizes[0] = 1;
  address[1] = put_bytes(length, 2);
  sizes[1] = 2;
  address[2] = put_bytes(packet, size + 8u);
  sizes[2] = size + 8u;
  note(4, packet, size + 8u);
  finish(ioctlv(1, 2, 1, address, sizes, 0));
}

/* The next packet from one remote on one channel (zero: any), or NULL. */
static Packet *acl_next(unsigned handle, unsigned cid) {
  const uint64_t deadline = m->cpu.cycles + patience;
  unsigned i = 0;
  for (;;) {
    for (; i < acl_count; i++) {
      Packet *p = &acl[i];
      if (p->taken) continue;
      if ((wii_le16(p->data) & 0x0fffu) != handle) continue;
      if (cid && wii_le16(p->data + 6) != cid) continue;
      p->taken = 1;
      return p;
    }
    if (m->cpu.cycles >= deadline) return NULL;
    host_poll();
    step();
  }
}

/* Forget what has arrived, so that the next packet looked at is new. */
static void acl_flush(void) {
  unsigned i;
  for (i = 0; i < acl_count; i++) acl[i].taken = 1;
}

static void signal_send(unsigned handle, unsigned code, unsigned id, const char *text) {
  uint8_t packet[64];
  const unsigned n = hex(text, packet + 4);
  packet[0] = (uint8_t)code;
  packet[1] = (uint8_t)id;
  wii_put_le16(packet + 2, (uint16_t)n);
  acl_send(handle, 1, packet, n + 4u);
}

/* The console's ends of the channels it has with a remote. */
#define HOST_CONTROL 0x0050u
#define HOST_INTERRUPT 0x0051u
#define HOST_SDP 0x0052u
/* And the remote's, which are fixed. */
#define REMOTE_CONTROL 0x0040u
#define REMOTE_INTERRUPT 0x0041u

static void remote_address(unsigned index, uint8_t out[6]) {
  uint8_t a[6];
  unsigned i;
  wii_remote_address(index, a);
  for (i = 0; i < 6u; i++) out[i] = a[5u - i];
}

/* The start of a session: reset, and listen for remotes. */
static void start_listening(void) {
  hci(0x0c03, NULL, 0);
  EVENT("0e 04 01 03 0c 00", "Reset");
  COMMAND(0x0c1a, "02", "0e 04 01 1a 0c 00", "Write Scan Enable");
}

/* A remote asks to connect. */
static void expect_request(unsigned index) {
  uint8_t a[6];
  char text[128];
  remote_address(index, a);
  snprintf(text, sizeof(text), "04 0a %02x %02x %02x %02x %02x %02x 04 25 00 01", a[0], a[1], a[2], a[3], a[4], a[5]);
  EVENT(text, "Connection Request, from the remote's address with a remote's class");
}

/* Accept it, as master. Returns its handle. */
static unsigned accept_request(unsigned index) {
  uint8_t a[6], params[7];
  char text[128];
  remote_address(index, a);
  memcpy(params, a, 6);
  params[6] = 0x00;
  hci(0x0409, params, 7);
  EVENT("0f 04 00 01 09 04", "Accept Connection Request: Command Status");
  snprintf(text, sizeof(text), "12 08 00 %02x %02x %02x %02x %02x %02x 00", a[0], a[1], a[2], a[3], a[4], a[5]);
  EVENT(text, "Role Change: the console is now the master");
  snprintf(text, sizeof(text), "03 0b 00 %02x 01 %02x %02x %02x %02x %02x %02x 01 00", index, a[0], a[1], a[2], a[3],
           a[4], a[5]);
  EVENT(text, "Connection Complete");
  return 0x0100u + index;
}

static unsigned accept_remote(unsigned index) {
  expect_request(index);
  return accept_request(index);
}

/* One channel the remote opens: its request, the console's answer, and
 * the two exchanges of options. */
static void remote_opens(unsigned handle, unsigned psm, unsigned remote_cid, unsigned host_cid) {
  Packet *p = acl_next(handle, 1);
  char text[64];
  unsigned id;
  CHECK(p && p->size == 16u && p->data[8] == 0x02u && wii_le16(p->data + 10) == 4u &&
            wii_le16(p->data + 12) == psm && wii_le16(p->data + 14) == remote_cid,
        "the remote asks for PSM %04x from its channel %04x", psm, remote_cid);
  if (!p) return;
  CHECK((wii_le16(p->data) >> 12) == 2u, "as the first packet of a message");
  id = p->data[9];
  snprintf(text, sizeof(text), "%02x %02x %02x %02x 00 00 00 00", host_cid & 0xffu, host_cid >> 8, remote_cid & 0xffu,
           remote_cid >> 8);
  signal_send(handle, 0x03, id, text);
  /* The remote's options: a packet size, to the console's channel. */
  p = acl_next(handle, 1);
  CHECK(p && p->data[8] == 0x04u && wii_le16(p->data + 12) == host_cid && wii_le16(p->data + 14) == 0u &&
            p->data[16] == 0x01u && p->data[17] == 2u && wii_le16(p->data + 18) >= 48u,
        "the remote sends its options for PSM %04x, with a packet size HID allows", psm);
  if (!p) return;
  snprintf(text, sizeof(text), "%02x %02x 00 00 00 00", remote_cid & 0xffu, remote_cid >> 8);
  signal_send(handle, 0x05, p->data[9], text);
  /* The console's: packet size 0x02a1, as lwbt sends. */
  snprintf(text, sizeof(text), "%02x %02x 00 00 01 02 a1 02", remote_cid & 0xffu, remote_cid >> 8);
  signal_send(handle, 0x04, 0x71, text);
  p = acl_next(handle, 1);
  CHECK(p && p->size == 18u && p->data[8] == 0x05u && p->data[9] == 0x71u && wii_le16(p->data + 12) == host_cid &&
            wii_le16(p->data + 16) == 0u,
        "and accepts the console's for PSM %04x", psm);
}

/* The whole of a remote's arrival: link, control channel, interrupt
 * channel. */
static unsigned connect_remote(unsigned index) {
  const unsigned handle = accept_remote(index);
  remote_opens(handle, 0x11, REMOTE_CONTROL, HOST_CONTROL);
  remote_opens(handle, 0x13, REMOTE_INTERRUPT, HOST_INTERRUPT);
  return handle;
}

/* An output report, as a retail game sends one: data on the interrupt
 * channel. */
static void output(unsigned handle, const char *text) {
  uint8_t packet[32];
  const unsigned n = hex(text, packet + 1);
  packet[0] = 0xa2;
  acl_send(handle, REMOTE_INTERRUPT, packet, n + 1u);
}

/* The next input report with this number: its bytes after the 0xa1, or
 * NULL. Others on the way are dropped. */
static const uint8_t *input_report(unsigned handle, unsigned id, unsigned *size, uint64_t *cycle) {
  for (;;) {
    Packet *p = acl_next(handle, HOST_INTERRUPT);
    if (!p) return NULL;
    if (p->size < 10u || p->data[8] != 0xa1u || p->data[9] != id) continue;
    if (size) *size = p->size - 9u;
    if (cycle) *cycle = p->cycle;
    return p->data + 9;
  }
}

static void expect_report(unsigned handle, const char *text, const char *what, int line) {
  uint8_t want[32];
  const unsigned n = hex(text, want);
  unsigned size = 0;
  const uint8_t *got = input_report(handle, want[0], &size, NULL);
  checks++;
  if (got && size == n && !memcmp(got, want, n)) return;
  failures++;
  printf("FAIL %s:%d: %s: wanted %s, got", __FILE__, line, what, text);
  if (got) {
    show(got, size);
  } else {
    printf(" nothing");
  }
  printf("\n");
}
#define REPORT(handle, text, what) expect_report(handle, text, what, __LINE__)

/* Read memory or registers: sixteen bytes or fewer, in one answer.
 * Returns the error nibble, and the data in `out`. */
static unsigned read_memory(unsigned handle, unsigned space, uint32_t address, unsigned count, uint8_t *out) {
  char text[64];
  unsigned size = 0;
  const uint8_t *r;
  snprintf(text, sizeof(text), "17 %02x %02x %02x %02x %02x %02x", space, (address >> 16) & 0xffu, (address >> 8) & 0xffu,
           address & 0xffu, count >> 8, count & 0xffu);
  output(handle, text);
  r = input_report(handle, 0x21, &size, NULL);
  if (!r || size != 22u) return 0xffu;
  if (!(r[3] & 0x0fu)) {
    CHECK((unsigned)(r[3] >> 4) + 1u == (count < 16u ? count : 16u), "a read of %u bytes says how many it holds", count);
    CHECK(wii_be16(r + 4) == (address & 0xffffu), "and where from (%04x)", wii_be16(r + 4));
  }
  memcpy(out, r + 6, 16);
  return r[3] & 0x0fu;
}

static void write_memory(unsigned handle, unsigned space, uint32_t address, const char *data, unsigned want_error,
                         const char *what) {
  uint8_t bytes[16];
  char text[128];
  const unsigned n = hex(data, bytes);
  unsigned i, at;
  at = (unsigned)snprintf(text, sizeof(text), "16 %02x %02x %02x %02x %02x", space, (address >> 16) & 0xffu,
                          (address >> 8) & 0xffu, address & 0xffu, n);
  for (i = 0; i < 16u; i++) at += (unsigned)snprintf(text + at, sizeof(text) - at, " %02x", i < n ? bytes[i] : 0u);
  output(handle, text);
  {
    /* Always acknowledged, whatever buttons are held at the time. */
    unsigned size = 0;
    const uint8_t *r = input_report(handle, 0x22, &size, NULL);
    CHECK(r && size == 5u && r[3] == 0x16u && r[4] == want_error, "%s (acknowledgement %02x %02x, wanted 16 %02x)", what,
          r ? r[3] : 0xffu, r ? r[4] : 0xffu, want_error);
  }
}

static WiiRemoteInput at_rest(void) {
  WiiRemoteInput input;
  wii_remote_rest(&input, 1);
  return input;
}

/* ------------------------------------------------------------------ */
/* The device                                                          */

static void test_device(void) {
  Request *a, *b;
  uint32_t address[3], size[3];
  uint8_t byte = 0, word[2] = {0, 64};
  int32_t result;

  fresh();
  fd = open_device();
  CHECK(fd >= 0, "the Bluetooth module opens (%d)", fd);

  /* Requests it does not have are refused and counted. */
  address[0] = put_bytes(&byte, 1);
  size[0] = 1;
  address[1] = alloc(32);
  size[1] = 32;
  result = finish(ioctlv(9, 1, 1, address, size, 0));
  CHECK(result == WII_IPC_EINVAL && wii_state(m)->ios.unknown_commands == 1u, "an ioctlv it does not have (%d, %u)",
        (int)result, wii_state(m)->ios.unknown_commands);
  result = finish(ioctlv(0, 1, 1, address, size, 0));
  CHECK(result == WII_IPC_EINVAL && wii_state(m)->ios.unknown_commands == 2u, "a control request of the wrong shape");
  byte = 0x03;
  address[0] = put_bytes(&byte, 1);
  address[1] = put_bytes(word, 2);
  size[1] = 2;
  address[2] = alloc(64);
  size[2] = 64;
  result = finish(ioctlv(1, 2, 1, address, size, 0));
  CHECK(result == WII_IPC_EINVAL && wii_state(m)->ios.unknown_commands == 3u, "an endpoint it does not have");

  /* Requests on the inward endpoints wait, and a close answers them. */
  a = wait_on(0x81);
  b = wait_on(0x82);
  run(GC_CYCLES_PER_FIELD * 3u);
  CHECK(!a->done && !b->done, "with nothing to say, both inward requests wait");
  result = finish(request(WII_IOS_CLOSE, (uint32_t)fd, 0, 0, 0, 0, 0, 0));
  CHECK(result == 0, "the module closes (%d)", (int)result);
  CHECK(a->done && a->result < 0 && b->done && b->result < 0, "and what was waiting is answered with an error (%d, %d)",
        (int)a->result, (int)b->result);
  fd = open_device();
  CHECK(fd >= 0, "and it opens again");

  /* An event arrives a known time after its command. */
  listening = 1;
  host_poll();
  {
    const uint64_t sent = m->cpu.cycles;
    Packet *p;
    hci(0x0c03, NULL, 0);
    p = event_next();
    same(p, "0e 04 01 03 0c 00", "Reset is answered", __LINE__);
    CHECK(p && p->cycle >= sent + WII_BT_EVENT_CYCLES && p->cycle < sent + WII_BT_EVENT_CYCLES + WII_IOS_ACK_CYCLES * 2u,
          "about a millisecond after it was sent (%llu cycles)", p ? (unsigned long long)(p->cycle - sent) : 0ull);
  }
}

/* ------------------------------------------------------------------ */
/* Start-up, as each stack does it                                     */

/* The commands libogc's lwbt sends between BTE_InitCore and the moment it
 * is listening, in its order (bte.c: the initcore, patch and initsub
 * chains), and the answer the specification gives for each. */
static void test_startup_lwbt(void) {
  uint8_t name[248];
  Packet *p;
  unsigned i;

  power_up();
  hci(0x0c03, NULL, 0);
  EVENT("0e 04 01 03 0c 00", "Reset");
  /* 339-byte ACL packets, ten of them; 64-byte SCO, none. */
  COMMAND(0x1005, "", "0e 0b 01 05 10 00 53 01 40 0a 00 00 00", "Read Buffer Size");
  COMMAND(0x0c24, "00 1f 00", "0e 04 01 24 0c 00", "Write Class Of Device");
  memset(name, 0, sizeof(name));
  hci(0x0c13, name, sizeof(name));
  EVENT("0e 04 01 13 0c 00", "Write Local Name");
  COMMAND(0x0c0a, "00", "0e 04 01 0a 0c 00", "Write PIN Type");
  COMMAND(0x0c33, "9b 06 ff 14 01 00 00", "0e 04 01 33 0c 00", "Host Buffer Size (lwbt's bytes, as it sends them)");
  /* HCI 2.0, revision 0x40a7, LMP 2.0, Broadcom, subversion 0x430e. */
  COMMAND(0x1001, "", "0e 0c 01 01 10 00 03 a7 40 03 0f 00 0e 43", "Read Local Version Information");
  COMMAND(0x1009, "", "0e 0a 01 09 10 00 01 57 43 4e 1e 00", "Read BD_ADDR: the console's, low byte first");
  COMMAND(0x1003, "", "0e 0c 01 03 10 00 ff ff 8d fe 9b f9 00 80", "Read Local Supported Features");
  COMMAND(0x0c45, "01", "0e 04 01 45 0c 00", "Write Inquiry Mode");
  COMMAND(0x0c47, "01", "0e 04 01 47 0c 00", "Write Page Scan Type");
  COMMAND(0x0c43, "01", "0e 04 01 43 0c 00", "Write Inquiry Scan Type");
  COMMAND(0x0c24, "04 02 40", "0e 04 01 24 0c 00", "Write Class Of Device");
  COMMAND(0x0c18, "00 20", "0e 04 01 18 0c 00", "Write Page Timeout");

  /* The link keys of the four registered remotes, then the count. */
  {
    uint8_t all[7] = {0, 0, 0, 0, 0, 0, 1};
    uint8_t a[6];
    hci(0x0c0d, all, 7);
    p = event_next();
    CHECK(p && p->size == 2u + 1u + 4u * 22u && p->data[0] == 0x15u && p->data[1] == 89u && p->data[2] == 4u,
          "Return Link Keys carries four keys");
    for (i = 0; p && i < 4u; i++) {
      remote_address(i, a);
      CHECK(!memcmp(p->data + 3u + i * 22u, a, 6), "key %u is remote %u's", i, i);
    }
    if (p) CHECK(memcmp(p->data + 9, p->data + 31, 16) != 0, "and each remote's key is its own");
    EVENT("0e 08 01 0d 0c 00 10 00 04 00", "Read Stored Link Key: room for sixteen, four read");
  }

  /* The firmware patch homebrew copies from a retail game. */
  COMMAND(0xfc4f, "00", "0e 04 01 4f fc 00", "Broadcom 0xfc4f (remove patch)");
  {
    uint8_t patch[184];
    memset(patch, 0x5a, sizeof(patch));
    hci(0xfc4c, patch, sizeof(patch));
    EVENT("0e 04 01 4c fc 00", "Broadcom 0xfc4c (write patch)");
    hci(0xfc4f, patch, 92);
    EVENT("0e 04 01 4f fc 00", "Broadcom 0xfc4f (install patch)");
  }
  hci(0x0c03, NULL, 0);
  EVENT("0e 04 01 03 0c 00", "Reset, again");
  COMMAND(0x0c19, "", "0e 05 01 19 0c 00 00", "which turned scanning off");
  COMMAND(0x0c24, "00 04 48", "0e 04 01 24 0c 00", "Write Class Of Device");
  COMMAND(0x0c18, "00 80", "0e 04 01 18 0c 00", "Write Page Timeout");
  memcpy(name, "Wii", 4);
  hci(0x0c13, name, sizeof(name));
  EVENT("0e 04 01 13 0c 00", "Write Local Name");
  hci(0x0c14, NULL, 0);
  p = event_next();
  CHECK(p && p->size == 6u + 248u && !memcmp(p->data, "\x0e\xfc\x01\x14\x0c\x00Wii", 10), "Read Local Name returns it");
  COMMAND(0x0c05, "01 00", "0e 04 01 05 0c 00", "Set Event Filter");

  /* Nothing asks to connect until the console listens for pages. */
  CHECK(event_next() == NULL, "no remote connects while the console is not listening");
  COMMAND(0x0c1a, "02", "0e 04 01 1a 0c 00", "Write Scan Enable");
  CHECK(wii_state(m)->bluetooth.unknown_commands == 0u, "none of lwbt's commands was unknown");
}

/* The commands Broadcom's stack sends, as doldecomp/ogws carries it
 * (BTE/stack/btm/btm_devctl.c's reset chain, btm_acl.c after a
 * connection, btm_pm.c, btm_sec.c, hcic/hcicmds.c for the layouts) and as
 * Nintendo's layer on top drives it (the firmware patch, the device name
 * and class, page timeout, default link policy, stored link keys). Nobody
 * here has a retail game to compare with: this is that code read, and
 * each command answered as the specification says. */
static void test_startup_bte(void) {
  Packet *p;
  unsigned handle;
  uint8_t patch[255];
  unsigned i;

  power_up();
  hci(0x0c03, NULL, 0);
  EVENT("0e 04 01 03 0c 00", "Reset");
  COMMAND(0x1005, "", "0e 0b 01 05 10 00 53 01 40 0a 00 00 00", "Read Buffer Size");
  COMMAND(0x0c33, "9b 06 40 14 00 0a 00", "0e 04 01 33 0c 00", "Host Buffer Size");
  COMMAND(0x1001, "", "0e 0c 01 01 10 00 03 a7 40 03 0f 00 0e 43", "Read Local Version Information");
  /* The revision's low twelve bits are 167: the firmware a retail game
   * patches. */
  COMMAND(0x1009, "", "0e 0a 01 09 10 00 01 57 43 4e 1e 00", "Read BD_ADDR");
  COMMAND(0x1003, "", "0e 0c 01 03 10 00 ff ff 8d fe 9b f9 00 80", "Read Local Supported Features");
  COMMAND(0x0c24, "00 1f 00", "0e 04 01 24 0c 00", "Write Class Of Device");
  COMMAND(0x0c0a, "00", "0e 04 01 0a 0c 00", "Write PIN Type");
  COMMAND(0x0c05, "00", "0e 04 01 05 0c 00", "Set Event Filter: clear all");
  COMMAND(0x0c16, "a0 1f", "0e 04 01 16 0c 00", "Write Connection Accept Timeout");
  COMMAND(0x0c15, "", "0e 06 01 15 0c 00 a0 1f", "Read Connection Accept Timeout");
  COMMAND(0x0c01, "ff ff ff ff ff ff ff 00", "0e 04 01 01 0c 00", "Set Event Mask");
  {
    uint8_t all[7] = {0, 0, 0, 0, 0, 0, 1};
    hci(0x0c0d, all, 7);
    p = event_next();
    CHECK(p && p->data[0] == 0x15u && p->data[2] == 4u, "Return Link Keys: all four registered remotes");
    EVENT("0e 08 01 0d 0c 00 10 00 04 00", "Read Stored Link Key");
  }
  /* The patch: remove, write in pieces of up to 255 bytes, install. */
  COMMAND(0xfc4f, "00", "0e 04 01 4f fc 00", "0xfc4f: remove the patch");
  memset(patch, 0xa5, sizeof(patch));
  for (i = 0; i < 2u; i++) {
    hci(0xfc4c, patch, i ? 100u : 255u);
    EVENT("0e 04 01 4c fc 00", "0xfc4c: write a piece of the patch");
  }
  hci(0xfc4f, patch, 53);
  EVENT("0e 04 01 4f fc 00", "0xfc4f: install the patch");
  hci(0x0c03, NULL, 0);
  EVENT("0e 04 01 03 0c 00", "Reset, after the patch");
  COMMAND(0x1005, "", "0e 0b 01 05 10 00 53 01 40 0a 00 00 00", "Read Buffer Size");
  COMMAND(0x0c33, "9b 06 40 14 00 0a 00", "0e 04 01 33 0c 00", "Host Buffer Size");

  /* Nintendo's layer: name, class, page timeout, policy, then listen. */
  {
    uint8_t name[248];
    memset(name, 0, sizeof(name));
    memcpy(name, "Wii", 4);
    hci(0x0c13, name, sizeof(name));
    EVENT("0e 04 01 13 0c 00", "Write Local Name");
  }
  COMMAND(0x0c24, "00 04 48", "0e 04 01 24 0c 00", "Write Class Of Device");
  COMMAND(0x0c18, "00 80", "0e 04 01 18 0c 00", "Write Page Timeout");
  COMMAND(0x080f, "05 00", "0e 04 01 0f 08 00", "Write Default Link Policy Settings");
  COMMAND(0x080e, "", "0e 06 01 0e 08 00 05 00", "Read Default Link Policy Settings");
  COMMAND(0x0c1c, "00 08 12 00", "0e 04 01 1c 0c 00", "Write Page Scan Activity");
  COMMAND(0x0c47, "01", "0e 04 01 47 0c 00", "Write Page Scan Type");
  COMMAND(0x0c3a, "01 00 8b 9e", "0e 04 01 3a 0c 00", "Write Current IAC LAP");
  COMMAND(0x0c39, "", "0e 08 01 39 0c 00 01 00 8b 9e", "Read Current IAC LAP");
  COMMAND(0x0c1e, "00 08 12 00", "0e 04 01 1e 0c 00", "Write Inquiry Scan Activity");
  COMMAND(0x0c43, "01", "0e 04 01 43 0c 00", "Write Inquiry Scan Type");
  COMMAND(0x0c45, "01", "0e 04 01 45 0c 00", "Write Inquiry Mode");
  COMMAND(0x0c3f, "ff ff ff ff ff ff ff ff ff 7f", "0e 04 01 3f 0c 00", "Set AFH Host Channel Classification");
  COMMAND(0x0c12, "aa bb cc dd ee ff 00", "0e 06 01 12 0c 00 00 00", "Delete Stored Link Key: one nobody has");
  COMMAND(0x0c11, "01 aa bb cc dd ee ff 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f",
          "0e 05 01 11 0c 00 01", "Write Stored Link Key");
  COMMAND(0x0c0d, "aa bb cc dd ee ff 00", "15 17 01 aa bb cc dd ee ff 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f",
          "Read Stored Link Key: that one");
  EVENT("0e 08 01 0d 0c 00 10 00 01 00", "and its count");
  COMMAND(0x0c12, "aa bb cc dd ee ff 00", "0e 06 01 12 0c 00 01 00", "Delete Stored Link Key");
  COMMAND(0x0c1a, "02", "0e 04 01 1a 0c 00", "Write Scan Enable: connectable, not discoverable");

  /* A remote arrives. The stack accepts as master, then asks the link
   * about itself. */
  {
    WiiRemoteInput input = at_rest();
    wii_remote_set(m, 0, &input);
  }
  handle = accept_remote(0);
  COMMAND(0x041f, "00 01", "0f 04 00 01 1f 04", "Read Clock Offset");
  EVENT("1c 05 00 00 01 00 00", "Read Clock Offset Complete");
  COMMAND(0x041d, "00 01", "0f 04 00 01 1d 04", "Read Remote Version Information");
  /* The remote: LMP 1.2, Broadcom, subversion 0x0229. */
  EVENT("0c 08 00 00 01 02 0f 00 29 02", "Read Remote Version Information Complete");
  COMMAND(0x041b, "00 01", "0f 04 00 01 1b 04", "Read Remote Supported Features");
  EVENT("0b 0b 00 00 01 bc 02 04 38 08 00 00 00", "Read Remote Supported Features Complete");
  COMMAND(0x080d, "00 01 05 00", "0e 06 01 0d 08 00 00 01", "Write Link Policy Settings");
  COMMAND(0x080c, "00 01", "0e 08 01 0c 08 00 00 01 05 00", "Read Link Policy Settings");
  COMMAND(0x0c37, "00 01 80 0c", "0e 06 01 37 0c 00 00 01", "Write Link Supervision Timeout");
  COMMAND(0x0c36, "00 01", "0e 08 01 36 0c 00 00 01 80 0c", "Read Link Supervision Timeout");
  COMMAND(0x040f, "00 01 18 cc", "0f 04 00 01 0f 04", "Change Connection Packet Type");
  EVENT("1d 05 00 00 01 18 cc", "Connection Packet Type Changed");
  COMMAND(0x0809, "00 01", "0e 07 01 09 08 00 00 01 00", "Role Discovery: master");

  /* L2CAP. The stack asks what the remote can do before anything else,
   * and holds the remote's first channel until it has authenticated. */
  signal_send(handle, 0x0a, 0x01, "02 00");
  {
    /* By now the remote has asked for its control channel; its answer to
     * the stack's question comes after that. */
    Packet *asked = acl_next(handle, 1);
    p = acl_next(handle, 1);
    CHECK(p && p->size == 16u && p->data[8] == 0x0bu && p->data[9] == 0x01u && wii_le16(p->data + 12) == 2u &&
              wii_le16(p->data + 14) == 1u,
          "Information Request (extended features): not supported, as Bluetooth 1.2 answers");
    p = asked;
  }
  CHECK(p && p->data[8] == 0x02u && wii_le16(p->data + 12) == 0x11u, "the remote asks for its control channel");
  if (p) {
    const unsigned id = p->data[9];
    char text[64];
    /* "Pending": wait. */
    snprintf(text, sizeof(text), "%02x %02x 40 00 01 00 01 00", HOST_CONTROL & 0xffu, HOST_CONTROL >> 8);
    signal_send(handle, 0x03, id, text);
    COMMAND(0x0411, "00 01", "0f 04 00 01 11 04", "Authentication Requested");
    EVENT("17 06 10 57 43 4e 1e 00", "Link Key Request, for the remote");
    {
      uint8_t reply[22];
      Packet *keys;
      uint8_t one[7];
      remote_address(0, one);
      one[6] = 0;
      hci(0x0c0d, one, 7);
      keys = event_next();
      CHECK(keys && keys->data[0] == 0x15u && keys->data[2] == 1u, "the module has that remote's key");
      EVENT("0e 08 01 0d 0c 00 10 00 01 00", "one key read");
      memcpy(reply, one, 6);
      if (keys) memcpy(reply + 6, keys->data + 9, 16);
      hci(0x040b, reply, 22);
      EVENT("0e 0a 01 0b 04 00 10 57 43 4e 1e 00", "Link Key Request Reply");
      EVENT("06 03 00 00 01", "Authentication Complete");
    }
    CHECK(acl_next(handle, 1) == NULL, "while its request is pending the remote sends nothing more");
    snprintf(text, sizeof(text), "%02x %02x 40 00 00 00 00 00", HOST_CONTROL & 0xffu, HOST_CONTROL >> 8);
    signal_send(handle, 0x03, id, text);
    /* The stack's options: packet size 640 and a flush timeout. */
    signal_send(handle, 0x04, 0x02, "40 00 00 00 01 02 80 02 02 02 ff ff");
    p = acl_next(handle, 1);
    CHECK(p && p->data[8] == 0x04u && wii_le16(p->data + 12) == HOST_CONTROL, "the remote's options for the control channel");
    if (p) signal_send(handle, 0x05, p->data[9], "40 00 00 00 00 00");
    p = acl_next(handle, 1);
    CHECK(p && p->data[8] == 0x05u && p->data[9] == 0x02u && wii_le16(p->data + 16) == 0u, "and its agreement to the stack's");
    remote_opens(handle, 0x13, REMOTE_INTERRUPT, HOST_INTERRUPT);
  }
  COMMAND(0x0413, "00 01 01", "0f 04 00 01 13 04", "Set Connection Encryption");
  EVENT("08 04 00 00 01 01", "Encryption Change");
  COMMAND(0x0807, "00 01 00 01 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff", "0f 04 00 01 07 08", "QoS Setup");
  EVENT("0d 15 00 00 01 00 01 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff", "QoS Setup Complete");
  COMMAND(0x0803, "00 01 10 00 10 00 01 00 00 00", "0f 04 00 01 03 08", "Sniff Mode");
  EVENT("14 06 00 00 01 02 10 00", "Mode Change: sniff");
  COMMAND(0x0804, "00 01", "0f 04 00 01 04 08", "Exit Sniff Mode");
  EVENT("14 06 00 00 01 00 00 00", "Mode Change: active");
  COMMAND(0x0801, "00 01 20 00 10 00", "0f 04 00 01 01 08", "Hold Mode");
  EVENT("14 06 00 00 01 01 20 00", "Mode Change: hold");
  COMMAND(0x0417, "00", "0f 04 00 01 17 04", "Master Link Key");
  EVENT("0a 04 00 00 01 00", "Master Link Key Complete");
  COMMAND(0x080b, "10 57 43 4e 1e 00 01", "0f 04 00 01 0b 08", "Switch Role");
  EVENT("12 08 00 10 57 43 4e 1e 00 01", "Role Change: slave");
  COMMAND(0x1405, "00 01", "0e 07 01 05 14 00 00 01 00", "Read RSSI");
  COMMAND(0x1403, "00 01", "0e 07 01 03 14 00 00 01 ff", "Get Link Quality");
  COMMAND(0xfc57, "00 01 01", "0e 04 01 57 fc 00", "Broadcom 0xfc57 (link priority)");
  /* The remote speaks. */
  output(handle, "11 12");
  REPORT(handle, "22 00 00 11 00", "an output report on the interrupt channel is acknowledged");
  CHECK(wii_state(m)->bluetooth.unknown_commands == 0u, "none of the stack's commands was unknown (%u)",
        wii_state(m)->bluetooth.unknown_commands);

  /* A command for a link that is not there, and one the module has not. */
  COMMAND(0x080d, "07 01 05 00", "0e 06 01 0d 08 02 07 01", "Write Link Policy Settings for no such connection");
  COMMAND(0x041d, "07 01", "0f 04 02 01 1d 04", "Read Remote Version Information for no such connection");
  COMMAND(0x0c52, "00", "0e 04 01 52 0c 01", "a command from a later Bluetooth is \"unknown HCI command\"");
  COMMAND(0xfc01, "00", "0e 04 01 01 fc 01", "and so is a Broadcom command nobody wrote");
  CHECK(wii_state(m)->bluetooth.unknown_commands == 2u, "and both are counted");

  /* What the module says it supports is what it answers. */
  hci(0x1002, NULL, 0);
  p = event_next();
  CHECK(p && p->size == 70u && (p->data[6 + 5] & 0x80u) && (p->data[6 + 15] & 0x02u) && (p->data[6 + 7] & 0xc0u) == 0xc0u,
        "Read Local Supported Commands lists Reset, Read BD_ADDR and scan enable");
  if (p) {
    /* Every command it lists in the first groups answers something other
     * than "unknown" (a short one may be refused as too short, which is
     * also "unknown" - so each is sent with room to spare). */
    static const struct {
      uint16_t opcode;
      uint8_t bit;
    } listed[] = {{0x0401, 0}, {0x0405, 4}, {0x0406, 5}, {0x0409, 8}, {0x0411, 15}, {0x0419, 19},
                  {0x0803, 34}, {0x0809, 39}, {0x0c01, 46}, {0x0c05, 48}, {0x0c0d, 53}, {0x0c33, 86}};
    uint8_t zeros[32];
    unsigned before = wii_state(m)->bluetooth.unknown_commands;
    memset(zeros, 0, sizeof(zeros));
    for (i = 0; i < sizeof(listed) / sizeof(listed[0]); i++) {
      CHECK(p->data[6u + (listed[i].bit >> 3)] & (1u << (listed[i].bit & 7u)), "command %04x is listed", listed[i].opcode);
    }
    hci(0x0c33, zeros, 7);
    hci(0x0c01, zeros, 8);
    hci(0x0c0d, zeros, 7);
    run(GC_CYCLES_PER_FIELD);
    CHECK(wii_state(m)->bluetooth.unknown_commands == before, "and is answered");
  }
}

/* ------------------------------------------------------------------ */
/* A remote's reports                                                  */

/* The ten-bit reading this test expects of an accelerometer: 512 at rest
 * and 104 a g, which is what the remote's own memory must then say. */
static int expected_accel(int milli_g) { return 512 + (int)lround(milli_g * 104.0 / 1000.0); }

static void test_reports(void) {
  WiiRemoteInput input = at_rest();
  unsigned handle, size = 0;
  const uint8_t *r;
  uint8_t data[16];
  uint64_t first = 0, second = 0;
  unsigned i;

  power_up();
  wii_remote_set(m, 0, &input);
  start_listening();
  handle = connect_remote(0);

  /* On connecting, the buttons report it defaults to, once. */
  REPORT(handle, "30 00 00", "a remote that has just connected reports its buttons");
  patience = GC_CYCLES_PER_FIELD * 3u;
  CHECK(input_report(handle, 0x30, NULL, NULL) == NULL, "and then nothing, while nothing changes");
  patience = (uint64_t)GC_CPU_HZ / 4u;

  /* Lights and motor. */
  CHECK(wii_remote_leds(m, 0) == 0u && !wii_remote_rumble(m, 0), "lights and motor start off");
  output(handle, "11 a3");
  REPORT(handle, "22 00 00 11 00", "the lights report is acknowledged when it asks to be");
  CHECK(wii_remote_leds(m, 0) == 0x0au && wii_remote_rumble(m, 0) == 1, "lights 2 and 4, and the motor (%x, %d)",
        wii_remote_leds(m, 0), wii_remote_rumble(m, 0));
  output(handle, "11 10");
  patience = GC_CYCLES_PER_FIELD * 2u;
  CHECK(input_report(handle, 0x22, NULL, NULL) == NULL, "and not when it does not");
  patience = (uint64_t)GC_CPU_HZ / 4u;
  CHECK(wii_remote_leds(m, 0) == 0x01u && wii_remote_rumble(m, 0) == 0, "light 1, and any report's bit 0 stops the motor");
  output(handle, "10 01");
  run(GC_CYCLES_PER_FIELD);
  CHECK(wii_remote_rumble(m, 0) == 1 && wii_remote_leds(m, 0) == 0x01u, "the rumble report leaves the lights alone");
  output(handle, "10 00");

  /* Status: buttons, lights and flags, two zero bytes, battery. */
  input.buttons = WII_REMOTE_HOME | WII_REMOTE_UP;
  wii_remote_set(m, 0, &input);
  output(handle, "15 00");
  REPORT(handle, "20 08 80 10 00 00 c8", "status: Up and Home held, light 1, nothing plugged in, a full battery");
  input.buttons = 0;
  wii_remote_set(m, 0, &input);

  /* The memory: two copies of the accelerometer's calibration, each
   * ending in its sum plus 0x55, saying 512 at rest and 616 at one g. */
  CHECK(read_memory(handle, 0x00, 0x0016, 10, data) == 0u, "the calibration reads");
  {
    unsigned sum = 0x55;
    for (i = 0; i < 9u; i++) sum += data[i];
    CHECK(data[9] == (uint8_t)sum, "its checksum holds");
    CHECK(((data[0] << 2) | ((data[3] >> 4) & 3)) == 512 && ((data[1] << 2) | ((data[3] >> 2) & 3)) == 512 &&
              ((data[2] << 2) | (data[3] & 3)) == 512,
          "zero g is 512 on every axis");
    CHECK(((data[4] << 2) | ((data[7] >> 4) & 3)) == 616 && ((data[5] << 2) | ((data[7] >> 2) & 3)) == 616 &&
              ((data[6] << 2) | (data[7] & 3)) == 616,
          "one g is 616 on every axis");
  }
  {
    uint8_t copy[16];
    CHECK(read_memory(handle, 0x00, 0x0020, 10, copy) == 0u && !memcmp(copy, data, 10), "the second copy is the same");
    CHECK(read_memory(handle, 0x00, 0x0000, 11, data) == 0u && read_memory(handle, 0x00, 0x000b, 11, copy) == 0u &&
              !memcmp(copy, data, 11),
          "and so are the camera's two");
    {
      unsigned sum = 0x55;
      for (i = 0; i < 10u; i++) sum += data[i];
      CHECK(data[10] == (uint8_t)sum, "whose checksum holds");
      CHECK((data[0] | ((data[2] >> 4) & 3) << 8) < 512 && (data[3] | (data[2] & 3) << 8) > 512,
            "and whose reference points are either side of the middle");
    }
  }
  /* A long read comes in sixteens, each saying where it is from. */
  output(handle, "17 00 00 00 10 00 28");
  for (i = 0; i < 3u; i++) {
    r = input_report(handle, 0x21, &size, NULL);
    CHECK(r && size == 22u && r[3] == (i < 2u ? 0xf0u : 0x70u) && wii_be16(r + 4) == 0x0010u + i * 16u,
          "piece %u of a 40-byte read", i);
  }
  CHECK(read_memory(handle, 0x00, 0x16f0, 16, data) == 0u, "the last sixteen bytes of memory read");
  CHECK(read_memory(handle, 0x00, 0x16f8, 16, data) == 8u, "past the end is error 8");
  CHECK(read_memory(handle, 0x00, 0x1700, 1, data) == 8u, "as is the byte after the end");
  CHECK(read_memory(handle, 0x00, 0x7f0016, 10, data) == 0u, "the top byte of an address is ignored");
  CHECK(read_memory(handle, 0x0c, 0x0016, 10, data) != 0u, "both address spaces at once is an error");
  /* Written, it reads back; written past the end, it is refused. */
  write_memory(handle, 0x00, 0x0100, "de ad be ef", 0, "a write to memory is acknowledged");
  CHECK(read_memory(handle, 0x00, 0x0100, 4, data) == 0u && !memcmp(data, "\xde\xad\xbe\xef", 4), "and reads back");
  write_memory(handle, 0x00, 0x16fe, "01 02 03 04", 8, "a write past the end is error 8");

  /* Registers. Nothing plugged in, and no MotionPlus: both say "nothing
   * there", which is error 7; an address no peripheral has is error 8. */
  write_memory(handle, 0x04, 0xa20001, "08 00 40 70 17 60 00", 0, "the speaker's registers take a write");
  CHECK(read_memory(handle, 0x04, 0xa20001, 7, data) == 0u && data[2] == 0x40u && data[4] == 0x17u, "and hold it");
  CHECK(read_memory(handle, 0x04, 0xa400fa, 6, data) == 7u, "reading an extension that is not plugged in is error 7");
  CHECK(read_memory(handle, 0x04, 0xa600fa, 6, data) == 7u, "and so is reading a MotionPlus that is not there");
  write_memory(handle, 0x04, 0xa600fe, "04", 7, "which cannot be switched on either");
  write_memory(handle, 0x04, 0xa400f0, "55", 7, "nor can an extension that is not there be written");
  CHECK(read_memory(handle, 0x04, 0xa80000, 1, data) == 8u, "an address no peripheral has is error 8");
  output(handle, "14 06");
  REPORT(handle, "22 00 00 14 00", "the speaker switches on");
  output(handle, "18 a0 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 11 12 13 14");
  output(handle, "19 06");
  REPORT(handle, "22 00 00 19 00", "sound is taken without comment, and mute acknowledged");
  output(handle, "15 00");
  REPORT(handle, "20 00 00 14 00 00 c8", "status shows the speaker on");
  output(handle, "14 00");

  /* Buttons and accelerometer, continuously: tipped so that every axis
   * reads something else, with buttons that share no byte. */
  input.buttons = WII_REMOTE_A | WII_REMOTE_TWO | WII_REMOTE_DOWN | WII_REMOTE_PLUS;
  input.accel_x = 250;
  input.accel_y = -530;
  input.accel_z = 1010;
  wii_remote_set(m, 0, &input);
  acl_flush();
  output(handle, "12 04 31");
  r = input_report(handle, 0x31, &size, &first);
  CHECK(r && size == 6u, "report 0x31 is buttons and three bytes");
  if (r) {
    const int x = (r[3] << 2) | ((r[1] >> 5) & 3), y = (r[4] << 2) | (((r[2] >> 5) & 1) << 1),
              z = (r[5] << 2) | (((r[2] >> 6) & 1) << 1);
    CHECK((r[1] & 0x1fu) == 0x14u && (r[2] & 0x9fu) == 0x09u, "Down and + in the first byte, A and 2 in the second (%02x %02x)",
          r[1], r[2]);
    CHECK(x == expected_accel(250), "X in ten bits, its low two beside the buttons (%d, wanted %d)", x, expected_accel(250));
    CHECK(y == (expected_accel(-530) & ~1), "Y in nine (%d, wanted %d)", y, expected_accel(-530) & ~1);
    CHECK(z == (expected_accel(1010) & ~1), "Z in nine (%d, wanted %d)", z, expected_accel(1010) & ~1);
  }
  /* Flat on a table, Z reads exactly the calibration's one g. */
  input.accel_x = input.accel_y = 0;
  input.accel_z = 1000;
  wii_remote_set(m, 0, &input);
  run_fields(1);
  acl_flush();
  r = input_report(handle, 0x31, &size, &first);
  CHECK(r && r[3] == 0x80u && r[4] == 0x80u && r[5] == 0x9au && !(r[1] & 0x60u) && !(r[2] & 0x60u),
        "flat on a table reads zero, zero and exactly one g");
  r = input_report(handle, 0x31, &size, &second);
  CHECK(r && second - first == WII_BT_REPORT_CYCLES, "continuous reports are five milliseconds apart (%llu cycles)",
        (unsigned long long)(second - first));
  r = input_report(handle, 0x31, &size, &first);
  CHECK(r && first - second == WII_BT_REPORT_CYCLES, "every time");

  /* Not continuous: a report when something changes, and only then. */
  output(handle, "12 00 30");
  REPORT(handle, "30 14 09", "setting a mode sends one report");
  patience = GC_CYCLES_PER_FIELD * 3u;
  acl_flush();
  CHECK(input_report(handle, 0x30, NULL, NULL) == NULL, "then silence while the buttons are held");
  input.buttons = WII_REMOTE_B | WII_REMOTE_ONE | WII_REMOTE_MINUS | WII_REMOTE_LEFT | WII_REMOTE_RIGHT;
  wii_remote_set(m, 0, &input);
  REPORT(handle, "30 03 16", "and a report when they change: Left and Right, B, 1 and -");
  patience = (uint64_t)GC_CPU_HZ / 4u;

  /* A mode it does not have is refused out loud. */
  output(handle, "12 00 38");
  REPORT(handle, "22 03 16 12 03", "an unknown reporting mode is an error");

  /* Every data report's length, with nothing plugged in and no camera:
   * the bytes that would be theirs are 0xff. */
  {
    static const struct {
      uint8_t mode, length;
    } modes[] = {{0x30, 3}, {0x31, 6}, {0x32, 11}, {0x33, 18}, {0x34, 22}, {0x35, 22}, {0x36, 22}, {0x37, 22}, {0x3d, 22}};
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
      char text[16];
      snprintf(text, sizeof(text), "12 04 %02x", modes[i].mode);
      acl_flush();
      output(handle, text);
      r = input_report(handle, modes[i].mode, &size, NULL);
      CHECK(r && size == modes[i].length, "report %02x is %u bytes (%u)", modes[i].mode, modes[i].length, size);
      if (r && modes[i].mode == 0x3du) CHECK(r[1] == 0xffu && r[21] == 0xffu, "report 0x3d is all extension, and there is none");
      if (r && modes[i].mode == 0x34u) CHECK(r[1] == 0x03u && r[3] == 0xffu && r[21] == 0xffu, "report 0x34: buttons, then extension");
      if (r && modes[i].mode == 0x37u) CHECK(r[3] == 0x80u && r[6] == 0xffu && r[21] == 0xffu, "report 0x37: accelerometer, camera, extension");
    }
  }
}

/* ------------------------------------------------------------------ */
/* The camera                                                          */

/* Where this test expects the two dots: the room wii_remote.c describes,
 * worked out in floating point. */
static void expected_dots(const WiiRemoteInput *in, double x[2], double y[2]) {
  const double middle_x = 512.0 + 280.0 - in->pointer_x * 560.0 / 65535.0;
  /* The bar is 107 units below the middle of the picture. */
  const double middle_y = 384.0 - 210.0 + in->pointer_y * 420.0 / 65535.0 - 107.0;
  const double roll = atan2((double)in->accel_x, (double)in->accel_z);
  unsigned i;
  for (i = 0; i < 2u; i++) {
    const double dx = middle_x + (i ? 59.0 : -59.0) - 512.0, dy = middle_y - 384.0;
    x[i] = 512.0 + dx * cos(roll) - dy * sin(roll);
    y[i] = 384.0 + dx * sin(roll) + dy * cos(roll);
  }
}

static void camera_on(unsigned handle, unsigned mode) {
  char text[8];
  output(handle, "13 06");
  REPORT(handle, "22 00 00 13 00", "the camera's clock");
  output(handle, "1a 06");
  REPORT(handle, "22 00 00 1a 00", "the camera's logic");
  write_memory(handle, 0x04, 0xb00030, "01", 0, "camera: wake");
  write_memory(handle, 0x04, 0xb00000, "02 00 00 71 01 00 aa 00 64", 0, "camera: sensitivity, first block");
  write_memory(handle, 0x04, 0xb0001a, "63 03", 0, "camera: sensitivity, second block");
  snprintf(text, sizeof(text), "%02x", mode);
  write_memory(handle, 0x04, 0xb00033, text, 0, "camera: format");
  write_memory(handle, 0x04, 0xb00030, "08", 0, "camera: start");
}

static void test_camera(void) {
  WiiRemoteInput input = at_rest();
  unsigned handle, size = 0, i;
  const uint8_t *r;
  double x[2], y[2];
  uint8_t data[16];

  power_up();
  /* Aimed at the top left corner of the picture, level. */
  input.pointer_visible = 1;
  input.pointer_x = 0;
  input.pointer_y = 0;
  wii_remote_set(m, 0, &input);
  start_listening();
  handle = connect_remote(0);

  /* Camera off: no dots, whatever the pointer does. */
  output(handle, "12 04 33");
  r = input_report(handle, 0x33, &size, NULL);
  CHECK(r && size == 18u && r[6] == 0xffu && r[17] == 0xffu, "with the camera off there are no dots");

  /* Extended: three bytes a dot. */
  camera_on(handle, 3);
  CHECK(read_memory(handle, 0x04, 0xb00033, 1, data) == 0u && data[0] == 3u, "the camera's format register reads back");
  output(handle, "15 00");
  REPORT(handle, "20 00 00 08 00 00 c8", "status shows the camera on");
  acl_flush();
  r = input_report(handle, 0x33, &size, NULL);
  expected_dots(&input, x, y);
  CHECK(r != NULL, "report 0x33 with the camera on");
  for (i = 0; r && i < 2u; i++) {
    const uint8_t *d = r + 6u + i * 3u;
    const int dx = d[0] | ((d[2] >> 4) & 3) << 8, dy = d[1] | (d[2] >> 6) << 8;
    CHECK(abs(dx - (int)lround(x[i])) <= 1 && abs(dy - (int)lround(y[i])) <= 1,
          "extended format, dot %u at (%d, %d); the corner of the picture puts it at (%.0f, %.0f)", i, dx, dy, x[i], y[i]);
    CHECK((d[2] & 0x0fu) > 0u && (d[2] & 0x0fu) < 8u, "with a small size");
  }
  if (r) CHECK(r[12] == 0xffu && r[13] == 0xffu && r[14] == 0xffu && r[17] == 0xffu, "and the other two slots empty");
  /* Top left of the picture is to the camera's right and low. */
  CHECK(x[0] > 512.0 && y[0] < 384.0 && fabs(x[1] - x[0] - 118.0) < 0.01, "(the dots sit right of and below the middle, 118 apart)");

  /* A format that does not fit the report: the remote sends as much of
   * it as there is room for, whatever a game makes of that. */
  output(handle, "12 04 37");
  acl_flush();
  r = input_report(handle, 0x37, &size, NULL);
  CHECK(r && (r[6] | ((r[8] >> 4) & 3) << 8) == (int)lround(x[0]) && (r[9] | ((r[11] >> 4) & 3) << 8) == (int)lround(x[1]) &&
            r[12] == 0xffu && r[15] == 0xffu,
        "an extended camera in a report with room for ten bytes sends the first ten");

  /* Basic: two dots in five bytes. Aimed low and right, and rolled. */
  input.pointer_x = 58000;
  input.pointer_y = 61000;
  input.accel_x = -500;
  input.accel_z = 866;
  wii_remote_set(m, 0, &input);
  write_memory(handle, 0x04, 0xb00033, "01", 0, "camera: basic format");
  acl_flush();
  r = input_report(handle, 0x37, &size, NULL);
  expected_dots(&input, x, y);
  if (r) {
    const uint8_t *d = r + 6;
    const int x0 = d[0] | ((d[2] >> 4) & 3) << 8, y0 = d[1] | ((d[2] >> 6) & 3) << 8;
    const int x1 = d[3] | (d[2] & 3) << 8, y1 = d[4] | ((d[2] >> 2) & 3) << 8;
    CHECK(abs(x0 - (int)lround(x[0])) <= 1 && abs(y0 - (int)lround(y[0])) <= 1 && abs(x1 - (int)lround(x[1])) <= 1 &&
              abs(y1 - (int)lround(y[1])) <= 1,
          "basic format, rolled 30 degrees: (%d, %d) and (%d, %d); wanted (%.0f, %.0f) and (%.0f, %.0f)", x0, y0, x1, y1,
          x[0], y[0], x[1], y[1]);
    CHECK(y1 != y0, "a rolled remote sees the bar at a slant");
    CHECK(d[5] == 0xffu && d[6] == 0xffu && d[7] == 0xffu && d[8] == 0xffu && d[9] == 0xffu, "and the second pair empty");
  }
  output(handle, "12 04 36");
  acl_flush();
  r = input_report(handle, 0x36, &size, NULL);
  CHECK(r && size == 22u && (r[3] | ((r[5] >> 4) & 3) << 8) == (int)lround(x[0]), "report 0x36 carries the same ten bytes after the buttons");

  /* Full: nine bytes a dot, over two alternating reports that also split
   * the accelerometer between them. */
  input.accel_x = 0;
  input.accel_y = 300;
  input.accel_z = 954;
  input.pointer_x = 32768;
  input.pointer_y = 32768;
  input.buttons = WII_REMOTE_A;
  wii_remote_set(m, 0, &input);
  write_memory(handle, 0x04, 0xb00033, "05", 0, "camera: full format");
  output(handle, "12 04 3e");
  acl_flush();
  {
    const uint8_t *a = input_report(handle, 0x3e, &size, NULL);
    uint8_t first_half[22];
    const uint8_t *b;
    CHECK(a && size == 22u, "report 0x3e");
    if (a) memcpy(first_half, a, 22);
    b = input_report(handle, 0x3f, &size, NULL);
    CHECK(b && size == 22u, "then report 0x3f");
    expected_dots(&input, x, y);
    if (a && b) {
      const int ax = first_half[3] << 2, ay = b[3] << 2;
      const int az = ((((first_half[2] >> 5) & 3) << 6) | (((first_half[1] >> 5) & 3) << 4) | (((b[2] >> 5) & 3) << 2) |
                      ((b[1] >> 5) & 3))
                     << 2;
      const uint8_t *d0 = first_half + 4, *d1 = first_half + 13;
      CHECK((first_half[2] & 0x9fu) == 0x08u && (b[2] & 0x9fu) == 0x08u, "both halves carry the buttons");
      CHECK(ax == (expected_accel(0) & ~3) && ay == (expected_accel(300) & ~3) && az == (expected_accel(954) & ~3),
            "X in one, Y in the other, Z across the spare bits of both (%d, %d, %d)", ax, ay, az);
      CHECK((d0[0] | ((d0[2] >> 4) & 3) << 8) == (int)lround(x[0]) && (d0[1] | (d0[2] >> 6) << 8) == (int)lround(y[0]) &&
                (d1[0] | ((d1[2] >> 4) & 3) << 8) == (int)lround(x[1]),
            "full format: both dots in the first half");
      CHECK(d0[3] <= d0[5] && d0[4] <= d0[6] && d0[5] < 128u && d0[6] < 96u && d0[8] > 0u, "each with a box and a brightness");
      CHECK(b[4] == 0xffu && b[21] == 0xffu, "and the second half's two slots empty");
    }
  }

  /* Aimed off the picture: nothing to see. */
  input.pointer_visible = 0;
  wii_remote_set(m, 0, &input);
  write_memory(handle, 0x04, 0xb00033, "03", 0, "camera: extended format");
  output(handle, "12 04 33");
  run_fields(1);
  acl_flush();
  r = input_report(handle, 0x33, &size, NULL);
  CHECK(r && r[6] == 0xffu && r[7] == 0xffu && r[8] == 0xffu && r[9] == 0xffu, "a remote aimed away sees no dots");
  /* And switched off again. */
  input.pointer_visible = 1;
  wii_remote_set(m, 0, &input);
  output(handle, "13 02");
  REPORT(handle, "22 00 08 13 00", "the camera's clock off");
  acl_flush();
  r = input_report(handle, 0x33, &size, NULL);
  CHECK(r && r[6] == 0xffu, "a camera with no clock sees nothing");
}

/* ------------------------------------------------------------------ */
/* Extensions                                                          */

/* What an extension does to a byte before sending it, with the tables a
 * key of sixteen zeros gives (0x97 throughout): undone here. */
static uint8_t unscramble(uint8_t value) { return (uint8_t)((value ^ 0x97u) + 0x97u); }

static void test_extensions(void) {
  WiiRemoteInput input = at_rest();
  unsigned handle, size = 0, i;
  const uint8_t *r;
  uint8_t data[16], more[16];

  power_up();
  wii_remote_set(m, 0, &input);
  start_listening();
  handle = connect_remote(0);
  REPORT(handle, "30 00 00", "connected");
  output(handle, "12 04 32");
  r = input_report(handle, 0x32, &size, NULL);
  CHECK(r && r[3] == 0xffu, "nothing plugged in");

  /* A Nunchuk goes in: the remote says so unasked, and stops reporting
   * until it is told again what to send. */
  input.extension = WII_EXTENSION_NUNCHUK;
  input.nunchuk_stick_x = -64;
  input.nunchuk_stick_y = 96;
  input.nunchuk_buttons = WII_NUNCHUK_Z;
  input.nunchuk_accel_x = -250;
  input.nunchuk_accel_y = 400;
  input.nunchuk_accel_z = 880;
  wii_remote_set(m, 0, &input);
  run_fields(1);
  REPORT(handle, "20 00 00 02 00 00 c8", "plugging a Nunchuk in sends a status report with the extension flag");
  patience = GC_CYCLES_PER_FIELD * 3u;
  acl_flush();
  CHECK(input_report(handle, 0x32, NULL, NULL) == NULL, "and data reports stop until the mode is set again");
  patience = (uint64_t)GC_CPU_HZ / 4u;

  /* The old way in: a zero written to the key, after which everything
   * the extension sends is obscured with the all-zero key's tables. */
  write_memory(handle, 0x04, 0xa40040, "00", 0, "extension: the old initialisation");
  CHECK(read_memory(handle, 0x04, 0xa400fa, 6, data) == 0u, "its identity reads");
  CHECK(data[4] == 0xfeu && data[5] == 0xfeu, "obscured: a Nunchuk's last two bytes read fe fe (%02x %02x)", data[4], data[5]);
  for (i = 0; i < 6u; i++) data[i] = unscramble(data[i]);
  CHECK(!memcmp(data, "\x00\x00\xa4\x20\x00\x00", 6), "and are 00 00 a4 20 00 00 underneath");
  output(handle, "12 04 32");
  acl_flush();
  r = input_report(handle, 0x32, &size, NULL);
  CHECK(r && size == 11u, "report 0x32 with a Nunchuk");
  if (r) {
    uint8_t n[6];
    int ax, ay, az;
    for (i = 0; i < 6u; i++) n[i] = unscramble(r[3 + i]);
    ax = (n[2] << 2) | ((n[5] >> 2) & 3);
    ay = (n[3] << 2) | ((n[5] >> 4) & 3);
    az = (n[4] << 2) | ((n[5] >> 6) & 3);
    CHECK(n[0] == 128 - 48 && n[1] == 128 + 73, "its stick, obscured in the report as well: (%d, %d)", n[0], n[1]);
    CHECK((n[5] & 3u) == 2u, "Z held reads 0 and C not held reads 1");
    CHECK(ax == 512 - 50 && ay == 512 + 80 && az == 512 + 176, "its accelerometer at 200 a g: (%d, %d, %d)", ax, ay, az);
  }

  /* The new way: 0x55 to 0xf0 and 0 to 0xfb, and nothing is obscured. */
  write_memory(handle, 0x04, 0xa400f0, "55", 0, "extension: the new initialisation, first write");
  write_memory(handle, 0x04, 0xa400fb, "00", 0, "extension: second write");
  CHECK(read_memory(handle, 0x04, 0xa400fa, 6, data) == 0u && !memcmp(data, "\x00\x00\xa4\x20\x00\x00", 6),
        "its identity reads plainly");
  /* Calibration: zero g, one g, then each stick axis's reach and middle;
   * twice, each ending in two sums. */
  CHECK(read_memory(handle, 0x04, 0xa40020, 16, data) == 0u && read_memory(handle, 0x04, 0xa40030, 16, more) == 0u &&
            !memcmp(data, more, 16),
        "a Nunchuk's calibration, twice");
  {
    unsigned sum = 0x55;
    for (i = 0; i < 14u; i++) sum += data[i];
    CHECK(data[14] == (uint8_t)sum && data[15] == (uint8_t)(sum + 0x55u), "with both its checksums");
    CHECK(data[0] == 0x80u && data[4] == 0xb2u, "accelerometer: 512 at rest, 712 at one g");
    CHECK(data[10] == 0x80u && data[13] == 0x80u && data[8] > 0x80u && data[9] < 0x80u, "stick: centred, with room either side");
  }
  r = input_report(handle, 0x32, &size, NULL);
  acl_flush();
  r = input_report(handle, 0x32, &size, NULL);
  CHECK(r && r[3] == 80u && r[4] == 201u && (r[8] & 3u) == 2u, "and its report is plain too");
  CHECK(r && r[9] == 0u && r[10] == 0u, "padded with zeros where the report has room for more");
  CHECK(read_memory(handle, 0x04, 0xa40000, 6, data) == 0u && data[0] == 80u && data[1] == 201u, "the same six bytes read from its registers");
  /* Buttons the other way round, and report 0x37's six bytes. */
  input.nunchuk_buttons = WII_NUNCHUK_C;
  input.nunchuk_stick_x = 127;
  input.nunchuk_stick_y = -128;
  wii_remote_set(m, 0, &input);
  output(handle, "12 04 37");
  run_fields(1);
  acl_flush();
  r = input_report(handle, 0x37, &size, NULL);
  CHECK(r && r[16] == 128 + 96 && r[17] == 128 - 97 && (r[21] & 3u) == 1u, "full right and full down, C held (%d, %d)",
        r ? r[16] : 0, r ? r[17] : 0);

  /* A key of its own, as a retail game's library sets one (the key and its
   * tables are test_extension_cipher's): the same six bytes come back
   * hidden under it, from the registers and in a report, and the console's
   * side of the arithmetic recovers them. */
  {
    static const uint8_t ADD[8] = {0x9a, 0xed, 0xb8, 0xe2, 0x0a, 0x9d, 0xc4, 0xc0};
    static const uint8_t XOR[8] = {0xbc, 0x13, 0x97, 0x78, 0xe8, 0x4d, 0x72, 0xc9};
    const unsigned before = wii_state(m)->ios.unknown_commands;
    uint8_t hidden[6], report[6];
    write_memory(handle, 0x04, 0xa400f0, "aa", 0, "extension: cipher on");
    write_memory(handle, 0x04, 0xa40040, "81 aa ea d3 50 e6", 0, "extension: key, first six");
    write_memory(handle, 0x04, 0xa40046, "d5 3d 6a 47 1d 41", 0, "extension: key, next six");
    write_memory(handle, 0x04, 0xa4004c, "92 d3 e9 5b", 0, "extension: key, last four");
    CHECK(wii_state(m)->ios.unknown_commands == before, "a retail library's key is taken without complaint");
    CHECK(read_memory(handle, 0x04, 0xa40000, 6, hidden) == 0u && hidden[0] != 128 + 96,
          "under it the six bytes read differently");
    for (i = 0; i < 6u; i++) hidden[i] = (uint8_t)((hidden[i] ^ XOR[i & 7u]) + ADD[i & 7u]);
    CHECK(hidden[0] == 128 + 96 && hidden[1] == 128 - 97 && (hidden[5] & 3u) == 1u,
          "and the console's side recovers them: full right, full down, C held (%d, %d, %x)", hidden[0], hidden[1],
          hidden[5]);
    output(handle, "12 04 37");
    run_fields(1);
    acl_flush();
    r = input_report(handle, 0x37, &size, NULL);
    memset(report, 0, sizeof(report));
    if (r) {
      for (i = 0; i < 6u; i++) report[i] = (uint8_t)((r[16 + i] ^ XOR[i & 7u]) + ADD[i & 7u]);
    }
    CHECK(r && memcmp(report, hidden, 6) == 0, "the report's six bytes are hidden the same way");
  }

  /* Out it comes. */
  input.extension = WII_EXTENSION_NONE;
  wii_remote_set(m, 0, &input);
  run_fields(1);
  REPORT(handle, "20 00 00 00 00 00 c8", "unplugging sends a status report without the flag");
  CHECK(read_memory(handle, 0x04, 0xa400fa, 6, data) == 7u, "and its registers are gone");

  /* A Classic Controller: sticks off-centre, triggers part way, a
   * button from each byte. */
  input.extension = WII_EXTENSION_CLASSIC;
  input.classic_left_x = 100;
  input.classic_left_y = -50;
  input.classic_right_x = -90;
  input.classic_right_y = 30;
  input.classic_trigger_l = 200;
  input.classic_trigger_r = 80;
  input.classic_buttons = WII_CLASSIC_A | WII_CLASSIC_X | WII_CLASSIC_ZL | WII_CLASSIC_MINUS | WII_CLASSIC_UP | WII_CLASSIC_R;
  wii_remote_set(m, 0, &input);
  run_fields(1);
  REPORT(handle, "20 00 00 02 00 00 c8", "plugging a Classic Controller in");
  write_memory(handle, 0x04, 0xa400f0, "55", 0, "extension: initialise");
  write_memory(handle, 0x04, 0xa400fb, "00", 0, "extension: initialise");
  CHECK(read_memory(handle, 0x04, 0xa400fa, 6, data) == 0u && !memcmp(data, "\x00\x00\xa4\x20\x01\x01", 6),
        "a Classic Controller's identity");
  CHECK(read_memory(handle, 0x04, 0xa40020, 16, data) == 0u, "its calibration");
  {
    unsigned sum = 0x55;
    for (i = 0; i < 14u; i++) sum += data[i];
    CHECK(data[14] == (uint8_t)sum && data[15] == (uint8_t)(sum + 0x55u), "with both its checksums");
    CHECK(data[2] == 0x80u && data[5] == 0x80u && data[8] == 0x80u && data[11] == 0x80u && data[0] > data[2] &&
              data[1] < data[2],
          "four axes, centred");
  }
  output(handle, "12 04 35");
  acl_flush();
  r = input_report(handle, 0x35, &size, NULL);
  CHECK(r && size == 22u, "report 0x35 with a Classic Controller");
  if (r) {
    const uint8_t *c = r + 6;
    const unsigned lx = c[0] & 63u, ly = c[1] & 63u, rx = ((c[0] >> 6) << 3) | ((c[1] >> 6) << 1) | (c[2] >> 7),
                   ry = c[2] & 31u, lt = (((c[2] >> 5) & 3u) << 3) | (c[3] >> 5), rt = c[3] & 31u;
    const unsigned held = (unsigned)~((c[4] << 8) | c[5]) & 0xffffu;
    CHECK(lx == 52u && ly == 22u, "left stick in six bits: (%u, %u)", lx, ly);
    CHECK(rx == 7u && ry == 19u, "right stick in five, its X in three places: (%u, %u)", rx, ry);
    CHECK(lt == 25u && rt == 10u, "triggers in five: (%u, %u)", lt, rt);
    CHECK(held == input.classic_buttons, "buttons read 0 when held (%04x)", held);
    CHECK(c[4] & 1u, "and the spare bit reads 1");
  }
}

/* ------------------------------------------------------------------ */
/* Service discovery                                                   */

/* One element of an SDP answer: its type, the length of its header and of
 * its value. */
static unsigned element(const uint8_t *p, unsigned *type, unsigned *length) {
  const unsigned index = p[0] & 7u;
  *type = p[0] >> 3;
  if (index < 5u) {
    *length = *type ? 1u << index : 0u;
    return 1;
  }
  if (index == 5u) {
    *length = p[1];
    return 2;
  }
  *length = wii_be16(p + 1);
  return 3;
}

/* An attribute of a record (a sequence of number and value pairs), or
 * NULL. */
static const uint8_t *attribute(const uint8_t *record, unsigned id, unsigned *length) {
  unsigned type, size, header = element(record, &type, &size), at = header;
  const unsigned end = header + size;
  while (at < end) {
    unsigned t, l, h = element(record + at, &t, &l);
    const unsigned this_id = wii_be16(record + at + h);
    at += h + l;
    h = element(record + at, &t, &l);
    if (this_id == id) {
      *length = h + l;
      return record + at;
    }
    at += h + l;
  }
  return NULL;
}

static int contains(const uint8_t *data, unsigned size, const char *text) {
  uint8_t want[32];
  const unsigned n = hex(text, want);
  unsigned i;
  for (i = 0; i + n <= size; i++) {
    if (!memcmp(data + i, want, n)) return 1;
  }
  return 0;
}

/* Ask, and put the pieces of the answer together. Returns its length. */
static unsigned sdp_ask(unsigned handle, unsigned remote_cid, const char *request, uint8_t *out, unsigned *pieces) {
  uint8_t packet[128], asked[96], continuation[3] = {0, 0, 0};
  unsigned total = 0, body, transaction = 1;
  *pieces = 0;
  for (;;) {
    Packet *p;
    /* The request's number, then its parameters and how far the last
     * answer got. */
    body = hex(request, asked) - 1u;
    packet[0] = asked[0];
    memcpy(packet + 5, asked + 1, body);
    memcpy(packet + 5 + body, continuation, continuation[0] ? 3u : 1u);
    body += continuation[0] ? 3u : 1u;
    packet[1] = 0;
    packet[2] = (uint8_t)transaction++;
    wii_put16(packet + 3, (uint16_t)body);
    acl_send(handle, remote_cid, packet, body + 5u);
    p = acl_next(handle, HOST_SDP);
    if (!p || p->data[8] != packet[0] + 1u || p->data[10] != packet[2]) return 0;
    {
      const unsigned count = wii_be16(p->data + 13);
      const uint8_t *tail = p->data + 15 + count;
      CHECK(wii_be16(p->data + 11) == p->size - 13u && count <= 60u, "an SDP answer's lengths agree, within what was asked for");
      memcpy(out + total, p->data + 15, count);
      total += count;
      (*pieces)++;
      if (tail[0] == 0u) return total;
      memcpy(continuation, tail, 3);
    }
  }
}

/* The extension's cipher with a key a console's library makes.
 *
 * Sixteen zeros - all a homebrew library writes - give tables of 0x97
 * throughout, which every description of the scheme states and nothing in
 * the tables' source does: the tables are right if that comes out. The
 * second key is one a retail game's own library wrote to this Nunchuk
 * (Super Mario Galaxy 2, which makes ten bytes up and works out six): that
 * the six are found to be an answer to the ten is the library's arithmetic
 * and this file's agreeing, and the tables are pinned as they were when the
 * game first read its Nunchuk correctly. test_extensions takes the same
 * key the whole way round, through a Nunchuk's registers and reports. */
static void test_extension_cipher(void) {
  static const uint8_t RETAIL_KEY[16] = {0x81, 0xaa, 0xea, 0xd3, 0x50, 0xe6, 0xd5, 0x3d,
                                         0x6a, 0x47, 0x1d, 0x41, 0x92, 0xd3, 0xe9, 0x5b};
  static const uint8_t RETAIL_ADD[8] = {0x9a, 0xed, 0xb8, 0xe2, 0x0a, 0x9d, 0xc4, 0xc0};
  static const uint8_t RETAIL_XOR[8] = {0xbc, 0x13, 0x97, 0x78, 0xe8, 0x4d, 0x72, 0xc9};
  uint8_t key[16], add[8], xor_[8], expected[8];
  unsigned i;

  memset(key, 0, sizeof(key));
  memset(expected, 0x97, sizeof(expected));
  CHECK(wii_extension_cipher_tables(key, add, xor_) == 0, "sixteen zeros are not a key a console's library makes");
  CHECK(memcmp(add, expected, 8) == 0 && memcmp(xor_, expected, 8) == 0, "and their tables are 0x97 throughout");

  CHECK(wii_extension_cipher_tables(RETAIL_KEY, add, xor_) != 0, "a retail library's key is recognised as one");
  CHECK(memcmp(add, RETAIL_ADD, 8) == 0, "its first table");
  CHECK(memcmp(xor_, RETAIL_XOR, 8) == 0, "its second table");

  memcpy(key, RETAIL_KEY, sizeof(key));
  key[12] ^= 0x01u;
  CHECK(wii_extension_cipher_tables(key, add, xor_) == 0, "one bit off in the six, and it is nobody's key");
  CHECK(memcmp(add, RETAIL_ADD, 8) != 0, "with other tables");

  /* What goes out is (byte - add) ^ xor, and comes back as (byte ^ xor) + add. */
  (void)wii_extension_cipher_tables(RETAIL_KEY, add, xor_);
  for (i = 0; i < 256u; i++) {
    const uint8_t sent = (uint8_t)((uint8_t)(i - add[i & 7u]) ^ xor_[i & 7u]);
    if ((uint8_t)((sent ^ xor_[i & 7u]) + add[i & 7u]) != (uint8_t)i) break;
  }
  CHECK(i == 256u, "what the extension hides the console's side recovers");
}

static void test_sdp(void) {
  static uint8_t answer[1024];
  WiiRemoteInput input = at_rest();
  unsigned handle, pieces = 0, size, length = 0, i;
  const uint8_t *value;
  Packet *p;
  uint8_t packet[64];

  power_up();
  wii_remote_set(m, 0, &input);
  start_listening();
  handle = connect_remote(0);

  /* The console opens a channel to the remote's SDP server. */
  signal_send(handle, 0x02, 0x21, "01 00 52 00");
  p = acl_next(handle, 1);
  CHECK(p && p->data[8] == 0x03u && p->data[9] == 0x21u && wii_le16(p->data + 14) == HOST_SDP && wii_le16(p->data + 16) == 0u,
        "the remote accepts a channel for service discovery");
  if (!p) return;
  {
    const unsigned remote_cid = wii_le16(p->data + 12);
    char text[64];
    CHECK(remote_cid >= 0x40u && remote_cid != REMOTE_CONTROL && remote_cid != REMOTE_INTERRUPT, "on a channel of its own (%04x)",
          remote_cid);
    p = acl_next(handle, 1);
    CHECK(p && p->data[8] == 0x04u, "and sends its options");
    snprintf(text, sizeof(text), "%02x %02x 00 00 00 00", remote_cid & 0xffu, remote_cid >> 8);
    if (p) signal_send(handle, 0x05, p->data[9], text);
    snprintf(text, sizeof(text), "%02x %02x 00 00", remote_cid & 0xffu, remote_cid >> 8);
    signal_send(handle, 0x04, 0x22, text);
    p = acl_next(handle, 1);
    CHECK(p && p->data[8] == 0x05u && wii_le16(p->data + 16) == 0u, "and takes the console's");

    /* Everything about its HID service, sixty bytes at a time. */
    size = sdp_ask(handle, remote_cid, "06 35 03 19 11 24 00 3c 35 05 0a 00 00 ff ff", answer, &pieces);
    CHECK(size > 300u && pieces > 5u, "the HID record arrives in pieces (%u bytes in %u)", size, pieces);
    {
      unsigned type, l, h = element(answer, &type, &l);
      const uint8_t *record = answer + h;
      CHECK(type == 6u && h + l == size, "as one list");
      h = element(record, &type, &l);
      CHECK(type == 6u && h + l == size - 3u, "of one record");

      value = attribute(record, 0x0000, &length);
      CHECK(value && length == 5u && wii_be32(value + 1) == 0x00010000u, "its handle");
      value = attribute(record, 0x0001, &length);
      CHECK(value && contains(value, length, "19 11 24"), "service class: Human Interface Device");
      value = attribute(record, 0x0004, &length);
      CHECK(value && contains(value, length, "19 01 00 09 00 11") && contains(value, length, "19 00 11"),
            "reached by L2CAP on PSM 0x11, speaking HID");
      value = attribute(record, 0x000d, &length);
      CHECK(value && contains(value, length, "19 01 00 09 00 13"), "and a second channel on PSM 0x13");
      value = attribute(record, 0x0100, &length);
      CHECK(value && length == 2u + strlen(WII_REMOTE_NAME) && !memcmp(value + 2, WII_REMOTE_NAME, strlen(WII_REMOTE_NAME)),
            "named " WII_REMOTE_NAME);
      value = attribute(record, 0x0102, &length);
      CHECK(value && !memcmp(value + 2, "Nintendo", 8), "made by Nintendo");
      value = attribute(record, 0x0204, &length);
      CHECK(value && value[0] == 0x28u && value[1] == 1u, "a virtual cable");
      value = attribute(record, 0x0205, &length);
      CHECK(value && value[1] == 1u, "that reconnects by itself");
      value = attribute(record, 0x0209, &length);
      CHECK(value && value[1] == 1u, "on batteries");

      /* The report descriptor: one application collection, and in it
       * every report with its length and direction. */
      value = attribute(record, 0x0206, &length);
      CHECK(value != NULL, "it has a descriptor list");
      if (value) {
        static const struct {
          uint8_t id, length, input;
        } want[] = {{0x10, 1, 0}, {0x11, 1, 0}, {0x12, 2, 0}, {0x13, 1, 0}, {0x14, 1, 0}, {0x15, 1, 0},  {0x16, 21, 0},
                    {0x17, 6, 0}, {0x18, 21, 0}, {0x19, 1, 0}, {0x1a, 1, 0}, {0x20, 6, 1}, {0x21, 21, 1}, {0x22, 4, 1},
                    {0x30, 2, 1}, {0x31, 5, 1}, {0x32, 10, 1}, {0x33, 17, 1}, {0x34, 21, 1}, {0x35, 21, 1}, {0x36, 21, 1},
                    {0x37, 21, 1}, {0x3d, 21, 1}, {0x3e, 21, 1}, {0x3f, 21, 1}};
        unsigned t, l2, at = element(value, &t, &l2), found = 0, id = 0, count = 0, bits = 0, depth = 0;
        const uint8_t *d;
        unsigned dl;
        at += element(value + at, &t, &l2); /* the inner list */
        CHECK(value[at] == 0x08u && value[at + 1u] == 0x22u, "of one report descriptor");
        at += 2;
        at += element(value + at, &t, &l2);
        d = value + at;
        dl = l2;
        CHECK(t == 4u && dl == 217u, "217 bytes long (%u)", dl);
        CHECK(d[0] == 0x05u && d[1] == 0x01u && d[2] == 0x09u && d[3] == 0x05u, "a game pad on the generic desktop page");
        for (i = 0; i < dl;) {
          const unsigned item = d[i], n = (item & 3u) == 3u ? 4u : (item & 3u);
          const unsigned v = n ? d[i + 1u] : 0u;
          if ((item & 0xfcu) == 0xa0u) depth++;
          if ((item & 0xfcu) == 0xc0u) depth--;
          if ((item & 0xfcu) == 0x84u) id = v;
          if ((item & 0xfcu) == 0x94u) count = v;
          if ((item & 0xfcu) == 0x74u) bits = v;
          if ((item & 0xfcu) == 0x80u || (item & 0xfcu) == 0x90u) {
            const unsigned is_input = (item & 0xfcu) == 0x80u;
            CHECK(found < sizeof(want) / sizeof(want[0]) && want[found].id == id && want[found].length == count &&
                      want[found].input == is_input && bits == 8u,
                  "report %02x: %u bytes, %s", id, count, is_input ? "in" : "out");
            found++;
          }
          i += 1u + n;
        }
        CHECK(found == sizeof(want) / sizeof(want[0]) && depth == 0u, "all twenty-five reports, in a closed collection (%u)", found);
      }
    }

    /* Its Device Identification: Nintendo's vendor number, the remote's
     * product number. */
    size = sdp_ask(handle, remote_cid, "06 35 03 19 12 00 00 3c 35 05 0a 00 00 ff ff", answer, &pieces);
    CHECK(size > 40u, "the Device ID record");
    if (size) {
      unsigned type, l, h = element(answer, &type, &l);
      const uint8_t *record = answer + h;
      value = attribute(record, 0x0201, &length);
      CHECK(value && value[0] == 0x09u && wii_be16(value + 1) == 0x057eu, "vendor 0x057e");
      value = attribute(record, 0x0202, &length);
      CHECK(value && wii_be16(value + 1) == 0x0306u, "product 0x0306");
      CHECK(attribute(record, 0x0206, &length) == NULL, "and no HID attributes in it");
    }
    /* Only some attributes; a 128-bit UUID; a class nobody has. */
    size = sdp_ask(handle, remote_cid,
                   "06 35 11 1c 00 00 11 24 00 00 10 00 80 00 00 80 5f 9b 34 fb 00 3c 35 06 09 00 01 09 02 01", answer, &pieces);
    CHECK(size == 3u + 3u + (3u + 6u) + (3u + 3u) && pieces == 1u, "two attributes asked for by number, with the long form of the UUID (%u)",
          size);
    size = sdp_ask(handle, remote_cid, "06 35 03 19 11 0a 00 3c 35 05 0a 00 00 ff ff", answer, &pieces);
    CHECK(size == 3u, "a service the remote does not have is an empty list (%u)", size);

    /* The two older requests: which records, then one record's
     * attributes. */
    {
      const unsigned n = hex("02 00 09 00 08 35 03 19 01 00 00 10 00", packet);
      acl_send(handle, remote_cid, packet, n);
      p = acl_next(handle, HOST_SDP);
      CHECK(p && p->size == 8u + 5u + 4u + 8u + 1u && p->data[8] == 0x03u && wii_be16(p->data + 13) == 2u &&
                wii_be32(p->data + 17) == 0x00010000u && wii_be32(p->data + 21) == 0x00010001u,
            "Service Search for L2CAP finds both records");
    }
    size = sdp_ask(handle, remote_cid, "04 00 01 00 01 00 3c 35 03 09 02 02", answer, &pieces);
    CHECK(size == 3u + 3u + 3u && wii_be16(answer + 7) == 0x0306u, "Service Attribute for the second: its product number");
    {
      const unsigned n = hex("04 00 0a 00 0c 00 01 00 07 00 3c 35 03 09 02 02 00", packet);
      acl_send(handle, remote_cid, packet, n);
      p = acl_next(handle, HOST_SDP);
      CHECK(p && p->data[8] == 0x01u && wii_be16(p->data + 13) == 2u, "and for a record that does not exist, an error");
    }

    /* Closed again. */
    snprintf(text, sizeof(text), "%02x %02x 52 00", remote_cid & 0xffu, remote_cid >> 8);
    signal_send(handle, 0x06, 0x23, text);
    p = acl_next(handle, 1);
    CHECK(p && p->data[8] == 0x07u && p->data[9] == 0x23u, "the remote agrees to close the channel");
  }
  /* The rest of L2CAP's signalling. */
  signal_send(handle, 0x08, 0x30, "de ad be ef");
  p = acl_next(handle, 1);
  CHECK(p && p->size == 16u && p->data[8] == 0x09u && p->data[9] == 0x30u && !memcmp(p->data + 12, "\xde\xad\xbe\xef", 4),
        "an echo comes back");
  signal_send(handle, 0x02, 0x31, "03 00 53 00");
  p = acl_next(handle, 1);
  CHECK(p && p->data[8] == 0x03u && wii_le16(p->data + 16) == 2u, "a PSM the remote has nothing on is refused");
  signal_send(handle, 0x04, 0x32, "77 00 00 00");
  p = acl_next(handle, 1);
  CHECK(p && p->data[8] == 0x01u && wii_le16(p->data + 12) == 2u, "options for a channel that is not open are rejected");
  signal_send(handle, 0x1f, 0x33, "");
  p = acl_next(handle, 1);
  CHECK(p && p->data[8] == 0x01u && p->data[9] == 0x33u && wii_le16(p->data + 12) == 0u, "and a command nobody defined is not understood");
}

/* ------------------------------------------------------------------ */
/* Coming and going                                                    */

static void test_links(void) {
  WiiRemoteInput input = at_rest();
  unsigned handle;
  Packet *p;
  uint64_t gone;

  power_up();
  wii_remote_set(m, 0, &input);
  start_listening();
  handle = connect_remote(0);
  REPORT(handle, "30 00 00", "connected");
  output(handle, "11 20");

  /* Switched off: the link drops with "the remote is powering off". */
  input.present = 0;
  wii_remote_set(m, 0, &input);
  EVENT("05 04 00 00 01 15", "a remote switched off: Disconnection Complete, reason 0x15");
  gone = m->cpu.cycles;
  CHECK(wii_remote_leds(m, 0) == 0u, "its lights go out");
  CHECK(event_next() == NULL, "and it does not come back by itself");
  /* Switched on again: it asks, from scratch. */
  input.present = 1;
  wii_remote_set(m, 0, &input);
  handle = connect_remote(0);
  REPORT(handle, "30 00 00", "a remote switched back on connects and reports again");
  CHECK(m->cpu.cycles > gone, "(later)");

  /* The control channel's long way round, which homebrew uses: SET_REPORT
   * answered by a handshake. */
  {
    uint8_t set[3] = {0x52, 0x11, 0x40};
    acl_send(handle, REMOTE_CONTROL, set, 3);
    p = acl_next(handle, HOST_CONTROL);
    CHECK(p && p->size == 9u && p->data[8] == 0x00u, "SET_REPORT on the control channel is answered with a handshake");
    CHECK(wii_remote_leds(m, 0) == 4u, "and does what the report says");
  }

  /* The console hangs up. The remote goes away and stays away. */
  COMMAND(0x0406, "00 01 13", "0f 04 00 01 06 04", "Disconnect");
  EVENT("05 04 00 00 01 16", "Disconnection Complete: ended by the console");
  patience = (uint64_t)GC_CPU_HZ * 3u;
  CHECK(event_next() == NULL, "a remote the console disconnected stays off");
  /* Until a button is pressed. */
  input.buttons = WII_REMOTE_A;
  wii_remote_set(m, 0, &input);
  patience = (uint64_t)GC_CPU_HZ / 4u;
  handle = connect_remote(0);
  REPORT(handle, "30 00 08", "a button wakes it, and the button is in its first report");

  /* A request refused: the remote tries again in a couple of seconds. */
  COMMAND(0x0406, "00 01 13", "0f 04 00 01 06 04", "Disconnect");
  EVENT("05 04 00 00 01 16", "Disconnection Complete");
  input.buttons = 0;
  wii_remote_set(m, 0, &input);
  run_fields(2);
  input.buttons = WII_REMOTE_B;
  wii_remote_set(m, 0, &input);
  EVENT("04 0a 10 57 43 4e 1e 00 04 25 00 01", "Connection Request");
  COMMAND(0x040a, "10 57 43 4e 1e 00 0d", "0f 04 00 01 0a 04", "Reject Connection Request");
  EVENT("03 0b 0d 00 00 10 57 43 4e 1e 00 01 00", "Connection Complete, with the reason given");
  CHECK(event_next() == NULL, "not at once");
  patience = (uint64_t)GC_CPU_HZ * 3u;
  EVENT("04 0a 10 57 43 4e 1e 00 04 25 00 01", "but again after a while");
  patience = (uint64_t)GC_CPU_HZ / 4u;
  COMMAND(0x0409, "11 57 43 4e 1e 00 00", "0f 04 0c 01 09 04", "accepting a remote that did not ask is disallowed");

  /* Not listening: nobody asks. */
  COMMAND(0x040a, "10 57 43 4e 1e 00 0d", "0f 04 00 01 0a 04", "Reject");
  EVENT("03 0b 0d 00 00 10 57 43 4e 1e 00 01 00", "Connection Complete");
  COMMAND(0x0c1a, "00", "0e 04 01 1a 0c 00", "Write Scan Enable: off");
  patience = (uint64_t)GC_CPU_HZ * 3u;
  CHECK(event_next() == NULL, "a console that is not listening hears from nobody");
}

/* The other direction: the console looks for remotes and connects to
 * one, as software does that pairs by discovery. */
static void test_discovery(void) {
  WiiRemoteInput input = at_rest();
  Packet *p;
  uint64_t started;
  unsigned handle = 0x0101u;

  power_up();
  wii_remote_set(m, 1, &input);
  wii_remote_set(m, 3, &input);
  hci(0x0c03, NULL, 0);
  EVENT("0e 04 01 03 0c 00", "Reset");

  /* An inquiry of 2.56 seconds finds the two that are switched on. */
  started = m->cpu.cycles;
  patience = (uint64_t)GC_CPU_HZ * 4u;
  COMMAND(0x0401, "33 8b 9e 02 00", "0f 04 00 01 01 04", "Inquiry");
  EVENT("02 0f 01 11 57 43 4e 1e 00 01 00 00 04 25 00 00 00", "Inquiry Result: remote 1");
  EVENT("02 0f 01 13 57 43 4e 1e 00 01 00 00 04 25 00 00 00", "Inquiry Result: remote 3");
  p = event_next();
  same(p, "01 01 00", "Inquiry Complete", __LINE__);
  CHECK(p && p->cycle - started >= (uint64_t)GC_CPU_HZ * 256u / 100u && p->cycle - started < (uint64_t)GC_CPU_HZ * 260u / 100u,
        "2.56 seconds after it began");
  /* With signal strength, when that mode is set; and cancelled. */
  COMMAND(0x0c45, "01", "0e 04 01 45 0c 00", "Write Inquiry Mode");
  COMMAND(0x0401, "00 8b 9e 30 01", "0f 04 00 01 01 04", "Inquiry, for one answer");
  EVENT("22 0f 01 11 57 43 4e 1e 00 01 00 04 25 00 00 00 c8", "Inquiry Result with RSSI");
  EVENT("01 01 00", "Inquiry Complete, as soon as it has its one");
  COMMAND(0x0401, "33 8b 9e 30 00", "0f 04 00 01 01 04", "Inquiry, for a minute");
  COMMAND(0x0401, "33 8b 9e 30 00", "0f 04 0c 01 01 04", "a second while the first runs is disallowed");
  COMMAND(0x0402, "", "0e 04 01 02 04 00", "Inquiry Cancel");
  patience = (uint64_t)GC_CPU_HZ;
  CHECK(event_next() == NULL, "a cancelled inquiry reports nothing more");
  COMMAND(0x0402, "", "0e 04 01 02 04 0c", "and cannot be cancelled twice");
  patience = (uint64_t)GC_CPU_HZ / 4u;

  /* Its name. */
  COMMAND(0x0419, "11 57 43 4e 1e 00 01 00 00 00", "0f 04 00 01 19 04", "Remote Name Request");
  p = event_next();
  CHECK(p && p->size == 257u && p->data[0] == 0x07u && p->data[2] == 0u && !memcmp(p->data + 3, "\x11\x57\x43\x4e\x1e\x00", 6) &&
            !strcmp((const char *)p->data + 9, WII_REMOTE_NAME),
        "Remote Name Request Complete: " WII_REMOTE_NAME);

  /* Connect, pair with a PIN, open both channels from the console. */
  COMMAND(0x0405, "11 57 43 4e 1e 00 18 cc 01 00 00 00 01", "0f 04 00 01 05 04", "Create Connection");
  EVENT("03 0b 00 01 01 11 57 43 4e 1e 00 01 00", "Connection Complete");
  COMMAND(0x0405, "11 57 43 4e 1e 00 18 cc 01 00 00 00 01", "0f 04 0b 01 05 04", "a second Create Connection: already connected");
  COMMAND(0x0411, "01 01", "0f 04 00 01 11 04", "Authentication Requested");
  EVENT("17 06 11 57 43 4e 1e 00", "Link Key Request");
  COMMAND(0x040c, "11 57 43 4e 1e 00", "0e 0a 01 0c 04 00 11 57 43 4e 1e 00", "Link Key Request Negative Reply");
  EVENT("16 06 11 57 43 4e 1e 00", "PIN Code Request");
  /* The PIN is the console's address, as the sync button makes it. */
  COMMAND(0x040d, "11 57 43 4e 1e 00 06 01 57 43 4e 1e 00 00 00 00 00 00 00 00 00 00 00",
          "0e 0a 01 0d 04 00 11 57 43 4e 1e 00", "PIN Code Request Reply");
  p = event_next();
  CHECK(p && p->size == 25u && p->data[0] == 0x18u && !memcmp(p->data + 2, "\x11\x57\x43\x4e\x1e\x00", 6) && p->data[24] == 0u,
        "Link Key Notification: a combination key for the remote");
  EVENT("06 03 00 01 01", "Authentication Complete");
  if (p) {
    /* And that key is the one the module has stored for it. */
    uint8_t one[7] = {0x11, 0x57, 0x43, 0x4e, 0x1e, 0x00, 0x00};
    const Packet *notified = p;
    Packet *keys;
    hci(0x0c0d, one, 7);
    keys = event_next();
    CHECK(keys && keys->data[0] == 0x15u && !memcmp(keys->data + 9, notified->data + 8, 16),
          "the same key the module holds for it");
    EVENT("0e 08 01 0d 0c 00 10 00 01 00", "one key read");
  }
  CHECK(acl_next(handle, 0) == NULL, "a remote the console connected to waits to be asked");
  signal_send(handle, 0x02, 0x41, "11 00 50 00");
  p = acl_next(handle, 1);
  same(p, "01 21 10 00 0c 00 01 00 03 41 08 00 40 00 50 00 00 00 00 00", "the console's control channel is accepted", __LINE__);
  p = acl_next(handle, 1);
  CHECK(p && p->data[8] == 0x04u, "the remote's options");
  if (p) signal_send(handle, 0x05, p->data[9], "40 00 00 00 00 00");
  signal_send(handle, 0x04, 0x42, "40 00 00 00");
  p = acl_next(handle, 1);
  CHECK(p && p->data[8] == 0x05u, "the console's accepted");
  signal_send(handle, 0x02, 0x43, "13 00 51 00");
  p = acl_next(handle, 1);
  CHECK(p && p->data[8] == 0x03u && wii_le16(p->data + 12) == REMOTE_INTERRUPT && wii_le16(p->data + 16) == 0u, "and its interrupt channel");
  p = acl_next(handle, 1);
  if (p) signal_send(handle, 0x05, p->data[9], "41 00 00 00 00 00");
  signal_send(handle, 0x04, 0x44, "41 00 00 00");
  p = acl_next(handle, 1);
  CHECK(p && p->data[8] == 0x05u, "configured");
  REPORT(handle, "30 00 00", "and the remote reports");
  CHECK(acl_next(0x0103u, 0) == NULL && wii_state(m)->bluetooth.link[3].state == WII_BT_LINK_NONE, "the other remote is left alone");

  /* An address nobody answers at: the page times out, when the timeout
   * says. */
  COMMAND(0x0c18, "40 06", "0e 04 01 18 0c 00", "Write Page Timeout: one second");
  started = m->cpu.cycles;
  COMMAND(0x0405, "99 57 43 4e 1e 00 18 cc 01 00 00 00 01", "0f 04 00 01 05 04", "Create Connection to nobody");
  patience = (uint64_t)GC_CPU_HZ * 2u;
  p = event_next();
  same(p, "03 0b 04 00 00 99 57 43 4e 1e 00 01 00", "Connection Complete: page timeout", __LINE__);
  CHECK(p && p->cycle - started >= GC_CPU_HZ && p->cycle - started < GC_CPU_HZ + 2u * GC_CYCLES_PER_FIELD,
        "after a second (%llu cycles)", p ? (unsigned long long)(p->cycle - started) : 0ull);
  COMMAND(0x0419, "12 57 43 4e 1e 00 01 00 00 00", "0f 04 00 01 19 04",
          "Remote Name Request to a remote that is switched off");
  p = event_next();
  CHECK(p && p->data[0] == 0x07u && p->data[2] == 0x04u, "fails with a page timeout");
}

/* ------------------------------------------------------------------ */
/* Flow control                                                        */

static void test_flow(void) {
  WiiRemoteInput input = at_rest();
  unsigned handle, i, before, arrived;

  power_up();
  wii_remote_set(m, 0, &input);
  start_listening();
  handle = connect_remote(0);
  REPORT(handle, "30 00 00", "connected");

  /* Out: every packet the console sends is counted back to it. */
  run_fields(1);
  (void)event_next();
  before = completed_packets;
  for (i = 0; i < 25u; i++) output(handle, "10 00");
  patience = GC_CYCLES_PER_FIELD * 2u;
  CHECK(event_next() == NULL, "(nothing else arrives)");
  CHECK(completed_packets - before == 25u, "twenty-five packets sent, twenty-five counted back (%u)", completed_packets - before);
  {
    Packet *last = &events[event_count - 1u];
    CHECK(last->size == 7u && !memcmp(last->data, "\x13\x05\x01\x00\x01\x01\x00", 7), "each as Number Of Completed Packets for its connection");
  }

  /* In: a stack that asks to count packets back gets only as many as it
   * said it has room for, until it counts some back. */
  COMMAND(0x0c33, "53 01 40 03 00 00 00", "0e 04 01 33 0c 00", "Host Buffer Size: three packets");
  COMMAND(0x0c31, "01", "0e 04 01 31 0c 00", "Set Host Controller To Host Flow Control");
  acl_flush();
  output(handle, "12 04 30");
  run_fields(6);
  for (arrived = 0, i = 0; i < acl_count; i++) arrived += acl[i].taken ? 0u : 1u;
  CHECK(arrived == 3u, "a remote reporting 200 times a second gets three packets through (%u)", arrived);
  acl_flush();
  {
    uint8_t counted[5] = {1, 0x00, 0x01, 2, 0};
    hci(0x0c35, counted, 5);
  }
  run_fields(6);
  for (arrived = 0, i = 0; i < acl_count; i++) arrived += acl[i].taken ? 0u : 1u;
  CHECK(arrived == 2u, "and two more when two are counted back (%u)", arrived);
  CHECK(event_next() == NULL, "which no event answers");
  COMMAND(0x0c31, "00", "0e 04 01 31 0c 00", "flow control off");
  acl_flush();
  run_fields(3);
  for (arrived = 0, i = 0; i < acl_count; i++) arrived += acl[i].taken ? 0u : 1u;
  CHECK(arrived >= 9u && arrived <= 11u, "and the reports flow again, ten in three fields (%u)", arrived);
}

/* ------------------------------------------------------------------ */
/* Four at once                                                        */

static void test_four(void) {
  WiiRemoteInput input;
  unsigned handle[4], i, count[4] = {0, 0, 0, 0};
  uint64_t last[4] = {0, 0, 0, 0};
  int evenly = 1, right = 1;

  power_up();
  for (i = 0; i < 4u; i++) {
    input = at_rest();
    input.buttons = (uint16_t)(1u << i); /* 2, 1, B, A */
    wii_remote_set(m, i, &input);
  }
  start_listening();
  /* All four ask at once; the stack takes them in turn. */
  for (i = 0; i < 4u; i++) expect_request(i);
  for (i = 0; i < 4u; i++) handle[i] = accept_request(i);
  for (i = 0; i < 4u; i++) {
    remote_opens(handle[i], 0x11, REMOTE_CONTROL, HOST_CONTROL);
    remote_opens(handle[i], 0x13, REMOTE_INTERRUPT, HOST_INTERRUPT);
  }
  for (i = 0; i < 4u; i++) {
    char text[16];
    snprintf(text, sizeof(text), "11 %02x", 0x10u << i);
    output(handle[i], text);
    output(handle[i], "12 04 31");
  }
  run_fields(2);
  for (i = 0; i < 4u; i++) CHECK(wii_remote_leds(m, i) == (1u << i), "remote %u shows light %u", i, i + 1u);
  acl_flush();
  run_fields(12);
  for (i = 0; i < acl_count; i++) {
    const Packet *p = &acl[i];
    const unsigned which = (wii_le16(p->data) & 0x0fffu) - 0x0100u;
    if (p->taken || which >= 4u || p->data[9] != 0x31u) continue;
    if (last[which] && p->cycle - last[which] != WII_BT_REPORT_CYCLES) evenly = 0;
    if (p->data[11] != (1u << which)) right = 0;
    last[which] = p->cycle;
    count[which]++;
  }
  for (i = 0; i < 4u; i++) CHECK(count[i] >= 38u && count[i] <= 41u, "remote %u sent 40 reports in 12 fields (%u)", i, count[i]);
  CHECK(evenly, "each remote's reports are exactly five milliseconds apart");
  CHECK(right, "and each carries its own buttons on its own connection");
  /* One leaves; the others carry on. */
  wii_remote_rest(&input, 0);
  wii_remote_set(m, 2, &input);
  EVENT("05 04 00 02 01 15", "remote 2 switched off");
  acl_flush();
  run_fields(3);
  memset(count, 0, sizeof(count));
  for (i = 0; i < acl_count; i++) {
    const unsigned which = (wii_le16(acl[i].data) & 0x0fffu) - 0x0100u;
    if (!acl[i].taken && which < 4u) count[which]++;
  }
  CHECK(count[0] >= 9u && count[1] >= 9u && count[2] == 0u && count[3] >= 9u, "the other three carry on (%u %u %u %u)", count[0],
        count[1], count[2], count[3]);
}

/* ------------------------------------------------------------------ */
/* The same run twice                                                  */

static uint64_t scripted_run(unsigned *count) {
  WiiRemoteInput input = at_rest();
  unsigned handle, i;
  power_up();
  wii_remote_set(m, 0, &input);
  start_listening();
  handle = connect_remote(0);
  output(handle, "11 10");
  output(handle, "15 00");
  output(handle, "17 00 00 00 16 00 0a");
  camera_on(handle, 3);
  output(handle, "12 04 33");
  for (i = 0; i < 60u; i++) {
    /* The input changes at fields, as a frontend's does. */
    input.buttons = (uint16_t)((i & 1u) ? WII_REMOTE_A : 0u) | (uint16_t)((i % 5u) == 0u ? WII_REMOTE_LEFT : 0u);
    input.accel_x = (int16_t)((int)i * 17 - 500);
    input.accel_z = (int16_t)(1000 - (int)i * 7);
    input.pointer_visible = (uint8_t)(i % 7u != 0u);
    input.pointer_x = (uint16_t)(i * 1000u);
    input.pointer_y = (uint16_t)(65535u - i * 900u);
    if (i == 20u) input.extension = WII_EXTENSION_NUNCHUK;
    if (i == 24u) output(handle, "12 04 37");
    if (i == 40u) input.extension = WII_EXTENSION_NONE;
    if (i == 44u) output(handle, "12 00 31");
    input.nunchuk_stick_x = (int8_t)(i * 3);
    wii_remote_set(m, 0, &input);
    run_fields(1);
  }
  *count = traffic_count;
  return traffic_hash;
}

static void test_determinism(void) {
  unsigned count_a = 0, count_b = 0;
  const uint64_t a = scripted_run(&count_a);
  const uint64_t cycles_a = m->cpu.cycles;
  const uint64_t b = scripted_run(&count_b);
  CHECK(count_a > 150u, "the scripted run is a real one (%u packets)", count_a);
  CHECK(a == b && count_a == count_b && cycles_a == m->cpu.cycles,
        "two runs of the same script cross the same bytes at the same cycles (%u and %u packets)", count_a, count_b);
}

int main(void) {
  gc_console_select(1);
  test_device();
  test_startup_lwbt();
  test_startup_bte();
  test_reports();
  test_camera();
  test_extension_cipher();
  test_extensions();
  test_sdp();
  test_links();
  test_discovery();
  test_flow();
  test_four();
  test_determinism();
  if (m) gc_machine_destroy(m);
  printf("wii_bt_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
