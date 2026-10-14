/*
 * SDL2 frontend for Nintendo 64 programs: window, input, audio, frame
 * pacing, the cartridge save and the settings menu. Recompiled games call
 * n64_sdl_main(); headless options go to n64_runner_main() instead (see
 * n64_sdl_wants_headless()). Adapted from the SNES frontend
 * (runtimes/runtime-snes/platform/snes_sdl.cpp); the menu follows the Game Boy
 * runtime's.
 *
 *   <exe> [ROM] [--scale N] [--save-dir DIR] [--fullscreen] [--mute]
 *         [--interpreter] [--no-learn] [--vi-raw] [--limit-frames N] [--screenshot-prefix P]
 *         [--audio-stats] [--prefs FILE] [--internal-scale 1|2|4]
 *         [--display-shot FILE.bmp] [--display-size WxH]
 *         [--menu-selftest | --menu-selftest-verify]
 *
 * --limit-frames exits after N frames; --screenshot-prefix writes P.bmp of
 * the last frame on exit (the hub uses both to capture cover art).
 * --display-shot instead writes the last frame as the display shows it, at
 * --display-size (default 3840x2160) through the saved scaling mode and
 * filter. --mute silences this run without touching the saved Mute
 * setting. The menu self-test is described above menu_selftest_step().
 *
 * The VI puts out a 640 x 480 picture (640 x 576 on a PAL console) for a
 * 4:3 television, whatever the game drew, so the window keeps 4:3: the
 * scale steps are multiples of 320 x 240, the size most games draw at, so
 * Pixel Perfect keeps their pixels square and whole. Scale filters are
 * Nearest and Linear.
 *
 * The RDP can draw a second picture at two or four times the game's
 * resolution (Internal Resolution in the menu, display.internal_scale,
 * --internal-scale N for one run; n64.h, n64_set_internal_scale). The
 * console's own picture is still drawn and is what the game reads back, so
 * the game runs as it does without; the window shows the larger one. A
 * 320 x 240 game is then 640 x 480 or 1280 x 960. Screenshots and the hub's
 * covers stay the console's picture.
 *
 * Controls are bindable in the menu; the defaults are in n64_input.h:
 * arrows = stick, X = A, C = B, Z = Z, Enter = Start, A/S = L/R, IJKL = C
 * buttons, TFGH = D-pad, Tab = fast forward (hold), M = mute, with F-key
 * shortcuts on Windows and Linux (F1 overlay, F10 menu, F11 fullscreen, F12
 * screenshot) and Command shortcuts on a Mac. Escape and a controller's
 * Guide button always open the settings menu (the game pauses while it is
 * open); L3 does too by default, and is the way in on a Steam Deck.
 *
 * Display, audio and control choices are kept in n64_prefs.ini in the
 * per-user app data folder, shared by all N64 games; bindings are stored
 * only where they differ from the platform's defaults. The cartridge's
 * battery memory and the Controller Paks are <save name>.sav. There are no
 * save states yet, so the menu offers none.
 */
#include "n64_sdl.h"

#define SDL_MAIN_HANDLED /* callers keep their own main() */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include "n64.h"
#include "n64_coverage.h"
#include "n64_input.h"
#include "n64_recomp.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_dir(path) mkdir(path, 0777)
#endif

/* The VI's field rate: 60 / 1.001 Hz on an NTSC console, 50 Hz on PAL. */
#define FRAME_SECONDS_NTSC (1001.0 / 60000.0)
#define FRAME_SECONDS_PAL (1.0 / 50.0)
/* The picture's 4:3 shape, and the unit the window scales in. */
#define ASPECT_W 320
#define ASPECT_H 240

static const char *const k_headless_flags[] = {
    "--headless",   "--frames",    "--dump-frame",       "--dump-state",     "--dump-audio",
    "--keys",       "--auto-input", "--differential",    "--coverage",       "--trace-out",
    "--isviewer",   "--expect-isviewer", "--fail-isviewer", "--until-gpr",   "--expect-gpr",
    "--benchmark",  "--quiet",     "--trace-last",       "--perf",          "--dump-every",
    "--dump-save",  "--learn-out", "--dump-large",
};

bool n64_sdl_wants_headless(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        for (size_t k = 0; k < sizeof k_headless_flags / sizeof k_headless_flags[0]; k++) {
            if (!strcmp(argv[i], k_headless_flags[k])) return true;
        }
    }
    return false;
}

/* SDL_Quit, with what has been printed sent on its way first. On Windows a
 * program that quits within a moment of SDL_Init(SDL_INIT_GAMECONTROLLER)
 * loses whatever is still in stdout's buffer one or two times in a hundred
 * (the SNES frontend has the measurements). The menu self-test's verdict is
 * such text, and its script reads it. */
static void quit_sdl(void)
{
    fflush(stdout);
    fflush(stderr);
    SDL_Quit();
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = len > 0 ? (uint8_t *)malloc((size_t)len) : NULL;
    if (data && fread(data, 1, (size_t)len, f) != (size_t)len) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = data ? (size_t)len : 0;
    return data;
}

/* <dir>/<rom file name without extension><ext>; dir defaults to the
 * executable's directory. Inside a macOS .app that folder belongs to the
 * signed bundle, so the default there is the per-user app data folder. */
static void sibling_path(char *out, size_t cap, const char *dir, const char *rom_path, const char *ext)
{
    const char *leaf = rom_path;
    for (const char *p = rom_path; *p; p++) {
        if (*p == '/' || *p == '\\') leaf = p + 1;
    }
    char stem[256];
    snprintf(stem, sizeof stem, "%s", leaf);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    char *base = NULL;
    if (!dir) base = SDL_GetBasePath();
#if defined(__APPLE__)
    if (base && strstr(base, ".app/Contents/")) {
        SDL_free(base);
        base = SDL_GetPrefPath("gbrecompiled", stem);
    }
#endif
    snprintf(out, cap, "%s%s%s%s", dir ? dir : (base ? base : ""),
             (dir && dir[0] && dir[strlen(dir) - 1] != '/' && dir[strlen(dir) - 1] != '\\') ? "/" : "", stem, ext);
    SDL_free(base);
}

/* The machine's save image (n64_save_export: the cartridge's EEPROM, SRAM
 * or FlashRAM, then the four Controller Paks), written whole. */
static bool write_save(N64Machine *m, const char *path)
{
    const size_t size = n64_save_size(m);
    if (!size) return false;
    std::vector<uint8_t> image(size);
    n64_save_export(m, image.data());
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write save %s\n", path);
        return false;
    }
    const bool ok = fwrite(image.data(), 1, size, f) == size;
    fclose(f);
    if (ok) m->save_dirty = false;
    return ok;
}

static void read_save(N64Machine *m, const char *path)
{
    size_t n = 0;
    uint8_t *sav = read_file(path, &n);
    if (!sav) return;
    n64_save_import(m, sav, n);
    free(sav);
}

static int frame_width(const N64Machine *m) { return m->out_width > 0 ? m->out_width : 640; }
static int frame_height(const N64Machine *m) { return m->out_height > 0 ? m->out_height : 480; }

/* The framebuffer is 0x00RRGGBB: XRGB, so the BMP has no alpha channel to
 * read as transparent. */
