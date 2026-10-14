/*
 * Input bindings for the GBA frontend: the actions a player can bind, two
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
 * east = A, south = B).
 */
#ifndef GBA_INPUT_H
#define GBA_INPUT_H

#include <SDL.h>
#include <stdint.h>

#include <string>

enum GbaAction {
    GBA_ACTION_UP,
    GBA_ACTION_DOWN,
    GBA_ACTION_LEFT,
    GBA_ACTION_RIGHT,
    GBA_ACTION_A,
    GBA_ACTION_B,
    GBA_ACTION_L,
    GBA_ACTION_R,
    GBA_ACTION_START,
    GBA_ACTION_SELECT,
    GBA_ACTION_GAMEPLAY_COUNT, /* actions above press GBA buttons; below are shortcuts */
    GBA_ACTION_FAST_FORWARD = GBA_ACTION_GAMEPLAY_COUNT,
    GBA_ACTION_SAVE_STATE,
    GBA_ACTION_LOAD_STATE,
    GBA_ACTION_PREVIOUS_SLOT,
    GBA_ACTION_NEXT_SLOT,
    GBA_ACTION_OVERLAY,
    GBA_ACTION_FPS,
    GBA_ACTION_MUTE,
    GBA_ACTION_MENU,
    GBA_ACTION_FULLSCREEN,
    GBA_ACTION_SCREENSHOT,
    GBA_ACTION_COUNT,
};

enum GbaBindingKind { GBA_BIND_NONE, GBA_BIND_KEY, GBA_BIND_BUTTON, GBA_BIND_AXIS_POSITIVE, GBA_BIND_AXIS_NEGATIVE };

/* Modifier keys a keyboard binding needs held (GbaBinding::mods). */
enum { GBA_MOD_CTRL = 1, GBA_MOD_SHIFT = 2, GBA_MOD_ALT = 4, GBA_MOD_GUI = 8 };

struct GbaBinding {
    uint8_t kind; /* GbaBindingKind */
    uint8_t mods; /* keys only */
    int16_t code; /* SDL scancode, controller button or axis */
};

inline bool operator==(const GbaBinding &a, const GbaBinding &b)
{
    return a.kind == b.kind && (a.kind == GBA_BIND_NONE || (a.code == b.code && a.mods == b.mods));
}
inline bool operator!=(const GbaBinding &a, const GbaBinding &b) { return !(a == b); }

enum GbaInputPlatform { GBA_PLATFORM_WINDOWS, GBA_PLATFORM_MAC, GBA_PLATFORM_STEAM_DECK };

/* Button names differ by controller family (Xbox A is the south button,
 * Nintendo A the east one). */
enum GbaPadProfile { GBA_PAD_GENERIC, GBA_PAD_XBOX, GBA_PAD_PLAYSTATION, GBA_PAD_NINTENDO, GBA_PAD_STEAM_DECK };

struct GbaInputMap {
    GbaBinding keyboard[GBA_ACTION_COUNT][2];
    GbaBinding controller[GBA_ACTION_COUNT][2];
};

GbaInputPlatform gba_input_host_platform(void);
void gba_input_defaults(GbaInputMap &map, GbaInputPlatform platform);

const char *gba_action_label(int action);  /* "Save State" */
const char *gba_action_config(int action); /* "save_state", for gba_prefs.ini */
uint16_t gba_action_keypad_bit(int action); /* GBA_KEY_* for gameplay actions, else 0 */

/* "key:22", "key:22:8" (with modifiers), "button:3", "axis:5:+", "none". */
std::string gba_binding_to_config(const GbaBinding &b);
bool gba_binding_from_config(const char *text, GbaBinding &out);

uint8_t gba_mods_from_sdl(unsigned sdl_keymod);
bool gba_scancode_is_modifier(int scancode);
/* Whether a key binding fires with these modifiers held. A binding without
 * modifiers ignores Shift but not Ctrl, Alt or Command, so Command+S (save
 * state on a Mac) does not also press S (down). */
bool gba_key_mods_match(const GbaBinding &b, uint8_t held_mods);

GbaPadProfile gba_pad_profile(SDL_GameController *pad, GbaInputPlatform platform);
std::string gba_binding_label(const GbaBinding &b, GbaInputPlatform platform, GbaPadProfile pad);
/* How to open the menu on this platform, for the menu and overlay. */
const char *gba_menu_hint(GbaInputPlatform platform);

#endif /* GBA_INPUT_H */
