/* Input bindings and screen layouts for the DS frontend (see nds_input.h). */
#include "nds_input.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nds.h"

static const struct {
    const char *label;
    const char *config;
    uint32_t key;
} k_actions[NDS_ACTION_COUNT] = {
    { "Up", "up", NDS_KEY_UP },
    { "Down", "down", NDS_KEY_DOWN },
    { "Left", "left", NDS_KEY_LEFT },
    { "Right", "right", NDS_KEY_RIGHT },
    { "A", "a", NDS_KEY_A },
    { "B", "b", NDS_KEY_B },
    { "X", "x", NDS_KEY_X },
    { "Y", "y", NDS_KEY_Y },
    { "L", "l", NDS_KEY_L },
    { "R", "r", NDS_KEY_R },
    { "Start", "start", NDS_KEY_START },
    { "Select", "select", NDS_KEY_SELECT },
    { "Touch (Pen Down at Cursor)", "touch", 0 },
    { "Cursor Up", "cursor_up", 0 },
    { "Cursor Down", "cursor_down", 0 },
    { "Cursor Left", "cursor_left", 0 },
    { "Cursor Right", "cursor_right", 0 },
    { "Blow / Shout (Hold)", "blow", 0 },
    { "Close Lid (Hold)", "lid", 0 },
    { "Fast Forward (Hold)", "fast_forward", 0 },
    { "Next Screen Layout", "next_layout", 0 },
    { "Swap Screens", "swap_screens", 0 },
    { "Toggle Overlay", "toggle_overlay", 0 },
    { "Toggle FPS Counter", "toggle_fps", 0 },
    { "Toggle Mute", "toggle_mute", 0 },
    { "Toggle Menu", "toggle_menu", 0 },
    { "Toggle Fullscreen", "toggle_fullscreen", 0 },
    { "Screenshot", "screenshot", 0 },
};

const char *nds_action_label(int action) { return k_actions[action].label; }
const char *nds_action_config(int action) { return k_actions[action].config; }
uint32_t nds_action_key_bit(int action) { return action < NDS_ACTION_GAMEPLAY_COUNT ? k_actions[action].key : 0; }

NdsInputPlatform nds_input_host_platform(void)
{
#if defined(__APPLE__)
    return NDS_PLATFORM_MAC;
#elif defined(_WIN32)
    return NDS_PLATFORM_WINDOWS;
#else
    return NDS_PLATFORM_STEAM_DECK;
#endif
}

static NdsBinding key(int scancode, uint8_t mods = 0)
{
    NdsBinding b = { NDS_BIND_KEY, mods, (int16_t)scancode };
    return b;
}
static NdsBinding button(int b)
{
    NdsBinding r = { NDS_BIND_BUTTON, 0, (int16_t)b };
    return r;
}
static NdsBinding axis(int a, bool positive)
{
    NdsBinding r = { (uint8_t)(positive ? NDS_BIND_AXIS_POSITIVE : NDS_BIND_AXIS_NEGATIVE), 0, (int16_t)a };
    return r;
}

