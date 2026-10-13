/*
 * SDL2 frontend for GBA programs: window, input, audio, frame pacing, save
 * files, save states and the settings menu. Recompiled games call
 * gba_sdl_main(); headless options go to gba_runner_main() instead (see
 * gba_sdl_wants_headless()).
 *
 *   <exe> [ROM] [--bios FILE] [--scale N] [--save-dir DIR] [--fullscreen]
 *         [--mute] [--interpreter] [--limit-frames N] [--screenshot-prefix P]
 *         [--prefs FILE] [--load-state SLOT] [--affine-scale N] [--display-shot FILE.bmp]
 *         [--display-size WxH] [--texture-pack FILE.gbatp]
 *         [--record-textures FILE] [--player N]
 *         [--link-host | --link-join HOST[:PORT]] [--link-port N] [--link-local]
 *         [--menu-selftest | --menu-selftest-verify]
 *
 * --mute silences this run only and leaves the saved Mute setting alone. The
 * menu self-test is described above menu_selftest_step().
 *
 * --player N (1-4) runs this window as that player's profile, for several
 * windows of one game on one computer: player 2-4 keep battery saves, save
 * states and screenshots in a "Player N" folder beside the game (unless
 * --save-dir says otherwise) and settings in gba_prefs_pN.ini, read the Nth
 * controller, and place their windows side by side. Player 1 uses the usual
 * files, so its saves are the ones single-player games use.
 * --link-host and --link-join plug in a link cable (gba_link.h) over TCP
 * (gba_net.h, default port 47400): the host is player 1, and the others
 * join it by address (127.0.0.1 on the same computer). --link-local hosts
 * for this computer only (no firewall prompt). The menu's Link Cable
 * section does the same while playing. Linked, a window whose game waits for
 * another player shows who it is waiting for; save states cannot be loaded
 * while other players are linked.
 *
 * --limit-frames exits after N frames; --screenshot-prefix writes P.bmp of
 * the last frame on exit (the hub uses both to capture cover art).
 * --display-shot instead writes the last frame as the display shows it, at
 * --display-size (default 3840x2160) through the saved scaling mode and
 * filter. --prefs reads and writes that settings file instead of the
 * player's own; --load-state loads save state slot SLOT (1-10) at start.
 * --texture-pack shows the game with a texture pack's high-resolution tiles
 * (gba_texture_pack.h; the menu can turn it off to compare).
 * --record-textures appends the video state every 10 frames while it
 * changes, for authoring packs (tools/gba/texpack).
 *
 * Scale filters: Nearest and Linear scale the 240x160 frame in one step.
 * Sharp Linear first scales by the largest whole number that fits, with
 * nearest-neighbour, then smooths only the remaining fraction, so pixels
 * stay square and even at any window size. Smooth runs MMPX
 * (gba_upscale.c) twice for 4x, which redraws stair-step edges and
 * diagonals, then scales linearly to the window. The screen looks are for
 * big windows and fullscreen, where each GBA pixel becomes a large block:
 * Soft scales the frame 3x with nearest-neighbour (the small window's
 * picture) and smooths the rest of the way, so blocks keep their shape with
 * soft edges; LCD Grid draws each pixel whole with faint lines between them,
 * like the GBA's screen; Scanlines is Soft with darker lines along the
 * bottom of each pixel row. The lines are one-pixel-thin textures stretched
 * over the picture and multiplied in, so they cost nothing measurable.
 * CRT draws beams that swell with brightness on the CPU (gba_crt.c), then
 * multiplies a phosphor mask over them and adds a halo.
 *
 * Controls are bindable in the menu; the defaults depend on the platform
 * (gba_input.h): arrows/WASD, Z = A, X = B, Q = L, E = R, Enter = Start,
 * Backspace/Right Shift = Select, Tab = fast forward (hold), M = mute, with
 * F-key shortcuts on Windows and Linux (F5/F8 save/load state, F11
 * fullscreen...) and Command shortcuts on a Mac (Cmd+S, Cmd+L, Cmd+F...).
 * Escape and a controller's Guide button always open the settings menu (the
 * game pauses while it is open); L3 does too by default, and is the way in
 * on a Steam Deck. Controllers use the Nintendo layout: east button = A,
 * south = B, shoulders = L/R.
 *
 * The menu follows the Game Boy runtime's (runtime/src/platform_sdl.cpp).
 * Display, audio, slot and control choices are kept in gba_prefs.ini in the
 * per-user app data folder, shared by all GBA games; bindings are stored
 * only where they differ from the platform's defaults. Save states are
 * <save name>.state1 to .state10 beside the battery save.
 */
#include "gba_sdl.h"

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

#include "gba.h"
#include "gba_input.h"
#include "gba_link.h"
#include "gba_net.h"
#include "gba_recomp.h"
#include "gba_texture_pack.h"
#include "gba_crt.h"
#include "gba_upscale.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_dir(path) mkdir(path, 0777)
#endif

#define FRAME_SECONDS (280896.0 / 16777216.0) /* 59.7275 Hz */

static const char *const k_headless_flags[] = {
    "--headless", "--frames", "--until-idle", "--expect", "--expect-text", "--screen-text",
    "--dump-frame", "--dump-audio", "--differential", "--coverage", "--trace-out", "--auto-input", "--quiet",
    "--hash-frames", "--dump-large",
};

bool gba_sdl_wants_headless(int argc, char **argv)
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
 * arrives every time. The screenshot, display shot and texture pack lines
 * are such text. */
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

static size_t save_size(const GbaMachine *m)
{
    switch (m->save_type) {
    case GBA_SAVE_SRAM: return 0x8000;
    case GBA_SAVE_FLASH64: return 0x10000;
    case GBA_SAVE_FLASH128: return 0x20000;
    case GBA_SAVE_EEPROM: return 0x2000;
    default: return 0;
    }
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

/* The battery save, followed by the clock's state for cartridges with an
 * RTC (GBA_RTC_STATE_SIZE bytes; see gba_rtc_save_state). */
static void write_save(GbaMachine *m, const char *path)
{
    size_t size = save_size(m);
    if (!size && !m->rtc.present) return;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write save %s\n", path);
        return;
    }
    fwrite(m->save, 1, size, f);
    if (m->rtc.present) {
        uint8_t rtc[GBA_RTC_STATE_SIZE];
        gba_rtc_save_state(m, rtc);
        fwrite(rtc, 1, sizeof rtc, f);
    }
    fclose(f);
    m->save_dirty = false;
}

static void read_save(GbaMachine *m, const char *path)
{
    size_t n = 0, size = save_size(m);
    uint8_t *sav = read_file(path, &n);
    if (!sav) return;
    memcpy(m->save, sav, n < size ? n : size);
    if (m->rtc.present && n >= size + GBA_RTC_STATE_SIZE) gba_rtc_load_state(m, sav + size, n - size);
    free(sav);
}

static void screenshot(const GbaMachine *m, const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)m->framebuffer, GBA_SCREEN_WIDTH,
                                                        GBA_SCREEN_HEIGHT, 32, GBA_SCREEN_WIDTH * 4,
                                                        SDL_PIXELFORMAT_ARGB8888);
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
enum { FILTER_NEAREST, FILTER_LINEAR, FILTER_SHARP_LINEAR, FILTER_SMOOTH, FILTER_SOFT, FILTER_LCD_GRID,
       FILTER_SCANLINES, FILTER_CRT, FILTER_COUNT };
static const char *const k_filter_names[] = { "Nearest", "Linear",   "Sharp Linear", "Smooth (MMPX)",
                                              "Soft",    "LCD Grid", "Scanlines",    "CRT" };
enum { SOFT_SCALE = 3 }; /* the small window's pixel size that Soft enlarges */
enum { SMOOTH_FACTOR = 4 }; /* two MMPX 2x passes */
static const char *const k_scale_names[] = { "1x (240x160)",  "2x (480x320)",   "3x (720x480)",   "4x (960x640)",
                                             "5x (1200x800)", "6x (1440x960)", "7x (1680x1120)", "8x (1920x1280)" };
enum { DEFAULT_SCALE = 3, MAX_SCALE = 8, SLOT_COUNT = 10 };

/* Master volume slider position, 0-100, as in the Game Boy runtime. The APU
 * mixes near full int16 scale, far too loud on real speakers, so the output
 * applies VOLUME_GAIN_DIVISOR on top of the slider: 50 % plays at 2 % of
 * the raw APU output. */
