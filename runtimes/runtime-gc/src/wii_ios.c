/*
 * IOS's kernel, as far as a game can see it: request blocks in, answers
 * out, and a table of descriptors in between (wii_ios.h).
 *
 * The request block (WiiBrew, "IOS/Resource request"; libogc's ipc.c is the
 * other side of it):
 *
 *   +0x00  the request's type, replaced by 8 when it is answered
 *   +0x04  the result
 *   +0x08  the descriptor, replaced by the original type when answered
 *   +0x0c  five words of arguments, whose meaning depends on the type
 *
 * Every pointer in it is a physical address. A request is acknowledged
 * (the mailbox's Y2) soon after it is handed over, which is what lets a
 * game queue the next one, and answered (Y1, with the block's address in
 * Starlet's message register) when the device is done. Only one of either
 * can be outstanding: Starlet waits for the processor to clear the flag
 * and the interrupt before it raises the next.
 */

#include <stdlib.h>
#include <string.h>

#include "wii.h"

/* The devices, each in its own file. */
extern const WiiIosDevice wii_ios_es;
extern const WiiIosDevice wii_ios_stm_immediate;
extern const WiiIosDevice wii_ios_stm_eventhook;
extern const WiiIosDevice wii_ios_di;
extern const WiiIosDevice wii_ios_fs;
extern const WiiIosDevice wii_ios_file;
extern const WiiIosDevice wii_ios_bluetooth;
extern const WiiIosDevice wii_ios_absent;

/* Searched in order. The filesystem's files come last because they are
 * whatever is not a device. */
static const WiiIosDevice *const devices[] = {
    &wii_ios_es,
    &wii_ios_stm_immediate,
    &wii_ios_stm_eventhook,
    &wii_ios_di,
    &wii_ios_fs,
    &wii_ios_bluetooth,
    &wii_ios_absent,
    &wii_ios_file,
};
#define DEVICE_COUNT (sizeof(devices) / sizeof(devices[0]))

const WiiIosDevice *wii_ios_device_of(const WiiIosFile *file) {
  return (file && file->open && file->device < DEVICE_COUNT) ? devices[file->device] : NULL;
}

uint8_t *wii_ios_memory(GcMachine *m, uint32_t physical, uint32_t size) {
  if (!physical || !size) return NULL;
  return gc_ram_at_register(m, physical, size);
}

uint8_t *wii_ios_buffer(GcMachine *m, const WiiIosVector *vector) {
  return wii_ios_memory(m, vector->address, vector->size);
}

/* ------------------------------------------------------------------ */
/* What Starlet owes                                                   */

static void enqueue(GcMachine *m, uint8_t kind, uint32_t request, int32_t result, uint64_t delay) {
  WiiIos *ios = &wii_state(m)->ios;
  unsigned i;
  for (i = 0; i < WII_IOS_QUEUE; i++) {
    WiiIosEvent *event = &ios->queue[i];
    if (event->used) continue;
    event->used = 1;
    event->kind = kind;
    event->request = request;
    event->result = result;
    event->due = m->cpu.cycles + delay;
    event->order = ios->next_order++;
    m->cpu.next_event = 0;
    return;
  }
  /* Forty-eight outstanding requests is more than any descriptor table
   * allows; reaching this means a device is not answering. */
  if (m->log) fprintf(m->log, "wii: IOS has no room to remember request %08x\n", request);
}

void wii_ios_reply(GcMachine *m, uint32_t request, int32_t result, uint64_t delay) {
  enqueue(m, WII_IOS_EVENT_REPLY, request, result, delay);
}

/* The mailbox is free when neither flag is up and the processor has taken
 * the last interrupt. */
static int mailbox_free(const WiiState *w) {
  return !w->ipc_y1 && !w->ipc_y2 && !(w->irq_flags & (1u << WII_IRQ_IPC_BROADWAY));
}

