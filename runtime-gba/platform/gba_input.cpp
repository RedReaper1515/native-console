/* Input bindings for the GBA frontend (see gba_input.h). */
#include "gba_input.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba.h"

static const struct {
    const char *label;
    const char *config;
    uint16_t keypad;
} k_actions[GBA_ACTION_COUNT] = {
    { "Up", "up", GBA_KEY_UP },
    { "Down", "down", GBA_KEY_DOWN },
    { "Left", "left", GBA_KEY_LEFT },
    { "Right", "right", GBA_KEY_RIGHT },
    { "A", "a", GBA_KEY_A },
    { "B", "b", GBA_KEY_B },
    { "L", "l", GBA_KEY_L },
    { "R", "r", GBA_KEY_R },
    { "Start", "start", GBA_KEY_START },
    { "Select", "select", GBA_KEY_SELECT },
    { "Fast Forward (Hold)", "fast_forward", 0 },
    { "Save State", "save_state", 0 },
    { "Load State", "load_state", 0 },
    { "Previous State Slot", "previous_state_slot", 0 },
    { "Next State Slot", "next_state_slot", 0 },
    { "Toggle Overlay", "toggle_overlay", 0 },
    { "Toggle FPS Counter", "toggle_fps", 0 },
    { "Toggle Mute", "toggle_mute", 0 },
    { "Toggle Menu", "toggle_menu", 0 },
    { "Toggle Fullscreen", "toggle_fullscreen", 0 },
    { "Screenshot", "screenshot", 0 },
};

const char *gba_action_label(int action) { return k_actions[action].label; }
const char *gba_action_config(int action) { return k_actions[action].config; }
uint16_t gba_action_keypad_bit(int action) { return action < GBA_ACTION_GAMEPLAY_COUNT ? k_actions[action].keypad : 0; }

GbaInputPlatform gba_input_host_platform(void)
{
#if defined(__APPLE__)
    return GBA_PLATFORM_MAC;
#elif defined(_WIN32)
    return GBA_PLATFORM_WINDOWS;
#else
    return GBA_PLATFORM_STEAM_DECK;
#endif
}

static GbaBinding key(int scancode, uint8_t mods = 0)
{
    GbaBinding b = { GBA_BIND_KEY, mods, (int16_t)scancode };
    return b;
}
static GbaBinding button(int b)
{
    GbaBinding r = { GBA_BIND_BUTTON, 0, (int16_t)b };
    return r;
}
static GbaBinding axis(int a, bool positive)
{
    GbaBinding r = { (uint8_t)(positive ? GBA_BIND_AXIS_POSITIVE : GBA_BIND_AXIS_NEGATIVE), 0, (int16_t)a };
    return r;
}