static bool screenshot(const N64Machine *m, const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)m->framebuffer, frame_width(m), frame_height(m), 32,
                                                        N64_FB_WIDTH * 4, SDL_PIXELFORMAT_RGB888);
    bool ok = false;
    if (s) {
        ok = SDL_SaveBMP(s, path) == 0;
        if (ok) printf("screenshot: %s\n", path);
        SDL_FreeSurface(s);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Settings menu and overlay                                           */
/* ------------------------------------------------------------------ */

enum { SCALING_PIXEL_PERFECT, SCALING_ASPECT_FIT, SCALING_ASPECT_FILL, SCALING_STRETCH };
static const char *const k_scaling_names[] = { "Pixel Perfect", "Aspect Fit", "Aspect Fill", "Stretch" };
enum { FILTER_NEAREST, FILTER_LINEAR, FILTER_COUNT };
static const char *const k_filter_names[] = { "Nearest", "Linear" };
static const char *const k_scale_names[] = { "1x (320x240)",   "2x (640x480)",    "3x (960x720)",
                                             "4x (1280x960)",  "5x (1600x1200)",  "6x (1920x1440)" };
enum { DEFAULT_SCALE = 3, MAX_SCALE = 6 };
/* Internal Resolution: the scales the RDP can draw at (n64_rdp_set_scale). */
static const int k_internal_scales[] = { 1, 2, 4 };
static const char *const k_internal_scale_names[] = { "1x (the console's own)", "2x (640x480 for a 320x240 game)",
                                                      "4x (1280x960 for a 320x240 game)" };

/* Master volume slider position, 0-100. The AI plays what the game mixed,
 * at the level a television would get, so the slider is a straight
 * percentage (as on the GameCube) rather than the heavy attenuation the
 * runtimes for consoles with a sound chip apply. */
enum { VOLUME_DEFAULT = 80, VOLUME_MAX = 100 };

/* What the frontend sends to controller port 1. */
struct PadState {
    uint16_t buttons = 0;
    int8_t x = 0, y = 0;
};

/* Frontend state kept between frames. */
struct Frontend {
    N64Machine *m = nullptr;
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    SDL_Texture *tex = nullptr;
    SDL_Texture *large_tex = nullptr; /* the larger picture, made when there is one */
    int large_w = 0, large_h = 0;
    SDL_Rect viewport = { 0, 0, ASPECT_W, ASPECT_H };
    SDL_GameController *pad = nullptr;
    SDL_AudioDeviceID audio = 0;
    SDL_AudioSpec have = {};
    std::string title;

    /* Kept in n64_prefs.ini. */
    int scale = DEFAULT_SCALE;
    int scaling = SCALING_PIXEL_PERFECT;
    int filter = FILTER_NEAREST;
    bool show_overlay = false;
    bool show_fps = false;
    bool console_filters = true; /* the VI's own filters (n64_vi.c); off shows the bare framebuffer */
    int internal_scale = 1;          /* the setting: 1, 2 or 4 */
    int internal_scale_this_run = 0; /* --internal-scale: this run only */
    bool muted = false;
    int volume = VOLUME_DEFAULT;
    N64InputMap map = {};

    /* Controls. */
    N64InputPlatform platform = n64_input_host_platform();
    N64PadProfile pad_profile = N64_PAD_GENERIC;
    N64InputMap defaults = {};
    bool action_down[N64_ACTION_COUNT] = {};   /* held at the last poll */
    bool action_tapped[N64_ACTION_COUNT] = {}; /* pressed by an event since the last poll */
    bool input_resync = false;                 /* take the next poll as the baseline, firing nothing */
    bool capturing = false;                    /* waiting for a key or button to bind */
    bool capture_keyboard = true;
    int capture_action = 0, capture_slot = 0;
    int capture_modifier = SDL_SCANCODE_UNKNOWN; /* modifier pressed alone so far */
    bool pad_nav_blocked = false; /* controller menu navigation off until its buttons are released */
    std::string bind_note;

    bool fullscreen = false;
    /* What the settings file holds (display.fullscreen): the last choice made in
     * the menu, with the key or by the system - not --fullscreen, which is one
     * run's. */
    bool fullscreen_saved = false;
    bool mute_this_run = false; /* --mute: silent without changing the saved setting */
    bool show_menu = false;
    /* Buttons, and the stick, still held from using the menu; ignored until
     * released (the stick until it is back in the centre). */
    uint16_t held_from_menu = 0;
    bool stick_from_menu = false;
    PadState sent; /* what port 1 got last frame, for the overlay */
    int speed = 100; /* percent; audio plays only at 100 */
    bool fast = false; /* fast forward held: unthrottled, no audio */
    double frame_seconds = FRAME_SECONDS_NTSC;

    std::string prefs_path, save_path, shot_path;
    bool save_dir_given = false;
    std::string status;     /* last shortcut's result */
    Uint32 status_until = 0; /* shown over the game until then */

    /* Emulated frames per second, measured while the game runs. */
    double fps = 0.0;
    Uint32 fps_since = 0;
    int fps_frames = 0;
    uint64_t interpreted_last_frame = 0;
};

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t') text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = '\0';
    return text;
}

/* Settings, then bindings as keyboard.<action>.<slot> = <binding> (and
 * controller.*) where they differ from the platform's defaults, so a later
 * change of default reaches everyone who has not rebound that control. */
static void load_prefs(Frontend &f)
{
    n64_input_defaults(f.defaults, f.platform);
    f.map = f.defaults;
    FILE *file = f.prefs_path.empty() ? NULL : fopen(f.prefs_path.c_str(), "r");
    if (!file) return;
    char line[256];
    while (fgets(line, sizeof line, file)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = '\0';
        char *key = trim(line), *value = trim(eq + 1);
        int number = atoi(value);
        if (!strcmp(key, "display.window_scale")) f.scale = clamp_int(number, 1, MAX_SCALE);
        else if (!strcmp(key, "display.fullscreen")) f.fullscreen_saved = number != 0;
        else if (!strcmp(key, "display.scaling")) f.scaling = clamp_int(number, 0, SCALING_STRETCH);
        else if (!strcmp(key, "display.filter")) f.filter = clamp_int(number, 0, FILTER_COUNT - 1);
        else if (!strcmp(key, "display.overlay")) f.show_overlay = number != 0;
        else if (!strcmp(key, "display.fps")) f.show_fps = number != 0;
        else if (!strcmp(key, "display.console_filters")) f.console_filters = number != 0;
        else if (!strcmp(key, "display.internal_scale")) f.internal_scale = number == 2 || number == 4 ? number : 1;
        else if (!strcmp(key, "audio.muted")) f.muted = number != 0;
        else if (!strcmp(key, "audio.master_volume")) f.volume = clamp_int(number, 0, VOLUME_MAX);
        else if (!strncmp(key, "keyboard.", 9) || !strncmp(key, "controller.", 11)) {
            const bool keyboard = key[0] == 'k';
            char *name = key + (keyboard ? 9 : 11), *dot = strrchr(name, '.');
            if (!dot) continue;
            *dot = '\0';
            int slot = atoi(dot + 1);
            N64Binding b;
            if (slot < 0 || slot > 1 || !n64_binding_from_config(value, b)) continue;
            if (b.kind != N64_BIND_NONE && (b.kind == N64_BIND_KEY) != keyboard) continue;
            for (int a = 0; a < N64_ACTION_COUNT; a++) {
                if (!strcmp(name, n64_action_config(a))) (keyboard ? f.map.keyboard : f.map.controller)[a][slot] = b;
            }
        }
    }
    fclose(file);
}

static void save_prefs(const Frontend &f)
{
    FILE *file = f.prefs_path.empty() ? NULL : fopen(f.prefs_path.c_str(), "w");
    if (!file) return;
    fprintf(file, "# Native Console Nintendo 64 settings\n");
    fprintf(file, "display.window_scale = %d\n", f.scale);
    fprintf(file, "display.fullscreen = %d\n", f.fullscreen_saved ? 1 : 0);
    fprintf(file, "display.scaling = %d\n", f.scaling);
    fprintf(file, "display.filter = %d\n", f.filter);
    fprintf(file, "display.overlay = %d\n", f.show_overlay ? 1 : 0);
    fprintf(file, "display.fps = %d\n", f.show_fps ? 1 : 0);
    fprintf(file, "display.console_filters = %d\n", f.console_filters ? 1 : 0);
    fprintf(file, "display.internal_scale = %d\n", f.internal_scale);
    fprintf(file, "audio.muted = %d\n", f.muted ? 1 : 0);
    fprintf(file, "audio.master_volume = %d\n", f.volume);
    for (int a = 0; a < N64_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            if (f.map.keyboard[a][s] != f.defaults.keyboard[a][s]) {
                fprintf(file, "keyboard.%s.%d = %s\n", n64_action_config(a), s,
                        n64_binding_to_config(f.map.keyboard[a][s]).c_str());
            }
            if (f.map.controller[a][s] != f.defaults.controller[a][s]) {
                fprintf(file, "controller.%s.%d = %s\n", n64_action_config(a), s,
                        n64_binding_to_config(f.map.controller[a][s]).c_str());
            }
        }
    }
    fclose(file);
}

/* ------------------------------------------------------------------ */
/* Controls                                                            */
/* ------------------------------------------------------------------ */

static bool binding_down(const Frontend &f, const N64Binding &b, const Uint8 *kb, uint8_t mods)
{
    switch (b.kind) {
    case N64_BIND_KEY: return kb[b.code] && n64_key_mods_match(b, mods);
    case N64_BIND_BUTTON: return f.pad && SDL_GameControllerGetButton(f.pad, (SDL_GameControllerButton)b.code);
    case N64_BIND_AXIS_POSITIVE: return f.pad && SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code) > 16000;
    case N64_BIND_AXIS_NEGATIVE: return f.pad && SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code) < -16000;
    default: return false;
    }
}

static bool action_held(const Frontend &f, int action, const Uint8 *kb, uint8_t mods)
{
    for (int s = 0; s < 2; s++) {
        if (binding_down(f, f.map.keyboard[action][s], kb, mods) || binding_down(f, f.map.controller[action][s], kb, mods))
            return true;
    }
    return false;
}

/* How far one binding pushes the stick in its direction: a key or button
 * all the way, an axis in proportion. */
static int stick_push(const Frontend &f, const N64Binding &b, const Uint8 *kb, uint8_t mods)
{
    if (b.kind == N64_BIND_AXIS_POSITIVE || b.kind == N64_BIND_AXIS_NEGATIVE) {
        if (!f.pad) return 0;
        const int v = SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code);
        return n64_stick_from_axis(b.kind == N64_BIND_AXIS_POSITIVE ? v : -v);
    }
    return binding_down(f, b, kb, mods) ? N64_STICK_MAX : 0;
}

/* The furthest any of a direction's bindings pushes. */
static int stick_direction(const Frontend &f, int action, const Uint8 *kb, uint8_t mods)
{
    int most = 0;
    for (int s = 0; s < 2; s++) {
        const int k = stick_push(f, f.map.keyboard[action][s], kb, mods);
        const int c = stick_push(f, f.map.controller[action][s], kb, mods);
        if (k > most) most = k;
        if (c > most) most = c;
    }
    return most;
}

