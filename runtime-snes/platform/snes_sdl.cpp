/*
 * SDL2 frontend for SNES programs: window, input, audio, frame pacing, the
 * battery save, save states and the settings menu. Recompiled games call
 * snes_sdl_main(); headless options go to snes_runner_main() instead (see
 * snes_sdl_wants_headless()). Adapted from the Game Boy Advance frontend
 * (runtime-gba/platform/gba_sdl.cpp); the menu follows the Game Boy
 * runtime's.
 *
 *   <exe> [ROM] [--scale N] [--save-dir DIR] [--fullscreen] [--mute]
 *         [--interpreter] [--limit-frames N] [--screenshot-prefix P]
 *         [--audio-stats] [--prefs FILE] [--load-state SLOT] [--mode7-scale N]
 *         [--display-shot FILE.bmp] [--display-size WxH]
 *         [--menu-selftest | --menu-selftest-verify]
 *
 * --limit-frames exits after N frames; --screenshot-prefix writes P.bmp of
 * the last frame on exit (the hub uses both to capture cover art).
 * --display-shot instead writes the last frame as the display shows it, at
 * --display-size (default 3840x2160) through the saved scaling mode and
 * filter; --load-state loads save state slot SLOT (1-10) at start. The
 * menu self-test is described above menu_selftest_step().
 *
 * Mode 7 Resolution in the menu (display.mode7_scale; --mode7-scale N for
 * one run) draws the mode 7 plane with up to four samples a pixel each way
 * (snes_ppu.h, snes_ppu_set_mode7_scale). The console's own picture is
 * still drawn and is what the game, screenshots and the CRT filter get; a
 * frame that showed the plane is displayed from the larger picture.
 *
 * Scale filters: Nearest, Linear, and CRT, which draws the frame as a CRT
 * television shows it (snes_crt.c): beams that swell with brightness, a
 * phosphor mask multiplied over them and a halo added.
 *
 * Controls are bindable in the menu; the defaults depend on the platform
 * (snes_input.h): arrows, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter =
 * Start, Backspace/Right Shift = Select, Tab = fast forward (hold), M = mute, with
 * F-key shortcuts on Windows and Linux (F5/F8 save/load state, F11
 * fullscreen...) and Command shortcuts on a Mac (Cmd+S, Cmd+L, Cmd+F...).
 * Escape and a controller's Guide button always open the settings menu (the
 * game pauses while it is open); L3 does too by default, and is the way in
 * on a Steam Deck. Controllers use the Nintendo layout: east button = A,
 * south = B, north = X, west = Y, shoulders = L/R.
 *
 * The menu follows the Game Boy runtime's (runtime/src/platform_sdl.cpp).
 * Display, audio, slot and control choices are kept in snes_prefs.ini in the
 * per-user app data folder, shared by all SNES games; bindings are stored
 * only where they differ from the platform's defaults. Save states are
 * <save name>.state1 to .state10 beside the battery save.
 */
#include "snes_sdl.h"

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

#include "snes.h"
#include "snes_crt.h"
#include "snes_input.h"
#include "snes_recomp.h"
#include "snes_state.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_dir(path) mkdir(path, 0777)
#endif

#define FRAME_SECONDS (357366.0 / 21477272.0) /* 60.0988 Hz (NTSC, the short line averaged in) */
#define SCREEN_W 256
#define SCREEN_H 224

static const char *const k_headless_flags[] = {
    "--headless", "--frames", "--until-idle", "--expect-text", "--fail-text", "--expect-wram", "--screen-text",
    "--dump-frame", "--dump-state", "--dump-audio", "--differential", "--coverage", "--trace-out", "--auto-input",
    "--quiet", "--keys", "--trace-last", "--trace-apu", "--apu-console", "--dump-large",
};

bool snes_sdl_wants_headless(int argc, char **argv)
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
 * loses whatever is still in stdout's buffer one or two times in a hundred:
 * SDL_Quit returns, the exit code is right, and the text never arrives. The
 * C runtime writes its buffers out late in the process's exit, and on those
 * runs the process ends before it gets there. Flushed first, the text
 * arrives every time. The menu self-test's verdict is such text, and its
 * script reads it. */
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

/* Battery RAM from the cartridge header (0 for carts without a battery). */
static size_t save_size(const SnesMachine *m)
{
    return m->cart.battery ? m->cart.sram_size : 0;
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

/* The battery RAM, exactly as large as the header says. */
static void write_save(SnesMachine *m, const char *path)
{
    size_t size = save_size(m);
    if (!size) return;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write save %s\n", path);
        return;
    }
    fwrite(m->sram, 1, size, f);
    fclose(f);
    m->sram_dirty = false;
}

static void read_save(SnesMachine *m, const char *path)
{
    size_t n = 0, size = save_size(m);
    uint8_t *sav = read_file(path, &n);
    if (!sav) return;
    memcpy(m->sram, sav, n < size ? n : size);
    free(sav);
}

static int frame_width(const SnesMachine *m) { return m->ppu.out_width ? m->ppu.out_width : SCREEN_W; }
static int frame_height(const SnesMachine *m) { return m->ppu.out_height ? m->ppu.out_height : SCREEN_H; }

static void screenshot(const SnesMachine *m, const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)m->ppu.framebuffer, frame_width(m), frame_height(m),
                                                        32, SNES_FB_WIDTH * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s) {
        if (SDL_SaveBMP(s, path) == 0) printf("screenshot: %s\n", path);
        SDL_FreeSurface(s);
    }
}

/* ------------------------------------------------------------------ */
/* Settings menu, overlay and save states                              */
/* ------------------------------------------------------------------ */

