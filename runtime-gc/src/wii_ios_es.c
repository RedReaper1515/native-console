/*
 * /dev/es: the part of IOS that knows which title is running.
 *
 * On a console this is the title manager and the rights checker - tickets,
 * signatures, installing and launching. A game that came off a disc asks it
 * very little: who am I, where do my saves go, what does my ticket allow,
 * and (through the disc device) is this disc genuine. Those are answered
 * from the disc in the drive; everything to do with installing titles is
 * refused, because there is no system memory here to install them into.
 *
 * The reference for the request numbers and the shapes of their buffers is
 * WiiBrew's /dev/es page and libogc's es.c. A "view" is IOS's name for the
 * part of a ticket or of the title metadata a game is allowed to see: the
 * same fields with the signature and the key left out.
 */

#include <stdio.h>
#include <string.h>

#include "gc_disc.h"
#include "wii.h"
#include "wii_nand.h"

enum {
  ES_GET_DEVICE_ID = 0x07,
  ES_LAUNCH = 0x08,
  ES_GET_OWNED_TITLE_COUNT = 0x0c,
  ES_GET_OWNED_TITLES = 0x0d,
  ES_GET_TITLE_COUNT = 0x0e,
  ES_GET_TITLES = 0x0f,
  ES_GET_TITLE_CONTENTS_COUNT = 0x10,
  ES_GET_TITLE_CONTENTS = 0x11,
  ES_GET_VIEW_COUNT = 0x12,
  ES_GET_VIEWS = 0x13,
  ES_GET_TMD_VIEW_SIZE = 0x14,
  ES_GET_TMD_VIEW = 0x15,
  ES_GET_CONSUMPTION = 0x16,
  ES_DI_GET_TMD_VIEW_SIZE = 0x19,
  ES_DI_GET_TMD_VIEW = 0x1a,
  ES_DI_GET_TICKET_VIEW = 0x1b,
  ES_DI_VERIFY = 0x1c,
  ES_GET_TITLE_DIR = 0x1d,
  ES_GET_DEVICE_CERT = 0x1e,
  ES_GET_TITLE_ID = 0x20,
  ES_SET_UID = 0x21,
  ES_GET_BOOT2_VERSION = 0x2e,
  ES_GET_STORED_TMD_SIZE = 0x34,
  ES_GET_STORED_TMD = 0x35,
  ES_DI_VERIFY_WITH_VIEW = 0x3b,
  ES_CHECK_KOREAN_KEY = 0x45
};

/* ES's own errors. */
#define ES_EINVAL (-1017)
#define ES_NO_TICKET (-1028)

/* Where things are in a ticket and in title metadata (WiiBrew, "Ticket" and
 * "Title metadata"), and how big the views of them are. */
#define TICKET_SIZE 0x2a4u
#define TICKET_VIEW_FROM 0x1d0u
#define TICKET_VIEW_SIZE 0xd8u
#define TICKET_TITLE_ID 0x1dcu
#define TMD_HEADER 0x1e4u
#define TMD_VIEW_FROM 0x180u
#define TMD_SYSTEM_VERSION 0x184u
#define TMD_TITLE_ID 0x18cu
#define TMD_GROUP_ID 0x198u
#define TMD_TITLE_VERSION 0x1dcu
#define TMD_CONTENT_COUNT 0x1deu
#define TMD_CONTENT_SIZE 0x24u
#define TMD_VIEW_HEADER 0x5cu
#define TMD_VIEW_CONTENT 0x10u

/* The disc's ticket and metadata, when there is a Wii disc with both. */
static const uint8_t *disc_ticket(GcMachine *m) {
  uint32_t size = 0;
  const uint8_t *ticket = m->disc ? gc_disc_ticket(m->disc, &size) : NULL;
  return (ticket && size >= TICKET_SIZE) ? ticket : NULL;
}

static const uint8_t *disc_tmd(GcMachine *m, uint32_t *size) {
  const uint8_t *tmd = m->disc ? gc_disc_tmd(m->disc, size) : NULL;
  return (tmd && *size >= TMD_HEADER) ? tmd : NULL;
}