/* The controller as the player is holding it. The N64's Y axis is up. */
static PadState gameplay_input(const Frontend &f)
{
    const Uint8 *kb = SDL_GetKeyboardState(NULL);
    uint8_t mods = n64_mods_from_sdl(SDL_GetModState());
    PadState p;
    for (int a = 0; a < N64_ACTION_GAMEPLAY_COUNT; a++) {
        if (!n64_action_is_stick(a) && action_held(f, a, kb, mods)) p.buttons |= n64_action_button(a);
    }
    p.x = (int8_t)(stick_direction(f, N64_ACTION_STICK_RIGHT, kb, mods) - stick_direction(f, N64_ACTION_STICK_LEFT, kb, mods));
    p.y = (int8_t)(stick_direction(f, N64_ACTION_STICK_UP, kb, mods) - stick_direction(f, N64_ACTION_STICK_DOWN, kb, mods));
    return p;
}

/* A shortcut pressed and released between two polls still counts. */
static void note_shortcut_press(Frontend &f, const SDL_Event &ev)
{
    for (int a = N64_ACTION_GAMEPLAY_COUNT; a < N64_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            const N64Binding &k = f.map.keyboard[a][s], &c = f.map.controller[a][s];
            if (ev.type == SDL_KEYDOWN && k.kind == N64_BIND_KEY && k.code == ev.key.keysym.scancode &&
                n64_key_mods_match(k, n64_mods_from_sdl(ev.key.keysym.mod)))
                f.action_tapped[a] = true;
            if (ev.type == SDL_CONTROLLERBUTTONDOWN && c.kind == N64_BIND_BUTTON && c.code == ev.cbutton.button)
                f.action_tapped[a] = true;
        }
    }
}

static void assign_binding(Frontend &f, bool keyboard, int action, int slot, const N64Binding &b)
{
    N64Binding(*map)[2] = keyboard ? f.map.keyboard : f.map.controller;
    f.bind_note.clear();
    /* One control does one thing: take the binding off any other action. */
    for (int a = 0; a < N64_ACTION_COUNT && b.kind != N64_BIND_NONE; a++) {
        for (int s = 0; s < 2; s++) {
            if ((a != action || s != slot) && map[a][s] == b) {
                map[a][s].kind = N64_BIND_NONE;
                f.bind_note = n64_binding_label(b, f.platform, f.pad_profile) + " was moved from " + n64_action_label(a) + ".";
            }
        }
    }
    map[action][slot] = b;
    f.input_resync = true;
    save_prefs(f);
}

static void start_capture(Frontend &f, bool keyboard, int action, int slot)
{
    f.capturing = true;
    f.capture_keyboard = keyboard;
    f.capture_action = action;
    f.capture_slot = slot;
    f.capture_modifier = SDL_SCANCODE_UNKNOWN;
    f.pad_nav_blocked = true; /* the button that chose the binding is still down */
}

static void finish_capture(Frontend &f, const N64Binding *b)
{
    if (b) assign_binding(f, f.capture_keyboard, f.capture_action, f.capture_slot, *b);
    f.capturing = false;
    f.pad_nav_blocked = true;
    f.input_resync = true;
}

/* While a binding is being captured, keys and buttons go to it rather than
 * the game, the menu or its shortcuts. Escape and Guide cancel. A modifier
 * pressed and released on its own binds that key; held with another key it
 * makes a combination. */
static bool handle_capture(Frontend &f, const SDL_Event &ev)
{
    if (!f.capturing) return false;
    switch (ev.type) {
    case SDL_KEYDOWN: {
        const int sc = ev.key.keysym.scancode;
        if (ev.key.repeat) return true;
        if (sc == SDL_SCANCODE_ESCAPE) finish_capture(f, NULL);
        else if (!f.capture_keyboard) return true;
        else if (n64_scancode_is_modifier(sc)) f.capture_modifier = sc;
        else {
            N64Binding b = { N64_BIND_KEY, n64_mods_from_sdl(ev.key.keysym.mod), (int16_t)sc };
            finish_capture(f, &b);
        }
        return true;
    }
    case SDL_KEYUP:
        if (f.capture_keyboard && ev.key.keysym.scancode == f.capture_modifier) {
            N64Binding b = { N64_BIND_KEY, 0, (int16_t)f.capture_modifier };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) finish_capture(f, NULL);
        else if (!f.capture_keyboard) {
            N64Binding b = { N64_BIND_BUTTON, 0, (int16_t)ev.cbutton.button };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERAXISMOTION:
        if (!f.capture_keyboard && (ev.caxis.value >= 16000 || ev.caxis.value <= -16000)) {
            N64Binding b = { (uint8_t)(ev.caxis.value > 0 ? N64_BIND_AXIS_POSITIVE : N64_BIND_AXIS_NEGATIVE), 0,
                             (int16_t)ev.caxis.axis };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERBUTTONUP:
    case SDL_TEXTINPUT: return true;
    default: return false;
    }
}

static bool any_pad_input(const Frontend &f)
{
    if (!f.pad) return false;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        if (SDL_GameControllerGetButton(f.pad, (SDL_GameControllerButton)b)) return true;
    }
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
        Sint16 v = SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)a);
        if (v >= 16000 || v <= -16000) return true;
    }
    return false;
}

static void set_pad(Frontend &f, SDL_GameController *pad)
{
    if (f.pad && f.pad != pad) SDL_GameControllerClose(f.pad);
    f.pad = pad;
    f.pad_profile = n64_pad_profile(pad, f.platform);
}

/* Where the game goes on a w x h screen: always the 4:3 shape, whether the
 * frame is 640 x 480 or a PAL 640 x 576. */
static SDL_Rect viewport_for(int scaling, int w, int h)
{
    if (w <= 0) w = ASPECT_W;
    if (h <= 0) h = ASPECT_H;
    double sx = (double)w / ASPECT_W, sy = (double)h / ASPECT_H;
    int vw = w, vh = h;
    switch (scaling) {
    case SCALING_PIXEL_PERFECT: {
        int s = (int)(sx < sy ? sx : sy);
        if (s < 1) s = 1;
        vw = ASPECT_W * s;
        vh = ASPECT_H * s;
        break;
    }
    case SCALING_ASPECT_FIT:
    case SCALING_ASPECT_FILL: {
        double fit = sx < sy ? sx : sy, fill = sx < sy ? sy : sx;
        double s = scaling == SCALING_ASPECT_FIT ? fit : fill;
        vw = (int)(ASPECT_W * s + 0.5);
        vh = (int)(ASPECT_H * s + 0.5);
        break;
    }
    default: break;
    }
    SDL_Rect r;
    r.x = (w - vw) / 2;
    r.y = (h - vh) / 2;
    r.w = vw < 1 ? 1 : vw;
    r.h = vh < 1 ? 1 : vh;
    return r;
}

/* Where the game goes inside the window (renderer output pixels). */
static void update_viewport(Frontend &f)
{
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(f.ren, &w, &h);
    f.viewport = viewport_for(f.scaling, w, h);
}

static void apply_filter(Frontend &f)
{
    SDL_SetTextureScaleMode(f.tex, f.filter == FILTER_LINEAR ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}

static int wanted_internal_scale(const Frontend &f)
{
    const int scale = f.internal_scale_this_run > 0 ? f.internal_scale_this_run : f.internal_scale;
    return scale == 2 || scale == 4 ? scale : 1;
}

static void apply_internal_scale(Frontend &f)
{
    if (f.m && n64_internal_scale(f.m) != wanted_internal_scale(f)) n64_set_internal_scale(f.m, wanted_internal_scale(f));
}

/* The larger picture as a texture, or NULL when the last field has none
 * (Internal Resolution at 1x, a blanked screen). */
static SDL_Texture *large_texture(Frontend &f, const SDL_Rect &dest)
{
    int w = 0, h = 0;
    const uint32_t *large = n64_large_frame(f.m, &w, &h);
    if (!large || w < 1 || h < 1) return nullptr;
    if (!f.large_tex || f.large_w != w || f.large_h != h) {
        if (f.large_tex) SDL_DestroyTexture(f.large_tex);
        f.large_tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w, h);
        f.large_w = w;
        f.large_h = h;
    }
    if (!f.large_tex) return nullptr;
    /* Nearest keeps pixels whole only where the picture goes into its place
     * a whole number of times; anywhere else, and wherever the picture is
     * the bigger of the two, it would drop or double lines of it. */
    const bool whole = dest.w >= w && dest.h >= h && dest.w % w == 0 && dest.h % h == 0;
    SDL_SetTextureScaleMode(f.large_tex, f.filter == FILTER_NEAREST && whole ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    SDL_UpdateTexture(f.large_tex, NULL, large, w * 4);
    return f.large_tex;
}

/* The frame texture: the framebuffer's full storage, of which the VI's
 * picture uses the top-left out_width x out_height. */
static SDL_Texture *create_frame_texture(SDL_Renderer *ren)
{
    return SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, N64_FB_WIDTH, N64_FB_HEIGHT);
}

