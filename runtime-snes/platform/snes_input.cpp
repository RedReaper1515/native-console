/* Input bindings for the SNES frontend (see snes_input.h). Adapted from the
 * Game Boy Advance frontend's gba_input.cpp. */
#include "snes_input.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes.h"

static const struct {
    const char *label;
    const char *config;
    uint16_t keypad;
} k_actions[SNES_ACTION_COUNT] = {
    { "Up", "up", SNES_BTN_UP },
    { "Down", "down", SNES_BTN_DOWN },
    { "Left", "left", SNES_BTN_LEFT },
    { "Right", "right", SNES_BTN_RIGHT },
    { "A", "a", SNES_BTN_A },
    { "B", "b", SNES_BTN_B },
    { "X", "x", SNES_BTN_X },
    { "Y", "y", SNES_BTN_Y },
    { "L", "l", SNES_BTN_L },
    { "R", "r", SNES_BTN_R },
    { "Start", "start", SNES_BTN_START },
    { "Select", "select", SNES_BTN_SELECT },
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

const char *snes_action_label(int action) { return k_actions[action].label; }
const char *snes_action_config(int action) { return k_actions[action].config; }
uint16_t snes_action_keypad_bit(int action) { return action < SNES_ACTION_GAMEPLAY_COUNT ? k_actions[action].keypad : 0; }

SnesInputPlatform snes_input_host_platform(void)
{
#if defined(__APPLE__)
    return SNES_PLATFORM_MAC;
#elif defined(_WIN32)
    return SNES_PLATFORM_WINDOWS;
#else
    return SNES_PLATFORM_STEAM_DECK;
#endif
}

static SnesBinding key(int scancode, uint8_t mods = 0)
{
    SnesBinding b = { SNES_BIND_KEY, mods, (int16_t)scancode };
    return b;
}
static SnesBinding button(int b)
{
    SnesBinding r = { SNES_BIND_BUTTON, 0, (int16_t)b };
    return r;
}
static SnesBinding axis(int a, bool positive)
{
    SnesBinding r = { (uint8_t)(positive ? SNES_BIND_AXIS_POSITIVE : SNES_BIND_AXIS_NEGATIVE), 0, (int16_t)a };
    return r;
}

void snes_input_defaults(SnesInputMap &map, SnesInputPlatform platform)
{
    memset(&map, 0, sizeof map);
    SnesBinding(*kb)[2] = map.keyboard;
    SnesBinding(*pad)[2] = map.controller;

    /* Gameplay: the same on every keyboard. */
    kb[SNES_ACTION_UP][0] = key(SDL_SCANCODE_UP);
    kb[SNES_ACTION_DOWN][0] = key(SDL_SCANCODE_DOWN);
    kb[SNES_ACTION_LEFT][0] = key(SDL_SCANCODE_LEFT);
    kb[SNES_ACTION_RIGHT][0] = key(SDL_SCANCODE_RIGHT);
    /* Face buttons in the SNES diamond: X above A, Y left of B. */
    kb[SNES_ACTION_A][0] = key(SDL_SCANCODE_X);
    kb[SNES_ACTION_A][1] = key(SDL_SCANCODE_L);
    kb[SNES_ACTION_B][0] = key(SDL_SCANCODE_Z);
    kb[SNES_ACTION_B][1] = key(SDL_SCANCODE_K);
    kb[SNES_ACTION_X][0] = key(SDL_SCANCODE_S);
    kb[SNES_ACTION_X][1] = key(SDL_SCANCODE_I);
    kb[SNES_ACTION_Y][0] = key(SDL_SCANCODE_A);
    kb[SNES_ACTION_Y][1] = key(SDL_SCANCODE_J);
    kb[SNES_ACTION_L][0] = key(SDL_SCANCODE_Q);
    kb[SNES_ACTION_R][0] = key(SDL_SCANCODE_W);
    kb[SNES_ACTION_START][0] = key(SDL_SCANCODE_RETURN);
    kb[SNES_ACTION_SELECT][0] = key(SDL_SCANCODE_BACKSPACE);
    kb[SNES_ACTION_SELECT][1] = key(SDL_SCANCODE_RSHIFT);
    kb[SNES_ACTION_FAST_FORWARD][0] = key(SDL_SCANCODE_TAB);
    kb[SNES_ACTION_MUTE][0] = key(SDL_SCANCODE_M);

    if (platform == SNES_PLATFORM_MAC) {
        kb[SNES_ACTION_SAVE_STATE][0] = key(SDL_SCANCODE_S, SNES_MOD_GUI);
        kb[SNES_ACTION_LOAD_STATE][0] = key(SDL_SCANCODE_L, SNES_MOD_GUI);
        kb[SNES_ACTION_PREVIOUS_SLOT][0] = key(SDL_SCANCODE_LEFTBRACKET, SNES_MOD_GUI);
        kb[SNES_ACTION_NEXT_SLOT][0] = key(SDL_SCANCODE_RIGHTBRACKET, SNES_MOD_GUI);
        kb[SNES_ACTION_OVERLAY][0] = key(SDL_SCANCODE_I, SNES_MOD_GUI);
        kb[SNES_ACTION_FPS][0] = key(SDL_SCANCODE_I, SNES_MOD_GUI | SNES_MOD_SHIFT);
        kb[SNES_ACTION_MENU][0] = key(SDL_SCANCODE_COMMA, SNES_MOD_GUI); /* the Mac Settings shortcut */
        kb[SNES_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F, SNES_MOD_GUI);
        kb[SNES_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_P, SNES_MOD_GUI);
    } else {
        kb[SNES_ACTION_SAVE_STATE][0] = key(SDL_SCANCODE_F5);
        kb[SNES_ACTION_LOAD_STATE][0] = key(SDL_SCANCODE_F8);
        kb[SNES_ACTION_PREVIOUS_SLOT][0] = key(SDL_SCANCODE_F6);
        kb[SNES_ACTION_NEXT_SLOT][0] = key(SDL_SCANCODE_F7);
        kb[SNES_ACTION_OVERLAY][0] = key(SDL_SCANCODE_F1);
        kb[SNES_ACTION_FPS][0] = key(SDL_SCANCODE_F9);
        kb[SNES_ACTION_MENU][0] = key(SDL_SCANCODE_F10);
        kb[SNES_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F11);
        kb[SNES_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_F12);
    }

    /* Controller gameplay: the same on every controller. */
    pad[SNES_ACTION_UP][0] = button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    pad[SNES_ACTION_UP][1] = axis(SDL_CONTROLLER_AXIS_LEFTY, false);
    pad[SNES_ACTION_DOWN][0] = button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad[SNES_ACTION_DOWN][1] = axis(SDL_CONTROLLER_AXIS_LEFTY, true);
    pad[SNES_ACTION_LEFT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    pad[SNES_ACTION_LEFT][1] = axis(SDL_CONTROLLER_AXIS_LEFTX, false);
    pad[SNES_ACTION_RIGHT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    pad[SNES_ACTION_RIGHT][1] = axis(SDL_CONTROLLER_AXIS_LEFTX, true);
    pad[SNES_ACTION_A][0] = button(SDL_CONTROLLER_BUTTON_B); /* east */
    pad[SNES_ACTION_B][0] = button(SDL_CONTROLLER_BUTTON_A); /* south */
    pad[SNES_ACTION_X][0] = button(SDL_CONTROLLER_BUTTON_Y); /* north */
    pad[SNES_ACTION_Y][0] = button(SDL_CONTROLLER_BUTTON_X); /* west */
    pad[SNES_ACTION_L][0] = button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    pad[SNES_ACTION_L][1] = axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true);
    pad[SNES_ACTION_R][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    pad[SNES_ACTION_R][1] = axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true);
    pad[SNES_ACTION_START][0] = button(SDL_CONTROLLER_BUTTON_START);
    pad[SNES_ACTION_SELECT][0] = button(SDL_CONTROLLER_BUTTON_BACK);
    /* Guide also opens the menu, but Steam, Xbox Game Bar and similar
     * overlays often take it, so L3 does too. */
    pad[SNES_ACTION_MENU][0] = button(SDL_CONTROLLER_BUTTON_LEFTSTICK);

    if (platform == SNES_PLATFORM_STEAM_DECK) {
        /* No keyboard on a Deck: save states and fast forward on buttons. */
        pad[SNES_ACTION_SAVE_STATE][0] = button(SDL_CONTROLLER_BUTTON_PADDLE2); /* L4 */
        pad[SNES_ACTION_LOAD_STATE][0] = button(SDL_CONTROLLER_BUTTON_PADDLE1); /* R4 */
        pad[SNES_ACTION_FAST_FORWARD][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    }
}

std::string snes_binding_to_config(const SnesBinding &b)
{
    char text[32];
    switch (b.kind) {
    case SNES_BIND_KEY:
        if (b.mods) snprintf(text, sizeof text, "key:%d:%d", b.code, b.mods);
        else snprintf(text, sizeof text, "key:%d", b.code);
        break;
    case SNES_BIND_BUTTON: snprintf(text, sizeof text, "button:%d", b.code); break;
    case SNES_BIND_AXIS_POSITIVE: snprintf(text, sizeof text, "axis:%d:+", b.code); break;
    case SNES_BIND_AXIS_NEGATIVE: snprintf(text, sizeof text, "axis:%d:-", b.code); break;
    default: snprintf(text, sizeof text, "none"); break;
    }
    return text;
}

bool snes_binding_from_config(const char *text, SnesBinding &out)
{
    SnesBinding b = { SNES_BIND_NONE, 0, 0 };
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

uint8_t snes_mods_from_sdl(unsigned m)
{
    return (uint8_t)(((m & KMOD_CTRL) ? SNES_MOD_CTRL : 0) | ((m & KMOD_SHIFT) ? SNES_MOD_SHIFT : 0) |
                     ((m & KMOD_ALT) ? SNES_MOD_ALT : 0) | ((m & KMOD_GUI) ? SNES_MOD_GUI : 0));
}

bool snes_scancode_is_modifier(int scancode)
{
    return scancode >= SDL_SCANCODE_LCTRL && scancode <= SDL_SCANCODE_RGUI;
}

bool snes_key_mods_match(const SnesBinding &b, uint8_t held)
{
    if (b.kind != SNES_BIND_KEY) return false;
    if (snes_scancode_is_modifier(b.code)) return true; /* a modifier bound on its own */
    if (!b.mods) return (held & (SNES_MOD_CTRL | SNES_MOD_ALT | SNES_MOD_GUI)) == 0;
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

SnesPadProfile snes_pad_profile(SDL_GameController *pad, SnesInputPlatform platform)
{
    if (pad) {
        const char *name = SDL_GameControllerName(pad);
        /* Under Steam the Deck's controls are a virtual gamepad. */
        if (contains(name, "steam")) return SNES_PAD_STEAM_DECK;
        switch (SDL_GameControllerGetType(pad)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE: return platform == SNES_PLATFORM_STEAM_DECK ? SNES_PAD_STEAM_DECK : SNES_PAD_XBOX;
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5: return SNES_PAD_PLAYSTATION;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return SNES_PAD_NINTENDO;
        default: break;
        }
        if (contains(name, "xbox") || contains(name, "xinput")) return SNES_PAD_XBOX;
        if (contains(name, "playstation") || contains(name, "dualshock") || contains(name, "dualsense")) return SNES_PAD_PLAYSTATION;
        if (contains(name, "nintendo") || contains(name, "switch") || contains(name, "joy-con")) return SNES_PAD_NINTENDO;
    }
    return platform == SNES_PLATFORM_STEAM_DECK ? SNES_PAD_STEAM_DECK : SNES_PAD_GENERIC;
}

/* Names per SnesPadProfile: generic, Xbox, PlayStation, Nintendo, Steam Deck. */
static const char *pad_name(const char *const names[5], SnesPadProfile pad) { return names[pad]; }

static std::string button_label(int b, SnesPadProfile pad)
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
    case SDL_CONTROLLER_BUTTON_MISC1: return pad == SNES_PAD_STEAM_DECK ? "Quick Access" : "Share / Capture";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad Click";
    default: return "Button " + std::to_string(b);
    }
}

static std::string axis_label(int a, bool positive, SnesPadProfile pad)
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
static std::string key_name(int scancode, SnesInputPlatform platform)
{
    if (platform == SNES_PLATFORM_MAC) {
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
        case SDL_SCANCODE_LGUI: return platform == SNES_PLATFORM_WINDOWS ? "Windows" : "Super";
        case SDL_SCANCODE_RGUI: return platform == SNES_PLATFORM_WINDOWS ? "Right Windows" : "Right Super";
        default: break;
        }
    }
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return name && name[0] ? name : "Key " + std::to_string(scancode);
}

std::string snes_binding_label(const SnesBinding &b, SnesInputPlatform platform, SnesPadProfile pad)
{
    switch (b.kind) {
    case SNES_BIND_KEY: {
        std::string text;
        const bool mac = platform == SNES_PLATFORM_MAC;
        if (b.mods & SNES_MOD_CTRL) text += mac ? "Control+" : "Ctrl+";
        if (b.mods & SNES_MOD_ALT) text += mac ? "Option+" : "Alt+";
        if (b.mods & SNES_MOD_SHIFT) text += "Shift+";
        if (b.mods & SNES_MOD_GUI) text += mac ? "Cmd+" : platform == SNES_PLATFORM_WINDOWS ? "Win+" : "Super+";
        return text + key_name(b.code, platform);
    }
    case SNES_BIND_BUTTON: return button_label(b.code, pad);
    case SNES_BIND_AXIS_POSITIVE: return axis_label(b.code, true, pad);
    case SNES_BIND_AXIS_NEGATIVE: return axis_label(b.code, false, pad);
    default: return "Unbound";
    }
}

const char *snes_menu_hint(SnesInputPlatform platform)
{
    switch (platform) {
    case SNES_PLATFORM_MAC: return "Menu: Escape or Cmd+, / controller Guide or L3";
    case SNES_PLATFORM_STEAM_DECK: return "Menu: L3 (click the left stick) / Escape on a keyboard. Steam keeps the Steam button.";
    default: return "Menu: Escape or F10 / controller Guide or L3";
    }
}