void nds_input_defaults(NdsInputMap &map, NdsInputPlatform platform)
{
    memset(&map, 0, sizeof map);
    NdsBinding(*kb)[2] = map.keyboard;
    NdsBinding(*pad)[2] = map.controller;

    /* Buttons: the same on every keyboard, and the GBA frontend's with X
     * and Y beside A and B. */
    kb[NDS_ACTION_UP][0] = key(SDL_SCANCODE_UP);
    kb[NDS_ACTION_UP][1] = key(SDL_SCANCODE_W);
    kb[NDS_ACTION_DOWN][0] = key(SDL_SCANCODE_DOWN);
    kb[NDS_ACTION_DOWN][1] = key(SDL_SCANCODE_S);
    kb[NDS_ACTION_LEFT][0] = key(SDL_SCANCODE_LEFT);
    kb[NDS_ACTION_LEFT][1] = key(SDL_SCANCODE_A);
    kb[NDS_ACTION_RIGHT][0] = key(SDL_SCANCODE_RIGHT);
    kb[NDS_ACTION_RIGHT][1] = key(SDL_SCANCODE_D);
    kb[NDS_ACTION_A][0] = key(SDL_SCANCODE_Z);
    kb[NDS_ACTION_A][1] = key(SDL_SCANCODE_J);
    kb[NDS_ACTION_B][0] = key(SDL_SCANCODE_X);
    kb[NDS_ACTION_B][1] = key(SDL_SCANCODE_K);
    kb[NDS_ACTION_X][0] = key(SDL_SCANCODE_C);
    kb[NDS_ACTION_X][1] = key(SDL_SCANCODE_I);
    kb[NDS_ACTION_Y][0] = key(SDL_SCANCODE_V);
    kb[NDS_ACTION_Y][1] = key(SDL_SCANCODE_U);
    kb[NDS_ACTION_L][0] = key(SDL_SCANCODE_Q);
    kb[NDS_ACTION_R][0] = key(SDL_SCANCODE_E);
    kb[NDS_ACTION_START][0] = key(SDL_SCANCODE_RETURN);
    kb[NDS_ACTION_SELECT][0] = key(SDL_SCANCODE_BACKSPACE);
    kb[NDS_ACTION_SELECT][1] = key(SDL_SCANCODE_RSHIFT);
    /* The mouse is the stylus, so the cursor and its button have no keys
     * until the player gives them some. */
    kb[NDS_ACTION_BLOW][0] = key(SDL_SCANCODE_SPACE);
    kb[NDS_ACTION_FAST_FORWARD][0] = key(SDL_SCANCODE_TAB);
    kb[NDS_ACTION_MUTE][0] = key(SDL_SCANCODE_M);

    if (platform == NDS_PLATFORM_MAC) {
        kb[NDS_ACTION_LID][0] = key(SDL_SCANCODE_K, NDS_MOD_GUI);
        kb[NDS_ACTION_NEXT_LAYOUT][0] = key(SDL_SCANCODE_D, NDS_MOD_GUI);
        kb[NDS_ACTION_SWAP_SCREENS][0] = key(SDL_SCANCODE_E, NDS_MOD_GUI);
        kb[NDS_ACTION_OVERLAY][0] = key(SDL_SCANCODE_I, NDS_MOD_GUI);
        kb[NDS_ACTION_FPS][0] = key(SDL_SCANCODE_I, NDS_MOD_GUI | NDS_MOD_SHIFT);
        kb[NDS_ACTION_MENU][0] = key(SDL_SCANCODE_COMMA, NDS_MOD_GUI); /* the Mac Settings shortcut */
        kb[NDS_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F, NDS_MOD_GUI);
        kb[NDS_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_P, NDS_MOD_GUI);
    } else {
        /* F5 to F8 are left free: they are the save-state keys on the
         * other consoles. */
        kb[NDS_ACTION_LID][0] = key(SDL_SCANCODE_F4);
        kb[NDS_ACTION_NEXT_LAYOUT][0] = key(SDL_SCANCODE_F2);
        kb[NDS_ACTION_SWAP_SCREENS][0] = key(SDL_SCANCODE_F3);
        kb[NDS_ACTION_OVERLAY][0] = key(SDL_SCANCODE_F1);
        kb[NDS_ACTION_FPS][0] = key(SDL_SCANCODE_F9);
        kb[NDS_ACTION_MENU][0] = key(SDL_SCANCODE_F10);
        kb[NDS_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F11);
        kb[NDS_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_F12);
    }

    /* Controller: the same on every controller. SDL names buttons by the
     * Xbox layout, so its B is the east button and its Y the north one. */
    pad[NDS_ACTION_UP][0] = button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    pad[NDS_ACTION_UP][1] = axis(SDL_CONTROLLER_AXIS_LEFTY, false);
    pad[NDS_ACTION_DOWN][0] = button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad[NDS_ACTION_DOWN][1] = axis(SDL_CONTROLLER_AXIS_LEFTY, true);
    pad[NDS_ACTION_LEFT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    pad[NDS_ACTION_LEFT][1] = axis(SDL_CONTROLLER_AXIS_LEFTX, false);
    pad[NDS_ACTION_RIGHT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    pad[NDS_ACTION_RIGHT][1] = axis(SDL_CONTROLLER_AXIS_LEFTX, true);
    pad[NDS_ACTION_A][0] = button(SDL_CONTROLLER_BUTTON_B); /* east */
    pad[NDS_ACTION_B][0] = button(SDL_CONTROLLER_BUTTON_A); /* south */
    pad[NDS_ACTION_X][0] = button(SDL_CONTROLLER_BUTTON_Y); /* north */
    pad[NDS_ACTION_Y][0] = button(SDL_CONTROLLER_BUTTON_X); /* west */
    pad[NDS_ACTION_L][0] = button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    pad[NDS_ACTION_R][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    pad[NDS_ACTION_START][0] = button(SDL_CONTROLLER_BUTTON_START);
    pad[NDS_ACTION_SELECT][0] = button(SDL_CONTROLLER_BUTTON_BACK);
    /* The stylus: the right stick moves a cursor over the bottom screen and
     * the trigger under the same hand puts the pen down. A Steam Deck in
     * Game Mode has no mouse. */
    pad[NDS_ACTION_TOUCH][0] = axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true);
    pad[NDS_ACTION_CURSOR_UP][0] = axis(SDL_CONTROLLER_AXIS_RIGHTY, false);
    pad[NDS_ACTION_CURSOR_DOWN][0] = axis(SDL_CONTROLLER_AXIS_RIGHTY, true);
    pad[NDS_ACTION_CURSOR_LEFT][0] = axis(SDL_CONTROLLER_AXIS_RIGHTX, false);
    pad[NDS_ACTION_CURSOR_RIGHT][0] = axis(SDL_CONTROLLER_AXIS_RIGHTX, true);
    pad[NDS_ACTION_BLOW][0] = axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true);
    /* The lid has no button: every one left is easy to press by accident,
     * and closing the lid stops the game. The menu closes it for a moment,
     * and the player can bind one. */
    pad[NDS_ACTION_SWAP_SCREENS][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    /* Guide also opens the menu, but Steam, Xbox Game Bar and similar
     * overlays often take it, so L3 does too. */
    pad[NDS_ACTION_MENU][0] = button(SDL_CONTROLLER_BUTTON_LEFTSTICK);
}

std::string nds_binding_to_config(const NdsBinding &b)
{
    char text[32];
    switch (b.kind) {
    case NDS_BIND_KEY:
        if (b.mods) snprintf(text, sizeof text, "key:%d:%d", b.code, b.mods);
        else snprintf(text, sizeof text, "key:%d", b.code);
        break;
    case NDS_BIND_BUTTON: snprintf(text, sizeof text, "button:%d", b.code); break;
    case NDS_BIND_AXIS_POSITIVE: snprintf(text, sizeof text, "axis:%d:+", b.code); break;
    case NDS_BIND_AXIS_NEGATIVE: snprintf(text, sizeof text, "axis:%d:-", b.code); break;
    default: snprintf(text, sizeof text, "none"); break;
    }
    return text;
}

bool nds_binding_from_config(const char *text, NdsBinding &out)
{
    NdsBinding b = { NDS_BIND_NONE, 0, 0 };
    char *end = NULL;
    if (!strcmp(text, "none")) {
        out = b;
        return true;
    }
    if (!strncmp(text, "key:", 4)) {
        long code = strtol(text + 4, &end, 10);
        long mods = 0;
        if (end && *end == ':') mods = strtol(end + 1, &end, 10);
        if (end == text + 4 || code <= SDL_SCANCODE_UNKNOWN || code >= SDL_NUM_SCANCODES || mods < 0 || mods > 15) return false;
        b = key((int)code, (uint8_t)mods);
    } else if (!strncmp(text, "button:", 7)) {
        long code = strtol(text + 7, &end, 10);
        if (end == text + 7 || code < 0 || code >= SDL_CONTROLLER_BUTTON_MAX) return false;
        b = button((int)code);
    } else if (!strncmp(text, "axis:", 5)) {
        long code = strtol(text + 5, &end, 10);
        if (end == text + 5 || code < 0 || code >= SDL_CONTROLLER_AXIS_MAX || !end || end[0] != ':' ||
            (end[1] != '+' && end[1] != '-'))
            return false;
        b = axis((int)code, end[1] == '+');
    } else {
        return false;
    }
    out = b;
    return true;
}

uint8_t nds_mods_from_sdl(unsigned m)
{
    return (uint8_t)(((m & KMOD_CTRL) ? NDS_MOD_CTRL : 0) | ((m & KMOD_SHIFT) ? NDS_MOD_SHIFT : 0) |
                     ((m & KMOD_ALT) ? NDS_MOD_ALT : 0) | ((m & KMOD_GUI) ? NDS_MOD_GUI : 0));
}

bool nds_scancode_is_modifier(int scancode)
{
    return scancode >= SDL_SCANCODE_LCTRL && scancode <= SDL_SCANCODE_RGUI;
}

bool nds_key_mods_match(const NdsBinding &b, uint8_t held)
{
    if (b.kind != NDS_BIND_KEY) return false;
    if (nds_scancode_is_modifier(b.code)) return true; /* a modifier bound on its own */
    if (!b.mods) return (held & (NDS_MOD_CTRL | NDS_MOD_ALT | NDS_MOD_GUI)) == 0;
    return held == b.mods;
}

static bool contains(const char *text, const char *needle)
{
    if (!text) return false;
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (needle[i] && p[i] && tolower((unsigned char)p[i]) == needle[i]) i++;
        if (!needle[i]) return true;
    }
    return false;
}

NdsPadProfile nds_pad_profile(SDL_GameController *pad, NdsInputPlatform platform)
{
    if (pad) {
        const char *name = SDL_GameControllerName(pad);
        /* Under Steam the Deck's controls are a virtual gamepad. */
        if (contains(name, "steam")) return NDS_PAD_STEAM_DECK;
        switch (SDL_GameControllerGetType(pad)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE: return platform == NDS_PLATFORM_STEAM_DECK ? NDS_PAD_STEAM_DECK : NDS_PAD_XBOX;
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5: return NDS_PAD_PLAYSTATION;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return NDS_PAD_NINTENDO;
        default: break;
        }
        if (contains(name, "xbox") || contains(name, "xinput")) return NDS_PAD_XBOX;
        if (contains(name, "playstation") || contains(name, "dualshock") || contains(name, "dualsense")) return NDS_PAD_PLAYSTATION;
        if (contains(name, "nintendo") || contains(name, "switch") || contains(name, "joy-con")) return NDS_PAD_NINTENDO;
    }
    return platform == NDS_PLATFORM_STEAM_DECK ? NDS_PAD_STEAM_DECK : NDS_PAD_GENERIC;
}

/* Names per NdsPadProfile: generic, Xbox, PlayStation, Nintendo, Steam Deck. */
static const char *pad_name(const char *const names[5], NdsPadProfile pad) { return names[pad]; }

static std::string button_label(int b, NdsPadProfile pad)
{
    static const char *const south[] = { "South Button", "A", "Cross", "B", "A" };
    static const char *const east[] = { "East Button", "B", "Circle", "A", "B" };
    static const char *const west[] = { "West Button", "X", "Square", "Y", "X" };
    static const char *const north[] = { "North Button", "Y", "Triangle", "X", "Y" };
    static const char *const back[] = { "Back", "View", "Create", "-", "View" };
    static const char *const start[] = { "Start", "Menu", "Options", "+", "Menu" };
    static const char *const guide[] = { "Guide", "Xbox", "PS", "Home", "Steam" };
    static const char *const lb[] = { "Left Shoulder", "LB", "L1", "L", "L1" };
    static const char *const rb[] = { "Right Shoulder", "RB", "R1", "R", "R1" };
    static const char *const ls[] = { "Left Stick Click", "LS Click", "L3", "Left Stick Click", "L3" };
    static const char *const rs[] = { "Right Stick Click", "RS Click", "R3", "Right Stick Click", "R3" };
    /* SDL's paddles: upper right, upper left, lower right, lower left. */
    static const char *const p1[] = { "Paddle 1", "P1", "Paddle 1", "Paddle 1", "R4" };
    static const char *const p2[] = { "Paddle 2", "P3", "Paddle 2", "Paddle 2", "L4" };
    static const char *const p3[] = { "Paddle 3", "P2", "Paddle 3", "Paddle 3", "R5" };
    static const char *const p4[] = { "Paddle 4", "P4", "Paddle 4", "Paddle 4", "L5" };
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return pad_name(south, pad);
    case SDL_CONTROLLER_BUTTON_B: return pad_name(east, pad);
    case SDL_CONTROLLER_BUTTON_X: return pad_name(west, pad);
    case SDL_CONTROLLER_BUTTON_Y: return pad_name(north, pad);
    case SDL_CONTROLLER_BUTTON_BACK: return pad_name(back, pad);
    case SDL_CONTROLLER_BUTTON_GUIDE: return pad_name(guide, pad);
    case SDL_CONTROLLER_BUTTON_START: return pad_name(start, pad);
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return pad_name(ls, pad);
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return pad_name(rs, pad);
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return pad_name(lb, pad);
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return pad_name(rb, pad);
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-Pad Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-Pad Down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-Pad Left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-Pad Right";
    case SDL_CONTROLLER_BUTTON_PADDLE1: return pad_name(p1, pad);
    case SDL_CONTROLLER_BUTTON_PADDLE2: return pad_name(p2, pad);
    case SDL_CONTROLLER_BUTTON_PADDLE3: return pad_name(p3, pad);
    case SDL_CONTROLLER_BUTTON_PADDLE4: return pad_name(p4, pad);
    case SDL_CONTROLLER_BUTTON_MISC1: return pad == NDS_PAD_STEAM_DECK ? "Quick Access" : "Share / Capture";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad Click";
    default: return "Button " + std::to_string(b);
    }
}