enum { SCALING_PIXEL_PERFECT, SCALING_ASPECT_FIT, SCALING_ASPECT_FILL, SCALING_STRETCH };
static const char *const k_scaling_names[] = { "Pixel Perfect", "Aspect Fit", "Aspect Fill", "Stretch" };
enum { FILTER_NEAREST, FILTER_LINEAR, FILTER_CRT, FILTER_COUNT };
static const char *const k_filter_names[] = { "Nearest", "Linear", "CRT" };
static const char *const k_scale_names[] = { "1x (256x224)",  "2x (512x448)",   "3x (768x672)",   "4x (1024x896)",
                                             "5x (1280x1120)", "6x (1536x1344)", "7x (1792x1568)", "8x (2048x1792)" };
enum { DEFAULT_SCALE = 3, MAX_SCALE = 8, SLOT_COUNT = 10 };

/* Master volume slider position, 0-100, as in the Game Boy runtime. The APU
 * mixes near full int16 scale, far too loud on real speakers, so the output
 * applies VOLUME_GAIN_DIVISOR on top of the slider: 50 % plays at 2 % of
 * the raw APU output. */
enum { VOLUME_DEFAULT = 50, VOLUME_MAX = 100, VOLUME_GAIN_DIVISOR = 2500 };

/* Frontend state kept between frames. */
struct Frontend {
    SnesMachine *m = nullptr;
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    SDL_Texture *tex = nullptr;
    /* CRT: the picture (snes_crt.h), its halo, and the mask for a viewport width. */
    snes_crt *crt = nullptr;
    SDL_Texture *crt_tex = nullptr, *glow_tex = nullptr, *mask_tex = nullptr;
    int crt_w = 0, crt_h = 0, glow_w = 0, glow_h = 0, mask_w = 0;
    std::vector<uint32_t> crt_frame, glow_frame;
    SDL_Rect viewport = { 0, 0, SCREEN_W, SCREEN_H };
    SDL_GameController *pad = nullptr;
    SDL_AudioDeviceID audio = 0;
    SDL_AudioSpec have = {};
    std::string title;

    /* Kept in snes_prefs.ini. */
    int scale = DEFAULT_SCALE;
    int scaling = SCALING_PIXEL_PERFECT;
    int filter = 0; /* index into k_filter_names */
    int mode7_scale = 1;          /* the setting, 1 to SNES_MODE7_SCALE_MAX */
    int mode7_scale_this_run = 0; /* --mode7-scale: this run only */
    SDL_Texture *large_tex = nullptr; /* the larger picture, made when there is one */
    int large_w = 0, large_h = 0;
    bool show_overlay = false;
    bool show_fps = false;
    bool muted = false;
    int volume = VOLUME_DEFAULT;
    int slot = 0;
    SnesInputMap map = {};

    /* Controls. */
    SnesInputPlatform platform = snes_input_host_platform();
    SnesPadProfile pad_profile = SNES_PAD_GENERIC;
    SnesInputMap defaults = {};
    bool action_down[SNES_ACTION_COUNT] = {};   /* held at the last poll */
    bool action_tapped[SNES_ACTION_COUNT] = {}; /* pressed by an event since the last poll */
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
    uint16_t held_from_menu = 0; /* buttons still down from using the menu; ignored until released */
    int speed = 100; /* percent; audio plays only at 100 */
    bool fast = false; /* fast forward held: unthrottled, no audio */

    std::string prefs_path, state_stem, shot_path;
    std::string status;     /* last save-state action */
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
 * change of default reaches everyone who has not rebound that control.
 * "audio.volume" (a percent of the raw output, before the 2 % scale) is
 * ignored rather than read as the new slider. */
static void load_prefs(Frontend &f)
{
    snes_input_defaults(f.defaults, f.platform);
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
        else if (!strcmp(key, "display.mode7_scale")) f.mode7_scale = clamp_int(number, 1, SNES_MODE7_SCALE_MAX);
        else if (!strcmp(key, "display.overlay")) f.show_overlay = number != 0;
        else if (!strcmp(key, "display.fps")) f.show_fps = number != 0;
        else if (!strcmp(key, "audio.muted")) f.muted = number != 0;
        else if (!strcmp(key, "audio.master_volume")) f.volume = clamp_int(number, 0, VOLUME_MAX);
        else if (!strcmp(key, "savestate.slot")) f.slot = clamp_int(number - 1, 0, SLOT_COUNT - 1);
        else if (!strncmp(key, "keyboard.", 9) || !strncmp(key, "controller.", 11)) {
            const bool keyboard = key[0] == 'k';
            char *name = key + (keyboard ? 9 : 11), *dot = strrchr(name, '.');
            if (!dot) continue;
            *dot = '\0';
            int slot = atoi(dot + 1);
            SnesBinding b;
            if (slot < 0 || slot > 1 || !snes_binding_from_config(value, b)) continue;
            if (b.kind != SNES_BIND_NONE && (b.kind == SNES_BIND_KEY) != keyboard) continue;
            for (int a = 0; a < SNES_ACTION_COUNT; a++) {
                if (!strcmp(name, snes_action_config(a))) (keyboard ? f.map.keyboard : f.map.controller)[a][slot] = b;
            }
        }
    }
    fclose(file);
}

static void save_prefs(const Frontend &f)
{
    FILE *file = f.prefs_path.empty() ? NULL : fopen(f.prefs_path.c_str(), "w");
    if (!file) return;
    fprintf(file, "# Native Console Super Nintendo settings\n");
    fprintf(file, "display.window_scale = %d\n", f.scale);
    fprintf(file, "display.fullscreen = %d\n", f.fullscreen_saved ? 1 : 0);
    fprintf(file, "display.scaling = %d\n", f.scaling);
    fprintf(file, "display.filter = %d\n", f.filter);
    fprintf(file, "display.mode7_scale = %d\n", f.mode7_scale);
    fprintf(file, "display.overlay = %d\n", f.show_overlay ? 1 : 0);
    fprintf(file, "display.fps = %d\n", f.show_fps ? 1 : 0);
    fprintf(file, "audio.muted = %d\n", f.muted ? 1 : 0);
    fprintf(file, "audio.master_volume = %d\n", f.volume);
    fprintf(file, "savestate.slot = %d\n", f.slot + 1);
    for (int a = 0; a < SNES_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            if (f.map.keyboard[a][s] != f.defaults.keyboard[a][s]) {
                fprintf(file, "keyboard.%s.%d = %s\n", snes_action_config(a), s,
                        snes_binding_to_config(f.map.keyboard[a][s]).c_str());
            }
            if (f.map.controller[a][s] != f.defaults.controller[a][s]) {
                fprintf(file, "controller.%s.%d = %s\n", snes_action_config(a), s,
                        snes_binding_to_config(f.map.controller[a][s]).c_str());
            }
        }
    }
    fclose(file);
}

