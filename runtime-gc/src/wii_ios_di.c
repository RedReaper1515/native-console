/*
 * /dev/di: the disc drive, as IOS presents it.
 *
 * On a GameCube a game drives the drive's registers itself (gc_io.c). On a
 * Wii the registers are IOS's and a game sends IOS the same commands as
 * requests - a block of words whose first byte is the command - and is
 * handed back the drive's verdict as the result: 1 for "transfer complete",
 * 2 for "drive error". What IOS adds is the reason the drive is behind it at
 * all: the game partition is encrypted, and a read through an open partition
 * comes back decrypted (gc_disc.h).
 *
 * The command numbers and argument layouts are WiiBrew's /dev/di page's and
 * libogc's libdi; nothing is modelled of the mechanism - a seek moves no
 * head - only of the time a read takes, so that a game that starts a read
 * and carries on drawing sees it finish later rather than at once.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_disc.h"
#include "gc_memory.h"
#include "wii.h"

enum {
  DI_INQUIRY = 0x12,
  DI_READ_DISC_ID = 0x70,
  DI_READ = 0x71,
  DI_WAIT_FOR_COVER_CLOSE = 0x79,
  DI_GET_COVER_REGISTER = 0x7a,
  DI_NOTIFY_RESET = 0x7e,
  DI_SET_SPINUP_FLAG = 0x7f,
  DI_READ_DVD_PHYSICAL = 0x80,
  DI_READ_DVD_COPYRIGHT = 0x81,
  DI_READ_DVD_DISC_KEY = 0x82,
  DI_GET_LENGTH = 0x83,
  DI_GET_DI_MMCR = 0x84,
  DI_GET_DI_BNR = 0x85,
  DI_CLEAR_COVER_INTERRUPT = 0x86,
  DI_GET_COVER_STATUS = 0x88,
  DI_ENABLE_COVER_INTERRUPT = 0x89,
  DI_RESET = 0x8a,
  DI_OPEN_PARTITION = 0x8b,
  DI_CLOSE_PARTITION = 0x8c,
  DI_UNENCRYPTED_READ = 0x8d,
  DI_ENABLE_DVD_VIDEO = 0x8e,
  DI_GET_NO_DISC_OPEN_PARTITION_PARAMS = 0x90,
  DI_NO_DISC_OPEN_PARTITION = 0x91,
  DI_GET_NO_DISC_BUFFER_SIZES = 0x92,
  DI_OPEN_PARTITION_WITH_TMD_AND_TICKET = 0x93,
  DI_OPEN_PARTITION_WITH_TMD_AND_TICKET_VIEW = 0x94,
  DI_GET_STATUS_REGISTER = 0x95,
  DI_GET_CONTROL_REGISTER = 0x96,
  DI_REPORT_KEY = 0xa4,
  DI_SEEK = 0xab,
  DI_READ_DVD = 0xd0,
  DI_READ_DVD_CONFIG = 0xd1,
  DI_STOP_LASER = 0xd2,
  DI_OFFSET = 0xd3,
  DI_READ_DISC_BCA = 0xd4,
  DI_REQUEST_DISC_STATUS = 0xd5,
  DI_REQUEST_RETRY_NUMBER = 0xd6,
  DI_SET_MAXIMUM_ROTATION = 0xd7,
  DI_SER_MEAS_CONTROL = 0xdf,
  DI_REQUEST_ERROR = 0xe0,
  DI_AUDIO_STREAM = 0xe1,
  DI_REQUEST_AUDIO_STATUS = 0xe2,
  DI_STOP_MOTOR = 0xe3,
  DI_AUDIO_BUFFER_CONFIG = 0xe4
};

/* What the drive says. */
#define DI_OK 1
#define DI_DRIVE_ERROR 2
#define DI_COVER_CLOSED 4
#define DI_SECURITY_ERROR 0x20
#define DI_BAD_ARGUMENT 0x80

/* The drive's own error words, for the "what went wrong" request. */
#define DI_ERROR_NONE 0x00000000u
#define DI_ERROR_NO_DISC 0x01023a00u
#define DI_ERROR_INVALID_COMMAND 0x00052000u
#define DI_ERROR_OUT_OF_RANGE 0x00052100u

/* A read's time. A Wii's drive manages three to eight megabytes a second;
 * this answers at sixty, so that loading is not the wait it was on a
 * console and a game still sees a read as something that takes a while.
 * Nothing a game does correctly depends on the figure. */
#define DI_BYTES_PER_SECOND 60000000ull

static uint64_t read_delay(uint32_t bytes) {
  return WII_IOS_REPLY_CYCLES + ((uint64_t)bytes * GC_CPU_HZ) / DI_BYTES_PER_SECOND;
}

/* Disc bytes into guest memory, by way of memory of the host's: the guest's
 * pages can be held for a copy the graphics thread still owes them
 * (gc_memory.h), and only an access from here - not one from inside the C
 * library's read - knows how to wait for that. */