/* Draws the current frame into dest through the chosen filter. */
static void draw_game(Frontend &f, const SDL_Rect &dest)
{
    if (SDL_Texture *large = large_texture(f, dest)) {
        SDL_RenderCopy(f.ren, large, NULL, &dest);
        return;
    }
    SDL_Rect used = { 0, 0, frame_width(f.m), frame_height(f.m) };
    SDL_UpdateTexture(f.tex, &used, f.m->framebuffer, N64_FB_WIDTH * 4);
    SDL_RenderCopy(f.ren, f.tex, &used, &dest);
}

/* --display-shot: the frame as the display shows it on a w x h screen. */
static bool save_display_shot(Frontend &f, const char *path, int w, int h)
{
    SDL_Texture *shot = SDL_RenderTargetSupported(f.ren)
                            ? SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h)
                            : nullptr;
    if (!shot) {
        fprintf(stderr, "display shot: %s\n", SDL_GetError());
        return false;
    }
    std::vector<uint32_t> pixels((size_t)w * h);
    SDL_SetRenderTarget(f.ren, shot);
    SDL_SetRenderDrawColor(f.ren, 0, 0, 0, 255);
    SDL_RenderClear(f.ren);
    draw_game(f, viewport_for(f.scaling, w, h));
    const bool read = SDL_RenderReadPixels(f.ren, NULL, SDL_PIXELFORMAT_RGB888, pixels.data(), w * 4) == 0;
    SDL_SetRenderTarget(f.ren, NULL);
    SDL_DestroyTexture(shot);
    SDL_Surface *s = read ? SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_RGB888)
                          : nullptr;
    const bool ok = s && SDL_SaveBMP(s, path) == 0;
    if (s) SDL_FreeSurface(s);
    if (ok) printf("display shot: %s (%dx%d, %s)\n", path, w, h, k_filter_names[f.filter]);
    else fprintf(stderr, "display shot: %s\n", SDL_GetError());
    return ok;
}