void gba_input_defaults(GbaInputMap &map, GbaInputPlatform platform)
{
    memset(&map, 0, sizeof map);
    GbaBinding(*kb)[2] = map.keyboard;
    GbaBinding(*pad)[2] = map.controller;

    /* Gameplay: the same on every keyboard. */
    kb[GBA_ACTION_UP][0] = key(SDL_SCANCODE_UP);
    kb[GBA_ACTION_UP][1] = key(SDL_SCANCODE_W);
    kb[GBA_ACTION_DOWN][0] = key(SDL_SCANCODE_DOWN);
    kb[GBA_ACTION_DOWN][1] = key(SDL_SCANCODE_S);
    kb[GBA_ACTION_LEFT][0] = key(SDL_SCANCODE_LEFT);
    kb[GBA_ACTION_LEFT][1] = key(SDL_SCANCODE_A);
    kb[GBA_ACTION_RIGHT][0] = key(SDL_SCANCODE_RIGHT);
    kb[GBA_ACTION_RIGHT][1] = key(SDL_SCANCODE_D);
    kb[GBA_ACTION_A][0] = key(SDL_SCANCODE_Z);
    kb[GBA_ACTION_A][1] = key(SDL_SCANCODE_J);
    kb[GBA_ACTION_B][0] = key(SDL_SCANCODE_X);
    kb[GBA_ACTION_B][1] = key(SDL_SCANCODE_K);
    kb[GBA_ACTION_L][0] = key(SDL_SCANCODE_Q);
    kb[GBA_ACTION_R][0] = key(SDL_SCANCODE_E);
    kb[GBA_ACTION_START][0] = key(SDL_SCANCODE_RETURN);
    kb[GBA_ACTION_SELECT][0] = key(SDL_SCANCODE_BACKSPACE);
    kb[GBA_ACTION_SELECT][1] = key(SDL_SCANCODE_RSHIFT);
    kb[GBA_ACTION_FAST_FORWARD][0] = key(SDL_SCANCODE_TAB);
    kb[GBA_ACTION_MUTE][0] = key(SDL_SCANCODE_M);

    if (platform == GBA_PLATFORM_MAC) {
        kb[GBA_ACTION_SAVE_STATE][0] = key(SDL_SCANCODE_S, GBA_MOD_GUI);
        kb[GBA_ACTION_LOAD_STATE][0] = key(SDL_SCANCODE_L, GBA_MOD_GUI);
        kb[GBA_ACTION_PREVIOUS_SLOT][0] = key(SDL_SCANCODE_LEFTBRACKET, GBA_MOD_GUI);
        kb[GBA_ACTION_NEXT_SLOT][0] = key(SDL_SCANCODE_RIGHTBRACKET, GBA_MOD_GUI);
        kb[GBA_ACTION_OVERLAY][0] = key(SDL_SCANCODE_I, GBA_MOD_GUI);
        kb[GBA_ACTION_FPS][0] = key(SDL_SCANCODE_I, GBA_MOD_GUI | GBA_MOD_SHIFT);
        kb[GBA_ACTION_MENU][0] = key(SDL_SCANCODE_COMMA, GBA_MOD_GUI); /* the Mac Settings shortcut */
        kb[GBA_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F, GBA_MOD_GUI);
        kb[GBA_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_P, GBA_MOD_GUI);
    } else {
        kb[GBA_ACTION_SAVE_STATE][0] = key(SDL_SCANCODE_F5);
        kb[GBA_ACTION_LOAD_STATE][0] = key(SDL_SCANCODE_F8);
        kb[GBA_ACTION_PREVIOUS_SLOT][0] = key(SDL_SCANCODE_F6);
        kb[GBA_ACTION_NEXT_SLOT][0] = key(SDL_SCANCODE_F7);
        kb[GBA_ACTION_OVERLAY][0] = key(SDL_SCANCODE_F1);
        kb[GBA_ACTION_FPS][0] = key(SDL_SCANCODE_F9);
        kb[GBA_ACTION_MENU][0] = key(SDL_SCANCODE_F10);
        kb[GBA_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F11);
        kb[GBA_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_F12);
    }

    /* Controller gameplay: the same on every controller. */
    pad[GBA_ACTION_UP][0] = button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    pad[GBA_ACTION_UP][1] = axis(SDL_CONTROLLER_AXIS_LEFTY, false);
    pad[GBA_ACTION_DOWN][0] = button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad[GBA_ACTION_DOWN][1] = axis(SDL_CONTROLLER_AXIS_LEFTY, true);
    pad[GBA_ACTION_LEFT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    pad[GBA_ACTION_LEFT][1] = axis(SDL_CONTROLLER_AXIS_LEFTX, false);
    pad[GBA_ACTION_RIGHT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    pad[GBA_ACTION_RIGHT][1] = axis(SDL_CONTROLLER_AXIS_LEFTX, true);
    pad[GBA_ACTION_A][0] = button(SDL_CONTROLLER_BUTTON_B); /* east */
    pad[GBA_ACTION_B][0] = button(SDL_CONTROLLER_BUTTON_A); /* south */
    pad[GBA_ACTION_L][0] = button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    pad[GBA_ACTION_L][1] = axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true);
    pad[GBA_ACTION_R][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    pad[GBA_ACTION_R][1] = axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true);
    pad[GBA_ACTION_START][0] = button(SDL_CONTROLLER_BUTTON_START);
    pad[GBA_ACTION_SELECT][0] = button(SDL_CONTROLLER_BUTTON_BACK);
    /* Guide also opens the menu, but Steam, Xbox Game Bar and similar
     * overlays often take it, so L3 does too. */
    pad[GBA_ACTION_MENU][0] = button(SDL_CONTROLLER_BUTTON_LEFTSTICK);

    if (platform == GBA_PLATFORM_STEAM_DECK) {
        /* No keyboard on a Deck: save states and fast forward on buttons. */
        pad[GBA_ACTION_SAVE_STATE][0] = button(SDL_CONTROLLER_BUTTON_X);
        pad[GBA_ACTION_LOAD_STATE][0] = button(SDL_CONTROLLER_BUTTON_Y);
        pad[GBA_ACTION_FAST_FORWARD][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    }
}

std::string gba_binding_to_config(const GbaBinding &b)
{
    char text[32];
    switch (b.kind) {
    case GBA_BIND_KEY:
        if (b.mods) snprintf(text, sizeof text, "key:%d:%d", b.code, b.mods);
        else snprintf(text, sizeof text, "key:%d", b.code);
        break;
    case GBA_BIND_BUTTON: snprintf(text, sizeof text, "button:%d", b.code); break;
    case GBA_BIND_AXIS_POSITIVE: snprintf(text, sizeof text, "axis:%d:+", b.code); break;
    case GBA_BIND_AXIS_NEGATIVE: snprintf(text, sizeof text, "axis:%d:-", b.code); break;
    default: snprintf(text, sizeof text, "none"); break;
    }
    return text;
}

bool gba_binding_from_config(const char *text, GbaBinding &out)
{
    GbaBinding b = { GBA_BIND_NONE, 0, 0 };
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

uint8_t gba_mods_from_sdl(unsigned m)
{
    return (uint8_t)(((m & KMOD_CTRL) ? GBA_MOD_CTRL : 0) | ((m & KMOD_SHIFT) ? GBA_MOD_SHIFT : 0) |
                     ((m & KMOD_ALT) ? GBA_MOD_ALT : 0) | ((m & KMOD_GUI) ? GBA_MOD_GUI : 0));
}

bool gba_scancode_is_modifier(int scancode)
{
    return scancode >= SDL_SCANCODE_LCTRL && scancode <= SDL_SCANCODE_RGUI;
}

bool gba_key_mods_match(const GbaBinding &b, uint8_t held)
{
    if (b.kind != GBA_BIND_KEY) return false;
    if (gba_scancode_is_modifier(b.code)) return true; /* a modifier bound on its own */
    if (!b.mods) return (held & (GBA_MOD_CTRL | GBA_MOD_ALT | GBA_MOD_GUI)) == 0;
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

GbaPadProfile gba_pad_profile(SDL_GameController *pad, GbaInputPlatform platform)
{
    if (pad) {
        const char *name = SDL_GameControllerName(pad);
        /* Under Steam the Deck's controls are a virtual gamepad. */
        if (contains(name, "steam")) return GBA_PAD_STEAM_DECK;
        switch (SDL_GameControllerGetType(pad)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE: return platform == GBA_PLATFORM_STEAM_DECK ? GBA_PAD_STEAM_DECK : GBA_PAD_XBOX;
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5: return GBA_PAD_PLAYSTATION;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return GBA_PAD_NINTENDO;
        default: break;
        }
        if (contains(name, "xbox") || contains(name, "xinput")) return GBA_PAD_XBOX;
        if (contains(name, "playstation") || contains(name, "dualshock") || contains(name, "dualsense")) return GBA_PAD_PLAYSTATION;
        if (contains(name, "nintendo") || contains(name, "switch") || contains(name, "joy-con")) return GBA_PAD_NINTENDO;
    }
    return platform == GBA_PLATFORM_STEAM_DECK ? GBA_PAD_STEAM_DECK : GBA_PAD_GENERIC;
}

/* Names per GbaPadProfile: generic, Xbox, PlayStation, Nintendo, Steam Deck. */
static const char *pad_name(const char *const names[5], GbaPadProfile pad) { return names[pad]; }

static std::string button_label(int b, GbaPadProfile pad)
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
    case SDL_CONTROLLER_BUTTON_MISC1: return pad == GBA_PAD_STEAM_DECK ? "Quick Access" : "Share / Capture";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad Click";
    default: return "Button " + std::to_string(b);
    }
}

static std::string axis_label(int a, bool positive, GbaPadProfile pad)
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
static std::string key_name(int scancode, GbaInputPlatform platform)
{
    if (platform == GBA_PLATFORM_MAC) {
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
        case SDL_SCANCODE_LGUI: return platform == GBA_PLATFORM_WINDOWS ? "Windows" : "Super";
        case SDL_SCANCODE_RGUI: return platform == GBA_PLATFORM_WINDOWS ? "Right Windows" : "Right Super";
        default: break;
        }
    }
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return name && name[0] ? name : "Key " + std::to_string(scancode);
}

std::string gba_binding_label(const GbaBinding &b, GbaInputPlatform platform, GbaPadProfile pad)
{
    switch (b.kind) {
    case GBA_BIND_KEY: {
        std::string text;
        const bool mac = platform == GBA_PLATFORM_MAC;
        if (b.mods & GBA_MOD_CTRL) text += mac ? "Control+" : "Ctrl+";
        if (b.mods & GBA_MOD_ALT) text += mac ? "Option+" : "Alt+";
        if (b.mods & GBA_MOD_SHIFT) text += "Shift+";
        if (b.mods & GBA_MOD_GUI) text += mac ? "Cmd+" : platform == GBA_PLATFORM_WINDOWS ? "Win+" : "Super+";
        return text + key_name(b.code, platform);
    }
    case GBA_BIND_BUTTON: return button_label(b.code, pad);
    case GBA_BIND_AXIS_POSITIVE: return axis_label(b.code, true, pad);
    case GBA_BIND_AXIS_NEGATIVE: return axis_label(b.code, false, pad);
    default: return "Unbound";
    }
}

const char *gba_menu_hint(GbaInputPlatform platform)
{
    switch (platform) {
    case GBA_PLATFORM_MAC: return "Menu: Escape or Cmd+, / controller Guide or L3";
    case GBA_PLATFORM_STEAM_DECK: return "Menu: L3 (click the left stick) / Escape on a keyboard. Steam keeps the Steam button.";
    default: return "Menu: Escape or F10 / controller Guide or L3";
    }
}