static int read_into(GcMachine *m, int raw, uint64_t offset, uint8_t *out, uint32_t size) {
  uint8_t *scratch;
  int result;
  if (!m->disc || !out || !size) return -1;
  scratch = (uint8_t *)malloc(size);
  if (!scratch) return -1;
  result = raw ? gc_disc_read_raw(m->disc, offset, scratch, size) : gc_disc_read(m->disc, offset, scratch, size);
  if (result == 0) memcpy(out, scratch, size);
  free(scratch);
  return result;
}

static int32_t fail(GcMachine *m, uint32_t error) {
  wii_state(m)->di.error = error;
  return DI_DRIVE_ERROR;
}

static int32_t di_ioctl(WiiIosCall *call, uint32_t command, uint8_t *in, uint32_t in_size, uint8_t *out,
                        uint32_t out_size) {
  GcMachine *m = call->m;
  WiiState *w = wii_state(m);
  const uint32_t a1 = (in && in_size >= 8u) ? wii_be32(in + 4) : 0u;
  const uint32_t a2 = (in && in_size >= 12u) ? wii_be32(in + 8) : 0u;

  /* The request names the command twice: as the request's own number, and
   * as the first byte of the block. They agree. */
  if (!in || in_size < 4u || in[0] != command) return DI_BAD_ARGUMENT;

  switch (command) {
    case DI_INQUIRY:
      /* What drive this is: a revision, a date and a version, as the
       * GameCube's drive answers the same question. */
      if (!out || out_size < 0x20u) return DI_BAD_ARGUMENT;
      memset(out, 0, out_size);
      wii_put16(out, 0x0002u);
      wii_put32(out + 4, 0x20060526u);
      out[8] = 0x41;
      w->di.error = DI_ERROR_NONE;
      return DI_OK;

    case DI_READ_DISC_ID:
      if (!out || out_size < 0x20u) return DI_BAD_ARGUMENT;
      if (!m->disc) return fail(m, DI_ERROR_NO_DISC);
      if (read_into(m, 1, 0, out, 0x20u) != 0) return fail(m, DI_ERROR_OUT_OF_RANGE);
      w->di.error = DI_ERROR_NONE;
      return DI_OK;

    case DI_READ: {
      /* From the open partition, decrypted: a length, and an offset in
       * words. */
      const uint32_t size = a1;
      const uint64_t offset = (uint64_t)a2 << 2;
      if (!m->disc) return fail(m, DI_ERROR_NO_DISC);
      if (!w->di.partition_open) return DI_SECURITY_ERROR;
      if (!out || out_size < size) return DI_BAD_ARGUMENT;
      if (read_into(m, 0, offset, out, size) != 0) return fail(m, DI_ERROR_OUT_OF_RANGE);
      w->di.error = DI_ERROR_NONE;
      w->di.reads++;
      w->di.bytes += size;
      call->delay = read_delay(size);
      return DI_OK;
    }

    case DI_UNENCRYPTED_READ: {
      /* The disc itself, outside any partition: its header, the partition
       * table, the region block. */
      const uint32_t size = a1;
      const uint64_t offset = (uint64_t)a2 << 2;
      if (!m->disc) return fail(m, DI_ERROR_NO_DISC);
      if (!out || out_size < size) return DI_BAD_ARGUMENT;
      if (read_into(m, 1, offset, out, size) != 0) return fail(m, DI_ERROR_OUT_OF_RANGE);
      w->di.error = DI_ERROR_NONE;
      call->delay = read_delay(size);
      return DI_OK;
    }

    case DI_CLOSE_PARTITION:
      w->di.partition_open = 0;
      return DI_OK;

    case DI_GET_COVER_STATUS:
      /* 2: a disc is in. 1: the slot is empty. */
      if (out && out_size >= 4u) wii_put32(out, m->disc ? 2u : 1u);
      return DI_OK;

    case DI_GET_COVER_REGISTER:
      /* The drive's cover register: bit 0 set while there is no disc. */
      if (out && out_size >= 4u) wii_put32(out, m->disc ? 0u : 1u);
      return DI_OK;

    case DI_WAIT_FOR_COVER_CLOSE:
      /* With a disc in, the cover has closed. With none, the wait is never
       * over: nothing here inserts a disc into a running machine. */
      return m->disc ? DI_COVER_CLOSED : WII_IOS_LATER;

    case DI_REQUEST_ERROR:
      if (out && out_size >= 4u) wii_put32(out, w->di.error);
      w->di.error = DI_ERROR_NONE; /* asking clears it */
      return DI_OK;

    case DI_GET_STATUS_REGISTER:
    case DI_GET_CONTROL_REGISTER:
    case DI_GET_LENGTH:
    case DI_GET_DI_MMCR:
    case DI_GET_DI_BNR:
      if (out && out_size >= 4u) wii_put32(out, 0u);
      return DI_OK;

    case DI_READ_DISC_BCA:
      /* The burst cutting area: sixty-four bytes pressed into the disc's
       * hub. A retail disc's is all zero but for one bit games look for. */
      if (!out || out_size < 0x40u) return DI_BAD_ARGUMENT;
      memset(out, 0, 0x40u);
      out[0x33] = 1;
      return DI_OK;

    case DI_RESET:
      w->di.partition_open = 0;
      w->di.error = DI_ERROR_NONE;
      return DI_OK;

    case DI_SEEK:
    case DI_STOP_MOTOR:
    case DI_STOP_LASER:
    case DI_OFFSET:
    case DI_AUDIO_BUFFER_CONFIG:
    case DI_AUDIO_STREAM:
    case DI_CLEAR_COVER_INTERRUPT:
    case DI_ENABLE_COVER_INTERRUPT:
    case DI_NOTIFY_RESET:
    case DI_SET_SPINUP_FLAG:
    case DI_REQUEST_DISC_STATUS:
    case DI_REQUEST_RETRY_NUMBER:
    case DI_SET_MAXIMUM_ROTATION:
    case DI_SER_MEAS_CONTROL:
    case DI_ENABLE_DVD_VIDEO:
      return DI_OK;

    case DI_REQUEST_AUDIO_STATUS:
      if (out && out_size >= 4u) wii_put32(out, 0u);
      return DI_OK;

    /* A game disc is not a film: the commands that read one are refused the
     * way the drive refuses them. */
    case DI_READ_DVD:
    case DI_READ_DVD_CONFIG:
    case DI_READ_DVD_PHYSICAL:
    case DI_READ_DVD_COPYRIGHT:
    case DI_READ_DVD_DISC_KEY:
    case DI_REPORT_KEY:
      return fail(m, DI_ERROR_INVALID_COMMAND);

    default: break;
  }

  /* Answered as if it worked, as the GameCube's drive does with a command
   * it does not know (gc_io.c) and for the same reason - but named. */
  w->ios.unknown_commands++;
  if (m->log && w->ios.unknown_commands <= 32) {
    fprintf(m->log, "wii: /dev/di command %02x (%08x %08x) is not implemented, pc %08x\n", command, a1, a2,
            m->cpu.pc);
  }
  return DI_OK;
}

