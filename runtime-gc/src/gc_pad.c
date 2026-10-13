/*
 * The controllers, as the machine holds them. See gc_pad.h for what a
 * caller is expected to put in, and gc_io.c for how it becomes the bytes a
 * game reads back.
 */
#include "gc_pad.h"

#include <stdlib.h>
#include <string.h>

#include "gc.h"

void gc_pad_set(GcMachine *m, unsigned port, const GcPad *pad) {
  if (!m || port >= GC_PAD_PORTS) return;
  if (!m->pads) {
    /* Allocated the first time somebody plugs something in, so a headless
     * run that never mentions controllers carries nothing. */
    m->pads = (GcPad *)calloc(GC_PAD_PORTS, sizeof(GcPad));
    if (!m->pads) return;
  }
  if (pad) {
    m->pads[port] = *pad;
  } else {
    memset(&m->pads[port], 0, sizeof(m->pads[port]));
  }
}

/* ------------------------------------------------------------ save states */

#include "gc_state.h"

/* Field by field, because a GcPad has padding and a frontend fills one on
 * the stack. */
#define GC_PAD_STATE_BYTES 12u

void gc_pad_state_save(GcMachine *m, GcStateBuffer *b) {
  unsigned port;
  gc_state_put_u32(b, m->pads ? 1u : 0u);
  if (!m->pads) return;
  for (port = 0; port < GC_PAD_PORTS; port++) {
    const GcPad *pad = &m->pads[port];
    uint8_t bytes[GC_PAD_STATE_BYTES];
    const uint32_t present = pad->present ? 1u : 0u;
    memcpy(bytes, &present, 4u);
    bytes[4] = (uint8_t)pad->buttons;
    bytes[5] = (uint8_t)(pad->buttons >> 8);
    bytes[6] = (uint8_t)pad->stick_x;
    bytes[7] = (uint8_t)pad->stick_y;
    bytes[8] = (uint8_t)pad->c_stick_x;
    bytes[9] = (uint8_t)pad->c_stick_y;
    bytes[10] = pad->trigger_l;
    bytes[11] = pad->trigger_r;
    gc_state_put(b, bytes, sizeof(bytes));
  }
}

int gc_pad_state_load(GcMachine *m, const uint8_t *payload, size_t size, int apply) {
  uint32_t allocated;
  unsigned port;
  if (size < 4u) return -1;
  memcpy(&allocated, payload, 4u);
  if (allocated > 1u || size != 4u + (allocated ? GC_PAD_PORTS * GC_PAD_STATE_BYTES : 0u)) return -1;
  if (!apply) return 0;
  if (!allocated) {
    /* No controller was ever plugged in, which reads the same as four
     * empty ports. */
    free(m->pads);
    m->pads = NULL;
    return 0;
  }
  if (!m->pads) m->pads = (GcPad *)calloc(GC_PAD_PORTS, sizeof(GcPad));
  if (!m->pads) return -1;
  for (port = 0; port < GC_PAD_PORTS; port++) {
    const uint8_t *bytes = payload + 4u + port * GC_PAD_STATE_BYTES;
    GcPad *pad = &m->pads[port];
    uint32_t present;
    memset(pad, 0, sizeof(*pad));
    memcpy(&present, bytes, 4u);
    pad->present = present != 0;
    pad->buttons = (uint16_t)(bytes[4] | (bytes[5] << 8));
    pad->stick_x = (int8_t)bytes[6];
    pad->stick_y = (int8_t)bytes[7];
    pad->c_stick_x = (int8_t)bytes[8];
    pad->c_stick_y = (int8_t)bytes[9];
    pad->trigger_l = bytes[10];
    pad->trigger_r = bytes[11];
  }
  return 0;
}