static std::string axis_label(int a, bool positive, NdsPadProfile pad)
{
    static const char *const lt[] = { "Left Trigger", "LT", "L2", "ZL", "L2" };
    static const char *const rt[] = { "Right Trigger", "RT", "R2", "ZR", "R2" };
    switch (a) {
    case SDL_CONTROLLER_AXIS_LEFTX: return positive ? "Left Stick Right" : "Left Stick Left";
    case SDL_CONTROLLER_AXIS_LEFTY: return positive ? "Left Stick Down" : "Left Stick Up";
    case SDL_CONTROLLER_AXIS_RIGHTX: return positive ? "Right Stick Right" : "Right Stick Left";
    case SDL_CONTROLLER_AXIS_RIGHTY: return positive ? "Right Stick Down" : "Right Stick Up";
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return pad_name(lt, pad);
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return pad_name(rt, pad);
    default: return "Axis " + std::to_string(a) + (positive ? "+" : "-");
    }
}

/* Key names as printed on this platform's keyboards. */
static std::string key_name(int scancode, NdsInputPlatform platform)
{
    if (platform == NDS_PLATFORM_MAC) {
        switch (scancode) {
        case SDL_SCANCODE_BACKSPACE: return "Delete";
        case SDL_SCANCODE_DELETE: return "Forward Delete";
        case SDL_SCANCODE_LGUI: return "Command";
        case SDL_SCANCODE_RGUI: return "Right Command";
        case SDL_SCANCODE_LALT: return "Option";
        case SDL_SCANCODE_RALT: return "Right Option";
        case SDL_SCANCODE_LCTRL: return "Control";
        case SDL_SCANCODE_RCTRL: return "Right Control";
        default: break;
        }
    } else {
        switch (scancode) {
        case SDL_SCANCODE_RETURN: return "Enter";
        case SDL_SCANCODE_LGUI: return platform == NDS_PLATFORM_WINDOWS ? "Windows" : "Super";
        case SDL_SCANCODE_RGUI: return platform == NDS_PLATFORM_WINDOWS ? "Right Windows" : "Right Super";
        default: break;
        }
    }
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return name && name[0] ? name : "Key " + std::to_string(scancode);
}

