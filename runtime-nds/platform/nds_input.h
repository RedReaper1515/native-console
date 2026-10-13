/*
 * Input for the DS frontend, kept apart from the window so that it can be
 * tested on its own (tests/nds/test_nds_input.cpp).
 *
 * Bindings: the actions a player can bind, two keyboard and two controller
 * bindings per action, the defaults for each host platform, the
 * preference-file format and display labels. As on GBA, plus what the DS
 * adds: X and Y, the stylus cursor a controller drives, the microphone, the
 * lid, and shortcuts for the screen layout.
 *
 * Defaults follow the hardware each build targets:
 *   Windows     PC keyboard: F-key shortcuts (F2 next layout, F11 fullscreen...)
 *   macOS       Mac keyboard: Command shortcuts, since a Mac laptop's F-keys
 *               are brightness and volume keys unless fn is held
 *   Linux       Steam Deck: the Steam and Quick Access buttons belong to
 *               Steam, so the menu is on L3. A keyboard gets the PC layout.
 * Controller buttons are the same everywhere (Nintendo positions: east = A,
 * south = B, north = X, west = Y). The right stick moves the stylus cursor,
 * the right trigger presses it, the left trigger blows into the microphone
 * and R3 swaps the screens.
 *
 * Screens: where the DS's two screens go on a display of any size in each
 * layout, and which pixel of the bottom screen a position on the display
 * touches. The mouse, a touch screen and the controller's cursor all go
 * through nds_layout_touch().
 */
#ifndef NDS_INPUT_H
#define NDS_INPUT_H

#include <SDL.h>
#include <stdint.h>

#include <string>

enum NdsAction {
    NDS_ACTION_UP,
    NDS_ACTION_DOWN,
    NDS_ACTION_LEFT,
    NDS_ACTION_RIGHT,
    NDS_ACTION_A,
    NDS_ACTION_B,
    NDS_ACTION_X,
    NDS_ACTION_Y,
    NDS_ACTION_L,
    NDS_ACTION_R,
    NDS_ACTION_START,
    NDS_ACTION_SELECT,
    NDS_ACTION_GAMEPLAY_COUNT, /* actions above press DS buttons */
    /* The rest of the console: the stylus as a controller drives it (a
     * cursor on the bottom screen and the button that puts the pen down),
     * the microphone and the lid. Held, like the buttons. */
    NDS_ACTION_TOUCH = NDS_ACTION_GAMEPLAY_COUNT,
    NDS_ACTION_CURSOR_UP,
    NDS_ACTION_CURSOR_DOWN,
    NDS_ACTION_CURSOR_LEFT,
    NDS_ACTION_CURSOR_RIGHT,
    NDS_ACTION_BLOW,
    NDS_ACTION_LID,
    NDS_ACTION_CONSOLE_COUNT, /* actions above reach the game; below are shortcuts */
    NDS_ACTION_FAST_FORWARD = NDS_ACTION_CONSOLE_COUNT,
    NDS_ACTION_NEXT_LAYOUT,
    NDS_ACTION_SWAP_SCREENS,
    NDS_ACTION_OVERLAY,
    NDS_ACTION_FPS,
    NDS_ACTION_MUTE,
    NDS_ACTION_MENU,
    NDS_ACTION_FULLSCREEN,
    NDS_ACTION_SCREENSHOT,
    NDS_ACTION_COUNT,
};

enum NdsBindingKind { NDS_BIND_NONE, NDS_BIND_KEY, NDS_BIND_BUTTON, NDS_BIND_AXIS_POSITIVE, NDS_BIND_AXIS_NEGATIVE };

/* Modifier keys a keyboard binding needs held (NdsBinding::mods). */
enum { NDS_MOD_CTRL = 1, NDS_MOD_SHIFT = 2, NDS_MOD_ALT = 4, NDS_MOD_GUI = 8 };

struct NdsBinding {
    uint8_t kind; /* NdsBindingKind */
    uint8_t mods; /* keys only */
    int16_t code; /* SDL scancode, controller button or axis */
};

inline bool operator==(const NdsBinding &a, const NdsBinding &b)
{
    return a.kind == b.kind && (a.kind == NDS_BIND_NONE || (a.code == b.code && a.mods == b.mods));
}
inline bool operator!=(const NdsBinding &a, const NdsBinding &b) { return !(a == b); }

enum NdsInputPlatform { NDS_PLATFORM_WINDOWS, NDS_PLATFORM_MAC, NDS_PLATFORM_STEAM_DECK };

/* Button names differ by controller family (Xbox A is the south button,
 * Nintendo A the east one). */
enum NdsPadProfile { NDS_PAD_GENERIC, NDS_PAD_XBOX, NDS_PAD_PLAYSTATION, NDS_PAD_NINTENDO, NDS_PAD_STEAM_DECK };

