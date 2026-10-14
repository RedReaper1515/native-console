/*
 * IOS, at high level.
 *
 * A Wii has a second processor, an ARM called Starlet, and it runs an
 * operating system of its own - IOS - that owns the disc drive, the flash
 * memory, the USB ports and the Bluetooth radio. A game never touches those
 * devices. It writes a request block into main memory, hands Starlet the
 * block's address through two registers, and waits for an interrupt that
 * says the block has been answered: open, close, read, write, seek, ioctl
 * and ioctlv on named devices, the way a Unix program talks to its kernel.
 *
 * That protocol is the whole of what a game can see of IOS, so it is what
 * is implemented here: the registers and the request blocks exactly, and
 * behind them C code in place of Starlet's. No part of IOS itself is needed
 * or used (docs/WII-PLAN.md, "No copyrighted firmware").
 *
 * A device is a table of functions (WiiIosDevice). Each gets the request
 * already decoded - host pointers to the guest's buffers, sizes checked
 * against memory - and returns the result the request block is answered
 * with. Time is the machine's: a reply arrives a fixed number of processor
 * cycles after the request unless the device says otherwise, so a run is
 * the same every time and the interpreter and recompiled code cannot
 * disagree about when an answer came.
 *
 * Everything here that is state lives in WiiIos, which is plain data: no
 * pointers into the host. A device that needs host resources (an open file,
 * a socket) keeps them in a table of its own, keyed by what is in here, and
 * rebuilds them when a state is loaded.
 */
#ifndef WII_IOS_H
#define WII_IOS_H

#include <stddef.h>
#include <stdint.h>

#include "gc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Request types, as the request block's first word holds them. */
enum {
  WII_IOS_OPEN = 1,
  WII_IOS_CLOSE = 2,
  WII_IOS_READ = 3,
  WII_IOS_WRITE = 4,
  WII_IOS_SEEK = 5,
  WII_IOS_IOCTL = 6,
  WII_IOS_IOCTLV = 7,
  WII_IOS_REPLY = 8
};

/* The kernel's own error numbers. A device's errors are its own (the
 * filesystem's start at -101, ES's at -1000) and are returned as they are. */
enum {
  WII_IPC_OK = 0,
  WII_IPC_EACCES = -1,
  WII_IPC_EEXIST = -2,
  WII_IPC_EINVAL = -4,
  WII_IPC_EMAX = -5,   /* no descriptor left */
  WII_IPC_ENOENT = -6, /* no such device or file */
  WII_IPC_EQUEUEFULL = -8,
  WII_IPC_EIO = -12,
  WII_IPC_ENOMEM = -22
};

/* Returned by a device function that will answer later (wii_ios_reply): a
 * read on an interrupt endpoint that has nothing yet, the hook a game leaves
 * with the power-button device. The request block is not answered and no
 * interrupt is raised until the device calls wii_ios_reply with its address
 * (WiiIosCall::request). */
#define WII_IOS_LATER ((int32_t)0x80000001)

/* How long Starlet takes, in processor cycles: to acknowledge that it has
 * taken a request off the registers, and to answer one that needs no
 * device time. Both are of the order measured on a console (tens of
 * microseconds and a few hundred, at 729 cycles a microsecond); nothing
 * correct depends on the exact figures, only on their being the same every
 * run. */
#define WII_IOS_ACK_CYCLES 20000u
#define WII_IOS_REPLY_CYCLES 200000u

#define WII_IOS_FILES 24u
#define WII_IOS_QUEUE 48u
#define WII_IOS_PATH 64u
#define WII_IOS_ABSENT_SEEN 32u

/* One open descriptor. `device` indexes the device table; the rest is the
 * device's to use. `path` is what was opened, which is the file's name for
 * the flash filesystem and the endpoint for anything else. */
typedef struct WiiIosFile {
  uint8_t open;
  uint8_t device;
  uint16_t reserved;
  uint32_t mode;
  uint32_t position;
  uint32_t user[4];
  char path[WII_IOS_PATH];
} WiiIosFile;

/* Something Starlet owes the processor: an acknowledgement or an answer,
 * and the cycle it is ready at. */
typedef struct WiiIosEvent {
  uint64_t due;
  uint32_t request; /* the block's physical address */
  int32_t result;
  uint8_t kind;     /* WII_IOS_EVENT_* */
  uint8_t used;
  uint16_t reserved;
  uint32_t order;   /* ties go to whichever was queued first */
} WiiIosEvent;

enum { WII_IOS_EVENT_ACK = 1, WII_IOS_EVENT_REPLY = 2 };

