/*
 * Input bindings for the GameCube frontend: the actions a player can bind,
 * two keyboard and two controller bindings for each, the defaults for each
 * host platform, the preference-file format and display labels. Adapted from
 * the SNES frontend's snes_input.h.
 *
 * The GameCube controller is the first one in this project that is mostly
 * *analogue*: two sticks and two pressure-sensitive triggers, and a game
 * reads how far they are pushed rather than whether they are. That splits
 * this file's job in two.
 *
 * The buttons are bound the way every other console's are - a table of
 * actions, each with two keyboard and two controller bindings, all
 * rebindable.
 *
 * The sticks and triggers are not in that table, because binding an axis to
 * an axis is not a choice anybody wants to make: a host controller's left
 * stick is the GameCube's left stick, its right stick the C-stick, its
 * triggers the triggers. What the table does carry is the *keyboard's* way
 * of pushing them, because a key can only be down or up and a stick pushed
 * by a key has to go somewhere - full deflection, in the direction the key
 * names. See GC_ACTION_STICK_UP and the three after it.
 *
 * Defaults follow the hardware each build targets:
 *   Windows     PC keyboard: F-key shortcuts (F5 save state, F8 load state,
 *               F6/F7 state slot, F11 fullscreen...)
 *   macOS       Mac keyboard: Command shortcuts, since a Mac laptop's F-keys
 *               are brightness and volume keys unless fn is held
 *   Linux       Steam Deck: the Steam and Quick Access buttons belong to
 *               Steam, so the menu is on L3.
 */
#ifndef GC_INPUT_H
#define GC_INPUT_H

#include <SDL.h>
#include <stdint.h>

#include <string>

enum GcAction {
    /* The digital buttons, in the order the controller reports them. */
    GC_ACTION_UP,
    GC_ACTION_DOWN,
    GC_ACTION_LEFT,
    GC_ACTION_RIGHT,
    GC_ACTION_A,
    GC_ACTION_B,
    GC_ACTION_X,
    GC_ACTION_Y,
    GC_ACTION_Z,
    GC_ACTION_L,
    GC_ACTION_R,
    GC_ACTION_START,
    /* The analogue parts, as a keyboard can reach them: a key pushes the
     * stick all the way. A controller's own sticks bypass all of this. */
    GC_ACTION_STICK_UP,
    GC_ACTION_STICK_DOWN,
    GC_ACTION_STICK_LEFT,
    GC_ACTION_STICK_RIGHT,
    GC_ACTION_C_UP,
    GC_ACTION_C_DOWN,
    GC_ACTION_C_LEFT,
    GC_ACTION_C_RIGHT,
    GC_ACTION_GAMEPLAY_COUNT, /* above this line the controller moves; below, the frontend */
    GC_ACTION_FAST_FORWARD = GC_ACTION_GAMEPLAY_COUNT,
    GC_ACTION_OVERLAY,
    GC_ACTION_MUTE,
    GC_ACTION_MENU,
    GC_ACTION_FULLSCREEN,
    GC_ACTION_SCREENSHOT,
    /* Save states, in slots 1-10 (added after the rest, so that nothing
     * above moves). */
    GC_ACTION_SAVE_STATE,
    GC_ACTION_LOAD_STATE,
    GC_ACTION_PREVIOUS_SLOT,
    GC_ACTION_NEXT_SLOT,
    GC_ACTION_FPS,
    GC_ACTION_SHORTCUT_COUNT, /* the frontend's own end here */
    /* A Wii Remote's controls that a GameCube controller has no counterpart
     * for. Held, like the gameplay actions, and shown only for a Wii game.
     * The rest of the remote is the actions above under another name (see
     * build_remote in gc_sdl.cpp). */
    GC_ACTION_WII_MINUS = GC_ACTION_SHORTCUT_COUNT,
    GC_ACTION_WII_HOME,
    GC_ACTION_WII_SHAKE,
    GC_ACTION_COUNT,
};

enum GcBindingKind { GC_BIND_NONE, GC_BIND_KEY, GC_BIND_BUTTON, GC_BIND_AXIS_POSITIVE, GC_BIND_AXIS_NEGATIVE };

/* Modifier keys a keyboard binding needs held (GcBinding::mods). */
enum { GC_MOD_CTRL = 1, GC_MOD_SHIFT = 2, GC_MOD_ALT = 4, GC_MOD_GUI = 8 };

struct GcBinding {
    uint8_t kind; /* GcBindingKind */
    uint8_t mods; /* keys only */
    int16_t code; /* SDL scancode, controller button or axis */
};

inline bool operator==(const GcBinding &a, const GcBinding &b)
{
    return a.kind == b.kind && (a.kind == GC_BIND_NONE || (a.code == b.code && a.mods == b.mods));
}
inline bool operator!=(const GcBinding &a, const GcBinding &b) { return !(a == b); }

enum GcInputPlatform { GC_PLATFORM_WINDOWS, GC_PLATFORM_MAC, GC_PLATFORM_STEAM_DECK };

/* Button names differ by controller family (Xbox A is the south button,
 * Nintendo A the east one). */
enum GcPadProfile { GC_PAD_GENERIC, GC_PAD_XBOX, GC_PAD_PLAYSTATION, GC_PAD_NINTENDO, GC_PAD_STEAM_DECK };

struct GcInputMap {
    GcBinding keyboard[GC_ACTION_COUNT][2];
    GcBinding controller[GC_ACTION_COUNT][2];
};

GcInputPlatform gc_input_host_platform(void);
void gc_input_defaults(GcInputMap &map, GcInputPlatform platform);

const char *gc_action_label(int action);  /* "Start" */
const char *gc_action_config(int action); /* "start", for gc_prefs.ini */
/* The GC_PAD_* bit a button action presses, or zero for everything else -
 * including the four stick directions, which move an axis rather than press
 * a button. */
uint16_t gc_action_pad_bit(int action);

/* "key:22", "key:22:8" (with modifiers), "button:3", "axis:5:+", "none". */
std::string gc_binding_to_config(const GcBinding &b);
bool gc_binding_from_config(const char *text, GcBinding &out);

uint8_t gc_mods_from_sdl(unsigned sdl_keymod);
bool gc_scancode_is_modifier(int scancode);
/* Whether a key binding fires with these modifiers held. A binding without
 * modifiers ignores Shift but not Ctrl, Alt or Command, so Command+S does
 * not also press S. */
bool gc_key_mods_match(const GcBinding &b, uint8_t held_mods);

GcPadProfile gc_pad_profile(SDL_GameController *pad, GcInputPlatform platform);
std::string gc_binding_label(const GcBinding &b, GcInputPlatform platform, GcPadProfile pad);
/* How to open the menu on this platform, for the menu and the overlay. */
const char *gc_menu_hint(GcInputPlatform platform);

#endif /* GC_INPUT_H */