enum { VOLUME_DEFAULT = 50, VOLUME_MAX = 100, VOLUME_GAIN_DIVISOR = 2500 };

/* Frontend state kept between frames. */
struct Frontend {
    GbaMachine *m = nullptr;
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    SDL_Texture *tex = nullptr;
    SDL_Texture *smooth_tex = nullptr;   /* the Smooth filter's 4x frame */
    SDL_Texture *prescale_tex = nullptr; /* render target for the whole-number step */
    int prescale_w = 0, prescale_h = 0;
    std::vector<uint32_t> smooth_2x, smooth_4x;
    /* LCD Grid and Scanlines: line textures one pixel across, for a viewport size. */
    SDL_Texture *columns_tex = nullptr, *rows_tex = nullptr;
    int lines_w = 0, lines_h = 0, lines_filter = -1;
    /* CRT: the picture (gba_crt.h), its halo, and the mask as columns_tex. */
    gba_crt *crt = nullptr;
    SDL_Texture *crt_tex = nullptr, *glow_tex = nullptr;
    int crt_line = 0;
    std::vector<uint32_t> crt_frame, glow_frame;

    /* Texture pack: the composed frame at the pack's scale. */
    SDL_Texture *pack_tex = nullptr;
    /* Rotated and scaled layers drawn larger (gba.h, gba_ppu_set_affine_scale):
     * Rotation/Scaling Resolution in the menu, display.affine_scale, and
     * --affine-scale N for one run. The console's own picture is still drawn
     * and is what the game, screenshots, texture packs and the CRT filter
     * get; a frame that had such a layer is displayed from the larger one. */
    int affine_scale = 1;
    int affine_scale_this_run = 0;
    SDL_Texture *large_tex = nullptr;
    int large_w = 0, large_h = 0;
    std::vector<uint32_t> pack_frame;
    bool pack_on = true;      /* shown; the menu can switch back to the original */
    bool pack_frame_ok = false;
    std::string pack_error;
    FILE *record = nullptr;   /* --record-textures */
    uint64_t record_hash = 0;
    uint64_t records = 0;
    SDL_Rect viewport = { 0, 0, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT };
    SDL_GameController *pad = nullptr;
    SDL_AudioDeviceID audio = 0;
    SDL_AudioSpec have = {};
    std::string title;

    bool mute_this_run = false; /* --mute: silent without changing the saved setting */

    /* Kept in gba_prefs.ini. */
    int scale = DEFAULT_SCALE;
    int scaling = SCALING_PIXEL_PERFECT;
    int filter = 0; /* index into k_filter_names */
    bool show_overlay = false;
    bool show_fps = false;
    bool muted = false;
    int volume = VOLUME_DEFAULT;
    int slot = 0;
    GbaInputMap map = {};

    /* Controls. */
    GbaInputPlatform platform = gba_input_host_platform();
    GbaPadProfile pad_profile = GBA_PAD_GENERIC;
    GbaInputMap defaults = {};
    bool action_down[GBA_ACTION_COUNT] = {};   /* held at the last poll */
    bool action_tapped[GBA_ACTION_COUNT] = {}; /* pressed by an event since the last poll */
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
    bool show_menu = false;
    uint16_t held_from_menu = 0; /* buttons still down from using the menu; ignored until released */
    int speed = 100; /* percent; audio plays only at 100 */
    bool fast = false; /* fast forward held: unthrottled, no audio */

    std::string prefs_path, state_stem, shot_path;
    std::string save_dir;   /* where saves go, for the menu */
    std::string status;     /* last save-state action */
    Uint32 status_until = 0; /* shown over the game until then */

    /* Emulated frames per second, measured while the game runs. */
    double fps = 0.0;
    Uint32 fps_since = 0;
    int fps_frames = 0;
    uint64_t interpreted_last_frame = 0;

    /* Player profile and link cable. */
    int player = 0;   /* --player N, 0 = none given */
    int pad_slot = 0; /* which connected controller to read (0 = first), -1 = none; kept in the prefs */
    GbaNet *net = nullptr;
    int link_port = GBA_NET_DEFAULT_PORT;
    char link_address[128] = "127.0.0.1";
    std::string link_error, link_addresses;
    Uint32 stalled_since = 0; /* first tick of the current wait for other players, 0 = running */
    int link_players = 0;     /* last roster size seen, to announce joins and departures */
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
    gba_input_defaults(f.defaults, f.platform);
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
        else if (!strcmp(key, "display.affine_scale")) f.affine_scale = clamp_int(number, 1, GBA_AFFINE_SCALE_MAX);
        else if (!strcmp(key, "display.overlay")) f.show_overlay = number != 0;
        else if (!strcmp(key, "display.fps")) f.show_fps = number != 0;
        else if (!strcmp(key, "audio.muted")) f.muted = number != 0;
        else if (!strcmp(key, "audio.master_volume")) f.volume = clamp_int(number, 0, VOLUME_MAX);
        else if (!strcmp(key, "savestate.slot")) f.slot = clamp_int(number - 1, 0, SLOT_COUNT - 1);
        else if (!strcmp(key, "input.controller")) f.pad_slot = clamp_int(number - 1, -1, 3);
        else if (!strncmp(key, "keyboard.", 9) || !strncmp(key, "controller.", 11)) {
            const bool keyboard = key[0] == 'k';
            char *name = key + (keyboard ? 9 : 11), *dot = strrchr(name, '.');
            if (!dot) continue;
            *dot = '\0';
            int slot = atoi(dot + 1);
            GbaBinding b;
            if (slot < 0 || slot > 1 || !gba_binding_from_config(value, b)) continue;
            if (b.kind != GBA_BIND_NONE && (b.kind == GBA_BIND_KEY) != keyboard) continue;
            for (int a = 0; a < GBA_ACTION_COUNT; a++) {
                if (!strcmp(name, gba_action_config(a))) (keyboard ? f.map.keyboard : f.map.controller)[a][slot] = b;
            }
        }
    }
    fclose(file);
}

static void save_prefs(const Frontend &f)
{
    FILE *file = f.prefs_path.empty() ? NULL : fopen(f.prefs_path.c_str(), "w");
    if (!file) return;
    fprintf(file, "# Native Console Game Boy Advance settings\n");
    fprintf(file, "display.window_scale = %d\n", f.scale);
    fprintf(file, "display.fullscreen = %d\n", f.fullscreen_saved ? 1 : 0);
    fprintf(file, "display.scaling = %d\n", f.scaling);
    fprintf(file, "display.filter = %d\n", f.filter);
    fprintf(file, "display.affine_scale = %d\n", f.affine_scale);
    fprintf(file, "display.overlay = %d\n", f.show_overlay ? 1 : 0);
    fprintf(file, "display.fps = %d\n", f.show_fps ? 1 : 0);
    fprintf(file, "audio.muted = %d\n", f.muted ? 1 : 0);
    fprintf(file, "audio.master_volume = %d\n", f.volume);
    fprintf(file, "savestate.slot = %d\n", f.slot + 1);
    fprintf(file, "input.controller = %d\n", f.pad_slot + 1);
    for (int a = 0; a < GBA_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            if (f.map.keyboard[a][s] != f.defaults.keyboard[a][s]) {
                fprintf(file, "keyboard.%s.%d = %s\n", gba_action_config(a), s,
                        gba_binding_to_config(f.map.keyboard[a][s]).c_str());
            }
            if (f.map.controller[a][s] != f.defaults.controller[a][s]) {
                fprintf(file, "controller.%s.%d = %s\n", gba_action_config(a), s,
                        gba_binding_to_config(f.map.controller[a][s]).c_str());
            }
        }
    }
    fclose(file);
}

/* ------------------------------------------------------------------ */
/* Controls                                                            */
/* ------------------------------------------------------------------ */

static bool binding_down(const Frontend &f, const GbaBinding &b, const Uint8 *kb, uint8_t mods)
{
    switch (b.kind) {
    case GBA_BIND_KEY: return kb[b.code] && gba_key_mods_match(b, mods);
    case GBA_BIND_BUTTON: return f.pad && SDL_GameControllerGetButton(f.pad, (SDL_GameControllerButton)b.code);
    case GBA_BIND_AXIS_POSITIVE: return f.pad && SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code) > 16000;
    case GBA_BIND_AXIS_NEGATIVE: return f.pad && SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code) < -16000;
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