typedef struct WiiIos {
  WiiIosFile files[WII_IOS_FILES];
  WiiIosEvent queue[WII_IOS_QUEUE];
  uint32_t next_order;
  /* The version a game sees: (major << 16) | revision, at 0x80003140. */
  uint32_t version;
  /* Counters for the end-of-run report: a request to a device nobody
   * wrote is the first thing to look for when a game stops. */
  uint32_t requests;
  uint32_t unknown_devices;
  uint32_t unknown_commands;
  /* The devices that were looked for and are not there, by a hash of their
   * names, so that each is reported once. */
  uint32_t absent_seen[WII_IOS_ABSENT_SEEN];
} WiiIos;

/* One decoded request, as a device function sees it. */
typedef struct WiiIosCall {
  GcMachine *m;
  uint32_t request; /* for wii_ios_reply, when answering later */
  int fd;
  WiiIosFile *file;
  /* Cycles from now until the answer. A device that models time (a disc
   * read) sets it; the default is WII_IOS_REPLY_CYCLES. */
  uint64_t delay;
} WiiIosCall;

/* One buffer of an ioctlv: a physical address and a length. The first
 * `in_count` are inputs, the rest are the device's to fill. */
typedef struct WiiIosVector {
  uint32_t address;
  uint32_t size;
} WiiIosVector;

/* A device. Any function may be null: the request is then answered with
 * WII_IPC_EINVAL and counted as unknown. `open` returns 0 (the kernel
 * assigns the descriptor) or an error. Buffers are host pointers into guest
 * memory, null when the guest passed a null pointer or a zero length. */
typedef struct WiiIosDevice {
  /* "/dev/es", or a prefix when `prefix` is set ("/dev/net/"). A device
   * named "/" takes every path that names no device: the flash filesystem's
   * files. */
  const char *name;
  int prefix;
  int32_t (*open)(WiiIosCall *call, const char *path, uint32_t mode);
  int32_t (*close)(WiiIosCall *call);
  int32_t (*read)(WiiIosCall *call, uint8_t *data, uint32_t size);
  int32_t (*write)(WiiIosCall *call, const uint8_t *data, uint32_t size);
  int32_t (*seek)(WiiIosCall *call, int32_t where, int32_t whence);
  int32_t (*ioctl)(WiiIosCall *call, uint32_t command, uint8_t *in, uint32_t in_size,
                   uint8_t *out, uint32_t out_size);
  int32_t (*ioctlv)(WiiIosCall *call, uint32_t command, unsigned in_count, unsigned io_count,
                    const WiiIosVector *vectors);
  /* Once a field, for a device with something to do when no request is
   * outstanding (a remote sending its buttons). May be null. */
  void (*field)(GcMachine *m);
  /* When the machine is reset or IOS is reloaded: forget everything. */
  void (*reset)(GcMachine *m);
} WiiIosDevice;

/* The buffer behind a vector, or NULL (a null address, a zero length, or
 * bytes that are not all memory). */
uint8_t *wii_ios_buffer(GcMachine *m, const WiiIosVector *vector);
/* Guest memory at a physical address, for a device that has to follow a
 * pointer of its own. */
uint8_t *wii_ios_memory(GcMachine *m, uint32_t physical, uint32_t size);

/* Answer a request a device function returned WII_IOS_LATER for, `delay`
 * cycles from now. */
void wii_ios_reply(GcMachine *m, uint32_t request, int32_t result, uint64_t delay);

/* Big-endian fields in a guest buffer. */
static inline uint16_t wii_be16(const uint8_t *p) { return (uint16_t)(((uint32_t)p[0] << 8) | p[1]); }
static inline uint32_t wii_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t wii_be64(const uint8_t *p) {
  return ((uint64_t)wii_be32(p) << 32) | wii_be32(p + 4);
}
static inline void wii_put16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}
static inline void wii_put32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}
static inline void wii_put64(uint8_t *p, uint64_t value) {
  wii_put32(p, (uint32_t)(value >> 32));
  wii_put32(p + 4, (uint32_t)value);
}

/* The kernel's side, called by the machine (wii.c). */
void wii_ios_reset(GcMachine *m);
/* The processor has put a request block's address on the registers. */
void wii_ios_request(GcMachine *m, uint32_t request);
/* Deliver whatever is due and can be delivered. */
void wii_ios_advance(GcMachine *m);
/* The earliest cycle at which wii_ios_advance could have something to do,
 * or `next` when that is sooner. */
uint64_t wii_ios_next_event(GcMachine *m, uint64_t next);
/* Once a field. */
void wii_ios_field(GcMachine *m);
/* The descriptor table, for the end-of-run report and the tests. */
const WiiIosDevice *wii_ios_device_of(const WiiIosFile *file);

#ifdef __cplusplus
}
#endif

#endif /* WII_IOS_H */