/* ------------------------------------------------------------------ */
/* Controls                                                            */
/* ------------------------------------------------------------------ */

static bool binding_down(const Frontend &f, const SnesBinding &b, const Uint8 *kb, uint8_t mods)
{
    switch (b.kind) {
    case SNES_BIND_KEY: return kb[b.code] && snes_key_mods_match(b, mods);
    case SNES_BIND_BUTTON: return f.pad && SDL_GameControllerGetButton(f.pad, (SDL_GameControllerButton)b.code);
    case SNES_BIND_AXIS_POSITIVE: return f.pad && SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code) > 16000;
    case SNES_BIND_AXIS_NEGATIVE: return f.pad && SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code) < -16000;
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

/* SNES buttons the player is holding. */
static uint16_t gameplay_keys(const Frontend &f)
{
    const Uint8 *kb = SDL_GetKeyboardState(NULL);
    uint8_t mods = snes_mods_from_sdl(SDL_GetModState());
    uint16_t keys = 0;
    for (int a = 0; a < SNES_ACTION_GAMEPLAY_COUNT; a++) {
        if (action_held(f, a, kb, mods)) keys |= snes_action_keypad_bit(a);
    }
    return keys;
}

/* A shortcut pressed and released between two polls still counts. */
static void note_shortcut_press(Frontend &f, const SDL_Event &ev)
{
    for (int a = SNES_ACTION_GAMEPLAY_COUNT; a < SNES_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            const SnesBinding &k = f.map.keyboard[a][s], &c = f.map.controller[a][s];
            if (ev.type == SDL_KEYDOWN && k.kind == SNES_BIND_KEY && k.code == ev.key.keysym.scancode &&
                snes_key_mods_match(k, snes_mods_from_sdl(ev.key.keysym.mod)))
                f.action_tapped[a] = true;
            if (ev.type == SDL_CONTROLLERBUTTONDOWN && c.kind == SNES_BIND_BUTTON && c.code == ev.cbutton.button)
                f.action_tapped[a] = true;
        }
    }
}

