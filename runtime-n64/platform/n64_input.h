/*
 * Input bindings for the Nintendo 64 frontend: the actions a player can
 * bind, two keyboard and two controller bindings per action, the defaults
 * for each host platform, the preference-file format and display labels.
 * Adapted from the SNES frontend (runtime-snes/platform/snes_input.h).
 *
 * The N64 controller has an analog stick as well as buttons. The stick is
 * four actions (Stick Up, Down, Left, Right) so it can be bound like
 * anything else: a key or button bound to a direction pushes the stick all
 * the way (80, about what a real controller reaches), and a controller axis
 * bound to one moves it in proportion, past a dead zone, so a full SDL
 * deflection is 80 as well (n64_stick_from_axis).
 *
 * Keyboard (every platform):
 *   arrows = stick, X = A, C = B, Z = Z, Enter = Start, A / S = L / R,
 *   I / K / J / L = C-up / C-down / C-left / C-right,
 *   T / G / F / H = D-pad up / down / left / right,
 *   Tab = fast forward (hold), M = mute.
 * Shortcuts follow the host keyboard:
 *   Windows     F-keys: F1 overlay, F10 menu, F11 fullscreen, F12 screenshot
 *   macOS       Command: Cmd+I, Cmd+, (menu), Cmd+F, Cmd+P, since a Mac
 *               laptop's F-keys are brightness and volume keys unless fn is
 *               held
 *   Linux       Steam Deck: Steam keeps the Steam and Quick Access buttons,
 *               so the menu is on L3 and fast forward on R3. A keyboard gets
 *               the PC layout.
 *
 * Controller (an SDL GameController, positions rather than letters, the
 * same everywhere): left stick = stick, D-pad = D-pad, south = A, west = B,
 * left trigger = Z, left shoulder = L, right shoulder or right trigger = R,
 * Start = Start, right stick = the four C buttons. The two free face
 * buttons are the C buttons games use most as extra action buttons (items
 * in the Zelda games, for one): north = C-left, east = C-down. L3 opens the
 * menu. On the N64 pad, B sits up and left of A with the C buttons up and
 * right, which these positions follow.
 */
#ifndef N64_INPUT_H
#define N64_INPUT_H

#include <SDL.h>
#include <stdint.h>

#include <string>

enum N64Action {
    N64_ACTION_STICK_UP,
    N64_ACTION_STICK_DOWN,
    N64_ACTION_STICK_LEFT,
    N64_ACTION_STICK_RIGHT,
    N64_ACTION_A,
    N64_ACTION_B,
    N64_ACTION_Z,
    N64_ACTION_START,
    N64_ACTION_L,
    N64_ACTION_R,
    N64_ACTION_DPAD_UP,
    N64_ACTION_DPAD_DOWN,
    N64_ACTION_DPAD_LEFT,
    N64_ACTION_DPAD_RIGHT,
    N64_ACTION_C_UP,
    N64_ACTION_C_DOWN,
    N64_ACTION_C_LEFT,
    N64_ACTION_C_RIGHT,
    N64_ACTION_GAMEPLAY_COUNT, /* actions above reach the controller; below are shortcuts */
    N64_ACTION_FAST_FORWARD = N64_ACTION_GAMEPLAY_COUNT,
    N64_ACTION_OVERLAY,
    N64_ACTION_FPS,
    N64_ACTION_MUTE,
    N64_ACTION_MENU,
    N64_ACTION_FULLSCREEN,
    N64_ACTION_SCREENSHOT,
    N64_ACTION_COUNT,
};

enum N64BindingKind { N64_BIND_NONE, N64_BIND_KEY, N64_BIND_BUTTON, N64_BIND_AXIS_POSITIVE, N64_BIND_AXIS_NEGATIVE };

/* Modifier keys a keyboard binding needs held (N64Binding::mods). */
enum { N64_MOD_CTRL = 1, N64_MOD_SHIFT = 2, N64_MOD_ALT = 4, N64_MOD_GUI = 8 };

/* The stick: full deflection, and the dead zone of a host controller's axis
 * (about XInput's recommended one for a thumbstick). */
enum { N64_STICK_MAX = 80, N64_STICK_DEADZONE = 7849 };

struct N64Binding {
    uint8_t kind; /* N64BindingKind */
    uint8_t mods; /* keys only */
    int16_t code; /* SDL scancode, controller button or axis */
};

inline bool operator==(const N64Binding &a, const N64Binding &b)
{
    return a.kind == b.kind && (a.kind == N64_BIND_NONE || (a.code == b.code && a.mods == b.mods));
}
inline bool operator!=(const N64Binding &a, const N64Binding &b) { return !(a == b); }

enum N64InputPlatform { N64_PLATFORM_WINDOWS, N64_PLATFORM_MAC, N64_PLATFORM_STEAM_DECK };

/* Button names differ by controller family (Xbox A is the south button,
 * Nintendo A the east one). */
enum N64PadProfile { N64_PAD_GENERIC, N64_PAD_XBOX, N64_PAD_PLAYSTATION, N64_PAD_NINTENDO, N64_PAD_STEAM_DECK };

struct N64InputMap {
    N64Binding keyboard[N64_ACTION_COUNT][2];
    N64Binding controller[N64_ACTION_COUNT][2];
};

N64InputPlatform n64_input_host_platform(void);
void n64_input_defaults(N64InputMap &map, N64InputPlatform platform);

const char *n64_action_label(int action);  /* "C-Up" */
const char *n64_action_config(int action); /* "c_up", for n64_prefs.ini */
/* N64_BTN_* for a button action; 0 for the stick's directions and shortcuts. */
uint16_t n64_action_button(int action);
/* True for the four stick directions. */
bool n64_action_is_stick(int action);

/* How far (0 to N64_STICK_MAX) a host axis pushed `magnitude` (0 to 32768)
 * in a direction moves the N64 stick: nothing inside the dead zone, then in
 * proportion up to full deflection. */
int n64_stick_from_axis(int magnitude);

/* "key:22", "key:22:8" (with modifiers), "button:3", "axis:5:+", "none". */
std::string n64_binding_to_config(const N64Binding &b);
bool n64_binding_from_config(const char *text, N64Binding &out);

uint8_t n64_mods_from_sdl(unsigned sdl_keymod);
bool n64_scancode_is_modifier(int scancode);
/* Whether a key binding fires with these modifiers held. A binding without
 * modifiers ignores Shift but not Ctrl, Alt or Command, so Command+F
 * (fullscreen on a Mac) does not also press F (D-pad left). */
bool n64_key_mods_match(const N64Binding &b, uint8_t held_mods);

N64PadProfile n64_pad_profile(SDL_GameController *pad, N64InputPlatform platform);
std::string n64_binding_label(const N64Binding &b, N64InputPlatform platform, N64PadProfile pad);
/* How to open the menu on this platform, for the menu and overlay. */
const char *n64_menu_hint(N64InputPlatform platform);

#endif /* N64_INPUT_H */
