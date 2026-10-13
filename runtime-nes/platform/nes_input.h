/*
 * Input bindings for the NES frontend: the actions a player can bind, two
 * keyboard and two controller bindings per action, the defaults for each
 * host platform, the preference-file format and display labels. Adapted
 * from the Super Nintendo frontend's snes_input.h.
 *
 * Defaults follow the hardware each build targets:
 *   Windows     PC keyboard: F-key shortcuts (F5 save state, F11 fullscreen...)
 *   macOS       Mac keyboard: Command shortcuts, since a Mac laptop's F-keys
 *               are brightness and volume keys unless fn is held
 *   Linux       Steam Deck: the Steam and Quick Access buttons belong to
 *               Steam, so the menu is on L3; save/load state on L4/R4 and
 *               fast forward on R3. A keyboard gets the PC layout.
 * Controller gameplay buttons are the same everywhere. The NES pad has two
 * buttons, B to the left of A; on a four-button diamond they sit where the
 * SNES frontend puts B and Y, the pair a thumb rolls across: south = A,
 * west = B (as Super Mario All-Stars lays the NES games out on an SNES
 * pad). East is a second A and north a second B, so the Nintendo position
 * of A (east) works too.
 */
#ifndef NES_INPUT_H
#define NES_INPUT_H

#include <SDL.h>
#include <stdint.h>

#include <string>

enum NesAction {
    NES_ACTION_UP,
    NES_ACTION_DOWN,
    NES_ACTION_LEFT,
    NES_ACTION_RIGHT,
    NES_ACTION_A,
    NES_ACTION_B,
    NES_ACTION_START,
    NES_ACTION_SELECT,
    NES_ACTION_GAMEPLAY_COUNT, /* actions above press NES buttons; below are shortcuts */
    NES_ACTION_FAST_FORWARD = NES_ACTION_GAMEPLAY_COUNT,
    NES_ACTION_SAVE_STATE,
    NES_ACTION_LOAD_STATE,
    NES_ACTION_PREVIOUS_SLOT,
    NES_ACTION_NEXT_SLOT,
    NES_ACTION_OVERLAY,
    NES_ACTION_FPS,
    NES_ACTION_MUTE,
    NES_ACTION_MENU,
    NES_ACTION_FULLSCREEN,
    NES_ACTION_SCREENSHOT,
    NES_ACTION_COUNT,
};

enum NesBindingKind { NES_BIND_NONE, NES_BIND_KEY, NES_BIND_BUTTON, NES_BIND_AXIS_POSITIVE, NES_BIND_AXIS_NEGATIVE };

/* Modifier keys a keyboard binding needs held (NesBinding::mods). */
enum { NES_MOD_CTRL = 1, NES_MOD_SHIFT = 2, NES_MOD_ALT = 4, NES_MOD_GUI = 8 };

struct NesBinding {
    uint8_t kind; /* NesBindingKind */
    uint8_t mods; /* keys only */
    int16_t code; /* SDL scancode, controller button or axis */
};

inline bool operator==(const NesBinding &a, const NesBinding &b)
{
    return a.kind == b.kind && (a.kind == NES_BIND_NONE || (a.code == b.code && a.mods == b.mods));
}
inline bool operator!=(const NesBinding &a, const NesBinding &b) { return !(a == b); }

enum NesInputPlatform { NES_PLATFORM_WINDOWS, NES_PLATFORM_MAC, NES_PLATFORM_STEAM_DECK };

/* Button names differ by controller family (Xbox A is the south button,
 * Nintendo A the east one). */
enum NesPadProfile { NES_PAD_GENERIC, NES_PAD_XBOX, NES_PAD_PLAYSTATION, NES_PAD_NINTENDO, NES_PAD_STEAM_DECK };

struct NesInputMap {
    NesBinding keyboard[NES_ACTION_COUNT][2];
    NesBinding controller[NES_ACTION_COUNT][2];
};

NesInputPlatform nes_input_host_platform(void);
void nes_input_defaults(NesInputMap &map, NesInputPlatform platform);

const char *nes_action_label(int action);  /* "Save State" */
const char *nes_action_config(int action); /* "save_state", for nes_prefs.ini */
uint8_t nes_action_keypad_bit(int action); /* NES_BTN_* for gameplay actions, else 0 */

/* "key:22", "key:22:8" (with modifiers), "button:3", "axis:5:+", "none". */
std::string nes_binding_to_config(const NesBinding &b);
bool nes_binding_from_config(const char *text, NesBinding &out);

uint8_t nes_mods_from_sdl(unsigned sdl_keymod);
bool nes_scancode_is_modifier(int scancode);
/* Whether a key binding fires with these modifiers held. A binding without
 * modifiers ignores Shift but not Ctrl, Alt or Command, so Command+S (save
 * state on a Mac) does not also press S (down). */
bool nes_key_mods_match(const NesBinding &b, uint8_t held_mods);

NesPadProfile nes_pad_profile(SDL_GameController *pad, NesInputPlatform platform);
std::string nes_binding_label(const NesBinding &b, NesInputPlatform platform, NesPadProfile pad);
/* How to open the menu on this platform, for the menu and overlay. */
const char *nes_menu_hint(NesInputPlatform platform);

#endif /* NES_INPUT_H */