/* GBA buttons the player is holding. */
static uint16_t gameplay_keys(const Frontend &f)
{
    const Uint8 *kb = SDL_GetKeyboardState(NULL);
    uint8_t mods = gba_mods_from_sdl(SDL_GetModState());
    uint16_t keys = 0;
    for (int a = 0; a < GBA_ACTION_GAMEPLAY_COUNT; a++) {
        if (action_held(f, a, kb, mods)) keys |= gba_action_keypad_bit(a);
    }
    return keys;
}

/* A shortcut pressed and released between two polls still counts. */
static void note_shortcut_press(Frontend &f, const SDL_Event &ev)
{
    for (int a = GBA_ACTION_GAMEPLAY_COUNT; a < GBA_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            const GbaBinding &k = f.map.keyboard[a][s], &c = f.map.controller[a][s];
            if (ev.type == SDL_KEYDOWN && k.kind == GBA_BIND_KEY && k.code == ev.key.keysym.scancode &&
                gba_key_mods_match(k, gba_mods_from_sdl(ev.key.keysym.mod)))
                f.action_tapped[a] = true;
            if (ev.type == SDL_CONTROLLERBUTTONDOWN && c.kind == GBA_BIND_BUTTON && c.code == ev.cbutton.button)
                f.action_tapped[a] = true;
        }
    }
}

static void assign_binding(Frontend &f, bool keyboard, int action, int slot, const GbaBinding &b)
{
    GbaBinding(*map)[2] = keyboard ? f.map.keyboard : f.map.controller;
    f.bind_note.clear();
    /* One control does one thing: take the binding off any other action. */
    for (int a = 0; a < GBA_ACTION_COUNT && b.kind != GBA_BIND_NONE; a++) {
        for (int s = 0; s < 2; s++) {
            if ((a != action || s != slot) && map[a][s] == b) {
                map[a][s].kind = GBA_BIND_NONE;
                f.bind_note = gba_binding_label(b, f.platform, f.pad_profile) + " was moved from " + gba_action_label(a) + ".";
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

static void finish_capture(Frontend &f, const GbaBinding *b)
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
        else if (gba_scancode_is_modifier(sc)) f.capture_modifier = sc;
        else {
            GbaBinding b = { GBA_BIND_KEY, gba_mods_from_sdl(ev.key.keysym.mod), (int16_t)sc };
            finish_capture(f, &b);
        }
        return true;
    }
    case SDL_KEYUP:
        if (f.capture_keyboard && ev.key.keysym.scancode == f.capture_modifier) {
            GbaBinding b = { GBA_BIND_KEY, 0, (int16_t)f.capture_modifier };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) finish_capture(f, NULL);
        else if (!f.capture_keyboard) {
            GbaBinding b = { GBA_BIND_BUTTON, 0, (int16_t)ev.cbutton.button };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERAXISMOTION:
        if (!f.capture_keyboard && (ev.caxis.value >= 16000 || ev.caxis.value <= -16000)) {
            GbaBinding b = { (uint8_t)(ev.caxis.value > 0 ? GBA_BIND_AXIS_POSITIVE : GBA_BIND_AXIS_NEGATIVE), 0,
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
    f.pad_profile = gba_pad_profile(pad, f.platform);
}

/* Read the pad_slot-th connected controller (none when there are fewer),
 * so windows for different players take different ones. Called at start
 * and whenever controllers come and go. */
static void choose_pad(Frontend &f)
{
    int seen = 0;
    for (int i = 0; f.pad_slot >= 0 && i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i) || seen++ != f.pad_slot) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        if (f.pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(f.pad)) == id) return;
        set_pad(f, SDL_GameControllerOpen(i));
        return;
    }
    set_pad(f, NULL);
}

/* Where the game goes on a w x h output under a scaling mode. */
static SDL_Rect viewport_for(int scaling, int w, int h)
{
    if (w <= 0) w = GBA_SCREEN_WIDTH;
    if (h <= 0) h = GBA_SCREEN_HEIGHT;
    double sx = (double)w / GBA_SCREEN_WIDTH, sy = (double)h / GBA_SCREEN_HEIGHT;
    int vw = w, vh = h;
    switch (scaling) {
    case SCALING_PIXEL_PERFECT: {
        int s = (int)(sx < sy ? sx : sy);
        if (s < 1) s = 1;
        vw = GBA_SCREEN_WIDTH * s;
        vh = GBA_SCREEN_HEIGHT * s;
        break;
    }
    case SCALING_ASPECT_FIT:
    case SCALING_ASPECT_FILL: {
        double fit = sx < sy ? sx : sy, fill = sx < sy ? sy : sx;
        double s = scaling == SCALING_ASPECT_FIT ? fit : fill;
        vw = (int)(GBA_SCREEN_WIDTH * s + 0.5);
        vh = (int)(GBA_SCREEN_HEIGHT * s + 0.5);
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

/* The filters' extra textures; made again on first use. */
static void release_filter_textures(Frontend &f)
{
    if (f.pack_tex) SDL_DestroyTexture(f.pack_tex);
    f.pack_tex = nullptr;
    if (f.large_tex) SDL_DestroyTexture(f.large_tex);
    f.large_tex = nullptr;
    if (f.smooth_tex) SDL_DestroyTexture(f.smooth_tex);
    if (f.prescale_tex) SDL_DestroyTexture(f.prescale_tex);
    f.smooth_tex = f.prescale_tex = nullptr;
    f.prescale_w = f.prescale_h = 0;
    if (f.columns_tex) SDL_DestroyTexture(f.columns_tex);
    if (f.rows_tex) SDL_DestroyTexture(f.rows_tex);
    f.columns_tex = f.rows_tex = nullptr;
    f.lines_w = f.lines_h = 0;
    f.lines_filter = -1;
    if (f.crt_tex) SDL_DestroyTexture(f.crt_tex);
    if (f.glow_tex) SDL_DestroyTexture(f.glow_tex);
    f.crt_tex = f.glow_tex = nullptr;
    f.crt_line = 0;
}

/* A line texture of px (along x for columns, y for rows), drawn multiplied
 * over the picture. */
static SDL_Texture *line_texture(Frontend &f, const std::vector<uint32_t> &px, bool along_x)
{
    const int n = (int)px.size();
    SDL_Texture *t = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, along_x ? n : 1,
                                       along_x ? 1 : n);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, NULL, px.data(), along_x ? n * 4 : 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_MOD);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
    return t;
}

/* A line texture n pixels long holding a brightness factor per pixel. */
static SDL_Texture *line_texture(Frontend &f, int n, bool along_x, float (*factor)(int i, int n, int cells))
{
    std::vector<uint32_t> px((size_t)n);
    const int cells = along_x ? GBA_SCREEN_WIDTH : GBA_SCREEN_HEIGHT;
    for (int i = 0; i < n; i++) {
        uint32_t v = (uint32_t)(factor(i, n, cells) * 255.0f + 0.5f);
        px[(size_t)i] = 0xFF000000u | (v << 16) | (v << 8) | v;
    }
    return line_texture(f, px, along_x);
}

/* LCD Grid: the last screen pixel of each GBA pixel is a darker line. */
static float grid_factor(int i, int n, int cells)
{
    return (i + 1) * cells / n != i * cells / n || i == n - 1 ? 0.72f : 1.0f;
}

/* Scanlines: brightness dips over the lower part of each GBA pixel row. */
static float scanline_factor(int i, int n, int cells)
{
    float t = (float)((i * cells) % n) / (float)n; /* position within the row, 0-1 */
    if (t < 0.55f) return 1.0f;
    return 1.0f - 0.3f * (float)SDL_sin(3.14159265 * (t - 0.55f) / 0.45f);
}

/* Draws LCD Grid's or Scanlines' lines over dest. Skipped while a GBA pixel
 * is under 3 screen pixels, where the lines would only darken the picture. */
static void draw_lines(Frontend &f, const SDL_Rect &dest)
{
    if (dest.w < GBA_SCREEN_WIDTH * 3 || dest.h < GBA_SCREEN_HEIGHT * 3) return;
    if (f.lines_w != dest.w || f.lines_h != dest.h || f.lines_filter != f.filter) {
        if (f.columns_tex) SDL_DestroyTexture(f.columns_tex);
        if (f.rows_tex) SDL_DestroyTexture(f.rows_tex);
        f.columns_tex = f.filter == FILTER_LCD_GRID ? line_texture(f, dest.w, true, grid_factor) : nullptr;
        f.rows_tex = line_texture(f, dest.h, false, f.filter == FILTER_LCD_GRID ? grid_factor : scanline_factor);
        f.lines_w = dest.w;
        f.lines_h = dest.h;
        f.lines_filter = f.filter;
    }
    if (f.columns_tex) SDL_RenderCopy(f.ren, f.columns_tex, NULL, &dest);
    if (f.rows_tex) SDL_RenderCopy(f.ren, f.rows_tex, NULL, &dest);
}

/* CRT: the picture at whole beam lines (gba_crt_render), scaled to dest,
 * then the phosphor mask multiplied in once a GBA pixel spans a whole
 * stripe triad, then the halo added. False when the viewport is too small
 * for beams or a texture cannot be made; the caller then draws Linear. */
static bool draw_crt(Frontend &f, const SDL_Rect &dest)
{
    const int w = GBA_SCREEN_WIDTH, h = GBA_SCREEN_HEIGHT, cols = w * GBA_CRT_COLUMNS;
    int line = dest.h / h;
    if (line > GBA_CRT_MAX_LINE) line = GBA_CRT_MAX_LINE;
    if (line < GBA_CRT_MIN_LINE) return false;
    if (!f.crt && !(f.crt = gba_crt_create())) return false;
    if (!f.crt_tex || f.crt_line != line) {
        if (f.crt_tex) SDL_DestroyTexture(f.crt_tex);
        f.crt_tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, cols, h * line);
        f.crt_line = f.crt_tex ? line : 0;
        if (!f.crt_tex) return false;
        SDL_SetTextureScaleMode(f.crt_tex, SDL_ScaleModeLinear);
        f.crt_frame.resize((size_t)cols * h * line);
    }
    if (!f.glow_tex) {
        f.glow_tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!f.glow_tex) return false;
        SDL_SetTextureScaleMode(f.glow_tex, SDL_ScaleModeLinear);
        SDL_SetTextureBlendMode(f.glow_tex, SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaMod(f.glow_tex, GBA_CRT_GLOW_ALPHA);
        f.glow_frame.resize((size_t)w * h);
    }
    if (!gba_crt_render(f.crt, f.m->framebuffer, w, h, line, f.crt_frame.data()) ||
        !gba_crt_glow(f.crt, f.m->framebuffer, w, h, f.glow_frame.data()))
        return false;
    SDL_UpdateTexture(f.crt_tex, NULL, f.crt_frame.data(), cols * 4);
    SDL_UpdateTexture(f.glow_tex, NULL, f.glow_frame.data(), w * 4);
    SDL_RenderCopy(f.ren, f.crt_tex, NULL, &dest);
    if (dest.w >= w * 3) {
        if (f.lines_w != dest.w || f.lines_h != dest.h || f.lines_filter != f.filter) {
            if (f.columns_tex) SDL_DestroyTexture(f.columns_tex);
            if (f.rows_tex) SDL_DestroyTexture(f.rows_tex);
            std::vector<uint32_t> mask((size_t)dest.w);
            for (int x = 0; x < dest.w; x++) mask[(size_t)x] = gba_crt_mask(x);
            f.columns_tex = line_texture(f, mask, true);
            f.rows_tex = nullptr;
            f.lines_w = dest.w;
            f.lines_h = dest.h;
            f.lines_filter = f.filter;
        }
        if (f.columns_tex) SDL_RenderCopy(f.ren, f.columns_tex, NULL, &dest);
    }
    SDL_RenderCopy(f.ren, f.glow_tex, NULL, &dest);
    return true;
}

/* The Smooth filter's frame: MMPX 2x twice into smooth_tex. */
static SDL_Texture *smooth_frame(Frontend &f)
{
    const int w = GBA_SCREEN_WIDTH, h = GBA_SCREEN_HEIGHT;
    if (!f.smooth_tex) {
        f.smooth_tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                         w * SMOOTH_FACTOR, h * SMOOTH_FACTOR);
        if (!f.smooth_tex) return nullptr;
        f.smooth_2x.resize((size_t)w * h * 4);
        f.smooth_4x.resize((size_t)w * h * 16);
    }
    if (!gba_mmpx_scale2x(f.m->framebuffer, w, h, f.smooth_2x.data()) ||
        !gba_mmpx_scale2x(f.smooth_2x.data(), w * 2, h * 2, f.smooth_4x.data()))
        return nullptr;
    SDL_UpdateTexture(f.smooth_tex, NULL, f.smooth_4x.data(), w * SMOOTH_FACTOR * 4);
    return f.smooth_tex;
}

/* A render target of w x h for Sharp Linear's whole-number step. */
static SDL_Texture *prescale_target(Frontend &f, int w, int h)
{
    if (f.prescale_tex && f.prescale_w == w && f.prescale_h == h) return f.prescale_tex;
    if (f.prescale_tex) SDL_DestroyTexture(f.prescale_tex);
    f.prescale_tex = SDL_RenderTargetSupported(f.ren)
                         ? SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h)
                         : nullptr;
    f.prescale_w = f.prescale_tex ? w : 0;
    f.prescale_h = f.prescale_tex ? h : 0;
    return f.prescale_tex;
}

