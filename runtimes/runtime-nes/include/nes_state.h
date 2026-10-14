#ifndef NES_STATE_H
#define NES_STATE_H

#include <stdbool.h>

#include "nes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Save states (nes_state.c). Loading refuses a state made for another ROM
 * or by another runtime layout and leaves the machine untouched. */
bool nes_state_save_file(NesMachine *m, const char *path);
bool nes_state_load_file(NesMachine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* NES_STATE_H */
