/*
 * DS frontend input (runtime-nds/platform/nds_input.cpp) and the choice
 * between window and headless runner (nds_sdl_wants_headless):
 *
 * Bindings
 * - Every platform's defaults bind each control to at most one action.
 * - Every DS button has a key and a controller button, the same controller
 *   buttons on every platform, in Nintendo positions.
 * - The DS-only inputs are reachable by default: blow, lid, next layout and
 *   swap screens from the keyboard; the stylus cursor, its button, blow,
 *   swap screens and the menu from a controller alone.
 * - Mac shortcuts use Command, never F-keys; Windows keeps F-keys.
 * - Bindings survive the preference-file format; bad text is refused.
 * - A plain key does not fire while Command/Ctrl is held, and Shift does
 *   not block it.
 *
 * Screens
 * - Each layout puts the screens where this file says, to the pixel, on the
 *   displays the frontend is used on: 3440x1440, 1920x1080, 1920x1200, the
 *   Steam Deck's 1280x800, and a window of the layout's own size.
 * - On any display: both screens are on it, do not overlap, keep their
 *   shape, are whole multiples of 256x192 in Pixel Perfect, and in Large +
 *   small the small screen has at least half the large one's scale (half
 *   rounded down to a whole scale in Pixel Perfect).
 * - A position on the display touches the right bottom-screen pixel in
 *   every layout; every display pixel of the bottom screen's rectangle
 *   touches something, and every DS pixel can be touched (no dead border);
 *   positions off the rectangle come back as its nearest edge pixel.
 *
 * Headless
 * - Every option of the headless runner sends a command line there; the
 *   window's own options, and the ones the two share, do not. Given the
 *   path of runtime-nds/src/nds_runner.c, the runner's options are also
 *   read from its source, so one added there and forgotten in the frontend
 *   fails here.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#define SDL_MAIN_HANDLED /* plain main() */
#include "nds.h"
#include "nds_input.h"
#include "nds_sdl.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

static const char *const k_platforms[] = { "Windows", "Mac", "Steam Deck" };

/* ------------------------------------------------------------------ */
/* Bindings                                                            */
/* ------------------------------------------------------------------ */

static bool is_function_key(const NdsBinding &b)
{
    return b.kind == NDS_BIND_KEY && b.code >= SDL_SCANCODE_F1 && b.code <= SDL_SCANCODE_F12;
}

static void check_unique(const NdsBinding (*map)[2], const char *device, int platform)
{
    for (int a = 0; a < NDS_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            if (map[a][s].kind == NDS_BIND_NONE) continue;
            for (int b = 0; b < NDS_ACTION_COUNT; b++) {
                for (int t = 0; t < 2; t++) {
                    if ((a != b || s != t) && map[a][s] == map[b][t]) {
                        CHECK(false, "%s %s default: %s and %s share a binding", k_platforms[platform], device,
                              nds_action_label(a), nds_action_label(b));
                    }
                }
            }
        }
    }
}

static bool has_button(const NdsInputMap &map, int action, int sdl_button)
{
    for (int s = 0; s < 2; s++) {
        if (map.controller[action][s].kind == NDS_BIND_BUTTON && map.controller[action][s].code == sdl_button) return true;
    }
    return false;
}

static bool has_axis(const NdsInputMap &map, int action, int sdl_axis, bool positive)
{
    for (int s = 0; s < 2; s++) {
        const NdsBinding &b = map.controller[action][s];
        if (b.kind == (positive ? NDS_BIND_AXIS_POSITIVE : NDS_BIND_AXIS_NEGATIVE) && b.code == sdl_axis) return true;
    }
    return false;
}

