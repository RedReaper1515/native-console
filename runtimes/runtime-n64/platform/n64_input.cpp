/* Input bindings for the Nintendo 64 frontend (see n64_input.h). Adapted
 * from the SNES frontend's snes_input.cpp. */
#include "n64_input.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n64.h"

static const struct {
    const char *label;
    const char *config;
    uint16_t button;
} k_actions[N64_ACTION_COUNT] = {
    { "Stick Up", "stick_up", 0 },
    { "Stick Down", "stick_down", 0 },
    { "Stick Left", "stick_left", 0 },
    { "Stick Right", "stick_right", 0 },
    { "A", "a", N64_BTN_A },
    { "B", "b", N64_BTN_B },
    { "Z", "z", N64_BTN_Z },
    { "Start", "start", N64_BTN_START },
    { "L", "l", N64_BTN_L },
    { "R", "r", N64_BTN_R },
    { "D-Pad Up", "dpad_up", N64_BTN_DUP },
    { "D-Pad Down", "dpad_down", N64_BTN_DDOWN },
    { "D-Pad Left", "dpad_left", N64_BTN_DLEFT },
    { "D-Pad Right", "dpad_right", N64_BTN_DRIGHT },
    { "C-Up", "c_up", N64_BTN_CUP },
    { "C-Down", "c_down", N64_BTN_CDOWN },
    { "C-Left", "c_left", N64_BTN_CLEFT },
    { "C-Right", "c_right", N64_BTN_CRIGHT },
    { "Fast Forward (Hold)", "fast_forward", 0 },
    { "Toggle Overlay", "toggle_overlay", 0 },
    { "Toggle FPS Counter", "toggle_fps", 0 },
    { "Toggle Mute", "toggle_mute", 0 },
    { "Toggle Menu", "toggle_menu", 0 },
    { "Toggle Fullscreen", "toggle_fullscreen", 0 },
    { "Screenshot", "screenshot", 0 },
};

const char *n64_action_label(int action) { return k_actions[action].label; }
const char *n64_action_config(int action) { return k_actions[action].config; }
uint16_t n64_action_button(int action) { return action < N64_ACTION_GAMEPLAY_COUNT ? k_actions[action].button : 0; }
bool n64_action_is_stick(int action) { return action >= N64_ACTION_STICK_UP && action <= N64_ACTION_STICK_RIGHT; }

int n64_stick_from_axis(int magnitude)
{
    if (magnitude <= N64_STICK_DEADZONE) return 0;
    if (magnitude > 32767) magnitude = 32767;
    /* Rounded, so the far edge of the host stick is exactly full deflection. */
    const int range = 32767 - N64_STICK_DEADZONE;
    return ((magnitude - N64_STICK_DEADZONE) * N64_STICK_MAX + range / 2) / range;
}

N64InputPlatform n64_input_host_platform(void)
{
#if defined(__APPLE__)
    return N64_PLATFORM_MAC;
#elif defined(_WIN32)
    return N64_PLATFORM_WINDOWS;
#else
    return N64_PLATFORM_STEAM_DECK;
#endif
}

static N64Binding key(int scancode, uint8_t mods = 0)
{
    N64Binding b = { N64_BIND_KEY, mods, (int16_t)scancode };
    return b;
}
static N64Binding button(int b)
{
    N64Binding r = { N64_BIND_BUTTON, 0, (int16_t)b };
    return r;
}
static N64Binding axis(int a, bool positive)
{
    N64Binding r = { (uint8_t)(positive ? N64_BIND_AXIS_POSITIVE : N64_BIND_AXIS_NEGATIVE), 0, (int16_t)a };
    return r;
}