std::string nds_binding_label(const NdsBinding &b, NdsInputPlatform platform, NdsPadProfile pad)
{
    switch (b.kind) {
    case NDS_BIND_KEY: {
        std::string text;
        const bool mac = platform == NDS_PLATFORM_MAC;
        if (b.mods & NDS_MOD_CTRL) text += mac ? "Control+" : "Ctrl+";
        if (b.mods & NDS_MOD_ALT) text += mac ? "Option+" : "Alt+";
        if (b.mods & NDS_MOD_SHIFT) text += "Shift+";
        if (b.mods & NDS_MOD_GUI) text += mac ? "Cmd+" : platform == NDS_PLATFORM_WINDOWS ? "Win+" : "Super+";
        return text + key_name(b.code, platform);
    }
    case NDS_BIND_BUTTON: return button_label(b.code, pad);
    case NDS_BIND_AXIS_POSITIVE: return axis_label(b.code, true, pad);
    case NDS_BIND_AXIS_NEGATIVE: return axis_label(b.code, false, pad);
    default: return "Unbound";
    }
}

const char *nds_menu_hint(NdsInputPlatform platform)
{
    switch (platform) {
    case NDS_PLATFORM_MAC: return "Menu: Escape or Cmd+, / controller Guide or L3";
    case NDS_PLATFORM_STEAM_DECK: return "Menu: L3 (click the left stick) / Escape on a keyboard. Steam keeps the Steam button.";
    default: return "Menu: Escape or F10 / controller Guide or L3";
    }
}