struct NdsInputMap {
    NdsBinding keyboard[NDS_ACTION_COUNT][2];
    NdsBinding controller[NDS_ACTION_COUNT][2];
};

NdsInputPlatform nds_input_host_platform(void);
void nds_input_defaults(NdsInputMap &map, NdsInputPlatform platform);

const char *nds_action_label(int action);  /* "Close Lid (Hold)" */
const char *nds_action_config(int action); /* "lid", for nds_prefs.ini */
uint32_t nds_action_key_bit(int action);   /* NDS_KEY_* for gameplay actions, else 0 */

/* "key:22", "key:22:8" (with modifiers), "button:3", "axis:5:+", "none". */
std::string nds_binding_to_config(const NdsBinding &b);
bool nds_binding_from_config(const char *text, NdsBinding &out);

uint8_t nds_mods_from_sdl(unsigned sdl_keymod);
bool nds_scancode_is_modifier(int scancode);
/* Whether a key binding fires with these modifiers held. A binding without
 * modifiers ignores Shift but not Ctrl, Alt or Command, so Command+D (next
 * layout on a Mac) does not also press D (right). */
bool nds_key_mods_match(const NdsBinding &b, uint8_t held_mods);

NdsPadProfile nds_pad_profile(SDL_GameController *pad, NdsInputPlatform platform);
std::string nds_binding_label(const NdsBinding &b, NdsInputPlatform platform, NdsPadProfile pad);
/* How to open the menu on this platform, for the menu and overlay. */
const char *nds_menu_hint(NdsInputPlatform platform);

/* ------------------------------------------------------------------ */
/* Screens                                                             */
/* ------------------------------------------------------------------ */

/*
 * The four ways to show two screens. Each has a usual arrangement and a
 * swapped one (the swap shortcut, kept per layout):
 *
 *   Stacked        top screen above the bottom one, as on the console;
 *                  swapped, the bottom screen is above.
 *   Side by side   top screen left, bottom screen right; swapped, the
 *                  other way round. The one for an ultrawide display.
 *   Large + small  the bottom (touch) screen as large as the display
 *                  allows and the top screen beside it in what is left, at
 *                  least half its scale; swapped, the top screen is the
 *                  large one. On a display taller than it is wide the small
 *                  screen goes above or below instead.
 *   Single         the bottom (touch) screen alone; swapped, the top one.
 */
enum NdsLayout { NDS_LAYOUT_STACKED, NDS_LAYOUT_SIDE_BY_SIDE, NDS_LAYOUT_LARGE_SMALL, NDS_LAYOUT_SINGLE, NDS_LAYOUT_COUNT };

/* Pixel Perfect scales by whole numbers only, so every DS pixel is the same
 * size; Fit uses all the room there is and keeps the screens' shape. */
enum NdsScaling { NDS_SCALING_PIXEL_PERFECT, NDS_SCALING_FIT, NDS_SCALING_COUNT };

/* The gap between the screens is in DS pixels, and is scaled with them
 * (with the small screen in Large + small). The console's own, across its
 * hinge, is about 90. */
enum { NDS_GAP_MAX = 128 };

struct NdsScreens {
    SDL_Rect rect[2]; /* [0] top, [1] bottom, in display pixels; w = 0: not shown */
};

const char *nds_layout_label(int layout);  /* "Side by Side" */
const char *nds_layout_config(int layout); /* "side", for --layout and nds_prefs.ini */
int nds_layout_from_config(const char *name); /* -1 if there is no such layout */
const char *nds_scaling_label(int scaling);

/* The size of a layout with its screens at whole scale `scale` (the small
 * screen, in Large + small; the large one is twice that): what the window
 * is made, and at scale 1 the smallest it may become. */
void nds_layout_size(int layout, int gap, int scale, int *w, int *h);

/* Where the screens go on a w x h display. */
NdsScreens nds_layout_screens(int layout, bool swapped, int gap, int scaling, int w, int h);

/*
 * The bottom-screen pixel under a display position, for a pen put down
 * there. Returns whether the position is on the bottom screen. Either way
 * x and y are a pixel of it: positions outside are brought to the nearest
 * edge, which is what a pen dragged off the screen needs. Every display
 * pixel of the screen's rectangle belongs to a DS pixel, so the rectangle
 * has no dead border. False with (0, 0) when the layout does not show the
 * bottom screen.
 */
bool nds_layout_touch(const NdsScreens &screens, double px, double py, int *x, int *y);

/* The other way: the display position of a point on the bottom screen, in
 * bottom-screen pixels with fractions (x + 0.5 is the middle of pixel x),
 * for drawing the controller's cursor. False when the bottom screen is not
 * shown. */
bool nds_layout_touch_position(const NdsScreens &screens, double x, double y, double *px, double *py);

#endif /* NDS_INPUT_H */