static int wanted_affine_scale(const Frontend &f)
{
    return clamp_int(f.affine_scale_this_run > 0 ? f.affine_scale_this_run : f.affine_scale, 1, GBA_AFFINE_SCALE_MAX);
}

static void apply_affine_scale(Frontend &f)
{
    if (f.m && gba_ppu_affine_scale(f.m) != wanted_affine_scale(f)) gba_ppu_set_affine_scale(f.m, wanted_affine_scale(f));
}

/* The larger picture as a texture, or NULL when the last frame has none. */
static SDL_Texture *large_texture(Frontend &f, int *w, int *h)
{
    const uint32_t *large = gba_ppu_large_frame(f.m, w, h);
    if (!large || *w < 1 || *h < 1) return nullptr;
    if (!f.large_tex || f.large_w != *w || f.large_h != *h) {
        if (f.large_tex) SDL_DestroyTexture(f.large_tex);
        f.large_tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, *w, *h);
        f.large_w = *w;
        f.large_h = *h;
    }
    if (f.large_tex) SDL_UpdateTexture(f.large_tex, NULL, large, *w * 4);
    return f.large_tex;
}

/* Draws the current frame into dest on target (NULL: the window) through
 * the chosen filter. A filter whose texture cannot be made falls back to
 * Linear. */