/* ------------------------------------------------------------------ */
/* Screens                                                             */
/* ------------------------------------------------------------------ */

/* Plain ints, not enumerators: they are multiplied by scales, and C++20
 * frowns on arithmetic between an enumeration and a floating-point type. */
static const int SCREEN_W = NDS_SCREEN_WIDTH, SCREEN_H = NDS_SCREEN_HEIGHT;

static const struct {
    const char *label;
    const char *config;
} k_layouts[NDS_LAYOUT_COUNT] = {
    { "Stacked", "stacked" },
    { "Side by Side", "side" },
    { "Large + Small", "large" },
    { "Single Screen", "single" },
};

const char *nds_layout_label(int layout) { return k_layouts[layout].label; }
const char *nds_layout_config(int layout) { return k_layouts[layout].config; }

int nds_layout_from_config(const char *name)
{
    for (int i = 0; i < NDS_LAYOUT_COUNT; i++) {
        if (!strcmp(name, k_layouts[i].config)) return i;
    }
    return -1;
}

const char *nds_scaling_label(int scaling)
{
    return scaling == NDS_SCALING_FIT ? "Fit" : "Pixel Perfect";
}

static int clamp_gap(int gap)
{
    return gap < 0 ? 0 : gap > NDS_GAP_MAX ? NDS_GAP_MAX : gap;
}