void n64_input_defaults(N64InputMap &map, N64InputPlatform platform)
{
    memset(&map, 0, sizeof map);
    N64Binding(*kb)[2] = map.keyboard;
    N64Binding(*pad)[2] = map.controller;

    /* Gameplay: the same on every keyboard. The left hand has Z, L and R
     * (Z, A, S), the right hand the C buttons (IJKL) beside A and B (X, C),
     * and the D-pad sits between them (TFGH), clear of both. */
    kb[N64_ACTION_STICK_UP][0] = key(SDL_SCANCODE_UP);
    kb[N64_ACTION_STICK_DOWN][0] = key(SDL_SCANCODE_DOWN);
    kb[N64_ACTION_STICK_LEFT][0] = key(SDL_SCANCODE_LEFT);
    kb[N64_ACTION_STICK_RIGHT][0] = key(SDL_SCANCODE_RIGHT);
    kb[N64_ACTION_A][0] = key(SDL_SCANCODE_X);
    kb[N64_ACTION_B][0] = key(SDL_SCANCODE_C);
    kb[N64_ACTION_Z][0] = key(SDL_SCANCODE_Z);
    kb[N64_ACTION_START][0] = key(SDL_SCANCODE_RETURN);
    kb[N64_ACTION_L][0] = key(SDL_SCANCODE_A);
    kb[N64_ACTION_R][0] = key(SDL_SCANCODE_S);
    kb[N64_ACTION_DPAD_UP][0] = key(SDL_SCANCODE_T);
    kb[N64_ACTION_DPAD_DOWN][0] = key(SDL_SCANCODE_G);
    kb[N64_ACTION_DPAD_LEFT][0] = key(SDL_SCANCODE_F);
    kb[N64_ACTION_DPAD_RIGHT][0] = key(SDL_SCANCODE_H);
    kb[N64_ACTION_C_UP][0] = key(SDL_SCANCODE_I);
    kb[N64_ACTION_C_DOWN][0] = key(SDL_SCANCODE_K);
    kb[N64_ACTION_C_LEFT][0] = key(SDL_SCANCODE_J);
    kb[N64_ACTION_C_RIGHT][0] = key(SDL_SCANCODE_L);
    kb[N64_ACTION_FAST_FORWARD][0] = key(SDL_SCANCODE_TAB);
    kb[N64_ACTION_MUTE][0] = key(SDL_SCANCODE_M);

    if (platform == N64_PLATFORM_MAC) {
        kb[N64_ACTION_OVERLAY][0] = key(SDL_SCANCODE_I, N64_MOD_GUI);
        kb[N64_ACTION_FPS][0] = key(SDL_SCANCODE_I, N64_MOD_GUI | N64_MOD_SHIFT);
        kb[N64_ACTION_MENU][0] = key(SDL_SCANCODE_COMMA, N64_MOD_GUI); /* the Mac Settings shortcut */
        kb[N64_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F, N64_MOD_GUI);
        kb[N64_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_P, N64_MOD_GUI);
    } else {
        kb[N64_ACTION_OVERLAY][0] = key(SDL_SCANCODE_F1);
        kb[N64_ACTION_FPS][0] = key(SDL_SCANCODE_F9);
        kb[N64_ACTION_MENU][0] = key(SDL_SCANCODE_F10);
        kb[N64_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F11);
        kb[N64_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_F12);
    }

    /* Controller gameplay: the same on every controller (n64_input.h). */
    pad[N64_ACTION_STICK_UP][0] = axis(SDL_CONTROLLER_AXIS_LEFTY, false);
    pad[N64_ACTION_STICK_DOWN][0] = axis(SDL_CONTROLLER_AXIS_LEFTY, true);
    pad[N64_ACTION_STICK_LEFT][0] = axis(SDL_CONTROLLER_AXIS_LEFTX, false);
    pad[N64_ACTION_STICK_RIGHT][0] = axis(SDL_CONTROLLER_AXIS_LEFTX, true);
    pad[N64_ACTION_A][0] = button(SDL_CONTROLLER_BUTTON_A); /* south */
    pad[N64_ACTION_B][0] = button(SDL_CONTROLLER_BUTTON_X); /* west */
    pad[N64_ACTION_Z][0] = axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true);
    pad[N64_ACTION_START][0] = button(SDL_CONTROLLER_BUTTON_START);
    pad[N64_ACTION_L][0] = button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    pad[N64_ACTION_R][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    pad[N64_ACTION_R][1] = axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true);
    pad[N64_ACTION_DPAD_UP][0] = button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    pad[N64_ACTION_DPAD_DOWN][0] = button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad[N64_ACTION_DPAD_LEFT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    pad[N64_ACTION_DPAD_RIGHT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    pad[N64_ACTION_C_UP][0] = axis(SDL_CONTROLLER_AXIS_RIGHTY, false);
    pad[N64_ACTION_C_DOWN][0] = axis(SDL_CONTROLLER_AXIS_RIGHTY, true);
    pad[N64_ACTION_C_DOWN][1] = button(SDL_CONTROLLER_BUTTON_B); /* east */
    pad[N64_ACTION_C_LEFT][0] = axis(SDL_CONTROLLER_AXIS_RIGHTX, false);
    pad[N64_ACTION_C_LEFT][1] = button(SDL_CONTROLLER_BUTTON_Y); /* north */
    pad[N64_ACTION_C_RIGHT][0] = axis(SDL_CONTROLLER_AXIS_RIGHTX, true);
    /* Guide also opens the menu, but Steam, Xbox Game Bar and similar
     * overlays often take it, so L3 does too. */
    pad[N64_ACTION_MENU][0] = button(SDL_CONTROLLER_BUTTON_LEFTSTICK);

    if (platform == N64_PLATFORM_STEAM_DECK) {
        /* No keyboard on a Deck: fast forward on a button. */
        pad[N64_ACTION_FAST_FORWARD][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    }
}

std::string n64_binding_to_config(const N64Binding &b)
{
    char text[32];
    switch (b.kind) {
    case N64_BIND_KEY:
        if (b.mods) snprintf(text, sizeof text, "key:%d:%d", b.code, b.mods);
        else snprintf(text, sizeof text, "key:%d", b.code);
        break;
    case N64_BIND_BUTTON: snprintf(text, sizeof text, "button:%d", b.code); break;
    case N64_BIND_AXIS_POSITIVE: snprintf(text, sizeof text, "axis:%d:+", b.code); break;
    case N64_BIND_AXIS_NEGATIVE: snprintf(text, sizeof text, "axis:%d:-", b.code); break;
    default: snprintf(text, sizeof text, "none"); break;
    }
    return text;
}

bool n64_binding_from_config(const char *text, N64Binding &out)
{
    N64Binding b = { N64_BIND_NONE, 0, 0 };
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

uint8_t n64_mods_from_sdl(unsigned m)
{
    return (uint8_t)(((m & KMOD_CTRL) ? N64_MOD_CTRL : 0) | ((m & KMOD_SHIFT) ? N64_MOD_SHIFT : 0) |
                     ((m & KMOD_ALT) ? N64_MOD_ALT : 0) | ((m & KMOD_GUI) ? N64_MOD_GUI : 0));
}

bool n64_scancode_is_modifier(int scancode)
{
    return scancode >= SDL_SCANCODE_LCTRL && scancode <= SDL_SCANCODE_RGUI;
}

bool n64_key_mods_match(const N64Binding &b, uint8_t held)
{
    if (b.kind != N64_BIND_KEY) return false;
    if (n64_scancode_is_modifier(b.code)) return true; /* a modifier bound on its own */
    if (!b.mods) return (held & (N64_MOD_CTRL | N64_MOD_ALT | N64_MOD_GUI)) == 0;
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

N64PadProfile n64_pad_profile(SDL_GameController *pad, N64InputPlatform platform)
{
    if (pad) {
        const char *name = SDL_GameControllerName(pad);
        /* Under Steam the Deck's controls are a virtual gamepad. */
        if (contains(name, "steam")) return N64_PAD_STEAM_DECK;
        switch (SDL_GameControllerGetType(pad)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE: return platform == N64_PLATFORM_STEAM_DECK ? N64_PAD_STEAM_DECK : N64_PAD_XBOX;
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5: return N64_PAD_PLAYSTATION;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return N64_PAD_NINTENDO;
        default: break;
        }
        if (contains(name, "xbox") || contains(name, "xinput")) return N64_PAD_XBOX;
        if (contains(name, "playstation") || contains(name, "dualshock") || contains(name, "dualsense")) return N64_PAD_PLAYSTATION;
        if (contains(name, "nintendo") || contains(name, "switch") || contains(name, "joy-con")) return N64_PAD_NINTENDO;
    }
    return platform == N64_PLATFORM_STEAM_DECK ? N64_PAD_STEAM_DECK : N64_PAD_GENERIC;
}

/* Names per N64PadProfile: generic, Xbox, PlayStation, Nintendo, Steam Deck. */
static const char *pad_name(const char *const names[5], N64PadProfile pad) { return names[pad]; }

static std::string button_label(int b, N64PadProfile pad)
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
    case SDL_CONTROLLER_BUTTON_MISC1: return pad == N64_PAD_STEAM_DECK ? "Quick Access" : "Share / Capture";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad Click";
    default: return "Button " + std::to_string(b);
    }
}

static std::string axis_label(int a, bool positive, N64PadProfile pad)
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
static std::string key_name(int scancode, N64InputPlatform platform)
{
    if (platform == N64_PLATFORM_MAC) {
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
        case SDL_SCANCODE_LGUI: return platform == N64_PLATFORM_WINDOWS ? "Windows" : "Super";
        case SDL_SCANCODE_RGUI: return platform == N64_PLATFORM_WINDOWS ? "Right Windows" : "Right Super";
        default: break;
        }
    }
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return name && name[0] ? name : "Key " + std::to_string(scancode);
}

std::string n64_binding_label(const N64Binding &b, N64InputPlatform platform, N64PadProfile pad)
{
    switch (b.kind) {
    case N64_BIND_KEY: {
        std::string text;
        const bool mac = platform == N64_PLATFORM_MAC;
        if (b.mods & N64_MOD_CTRL) text += mac ? "Control+" : "Ctrl+";
        if (b.mods & N64_MOD_ALT) text += mac ? "Option+" : "Alt+";
        if (b.mods & N64_MOD_SHIFT) text += "Shift+";
        if (b.mods & N64_MOD_GUI) text += mac ? "Cmd+" : platform == N64_PLATFORM_WINDOWS ? "Win+" : "Super+";
        return text + key_name(b.code, platform);
    }
    case N64_BIND_BUTTON: return button_label(b.code, pad);
    case N64_BIND_AXIS_POSITIVE: return axis_label(b.code, true, pad);
    case N64_BIND_AXIS_NEGATIVE: return axis_label(b.code, false, pad);
    default: return "Unbound";
    }
}

const char *n64_menu_hint(N64InputPlatform platform)
{
    switch (platform) {
    case N64_PLATFORM_MAC: return "Menu: Escape or Cmd+, / controller Guide or L3";
    case N64_PLATFORM_STEAM_DECK: return "Menu: L3 (click the left stick) / Escape on a keyboard. Steam keeps the Steam button.";
    default: return "Menu: Escape or F10 / controller Guide or L3";
    }
}