static unsigned tmd_contents(const uint8_t *tmd, uint32_t size) {
  const unsigned count = wii_be16(tmd + TMD_CONTENT_COUNT);
  const unsigned room = (size - TMD_HEADER) / TMD_CONTENT_SIZE;
  return count < room ? count : room;
}

/* The title that is running. A disc's is its ticket's; a program loaded
 * without one is given a disc-title id made from its game code, or from
 * four letters nothing retail uses, so that it has somewhere of its own to
 * save. */
uint64_t wii_es_title(GcMachine *m) {
  WiiState *w = wii_state(m);
  if (!w->title_id) {
    const uint8_t *ticket = disc_ticket(m);
    uint32_t low = 0x4e435748u; /* "NCWH" */
    if (ticket) {
      w->title_id = wii_be64(ticket + TICKET_TITLE_ID);
    } else {
      if (m->disc && m->disc->game_code[0]) {
        low = ((uint32_t)(uint8_t)m->disc->game_code[0] << 24) | ((uint32_t)(uint8_t)m->disc->game_code[1] << 16) |
              ((uint32_t)(uint8_t)m->disc->game_code[2] << 8) | (uint8_t)m->disc->game_code[3];
      }
      w->title_id = ((uint64_t)0x00010000u << 32) | low;
    }
    /* A title that runs has a home in the flash filesystem: the system
     * menu makes /title/<id>/data, owned by the title, the first time a
     * disc is started, and a game saves there without asking whether it
     * exists. Making it again is harmless. */
    wii_nand_add_title(wii_nand_of(m), w->title_id, wii_es_group(m));
  }
  return w->title_id;
}

/* The IOS a disc's title asks for, as the version word a game reads, or 0. */
uint32_t wii_es_disc_ios(GcMachine *m) {
  uint32_t size = 0;
  const uint8_t *tmd = disc_tmd(m, &size);
  uint64_t system;
  if (!tmd) return 0;
  system = wii_be64(tmd + TMD_SYSTEM_VERSION);
  if ((system >> 32) != 1u || !(uint32_t)system || (uint32_t)system > 255u) return 0;
  return ((uint32_t)system << 16) | 0x1820u;
}

/* The maker's two letters, which IOS uses as a title's group. */
uint16_t wii_es_group(GcMachine *m) {
  uint32_t size = 0;
  const uint8_t *tmd = disc_tmd(m, &size);
  if (tmd) return wii_be16(tmd + TMD_GROUP_ID);
  if (m->disc && m->disc->game_code[4]) {
    return (uint16_t)(((uint8_t)m->disc->game_code[4] << 8) | (uint8_t)m->disc->game_code[5]);
  }
  return 0x3031u; /* "01" */
}

static void ticket_view(const uint8_t *ticket, uint8_t *view) {
  memset(view, 0, 4);
  memcpy(view + 4, ticket + TICKET_VIEW_FROM, TICKET_VIEW_SIZE - 4u);
}

static uint32_t tmd_view_size(const uint8_t *tmd, uint32_t size) {
  return TMD_VIEW_HEADER + TMD_VIEW_CONTENT * tmd_contents(tmd, size);
}

static void tmd_view(const uint8_t *tmd, uint32_t size, uint8_t *view) {
  const unsigned count = tmd_contents(tmd, size);
  unsigned i;
  /* Version and revocation bytes, the system version, the title's id and
   * type, and its group: twenty-six bytes that are the same in both. */
  memcpy(view, tmd + TMD_VIEW_FROM, 26);
  /* Sixty-two bytes of region, ratings and access masks, which follow the
   * group in both. */
  memcpy(view + 26, tmd + TMD_VIEW_FROM + 26, 0x3e);
  wii_put16(view + 0x58, wii_be16(tmd + TMD_TITLE_VERSION));
  wii_put16(view + 0x5a, (uint16_t)count);
  /* A content's id, index, type and size - not its hash. */
  for (i = 0; i < count; i++) {
    memcpy(view + TMD_VIEW_HEADER + i * TMD_VIEW_CONTENT, tmd + TMD_HEADER + i * TMD_CONTENT_SIZE, TMD_VIEW_CONTENT);
  }
}