void nds_layout_size(int layout, int gap, int scale, int *w, int *h)
{
    if (scale < 1) scale = 1;
    gap = clamp_gap(gap);
    switch (layout) {
    case NDS_LAYOUT_SIDE_BY_SIDE:
        *w = (2 * SCREEN_W + gap) * scale;
        *h = SCREEN_H * scale;
        break;
    case NDS_LAYOUT_LARGE_SMALL:
        /* The large screen at twice the small one's scale, side by side. */
        *w = (3 * SCREEN_W + gap) * scale;
        *h = 2 * SCREEN_H * scale;
        break;
    case NDS_LAYOUT_SINGLE:
        *w = SCREEN_W * scale;
        *h = SCREEN_H * scale;
        break;
    default:
        *w = SCREEN_W * scale;
        *h = (2 * SCREEN_H + gap) * scale;
        break;
    }
}

/* The scale at which a unit_w x unit_h block of DS pixels fits w x h. */
static double fit_scale(int scaling, int w, int h, int unit_w, int unit_h)
{
    double sx = (double)w / unit_w, sy = (double)h / unit_h;
    double s = sx < sy ? sx : sy;
    if (scaling == NDS_SCALING_PIXEL_PERFECT) return s < 1.0 ? 1.0 : floor(s);
    return s;
}

