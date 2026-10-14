/*
 * GBA input bindings (runtimes/runtime-gba/platform/gba_input.cpp):
 *
 * - Every platform's defaults bind each control to at most one action.
 * - Mac shortcuts use Command, never F-keys (a Mac laptop's F-keys are
 *   brightness and volume keys unless fn is held); Windows keeps F-keys.
 * - The Steam Deck defaults reach the menu, save states and fast forward
 *   from its buttons, without the Steam button.
 * - Controller gameplay buttons are the same on every platform.
 * - Bindings survive the preference-file format; bad text is refused.
 * - A plain key does not fire while Command/Ctrl is held (Command+S must not
 *   also press S = down), and Shift does not block it.
 * - Labels use the platform's key names and the controller's button names.
 */
#include <stdio.h>
#include <string.h>

#define SDL_MAIN_HANDLED /* plain main() */
#include "gba_input.h"

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

static const char *const k_platforms[] = { "Windows", "Mac", "Steam Deck" };

static bool is_function_key(const GbaBinding &b)
{
    return b.kind == GBA_BIND_KEY && b.code >= SDL_SCANCODE_F1 && b.code <= SDL_SCANCODE_F12;
}

static void check_unique(const GbaBinding (*map)[2], const char *device, int platform)
{
    for (int a = 0; a < GBA_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            if (map[a][s].kind == GBA_BIND_NONE) continue;
            for (int b = 0; b < GBA_ACTION_COUNT; b++) {
                for (int t = 0; t < 2; t++) {
                    if ((a != b || s != t) && map[a][s] == map[b][t]) {
                        CHECK(false, "%s %s default: %s and %s share a binding", k_platforms[platform], device,
                              gba_action_label(a), gba_action_label(b));
                    }
                }
            }
        }
    }
}