static void set_fullscreen(Frontend &f, bool on)
{
    if (SDL_SetWindowFullscreen(f.win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0) f.fullscreen = on;
    if (f.fullscreen_saved != f.fullscreen) {
        f.fullscreen_saved = f.fullscreen;
        save_prefs(f);
    }
    update_viewport(f);
}

static void apply_window_scale(Frontend &f)
{
    if (!f.fullscreen) {
        SDL_SetWindowSize(f.win, ASPECT_W * f.scale, ASPECT_H * f.scale);
        SDL_SetWindowPosition(f.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    update_viewport(f);
}

static void reset_display(Frontend &f)
{
    f.scale = DEFAULT_SCALE;
    f.scaling = SCALING_PIXEL_PERFECT;
    f.filter = FILTER_NEAREST;
    f.show_overlay = false;
    f.show_fps = false;
    f.console_filters = true;
    f.m->vi_raw = false;
    f.internal_scale = 1;
    f.internal_scale_this_run = 0;
    apply_internal_scale(f);
    f.speed = 100;
    apply_filter(f);
    if (f.fullscreen) set_fullscreen(f, false);
    apply_window_scale(f);
    save_prefs(f);
}

/* The game runs only while the menu is closed; the audio device pauses with
 * it so what is queued plays on when the game resumes. Enter or a face
 * button that chose Resume is still down then, and must not reach the game;
 * nor must a stick that was moving the menu's cursor. */
static void set_menu(Frontend &f, bool open)
{
    f.show_menu = open;
    if (f.audio) SDL_PauseAudioDevice(f.audio, open ? 1 : 0);
    if (!open) {
        const PadState held = gameplay_input(f);
        f.held_from_menu = held.buttons;
        f.stick_from_menu = held.x != 0 || held.y != 0;
    }
    if (!open && f.capturing) finish_capture(f, NULL);
}

static void set_status(Frontend &f, const std::string &message)
{
    f.status = message;
    f.status_until = SDL_GetTicks() + 2500;
    printf("%s\n", message.c_str());
}

/* Fires shortcuts pressed since the last call and updates fast forward.
 * Runs once per loop, after events, whether or not the game is paused. */
static void run_shortcuts(Frontend &f)
{
    const Uint8 *kb = SDL_GetKeyboardState(NULL);
    const uint8_t mods = n64_mods_from_sdl(SDL_GetModState());
    const bool baseline = f.input_resync || f.capturing;
    f.input_resync = false;
    for (int a = N64_ACTION_GAMEPLAY_COUNT; a < N64_ACTION_COUNT; a++) {
        const bool held = action_held(f, a, kb, mods);
        const bool pressed = (held || f.action_tapped[a]) && !f.action_down[a];
        f.action_down[a] = held;
        f.action_tapped[a] = false;
        if (!pressed || baseline) continue;
        switch (a) {
        case N64_ACTION_OVERLAY:
            f.show_overlay = !f.show_overlay;
            save_prefs(f);
            break;
        case N64_ACTION_FPS:
            f.show_fps = !f.show_fps;
            save_prefs(f);
            break;
        case N64_ACTION_MUTE:
            f.muted = !f.muted;
            save_prefs(f);
            break;
        case N64_ACTION_MENU: set_menu(f, !f.show_menu); break;
        case N64_ACTION_FULLSCREEN: set_fullscreen(f, !f.fullscreen); break;
        case N64_ACTION_SCREENSHOT:
            if (screenshot(f.m, f.shot_path.c_str())) set_status(f, "Screenshot saved to " + f.shot_path);
            else set_status(f, "Could not write " + f.shot_path);
            break;
        default: break;
        }
    }
    f.fast = f.action_down[N64_ACTION_FAST_FORWARD] && !f.capturing;
}

static void request_quit(void)
{
    SDL_Event quit = {};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
}

static float ui_scale_for_size(const ImVec2 &size)
{
    float min_dimension = size.x < size.y ? size.x : size.y;
    if (min_dimension <= 0.0f) return 1.0f;
    float scale = min_dimension / 720.0f;
    return scale < 0.95f ? 0.95f : scale > 1.25f ? 1.25f : scale;
}

static double audio_queue_ms(const Frontend &f)
{
    if (!f.audio) return 0.0;
    double bytes_per_ms = f.have.freq * f.have.channels * (SDL_AUDIO_BITSIZE(f.have.format) / 8) / 1000.0;
    return SDL_GetQueuedAudioSize(f.audio) / bytes_per_ms;
}

/* The first binding of an action, for hints such as "Use F1 for Overlay":
 * keyboard, else controller; empty when unbound. */
static std::string shortcut_hint(const Frontend &f, int action)
{
    for (int device = 0; device < 2; device++) {
        for (int s = 0; s < 2; s++) {
            const N64Binding &b = (device == 0 ? f.map.keyboard : f.map.controller)[action][s];
            if (b.kind != N64_BIND_NONE) return n64_binding_label(b, f.platform, f.pad_profile);
        }
    }
    return std::string();
}

/* One row per action with two binding buttons; choosing one waits for the
 * new key or button (handle_capture). */
static void draw_binding_table(Frontend &f, const char *title, bool keyboard, int first, int last)
{
    ImGui::TextDisabled("%s", title);
    if (!ImGui::BeginTable(title, 3, ImGuiTableFlags_RowBg)) return;
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("First", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Second", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    const ImGuiStyle &style = ImGui::GetStyle();
    const float clear_width = ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f;
    for (int a = first; a < last; a++) {
        ImGui::PushID(a);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(n64_action_label(a));
        for (int s = 0; s < 2; s++) {
            ImGui::TableNextColumn();
            ImGui::PushID(s);
            const N64Binding b = (keyboard ? f.map.keyboard : f.map.controller)[a][s];
            const bool waiting = f.capturing && f.capture_keyboard == keyboard && f.capture_action == a &&
                                 f.capture_slot == s;
            const std::string label =
                (waiting ? std::string("Press...") : n64_binding_label(b, f.platform, f.pad_profile)) + "###bind";
            float width = ImGui::GetContentRegionAvail().x - clear_width - style.ItemSpacing.x;
            if (ImGui::Button(label.c_str(), ImVec2(width > 40.0f ? width : 40.0f, 0.0f))) start_capture(f, keyboard, a, s);
            ImGui::SameLine();
            ImGui::BeginDisabled(b.kind == N64_BIND_NONE);
            if (ImGui::Button("Clear")) {
                N64Binding none = { N64_BIND_NONE, 0, 0 };
                assign_binding(f, keyboard, a, s, none);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void draw_menu(Frontend &f)
{
    ImGuiIO &io = ImGui::GetIO();
    const float ui_scale = io.FontGlobalScale;
    const float footer_height = ImGui::GetFrameHeightWithSpacing() * 3.0f;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * ui_scale, 16.0f * ui_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * ui_scale, 10.0f * ui_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * ui_scale, 10.0f * ui_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 18.0f * ui_scale);
    ImGui::Begin("Native Console", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("%s", f.title.c_str());
    ImGui::TextDisabled("Nintendo 64 Settings");
    ImGui::SameLine();
    if (ImGui::Button("Resume")) set_menu(f, false);
    ImGui::Separator();
    ImGui::TextWrapped("%s", n64_menu_hint(f.platform));
    ImGui::TextWrapped("The game is paused while the menu is open.");
    ImGui::Separator();
    /* Flattened so arrow keys and the D-pad move into the settings and
     * scroll them, not just between Resume and the footer buttons. */
    ImGui::BeginChild("SettingsScroll", ImVec2(0.0f, -footer_height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NavFlattened);

    ImGui::Text("Performance: %.1f FPS (%s, %.2f Hz)", f.fps, f.m->cart.pal ? "PAL" : "NTSC", 1.0 / f.frame_seconds);
    if (f.m->rc_program) {
        ImGui::Text("Recompiled code: %u entry points", f.m->rc_program->entry_count);
    } else {
        ImGui::Text("Recompiled code: not in use (interpreter only)");
    }
    ImGui::Text("Interpreted instructions: %llu last frame, %llu total",
                (unsigned long long)f.interpreted_last_frame, (unsigned long long)f.m->cpu.interpreted);

    int window_w = 0, window_h = 0;
    SDL_GetWindowSize(f.win, &window_w, &window_h);
    ImGui::Separator();
    ImGui::TextDisabled("Graphics");
    ImGui::Text("Window: %d x %d", window_w, window_h);
    int picture_w = frame_width(f.m), picture_h = frame_height(f.m);
    n64_large_frame(f.m, &picture_w, &picture_h);
    ImGui::Text("Picture: %d x %d, shown at %d x %d (4:3)", picture_w, picture_h, f.viewport.w, f.viewport.h);
    bool fullscreen = f.fullscreen;
    if (ImGui::Checkbox("Fullscreen", &fullscreen)) set_fullscreen(f, fullscreen);
    if (ImGui::Combo("Scaling Mode", &f.scaling, k_scaling_names, IM_ARRAYSIZE(k_scaling_names))) {
        update_viewport(f);
        save_prefs(f);
    }
    if (ImGui::Combo("Scale Filter", &f.filter, k_filter_names, IM_ARRAYSIZE(k_filter_names))) {
        apply_filter(f);
        save_prefs(f);
    }
    /* What the console's video output does to the framebuffer: anti-aliasing,
     * the dither filter, interpolation. Off shows the framebuffer's own
     * pixels, sharper and grainier. */
    if (ImGui::Checkbox("Console Video Filters", &f.console_filters)) {
        f.m->vi_raw = !f.console_filters;
        save_prefs(f);
    }
    /* The RDP's second, larger picture; the game still gets the console's. */
    {
        int index = 0;
        for (int i = 0; i < IM_ARRAYSIZE(k_internal_scales); i++) {
            if (k_internal_scales[i] == wanted_internal_scale(f)) index = i;
        }
        if (ImGui::Combo("Internal Resolution", &index, k_internal_scale_names, IM_ARRAYSIZE(k_internal_scale_names))) {
            f.internal_scale = k_internal_scales[index];
            f.internal_scale_this_run = 0;
            apply_internal_scale(f);
            save_prefs(f);
        }
    }
    if (!f.fullscreen) {
        int scale_index = f.scale - 1;
        if (ImGui::Combo("Window Size", &scale_index, k_scale_names, IM_ARRAYSIZE(k_scale_names))) {
            f.scale = scale_index + 1;
            apply_window_scale(f);
            save_prefs(f);
        }
    } else {
        ImGui::TextDisabled("Window Size is disabled while fullscreen is active.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Runtime");
    if (ImGui::Checkbox("Show FPS Counter", &f.show_fps)) save_prefs(f);
    if (ImGui::Checkbox("Show Overlay", &f.show_overlay)) save_prefs(f);
    ImGui::SliderInt("Speed %", &f.speed, 10, 500);
    if (ImGui::Button("Reset Speed")) f.speed = 100;
    const std::string fast_forward = shortcut_hint(f, N64_ACTION_FAST_FORWARD);
    if (!fast_forward.empty()) ImGui::TextDisabled("Hold %s to fast forward at full speed.", fast_forward.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Audio");
    if (ImGui::Checkbox("Mute", &f.muted)) save_prefs(f);
    if (f.mute_this_run) ImGui::TextDisabled("Silenced for this run by --mute.");
    if (ImGui::SliderInt("Master Volume (%)", &f.volume, 0, VOLUME_MAX)) save_prefs(f);
    if (f.audio) {
        ImGui::Text("Output: %s, %d Hz, %d-frame device buffer", SDL_GetCurrentAudioDriver(), f.have.freq,
                    f.have.samples);
        ImGui::Text("Queued: %.0f ms", audio_queue_ms(f));
    } else {
        ImGui::TextDisabled("No audio output device is available.");
    }
    if (f.speed != 100) ImGui::TextDisabled("Audio output pauses while Speed %% is not 100.");

    ImGui::Separator();
    ImGui::TextDisabled("Saves");
    ImGui::TextWrapped("The game saves to its cartridge and Controller Paks as on the console: %s",
                       f.save_path.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Controls");
    static const char *const platform_names[] = { "Windows", "Mac", "Steam Deck" };
    ImGui::Text("Default layout: %s", platform_names[f.platform]);
    if (f.pad) {
        const char *name = SDL_GameControllerName(f.pad);
        ImGui::Text("Detected Controller: %s", name ? name : "(unnamed)");
    } else {
        ImGui::TextDisabled("No controller detected.");
    }
    ImGui::TextWrapped("Choose a binding, then press the new key or button. Escape or Guide cancels. Hold %s with a "
                       "key to bind a combination. A controller stick bound to a stick direction moves the N64 stick "
                       "in proportion; a key or button pushes it all the way.",
                       f.platform == N64_PLATFORM_MAC ? "Command, Control, Option or Shift" : "Ctrl, Alt or Shift");
    if (f.capturing) {
        ImGui::Text("Waiting for a %s for %s...", f.capture_keyboard ? "key" : "controller button",
                    n64_action_label(f.capture_action));
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) finish_capture(f, NULL);
    }
    if (!f.bind_note.empty()) ImGui::TextWrapped("%s", f.bind_note.c_str());
    draw_binding_table(f, "Keyboard Gameplay", true, 0, N64_ACTION_GAMEPLAY_COUNT);
    draw_binding_table(f, "Controller Gameplay", false, 0, N64_ACTION_GAMEPLAY_COUNT);
    draw_binding_table(f, "Keyboard Shortcuts", true, N64_ACTION_GAMEPLAY_COUNT, N64_ACTION_COUNT);
    draw_binding_table(f, "Controller Shortcuts", false, N64_ACTION_GAMEPLAY_COUNT, N64_ACTION_COUNT);
    ImGui::TextDisabled("Escape and the controller's Guide button always open the menu as well.");
    if (ImGui::Button("Reset Controls")) {
        f.map = f.defaults;
        f.bind_note = "Controls reset to the " + std::string(platform_names[f.platform]) + " defaults.";
        f.input_resync = true;
        save_prefs(f);
    }

    ImGui::EndChild();
    ImGui::Separator();
    const float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    if (ImGui::Button("Reset Display", ImVec2(button_width, 0.0f))) reset_display(f);
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(button_width, 0.0f))) request_quit();
    ImGui::End();
    ImGui::PopStyleVar(6);
}

static void draw_overlay(const Frontend &f)
{
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);
    if (ImGui::Begin("Overlay", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
        ImGui::Text("FPS: %.1f", f.fps);
        ImGui::TextUnformatted(n64_menu_hint(f.platform));
        const std::string overlay = shortcut_hint(f, N64_ACTION_OVERLAY);
        if (!overlay.empty()) ImGui::Text("Use %s for Overlay", overlay.c_str());
        ImGui::Text("Viewport: %d x %d (%s)", f.viewport.w, f.viewport.h, k_scaling_names[f.scaling]);
        ImGui::Text("Speed: %s", f.fast ? "fast forward" : (std::to_string(f.speed) + "%").c_str());
        if (f.audio) ImGui::Text("Audio queued: %.0f ms", audio_queue_ms(f));
        ImGui::Text("Stick: %d, %d  Buttons: %04X", f.sent.x, f.sent.y, f.sent.buttons);
        ImGui::Text("Interpreted: %llu instructions/frame", (unsigned long long)f.interpreted_last_frame);
    }
    ImGui::End();
}

/* The FPS counter: one line in the window's top right corner, clear of the
 * overlay. The rate is the console's own, so the percentage beside it is the
 * game's speed, 100% being full speed whatever that rate is. */
static void draw_fps(const Frontend &f)
{
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f, 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (ImGui::Begin("FpsCounter", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
        /* The rate is measured once a second, so there is none in the first. */
        if (f.fps > 0.0) ImGui::Text("%.0f FPS  %.0f%%", f.fps, f.fps * f.frame_seconds * 100.0);
        else ImGui::TextUnformatted("-- FPS");
    }
    ImGui::End();
}

static void draw_status(const Frontend &f)
{
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(10.0f, io.DisplaySize.y - 10.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (ImGui::Begin("Status", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
        ImGui::TextUnformatted(f.status.c_str());
    }
    ImGui::End();
}

static void present(Frontend &f)
{
    SDL_SetRenderDrawColor(f.ren, 0, 0, 0, 255);
    SDL_RenderClear(f.ren);
    draw_game(f, f.viewport);

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.FontGlobalScale = ui_scale_for_size(io.DisplaySize);
    if (f.show_menu) {
        draw_menu(f);
    } else {
        if (f.show_overlay) draw_overlay(f);
        if (f.show_fps) draw_fps(f);
        if (!SDL_TICKS_PASSED(SDL_GetTicks(), f.status_until)) draw_status(f);
    }
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
    SDL_RenderPresent(f.ren);
}

/* ------------------------------------------------------------------ */
/* Menu self-test                                                      */
/* ------------------------------------------------------------------ */

/* --menu-selftest drives the overlay menu with synthetic input through SDL's
 * event queue, as a keyboard and a controller would deliver it: Escape, F10
 * (Cmd+, on a Mac), then the controller's Guide button, then L3 (the Steam
 * Deck's way in). It checks that each opens and closes the menu and that the
 * game stops while the menu is open, sets the volume to 37 the way the
 * menu's slider does, and rebinds a key the way the Controls table does,
 * choosing one another action has so that it is also taken off that action:
 * the new binding must reach the preferences file, and putting the old ones
 * back must leave the file's bindings as they were found. It checks the
 * stick's scaling, and, given --save-dir, that the cartridge save is written
 * there whole. A second run with --menu-selftest-verify checks the volume
 * was kept. Pass --prefs FILE so the player's own settings are not
 * touched. */
enum { SELFTEST_VOLUME = 37 };

struct MenuSelfTest {
    bool enabled = false;
    int loop = 0;
    uint64_t frame_mark = 0;
    int failures = 0;
    N64InputMap found = {}; /* the bindings before the rebinding test */
};

/* A key pressed and released, with N64_MOD_* modifiers held. */
static void push_key(SDL_Scancode sc, uint8_t mods = 0)
{
    SDL_Event e = {};
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    e.key.keysym.mod = (Uint16)(((mods & N64_MOD_CTRL) ? KMOD_LCTRL : 0) | ((mods & N64_MOD_SHIFT) ? KMOD_LSHIFT : 0) |
                                ((mods & N64_MOD_ALT) ? KMOD_LALT : 0) | ((mods & N64_MOD_GUI) ? KMOD_LGUI : 0));
    SDL_PushEvent(&e);
    e.type = SDL_KEYUP;
    SDL_PushEvent(&e);
}

static void push_button(SDL_GameControllerButton button)
{
    SDL_Event e = {};
    e.type = SDL_CONTROLLERBUTTONDOWN;
    e.cbutton.button = (Uint8)button;
    e.cbutton.state = SDL_PRESSED;
    SDL_PushEvent(&e);
    e.type = SDL_CONTROLLERBUTTONUP;
    e.cbutton.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

static void selftest_check(MenuSelfTest &t, bool ok, const char *what)
{
    printf("menu selftest: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) t.failures++;
}

static bool same_bindings(const N64InputMap &a, const N64InputMap &b)
{
    for (int action = 0; action < N64_ACTION_COUNT; action++) {
        for (int s = 0; s < 2; s++) {
            if (a.keyboard[action][s] != b.keyboard[action][s] || a.controller[action][s] != b.controller[action][s])
                return false;
        }
    }
    return true;
}

/* The bindings as the preferences file has them now. */
static N64InputMap bindings_on_disk(const Frontend &f)
{
    Frontend read;
    read.platform = f.platform;
    read.prefs_path = f.prefs_path;
    load_prefs(read);
    return read.map;
}

/* The save file holds exactly what the machine exports. */
static bool save_file_matches(N64Machine *m, const std::string &path)
{
    size_t n = 0;
    uint8_t *data = read_file(path.c_str(), &n);
    std::vector<uint8_t> image(n64_save_size(m));
    if (!image.empty()) n64_save_export(m, image.data());
    const bool same = data && n == image.size() && !memcmp(data, image.data(), n);
    free(data);
    return same;
}

/* One step per loop, before events are polled; false when done. Each input
 * is given 30 loops (half a second) to take effect. */
static bool menu_selftest_step(Frontend &f, MenuSelfTest &t)
{
    const N64Machine *m = f.m;
    /* The rebinding test's key: D-pad Up's (T), so it is moved from there. */
    const int taken_action = N64_ACTION_DPAD_UP;
    const N64Binding test_key = f.defaults.keyboard[taken_action][0];
    /* The menu shortcut as bound: F10, or Cmd+, on a Mac. */
    const N64Binding menu_key = f.map.keyboard[N64_ACTION_MENU][0];
    const bool has_menu_key = menu_key.kind == N64_BIND_KEY;
    switch (t.loop++) {
    case 60: push_key(SDL_SCANCODE_ESCAPE); break;
    case 62:
        selftest_check(t, f.show_menu, "Escape opens the menu");
        t.frame_mark = m->frame_count;
        break;
    case 92:
        selftest_check(t, m->frame_count == t.frame_mark, "the game is paused while the menu is open");
        push_key(SDL_SCANCODE_ESCAPE);
        break;
    case 94:
        selftest_check(t, !f.show_menu, "Escape closes the menu");
        t.frame_mark = m->frame_count;
        break;
    case 100: push_key(SDL_SCANCODE_F9); break;
    case 102: selftest_check(t, f.show_fps, "F9 turns the FPS counter on"); break;
    case 124:
        selftest_check(t, m->frame_count > t.frame_mark, "the game runs again");
        if (has_menu_key) push_key((SDL_Scancode)menu_key.code, menu_key.mods);
        break;
    case 126:
        if (has_menu_key) selftest_check(t, f.show_menu, "the menu shortcut (F10; Cmd+, on a Mac) opens the menu");
        break;
    case 128:
        if (has_menu_key) push_key((SDL_Scancode)menu_key.code, menu_key.mods);
        break;
    case 130:
        if (has_menu_key) selftest_check(t, !f.show_menu, "the menu shortcut closes the menu");
        push_button(SDL_CONTROLLER_BUTTON_GUIDE);
        break;
    case 132:
        selftest_check(t, f.show_menu, "a controller's Guide button opens the menu");
        t.frame_mark = m->frame_count;
        break;
    case 162:
        selftest_check(t, m->frame_count == t.frame_mark, "the game is paused (opened from the controller)");
        f.volume = SELFTEST_VOLUME; /* as the menu's volume slider sets it */
        save_prefs(f);
        set_fullscreen(f, true); /* as its Fullscreen box does */
        /* As the Controls table does: choose A's second key, then press the
         * key D-pad Up has. */
        t.found = f.map;
        start_capture(f, true, N64_ACTION_A, 1);
        push_key((SDL_Scancode)test_key.code);
        break;
    case 164: {
        selftest_check(t, !f.capturing && f.map.keyboard[N64_ACTION_A][1] == test_key,
                       "a key pressed for a waiting binding is bound");
        selftest_check(t, f.map.keyboard[taken_action][0].kind == N64_BIND_NONE && !f.bind_note.empty(),
                       "and is taken from the action that had it");
        selftest_check(t, f.show_menu, "that key does nothing else in the menu");
        const N64InputMap disk = bindings_on_disk(f);
        selftest_check(t, disk.keyboard[N64_ACTION_A][1] == test_key && disk.keyboard[taken_action][0].kind == N64_BIND_NONE,
                       "the new bindings are in the preferences file");
        /* Put back whatever the capture changed, and write that. */
        f.map = t.found;
        f.bind_note.clear();
        f.input_resync = true;
        save_prefs(f);
        selftest_check(t, same_bindings(bindings_on_disk(f), t.found), "the bindings are saved back as they were found");
        push_button(SDL_CONTROLLER_BUTTON_GUIDE);
        break;
    }
    case 166:
        selftest_check(t, !f.show_menu, "Guide closes the menu");
        selftest_check(t, n64_stick_from_axis(0) == 0 && n64_stick_from_axis(N64_STICK_DEADZONE) == 0 &&
                              n64_stick_from_axis(32767) == N64_STICK_MAX && n64_stick_from_axis(32768) == N64_STICK_MAX &&
                              n64_stick_from_axis(20000) > 0 && n64_stick_from_axis(20000) < N64_STICK_MAX,
                       "a host stick moves the N64 stick past its dead zone, up to 80 at full deflection");
        break;
    case 168: push_button(SDL_CONTROLLER_BUTTON_LEFTSTICK); break;
    case 170: selftest_check(t, f.show_menu, "L3 opens the menu (Steam Deck)"); break;
    case 172: push_button(SDL_CONTROLLER_BUTTON_LEFTSTICK); break;
    case 174:
        selftest_check(t, !f.show_menu, "L3 closes the menu");
        /* The cartridge save, into the scratch --save-dir only: without one
         * it would be the player's own file. */
        if (f.save_dir_given) {
            selftest_check(t, write_save(f.m, f.save_path.c_str()) && save_file_matches(f.m, f.save_path),
                           "the cartridge save is written to --save-dir whole");
        }
        printf("menu selftest: %s\n", t.failures ? "FAIL" : "PASS");
        return false;
    default: break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int n64_sdl_main(int argc, char **argv, const N64RcProgram *program, const char *default_rom)
{
    const char *rom_path = NULL, *save_dir = NULL, *shot_prefix = NULL, *prefs_file = NULL, *display_shot = NULL;
    int cli_scale = 0, cli_internal_scale = 0, display_w = 3840, display_h = 2160;
    long limit_frames = -1;
    bool cli_fullscreen = false, cli_muted = false, interpreter_only = false, audio_stats = false;
    bool selftest_verify = false, no_learn = false, cli_vi_raw = false;
    MenuSelfTest selftest;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--scale") && i + 1 < argc) cli_scale = atoi(argv[++i]);
        else if (!strcmp(a, "--prefs") && i + 1 < argc) prefs_file = argv[++i];
        else if (!strcmp(a, "--display-shot") && i + 1 < argc) display_shot = argv[++i];
        else if (!strcmp(a, "--display-size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &display_w, &display_h) != 2 || display_w < 1 || display_h < 1) {
                fprintf(stderr, "--display-size wants WxH\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--menu-selftest")) selftest.enabled = true;
        else if (!strcmp(a, "--menu-selftest-verify")) selftest_verify = true;
        else if (!strcmp(a, "--save-dir") && i + 1 < argc) save_dir = argv[++i];
        else if (!strcmp(a, "--fullscreen")) cli_fullscreen = true;
        else if (!strcmp(a, "--mute")) cli_muted = true;
        else if (!strcmp(a, "--audio-stats")) audio_stats = true;
        else if (!strcmp(a, "--interpreter")) interpreter_only = true;
        else if (!strcmp(a, "--no-learn")) no_learn = true;
        else if (!strcmp(a, "--vi-raw")) cli_vi_raw = true;
        else if (!strcmp(a, "--internal-scale") && i + 1 < argc) {
            cli_internal_scale = atoi(argv[++i]);
            if (cli_internal_scale != 1 && cli_internal_scale != 2 && cli_internal_scale != 4) {
                fprintf(stderr, "--internal-scale wants 1, 2 or 4\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--limit-frames") && i + 1 < argc) limit_frames = atol(argv[++i]);
        else if (!strcmp(a, "--screenshot-prefix") && i + 1 < argc) shot_prefix = argv[++i];
        else if (a[0] != '-' && !rom_path) rom_path = a;
        else {
            fprintf(stderr, "unknown argument %s\n", a);
            return 2;
        }
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* A ROM given on the command line wins; then the ROM compiled into the
     * program; then a default file next to the executable. */
    char default_path[1024] = "";
    bool embedded = !rom_path && program && program->embedded_rom;
    if (!rom_path && !embedded && default_rom) {
        char *base = SDL_GetBasePath();
        snprintf(default_path, sizeof default_path, "%s%s", base ? base : "", default_rom);
        SDL_free(base);
        rom_path = default_path;
    }
    if (!rom_path && !embedded) {
        fprintf(stderr, "usage: %s ROM [--scale N] [--save-dir DIR] [--fullscreen] [--mute]\n", argv[0]);
        quit_sdl();
        return 2;
    }

    N64Machine *m = n64_create();
    size_t size = 0;
    bool loaded = false;
    if (m && embedded) {
        const uint8_t *data = program->embedded_rom(&size);
        loaded = data && n64_load_rom(m, data, size);
    } else if (m) {
        uint8_t *rom = read_file(rom_path, &size);
        loaded = rom && n64_load_rom(m, rom, size);
        free(rom);
    }
    if (!loaded) {
        char msg[1100];
        snprintf(msg, sizeof msg, "Cannot load the ROM:\n%s", embedded ? "(embedded)" : rom_path);
        fprintf(stderr, "%s\n", msg);
        /* No dialog in a scripted run: nobody is there to close it. */
        const char *driver = SDL_GetCurrentVideoDriver();
        if (!selftest.enabled && !selftest_verify && limit_frames < 0 && !(driver && !strcmp(driver, "dummy")))
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Native Console", msg, NULL);
        if (m) n64_destroy(m);
        quit_sdl();
        return 1;
    }
    /* Saves and screenshots are named after the executable for an
     * embedded ROM (the hub keeps <game id>.sav next to <game id>.exe). */
    if (embedded) rom_path = argv[0];

    Frontend f;
    f.m = m;
    char save_path[1100], shot_path[1100], trace_path[1100];
    if (save_dir) make_dir(save_dir); /* fails harmlessly if it exists */
    sibling_path(save_path, sizeof save_path, save_dir, rom_path, ".sav");
    sibling_path(shot_path, sizeof shot_path, save_dir, rom_path, ".bmp");
    sibling_path(trace_path, sizeof trace_path, save_dir, rom_path, ".trace");
    f.save_path = save_path;
    f.shot_path = shot_path;
    f.save_dir_given = save_dir != NULL;
    /* The save goes in before power-on, as the cartridge's memory is there
     * when the console starts. */
    read_save(m, save_path);
    n64_power(m);
    if (program && !interpreter_only && !n64_rc_install(m, program)) {
        fprintf(stderr, "%s does not match the ROM this program was built from; running interpreted\n", rom_path);
    }
    /* A recompiled game learns from being played: the code its build's
     * profile never reached runs in the interpreter, which notes where it
     * was entered and the RDRAM it ran from. On exit that is added to
     * <save name>.trace, and the next build compiles it (the hub passes
     * the file to n64recomp). */
    const bool learn = m->rc_program && !no_learn && !selftest.enabled;
    if (learn) n64_coverage_enable_light(m);
    f.frame_seconds = m->cart.pal ? FRAME_SECONDS_PAL : FRAME_SECONDS_NTSC;

    if (prefs_file) {
        f.prefs_path = prefs_file;
    } else {
        char *pref_dir = SDL_GetPrefPath("gbrecompiled", "runtime-n64");
        if (pref_dir) f.prefs_path = std::string(pref_dir) + "n64_prefs.ini";
        SDL_free(pref_dir);
    }
    load_prefs(f);
    m->vi_raw = cli_vi_raw || !f.console_filters; /* --vi-raw: this run only */
    f.internal_scale_this_run = cli_internal_scale;
    apply_internal_scale(f);
    if (selftest_verify) {
        const bool volume_kept = f.volume == SELFTEST_VOLUME;
        const bool kept = volume_kept && f.show_fps && f.fullscreen_saved;
        printf("menu selftest: %s the volume set in the menu is %d after a restart\n", volume_kept ? "ok  " : "FAIL",
               f.volume);
        printf("menu selftest: %s the FPS counter is still on after a restart\n", f.show_fps ? "ok  " : "FAIL");
        printf("menu selftest: %s Fullscreen, turned on in the menu, is %s after a restart\n",
               f.fullscreen_saved ? "ok  " : "FAIL", f.fullscreen_saved ? "on" : "off");
        printf("menu selftest: %s\n", kept ? "PASS" : "FAIL");
        n64_destroy(m);
        quit_sdl();
        return kept ? 0 : 1;
    }
    if (cli_scale > 0) f.scale = clamp_int(cli_scale, 1, MAX_SCALE);
    f.mute_this_run = cli_muted;
    f.fullscreen = cli_fullscreen || f.fullscreen_saved;

    /* The header's title is space-padded to 20 characters. */
    std::string cart_title = m->cart.title;
    while (!cart_title.empty() && (cart_title.back() == ' ' || cart_title.back() == '\0')) cart_title.pop_back();
    f.title = !cart_title.empty() ? cart_title : "Nintendo 64";
    char title[128];
    snprintf(title, sizeof title, "%s - Native Console", f.title.c_str());
    f.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ASPECT_W * f.scale,
                             ASPECT_H * f.scale,
                             SDL_WINDOW_RESIZABLE | (f.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    f.ren = f.win ? SDL_CreateRenderer(f.win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (f.win && !f.ren) f.ren = SDL_CreateRenderer(f.win, -1, SDL_RENDERER_SOFTWARE); /* no GPU (VMs, remote desktop) */
    f.tex = f.ren ? create_frame_texture(f.ren) : NULL;
    if (!f.tex) {
        fprintf(stderr, "SDL video: %s\n", SDL_GetError());
        n64_destroy(m);
        quit_sdl();
        return 1;
    }
    SDL_SetWindowMinimumSize(f.win, ASPECT_W, ASPECT_H);
    apply_filter(f);
    update_viewport(f);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL; /* every window is placed each frame; nothing to remember */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(f.win, f.ren);
    ImGui_ImplSDLRenderer2_Init(f.ren);

    SDL_AudioSpec want = {};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioSpec &have = f.have;
    f.audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    SDL_AudioStream *stream =
        f.audio ? SDL_NewAudioStream(AUDIO_S16SYS, 2, (int)N64_AUDIO_RATE, have.format, have.channels, have.freq) : NULL;
    /* Audio is queued ahead of the device by a target latency (80 ms, and
     * at least three device buffers) so a late frame never leaves it short;
     * a device that runs out plays silence, which is heard as a click. The
     * queue starts full of silence. */
    const Uint32 frame_bytes = have.channels * (SDL_AUDIO_BITSIZE(have.format) / 8);
    Uint32 audio_target = (Uint32)(have.freq * 80 / 1000) * frame_bytes;
    if (audio_target < 3u * have.samples * frame_bytes) audio_target = 3u * have.samples * frame_bytes;
    if (stream) {
        void *silence = calloc(1, audio_target);
        if (silence) SDL_QueueAudio(f.audio, silence, audio_target);
        free(silence);
        SDL_PauseAudioDevice(f.audio, 0);
    } else if (f.audio) {
        SDL_CloseAudioDevice(f.audio);
        f.audio = 0;
    }

    for (int i = 0; i < SDL_NumJoysticks() && !f.pad; i++) {
        if (SDL_IsGameController(i)) set_pad(f, SDL_GameControllerOpen(i));
    }
    f.pad_profile = n64_pad_profile(f.pad, f.platform);

    const double freq = (double)SDL_GetPerformanceFrequency();
    double next_frame = (double)SDL_GetPerformanceCounter() / freq;
    uint32_t last_save_check = SDL_GetTicks();
    f.fps_since = SDL_GetTicks();
    bool running = true;
    static int16_t samples[4096 * 2];
    static uint8_t converted[65536];
    uint64_t stat_dry = 0, stat_sum = 0, stat_n = 0, stat_pushed = 0;
    Uint32 stat_min = UINT32_MAX;

    while (running) {
        if (selftest.enabled && !menu_selftest_step(f, selftest)) running = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            /* Keys pressed while a binding is captured belong to it, not to
             * menu navigation (key releases still go through, so ImGui does
             * not think a key stays down). */
            if (!(f.capturing && (ev.type == SDL_KEYDOWN || ev.type == SDL_TEXTINPUT))) ImGui_ImplSDL2_ProcessEvent(&ev);
            if (handle_capture(f, ev)) continue;
            if (ev.type == SDL_QUIT) running = false;
            else if (ev.type == SDL_CONTROLLERDEVICEADDED && !f.pad) set_pad(f, SDL_GameControllerOpen(ev.cdevice.which));
            else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && f.pad &&
                     ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(f.pad))) {
                set_pad(f, NULL);
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                set_menu(f, !f.show_menu);
            } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                /* Also follows fullscreen changes made by the system (the
                 * macOS window menu, a double-click on the title bar): the
                 * window is then not as the frontend last asked for it, and
                 * that is a choice to keep. A size change that finds it as
                 * asked is no choice - the one that follows making the
                 * window fullscreen for --fullscreen is such an event, and
                 * used to write that run's fullscreen as the setting. */
                const bool fullscreen_now = (SDL_GetWindowFlags(f.win) & SDL_WINDOW_FULLSCREEN) != 0;
                if (fullscreen_now != f.fullscreen) {
                    f.fullscreen = fullscreen_now;
                    if (f.fullscreen_saved != f.fullscreen) {
                        f.fullscreen_saved = f.fullscreen;
                        save_prefs(f);
                    }
                }
                update_viewport(f);
            } else if (ev.type == SDL_RENDER_DEVICE_RESET) {
                /* The renderer lost its textures (Direct3D device reset). */
                SDL_DestroyTexture(f.tex);
                f.tex = create_frame_texture(f.ren);
                if (f.large_tex) SDL_DestroyTexture(f.large_tex);
                f.large_tex = nullptr; /* made again when it is next drawn */
                if (!f.tex) running = false;
                else apply_filter(f);
            } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat &&
                       (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE || ev.key.keysym.scancode == SDL_SCANCODE_AC_BACK)) {
                set_menu(f, !f.show_menu);
            } else if ((ev.type == SDL_KEYDOWN && !ev.key.repeat) || ev.type == SDL_CONTROLLERBUTTONDOWN) {
                note_shortcut_press(f, ev);
            }
        }
        run_shortcuts(f);
        /* Controller navigation of the menu waits while a binding is being
         * captured, and until the buttons used for it are released. */
        if (f.pad_nav_blocked && !f.capturing && !any_pad_input(f)) f.pad_nav_blocked = false;
        if (f.capturing || f.pad_nav_blocked) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        const bool paused = f.show_menu;
        if (!paused) {
            PadState in = gameplay_input(f);
            f.held_from_menu &= in.buttons;
            in.buttons &= (uint16_t)~f.held_from_menu;
            if (f.stick_from_menu && in.x == 0 && in.y == 0) f.stick_from_menu = false;
            if (f.stick_from_menu) in.x = in.y = 0;
            n64_set_buttons(m, 0, in.buttons, in.x, in.y);
            f.sent = in;

            uint64_t interpreted = m->cpu.interpreted;
            n64_run_frame(m);
            f.interpreted_last_frame = m->cpu.interpreted - interpreted;
            if (limit_frames > 0 && (long)m->frame_count >= limit_frames) running = false;
            f.fps_frames++;
            Uint32 elapsed = SDL_GetTicks() - f.fps_since;
            if (elapsed >= 1000) {
                f.fps = f.fps_frames * 1000.0 / elapsed;
                f.fps_frames = 0;
                f.fps_since = SDL_GetTicks();
            }

            /* Everything the AI produced this frame is drained, played or
             * not, so the machine's ring never overflows into old sound. */
            const bool play = stream && !f.fast && f.speed == 100;
            if (play && audio_stats) {
                Uint32 queued = SDL_GetQueuedAudioSize(f.audio);
                if (m->frame_count > 60 && queued == 0) stat_dry++;
                if (m->frame_count > 60 && queued < stat_min) stat_min = queued;
                stat_sum += queued;
                stat_n++;
            }
            size_t frames;
            while ((frames = n64_read_audio(m, samples, 4096)) > 0) {
                if (!play) continue;
                stat_pushed += frames;
                /* Muted audio is still queued, as silence, to keep the device clock running. */
                if (f.muted || f.mute_this_run || f.volume == 0) {
                    memset(samples, 0, frames * 4);
                } else if (f.volume != VOLUME_MAX) {
                    for (size_t i = 0; i < frames * 2; i++) samples[i] = (int16_t)(samples[i] * f.volume / VOLUME_MAX);
                }
                SDL_AudioStreamPut(stream, samples, (int)(frames * 4));
            }
            if (play) {
                int got;
                while ((got = SDL_AudioStreamGet(stream, converted, sizeof converted)) > 0) {
                    SDL_QueueAudio(f.audio, converted, (Uint32)got);
                }
            }
        } else {
            f.fps_frames = 0;
            f.fps_since = SDL_GetTicks();
        }

        present(f);

        if (SDL_GetTicks() - last_save_check > 2000) {
            last_save_check = SDL_GetTicks();
            if (m->save_dirty) write_save(m, save_path);
        }

        /* Pace to the VI's field rate, steered by the audio queue: below half
         * the target, run the next frame at once (catch up); otherwise run up
         * to 0.5 % fast or slow to hold the queue at the target, since the
         * audio device's clock and the performance counter drift apart.
         * Other speeds pace by the clock alone; the menu redraws at about
         * 60 Hz while the game is paused. */
        double now = (double)SDL_GetPerformanceCounter() / freq;
        if (paused) {
            SDL_Delay(16);
            next_frame = (double)SDL_GetPerformanceCounter() / freq;
        } else if (!f.fast) {
            double frame = f.frame_seconds * 100.0 / f.speed;
            bool starved = false;
            if (f.audio && f.speed == 100) {
                Uint32 queued = SDL_GetQueuedAudioSize(f.audio);
                starved = queued < audio_target / 2;
                double error = ((double)queued - audio_target) / audio_target;
                frame *= 1.0 + 0.005 * (error > 1.0 ? 1.0 : error < -1.0 ? -1.0 : error);
            }
            next_frame += frame;
            if (starved || now - next_frame > 0.25) {
                next_frame = now; /* do not pay back time spent catching up */
            } else {
                /* SDL_Delay can oversleep by a millisecond or more: sleep
                 * short, then wait out the rest. */
                while (next_frame - now > 0.002) {
                    SDL_Delay((Uint32)((next_frame - now - 0.0015) * 1000.0));
                    now = (double)SDL_GetPerformanceCounter() / freq;
                }
                while (now < next_frame) now = (double)SDL_GetPerformanceCounter() / freq;
            }
        } else {
            next_frame = now;
        }
    }

    if (audio_stats && stat_n) {
        double bytes_per_ms = have.freq * have.channels * (SDL_AUDIO_BITSIZE(have.format) / 8) / 1000.0;
        fprintf(stderr, "audio: %s %d Hz, %u-frame device buffer; queue before each push: mean %.1f ms, min %.1f ms, "
                        "ran dry %llu times in %llu frames; %llu sample frames pushed\n",
                SDL_GetCurrentAudioDriver(), have.freq, have.samples, (double)stat_sum / (double)stat_n / bytes_per_ms,
                stat_min == UINT32_MAX ? 0.0 : stat_min / bytes_per_ms, (unsigned long long)stat_dry,
                (unsigned long long)stat_n, (unsigned long long)stat_pushed);
    }
    if (shot_prefix) {
        char path[1100];
        snprintf(path, sizeof path, "%s.bmp", shot_prefix);
        screenshot(m, path);
    }
    if (display_shot) save_display_shot(f, display_shot, display_w, display_h);
    if (m->save_dirty) write_save(m, save_path);
    if (learn && n64_coverage_entries(m) && !n64_coverage_merge_trace(m, trace_path))
        fprintf(stderr, "cannot write %s\n", trace_path);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (f.pad) SDL_GameControllerClose(f.pad);
    if (stream) SDL_FreeAudioStream(stream);
    if (f.audio) SDL_CloseAudioDevice(f.audio);
    SDL_DestroyTexture(f.tex);
    if (f.large_tex) SDL_DestroyTexture(f.large_tex);
    SDL_DestroyRenderer(f.ren);
    SDL_DestroyWindow(f.win);
    n64_destroy(m);
    quit_sdl();
    return selftest.failures ? 1 : 0;
}