static void test_bindings(void)
{
    NdsInputMap maps[3];
    for (int p = 0; p < 3; p++) {
        nds_input_defaults(maps[p], (NdsInputPlatform)p);
        check_unique(maps[p].keyboard, "keyboard", p);
        check_unique(maps[p].controller, "controller", p);
        for (int a = 0; a < NDS_ACTION_GAMEPLAY_COUNT; a++) {
            CHECK(maps[p].keyboard[a][0].kind == NDS_BIND_KEY, "%s: %s has no key", k_platforms[p], nds_action_label(a));
            CHECK(maps[p].controller[a][0].kind != NDS_BIND_NONE, "%s: %s has no controller button", k_platforms[p],
                  nds_action_label(a));
            CHECK(nds_action_key_bit(a) != 0, "%s presses no DS button", nds_action_label(a));
        }
        for (int a = 0; a < NDS_ACTION_COUNT; a++) {
            CHECK(!memcmp(maps[p].controller[a], maps[0].controller[a], sizeof maps[p].controller[a]),
                  "%s: controller %s differs from Windows", k_platforms[p], nds_action_label(a));
            if (a >= NDS_ACTION_GAMEPLAY_COUNT) CHECK(nds_action_key_bit(a) == 0, "%s presses a DS button", nds_action_label(a));
            for (int s = 0; s < 2; s++) {
                for (const NdsBinding &b : { maps[p].keyboard[a][s], maps[p].controller[a][s] }) {
                    NdsBinding back;
                    std::string text = nds_binding_to_config(b);
                    CHECK(nds_binding_from_config(text.c_str(), back) && back == b, "%s: %s did not round-trip",
                          k_platforms[p], text.c_str());
                }
                CHECK(maps[p].keyboard[a][s].kind == NDS_BIND_NONE || maps[p].keyboard[a][s].kind == NDS_BIND_KEY,
                      "%s: keyboard %s is not a key", k_platforms[p], nds_action_label(a));
                CHECK(maps[p].controller[a][s].kind != NDS_BIND_KEY, "%s: controller %s is a key", k_platforms[p],
                      nds_action_label(a));
            }
        }
        /* The DS's own inputs and the layout shortcuts, from the keyboard. */
        for (int a : { NDS_ACTION_BLOW, NDS_ACTION_LID, NDS_ACTION_NEXT_LAYOUT, NDS_ACTION_SWAP_SCREENS, NDS_ACTION_MENU,
                       NDS_ACTION_FULLSCREEN, NDS_ACTION_FAST_FORWARD, NDS_ACTION_MUTE, NDS_ACTION_SCREENSHOT }) {
            CHECK(maps[p].keyboard[a][0].kind == NDS_BIND_KEY, "%s: %s has no key", k_platforms[p], nds_action_label(a));
        }
    }

    /* The buttons' bits are the machine's, each once. */
    uint32_t bits = 0;
    for (int a = 0; a < NDS_ACTION_GAMEPLAY_COUNT; a++) {
        CHECK(!(bits & nds_action_key_bit(a)), "%s shares a DS button with another action", nds_action_label(a));
        bits |= nds_action_key_bit(a);
    }
    CHECK(bits == (NDS_KEY_A | NDS_KEY_B | NDS_KEY_SELECT | NDS_KEY_START | NDS_KEY_RIGHT | NDS_KEY_LEFT | NDS_KEY_UP |
                   NDS_KEY_DOWN | NDS_KEY_R | NDS_KEY_L | NDS_KEY_X | NDS_KEY_Y),
          "the gameplay actions cover %03X, not the twelve DS buttons", bits);
    CHECK(nds_action_key_bit(NDS_ACTION_X) == NDS_KEY_X && nds_action_key_bit(NDS_ACTION_Y) == NDS_KEY_Y, "X and Y");

    /* The GBA frontend's keys, with X and Y added. */
    const NdsInputMap &win = maps[NDS_PLATFORM_WINDOWS], &mac = maps[NDS_PLATFORM_MAC], &deck = maps[NDS_PLATFORM_STEAM_DECK];
    CHECK(win.keyboard[NDS_ACTION_A][0].code == SDL_SCANCODE_Z && win.keyboard[NDS_ACTION_B][0].code == SDL_SCANCODE_X,
          "A and B are not Z and X");
    CHECK(win.keyboard[NDS_ACTION_X][0].code == SDL_SCANCODE_C && win.keyboard[NDS_ACTION_Y][0].code == SDL_SCANCODE_V,
          "X and Y are not C and V");
    CHECK(win.keyboard[NDS_ACTION_L][0].code == SDL_SCANCODE_Q && win.keyboard[NDS_ACTION_R][0].code == SDL_SCANCODE_E,
          "L and R are not Q and E");
    CHECK(win.keyboard[NDS_ACTION_UP][1].code == SDL_SCANCODE_W && win.keyboard[NDS_ACTION_START][0].code == SDL_SCANCODE_RETURN,
          "W and Enter");

    /* Nintendo positions on a controller: east A, south B, north X, west Y. */
    CHECK(has_button(win, NDS_ACTION_A, SDL_CONTROLLER_BUTTON_B) && has_button(win, NDS_ACTION_B, SDL_CONTROLLER_BUTTON_A),
          "A is not the east button or B not the south one");
    CHECK(has_button(win, NDS_ACTION_X, SDL_CONTROLLER_BUTTON_Y) && has_button(win, NDS_ACTION_Y, SDL_CONTROLLER_BUTTON_X),
          "X is not the north button or Y not the west one");
    CHECK(has_button(win, NDS_ACTION_L, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) &&
              has_button(win, NDS_ACTION_R, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER),
          "L and R are not the shoulder buttons");

    /* A controller alone (a Steam Deck in Game Mode): the stylus, the
     * microphone, the screens and the menu. */
    CHECK(has_axis(deck, NDS_ACTION_CURSOR_UP, SDL_CONTROLLER_AXIS_RIGHTY, false) &&
              has_axis(deck, NDS_ACTION_CURSOR_DOWN, SDL_CONTROLLER_AXIS_RIGHTY, true) &&
              has_axis(deck, NDS_ACTION_CURSOR_LEFT, SDL_CONTROLLER_AXIS_RIGHTX, false) &&
              has_axis(deck, NDS_ACTION_CURSOR_RIGHT, SDL_CONTROLLER_AXIS_RIGHTX, true),
          "the right stick does not move the stylus cursor");
    CHECK(deck.controller[NDS_ACTION_TOUCH][0].kind != NDS_BIND_NONE, "nothing on a controller puts the pen down");
    CHECK(deck.controller[NDS_ACTION_BLOW][0].kind != NDS_BIND_NONE, "nothing on a controller blows into the microphone");
    CHECK(deck.controller[NDS_ACTION_SWAP_SCREENS][0].kind != NDS_BIND_NONE, "nothing on a controller swaps the screens");
    CHECK(has_button(deck, NDS_ACTION_MENU, SDL_CONTROLLER_BUTTON_LEFTSTICK), "L3 does not open the menu");
    for (int a = 0; a < NDS_ACTION_COUNT; a++) {
        CHECK(!has_button(deck, a, SDL_CONTROLLER_BUTTON_GUIDE), "%s is on Guide, which Steam keeps", nds_action_label(a));
    }
    /* The lid stops the game, so no controller button closes it until the
     * player chooses one. */
    CHECK(deck.controller[NDS_ACTION_LID][0].kind == NDS_BIND_NONE && deck.controller[NDS_ACTION_LID][1].kind == NDS_BIND_NONE,
          "a controller button closes the lid by default");

    /* Shortcuts: F-keys on Windows and the Deck's keyboard, Command on a Mac. */
    for (int a = NDS_ACTION_CONSOLE_COUNT; a < NDS_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            CHECK(!is_function_key(mac.keyboard[a][s]), "Mac: %s is on an F-key", nds_action_label(a));
            CHECK(!memcmp(&deck.keyboard[a][s], &win.keyboard[a][s], sizeof win.keyboard[a][s]),
                  "Steam Deck: keyboard %s differs from Windows", nds_action_label(a));
        }
    }
    CHECK(!is_function_key(mac.keyboard[NDS_ACTION_LID][0]) && (mac.keyboard[NDS_ACTION_LID][0].mods & NDS_MOD_GUI),
          "Mac: the lid is not on a Command shortcut");
    for (int a : { NDS_ACTION_NEXT_LAYOUT, NDS_ACTION_SWAP_SCREENS, NDS_ACTION_MENU, NDS_ACTION_FULLSCREEN }) {
        CHECK(mac.keyboard[a][0].mods & NDS_MOD_GUI, "Mac: %s does not use Command", nds_action_label(a));
        CHECK(is_function_key(win.keyboard[a][0]), "Windows: %s is not on an F-key", nds_action_label(a));
    }
    CHECK(win.keyboard[NDS_ACTION_MENU][0].code == SDL_SCANCODE_F10, "Windows: F10 does not open the menu");
    CHECK(win.keyboard[NDS_ACTION_FULLSCREEN][0].code == SDL_SCANCODE_F11, "Windows: F11 is not fullscreen");
    /* F5 to F8 stay free for save states, as on the other consoles. */
    for (int a = 0; a < NDS_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            const NdsBinding &b = win.keyboard[a][s];
            CHECK(!(b.kind == NDS_BIND_KEY && b.code >= SDL_SCANCODE_F5 && b.code <= SDL_SCANCODE_F8),
                  "Windows: %s takes a save-state key", nds_action_label(a));
        }
    }

    /* Names for the preferences file: one per action, no spaces. */
    for (int a = 0; a < NDS_ACTION_COUNT; a++) {
        CHECK(nds_action_config(a)[0] && !strchr(nds_action_config(a), ' ') && !strchr(nds_action_config(a), '.'),
              "config name of %s", nds_action_label(a));
        for (int b = a + 1; b < NDS_ACTION_COUNT; b++) {
            CHECK(strcmp(nds_action_config(a), nds_action_config(b)) != 0, "%s and %s share a config name",
                  nds_action_label(a), nds_action_label(b));
        }
    }

    /* Preference-file text. */
    NdsBinding b;
    CHECK(nds_binding_from_config("key:22:8", b) && b.kind == NDS_BIND_KEY && b.code == 22 && b.mods == NDS_MOD_GUI, "key:22:8");
    CHECK(nds_binding_from_config("axis:5:+", b) && b.kind == NDS_BIND_AXIS_POSITIVE && b.code == 5, "axis:5:+");
    CHECK(nds_binding_from_config("axis:2:-", b) && b.kind == NDS_BIND_AXIS_NEGATIVE && b.code == 2, "axis:2:-");
    CHECK(nds_binding_from_config("button:7", b) && b.kind == NDS_BIND_BUTTON && b.code == 7, "button:7");
    CHECK(nds_binding_from_config("none", b) && b.kind == NDS_BIND_NONE, "none");
    for (const char *bad : { "", "key:", "key:0", "key:99999", "key:22:16", "button:-1", "button:99", "axis:5", "axis:5:x",
                             "axis:99:+", "stick:1" }) {
        CHECK(!nds_binding_from_config(bad, b), "\"%s\" was accepted", bad);
    }

    /* Modifiers. */
    NdsBinding plain = { NDS_BIND_KEY, 0, SDL_SCANCODE_D };
    NdsBinding combo = { NDS_BIND_KEY, NDS_MOD_GUI, SDL_SCANCODE_D };
    NdsBinding shift = { NDS_BIND_KEY, 0, SDL_SCANCODE_RSHIFT };
    CHECK(nds_key_mods_match(plain, 0) && nds_key_mods_match(plain, NDS_MOD_SHIFT), "a plain key fires alone and with Shift");
    CHECK(!nds_key_mods_match(plain, NDS_MOD_GUI) && !nds_key_mods_match(plain, NDS_MOD_CTRL) &&
              !nds_key_mods_match(plain, NDS_MOD_ALT),
          "a plain key fired with Command, Ctrl or Alt held");
    CHECK(nds_key_mods_match(combo, NDS_MOD_GUI) && !nds_key_mods_match(combo, 0) &&
              !nds_key_mods_match(combo, NDS_MOD_GUI | NDS_MOD_SHIFT),
          "Command+D fires only with exactly Command");
    CHECK(nds_key_mods_match(shift, NDS_MOD_SHIFT), "Right Shift bound on its own (Select)");
    CHECK(nds_mods_from_sdl(KMOD_LCTRL | KMOD_RSHIFT) == (NDS_MOD_CTRL | NDS_MOD_SHIFT) &&
              nds_mods_from_sdl(KMOD_RALT | KMOD_LGUI) == (NDS_MOD_ALT | NDS_MOD_GUI),
          "modifier bits from SDL");
    CHECK(nds_scancode_is_modifier(SDL_SCANCODE_LCTRL) && nds_scancode_is_modifier(SDL_SCANCODE_RGUI) &&
              !nds_scancode_is_modifier(SDL_SCANCODE_SPACE),
          "which keys are modifiers");

    /* Labels. */
    CHECK(nds_binding_label(win.controller[NDS_ACTION_A][0], NDS_PLATFORM_WINDOWS, NDS_PAD_XBOX) == "B",
          "DS A on an Xbox controller is its B");
    CHECK(nds_binding_label(win.controller[NDS_ACTION_A][0], NDS_PLATFORM_WINDOWS, NDS_PAD_NINTENDO) == "A",
          "DS A on a Nintendo controller is its A");
    CHECK(nds_binding_label(win.controller[NDS_ACTION_X][0], NDS_PLATFORM_WINDOWS, NDS_PAD_NINTENDO) == "X",
          "DS X on a Nintendo controller is its X");
    CHECK(nds_binding_label(win.controller[NDS_ACTION_TOUCH][0], NDS_PLATFORM_STEAM_DECK, NDS_PAD_STEAM_DECK) == "R2",
          "the pen button on a Steam Deck is R2");
    CHECK(nds_binding_label(win.controller[NDS_ACTION_CURSOR_LEFT][0], NDS_PLATFORM_WINDOWS, NDS_PAD_GENERIC) ==
              "Right Stick Left",
          "cursor left");
    CHECK(nds_binding_label(mac.keyboard[NDS_ACTION_NEXT_LAYOUT][0], NDS_PLATFORM_MAC, NDS_PAD_GENERIC) == "Cmd+D",
          "Mac next layout reads %s",
          nds_binding_label(mac.keyboard[NDS_ACTION_NEXT_LAYOUT][0], NDS_PLATFORM_MAC, NDS_PAD_GENERIC).c_str());
    NdsBinding none = { NDS_BIND_NONE, 0, 0 };
    CHECK(nds_binding_label(none, NDS_PLATFORM_WINDOWS, NDS_PAD_GENERIC) == "Unbound", "an empty binding");
    for (int p = 0; p < 3; p++) CHECK(strstr(nds_menu_hint((NdsInputPlatform)p), "L3") != NULL, "menu hint");
}

