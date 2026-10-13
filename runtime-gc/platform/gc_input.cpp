/* Input bindings for the GameCube frontend (see gc_input.h). Adapted from
 * the SNES frontend's snes_input.cpp. */
#include "gc_input.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_pad.h"

static const struct {
    const char *label;
    const char *config;
    uint16_t pad;
} k_actions[GC_ACTION_COUNT] = {
    { "D-Pad Up", "up", GC_PAD_UP },
    { "D-Pad Down", "down", GC_PAD_DOWN },
    { "D-Pad Left", "left", GC_PAD_LEFT },
    { "D-Pad Right", "right", GC_PAD_RIGHT },
    { "A", "a", GC_PAD_A },
    { "B", "b", GC_PAD_B },
    { "X", "x", GC_PAD_X },
    { "Y", "y", GC_PAD_Y },
    { "Z", "z", GC_PAD_Z },
    { "L", "l", GC_PAD_L },
    { "R", "r", GC_PAD_R },
    { "Start", "start", GC_PAD_START },
    { "Stick Up", "stick_up", 0 },
    { "Stick Down", "stick_down", 0 },
    { "Stick Left", "stick_left", 0 },
    { "Stick Right", "stick_right", 0 },
    { "C-Stick Up", "c_up", 0 },
    { "C-Stick Down", "c_down", 0 },
    { "C-Stick Left", "c_left", 0 },
    { "C-Stick Right", "c_right", 0 },
    { "Fast Forward (Hold)", "fast_forward", 0 },
    { "Toggle Overlay", "toggle_overlay", 0 },
    { "Toggle Mute", "toggle_mute", 0 },
    { "Toggle Menu", "toggle_menu", 0 },
    { "Toggle Fullscreen", "toggle_fullscreen", 0 },
    { "Screenshot", "screenshot", 0 },
    { "Save State", "save_state", 0 },
    { "Load State", "load_state", 0 },
    { "Previous State Slot", "previous_state_slot", 0 },
    { "Next State Slot", "next_state_slot", 0 },
    { "Toggle FPS Counter", "toggle_fps", 0 },
    { "Minus (-)", "wii_minus", 0 },
    { "Home", "wii_home", 0 },
    { "Shake", "wii_shake", 0 },
};

const char *gc_action_label(int action) { return k_actions[action].label; }
const char *gc_action_config(int action) { return k_actions[action].config; }
uint16_t gc_action_pad_bit(int action)
{
    return action < GC_ACTION_GAMEPLAY_COUNT ? k_actions[action].pad : 0;
}

GcInputPlatform gc_input_host_platform(void)
{
#if defined(__APPLE__)
    return GC_PLATFORM_MAC;
#elif defined(_WIN32)
    return GC_PLATFORM_WINDOWS;
#else
    return GC_PLATFORM_STEAM_DECK;
#endif
}

static GcBinding key(int scancode, uint8_t mods = 0)
{
    GcBinding b = { GC_BIND_KEY, mods, (int16_t)scancode };
    return b;
}
static GcBinding button(int b)
{
    GcBinding r = { GC_BIND_BUTTON, 0, (int16_t)b };
    return r;
}
static GcBinding axis(int a, bool positive)
{
    GcBinding r = { (uint8_t)(positive ? GC_BIND_AXIS_POSITIVE : GC_BIND_AXIS_NEGATIVE), 0, (int16_t)a };
    return r;
}