static WiiIosEvent *earliest(WiiIos *ios) {
  WiiIosEvent *best = NULL;
  unsigned i;
  for (i = 0; i < WII_IOS_QUEUE; i++) {
    WiiIosEvent *event = &ios->queue[i];
    if (!event->used) continue;
    if (!best || event->due < best->due || (event->due == best->due && event->order < best->order)) {
      best = event;
    }
  }
  return best;
}

void wii_ios_advance(GcMachine *m) {
  WiiState *w = wii_state(m);
  WiiIosEvent *event;
  if (!w || !mailbox_free(w)) return;
  event = earliest(&w->ios);
  if (!event || event->due > m->cpu.cycles) return;

  if (event->kind == WII_IOS_EVENT_ACK) {
    w->ipc_y2 = 1;
  } else {
    uint8_t *block = wii_ios_memory(m, event->request, 0x20);
    if (block) {
      const uint32_t type = wii_be32(block);
      wii_put32(block + 8, type);
      wii_put32(block, WII_IOS_REPLY);
      wii_put32(block + 4, (uint32_t)event->result);
    }
    w->ipc_arm_message = event->request;
    w->ipc_y1 = 1;
  }
  event->used = 0;
  wii_ipc_update(m);
}

uint64_t wii_ios_next_event(GcMachine *m, uint64_t next) {
  WiiState *w = wii_state(m);
  WiiIosEvent *event;
  /* While the mailbox is busy nothing here is waiting on the clock: it is
   * waiting on the processor, whose every register access is looked at. */
  if (!w || !mailbox_free(w)) return next;
  event = earliest(&w->ios);
  if (event && event->due < next) return event->due;
  return next;
}

void wii_ios_field(GcMachine *m) {
  unsigned i;
  for (i = 0; i < DEVICE_COUNT; i++) {
    if (devices[i]->field) devices[i]->field(m);
  }
}