static int32_t di_ioctlv(WiiIosCall *call, uint32_t command, unsigned in_count, unsigned io_count,
                         const WiiIosVector *v) {
  GcMachine *m = call->m;
  WiiState *w = wii_state(m);
  const uint8_t *in = in_count ? wii_ios_buffer(m, &v[0]) : NULL;

  switch (command) {
    case DI_OPEN_PARTITION:
    case DI_OPEN_PARTITION_WITH_TMD_AND_TICKET:
    case DI_OPEN_PARTITION_WITH_TMD_AND_TICKET_VIEW: {
      /* The partition at an offset (in words): IOS checks its ticket and
       * metadata, hands the metadata back, and from then on reads are
       * decrypted. The one partition there is a key for is the game's. */
      const uint64_t offset = (in && v[0].size >= 8u) ? (uint64_t)wii_be32(in + 4) << 2 : 0u;
      uint32_t tmd_size = 0;
      const uint8_t *tmd;
      if (!m->disc) return fail(m, DI_ERROR_NO_DISC);
      if (!m->disc->is_wii || offset != gc_disc_partition_offset(m->disc)) {
        if (m->log) {
          fprintf(m->log, "wii: a game asked to open the partition at 0x%llx, which is not the game's\n",
                  (unsigned long long)offset);
        }
        return DI_SECURITY_ERROR;
      }
      if (gc_disc_is_locked(m->disc)) {
        if (m->log) fprintf(m->log, "wii: %s\n", gc_disc_key_help());
        return DI_SECURITY_ERROR;
      }
      tmd = gc_disc_tmd(m->disc, &tmd_size);
      if (io_count >= 1u && tmd) {
        uint8_t *out = wii_ios_buffer(m, &v[in_count]);
        const uint32_t room = v[in_count].size;
        if (out) memcpy(out, tmd, tmd_size < room ? tmd_size : room);
      }
      if (io_count >= 2u) {
        /* ES's verdict on the ticket: no error. */
        uint8_t *out = wii_ios_buffer(m, &v[in_count + 1u]);
        if (out) memset(out, 0, v[in_count + 1u].size);
      }
      w->di.partition_open = 1;
      w->di.error = DI_ERROR_NONE;
      return DI_OK;
    }
    default: break;
  }
  w->ios.unknown_commands++;
  if (m->log && w->ios.unknown_commands <= 32) {
    fprintf(m->log, "wii: /dev/di vectored command %02x (%u in, %u out) is not implemented, pc %08x\n",
            command, in_count, io_count, m->cpu.pc);
  }
  return DI_BAD_ARGUMENT;
}

static void di_reset(GcMachine *m) { memset(&wii_state(m)->di, 0, sizeof(wii_state(m)->di)); }

const WiiIosDevice wii_ios_di = {
    "/dev/di", 0, NULL, NULL, NULL, NULL, NULL, di_ioctl, di_ioctlv, NULL, di_reset,
};
