#ifndef SNES_STATE_H
#define SNES_STATE_H

#include <stdbool.h>

#include "snes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Save states (snes_state.c). Loading refuses a state made for another ROM
 * or by another runtime layout and leaves the machine untouched. */
bool snes_state_save_file(SnesMachine *m, const char *path);
bool snes_state_load_file(SnesMachine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SNES_STATE_H */
