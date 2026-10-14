/* Input bindings for the NES frontend (see nes_input.h). Adapted from the
 * Super Nintendo frontend's snes_input.cpp. */
#include "nes_input.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes.h"

static const struct {
    const char *label;
    const char *config;
    uint8_t keypad;
} k_actions[NES_ACTION_COUNT] = {
    { "Up", "up", NES_BTN_UP },
    { "Down", "down", NES_BTN_DOWN },
    { "Left", "left", NES_BTN_LEFT },
    { "Right", "right", NES_BTN_RIGHT },
    { "A", "a", NES_BTN_A },
    { "B", "b", NES_BTN_B },
    { "Start", "start", NES_BTN_START },
    { "Select", "select", NES_BTN_SELECT },
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

const char *nes_action_label(int action) { return k_actions[action].label; }
const char *nes_action_config(int action) { return k_actions[action].config; }
uint8_t nes_action_keypad_bit(int action) { return action < NES_ACTION_GAMEPLAY_COUNT ? k_actions[action].keypad : 0; }

NesInputPlatform nes_input_host_platform(void)
{
#if defined(__APPLE__)
    return NES_PLATFORM_MAC;
#elif defined(_WIN32)
    return NES_PLATFORM_WINDOWS;
#else
    return NES_PLATFORM_STEAM_DECK;
#endif
}

static NesBinding key(int scancode, uint8_t mods = 0)
{
    NesBinding b = { NES_BIND_KEY, mods, (int16_t)scancode };
    return b;
}
static NesBinding button(int b)
{
    NesBinding r = { NES_BIND_BUTTON, 0, (int16_t)b };
    return r;
}
static NesBinding axis(int a, bool positive)
{
    NesBinding r = { (uint8_t)(positive ? NES_BIND_AXIS_POSITIVE : NES_BIND_AXIS_NEGATIVE), 0, (int16_t)a };
    return r;
}

void nes_input_defaults(NesInputMap &map, NesInputPlatform platform)
{
    memset(&map, 0, sizeof map);
    NesBinding(*kb)[2] = map.keyboard;
    NesBinding(*pad)[2] = map.controller;

    /* Gameplay: the same on every keyboard. */
    kb[NES_ACTION_UP][0] = key(SDL_SCANCODE_UP);
    kb[NES_ACTION_DOWN][0] = key(SDL_SCANCODE_DOWN);
    kb[NES_ACTION_LEFT][0] = key(SDL_SCANCODE_LEFT);
    kb[NES_ACTION_RIGHT][0] = key(SDL_SCANCODE_RIGHT);
    /* The two buttons side by side as on the pad, B left of A; K and L
     * are the same pair for the right hand. */
    kb[NES_ACTION_A][0] = key(SDL_SCANCODE_X);
    kb[NES_ACTION_A][1] = key(SDL_SCANCODE_L);
    kb[NES_ACTION_B][0] = key(SDL_SCANCODE_Z);
    kb[NES_ACTION_B][1] = key(SDL_SCANCODE_K);
    kb[NES_ACTION_START][0] = key(SDL_SCANCODE_RETURN);
    kb[NES_ACTION_SELECT][0] = key(SDL_SCANCODE_BACKSPACE);
    kb[NES_ACTION_SELECT][1] = key(SDL_SCANCODE_RSHIFT);
    kb[NES_ACTION_FAST_FORWARD][0] = key(SDL_SCANCODE_TAB);
    kb[NES_ACTION_MUTE][0] = key(SDL_SCANCODE_M);

    if (platform == NES_PLATFORM_MAC) {
        kb[NES_ACTION_SAVE_STATE][0] = key(SDL_SCANCODE_S, NES_MOD_GUI);
        kb[NES_ACTION_LOAD_STATE][0] = key(SDL_SCANCODE_L, NES_MOD_GUI);
        kb[NES_ACTION_PREVIOUS_SLOT][0] = key(SDL_SCANCODE_LEFTBRACKET, NES_MOD_GUI);
        kb[NES_ACTION_NEXT_SLOT][0] = key(SDL_SCANCODE_RIGHTBRACKET, NES_MOD_GUI);
        kb[NES_ACTION_OVERLAY][0] = key(SDL_SCANCODE_I, NES_MOD_GUI);
        kb[NES_ACTION_FPS][0] = key(SDL_SCANCODE_I, NES_MOD_GUI | NES_MOD_SHIFT);
        kb[NES_ACTION_MENU][0] = key(SDL_SCANCODE_COMMA, NES_MOD_GUI); /* the Mac Settings shortcut */
        kb[NES_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F, NES_MOD_GUI);
        kb[NES_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_P, NES_MOD_GUI);
    } else {
        kb[NES_ACTION_SAVE_STATE][0] = key(SDL_SCANCODE_F5);
        kb[NES_ACTION_LOAD_STATE][0] = key(SDL_SCANCODE_F8);
        kb[NES_ACTION_PREVIOUS_SLOT][0] = key(SDL_SCANCODE_F6);
        kb[NES_ACTION_NEXT_SLOT][0] = key(SDL_SCANCODE_F7);
        kb[NES_ACTION_OVERLAY][0] = key(SDL_SCANCODE_F1);
        kb[NES_ACTION_FPS][0] = key(SDL_SCANCODE_F9);
        kb[NES_ACTION_MENU][0] = key(SDL_SCANCODE_F10);
        kb[NES_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F11);
        kb[NES_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_F12);
    }

    /* Controller gameplay: the same on every controller. */
    pad[NES_ACTION_UP][0] = button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    pad[NES_ACTION_UP][1] = axis(SDL_CONTROLLER_AXIS_LEFTY, false);
    pad[NES_ACTION_DOWN][0] = button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad[NES_ACTION_DOWN][1] = axis(SDL_CONTROLLER_AXIS_LEFTY, true);
    pad[NES_ACTION_LEFT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    pad[NES_ACTION_LEFT][1] = axis(SDL_CONTROLLER_AXIS_LEFTX, false);
    pad[NES_ACTION_RIGHT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    pad[NES_ACTION_RIGHT][1] = axis(SDL_CONTROLLER_AXIS_LEFTX, true);
    /* South = A and west = B, the pair a thumb rolls across (where the SNES
     * frontend has B and Y); east and north repeat them, so A is also where
     * a Nintendo pad prints it. */
    pad[NES_ACTION_A][0] = button(SDL_CONTROLLER_BUTTON_A); /* south */
    pad[NES_ACTION_A][1] = button(SDL_CONTROLLER_BUTTON_B); /* east */
    pad[NES_ACTION_B][0] = button(SDL_CONTROLLER_BUTTON_X); /* west */
    pad[NES_ACTION_B][1] = button(SDL_CONTROLLER_BUTTON_Y); /* north */
    pad[NES_ACTION_START][0] = button(SDL_CONTROLLER_BUTTON_START);
    pad[NES_ACTION_SELECT][0] = button(SDL_CONTROLLER_BUTTON_BACK);
    /* Guide also opens the menu, but Steam, Xbox Game Bar and similar
     * overlays often take it, so L3 does too. */
    pad[NES_ACTION_MENU][0] = button(SDL_CONTROLLER_BUTTON_LEFTSTICK);

    if (platform == NES_PLATFORM_STEAM_DECK) {
        /* No keyboard on a Deck: save states and fast forward on buttons. */
        pad[NES_ACTION_SAVE_STATE][0] = button(SDL_CONTROLLER_BUTTON_PADDLE2); /* L4 */
        pad[NES_ACTION_LOAD_STATE][0] = button(SDL_CONTROLLER_BUTTON_PADDLE1); /* R4 */
        pad[NES_ACTION_FAST_FORWARD][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    }
}

std::string nes_binding_to_config(const NesBinding &b)
{
    char text[32];
    switch (b.kind) {
    case NES_BIND_KEY:
        if (b.mods) snprintf(text, sizeof text, "key:%d:%d", b.code, b.mods);
        else snprintf(text, sizeof text, "key:%d", b.code);
        break;
    case NES_BIND_BUTTON: snprintf(text, sizeof text, "button:%d", b.code); break;
    case NES_BIND_AXIS_POSITIVE: snprintf(text, sizeof text, "axis:%d:+", b.code); break;
    case NES_BIND_AXIS_NEGATIVE: snprintf(text, sizeof text, "axis:%d:-", b.code); break;
    default: snprintf(text, sizeof text, "none"); break;
    }
    return text;
}

bool nes_binding_from_config(const char *text, NesBinding &out)
{
    NesBinding b = { NES_BIND_NONE, 0, 0 };
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

uint8_t nes_mods_from_sdl(unsigned m)
{
    return (uint8_t)(((m & KMOD_CTRL) ? NES_MOD_CTRL : 0) | ((m & KMOD_SHIFT) ? NES_MOD_SHIFT : 0) |
                     ((m & KMOD_ALT) ? NES_MOD_ALT : 0) | ((m & KMOD_GUI) ? NES_MOD_GUI : 0));
}

bool nes_scancode_is_modifier(int scancode)
{
    return scancode >= SDL_SCANCODE_LCTRL && scancode <= SDL_SCANCODE_RGUI;
}

bool nes_key_mods_match(const NesBinding &b, uint8_t held)
{
    if (b.kind != NES_BIND_KEY) return false;
    if (nes_scancode_is_modifier(b.code)) return true; /* a modifier bound on its own */
    if (!b.mods) return (held & (NES_MOD_CTRL | NES_MOD_ALT | NES_MOD_GUI)) == 0;
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

NesPadProfile nes_pad_profile(SDL_GameController *pad, NesInputPlatform platform)
{
    if (pad) {
        const char *name = SDL_GameControllerName(pad);
        /* Under Steam the Deck's controls are a virtual gamepad. */
        if (contains(name, "steam")) return NES_PAD_STEAM_DECK;
        switch (SDL_GameControllerGetType(pad)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE: return platform == NES_PLATFORM_STEAM_DECK ? NES_PAD_STEAM_DECK : NES_PAD_XBOX;
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5: return NES_PAD_PLAYSTATION;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return NES_PAD_NINTENDO;
        default: break;
        }
        if (contains(name, "xbox") || contains(name, "xinput")) return NES_PAD_XBOX;
        if (contains(name, "playstation") || contains(name, "dualshock") || contains(name, "dualsense")) return NES_PAD_PLAYSTATION;
        if (contains(name, "nintendo") || contains(name, "switch") || contains(name, "joy-con")) return NES_PAD_NINTENDO;
    }
    return platform == NES_PLATFORM_STEAM_DECK ? NES_PAD_STEAM_DECK : NES_PAD_GENERIC;
}

/* Names per NesPadProfile: generic, Xbox, PlayStation, Nintendo, Steam Deck. */
static const char *pad_name(const char *const names[5], NesPadProfile pad) { return names[pad]; }

static std::string button_label(int b, NesPadProfile pad)
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
    case SDL_CONTROLLER_BUTTON_MISC1: return pad == NES_PAD_STEAM_DECK ? "Quick Access" : "Share / Capture";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad Click";
    default: return "Button " + std::to_string(b);
    }
}

static std::string axis_label(int a, bool positive, NesPadProfile pad)
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
static std::string key_name(int scancode, NesInputPlatform platform)
{
    if (platform == NES_PLATFORM_MAC) {
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
        case SDL_SCANCODE_LGUI: return platform == NES_PLATFORM_WINDOWS ? "Windows" : "Super";
        case SDL_SCANCODE_RGUI: return platform == NES_PLATFORM_WINDOWS ? "Right Windows" : "Right Super";
        default: break;
        }
    }
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return name && name[0] ? name : "Key " + std::to_string(scancode);
}

std::string nes_binding_label(const NesBinding &b, NesInputPlatform platform, NesPadProfile pad)
{
    switch (b.kind) {
    case NES_BIND_KEY: {
        std::string text;
        const bool mac = platform == NES_PLATFORM_MAC;
        if (b.mods & NES_MOD_CTRL) text += mac ? "Control+" : "Ctrl+";
        if (b.mods & NES_MOD_ALT) text += mac ? "Option+" : "Alt+";
        if (b.mods & NES_MOD_SHIFT) text += "Shift+";
        if (b.mods & NES_MOD_GUI) text += mac ? "Cmd+" : platform == NES_PLATFORM_WINDOWS ? "Win+" : "Super+";
        return text + key_name(b.code, platform);
    }
    case NES_BIND_BUTTON: return button_label(b.code, pad);
    case NES_BIND_AXIS_POSITIVE: return axis_label(b.code, true, pad);
    case NES_BIND_AXIS_NEGATIVE: return axis_label(b.code, false, pad);
    default: return "Unbound";
    }
}

const char *nes_menu_hint(NesInputPlatform platform)
{
    switch (platform) {
    case NES_PLATFORM_MAC: return "Menu: Escape or Cmd+, / controller Guide or L3";
    case NES_PLATFORM_STEAM_DECK: return "Menu: L3 (click the left stick) / Escape on a keyboard. Steam keeps the Steam button.";
    default: return "Menu: Escape or F10 / controller Guide or L3";
    }
}