static void draw_game(Frontend &f, SDL_Texture *target, const SDL_Rect &dest)
{
    SDL_UpdateTexture(f.tex, NULL, f.m->framebuffer, GBA_SCREEN_WIDTH * 4);
    if (f.filter == FILTER_CRT && draw_crt(f, dest)) return;
    SDL_Texture *src = f.tex;
    int src_w = GBA_SCREEN_WIDTH, src_h = GBA_SCREEN_HEIGHT;
    SDL_ScaleMode mode =
        f.filter == FILTER_NEAREST || f.filter == FILTER_LCD_GRID ? SDL_ScaleModeNearest : SDL_ScaleModeLinear;
    bool packed = false;
    if (f.pack_on && f.pack_frame_ok) {
        const int ps = (int)gba_texture_pack_scale();
        if (!f.pack_tex) {
            f.pack_tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                           GBA_SCREEN_WIDTH * ps, GBA_SCREEN_HEIGHT * ps);
        }
        if (f.pack_tex) {
            SDL_UpdateTexture(f.pack_tex, NULL, f.pack_frame.data(), GBA_SCREEN_WIDTH * ps * 4);
            src = f.pack_tex;
            src_w *= ps;
            src_h *= ps;
            packed = true;
        }
    }
    if (!packed) {
        /* A frame with rotated or scaled layers drawn larger takes the
         * place a pack's frame would: it is already at a higher resolution. */
        int lw = 0, lh = 0;
        if (SDL_Texture *large = large_texture(f, &lw, &lh)) {
            src = large;
            src_w = lw;
            src_h = lh;
            packed = true;
            /* Nearest keeps pixels whole only at a whole multiple. */
            if (mode == SDL_ScaleModeNearest && (dest.w % lw != 0 || dest.h % lh != 0 || dest.w < lw))
                mode = SDL_ScaleModeLinear;
        }
    }
    if (f.filter == FILTER_SMOOTH && packed) {
        /* The pack's art is already high resolution: scale it linearly. */
    } else if (f.filter == FILTER_SMOOTH) {
        if (SDL_Texture *smooth = smooth_frame(f)) {
            src = smooth;
            src_w *= SMOOTH_FACTOR;
            src_h *= SMOOTH_FACTOR;
        }
    } else if (f.filter == FILTER_LCD_GRID && packed) {
        mode = SDL_ScaleModeLinear; /* the pack's art under the grid */
    } else if ((f.filter == FILTER_SOFT || f.filter == FILTER_SCANLINES) && !packed) {
        /* The small window's picture, then smoothed up to the window. */
        int k = dest.w / src_w < dest.h / src_h ? dest.w / src_w : dest.h / src_h;
        if (k > SOFT_SCALE) k = SOFT_SCALE;
        if (k >= 2) {
            if (SDL_Texture *pre = prescale_target(f, src_w * k, src_h * k)) {
                SDL_SetTextureScaleMode(src, SDL_ScaleModeNearest);
                SDL_SetRenderTarget(f.ren, pre);
                SDL_RenderCopy(f.ren, src, NULL, NULL);
                SDL_SetRenderTarget(f.ren, target);
                src = pre;
            }
        }
    } else if (f.filter == FILTER_SHARP_LINEAR) {
        int k = dest.w / src_w < dest.h / src_h ? dest.w / src_w : dest.h / src_h;
        if (k >= 1 && dest.w == src_w * k && dest.h == src_h * k) {
            mode = SDL_ScaleModeNearest; /* a whole multiple needs no smoothing */
        } else if (k >= 2) {
            if (SDL_Texture *pre = prescale_target(f, src_w * k, src_h * k)) {
                SDL_SetTextureScaleMode(src, SDL_ScaleModeNearest);
                SDL_SetRenderTarget(f.ren, pre);
                SDL_RenderCopy(f.ren, src, NULL, NULL);
                SDL_SetRenderTarget(f.ren, target);
                src = pre;
            }
        }
    }
    SDL_SetTextureScaleMode(src, mode);
    SDL_RenderCopy(f.ren, src, NULL, &dest);
    if (f.filter == FILTER_LCD_GRID || f.filter == FILTER_SCANLINES) draw_lines(f, dest);
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
    draw_game(f, shot, viewport_for(f.scaling, w, h));
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
        SDL_SetWindowSize(f.win, GBA_SCREEN_WIDTH * f.scale, GBA_SCREEN_HEIGHT * f.scale);
        SDL_SetWindowPosition(f.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    update_viewport(f);
}

static void reset_display(Frontend &f)
{
    f.scale = DEFAULT_SCALE;
    f.scaling = SCALING_PIXEL_PERFECT;
    f.filter = 0;
    f.affine_scale = 1;
    f.affine_scale_this_run = 0;
    apply_affine_scale(f);
    f.show_overlay = false;
    f.show_fps = false;
    f.speed = 100;
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
    if (gba_state_save_file(f.m, state_path(f, f.slot).c_str())) set_status(f, "Saved state to " + slot);
    else set_status(f, "Could not write " + state_path(f, f.slot));
}

static bool linked_to_others(const Frontend &f)
{
    if (!f.m->link) return false;
    GbaLinkStatus ls;
    gba_link_status(f.m->link, &ls);
    return ls.players >= 2;
}

static void load_state(Frontend &f)
{
    const std::string path = state_path(f, f.slot), number = std::to_string(f.slot + 1);
    /* The other players' games would not go back with this one. */
    if (linked_to_others(f)) set_status(f, "Save states cannot be loaded while other players are linked");
    else if (!file_exists(path)) set_status(f, "Slot " + number + " is empty");
    else if (gba_state_load_file(f.m, path.c_str())) set_status(f, "Loaded state from slot " + number);
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
    const uint8_t mods = gba_mods_from_sdl(SDL_GetModState());
    const bool baseline = f.input_resync || f.capturing;
    f.input_resync = false;
    for (int a = GBA_ACTION_GAMEPLAY_COUNT; a < GBA_ACTION_COUNT; a++) {
        const bool held = action_held(f, a, kb, mods);
        const bool pressed = (held || f.action_tapped[a]) && !f.action_down[a];
        f.action_down[a] = held;
        f.action_tapped[a] = false;
        if (!pressed || baseline) continue;
        switch (a) {
        case GBA_ACTION_SAVE_STATE: save_state(f); break;
        case GBA_ACTION_LOAD_STATE: load_state(f); break;
        case GBA_ACTION_PREVIOUS_SLOT: select_slot(f, f.slot - 1); break;
        case GBA_ACTION_NEXT_SLOT: select_slot(f, f.slot + 1); break;
        case GBA_ACTION_OVERLAY:
            f.show_overlay = !f.show_overlay;
            save_prefs(f);
            break;
        case GBA_ACTION_FPS:
            f.show_fps = !f.show_fps;
            save_prefs(f);
            break;
        case GBA_ACTION_MUTE:
            f.muted = !f.muted;
            save_prefs(f);
            break;
        case GBA_ACTION_MENU: set_menu(f, !f.show_menu); break;
        case GBA_ACTION_FULLSCREEN: set_fullscreen(f, !f.fullscreen); break;
        case GBA_ACTION_SCREENSHOT: screenshot(f.m, f.shot_path.c_str()); break;
        default: break;
        }
    }
    f.fast = f.action_down[GBA_ACTION_FAST_FORWARD] && !f.capturing;
}

/* ------------------------------------------------------------------ */
/* Link cable                                                          */
/* ------------------------------------------------------------------ */

static void stop_link(Frontend &f)
{
    gba_link_detach(f.m);
    gba_net_close(f.net);
    f.net = nullptr;
    f.stalled_since = 0;
    f.link_players = 0;
}