/* A screen at `scale` whose top left corner is (x, y). Sizes round down,
 * so two screens and a gap never add up to more than the room they were
 * fitted to. */
static int scaled(int pixels, double scale)
{
    int v = (int)floor(pixels * scale + 1e-9);
    return v < 1 && pixels > 0 ? 1 : v;
}

static SDL_Rect screen_rect(int x, int y, double scale)
{
    SDL_Rect r = { x, y, scaled(SCREEN_W, scale), scaled(SCREEN_H, scale) };
    return r;
}

/* Large + small: the two scales, and whether the small screen goes beside
 * the large one or above/below it. */
struct LargeSmall {
    double large, small;
    bool beside;
};

/* The large screen takes the biggest scale that still leaves the small one
 * at least half of it (never under 1x); the small one then takes what is
 * left, up to the large one's scale. Tried both ways round; beside wins a
 * tie. `along` is the direction the two screens are laid out in. */
static LargeSmall large_small_scales(int scaling, int gap, int w, int h)
{
    LargeSmall best = { 0.0, 0.0, true };
    for (int beside = 1; beside >= 0; beside--) {
        const double along = beside ? w : h, across = beside ? h : w;
        const double unit_along = beside ? SCREEN_W : SCREEN_H, unit_across = beside ? SCREEN_H : SCREEN_W;
        double large, small;
        if (scaling == NDS_SCALING_PIXEL_PERFECT) {
            int l = (int)floor(across / unit_across), s = 0;
            if (l > (int)floor(along / unit_along)) l = (int)floor(along / unit_along);
            for (; l >= 1; l--) {
                s = (int)floor((along - l * unit_along) / (unit_along + gap));
                if (s > l) s = l;
                if (s >= (l / 2 > 1 ? l / 2 : 1)) break;
            }
            if (l < 1) l = s = 1; /* too small a display for both: they overlap its edges */
            large = l;
            small = s;
        } else {
            large = along / (unit_along + (unit_along + gap) / 2.0);
            if (large > across / unit_across) large = across / unit_across;
            small = (along - large * unit_along) / (unit_along + gap);
            if (small > large) small = large;
        }
        if (large > best.large + 1e-9 || (large > best.large - 1e-9 && small > best.small + 1e-9)) {
            best.large = large;
            best.small = small;
            best.beside = beside != 0;
        }
    }
    return best;
}