/* ------------------------------------------------------------------ */
/* Screens                                                             */
/* ------------------------------------------------------------------ */

enum { TOP, BOTTOM };

static bool rect_is(const SDL_Rect &r, int x, int y, int w, int h)
{
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

/* One expected arrangement: both rectangles, to the pixel. */
static void expect_screens(const char *what, int layout, bool swapped, int gap, int scaling, int w, int h, const int top[4],
                           const int bottom[4])
{
    const NdsScreens s = nds_layout_screens(layout, swapped, gap, scaling, w, h);
    CHECK(rect_is(s.rect[TOP], top[0], top[1], top[2], top[3]), "%s: top screen at %d,%d %dx%d, expected %d,%d %dx%d", what,
          s.rect[TOP].x, s.rect[TOP].y, s.rect[TOP].w, s.rect[TOP].h, top[0], top[1], top[2], top[3]);
    CHECK(rect_is(s.rect[BOTTOM], bottom[0], bottom[1], bottom[2], bottom[3]),
          "%s: bottom screen at %d,%d %dx%d, expected %d,%d %dx%d", what, s.rect[BOTTOM].x, s.rect[BOTTOM].y,
          s.rect[BOTTOM].w, s.rect[BOTTOM].h, bottom[0], bottom[1], bottom[2], bottom[3]);
}

#define EXPECT(what, layout, swapped, gap, scaling, w, h, tx, ty, tw, th, bx, by, bw, bh) \
    do {                                                                                 \
        const int top_[4] = { tx, ty, tw, th }, bottom_[4] = { bx, by, bw, bh };         \
        expect_screens(what, layout, swapped, gap, scaling, w, h, top_, bottom_);        \
    } while (0)

static void test_known_displays(void)
{
    const int PP = NDS_SCALING_PIXEL_PERFECT, FIT = NDS_SCALING_FIT;

    /* Stacked: 256 x 384 at the largest whole scale, centred. */
    EXPECT("stacked, 512x768 window", NDS_LAYOUT_STACKED, false, 0, PP, 512, 768, 0, 0, 512, 384, 0, 384, 512, 384);
    EXPECT("stacked, 3440x1440", NDS_LAYOUT_STACKED, false, 0, PP, 3440, 1440, 1336, 144, 768, 576, 1336, 720, 768, 576);
    EXPECT("stacked, 1920x1080", NDS_LAYOUT_STACKED, false, 0, PP, 1920, 1080, 704, 156, 512, 384, 704, 540, 512, 384);
    EXPECT("stacked, 1920x1200", NDS_LAYOUT_STACKED, false, 0, PP, 1920, 1200, 576, 24, 768, 576, 576, 600, 768, 576);
    EXPECT("stacked, 1280x800", NDS_LAYOUT_STACKED, false, 0, PP, 1280, 800, 384, 16, 512, 384, 384, 400, 512, 384);
    EXPECT("stacked swapped, 1920x1080", NDS_LAYOUT_STACKED, true, 0, PP, 1920, 1080, 704, 540, 512, 384, 704, 156, 512, 384);
    /* The console's own gap, 90 lines, still fits three times into 1440. */
    EXPECT("stacked, gap 90, 3440x1440", NDS_LAYOUT_STACKED, false, 90, PP, 3440, 1440, 1336, 9, 768, 576, 1336, 855, 768, 576);
    EXPECT("stacked, gap 90, 1920x1080", NDS_LAYOUT_STACKED, false, 90, PP, 1920, 1080, 704, 66, 512, 384, 704, 630, 512, 384);
    /* Fit: the whole height. */
    EXPECT("stacked fit, 1920x1080", NDS_LAYOUT_STACKED, false, 0, FIT, 1920, 1080, 600, 0, 720, 540, 600, 540, 720, 540);
    EXPECT("stacked fit, 3440x1440", NDS_LAYOUT_STACKED, false, 0, FIT, 3440, 1440, 1240, 0, 960, 720, 1240, 720, 960, 720);

    /* Side by side: 512 x 192. On the ultrawide display each screen is 6x. */
    EXPECT("side, 1024x384 window", NDS_LAYOUT_SIDE_BY_SIDE, false, 0, PP, 1024, 384, 0, 0, 512, 384, 512, 0, 512, 384);
    EXPECT("side, 3440x1440", NDS_LAYOUT_SIDE_BY_SIDE, false, 0, PP, 3440, 1440, 184, 144, 1536, 1152, 1720, 144, 1536, 1152);
    EXPECT("side, 1920x1080", NDS_LAYOUT_SIDE_BY_SIDE, false, 0, PP, 1920, 1080, 192, 252, 768, 576, 960, 252, 768, 576);
    EXPECT("side, 1920x1200", NDS_LAYOUT_SIDE_BY_SIDE, false, 0, PP, 1920, 1200, 192, 312, 768, 576, 960, 312, 768, 576);
    EXPECT("side, 1280x800", NDS_LAYOUT_SIDE_BY_SIDE, false, 0, PP, 1280, 800, 128, 208, 512, 384, 640, 208, 512, 384);
    EXPECT("side swapped, 3440x1440", NDS_LAYOUT_SIDE_BY_SIDE, true, 0, PP, 3440, 1440, 1720, 144, 1536, 1152, 184, 144, 1536,
           1152);
    EXPECT("side, gap 16, 3440x1440", NDS_LAYOUT_SIDE_BY_SIDE, false, 16, PP, 3440, 1440, 136, 144, 1536, 1152, 1768, 144,
           1536, 1152);
    EXPECT("side fit, 3440x1440", NDS_LAYOUT_SIDE_BY_SIDE, false, 0, FIT, 3440, 1440, 0, 75, 1720, 1290, 1720, 75, 1720, 1290);
    EXPECT("side fit, 1920x1080", NDS_LAYOUT_SIDE_BY_SIDE, false, 0, FIT, 1920, 1080, 0, 180, 960, 720, 960, 180, 960, 720);

    /* Large + small: the touch screen large, the top one in what is left. */
    EXPECT("large, 1536x768 window", NDS_LAYOUT_LARGE_SMALL, false, 0, PP, 1536, 768, 1024, 192, 512, 384, 0, 0, 1024, 768);
    EXPECT("large, 3440x1440", NDS_LAYOUT_LARGE_SMALL, false, 0, PP, 3440, 1440, 1848, 144, 1536, 1152, 56, 48, 1792, 1344);
    EXPECT("large, 1920x1080", NDS_LAYOUT_LARGE_SMALL, false, 0, PP, 1920, 1080, 1344, 348, 512, 384, 64, 60, 1280, 960);
    /* 6x would leave the small screen 1x, a sixth of the large one: 5x and 2x. */
    EXPECT("large, 1920x1200", NDS_LAYOUT_LARGE_SMALL, false, 0, PP, 1920, 1200, 1344, 408, 512, 384, 64, 120, 1280, 960);
    EXPECT("large, 2560x1440", NDS_LAYOUT_LARGE_SMALL, false, 0, PP, 2560, 1440, 1792, 432, 768, 576, 0, 48, 1792, 1344);
    EXPECT("large, 1280x800", NDS_LAYOUT_LARGE_SMALL, false, 0, PP, 1280, 800, 768, 208, 512, 384, 0, 112, 768, 576);
    EXPECT("large swapped, 1920x1080", NDS_LAYOUT_LARGE_SMALL, true, 0, PP, 1920, 1080, 64, 60, 1280, 960, 1344, 348, 512, 384);
    EXPECT("large fit, 1920x1080", NDS_LAYOUT_LARGE_SMALL, false, 0, FIT, 1920, 1080, 1280, 300, 640, 480, 0, 60, 1280, 960);
    EXPECT("large fit, 3440x1440", NDS_LAYOUT_LARGE_SMALL, false, 0, FIT, 3440, 1440, 1920, 150, 1520, 1140, 0, 0, 1920, 1440);
    /* A display on its side: the two go one above the other, the top
     * screen staying on top whichever is the large one. On the first two
     * both reach the same scale. */
    EXPECT("large, 1080x1920", NDS_LAYOUT_LARGE_SMALL, false, 0, PP, 1080, 1920, 28, 192, 1024, 768, 28, 960, 1024, 768);
    EXPECT("large, 800x1280", NDS_LAYOUT_LARGE_SMALL, false, 0, PP, 800, 1280, 16, 64, 768, 576, 16, 640, 768, 576);
    EXPECT("large, 800x1100", NDS_LAYOUT_LARGE_SMALL, false, 0, PP, 800, 1100, 144, 70, 512, 384, 16, 454, 768, 576);
    EXPECT("large swapped, 800x1100", NDS_LAYOUT_LARGE_SMALL, true, 0, PP, 800, 1100, 16, 70, 768, 576, 144, 646, 512, 384);

    /* Single: the touch screen alone; swapped, the top one. */
    EXPECT("single, 3440x1440", NDS_LAYOUT_SINGLE, false, 0, PP, 3440, 1440, 0, 0, 0, 0, 824, 48, 1792, 1344);
    EXPECT("single, 1920x1080", NDS_LAYOUT_SINGLE, false, 0, PP, 1920, 1080, 0, 0, 0, 0, 320, 60, 1280, 960);
    EXPECT("single, 1920x1200", NDS_LAYOUT_SINGLE, false, 0, PP, 1920, 1200, 0, 0, 0, 0, 192, 24, 1536, 1152);
    EXPECT("single swapped, 1920x1080", NDS_LAYOUT_SINGLE, true, 0, PP, 1920, 1080, 320, 60, 1280, 960, 0, 0, 0, 0);
    EXPECT("single fit, 1920x1080", NDS_LAYOUT_SINGLE, false, 0, FIT, 1920, 1080, 0, 0, 0, 0, 240, 0, 1440, 1080);

    /* The window is made the layout's own size; the screens then fill it. */
    static const struct { int layout, gap, scale, w, h; } sizes[] = {
        { NDS_LAYOUT_STACKED, 0, 1, 256, 384 },      { NDS_LAYOUT_STACKED, 0, 2, 512, 768 },
        { NDS_LAYOUT_STACKED, 16, 2, 512, 800 },     { NDS_LAYOUT_SIDE_BY_SIDE, 0, 2, 1024, 384 },
        { NDS_LAYOUT_SIDE_BY_SIDE, 16, 2, 1056, 384 }, { NDS_LAYOUT_LARGE_SMALL, 0, 1, 768, 384 },
        { NDS_LAYOUT_LARGE_SMALL, 0, 2, 1536, 768 }, { NDS_LAYOUT_LARGE_SMALL, 16, 2, 1568, 768 },
        { NDS_LAYOUT_SINGLE, 0, 3, 768, 576 },       { NDS_LAYOUT_SINGLE, 90, 3, 768, 576 },
    };
    for (const auto &e : sizes) {
        int w = 0, h = 0;
        nds_layout_size(e.layout, e.gap, e.scale, &w, &h);
        CHECK(w == e.w && h == e.h, "%s at %dx with gap %d is %dx%d, expected %dx%d", nds_layout_label(e.layout), e.scale,
              e.gap, w, h, e.w, e.h);
    }
}

static bool overlap(const SDL_Rect &a, const SDL_Rect &b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* What must hold on any display at least as large as the layout at 1x. */
static void test_any_display(void)
{
    static const int displays[][2] = {
        { 3440, 1440 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 }, { 3840, 2160 }, { 5120, 1440 },
        { 1280, 800 },  { 1280, 720 },  { 1366, 768 },  { 1024, 768 },  { 1600, 1200 }, { 1080, 1920 }, { 800, 1280 },
        { 2560, 1080 }, { 1000, 1000 }, { 1537, 771 },  { 777, 613 },
    };
    for (const auto &d : displays) {
        for (int layout = 0; layout < NDS_LAYOUT_COUNT; layout++) {
            for (int scaling = 0; scaling < NDS_SCALING_COUNT; scaling++) {
                for (int swapped = 0; swapped < 2; swapped++) {
                    for (int gap : { 0, 8, 90 }) {
                        const int w = d[0], h = d[1];
                        int min_w = 0, min_h = 0;
                        nds_layout_size(layout, gap, 1, &min_w, &min_h);
                        /* Large + small turns on its side for a tall display. */
                        const bool fits = (w >= min_w && h >= min_h) ||
                                          (layout == NDS_LAYOUT_LARGE_SMALL && w >= 256 && h >= 384 + gap);
                        if (!fits) continue;
                        char what[160];
                        snprintf(what, sizeof what, "%s%s, %s, gap %d, %dx%d", nds_layout_label(layout),
                                 swapped ? " swapped" : "", nds_scaling_label(scaling), gap, w, h);
                        const NdsScreens s = nds_layout_screens(layout, swapped != 0, gap, scaling, w, h);
                        int shown = 0;
                        for (int i = 0; i < 2; i++) {
                            const SDL_Rect &r = s.rect[i];
                            if (r.w == 0) continue;
                            shown++;
                            CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h, "%s: screen %d at %d,%d %dx%d leaves the display",
                                  what, i, r.x, r.y, r.w, r.h);
                            /* 4:3 to within the pixel a fractional scale rounds away. */
                            CHECK(abs(r.w * 3 - r.h * 4) <= 4, "%s: screen %d is %dx%d, not 4:3", what, i, r.w, r.h);
                            if (scaling == NDS_SCALING_PIXEL_PERFECT) {
                                CHECK(r.w % 256 == 0 && r.h % 192 == 0 && r.w / 256 == r.h / 192,
                                      "%s: screen %d is %dx%d, not a whole multiple of 256x192", what, i, r.w, r.h);
                            }
                        }
                        CHECK(shown == (layout == NDS_LAYOUT_SINGLE ? 1 : 2), "%s: %d screens shown", what, shown);
                        if (shown == 2) {
                            CHECK(!overlap(s.rect[TOP], s.rect[BOTTOM]), "%s: the screens overlap", what);
                            if (layout != NDS_LAYOUT_LARGE_SMALL) {
                                CHECK(s.rect[TOP].w == s.rect[BOTTOM].w && s.rect[TOP].h == s.rect[BOTTOM].h,
                                      "%s: the screens differ in size", what);
                            } else {
                                const SDL_Rect &large = s.rect[swapped ? TOP : BOTTOM], &other = s.rect[swapped ? BOTTOM : TOP];
                                CHECK(large.w >= other.w, "%s: the large screen (%d wide) is smaller than the other (%d)", what,
                                      large.w, other.w);
                                /* Half the large one's scale, in whole scales in Pixel Perfect. */
                                const bool half = scaling == NDS_SCALING_PIXEL_PERFECT
                                                      ? other.w / 256 >= (large.w / 256) / 2
                                                      : other.w * 2 + 2 >= large.w;
                                CHECK(half, "%s: the small screen (%d wide) is under half the large one (%d)", what, other.w,
                                      large.w);
                            }
                        }
                        /* The touch screen is the bottom one whatever the arrangement. */
                        if (layout == NDS_LAYOUT_SINGLE) {
                            CHECK((s.rect[BOTTOM].w != 0) == !swapped, "%s: the wrong screen is shown", what);
                        }
                    }
                }
            }
        }
    }

    /* A display smaller than the layout still gets both screens at 1x. */
    const NdsScreens tiny = nds_layout_screens(NDS_LAYOUT_STACKED, false, 0, NDS_SCALING_PIXEL_PERFECT, 100, 100);
    CHECK(tiny.rect[TOP].w == 256 && tiny.rect[BOTTOM].w == 256 && !overlap(tiny.rect[TOP], tiny.rect[BOTTOM]),
          "stacked on a 100x100 display");
    const NdsScreens tiny_pair = nds_layout_screens(NDS_LAYOUT_LARGE_SMALL, false, 0, NDS_SCALING_PIXEL_PERFECT, 100, 100);
    CHECK(tiny_pair.rect[TOP].w == 256 && tiny_pair.rect[BOTTOM].w == 256, "large + small on a 100x100 display");
    /* An absurd gap is brought into range rather than believed. */
    const NdsScreens wide_gap = nds_layout_screens(NDS_LAYOUT_STACKED, false, 100000, NDS_SCALING_PIXEL_PERFECT, 1920, 1080);
    CHECK(wide_gap.rect[BOTTOM].y - (wide_gap.rect[TOP].y + wide_gap.rect[TOP].h) == NDS_GAP_MAX * 2, "gap clamp");

    /* Names. */
    for (int layout = 0; layout < NDS_LAYOUT_COUNT; layout++) {
        CHECK(nds_layout_from_config(nds_layout_config(layout)) == layout, "%s does not round-trip", nds_layout_config(layout));
    }
    CHECK(nds_layout_from_config("nonsense") == -1 && nds_layout_from_config("") == -1, "unknown layout names");
}

static void expect_touch(const char *what, const NdsScreens &s, double px, double py, bool inside, int x, int y)
{
    int tx = -1, ty = -1;
    const bool hit = nds_layout_touch(s, px, py, &tx, &ty);
    CHECK(hit == inside && tx == x && ty == y, "%s: display (%g, %g) gives %s (%d, %d), expected %s (%d, %d)", what, px, py,
          hit ? "a touch at" : "no touch,", tx, ty, inside ? "a touch at" : "no touch,", x, y);
}

static void test_touch(void)
{
    const int PP = NDS_SCALING_PIXEL_PERFECT;

    /* A 512x768 stacked window: the bottom screen is the lower half at 2x. */
    const NdsScreens stacked = nds_layout_screens(NDS_LAYOUT_STACKED, false, 0, PP, 512, 768);
    expect_touch("stacked 2x", stacked, 75, 685, true, 37, 150);
    expect_touch("stacked 2x, first pixel", stacked, 0, 384, true, 0, 0);
    expect_touch("stacked 2x, last pixel", stacked, 511, 767, true, 255, 191);
    expect_touch("stacked 2x, on the top screen", stacked, 300, 383, false, 150, 0);
    expect_touch("stacked 2x, left of the window", stacked, -100, 500, false, 0, 58);
    expect_touch("stacked 2x, past the corner", stacked, 9000, 9000, false, 255, 191);
    expect_touch("stacked 2x, above and left", stacked, -5, -5, false, 0, 0);

    /* The same DS pixel, where each layout puts it on the 3440x1440 display. */
    const NdsScreens wide_stacked = nds_layout_screens(NDS_LAYOUT_STACKED, false, 0, PP, 3440, 1440);
    expect_touch("stacked, 3440x1440", wide_stacked, 1336 + 37 * 3 + 1, 720 + 150 * 3 + 1, true, 37, 150);
    const NdsScreens wide_side = nds_layout_screens(NDS_LAYOUT_SIDE_BY_SIDE, false, 0, PP, 3440, 1440);
    expect_touch("side, 3440x1440", wide_side, 1720 + 37 * 6 + 3, 144 + 150 * 6 + 3, true, 37, 150);
    expect_touch("side, 3440x1440, on the top screen", wide_side, 1719, 700, false, 0, 92);
    const NdsScreens wide_swapped = nds_layout_screens(NDS_LAYOUT_SIDE_BY_SIDE, true, 0, PP, 3440, 1440);
    expect_touch("side swapped, 3440x1440", wide_swapped, 184 + 37 * 6 + 3, 144 + 150 * 6 + 3, true, 37, 150);
    const NdsScreens wide_large = nds_layout_screens(NDS_LAYOUT_LARGE_SMALL, false, 0, PP, 3440, 1440);
    expect_touch("large, 3440x1440", wide_large, 56 + 37 * 7 + 3, 48 + 150 * 7 + 3, true, 37, 150);
    const NdsScreens wide_large_swapped = nds_layout_screens(NDS_LAYOUT_LARGE_SMALL, true, 0, PP, 3440, 1440);
    expect_touch("large swapped, 3440x1440", wide_large_swapped, 1848 + 37 * 6 + 3, 144 + 150 * 6 + 3, true, 37, 150);
    const NdsScreens wide_single = nds_layout_screens(NDS_LAYOUT_SINGLE, false, 0, PP, 3440, 1440);
    expect_touch("single, 3440x1440", wide_single, 824 + 37 * 7 + 3, 48 + 150 * 7 + 3, true, 37, 150);
    /* With only the top screen shown there is nothing to touch. */
    const NdsScreens top_only = nds_layout_screens(NDS_LAYOUT_SINGLE, true, 0, PP, 3440, 1440);
    expect_touch("single swapped", top_only, 1700, 700, false, 0, 0);
    double px = 1.0, py = 1.0;
    CHECK(!nds_layout_touch_position(top_only, 10, 10, &px, &py), "a cursor position on a screen that is not shown");

    /* No dead border: on every layout, display and scaling, each display
     * pixel of the rectangle touches a DS pixel, each DS pixel is touched
     * by as many display pixels as the scale says, and the pixels just
     * outside do not count as on the screen. */
    static const int displays[][2] = { { 3440, 1440 }, { 1920, 1080 }, { 1920, 1200 }, { 1280, 800 }, { 777, 613 } };
    for (const auto &d : displays) {
        for (int layout = 0; layout < NDS_LAYOUT_COUNT; layout++) {
            for (int scaling = 0; scaling < NDS_SCALING_COUNT; scaling++) {
                const NdsScreens s = nds_layout_screens(layout, false, 8, scaling, d[0], d[1]);
                const SDL_Rect &r = s.rect[BOTTOM];
                char what[120];
                snprintf(what, sizeof what, "%s, %s, %dx%d", nds_layout_label(layout), nds_scaling_label(scaling), d[0], d[1]);
                std::vector<int> columns(256, 0), rows(192, 0);
                bool all_inside = true;
                for (int x = r.x; x < r.x + r.w; x++) {
                    int tx = 0, ty = 0;
                    all_inside = nds_layout_touch(s, x, r.y, &tx, &ty) && all_inside;
                    columns[(size_t)tx]++;
                }
                for (int y = r.y; y < r.y + r.h; y++) {
                    int tx = 0, ty = 0;
                    all_inside = nds_layout_touch(s, r.x, y, &tx, &ty) && all_inside;
                    rows[(size_t)ty]++;
                }
                CHECK(all_inside, "%s: part of the bottom screen's rectangle does not count as touching it", what);
                int least = 1 << 30, most = 0;
                for (int v : columns) {
                    least = v < least ? v : least;
                    most = v > most ? v : most;
                }
                for (int v : rows) {
                    least = v < least ? v : least;
                    most = v > most ? v : most;
                }
                CHECK(least >= r.w / 256 && most <= r.w / 256 + 1, "%s: DS pixels get %d to %d display pixels at scale %.2f",
                      what, least, most, r.w / 256.0);
                int tx = 0, ty = 0;
                CHECK(!nds_layout_touch(s, r.x - 1, r.y, &tx, &ty) && tx == 0 && ty == 0, "%s: left of the rectangle", what);
                CHECK(!nds_layout_touch(s, r.x, r.y - 1, &tx, &ty) && tx == 0 && ty == 0, "%s: above the rectangle", what);
                CHECK(!nds_layout_touch(s, r.x + r.w, r.y + r.h - 1, &tx, &ty) && tx == 255 && ty == 191,
                      "%s: right of the rectangle", what);
                CHECK(!nds_layout_touch(s, r.x + r.w - 1, r.y + r.h, &tx, &ty) && tx == 255 && ty == 191,
                      "%s: below the rectangle", what);
                /* And back: the middle of a DS pixel is a display position that touches it. */
                for (int x : { 0, 37, 128, 255 }) {
                    for (int y : { 0, 150, 191 }) {
                        CHECK(nds_layout_touch_position(s, x + 0.5, y + 0.5, &px, &py) && nds_layout_touch(s, px, py, &tx, &ty) &&
                                  tx == x && ty == y,
                              "%s: the middle of pixel (%d, %d) is display (%g, %g), which touches (%d, %d)", what, x, y, px,
                              py, tx, ty);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Window or headless                                                  */
/* ------------------------------------------------------------------ */

static bool headless(std::vector<const char *> args)
{
    args.insert(args.begin(), "game");
    return nds_sdl_wants_headless((int)args.size(), const_cast<char **>(args.data()));
}

/* The options the window and the runner both take: they do not choose
 * between the two. */
static const char *const k_shared_options[] = { "--bios9", "--bios7", "--interpreter", "--internal-scale" };

/* The runner's own source says which options it takes: every one that is
 * not shared with the window must send the command line to it. This is
 * what notices an option added to the runner and not to the frontend's
 * list. */
static void test_runner_source(const char *path)
{
    FILE *file = NULL;
#ifdef _MSC_VER
    if (fopen_s(&file, path, "rb") != 0) file = NULL;
#else
    file = fopen(path, "rb");
#endif
    CHECK(file != NULL, "cannot read %s", path);
    if (!file) return;
    std::string source;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, file)) > 0) source.append(chunk, got);
    fclose(file);

    /* Every strcmp(a, "--option") and strncmp(a, "--option=", n) in it. An
     * option compared by its first characters carries its value in the same
     * word (--auto-input=RUN), so it is tried with one. */
    int found = 0;
    const std::string compare = "cmp(a, \"--";
    for (size_t at = source.find(compare); at != std::string::npos; at = source.find(compare, at + 1)) {
        const size_t start = at + compare.size() - 2, end = source.find('"', start);
        if (end == std::string::npos) break;
        std::string option = source.substr(start, end - start);
        if (!option.empty() && option.back() == '=') option += "VALUE";
        bool shared = false;
        for (const char *s : k_shared_options) shared = shared || option == s;
        found++;
        if (shared) CHECK(!headless({ "game.nds", option.c_str() }), "%s, which the window takes too, runs headless", option.c_str());
        else CHECK(headless({ "game.nds", option.c_str() }), "the runner's %s opens a window", option.c_str());
    }
    CHECK(found >= 20, "only %d options found in %s: has its option parsing changed shape?", found, path);
    printf("runner options checked against %s: %d\n", path, found);
}

static void test_headless(void)
{
    /* Each option of nds_runner.c that only the runner has. */
    static const char *const runner[][2] = {
        { "--frames", "600" },      { "--keys", "30:A+B:5" },    { "--touch", "40:128,96:3" }, { "--lid", "50:10" },
        { "--dump-frame", "f.ppm" }, { "--dump-every", "60" },    { "--dump-from", "120" },     { "--hash-frames", NULL },
        { "--sha-frames", NULL },   { "--state-hash", NULL },    { "--dump-audio", "a.raw" },  { "--audio-hash", NULL },
        { "--dump-ram", "ram.bin" }, { "--regs", NULL },          { "--differential", NULL },   { "--differential-step", NULL },
        { "--coverage", NULL },     { "--trace-out", "t.txt" },  { "--trace", "9:0:FFFFFFFF" }, { "--quiet", NULL },
        { "--dump-mem", "2000000:100:m.bin" }, { "--profile", NULL }, { "--load-save", "in.sav" }, { "--dump-save", "out.sav" },
        { "--auto-input", NULL },   { "--auto-input=play", NULL },
        /* The GBA frontend's list, for runner options still to come. */
        { "--headless", NULL },     { "--until-idle", NULL },    { "--expect", "r0=1" },       { "--expect-text", "PASS" },
        { "--screen-text", NULL },
    };
    for (const auto &option : runner) {
        std::vector<const char *> args = { option[0] };
        if (option[1]) args.push_back(option[1]);
        CHECK(headless(args), "%s opens a window", option[0]);
        /* With a ROM, and among the options both take. */
        args.insert(args.begin(), { "game.nds", "--interpreter", "--bios9", "bios9.bin" });
        CHECK(headless(args), "game.nds --interpreter --bios9 bios9.bin %s opens a window", option[0]);
    }

    /* The window's options, and the ones both take, do not ask for a headless run. */
    static const char *const window[][2] = {
        { "--scale", "3" },          { "--layout", "side" },      { "--save-dir", "saves" },   { "--fullscreen", NULL },
        { "--mute", NULL },          { "--interpreter", NULL },   { "--limit-frames", "600" }, { "--screenshot-prefix", "shot" },
        { "--audio-stats", NULL },   { "--prefs", "prefs.ini" },  { "--display-shot", "d.bmp" }, { "--display-size", "3440x1440" },
        { "--menu-selftest", NULL }, { "--menu-selftest-verify", NULL }, { "--bios9", "bios9.bin" }, { "--bios7", "bios7.bin" },
        { "--menu-shot", "m.bmp" },
    };
    CHECK(!headless({}), "no arguments run headless");
    CHECK(!headless({ "game.nds" }), "a ROM alone runs headless");
    for (const auto &option : window) {
        std::vector<const char *> args = { "game.nds", option[0] };
        if (option[1]) args.push_back(option[1]);
        CHECK(!headless(args), "game.nds %s runs headless", option[0]);
    }
}

/* test_nds_input [runtime-nds/src/nds_runner.c] */
int main(int argc, char **argv)
{
    test_bindings();
    test_known_displays();
    test_any_display();
    test_touch();
    test_headless();
    if (argc > 1) test_runner_source(argv[1]);
    else printf("no runner source given: its options were not read from it\n");
    printf("%d checks, %d failed\n", checks, failures);
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