static void assign_binding(Frontend &f, bool keyboard, int action, int slot, const SnesBinding &b)
{
    SnesBinding(*map)[2] = keyboard ? f.map.keyboard : f.map.controller;
    f.bind_note.clear();
    /* One control does one thing: take the binding off any other action. */
    for (int a = 0; a < SNES_ACTION_COUNT && b.kind != SNES_BIND_NONE; a++) {
        for (int s = 0; s < 2; s++) {
            if ((a != action || s != slot) && map[a][s] == b) {
                map[a][s].kind = SNES_BIND_NONE;
                f.bind_note = snes_binding_label(b, f.platform, f.pad_profile) + " was moved from " + snes_action_label(a) + ".";
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

static void finish_capture(Frontend &f, const SnesBinding *b)
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
        else if (snes_scancode_is_modifier(sc)) f.capture_modifier = sc;
        else {
            SnesBinding b = { SNES_BIND_KEY, snes_mods_from_sdl(ev.key.keysym.mod), (int16_t)sc };
            finish_capture(f, &b);
        }
        return true;
    }
    case SDL_KEYUP:
        if (f.capture_keyboard && ev.key.keysym.scancode == f.capture_modifier) {
            SnesBinding b = { SNES_BIND_KEY, 0, (int16_t)f.capture_modifier };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) finish_capture(f, NULL);
        else if (!f.capture_keyboard) {
            SnesBinding b = { SNES_BIND_BUTTON, 0, (int16_t)ev.cbutton.button };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERAXISMOTION:
        if (!f.capture_keyboard && (ev.caxis.value >= 16000 || ev.caxis.value <= -16000)) {
            SnesBinding b = { (uint8_t)(ev.caxis.value > 0 ? SNES_BIND_AXIS_POSITIVE : SNES_BIND_AXIS_NEGATIVE), 0,
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
    f.pad_profile = snes_pad_profile(pad, f.platform);
}

/* Where the game goes on a w x h screen. */
static SDL_Rect viewport_for(int scaling, int w, int h)
{
    if (w <= 0) w = SCREEN_W;
    if (h <= 0) h = SCREEN_H;
    double sx = (double)w / SCREEN_W, sy = (double)h / SCREEN_H;
    int vw = w, vh = h;
    switch (scaling) {
    case SCALING_PIXEL_PERFECT: {
        int s = (int)(sx < sy ? sx : sy);
        if (s < 1) s = 1;
        vw = SCREEN_W * s;
        vh = SCREEN_H * s;
        break;
    }
    case SCALING_ASPECT_FIT:
    case SCALING_ASPECT_FILL: {
        double fit = sx < sy ? sx : sy, fill = sx < sy ? sy : sx;
        double s = scaling == SCALING_ASPECT_FIT ? fit : fill;
        vw = (int)(SCREEN_W * s + 0.5);
        vh = (int)(SCREEN_H * s + 0.5);
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

/* CRT falls back to Linear when it cannot draw. */
static void apply_filter(Frontend &f)
{
    SDL_SetTextureScaleMode(f.tex, f.filter != FILTER_NEAREST ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}

/* The CRT filter's textures; made again on first use. */
static void release_filter_textures(Frontend &f)
{
    for (SDL_Texture **t : { &f.crt_tex, &f.glow_tex, &f.mask_tex }) {
        if (*t) SDL_DestroyTexture(*t);
        *t = nullptr;
    }
    f.crt_w = f.crt_h = f.glow_w = f.glow_h = f.mask_w = 0;
}

/* A streaming texture of w x h in *t, made again when the size changes. */
static bool sized_texture(Frontend &f, SDL_Texture **t, int *tw, int *th, int w, int h)
{
    if (*t && *tw == w && *th == h) return true;
    if (*t) SDL_DestroyTexture(*t);
    *t = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    *tw = *t ? w : 0;
    *th = *t ? h : 0;
    if (*t) SDL_SetTextureScaleMode(*t, SDL_ScaleModeLinear);
    return *t != nullptr;
}

/* CRT: the picture at whole beam lines (snes_crt_render), 4 columns per
 * pixel of a 256-wide frame and 2 of a 512-wide one, scaled to dest; then
 * the phosphor mask multiplied in once a 256-wide pixel spans a whole
 * stripe triad, then the halo added. False when the viewport has under 2
 * rows per line (an interlaced frame's lines count) or a texture cannot be
 * made; the caller then draws Linear. */
static bool draw_crt(Frontend &f, const SDL_Rect &dest)
{
    const int w = frame_width(f.m), h = frame_height(f.m), columns = w > SCREEN_W ? 2 : 4;
    int line = dest.h / h;
    if (line > SNES_CRT_MAX_LINE) line = SNES_CRT_MAX_LINE;
    if (line < SNES_CRT_MIN_LINE) return false;
    if (!f.crt && !(f.crt = snes_crt_create())) return false;
    const int cw = w * columns, ch = h * line;
    if (!sized_texture(f, &f.crt_tex, &f.crt_w, &f.crt_h, cw, ch)) return false;
    if (!f.glow_tex || f.glow_w != w || f.glow_h != h) {
        if (!sized_texture(f, &f.glow_tex, &f.glow_w, &f.glow_h, w, h)) return false;
        SDL_SetTextureBlendMode(f.glow_tex, SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaMod(f.glow_tex, SNES_CRT_GLOW_ALPHA);
    }
    f.crt_frame.resize((size_t)cw * ch);
    f.glow_frame.resize((size_t)w * h);
    if (!snes_crt_render(f.crt, f.m->ppu.framebuffer, w, h, SNES_FB_WIDTH, columns, line, f.crt_frame.data()) ||
        !snes_crt_glow(f.crt, f.m->ppu.framebuffer, w, h, SNES_FB_WIDTH, f.glow_frame.data()))
        return false;
    SDL_UpdateTexture(f.crt_tex, NULL, f.crt_frame.data(), cw * 4);
    SDL_UpdateTexture(f.glow_tex, NULL, f.glow_frame.data(), w * 4);
    SDL_RenderCopy(f.ren, f.crt_tex, NULL, &dest);
    if (dest.w >= SCREEN_W * 3) {
        if (!f.mask_tex || f.mask_w != dest.w) {
            if (f.mask_tex) SDL_DestroyTexture(f.mask_tex);
            f.mask_tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, dest.w, 1);
            f.mask_w = f.mask_tex ? dest.w : 0;
            if (f.mask_tex) {
                std::vector<uint32_t> mask((size_t)dest.w);
                for (int x = 0; x < dest.w; x++) mask[(size_t)x] = snes_crt_mask(x);
                SDL_UpdateTexture(f.mask_tex, NULL, mask.data(), dest.w * 4);
                SDL_SetTextureBlendMode(f.mask_tex, SDL_BLENDMODE_MOD);
                SDL_SetTextureScaleMode(f.mask_tex, SDL_ScaleModeNearest);
            }
        }
        if (f.mask_tex) SDL_RenderCopy(f.ren, f.mask_tex, NULL, &dest);
    }
    SDL_RenderCopy(f.ren, f.glow_tex, NULL, &dest);
    return true;
}

static int wanted_mode7_scale(const Frontend &f)
{
    return clamp_int(f.mode7_scale_this_run > 0 ? f.mode7_scale_this_run : f.mode7_scale, 1, SNES_MODE7_SCALE_MAX);
}

static void apply_mode7_scale(Frontend &f)
{
    if (f.m && snes_ppu_mode7_scale(&f.m->ppu) != wanted_mode7_scale(f))
        snes_ppu_set_mode7_scale(&f.m->ppu, wanted_mode7_scale(f));
}

/* The larger picture as a texture, or NULL when the last frame has none
 * (Mode 7 Resolution at 1x, or a frame without the mode 7 plane). */
static SDL_Texture *large_texture(Frontend &f, const SDL_Rect &dest)
{
    int w = 0, h = 0;
    const uint32_t *large = snes_ppu_large_frame(&f.m->ppu, &w, &h);
    if (!large || w < 1 || h < 1) return nullptr;
    if (!f.large_tex || f.large_w != w || f.large_h != h) {
        if (f.large_tex) SDL_DestroyTexture(f.large_tex);
        f.large_tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        f.large_w = w;
        f.large_h = h;
    }
    if (!f.large_tex) return nullptr;
    /* Nearest keeps pixels whole only where the picture goes into its place
     * a whole number of times; anywhere else it would drop or double lines. */
    const bool whole = dest.w >= w && dest.h >= h && dest.w % w == 0 && dest.h % h == 0;
    SDL_SetTextureScaleMode(f.large_tex, f.filter == FILTER_NEAREST && whole ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    SDL_UpdateTexture(f.large_tex, NULL, large, w * 4);
    return f.large_tex;
}

/* Draws the current frame into dest through the chosen filter. */
static void draw_game(Frontend &f, const SDL_Rect &dest)
{
    /* The CRT is drawn from the console's lines. */
    if (f.filter == FILTER_CRT && draw_crt(f, dest)) return;
    if (SDL_Texture *large = large_texture(f, dest)) {
        SDL_RenderCopy(f.ren, large, NULL, &dest);
        return;
    }
    SDL_Rect used = { 0, 0, frame_width(f.m), frame_height(f.m) };
    SDL_UpdateTexture(f.tex, &used, f.m->ppu.framebuffer, SNES_FB_WIDTH * 4);
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
    const bool read = SDL_RenderReadPixels(f.ren, NULL, SDL_PIXELFORMAT_ARGB8888, pixels.data(), w * 4) == 0;
    SDL_SetRenderTarget(f.ren, NULL);
    SDL_DestroyTexture(shot);
    SDL_Surface *s = read ? SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888)
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
        SDL_SetWindowSize(f.win, SCREEN_W * f.scale, SCREEN_H * f.scale);
        SDL_SetWindowPosition(f.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    update_viewport(f);
}

static void reset_display(Frontend &f)
{
    f.scale = DEFAULT_SCALE;
    f.scaling = SCALING_PIXEL_PERFECT;
    f.filter = 0;
    f.mode7_scale = 1;
    f.mode7_scale_this_run = 0;
    apply_mode7_scale(f);
    f.show_overlay = false;
    f.show_fps = false;
    f.speed = 100;
    apply_filter(f);
    if (f.fullscreen) set_fullscreen(f, false);
    apply_window_scale(f);
    save_prefs(f);
}

/* The game runs only while the menu is closed; the audio device pauses with
 * it so what is queued plays on when the game resumes. Enter or a face
 * button that chose Resume is still down then, and must not reach the game. */
static void set_menu(Frontend &f, bool open)
{
    f.show_menu = open;
    if (f.audio) SDL_PauseAudioDevice(f.audio, open ? 1 : 0);
    if (!open) f.held_from_menu = gameplay_keys(f);
    if (!open && f.capturing) finish_capture(f, NULL);
}

static std::string state_path(const Frontend &f, int slot)
{
    return f.state_stem + ".state" + std::to_string(slot + 1);
}

static bool file_exists(const std::string &path)
{
    FILE *file = fopen(path.c_str(), "rb");
    if (file) fclose(file);
    return file != NULL;
}

static void set_status(Frontend &f, const std::string &message)
{
    f.status = message;
    f.status_until = SDL_GetTicks() + 2500;
    printf("%s\n", message.c_str());
}

static void save_state(Frontend &f)
{
    const std::string slot = "slot " + std::to_string(f.slot + 1);
    if (snes_state_save_file(f.m, state_path(f, f.slot).c_str())) set_status(f, "Saved state to " + slot);
    else set_status(f, "Could not write " + state_path(f, f.slot));
}

static void load_state(Frontend &f)
{
    const std::string path = state_path(f, f.slot), number = std::to_string(f.slot + 1);
    if (!file_exists(path)) set_status(f, "Slot " + number + " is empty");
    else if (snes_state_load_file(f.m, path.c_str())) set_status(f, "Loaded state from slot " + number);
    else set_status(f, "Slot " + number + " was saved by another game or runtime version");
}

static void delete_state(Frontend &f)
{
    const std::string path = state_path(f, f.slot), slot = "Slot " + std::to_string(f.slot + 1);
    if (!file_exists(path)) set_status(f, slot + " is already empty");
    else if (remove(path.c_str()) == 0) set_status(f, slot + " deleted");
    else set_status(f, "Could not delete " + path);
}

static void select_slot(Frontend &f, int slot)
{
    f.slot = (slot + SLOT_COUNT) % SLOT_COUNT;
    set_status(f, "Selected slot " + std::to_string(f.slot + 1));
    save_prefs(f);
}

/* Fires shortcuts pressed since the last call and updates fast forward.
 * Runs once per loop, after events, whether or not the game is paused. */
static void run_shortcuts(Frontend &f)
{
    const Uint8 *kb = SDL_GetKeyboardState(NULL);
    const uint8_t mods = snes_mods_from_sdl(SDL_GetModState());
    const bool baseline = f.input_resync || f.capturing;
    f.input_resync = false;
    for (int a = SNES_ACTION_GAMEPLAY_COUNT; a < SNES_ACTION_COUNT; a++) {
        const bool held = action_held(f, a, kb, mods);
        const bool pressed = (held || f.action_tapped[a]) && !f.action_down[a];
        f.action_down[a] = held;
        f.action_tapped[a] = false;
        if (!pressed || baseline) continue;
        switch (a) {
        case SNES_ACTION_SAVE_STATE: save_state(f); break;
        case SNES_ACTION_LOAD_STATE: load_state(f); break;
        case SNES_ACTION_PREVIOUS_SLOT: select_slot(f, f.slot - 1); break;
        case SNES_ACTION_NEXT_SLOT: select_slot(f, f.slot + 1); break;
        case SNES_ACTION_OVERLAY:
            f.show_overlay = !f.show_overlay;
            save_prefs(f);
            break;
        case SNES_ACTION_FPS:
            f.show_fps = !f.show_fps;
            save_prefs(f);
            break;
        case SNES_ACTION_MUTE:
            f.muted = !f.muted;
            save_prefs(f);
            break;
        case SNES_ACTION_MENU: set_menu(f, !f.show_menu); break;
        case SNES_ACTION_FULLSCREEN: set_fullscreen(f, !f.fullscreen); break;
        case SNES_ACTION_SCREENSHOT: screenshot(f.m, f.shot_path.c_str()); break;
        default: break;
        }
    }
    f.fast = f.action_down[SNES_ACTION_FAST_FORWARD] && !f.capturing;
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

/* The first binding of an action, for hints such as "Save State (F5)":
 * keyboard, else controller; empty when unbound. */
static std::string shortcut_hint(const Frontend &f, int action)
{
    for (int device = 0; device < 2; device++) {
        for (int s = 0; s < 2; s++) {
            const SnesBinding &b = (device == 0 ? f.map.keyboard : f.map.controller)[action][s];
            if (b.kind != SNES_BIND_NONE) return snes_binding_label(b, f.platform, f.pad_profile);
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
        ImGui::TextUnformatted(snes_action_label(a));
        for (int s = 0; s < 2; s++) {
            ImGui::TableNextColumn();
            ImGui::PushID(s);
            const SnesBinding b = (keyboard ? f.map.keyboard : f.map.controller)[a][s];
            const bool waiting = f.capturing && f.capture_keyboard == keyboard && f.capture_action == a &&
                                 f.capture_slot == s;
            const std::string label =
                (waiting ? std::string("Press...") : snes_binding_label(b, f.platform, f.pad_profile)) + "###bind";
            float width = ImGui::GetContentRegionAvail().x - clear_width - style.ItemSpacing.x;
            if (ImGui::Button(label.c_str(), ImVec2(width > 40.0f ? width : 40.0f, 0.0f))) start_capture(f, keyboard, a, s);
            ImGui::SameLine();
            ImGui::BeginDisabled(b.kind == SNES_BIND_NONE);
            if (ImGui::Button("Clear")) {
                SnesBinding none = { SNES_BIND_NONE, 0, 0 };
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
    ImGui::TextDisabled("Super Nintendo Settings");
    ImGui::SameLine();
    if (ImGui::Button("Resume")) set_menu(f, false);
    ImGui::Separator();
    ImGui::TextWrapped("%s", snes_menu_hint(f.platform));
    ImGui::TextWrapped("The game is paused while the menu is open.");
    ImGui::Separator();
    /* Flattened so arrow keys and the D-pad move into the settings and
     * scroll them, not just between Resume and the footer buttons. */
    ImGui::BeginChild("SettingsScroll", ImVec2(0.0f, -footer_height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NavFlattened);

    ImGui::Text("Performance: %.1f FPS", f.fps);
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
    ImGui::Text("Viewport: %d x %d (%.2fx)", f.viewport.w, f.viewport.h, (double)f.viewport.w / SCREEN_W);
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
    /* The mode 7 plane with more samples a pixel; the game still gets the
     * console's picture. */
    {
        static const char *const names[] = { "1x (the console's own)", "2x", "3x", "4x" };
        int index = wanted_mode7_scale(f) - 1;
        if (ImGui::Combo("Mode 7 Resolution", &index, names, IM_ARRAYSIZE(names))) {
            f.mode7_scale = index + 1;
            f.mode7_scale_this_run = 0;
            apply_mode7_scale(f);
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
    const std::string fast_forward = shortcut_hint(f, SNES_ACTION_FAST_FORWARD);
    if (!fast_forward.empty()) ImGui::TextDisabled("Hold %s to fast forward at full speed.", fast_forward.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Audio");
    if (ImGui::Checkbox("Mute", &f.muted)) save_prefs(f);
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
    ImGui::TextDisabled("Savestates");
    int slot = f.slot + 1;
    if (ImGui::SliderInt("Active Slot", &slot, 1, SLOT_COUNT)) {
        f.slot = slot - 1;
        save_prefs(f);
    }
    const std::string save_hint = shortcut_hint(f, SNES_ACTION_SAVE_STATE);
    const std::string load_hint = shortcut_hint(f, SNES_ACTION_LOAD_STATE);
    const std::string save_label = (save_hint.empty() ? std::string("Save State") : "Save State (" + save_hint + ")") + "###save";
    const std::string load_label = (load_hint.empty() ? std::string("Load State") : "Load State (" + load_hint + ")") + "###load";
    if (ImGui::Button(save_label.c_str())) save_state(f);
    ImGui::SameLine();
    if (ImGui::Button(load_label.c_str())) load_state(f);
    ImGui::SameLine();
    if (ImGui::Button("Delete Slot")) delete_state(f);
    const std::string path = state_path(f, f.slot);
    ImGui::TextWrapped("Slot File: %s", path.c_str());
    ImGui::Text("Slot Status: %s", file_exists(path) ? "Present" : "Empty");
    if (!f.status.empty()) ImGui::TextWrapped("%s", f.status.c_str());

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
                       "key to bind a combination.",
                       f.platform == SNES_PLATFORM_MAC ? "Command, Control, Option or Shift" : "Ctrl, Alt or Shift");
    if (f.capturing) {
        ImGui::Text("Waiting for a %s for %s...", f.capture_keyboard ? "key" : "controller button",
                    snes_action_label(f.capture_action));
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) finish_capture(f, NULL);
    }
    if (!f.bind_note.empty()) ImGui::TextWrapped("%s", f.bind_note.c_str());
    draw_binding_table(f, "Keyboard Gameplay", true, 0, SNES_ACTION_GAMEPLAY_COUNT);
    draw_binding_table(f, "Controller Gameplay", false, 0, SNES_ACTION_GAMEPLAY_COUNT);
    draw_binding_table(f, "Keyboard Shortcuts", true, SNES_ACTION_GAMEPLAY_COUNT, SNES_ACTION_COUNT);
    draw_binding_table(f, "Controller Shortcuts", false, SNES_ACTION_GAMEPLAY_COUNT, SNES_ACTION_COUNT);
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
        ImGui::TextUnformatted(snes_menu_hint(f.platform));
        const std::string overlay = shortcut_hint(f, SNES_ACTION_OVERLAY);
        if (!overlay.empty()) ImGui::Text("Use %s for Overlay", overlay.c_str());
        ImGui::Text("Viewport: %d x %d (%s)", f.viewport.w, f.viewport.h, k_scaling_names[f.scaling]);
        ImGui::Text("Speed: %s", f.fast ? "fast forward" : (std::to_string(f.speed) + "%").c_str());
        if (f.audio) ImGui::Text("Audio queued: %.0f ms", audio_queue_ms(f));
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
        if (f.fps > 0.0) ImGui::Text("%.0f FPS  %.0f%%", f.fps, f.fps * FRAME_SECONDS * 100.0);
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

/* The PPU writes 256- or 512-wide frames, 224 or 239 lines (doubled when
 * interlaced), into 512x480 storage; the texture shows the used part, and
 * the viewport keeps the 256x224 shape either way. */
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
 * event queue, as a keyboard and a controller would deliver it: Escape, then
 * the controller's Guide button, then L3 (the Steam Deck's way in). It checks
 * that each opens and closes the menu and that the game stops while the menu
 * is open, and sets the volume to 37 the way the menu's slider does. A second
 * run with --menu-selftest-verify checks the setting was kept. Pass
 * --prefs FILE so the player's own settings are not touched. */
enum { SELFTEST_VOLUME = 37 };

struct MenuSelfTest {
    bool enabled = false;
    int loop = 0;
    uint64_t frame_mark = 0;
    int failures = 0;
};

static void push_key(SDL_Scancode sc)
{
    SDL_Event e = {};
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
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

/* One step per loop, before events are polled; false when done. Each input
 * is given 30 loops (half a second) to take effect. */
static bool menu_selftest_step(Frontend &f, MenuSelfTest &t)
{
    const SnesMachine *m = f.m;
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
        push_button(SDL_CONTROLLER_BUTTON_GUIDE);
        break;
    case 126:
        selftest_check(t, f.show_menu, "a controller's Guide button opens the menu");
        t.frame_mark = m->frame_count;
        break;
    case 156:
        selftest_check(t, m->frame_count == t.frame_mark, "the game is paused (opened from the controller)");
        f.volume = SELFTEST_VOLUME; /* as the menu's volume slider sets it */
        save_prefs(f);
        set_fullscreen(f, true); /* as its Fullscreen box does */
        push_button(SDL_CONTROLLER_BUTTON_GUIDE);
        break;
    case 158: selftest_check(t, !f.show_menu, "Guide closes the menu"); break;
    case 160: push_button(SDL_CONTROLLER_BUTTON_LEFTSTICK); break;
    case 162: selftest_check(t, f.show_menu, "L3 opens the menu (Steam Deck)"); break;
    case 164: push_button(SDL_CONTROLLER_BUTTON_LEFTSTICK); break;
    case 166:
        selftest_check(t, !f.show_menu, "L3 closes the menu");
        printf("menu selftest: %s\n", t.failures ? "FAIL" : "PASS");
        return false;
    default: break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int snes_sdl_main(int argc, char **argv, const SnesRcProgram *program, const char *default_rom)
{
    const char *rom_path = NULL, *save_dir = NULL, *shot_prefix = NULL, *prefs_file = NULL, *display_shot = NULL;
    int cli_scale = 0, cli_mode7_scale = 0, load_slot = 0, display_w = 3840, display_h = 2160;
    long limit_frames = -1;
    bool cli_fullscreen = false, cli_muted = false, interpreter_only = false, audio_stats = false;
    bool selftest_verify = false;
    MenuSelfTest selftest;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--scale") && i + 1 < argc) cli_scale = atoi(argv[++i]);
        else if (!strcmp(a, "--prefs") && i + 1 < argc) prefs_file = argv[++i];
        else if (!strcmp(a, "--load-state") && i + 1 < argc) load_slot = atoi(argv[++i]);
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
        else if (!strcmp(a, "--mode7-scale") && i + 1 < argc) {
            cli_mode7_scale = atoi(argv[++i]);
            if (cli_mode7_scale < 1 || cli_mode7_scale > SNES_MODE7_SCALE_MAX) {
                fprintf(stderr, "--mode7-scale wants 1 to %d\n", SNES_MODE7_SCALE_MAX);
                return 2;
            }
        }
        else if (!strcmp(a, "--audio-stats")) audio_stats = true;
        else if (!strcmp(a, "--interpreter")) interpreter_only = true;
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

    SnesMachine *m = snes_create();
    size_t size = 0;
    bool loaded;
    if (embedded) {
        const uint8_t *data = program->embedded_rom(&size);
        loaded = data && snes_load_rom(m, data, size);
    } else {
        uint8_t *rom = read_file(rom_path, &size);
        loaded = rom && snes_load_rom(m, rom, size);
        free(rom);
    }
    if (!loaded) {
        char msg[1100];
        snprintf(msg, sizeof msg, "Cannot load the ROM:\n%s", embedded ? "(embedded)" : rom_path);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Native Console", msg, NULL);
        snes_destroy(m);
        quit_sdl();
        return 1;
    }
    /* Saves and screenshots are named after the executable for an
     * embedded ROM (the hub keeps <game id>.sav next to <game id>.exe). */
    if (embedded) rom_path = argv[0];
    snes_power(m);
    if (program && !interpreter_only && !snes_rc_install(m, program)) {
        fprintf(stderr, "%s does not match the ROM this program was built from; running interpreted\n", rom_path);
    }

    Frontend f;
    f.m = m;
    char save_path[1100], shot_path[1100], state_stem[1100];
    if (save_dir) make_dir(save_dir); /* fails harmlessly if it exists */
    sibling_path(save_path, sizeof save_path, save_dir, rom_path, ".sav");
    sibling_path(shot_path, sizeof shot_path, save_dir, rom_path, ".bmp");
    sibling_path(state_stem, sizeof state_stem, save_dir, rom_path, "");
    f.state_stem = state_stem;
    f.shot_path = shot_path;
    read_save(m, save_path);

    if (prefs_file) {
        f.prefs_path = prefs_file;
    } else {
        char *pref_dir = SDL_GetPrefPath("gbrecompiled", "runtime-snes");
        if (pref_dir) f.prefs_path = std::string(pref_dir) + "snes_prefs.ini";
        SDL_free(pref_dir);
    }
    load_prefs(f);
    f.mode7_scale_this_run = cli_mode7_scale;
    apply_mode7_scale(f);
    if (selftest_verify) {
        const bool volume_kept = f.volume == SELFTEST_VOLUME;
        const bool kept = volume_kept && f.show_fps && f.fullscreen_saved;
        printf("menu selftest: %s the volume set in the menu is %d after a restart\n", volume_kept ? "ok  " : "FAIL",
               f.volume);
        printf("menu selftest: %s the FPS counter is still on after a restart\n", f.show_fps ? "ok  " : "FAIL");
        printf("menu selftest: %s Fullscreen, turned on in the menu, is %s after a restart\n",
               f.fullscreen_saved ? "ok  " : "FAIL", f.fullscreen_saved ? "on" : "off");
        printf("menu selftest: %s\n", kept ? "PASS" : "FAIL");
        snes_destroy(m);
        quit_sdl();
        return kept ? 0 : 1;
    }
    if (cli_scale > 0) f.scale = clamp_int(cli_scale, 1, MAX_SCALE);
    f.mute_this_run = cli_muted;
    f.fullscreen = cli_fullscreen || f.fullscreen_saved;

    f.title = m->cart.title[0] ? m->cart.title : "Super Nintendo";
    char title[128];
    snprintf(title, sizeof title, "%s - Native Console", f.title.c_str());
    f.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W * f.scale,
                             SCREEN_H * f.scale,
                             SDL_WINDOW_RESIZABLE | (f.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    f.ren = f.win ? SDL_CreateRenderer(f.win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (f.win && !f.ren) f.ren = SDL_CreateRenderer(f.win, -1, SDL_RENDERER_SOFTWARE); /* no GPU (VMs, remote desktop) */
    f.tex = f.ren ? SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SNES_FB_WIDTH,
                                      SNES_FB_HEIGHT)
                  : NULL;
    if (!f.tex) {
        fprintf(stderr, "SDL video: %s\n", SDL_GetError());
        snes_destroy(m);
        quit_sdl();
        return 1;
    }
    SDL_SetWindowMinimumSize(f.win, SCREEN_W, SCREEN_H);
    apply_filter(f);
    update_viewport(f);
    if (load_slot >= 1 && load_slot <= SLOT_COUNT) {
        const int kept = f.slot;
        f.slot = load_slot - 1;
        load_state(f);
        f.slot = kept;
    }

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
    SDL_AudioStream *stream = f.audio ? SDL_NewAudioStream(AUDIO_S16SYS, 2, SNES_APU_RATE, have.format, have.channels, have.freq) : NULL;
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
    f.pad_profile = snes_pad_profile(f.pad, f.platform);

    const double freq = (double)SDL_GetPerformanceFrequency();
    double next_frame = (double)SDL_GetPerformanceCounter() / freq;
    uint32_t last_save_check = SDL_GetTicks();
    f.fps_since = SDL_GetTicks();
    bool running = true;
    static int16_t samples[4096 * 2];
    static uint8_t converted[65536];
    uint64_t stat_dry = 0, stat_sum = 0, stat_n = 0;
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
                if (f.large_tex) SDL_DestroyTexture(f.large_tex);
                f.large_tex = nullptr; /* made again when it is next drawn */
                release_filter_textures(f);
                f.tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                          SNES_FB_WIDTH, SNES_FB_HEIGHT);
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
            uint16_t keys = gameplay_keys(f);
            f.held_from_menu &= keys;
            snes_set_buttons(m, 0, (uint16_t)(keys & ~f.held_from_menu));

            uint64_t interpreted = m->cpu.interpreted;
            snes_run_frame(m);
            f.interpreted_last_frame = m->cpu.interpreted - interpreted;
            if (limit_frames > 0 && (long)m->frame_count >= limit_frames) running = false;
            f.fps_frames++;
            Uint32 elapsed = SDL_GetTicks() - f.fps_since;
            if (elapsed >= 1000) {
                f.fps = f.fps_frames * 1000.0 / elapsed;
                f.fps_frames = 0;
                f.fps_since = SDL_GetTicks();
            }

            size_t frames = snes_apu_read_samples(&m->apu, samples, 4096);
            if (stream && !f.fast && f.speed == 100) {
                if (audio_stats) {
                    Uint32 queued = SDL_GetQueuedAudioSize(f.audio);
                    if (m->frame_count > 60 && queued == 0) stat_dry++;
                    if (m->frame_count > 60 && queued < stat_min) stat_min = queued;
                    stat_sum += queued;
                    stat_n++;
                }
                /* Muted audio is still queued, as silence, to keep the device clock running. */
                if (f.muted || f.mute_this_run || f.volume == 0) {
                    memset(samples, 0, frames * 4);
                } else {
                    for (size_t i = 0; i < frames * 2; i++) samples[i] = (int16_t)(samples[i] * f.volume / VOLUME_GAIN_DIVISOR);
                }
                SDL_AudioStreamPut(stream, samples, (int)(frames * 4));
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
            if (m->sram_dirty) write_save(m, save_path);
        }

        /* Pace to the SNES frame rate, steered by the audio queue: below half
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
            double frame = FRAME_SECONDS * 100.0 / f.speed;
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
                        "ran dry %llu times in %llu frames\n",
                SDL_GetCurrentAudioDriver(), have.freq, have.samples, (double)stat_sum / (double)stat_n / bytes_per_ms,
                stat_min == UINT32_MAX ? 0.0 : stat_min / bytes_per_ms, (unsigned long long)stat_dry,
                (unsigned long long)stat_n);
    }
    if (shot_prefix) {
        char path[1100];
        snprintf(path, sizeof path, "%s.bmp", shot_prefix);
        screenshot(m, path);
    }
    if (display_shot) save_display_shot(f, display_shot, display_w, display_h);
    if (m->sram_dirty) write_save(m, save_path);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (f.pad) SDL_GameControllerClose(f.pad);
    if (stream) SDL_FreeAudioStream(stream);
    if (f.audio) SDL_CloseAudioDevice(f.audio);
    release_filter_textures(f);
    snes_crt_free(f.crt);
    if (f.large_tex) SDL_DestroyTexture(f.large_tex);
    SDL_DestroyTexture(f.tex);
    SDL_DestroyRenderer(f.ren);
    SDL_DestroyWindow(f.win);
    snes_destroy(m);
    quit_sdl();
    return selftest.failures ? 1 : 0;
}
