/*
 * Input bindings for the SNES frontend: the actions a player can bind, two
 * keyboard and two controller bindings per action, the defaults for each
 * host platform, the preference-file format and display labels.
 *
 * Defaults follow the hardware each build targets:
 *   Windows     PC keyboard: F-key shortcuts (F5 save state, F11 fullscreen...)
 *   macOS       Mac keyboard: Command shortcuts, since a Mac laptop's F-keys
 *               are brightness and volume keys unless fn is held
 *   Linux       Steam Deck: the Steam and Quick Access buttons belong to
 *               Steam, so the menu is on L3; save/load state on X/Y and fast
 *               forward on R3. A keyboard gets the PC layout.
 * Controller gameplay buttons are the same everywhere (Nintendo positions:
 * east = A, south = B, north = X, west = Y).
 */
#ifndef SNES_INPUT_H
#define SNES_INPUT_H

#include <SDL.h>
#include <stdint.h>

#include <string>

enum SnesAction {
    SNES_ACTION_UP,
    SNES_ACTION_DOWN,
    SNES_ACTION_LEFT,
    SNES_ACTION_RIGHT,
    SNES_ACTION_A,
    SNES_ACTION_B,
    SNES_ACTION_X,
    SNES_ACTION_Y,
    SNES_ACTION_L,
    SNES_ACTION_R,
    SNES_ACTION_START,
    SNES_ACTION_SELECT,
    SNES_ACTION_GAMEPLAY_COUNT, /* actions above press SNES buttons; below are shortcuts */
    SNES_ACTION_FAST_FORWARD = SNES_ACTION_GAMEPLAY_COUNT,
    SNES_ACTION_SAVE_STATE,
    SNES_ACTION_LOAD_STATE,
    SNES_ACTION_PREVIOUS_SLOT,
    SNES_ACTION_NEXT_SLOT,
    SNES_ACTION_OVERLAY,
    SNES_ACTION_FPS,
    SNES_ACTION_MUTE,
    SNES_ACTION_MENU,
    SNES_ACTION_FULLSCREEN,
    SNES_ACTION_SCREENSHOT,
    SNES_ACTION_COUNT,
};

enum SnesBindingKind { SNES_BIND_NONE, SNES_BIND_KEY, SNES_BIND_BUTTON, SNES_BIND_AXIS_POSITIVE, SNES_BIND_AXIS_NEGATIVE };

/* Modifier keys a keyboard binding needs held (SnesBinding::mods). */
enum { SNES_MOD_CTRL = 1, SNES_MOD_SHIFT = 2, SNES_MOD_ALT = 4, SNES_MOD_GUI = 8 };

struct SnesBinding {
    uint8_t kind; /* SnesBindingKind */
    uint8_t mods; /* keys only */
    int16_t code; /* SDL scancode, controller button or axis */
};

inline bool operator==(const SnesBinding &a, const SnesBinding &b)
{
    return a.kind == b.kind && (a.kind == SNES_BIND_NONE || (a.code == b.code && a.mods == b.mods));
}
inline bool operator!=(const SnesBinding &a, const SnesBinding &b) { return !(a == b); }

enum SnesInputPlatform { SNES_PLATFORM_WINDOWS, SNES_PLATFORM_MAC, SNES_PLATFORM_STEAM_DECK };

/* Button names differ by controller family (Xbox A is the south button,
 * Nintendo A the east one). */
enum SnesPadProfile { SNES_PAD_GENERIC, SNES_PAD_XBOX, SNES_PAD_PLAYSTATION, SNES_PAD_NINTENDO, SNES_PAD_STEAM_DECK };

struct SnesInputMap {
    SnesBinding keyboard[SNES_ACTION_COUNT][2];
    SnesBinding controller[SNES_ACTION_COUNT][2];
};

SnesInputPlatform snes_input_host_platform(void);
void snes_input_defaults(SnesInputMap &map, SnesInputPlatform platform);

const char *snes_action_label(int action);  /* "Save State" */
const char *snes_action_config(int action); /* "save_state", for snes_prefs.ini */
uint16_t snes_action_keypad_bit(int action); /* SNES_BTN_* for gameplay actions, else 0 */

/* "key:22", "key:22:8" (with modifiers), "button:3", "axis:5:+", "none". */
std::string snes_binding_to_config(const SnesBinding &b);
bool snes_binding_from_config(const char *text, SnesBinding &out);

uint8_t snes_mods_from_sdl(unsigned sdl_keymod);
bool snes_scancode_is_modifier(int scancode);
/* Whether a key binding fires with these modifiers held. A binding without
 * modifiers ignores Shift but not Ctrl, Alt or Command, so Command+S (save
 * state on a Mac) does not also press S (down). */
bool snes_key_mods_match(const SnesBinding &b, uint8_t held_mods);

SnesPadProfile snes_pad_profile(SDL_GameController *pad, SnesInputPlatform platform);
std::string snes_binding_label(const SnesBinding &b, SnesInputPlatform platform, SnesPadProfile pad);
/* How to open the menu on this platform, for the menu and overlay. */
const char *snes_menu_hint(SnesInputPlatform platform);

#endif /* SNES_INPUT_H */