void gc_input_defaults(GcInputMap &map, GcInputPlatform platform)
{
    memset(&map, 0, sizeof map);
    GcBinding(*kb)[2] = map.keyboard;
    GcBinding(*pad)[2] = map.controller;

    /* The keyboard, and the one decision worth explaining: the arrow keys
     * are the *stick*, not the d-pad. A GameCube game is steered with the
     * stick and uses the d-pad for menus and shortcuts, so a keyboard whose
     * most obvious keys pressed the d-pad would leave most games unplayable
     * in the least obvious way - the player would press up and nothing much
     * would happen. The d-pad is on the number pad and on IJKL. */
    kb[GC_ACTION_STICK_UP][0] = key(SDL_SCANCODE_UP);
    kb[GC_ACTION_STICK_UP][1] = key(SDL_SCANCODE_W);
    kb[GC_ACTION_STICK_DOWN][0] = key(SDL_SCANCODE_DOWN);
    kb[GC_ACTION_STICK_DOWN][1] = key(SDL_SCANCODE_S);
    kb[GC_ACTION_STICK_LEFT][0] = key(SDL_SCANCODE_LEFT);
    kb[GC_ACTION_STICK_LEFT][1] = key(SDL_SCANCODE_A);
    kb[GC_ACTION_STICK_RIGHT][0] = key(SDL_SCANCODE_RIGHT);
    kb[GC_ACTION_STICK_RIGHT][1] = key(SDL_SCANCODE_D);

    kb[GC_ACTION_UP][0] = key(SDL_SCANCODE_I);
    kb[GC_ACTION_DOWN][0] = key(SDL_SCANCODE_K);
    kb[GC_ACTION_LEFT][0] = key(SDL_SCANCODE_J);
    kb[GC_ACTION_RIGHT][0] = key(SDL_SCANCODE_L);

    /* The C-stick, on the number pad, where its four directions sit in the
     * same arrangement they do on the controller. */
    kb[GC_ACTION_C_UP][0] = key(SDL_SCANCODE_KP_8);
    kb[GC_ACTION_C_DOWN][0] = key(SDL_SCANCODE_KP_5);
    kb[GC_ACTION_C_DOWN][1] = key(SDL_SCANCODE_KP_2);
    kb[GC_ACTION_C_LEFT][0] = key(SDL_SCANCODE_KP_4);
    kb[GC_ACTION_C_RIGHT][0] = key(SDL_SCANCODE_KP_6);

    /* The face buttons, in the positions they occupy on the controller: A in
     * the middle with B below-left, and X and Y around them. */
    kb[GC_ACTION_A][0] = key(SDL_SCANCODE_X);
    kb[GC_ACTION_B][0] = key(SDL_SCANCODE_Z);
    kb[GC_ACTION_X][0] = key(SDL_SCANCODE_C);
    kb[GC_ACTION_Y][0] = key(SDL_SCANCODE_V);
    kb[GC_ACTION_L][0] = key(SDL_SCANCODE_Q);
    kb[GC_ACTION_R][0] = key(SDL_SCANCODE_E);
    kb[GC_ACTION_Z][0] = key(SDL_SCANCODE_R);
    kb[GC_ACTION_START][0] = key(SDL_SCANCODE_RETURN);
    kb[GC_ACTION_FAST_FORWARD][0] = key(SDL_SCANCODE_TAB);
    /* A Wii Remote's own: minus on the key that says so, Home on Home, and a
     * shake on the space bar, which is the biggest thing to hit. */
    kb[GC_ACTION_WII_MINUS][0] = key(SDL_SCANCODE_MINUS);
    kb[GC_ACTION_WII_HOME][0] = key(SDL_SCANCODE_HOME);
    kb[GC_ACTION_WII_SHAKE][0] = key(SDL_SCANCODE_SPACE);
    kb[GC_ACTION_MUTE][0] = key(SDL_SCANCODE_M);

    if (platform == GC_PLATFORM_MAC) {
        kb[GC_ACTION_SAVE_STATE][0] = key(SDL_SCANCODE_S, GC_MOD_GUI);
        kb[GC_ACTION_LOAD_STATE][0] = key(SDL_SCANCODE_L, GC_MOD_GUI);
        kb[GC_ACTION_PREVIOUS_SLOT][0] = key(SDL_SCANCODE_LEFTBRACKET, GC_MOD_GUI);
        kb[GC_ACTION_NEXT_SLOT][0] = key(SDL_SCANCODE_RIGHTBRACKET, GC_MOD_GUI);
        kb[GC_ACTION_OVERLAY][0] = key(SDL_SCANCODE_I, GC_MOD_GUI);
        kb[GC_ACTION_FPS][0] = key(SDL_SCANCODE_I, GC_MOD_GUI | GC_MOD_SHIFT);
        kb[GC_ACTION_MENU][0] = key(SDL_SCANCODE_COMMA, GC_MOD_GUI); /* the Mac Settings shortcut */
        kb[GC_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F, GC_MOD_GUI);
        kb[GC_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_P, GC_MOD_GUI);
    } else {
        /* The GBA and SNES frontends' keys, so a player's hands already know
         * them: F5 saves, F8 loads, F6 and F7 step through the slots. */
        kb[GC_ACTION_SAVE_STATE][0] = key(SDL_SCANCODE_F5);
        kb[GC_ACTION_LOAD_STATE][0] = key(SDL_SCANCODE_F8);
        kb[GC_ACTION_PREVIOUS_SLOT][0] = key(SDL_SCANCODE_F6);
        kb[GC_ACTION_NEXT_SLOT][0] = key(SDL_SCANCODE_F7);
        kb[GC_ACTION_OVERLAY][0] = key(SDL_SCANCODE_F1);
        kb[GC_ACTION_FPS][0] = key(SDL_SCANCODE_F9);
        kb[GC_ACTION_MENU][0] = key(SDL_SCANCODE_F10);
        kb[GC_ACTION_FULLSCREEN][0] = key(SDL_SCANCODE_F11);
        kb[GC_ACTION_SCREENSHOT][0] = key(SDL_SCANCODE_F12);
    }

    /* The controller. The sticks and triggers are not here: a host stick is
     * a GameCube stick and there is nothing to choose. What is here is the
     * digital layout, in the Nintendo positions - east = A, south = B - so
     * that the button under the thumb is the button the game calls A. */
    pad[GC_ACTION_UP][0] = button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    pad[GC_ACTION_DOWN][0] = button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad[GC_ACTION_LEFT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    pad[GC_ACTION_RIGHT][0] = button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    pad[GC_ACTION_A][0] = button(SDL_CONTROLLER_BUTTON_B); /* east */
    pad[GC_ACTION_B][0] = button(SDL_CONTROLLER_BUTTON_A); /* south */
    pad[GC_ACTION_X][0] = button(SDL_CONTROLLER_BUTTON_Y); /* north */
    pad[GC_ACTION_Y][0] = button(SDL_CONTROLLER_BUTTON_X); /* west */
    /* L and R are the shoulder *buttons* - the click at the bottom of the
     * GameCube's trigger travel. The analogue part of the same trigger is
     * read straight from the host's trigger axis, so pressing a host trigger
     * all the way gives both, which is what the hardware does. */
    pad[GC_ACTION_L][0] = button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    pad[GC_ACTION_R][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    /* Z has no natural home on a modern controller: it is a shoulder button
     * on a pad that has only two, so it goes on the right stick's click,
     * which nothing else uses. */
    pad[GC_ACTION_Z][0] = button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    pad[GC_ACTION_START][0] = button(SDL_CONTROLLER_BUTTON_START);
    /* Minus is the button opposite Start, as it is on the remote. Home has
     * no controller default: Guide belongs to whatever overlay is running. */
    pad[GC_ACTION_WII_MINUS][0] = button(SDL_CONTROLLER_BUTTON_BACK);
    /* Guide also opens the menu, but Steam, Xbox Game Bar and similar
     * overlays often take it, so L3 does too. */
    pad[GC_ACTION_MENU][0] = button(SDL_CONTROLLER_BUTTON_LEFTSTICK);

    if (platform == GC_PLATFORM_STEAM_DECK) {
        /* No keyboard on a Deck, so fast forward needs a button. The paddles
         * are the only controls nothing else wants. */
        pad[GC_ACTION_FAST_FORWARD][0] = button(SDL_CONTROLLER_BUTTON_PADDLE1); /* R4 */
        /* The GBA frontend puts save states on X and Y, which a GameCube
         * game needs for itself; the other paddles are free. */
        pad[GC_ACTION_SAVE_STATE][0] = button(SDL_CONTROLLER_BUTTON_PADDLE2); /* L4 */
        pad[GC_ACTION_LOAD_STATE][0] = button(SDL_CONTROLLER_BUTTON_PADDLE4); /* L5 */
        pad[GC_ACTION_NEXT_SLOT][0] = button(SDL_CONTROLLER_BUTTON_PADDLE3);  /* R5 */
    }
}

std::string gc_binding_to_config(const GcBinding &b)
{
    char text[32];
    switch (b.kind) {
    case GC_BIND_KEY:
        if (b.mods) snprintf(text, sizeof text, "key:%d:%d", b.code, b.mods);
        else snprintf(text, sizeof text, "key:%d", b.code);
        break;
    case GC_BIND_BUTTON: snprintf(text, sizeof text, "button:%d", b.code); break;
    case GC_BIND_AXIS_POSITIVE: snprintf(text, sizeof text, "axis:%d:+", b.code); break;
    case GC_BIND_AXIS_NEGATIVE: snprintf(text, sizeof text, "axis:%d:-", b.code); break;
    default: snprintf(text, sizeof text, "none"); break;
    }
    return text;
}

bool gc_binding_from_config(const char *text, GcBinding &out)
{
    GcBinding b = { GC_BIND_NONE, 0, 0 };
    char *end = NULL;
    if (!strcmp(text, "none")) {
        out = b;
        return true;
    }
    if (!strncmp(text, "key:", 4)) {
        long code = strtol(text + 4, &end, 10);
        long mods = 0;
        if (end && *end == ':') mods = strtol(end + 1, &end, 10);
        if (end == text + 4 || code <= SDL_SCANCODE_UNKNOWN || code >= SDL_NUM_SCANCODES || mods < 0 || mods > 15)
            return false;
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

uint8_t gc_mods_from_sdl(unsigned m)
{
    return (uint8_t)(((m & KMOD_CTRL) ? GC_MOD_CTRL : 0) | ((m & KMOD_SHIFT) ? GC_MOD_SHIFT : 0) |
                     ((m & KMOD_ALT) ? GC_MOD_ALT : 0) | ((m & KMOD_GUI) ? GC_MOD_GUI : 0));
}

bool gc_scancode_is_modifier(int scancode)
{
    return scancode >= SDL_SCANCODE_LCTRL && scancode <= SDL_SCANCODE_RGUI;
}

bool gc_key_mods_match(const GcBinding &b, uint8_t held)
{
    if (b.kind != GC_BIND_KEY) return false;
    if (gc_scancode_is_modifier(b.code)) return true; /* a modifier bound on its own */
    if (!b.mods) return (held & (GC_MOD_CTRL | GC_MOD_ALT | GC_MOD_GUI)) == 0;
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

GcPadProfile gc_pad_profile(SDL_GameController *pad, GcInputPlatform platform)
{
    if (pad) {
        const char *name = SDL_GameControllerName(pad);
        /* Under Steam the Deck's controls are a virtual gamepad. */
        if (contains(name, "steam")) return GC_PAD_STEAM_DECK;
        switch (SDL_GameControllerGetType(pad)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE:
            return platform == GC_PLATFORM_STEAM_DECK ? GC_PAD_STEAM_DECK : GC_PAD_XBOX;
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5: return GC_PAD_PLAYSTATION;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return GC_PAD_NINTENDO;
        default: break;
        }
        if (contains(name, "xbox") || contains(name, "xinput")) return GC_PAD_XBOX;
        if (contains(name, "playstation") || contains(name, "dualshock") || contains(name, "dualsense"))
            return GC_PAD_PLAYSTATION;
        if (contains(name, "nintendo") || contains(name, "switch") || contains(name, "joy-con")) return GC_PAD_NINTENDO;
    }
    return platform == GC_PLATFORM_STEAM_DECK ? GC_PAD_STEAM_DECK : GC_PAD_GENERIC;
}

/* Names per GcPadProfile: generic, Xbox, PlayStation, Nintendo, Steam Deck. */
static const char *pad_name(const char *const names[5], GcPadProfile pad) { return names[pad]; }

static std::string button_label(int b, GcPadProfile pad)
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
    case SDL_CONTROLLER_BUTTON_MISC1: return pad == GC_PAD_STEAM_DECK ? "Quick Access" : "Share / Capture";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad Click";
    default: return "Button " + std::to_string(b);
    }
}

static std::string axis_label(int a, bool positive, GcPadProfile pad)
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
static std::string key_name(int scancode, GcInputPlatform platform)
{
    if (platform == GC_PLATFORM_MAC) {
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
        case SDL_SCANCODE_LGUI: return platform == GC_PLATFORM_WINDOWS ? "Windows" : "Super";
        case SDL_SCANCODE_RGUI: return platform == GC_PLATFORM_WINDOWS ? "Right Windows" : "Right Super";
        default: break;
        }
    }
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return name && name[0] ? name : "Key " + std::to_string(scancode);
}

std::string gc_binding_label(const GcBinding &b, GcInputPlatform platform, GcPadProfile pad)
{
    switch (b.kind) {
    case GC_BIND_KEY: {
        std::string text;
        const bool mac = platform == GC_PLATFORM_MAC;
        if (b.mods & GC_MOD_CTRL) text += mac ? "Control+" : "Ctrl+";
        if (b.mods & GC_MOD_ALT) text += mac ? "Option+" : "Alt+";
        if (b.mods & GC_MOD_SHIFT) text += "Shift+";
        if (b.mods & GC_MOD_GUI) text += mac ? "Cmd+" : platform == GC_PLATFORM_WINDOWS ? "Win+" : "Super+";
        return text + key_name(b.code, platform);
    }
    case GC_BIND_BUTTON: return button_label(b.code, pad);
    case GC_BIND_AXIS_POSITIVE: return axis_label(b.code, true, pad);
    case GC_BIND_AXIS_NEGATIVE: return axis_label(b.code, false, pad);
    default: return "Unbound";
    }
}

const char *gc_menu_hint(GcInputPlatform platform)
{
    switch (platform) {
    case GC_PLATFORM_MAC: return "Menu: Escape or Cmd+, / controller Guide or L3";
    case GC_PLATFORM_STEAM_DECK:
        return "Menu: L3 (click the left stick) / Escape on a keyboard. Steam keeps the Steam button.";
    default: return "Menu: Escape or F10 / controller Guide or L3";
    }
}