static int32_t es_ioctlv(WiiIosCall *call, uint32_t command, unsigned in_count, unsigned io_count,
                         const WiiIosVector *v) {
  GcMachine *m = call->m;
  uint8_t *in0 = in_count > 0 ? wii_ios_buffer(m, &v[0]) : NULL;
  uint8_t *out0 = io_count > 0 ? wii_ios_buffer(m, &v[in_count]) : NULL;
  const uint32_t out0_size = io_count > 0 ? v[in_count].size : 0;
  const uint64_t title = wii_es_title(m);
  /* Whether a request that names a title means the one that is running. */
  const int ours = in0 && v[0].size >= 8u && wii_be64(in0) == title;
  uint32_t tmd_size = 0;
  const uint8_t *tmd = disc_tmd(m, &tmd_size);
  const uint8_t *ticket = disc_ticket(m);

  switch (command) {
    case ES_GET_TITLE_ID:
      if (!out0 || out0_size < 8u) return ES_EINVAL;
      wii_put64(out0, title);
      return 0;

    case ES_GET_TITLE_DIR: {
      /* Where a title keeps its saves: a path in the flash filesystem,
       * made from the two halves of its id. */
      char path[32];
      uint64_t which;
      if (!in0 || v[0].size < 8u || !out0 || out0_size < 30u) return ES_EINVAL;
      which = wii_be64(in0);
      snprintf(path, sizeof(path), "/title/%08x/%08x/data", (unsigned)(which >> 32), (unsigned)which);
      memset(out0, 0, out0_size);
      memcpy(out0, path, strlen(path));
      return 0;
    }

    case ES_GET_DEVICE_ID:
      /* A console's serial number, as IOS reports it. This is nobody's. */
      if (!out0 || out0_size < 4u) return ES_EINVAL;
      wii_put32(out0, 0x0456f93du);
      return 0;

    case ES_GET_BOOT2_VERSION:
      if (!out0 || out0_size < 4u) return ES_EINVAL;
      wii_put32(out0, 4u);
      return 0;

    case ES_GET_CONSUMPTION:
      /* Limited-play tickets: none. The count is the second buffer. */
      if (io_count >= 2u) {
        uint8_t *count = wii_ios_buffer(m, &v[in_count + 1u]);
        if (count && v[in_count + 1u].size >= 4u) wii_put32(count, 0u);
      }
      return 0;

    /* The tickets a title has: one, for the title that is running off a
     * disc that carries it. */
    case ES_GET_VIEW_COUNT:
      if (!out0 || out0_size < 4u) return ES_EINVAL;
      wii_put32(out0, (ours && ticket) ? 1u : 0u);
      return 0;

    case ES_GET_VIEWS:
      if (!ours || !ticket) return ES_NO_TICKET;
      if (!out0 || out0_size < TICKET_VIEW_SIZE) return ES_EINVAL;
      ticket_view(ticket, out0);
      return 0;

    case ES_DI_GET_TICKET_VIEW: {
      /* The view of a ticket the game hands over, or of the disc's own
       * when it hands over none. */
      const uint8_t *which = (in0 && v[0].size >= TICKET_SIZE) ? in0 : ticket;
      if (!which) return ES_NO_TICKET;
      if (!out0 || out0_size < TICKET_VIEW_SIZE) return ES_EINVAL;
      ticket_view(which, out0);
      return 0;
    }

    case ES_GET_TMD_VIEW_SIZE:
    case ES_GET_STORED_TMD_SIZE:
      if (!ours || !tmd) return ES_EINVAL;
      if (!out0 || out0_size < 4u) return ES_EINVAL;
      wii_put32(out0, command == ES_GET_TMD_VIEW_SIZE ? tmd_view_size(tmd, tmd_size) : tmd_size);
      return 0;

    case ES_GET_TMD_VIEW:
      if (!ours || !tmd) return ES_EINVAL;
      if (!out0 || out0_size < tmd_view_size(tmd, tmd_size)) return ES_EINVAL;
      tmd_view(tmd, tmd_size, out0);
      return 0;

    case ES_GET_STORED_TMD:
      if (!ours || !tmd) return ES_EINVAL;
      if (!out0 || out0_size < tmd_size) return ES_EINVAL;
      memcpy(out0, tmd, tmd_size);
      return 0;

    case ES_DI_GET_TMD_VIEW_SIZE: {
      /* Of the metadata handed over, or of the disc's. */
      const uint8_t *which = (in0 && v[0].size >= TMD_HEADER) ? in0 : tmd;
      const uint32_t size = (in0 && v[0].size >= TMD_HEADER) ? v[0].size : tmd_size;
      if (!which || !out0 || out0_size < 4u) return ES_EINVAL;
      wii_put32(out0, tmd_view_size(which, size));
      return 0;
    }

    case ES_DI_GET_TMD_VIEW: {
      const uint8_t *which = (in0 && v[0].size >= TMD_HEADER) ? in0 : tmd;
      const uint32_t size = (in0 && v[0].size >= TMD_HEADER) ? v[0].size : tmd_size;
      /* The view goes in the last buffer; a size may come before it. */
      uint8_t *out = io_count ? wii_ios_buffer(m, &v[in_count + io_count - 1u]) : NULL;
      if (!which || !out || v[in_count + io_count - 1u].size < tmd_view_size(which, size)) return ES_EINVAL;
      tmd_view(which, size, out);
      return 0;
    }

    case ES_GET_TITLE_CONTENTS_COUNT:
      if (!ours || !tmd || !out0 || out0_size < 4u) return ES_EINVAL;
      wii_put32(out0, tmd_contents(tmd, tmd_size));
      return 0;

    case ES_GET_TITLE_CONTENTS: {
      unsigned i;
      if (!ours || !tmd || !out0) return ES_EINVAL;
      for (i = 0; i < tmd_contents(tmd, tmd_size) && (i + 1u) * 4u <= out0_size; i++) {
        memcpy(out0 + i * 4u, tmd + TMD_HEADER + i * TMD_CONTENT_SIZE, 4);
      }
      return 0;
    }

    /* Titles installed in the console's memory: none. */
    case ES_GET_OWNED_TITLE_COUNT:
    case ES_GET_TITLE_COUNT:
      if (!out0 || out0_size < 4u) return ES_EINVAL;
      wii_put32(out0, 0u);
      return 0;

    case ES_GET_OWNED_TITLES:
    case ES_GET_TITLES: return 0;

    /* The disc is genuine, and the game may be itself. */
    case ES_SET_UID:
    case ES_DI_VERIFY:
    case ES_DI_VERIFY_WITH_VIEW: return 0;

    case ES_GET_DEVICE_CERT:
      if (out0) memset(out0, 0, out0_size);
      return 0;

    case ES_CHECK_KOREAN_KEY: return ES_EINVAL;

    case ES_LAUNCH:
      /* Another title: the system menu, when a game exits to it. There is
       * no menu to go back to, so the game has finished. */
      m->finished = 1;
      return WII_IOS_LATER;

    default: break;
  }

  wii_state(m)->ios.unknown_commands++;
  if (m->log && wii_state(m)->ios.unknown_commands <= 32) {
    fprintf(m->log, "wii: /dev/es request %02x (%u in, %u out) is not implemented, pc %08x\n", command,
            in_count, io_count, m->cpu.pc);
  }
  return ES_EINVAL;
}

const WiiIosDevice wii_ios_es = {
    "/dev/es", 0, NULL, NULL, NULL, NULL, NULL, NULL, es_ioctlv, NULL, NULL,
};