/* Host (player 1) or join; the game keeps running either way. */
static bool start_link(Frontend &f, bool host, bool local_only)
{
    stop_link(f);
    char game[64], err[200] = "";
    gba_link_game_id(f.m, game, sizeof game);
    f.net = host ? gba_net_host((uint16_t)f.link_port, local_only, game, err, sizeof err)
                 : gba_net_join(f.link_address, game);
    if (!f.net) {
        f.link_error = std::string(host ? "cannot host: " : "cannot join: ") + err;
        return false;
    }
    f.link_error.clear();
    gba_link_attach(f.m, gba_net_io(f.net), gba_net_initial_player(f.net));
    /* Each window reads its own controller even while another has focus. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    f.link_addresses.clear();
    if (host && !local_only) {
        char addresses[256];
        gba_net_local_addresses(addresses, sizeof addresses);
        f.link_addresses = addresses;
    }
    return true;
}

/* One frame, or as much of it as the other players allow within ~40 ms
 * (the window stays responsive while it waits). True when it completed. */
static bool run_frame(Frontend &f)
{
    GbaMachine *m = f.m;
    if (!m->link) {
        gba_run_frame(m);
        return true;
    }
    const Uint32 start = SDL_GetTicks();
    for (;;) {
        gba_run_frame(m);
        if (m->frame_ready) {
            f.stalled_since = 0;
            gba_link_frame_done(m->link);
            return true;
        }
        const Uint32 waited = SDL_GetTicks() - start;
        if (waited >= 40) {
            if (!f.stalled_since) f.stalled_since = start ? start : 1;
            return false;
        }
        gba_link_wait(m->link, (int)(40 - waited));
    }
}

/* Say when players join or leave. */
static void note_roster(Frontend &f)
{
    if (!f.m->link) return;
    GbaLinkStatus ls;
    gba_link_status(f.m->link, &ls);
    int players = ls.self >= 0 ? ls.players : 0;
    if (players != f.link_players) {
        if (players >= 2) {
            set_status(f, "Link cable: " + std::to_string(players) + " players linked, you are Player " +
                              std::to_string(ls.self + 1));
        } else if (f.link_players >= 2) {
            set_status(f, "Link cable: the other players left");
        }
        f.link_players = players;
    }
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
            const GbaBinding &b = (device == 0 ? f.map.keyboard : f.map.controller)[action][s];
            if (b.kind != GBA_BIND_NONE) return gba_binding_label(b, f.platform, f.pad_profile);
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
        ImGui::TextUnformatted(gba_action_label(a));
        for (int s = 0; s < 2; s++) {
            ImGui::TableNextColumn();
            ImGui::PushID(s);
            const GbaBinding b = (keyboard ? f.map.keyboard : f.map.controller)[a][s];
            const bool waiting = f.capturing && f.capture_keyboard == keyboard && f.capture_action == a &&
                                 f.capture_slot == s;
            const std::string label =
                (waiting ? std::string("Press...") : gba_binding_label(b, f.platform, f.pad_profile)) + "###bind";
            float width = ImGui::GetContentRegionAvail().x - clear_width - style.ItemSpacing.x;
            if (ImGui::Button(label.c_str(), ImVec2(width > 40.0f ? width : 40.0f, 0.0f))) start_capture(f, keyboard, a, s);
            ImGui::SameLine();
            ImGui::BeginDisabled(b.kind == GBA_BIND_NONE);
            if (ImGui::Button("Clear")) {
                GbaBinding none = { GBA_BIND_NONE, 0, 0 };
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
    ImGui::TextDisabled("Game Boy Advance Settings");
    ImGui::SameLine();
    if (ImGui::Button("Resume")) set_menu(f, false);
    ImGui::Separator();
    ImGui::TextWrapped("%s", gba_menu_hint(f.platform));
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
                (unsigned long long)f.interpreted_last_frame, (unsigned long long)f.m->interpreted_instructions);

    int window_w = 0, window_h = 0;
    SDL_GetWindowSize(f.win, &window_w, &window_h);
    ImGui::Separator();
    ImGui::TextDisabled("Graphics");
    ImGui::Text("Window: %d x %d", window_w, window_h);
    ImGui::Text("Viewport: %d x %d (%.2fx)", f.viewport.w, f.viewport.h, (double)f.viewport.w / GBA_SCREEN_WIDTH);
    bool fullscreen = f.fullscreen;
    if (ImGui::Checkbox("Fullscreen", &fullscreen)) set_fullscreen(f, fullscreen);
    if (ImGui::Combo("Scaling Mode", &f.scaling, k_scaling_names, IM_ARRAYSIZE(k_scaling_names))) {
        update_viewport(f);
        save_prefs(f);
    }
    if (ImGui::Combo("Scale Filter", &f.filter, k_filter_names, IM_ARRAYSIZE(k_filter_names))) save_prefs(f);
    /* Rotated and scaled backgrounds and sprites with more samples a pixel;
     * the game still gets the console's picture. */
    {
        static const char *const names[] = { "1x (the console's own)", "2x", "3x", "4x" };
        int index = wanted_affine_scale(f) - 1;
        if (ImGui::Combo("Rotation/Scaling Resolution", &index, names, IM_ARRAYSIZE(names))) {
            f.affine_scale = index + 1;
            f.affine_scale_this_run = 0;
            apply_affine_scale(f);
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

    if (gba_texture_pack_active()) {
        ImGui::Checkbox("Use Texture Pack", &f.pack_on);
        GbaTexturePackStats ts = gba_texture_pack_stats();
        ImGui::Text("Texture pack: %s, %zu tiles at %ux", gba_texture_pack_id(), gba_texture_pack_entry_count(),
                    gba_texture_pack_scale());
        ImGui::Text("Last frame: %u tiles replaced, %u recoloured, %u without art", ts.tiles_matched,
                    ts.tiles_recoloured, ts.tiles_missing);
    } else if (!f.pack_error.empty()) {
        ImGui::TextWrapped("Texture pack not loaded: %s", f.pack_error.c_str());
    }
    if (f.record) ImGui::Text("Recording textures: %llu snapshots", (unsigned long long)f.records);

    ImGui::Separator();
    ImGui::TextDisabled("Runtime");
    if (ImGui::Checkbox("Show FPS Counter", &f.show_fps)) save_prefs(f);
    if (ImGui::Checkbox("Show Overlay", &f.show_overlay)) save_prefs(f);
    ImGui::SliderInt("Speed %", &f.speed, 10, 500);
    if (ImGui::Button("Reset Speed")) f.speed = 100;
    const std::string fast_forward = shortcut_hint(f, GBA_ACTION_FAST_FORWARD);
    if (!fast_forward.empty()) ImGui::TextDisabled("Hold %s to fast forward at full speed.", fast_forward.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Link Cable");
    if (f.player > 0) ImGui::TextWrapped("Player %d profile. Saves: %s", f.player, f.save_dir.c_str());
    if (f.net) {
        ImGui::TextWrapped("%s", gba_net_status(f.net));
        GbaLinkStatus ls;
        gba_link_status(f.m->link, &ls);
        if (ls.self >= 0 && ls.players >= 2) {
            ImGui::Text("This window is Player %d of %d (%llu transfers)", ls.self + 1, ls.players,
                        (unsigned long long)ls.transfers);
        }
        if (gba_net_is_host(f.net) && !f.link_addresses.empty()) {
            ImGui::TextWrapped("Other computers join %s (port %d).", f.link_addresses.c_str(), f.link_port);
        }
        if (ImGui::Button("Disconnect")) stop_link(f);
    } else {
        ImGui::TextWrapped("Link with other windows or computers playing this game, as with a GBA link cable "
                           "(Four Swords, trading, multiplayer). Player 1 hosts; the others join it.");
        if (ImGui::InputInt("Port", &f.link_port)) f.link_port = clamp_int(f.link_port, 1024, 65535);
        if (ImGui::Button("Host as Player 1")) start_link(f, true, false);
        ImGui::InputText("Host Address", f.link_address, sizeof f.link_address);
        if (ImGui::Button("Join")) start_link(f, false, false);
        ImGui::TextDisabled("On one computer, join 127.0.0.1. Start the second window as another player "
                            "(the hub's 2 Players button, or --player 2) so its saves stay separate.");
    }
    if (!f.link_error.empty()) ImGui::TextWrapped("%s", f.link_error.c_str());

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
    const std::string save_hint = shortcut_hint(f, GBA_ACTION_SAVE_STATE);
    const std::string load_hint = shortcut_hint(f, GBA_ACTION_LOAD_STATE);
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
    static const char *const pad_choices[] = { "Controller 1", "Controller 2", "Controller 3", "Controller 4",
                                               "None (keyboard only)" };
    int pad_choice = f.pad_slot < 0 ? 4 : f.pad_slot;
    if (ImGui::Combo("Controller", &pad_choice, pad_choices, IM_ARRAYSIZE(pad_choices))) {
        f.pad_slot = pad_choice == 4 ? -1 : pad_choice;
        choose_pad(f);
        save_prefs(f);
    }
    if (f.pad) {
        const char *name = SDL_GameControllerName(f.pad);
        ImGui::Text("Detected Controller: %s", name ? name : "(unnamed)");
    } else {
        ImGui::TextDisabled(f.pad_slot < 0 ? "Keyboard only." : "No controller detected in that position.");
    }
    ImGui::TextWrapped("Choose a binding, then press the new key or button. Escape or Guide cancels. Hold %s with a "
                       "key to bind a combination.",
                       f.platform == GBA_PLATFORM_MAC ? "Command, Control, Option or Shift" : "Ctrl, Alt or Shift");
    if (f.capturing) {
        ImGui::Text("Waiting for a %s for %s...", f.capture_keyboard ? "key" : "controller button",
                    gba_action_label(f.capture_action));
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) finish_capture(f, NULL);
    }
    if (!f.bind_note.empty()) ImGui::TextWrapped("%s", f.bind_note.c_str());
    draw_binding_table(f, "Keyboard Gameplay", true, 0, GBA_ACTION_GAMEPLAY_COUNT);
    draw_binding_table(f, "Controller Gameplay", false, 0, GBA_ACTION_GAMEPLAY_COUNT);
    draw_binding_table(f, "Keyboard Shortcuts", true, GBA_ACTION_GAMEPLAY_COUNT, GBA_ACTION_COUNT);
    draw_binding_table(f, "Controller Shortcuts", false, GBA_ACTION_GAMEPLAY_COUNT, GBA_ACTION_COUNT);
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
        ImGui::TextUnformatted(gba_menu_hint(f.platform));
        const std::string overlay = shortcut_hint(f, GBA_ACTION_OVERLAY);
        if (!overlay.empty()) ImGui::Text("Use %s for Overlay", overlay.c_str());
        ImGui::Text("Viewport: %d x %d (%s)", f.viewport.w, f.viewport.h, k_scaling_names[f.scaling]);
        ImGui::Text("Speed: %s", f.fast ? "fast forward" : (std::to_string(f.speed) + "%").c_str());
        if (f.audio) ImGui::Text("Audio queued: %.0f ms", audio_queue_ms(f));
        ImGui::Text("Interpreted: %llu instructions/frame", (unsigned long long)f.interpreted_last_frame);
        if (f.m->link) {
            GbaLinkStatus ls;
            gba_link_status(f.m->link, &ls);
            if (ls.self >= 0) ImGui::Text("Link: Player %d of %d, %llu transfers", ls.self + 1, ls.players,
                                          (unsigned long long)ls.transfers);
            else ImGui::Text("Link: %s", gba_net_status(f.net));
        }
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

/* Shown while the game waits for another player (its menu is open, its
 * window is busy, or the network is slow). */
static void draw_link_wait(const Frontend &f)
{
    if (!f.m->link || !f.stalled_since || SDL_GetTicks() - f.stalled_since < 500) return;
    GbaLinkStatus ls;
    gba_link_status(f.m->link, &ls);
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("LinkWait", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
        if (ls.waiting_for >= 0) ImGui::Text("Waiting for Player %d...", ls.waiting_for + 1);
        else ImGui::TextUnformatted("Waiting for the other players...");
        ImGui::TextDisabled("Their game is paused, or their window is busy.");
    }
    ImGui::End();
}

static void present(Frontend &f)
{
    SDL_SetRenderDrawColor(f.ren, 0, 0, 0, 255);
    SDL_RenderClear(f.ren);
    draw_game(f, NULL, f.viewport);

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
        draw_link_wait(f);
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
    const GbaMachine *m = f.m;
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

int gba_sdl_main(int argc, char **argv, const GbaRcProgram *program, const char *default_rom)
{
    const char *rom_path = NULL, *bios_path = NULL, *save_dir = NULL, *shot_prefix = NULL, *prefs_file = NULL;
    const char *display_shot = NULL, *pack_path = NULL, *record_path = NULL, *link_join = NULL;
    int cli_affine_scale = 0;
    int cli_scale = 0, load_slot = 0, display_w = 3840, display_h = 2160, player = 0, link_port = GBA_NET_DEFAULT_PORT;
    bool link_host = false, link_local = false;
    long limit_frames = -1;
    bool cli_fullscreen = false, cli_muted = false, interpreter_only = false, audio_stats = false;
    bool selftest_verify = false;
    MenuSelfTest selftest;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--bios") && i + 1 < argc) bios_path = argv[++i];
        else if (!strcmp(a, "--scale") && i + 1 < argc) cli_scale = atoi(argv[++i]);
        else if (!strcmp(a, "--save-dir") && i + 1 < argc) save_dir = argv[++i];
        else if (!strcmp(a, "--fullscreen")) cli_fullscreen = true;
        else if (!strcmp(a, "--mute")) cli_muted = true;
        else if (!strcmp(a, "--affine-scale") && i + 1 < argc) {
            cli_affine_scale = atoi(argv[++i]);
            if (cli_affine_scale < 1 || cli_affine_scale > GBA_AFFINE_SCALE_MAX) {
                fprintf(stderr, "--affine-scale wants 1 to %d\n", GBA_AFFINE_SCALE_MAX);
                return 2;
            }
        }
        else if (!strcmp(a, "--menu-selftest")) selftest.enabled = true;
        else if (!strcmp(a, "--menu-selftest-verify")) selftest_verify = true;
        else if (!strcmp(a, "--audio-stats")) audio_stats = true;
        else if (!strcmp(a, "--interpreter")) interpreter_only = true;
        else if (!strcmp(a, "--limit-frames") && i + 1 < argc) limit_frames = atol(argv[++i]);
        else if (!strcmp(a, "--screenshot-prefix") && i + 1 < argc) shot_prefix = argv[++i];
        else if (!strcmp(a, "--prefs") && i + 1 < argc) prefs_file = argv[++i];
        else if (!strcmp(a, "--load-state") && i + 1 < argc) load_slot = atoi(argv[++i]);
        else if (!strcmp(a, "--display-shot") && i + 1 < argc) display_shot = argv[++i];
        else if (!strcmp(a, "--texture-pack") && i + 1 < argc) pack_path = argv[++i];
        else if (!strcmp(a, "--record-textures") && i + 1 < argc) record_path = argv[++i];
        else if (!strcmp(a, "--player") && i + 1 < argc) player = clamp_int(atoi(argv[++i]), 1, 4);
        else if (!strcmp(a, "--link-host")) link_host = true;
        else if (!strcmp(a, "--link-join") && i + 1 < argc) link_join = argv[++i];
        else if (!strcmp(a, "--link-port") && i + 1 < argc) link_port = clamp_int(atoi(argv[++i]), 1024, 65535);
        else if (!strcmp(a, "--link-local")) link_local = true;
        else if (!strcmp(a, "--display-size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &display_w, &display_h) != 2 || display_w < 1 || display_h < 1 ||
                display_w > 16384 || display_h > 16384) {
                fprintf(stderr, "--display-size wants WxH, e.g. 3840x2160\n");
                return 2;
            }
        }
        else if (a[0] != '-' && !rom_path) rom_path = a;
        else {
            fprintf(stderr, "unknown argument %s\n", a);
            return 2;
        }
    }

    SDL_SetMainReady();
    /* Windows for different players read their controllers even when
     * another window has focus. */
    if (player || link_host || link_join) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
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
        fprintf(stderr, "usage: %s ROM [--bios FILE] [--scale N] [--save-dir DIR] [--fullscreen] [--mute]\n", argv[0]);
        quit_sdl();
        return 2;
    }

    GbaMachine *m = gba_create();
    size_t size = 0;
    bool loaded;
    if (embedded) {
        const uint8_t *data = program->embedded_rom(&size);
        loaded = data && gba_load_rom(m, data, size);
    } else {
        uint8_t *rom = read_file(rom_path, &size);
        loaded = rom && gba_load_rom(m, rom, size);
        free(rom);
    }
    if (!loaded) {
        char msg[1100];
        snprintf(msg, sizeof msg, "Cannot load the ROM:\n%s", embedded ? "(embedded)" : rom_path);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Native Console", msg, NULL);
        gba_destroy(m);
        quit_sdl();
        return 1;
    }
    /* Saves and screenshots are named after the executable for an
     * embedded ROM (the hub keeps <game id>.sav next to <game id>.exe). */
    if (embedded) rom_path = argv[0];
    if (bios_path) {
        uint8_t *bios = read_file(bios_path, &size);
        if (!bios || !gba_load_bios(m, bios, size)) fprintf(stderr, "cannot load BIOS %s; using the built-in one\n", bios_path);
        free(bios);
    }
    gba_reset(m, bios_path == NULL);
    if (program && !interpreter_only && !gba_rc_install(m, program)) {
        fprintf(stderr, "%s does not match the ROM this program was built from; running interpreted\n", rom_path);
    }

    Frontend f;
    f.m = m;
    if (pack_path) {
        GbaTexturePackStatus st = gba_texture_pack_load_file(m, pack_path);
        if (st == GBA_TEXTURE_PACK_OK) {
            const size_t ps = gba_texture_pack_scale();
            f.pack_frame.resize((size_t)GBA_SCREEN_WIDTH * ps * GBA_SCREEN_HEIGHT * ps);
            printf("texture pack: %s (%zu tiles, %zux)\n", gba_texture_pack_id(), gba_texture_pack_entry_count(), ps);
        } else {
            f.pack_error = std::string(pack_path) + ": " + gba_texture_pack_status_string(st);
            fprintf(stderr, "texture pack %s\n", f.pack_error.c_str());
        }
    }
    if (record_path) {
        f.record = fopen(record_path, "ab");
        if (!f.record) fprintf(stderr, "cannot write %s\n", record_path);
    }
    char save_path[1100], shot_path[1100], state_stem[1100];
    /* Players 2-4 keep their own saves beside player 1's. */
    std::string player_dir;
    if (!save_dir && player > 1) {
        char *base = SDL_GetBasePath();
        player_dir = std::string(base ? base : "") + "Player " + std::to_string(player);
        SDL_free(base);
#if defined(__APPLE__)
        /* A .app's own folder is read-only; use the per-user one. */
        if (player_dir.find(".app/Contents/") != std::string::npos) {
            char *pref = SDL_GetPrefPath("gbrecompiled", "runtime-gba");
            player_dir = std::string(pref ? pref : "") + "Player " + std::to_string(player);
            SDL_free(pref);
        }
#endif
        save_dir = player_dir.c_str();
    }
    if (save_dir) make_dir(save_dir); /* fails harmlessly if it exists */
    sibling_path(save_path, sizeof save_path, save_dir, rom_path, ".sav");
    sibling_path(shot_path, sizeof shot_path, save_dir, rom_path, ".bmp");
    sibling_path(state_stem, sizeof state_stem, save_dir, rom_path, "");
    f.state_stem = state_stem;
    f.shot_path = shot_path;
    f.save_dir = save_dir ? save_dir : "beside the game";
    f.player = player;
    f.link_port = link_port;
    if (link_join) snprintf(f.link_address, sizeof f.link_address, "%s", link_join);
    read_save(m, save_path);
    m->rtc.wall_clock = true; /* the cartridge clock follows the host's local time */

    if (prefs_file) {
        f.prefs_path = prefs_file;
    } else {
        char *pref_dir = SDL_GetPrefPath("gbrecompiled", "runtime-gba");
        std::string name = player > 1 ? "gba_prefs_p" + std::to_string(player) + ".ini" : "gba_prefs.ini";
        if (pref_dir) f.prefs_path = std::string(pref_dir) + name;
        SDL_free(pref_dir);
    }
    f.pad_slot = player > 0 ? player - 1 : 0;
    load_prefs(f);
    f.affine_scale_this_run = cli_affine_scale;
    apply_affine_scale(f);
    if (selftest_verify) {
        const bool volume_kept = f.volume == SELFTEST_VOLUME;
        const bool kept = volume_kept && f.show_fps && f.fullscreen_saved;
        printf("menu selftest: %s the volume set in the menu is %d after a restart\n", volume_kept ? "ok  " : "FAIL",
               f.volume);
        printf("menu selftest: %s the FPS counter is still on after a restart\n", f.show_fps ? "ok  " : "FAIL");
        printf("menu selftest: %s Fullscreen, turned on in the menu, is %s after a restart\n",
               f.fullscreen_saved ? "ok  " : "FAIL", f.fullscreen_saved ? "on" : "off");
        printf("menu selftest: %s\n", kept ? "PASS" : "FAIL");
        gba_texture_pack_unload();
        gba_destroy(m);
        quit_sdl();
        return kept ? 0 : 1;
    }
    if (load_slot >= 1 && load_slot <= SLOT_COUNT) {
        f.slot = load_slot - 1;
        load_state(f);
    }
    if (cli_scale > 0) f.scale = clamp_int(cli_scale, 1, MAX_SCALE);
    f.mute_this_run = cli_muted;
    f.fullscreen = cli_fullscreen || f.fullscreen_saved;

    char rom_title[13] = "";
    memcpy(rom_title, m->rom + 0xA0, 12);
    f.title = rom_title[0] ? rom_title : "Game Boy Advance";
    char title[128];
    if (player) snprintf(title, sizeof title, "%s - Player %d - Native Console", f.title.c_str(), player);
    else snprintf(title, sizeof title, "%s - Native Console", f.title.c_str());
    /* Players' windows side by side: 1 and 3 left of centre, 2 and 4 right. */
    int win_x = SDL_WINDOWPOS_CENTERED, win_y = SDL_WINDOWPOS_CENTERED;
    SDL_Rect usable;
    if (player && !f.fullscreen && SDL_GetDisplayUsableBounds(0, &usable) == 0) {
        const int w = GBA_SCREEN_WIDTH * f.scale, h = GBA_SCREEN_HEIGHT * f.scale, gap = 16;
        win_x = usable.x + usable.w / 2 + ((player - 1) % 2 == 0 ? -(w + gap / 2) : gap / 2);
        win_y = usable.y + (usable.h - (player > 2 ? 2 * h + 3 * gap : h)) / 2 + (player > 2 ? h + 3 * gap : 0);
        if (win_x < usable.x) win_x = usable.x;
        if (win_y < usable.y + 32) win_y = usable.y + 32;
    }
    f.win = SDL_CreateWindow(title, win_x, win_y, GBA_SCREEN_WIDTH * f.scale,
                             GBA_SCREEN_HEIGHT * f.scale,
                             SDL_WINDOW_RESIZABLE | (f.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    f.ren = f.win ? SDL_CreateRenderer(f.win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (f.win && !f.ren) f.ren = SDL_CreateRenderer(f.win, -1, SDL_RENDERER_SOFTWARE); /* no GPU (VMs, remote desktop) */
    f.tex = f.ren ? SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, GBA_SCREEN_WIDTH,
                                      GBA_SCREEN_HEIGHT)
                  : NULL;
    if (!f.tex) {
        fprintf(stderr, "SDL video: %s\n", SDL_GetError());
        gba_destroy(m);
        quit_sdl();
        return 1;
    }
    SDL_SetWindowMinimumSize(f.win, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);
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
    SDL_AudioStream *stream = f.audio ? SDL_NewAudioStream(AUDIO_S16SYS, 2, GBA_APU_RATE, have.format, have.channels, have.freq) : NULL;
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

    choose_pad(f);
    f.pad_profile = gba_pad_profile(f.pad, f.platform);
    if (link_host || link_join) start_link(f, link_host, link_local);

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
            else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                if (ev.type == SDL_CONTROLLERDEVICEREMOVED && f.pad &&
                    ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(f.pad))) {
                    set_pad(f, NULL);
                }
                choose_pad(f);
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
                release_filter_textures(f);
                f.tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                          GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);
                if (!f.tex) running = false;
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
        const bool following = gba_link_follows(m->link);
        bool frame_done = false;
        if (!paused) {
            uint16_t keys = gameplay_keys(f);
            f.held_from_menu &= keys;
            gba_set_keys(m, (uint16_t)(keys & ~f.held_from_menu));

            uint64_t interpreted = m->interpreted_instructions;
            frame_done = run_frame(f);
            f.interpreted_last_frame = m->interpreted_instructions - interpreted;
            note_roster(f);
            if (frame_done) {
            if (f.pack_on && gba_texture_pack_active() && !f.fast) f.pack_frame_ok = gba_texture_pack_compose(f.pack_frame.data());
            else if (!gba_texture_pack_active()) f.pack_frame_ok = false;
            if (f.record && m->frame_count % 10 == 0 && gba_texture_capture(m, f.record, &f.record_hash)) f.records++;
            if (limit_frames > 0 && (long)m->frame_count >= limit_frames) running = false;
            f.fps_frames++;
            }
            Uint32 elapsed = SDL_GetTicks() - f.fps_since;
            if (elapsed >= 1000) {
                f.fps = f.fps_frames * 1000.0 / elapsed;
                f.fps_frames = 0;
                f.fps_since = SDL_GetTicks();
            }

            size_t frames = gba_apu_read_samples(m, samples, 4096);
            /* A window paced by player 1 queues what it makes, but never
             * runs its queue up past twice the target. */
            const bool queue_full = following && f.audio && SDL_GetQueuedAudioSize(f.audio) > 2 * audio_target;
            if (stream && !f.fast && f.speed == 100 && !queue_full) {
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
            if (m->save_dirty) write_save(m, save_path);
        }

        /* Pace to the GBA frame rate, steered by the audio queue: below half
         * the target, run the next frame at once (catch up); otherwise run up
         * to 0.5 % fast or slow to hold the queue at the target, since the
         * audio device's clock and the performance counter drift apart.
         * Other speeds pace by the clock alone; the menu redraws at about
         * 60 Hz while the game is paused. */
        double now = (double)SDL_GetPerformanceCounter() / freq;
        if (paused) {
            /* A linked window keeps answering the others (the host passes
             * their messages on) while its menu is open. */
            if (m->link) gba_link_idle(m->link, 16);
            else SDL_Delay(16);
            next_frame = (double)SDL_GetPerformanceCounter() / freq;
        } else if (following || !frame_done) {
            /* Paced by player 1's machine, or already waited for the others. */
            next_frame = now;
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
                    const Uint32 ms = (Uint32)((next_frame - now - 0.0015) * 1000.0);
                    if (m->link) gba_link_idle(m->link, (int)ms);
                    else SDL_Delay(ms);
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
    if (f.record) {
        fclose(f.record);
        printf("recorded %llu texture snapshots\n", (unsigned long long)f.records);
    }
    if (m->save_dirty) write_save(m, save_path);
    stop_link(f);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (f.pad) SDL_GameControllerClose(f.pad);
    if (stream) SDL_FreeAudioStream(stream);
    if (f.audio) SDL_CloseAudioDevice(f.audio);
    release_filter_textures(f);
    gba_crt_free(f.crt);
    gba_texture_pack_unload();
    SDL_DestroyTexture(f.tex);
    SDL_DestroyRenderer(f.ren);
    SDL_DestroyWindow(f.win);
    gba_destroy(m);
    quit_sdl();
    return selftest.failures ? 1 : 0;
}