NdsScreens nds_layout_screens(int layout, bool swapped, int gap, int scaling, int w, int h)
{
    NdsScreens out;
    memset(&out, 0, sizeof out);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    gap = clamp_gap(gap);
    /* The screen that comes first: above, on the left, large, or alone. */
    const int stacked_first = swapped ? 1 : 0;
    const int touch_first = swapped ? 0 : 1;

    switch (layout) {
    case NDS_LAYOUT_SIDE_BY_SIDE: {
        const double s = fit_scale(scaling, w, h, 2 * SCREEN_W + gap, SCREEN_H);
        const int sw = scaled(SCREEN_W, s), sh = scaled(SCREEN_H, s), gp = scaled(gap, s);
        const int x = (w - (2 * sw + gp)) / 2, y = (h - sh) / 2;
        out.rect[stacked_first] = screen_rect(x, y, s);
        out.rect[1 - stacked_first] = screen_rect(x + sw + gp, y, s);
        break;
    }
    case NDS_LAYOUT_LARGE_SMALL: {
        const LargeSmall ls = large_small_scales(scaling, gap, w, h);
        const int lw = scaled(SCREEN_W, ls.large), lh = scaled(SCREEN_H, ls.large);
        const int sw = scaled(SCREEN_W, ls.small), sh = scaled(SCREEN_H, ls.small), gp = scaled(gap, ls.small);
        const int large = touch_first, small = 1 - touch_first;
        if (ls.beside) {
            const int x = (w - (lw + gp + sw)) / 2;
            out.rect[large] = screen_rect(x, (h - lh) / 2, ls.large);
            out.rect[small] = screen_rect(x + lw + gp, (h - sh) / 2, ls.small);
        } else {
            /* One above the other: the top screen stays on top. */
            const int y = (h - (lh + gp + sh)) / 2;
            if (large == 0) {
                out.rect[large] = screen_rect((w - lw) / 2, y, ls.large);
                out.rect[small] = screen_rect((w - sw) / 2, y + lh + gp, ls.small);
            } else {
                out.rect[small] = screen_rect((w - sw) / 2, y, ls.small);
                out.rect[large] = screen_rect((w - lw) / 2, y + sh + gp, ls.large);
            }
        }
        break;
    }
    case NDS_LAYOUT_SINGLE: {
        const double s = fit_scale(scaling, w, h, SCREEN_W, SCREEN_H);
        out.rect[touch_first] = screen_rect((w - scaled(SCREEN_W, s)) / 2, (h - scaled(SCREEN_H, s)) / 2, s);
        break;
    }
    default: {
        const double s = fit_scale(scaling, w, h, SCREEN_W, 2 * SCREEN_H + gap);
        const int sw = scaled(SCREEN_W, s), sh = scaled(SCREEN_H, s), gp = scaled(gap, s);
        const int x = (w - sw) / 2, y = (h - (2 * sh + gp)) / 2;
        out.rect[stacked_first] = screen_rect(x, y, s);
        out.rect[1 - stacked_first] = screen_rect(x, y + sh + gp, s);
        break;
    }
    }
    return out;
}

bool nds_layout_touch(const NdsScreens &screens, double px, double py, int *x, int *y)
{
    const SDL_Rect &r = screens.rect[1];
    *x = *y = 0;
    if (r.w <= 0 || r.h <= 0) return false;
    const double fx = floor((px - r.x) * SCREEN_W / r.w), fy = floor((py - r.y) * SCREEN_H / r.h);
    *x = fx < 0.0 ? 0 : fx > SCREEN_W - 1 ? SCREEN_W - 1 : (int)fx;
    *y = fy < 0.0 ? 0 : fy > SCREEN_H - 1 ? SCREEN_H - 1 : (int)fy;
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

bool nds_layout_touch_position(const NdsScreens &screens, double x, double y, double *px, double *py)
{
    const SDL_Rect &r = screens.rect[1];
    *px = *py = 0.0;
    if (r.w <= 0 || r.h <= 0) return false;
    *px = r.x + x * r.w / SCREEN_W;
    *py = r.y + y * r.h / SCREEN_H;
    return true;
}