void wii_ios_reset(GcMachine *m) {
  WiiState *w = wii_state(m);
  unsigned i;
  const uint32_t version = w->ios.version;
  memset(&w->ios, 0, sizeof(w->ios));
  w->ios.version = version;
  for (i = 0; i < DEVICE_COUNT; i++) {
    if (devices[i]->reset) devices[i]->reset(m);
  }
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */

static const WiiIosDevice *find_device(const char *path, unsigned *index) {
  unsigned i;
  for (i = 0; i < DEVICE_COUNT; i++) {
    const WiiIosDevice *device = devices[i];
    const size_t length = strlen(device->name);
    if (device->prefix ? strncmp(path, device->name, length) == 0 : strcmp(path, device->name) == 0) {
      /* The filesystem's files are every path that is not a device's. */
      if (device == &wii_ios_file && strncmp(path, "/dev/", 5) == 0) continue;
      *index = i;
      return device;
    }
  }
  return NULL;
}

static void unknown(GcMachine *m, const char *what, const char *path, uint32_t command) {
  WiiIos *ios = &wii_state(m)->ios;
  ios->unknown_commands++;
  if (m->log && ios->unknown_commands <= 32) {
    fprintf(m->log, "wii: IOS %s %s, command %08x, pc %08x\n", what, path, command, m->cpu.pc);
  }
}

static int32_t do_open(GcMachine *m, WiiIosCall *call, const uint8_t *block) {
  WiiIos *ios = &wii_state(m)->ios;
  const uint32_t path_at = wii_be32(block + 0x0c);
  const uint32_t mode = wii_be32(block + 0x10);
  const uint8_t *guest_path = wii_ios_memory(m, path_at, 1);
  char path[WII_IOS_PATH] = {0}; /* whole, so that what is kept of it is the same every run */
  const WiiIosDevice *device;
  unsigned index = 0, fd, i;
  int32_t result;

  if (!guest_path) return WII_IPC_EINVAL;
  for (i = 0; i + 1u < sizeof(path); i++) {
    const uint8_t *byte = wii_ios_memory(m, path_at + i, 1);
    if (!byte || !*byte) break;
    path[i] = (char)*byte;
  }
  path[i] = 0;

  device = find_device(path, &index);
  if (!device) {
    ios->unknown_devices++;
    if (m->log && ios->unknown_devices <= 32) {
      fprintf(m->log, "wii: IOS has no device %s, pc %08x\n", path, m->cpu.pc);
    }
    return WII_IPC_ENOENT;
  }
  for (fd = 0; fd < WII_IOS_FILES; fd++) {
    if (!ios->files[fd].open) break;
  }
  if (fd == WII_IOS_FILES) return WII_IPC_EMAX;

  call->fd = (int)fd;
  call->file = &ios->files[fd];
  memset(call->file, 0, sizeof(*call->file));
  call->file->device = (uint8_t)index;
  call->file->mode = mode;
  memcpy(call->file->path, path, sizeof(path));
  result = device->open ? device->open(call, path, mode) : 0;
  if (result < 0) return result;
  call->file->open = 1;
  return (int32_t)fd;
}

void wii_ios_request(GcMachine *m, uint32_t request) {
  WiiState *w = wii_state(m);
  WiiIos *ios = &w->ios;
  uint8_t *block = wii_ios_memory(m, request, 0x20);
  WiiIosCall call;
  const WiiIosDevice *device = NULL;
  uint32_t type, argument[5];
  int32_t result = WII_IPC_EINVAL;
  unsigned i;

  ios->requests++;
  /* Taken, whatever it turns out to be. */
  enqueue(m, WII_IOS_EVENT_ACK, request, 0, WII_IOS_ACK_CYCLES);
  if (!block) {
    if (m->log) fprintf(m->log, "wii: an IOS request at %08x, which is not memory\n", request);
    return;
  }

  type = wii_be32(block);
  for (i = 0; i < 5u; i++) argument[i] = wii_be32(block + 0x0c + 4u * i);
  memset(&call, 0, sizeof(call));
  call.m = m;
  call.request = request;
  call.delay = WII_IOS_REPLY_CYCLES;
  call.fd = (int)wii_be32(block + 8);

  if (type == WII_IOS_OPEN) {
    result = do_open(m, &call, block);
  } else if (call.fd < 0 || (unsigned)call.fd >= WII_IOS_FILES || !ios->files[call.fd].open) {
    result = WII_IPC_EINVAL;
  } else {
    call.file = &ios->files[call.fd];
    device = devices[call.file->device];
    switch (type) {
      case WII_IOS_CLOSE:
        result = device->close ? device->close(&call) : 0;
        call.file->open = 0;
        break;
      case WII_IOS_READ:
        if (device->read) {
          result = device->read(&call, wii_ios_memory(m, argument[0], argument[1]), argument[1]);
        } else {
          unknown(m, "has no read for", call.file->path, 0);
        }
        break;
      case WII_IOS_WRITE:
        if (device->write) {
          result = device->write(&call, wii_ios_memory(m, argument[0], argument[1]), argument[1]);
        } else {
          unknown(m, "has no write for", call.file->path, 0);
        }
        break;
      case WII_IOS_SEEK:
        if (device->seek) {
          result = device->seek(&call, (int32_t)argument[0], (int32_t)argument[1]);
        } else {
          unknown(m, "has no seek for", call.file->path, 0);
        }
        break;
      case WII_IOS_IOCTL:
        if (device->ioctl) {
          result = device->ioctl(&call, argument[0], wii_ios_memory(m, argument[1], argument[2]),
                                 argument[2], wii_ios_memory(m, argument[3], argument[4]), argument[4]);
        } else {
          unknown(m, "has no ioctl for", call.file->path, argument[0]);
        }
        break;
      case WII_IOS_IOCTLV: {
        WiiIosVector vectors[32];
        const unsigned total = argument[1] + argument[2];
        const uint8_t *table = wii_ios_memory(m, argument[3], total * 8u);
        if (argument[1] > 32u || argument[2] > 32u || total > 32u || (total && !table)) {
          result = WII_IPC_EINVAL;
          break;
        }
        for (i = 0; i < total; i++) {
          vectors[i].address = wii_be32(table + i * 8u);
          vectors[i].size = wii_be32(table + i * 8u + 4u);
        }
        if (device->ioctlv) {
          result = device->ioctlv(&call, argument[0], argument[1], argument[2], vectors);
        } else {
          unknown(m, "has no ioctlv for", call.file->path, argument[0]);
        }
        break;
      }
      default:
        if (m->log) fprintf(m->log, "wii: an IOS request of type %u\n", type);
        break;
    }
  }

  if (m->trace_io && m->log) {
    fprintf(m->log, "wii: IOS %u fd %d (%s) %08x %08x %08x -> %d\n", type, call.fd,
            call.file ? call.file->path : "?", argument[0], argument[1], argument[2], (int)result);
  } else if (m->log) {
    /* WII_IOS_TRACE in the environment: the requests alone, with the field
     * each was made in, and without the Bluetooth module's (hundreds a
     * second, and its own trace) unless the variable says "all". The whole
     * I/O trace of a game is a gigabyte a minute; this is what a game that
     * has stopped was last asking for. */
    static int wanted = -1;
    if (wanted < 0) {
      const char *env = getenv("WII_IOS_TRACE");
      wanted = !env || !*env || !strcmp(env, "0") ? 0 : (!strcmp(env, "all") ? 2 : 1);
    }
    if (wanted && (wanted == 2 || !call.file || strncmp(call.file->path, "/dev/usb/oh1", 12) != 0)) {
      char name[WII_IOS_PATH] = "";
      if (type == WII_IOS_OPEN) {
        const uint8_t *path = wii_ios_memory(m, argument[0], 1);
        unsigned k;
        for (k = 0; path && k + 1u < sizeof(name); k++) {
          const uint8_t *c = wii_ios_memory(m, argument[0] + k, 1);
          if (!c || !*c) break;
          name[k] = (char)*c;
        }
      }
      fprintf(m->log, "wii: field %llu IOS %u fd %d (%s) %08x %08x %08x -> %d\n", (unsigned long long)m->field, type,
              call.fd, name[0] ? name : (call.file ? call.file->path : "?"), argument[0], argument[1], argument[2],
              (int)result);
    }
  }
  if (result == WII_IOS_LATER) return;
  /* Never before the acknowledgement: that is the order a game expects. */
  if (call.delay <= WII_IOS_ACK_CYCLES) call.delay = WII_IOS_ACK_CYCLES + 1u;
  enqueue(m, WII_IOS_EVENT_REPLY, request, result, call.delay);
}

/* ------------------------------------------------------------------ */
/* The power-button device                                             */

/* Two descriptors: one that answers at once, and one a game leaves a
 * request with so as to be told when reset or power is pressed. Nothing
 * presses them here yet, so the hook is held until the game takes it back
 * - which is exactly what happens on a console where nobody touches the
 * buttons. */
enum {
  STM_EVENT_HOOK = 0x1000,
  STM_HOT_RESET = 0x2001,
  STM_HOT_RESET_FOR_PD = 0x2002,
  STM_SHUTDOWN = 0x2003,
  STM_IDLE = 0x2004,
  STM_WAKEUP = 0x2005,
  STM_GET_IDLE_MODE = 0x3001,
  STM_RELEASE_EVENT_HOOK = 0x3002,
  STM_READ_DDR_REG = 0x4001,
  STM_READ_DDR_REG2 = 0x4002,
  STM_VIDEO_DIMMING = 0x5001,
  STM_LED_FLASH = 0x6001,
  STM_LED_MODE = 0x6002,
  STM_READ_VERSION = 0x7001
};

static int32_t stm_immediate_ioctl(WiiIosCall *call, uint32_t command, uint8_t *in, uint32_t in_size,
                                   uint8_t *out, uint32_t out_size) {
  WiiState *w = wii_state(call->m);
  (void)in;
  (void)in_size;
  switch (command) {
    case STM_RELEASE_EVENT_HOOK:
      if (w->stm.hook_request) {
        wii_ios_reply(call->m, w->stm.hook_request, 0, WII_IOS_REPLY_CYCLES);
        w->stm.hook_request = 0;
      }
      return 0;
    case STM_HOT_RESET:
    case STM_HOT_RESET_FOR_PD:
    case STM_SHUTDOWN:
      /* The game asked for the console to restart or turn off. */
      call->m->finished = 1;
      return 0;
    case STM_READ_VERSION:
      if (out && out_size >= 4u) wii_put32(out, 0x20080519u);
      return 0;
    case STM_IDLE:
    case STM_WAKEUP:
    case STM_GET_IDLE_MODE:
    case STM_VIDEO_DIMMING:
    case STM_LED_FLASH:
    case STM_LED_MODE:
    case STM_READ_DDR_REG:
    case STM_READ_DDR_REG2:
      if (out && out_size) memset(out, 0, out_size);
      return 0;
    default: break;
  }
  unknown(call->m, "does not know the power device's", call->file->path, command);
  return WII_IPC_EINVAL;
}

static int32_t stm_eventhook_ioctl(WiiIosCall *call, uint32_t command, uint8_t *in, uint32_t in_size,
                                   uint8_t *out, uint32_t out_size) {
  WiiState *w = wii_state(call->m);
  (void)in;
  (void)in_size;
  (void)out;
  (void)out_size;
  if (command != STM_EVENT_HOOK) {
    unknown(call->m, "does not know the power device's", call->file->path, command);
    return WII_IPC_EINVAL;
  }
  if (w->stm.hook_request) return WII_IPC_EEXIST;
  w->stm.hook_request = call->request;
  return WII_IOS_LATER;
}

static void stm_reset(GcMachine *m) { wii_state(m)->stm.hook_request = 0; }

const WiiIosDevice wii_ios_stm_immediate = {
    "/dev/stm/immediate", 0, NULL, NULL, NULL, NULL, NULL, stm_immediate_ioctl, NULL, NULL, stm_reset,
};

const WiiIosDevice wii_ios_stm_eventhook = {
    "/dev/stm/eventhook", 0, NULL, NULL, NULL, NULL, NULL, stm_eventhook_ioctl, NULL, NULL, NULL,
};

/* ------------------------------------------------------------------ */
/* Devices that are not there                                          */

/* A console with nothing plugged into its USB ports, no SD card and no
 * network. Software opens these to look, and "no such device" is the true
 * answer. Each name is said once on the log, because the first thing to ask
 * of a game that stops is which device it went looking for - and only
 * once, because looking is normal and some software looks every second. */
static int32_t absent_open(WiiIosCall *call, const char *path, uint32_t mode) {
  WiiIos *ios = &wii_state(call->m)->ios;
  uint32_t hash = 2166136261u;
  const char *c;
  unsigned i;
  (void)mode;
  for (c = path; *c; c++) hash = (hash ^ (uint8_t)*c) * 16777619u;
  if (!hash) hash = 1;
  for (i = 0; i < WII_IOS_ABSENT_SEEN; i++) {
    if (ios->absent_seen[i] == hash) return WII_IPC_ENOENT;
    if (!ios->absent_seen[i]) break;
  }
  if (i < WII_IOS_ABSENT_SEEN) ios->absent_seen[i] = hash;
  ios->unknown_devices++;
  if (call->m->log) {
    fprintf(call->m->log, "wii: IOS has no device %s (a console with nothing plugged in), pc %08x\n", path,
            call->m->cpu.pc);
  }
  return WII_IPC_ENOENT;
}

const WiiIosDevice wii_ios_absent = {
    "/dev/", 1, absent_open, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};