int main(void)
{
    GbaInputMap maps[3];
    for (int p = 0; p < 3; p++) {
        gba_input_defaults(maps[p], (GbaInputPlatform)p);
        check_unique(maps[p].keyboard, "keyboard", p);
        check_unique(maps[p].controller, "controller", p);
        for (int a = 0; a < GBA_ACTION_GAMEPLAY_COUNT; a++) {
            CHECK(maps[p].keyboard[a][0].kind == GBA_BIND_KEY, "%s: %s has no key", k_platforms[p], gba_action_label(a));
            CHECK(maps[p].controller[a][0].kind != GBA_BIND_NONE, "%s: %s has no controller button", k_platforms[p],
                  gba_action_label(a));
            CHECK(!memcmp(maps[p].controller[a], maps[0].controller[a], sizeof maps[p].controller[a]),
                  "%s: controller %s differs from Windows", k_platforms[p], gba_action_label(a));
        }
        for (int a = 0; a < GBA_ACTION_COUNT; a++) {
            for (int s = 0; s < 2; s++) {
                for (const GbaBinding &b : { maps[p].keyboard[a][s], maps[p].controller[a][s] }) {
                    GbaBinding back;
                    std::string text = gba_binding_to_config(b);
                    CHECK(gba_binding_from_config(text.c_str(), back) && back == b, "%s: %s did not round-trip",
                          k_platforms[p], text.c_str());
                }
            }
        }
    }

    const GbaInputMap &mac = maps[GBA_PLATFORM_MAC], &win = maps[GBA_PLATFORM_WINDOWS], &deck = maps[GBA_PLATFORM_STEAM_DECK];
    for (int a = GBA_ACTION_GAMEPLAY_COUNT; a < GBA_ACTION_COUNT; a++) {
        CHECK(!is_function_key(mac.keyboard[a][0]) && !is_function_key(mac.keyboard[a][1]), "Mac %s uses an F-key",
              gba_action_label(a));
    }
    CHECK(mac.keyboard[GBA_ACTION_SAVE_STATE][0] == (GbaBinding{ GBA_BIND_KEY, GBA_MOD_GUI, SDL_SCANCODE_S }),
          "Mac save state is not Cmd+S");
    CHECK(mac.keyboard[GBA_ACTION_FULLSCREEN][0].mods == GBA_MOD_GUI, "Mac fullscreen is not a Command shortcut");
    CHECK(win.keyboard[GBA_ACTION_SAVE_STATE][0] == (GbaBinding{ GBA_BIND_KEY, 0, SDL_SCANCODE_F5 }), "Windows save state is not F5");
    CHECK(win.keyboard[GBA_ACTION_FULLSCREEN][0] == (GbaBinding{ GBA_BIND_KEY, 0, SDL_SCANCODE_F11 }), "Windows fullscreen is not F11");
    for (int a : { GBA_ACTION_MENU, GBA_ACTION_SAVE_STATE, GBA_ACTION_LOAD_STATE, GBA_ACTION_FAST_FORWARD }) {
        const GbaBinding &b = deck.controller[a][0];
        CHECK(b.kind == GBA_BIND_BUTTON && b.code != SDL_CONTROLLER_BUTTON_GUIDE, "Steam Deck %s is not on a Deck button",
              gba_action_label(a));
    }

    GbaBinding parsed;
    CHECK(gba_binding_from_config("none", parsed) && parsed.kind == GBA_BIND_NONE, "none not parsed");
    CHECK(gba_binding_from_config("key:22:8", parsed) && parsed.code == 22 && parsed.mods == 8, "key with mods not parsed");
    CHECK(gba_binding_from_config("key:22", parsed) && parsed.mods == 0, "key without mods not parsed");
    for (const char *bad : { "", "key:", "key:0", "key:99999", "key:22:99", "button:", "button:99", "axis:1", "axis:1:x",
                             "axis:9:+", "mouse:1" }) {
        CHECK(!gba_binding_from_config(bad, parsed), "accepted \"%s\"", bad);
    }

    const GbaBinding plain_s = { GBA_BIND_KEY, 0, SDL_SCANCODE_S }, cmd_s = { GBA_BIND_KEY, GBA_MOD_GUI, SDL_SCANCODE_S };
    const GbaBinding right_shift = { GBA_BIND_KEY, 0, SDL_SCANCODE_RSHIFT };
    CHECK(gba_key_mods_match(plain_s, 0), "S alone did not match");
    CHECK(gba_key_mods_match(plain_s, GBA_MOD_SHIFT), "Shift blocked a plain key");
    CHECK(!gba_key_mods_match(plain_s, GBA_MOD_GUI), "Cmd+S pressed plain S");
    CHECK(!gba_key_mods_match(plain_s, GBA_MOD_CTRL), "Ctrl+S pressed plain S");
    CHECK(gba_key_mods_match(cmd_s, GBA_MOD_GUI), "Cmd+S did not match");
    CHECK(!gba_key_mods_match(cmd_s, 0), "S alone matched Cmd+S");
    CHECK(!gba_key_mods_match(cmd_s, GBA_MOD_GUI | GBA_MOD_SHIFT), "Cmd+Shift+S matched Cmd+S");
    CHECK(gba_key_mods_match(right_shift, GBA_MOD_SHIFT), "Right Shift bound alone did not match");
    CHECK(gba_mods_from_sdl(KMOD_LGUI | KMOD_RSHIFT) == (GBA_MOD_GUI | GBA_MOD_SHIFT), "SDL modifiers not converted");

    const GbaBinding enter = { GBA_BIND_KEY, 0, SDL_SCANCODE_RETURN }, backspace = { GBA_BIND_KEY, 0, SDL_SCANCODE_BACKSPACE };
    const GbaBinding south = { GBA_BIND_BUTTON, 0, SDL_CONTROLLER_BUTTON_A };
    const GbaBinding lt = { GBA_BIND_AXIS_POSITIVE, 0, SDL_CONTROLLER_AXIS_TRIGGERLEFT };
    CHECK(gba_binding_label(cmd_s, GBA_PLATFORM_MAC, GBA_PAD_GENERIC) == "Cmd+S", "Mac label: %s",
          gba_binding_label(cmd_s, GBA_PLATFORM_MAC, GBA_PAD_GENERIC).c_str());
    CHECK(gba_binding_label(backspace, GBA_PLATFORM_MAC, GBA_PAD_GENERIC) == "Delete", "Mac Backspace label: %s",
          gba_binding_label(backspace, GBA_PLATFORM_MAC, GBA_PAD_GENERIC).c_str());
    CHECK(gba_binding_label(enter, GBA_PLATFORM_WINDOWS, GBA_PAD_GENERIC) == "Enter", "Windows Enter label");
    CHECK(gba_binding_label(south, GBA_PLATFORM_STEAM_DECK, GBA_PAD_STEAM_DECK) == "A", "Deck south label");
    CHECK(gba_binding_label(south, GBA_PLATFORM_WINDOWS, GBA_PAD_NINTENDO) == "B", "Nintendo south label");
    CHECK(gba_binding_label(lt, GBA_PLATFORM_STEAM_DECK, GBA_PAD_STEAM_DECK) == "L2", "Deck trigger label");
    CHECK(gba_pad_profile(NULL, GBA_PLATFORM_STEAM_DECK) == GBA_PAD_STEAM_DECK, "Deck profile without a pad");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("input: all checks passed\n");
    return 0;
}
