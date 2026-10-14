/*
 * SDL2 frontend for DS programs: window, the two screens and their layouts,
 * the stylus, buttons, microphone and lid, audio, frame pacing, the save
 * file and the settings menu. Recompiled games call nds_sdl_main(); headless
 * options go to nds_runner_main() instead (see nds_sdl_wants_headless()).
 * Adapted from the Game Boy Advance frontend (runtimes/runtime-gba/platform/
 * gba_sdl.cpp); the menu follows the Game Boy runtime's.
 *
 *   <exe> [ROM] [--bios9 FILE] [--bios7 FILE] [--scale N] [--layout NAME]
 *         [--save-dir DIR] [--fullscreen] [--mute] [--interpreter]
 *         [--limit-frames N] [--screenshot-prefix P] [--audio-stats]
 *         [--prefs FILE] [--display-shot FILE.bmp] [--display-size WxH]
 *         [--internal-scale N]
 *         [--menu-shot FILE.bmp] [--menu-selftest | --menu-selftest-verify]
 *
 * --limit-frames exits after N frames; --screenshot-prefix writes P.bmp of
 * the last frame on exit, the top screen above the bottom one (256x384), as
 * the headless runner's --dump-frame does. --display-shot instead writes the
 * last frame as the display shows it, at --display-size (default 3840x2160)
 * through the saved layout, scaling mode and filter, and --menu-shot the
 * window as it is with the settings menu open. --layout (stacked, side,
 * large or single) and --scale choose the layout and the window's size for
 * this run; --prefs reads and writes that settings file instead of the
 * player's own (and keeps the firmware beside it). --mute silences this run
 * and leaves the saved Mute setting alone. The menu self-test is described
 * under "Menu self-test" below.
 *
 * Screens. Four layouts (nds_input.h), switched in the menu or with a
 * shortcut: stacked, side by side, one large and one small, and a single
 * screen. Each has a swapped arrangement, remembered per layout. Scaling is
 * by whole numbers (Pixel Perfect) or to fill the display (Fit), with an
 * optional gap between the screens. Filters: Nearest and Linear scale each
 * 256x192 screen in one step; Sharp Linear first scales by the largest
 * whole number that fits, with nearest-neighbour, then smooths only the
 * remaining fraction.
 *
 * Internal resolution. The 3D engine's picture can be drawn at two to four
 * times the screens' size (Internal Resolution in the menu,
 * display.internal_scale, --internal-scale N for one run): polygons get the
 * detail, and backgrounds and sprites stay the screen's own pixels. The
 * game sees the console's picture either way (nds.h,
 * nds_set_internal_scale). A screen showing 3D is then drawn from its
 * larger frame, and the filters above work from that frame's pixels.
 * --screenshot-prefix still writes the console's own picture;
 * --display-shot writes what the display shows.
 *
 * The stylus. The mouse on the bottom screen is the pen: the left button
 * puts it down, and a pen dragged off the screen stays down at the edge
 * until the button is released. A finger on a touch screen is the same. A
 * controller's right stick moves a cursor over the bottom screen and the
 * right trigger puts the pen down on it, either moved about by the stick or
 * following it out from the centre (the menu chooses).
 *
 * Microphone and lid. Holding the blow key or button feeds the microphone
 * loud noise, which is what a game listening for a breath or a shout wants;
 * the menu can also pass this computer's microphone through. The lid is
 * closed while its key is held and opens when it is let go: a toggle left
 * closed would look like a frozen game, since the DS sleeps with its lid
 * shut, and the one thing games ask for is to close it and open it again.
 *
 * Controls are bindable in the menu; the defaults depend on the platform
 * (nds_input.h): arrows/WASD, Z = A, X = B, C = X, V = Y, Q = L, E = R,
 * Enter = Start, Backspace/Right Shift = Select, Space = blow, Tab = fast
 * forward (hold), M = mute, with F-key shortcuts on Windows and Linux (F2
 * next layout, F3 swap screens, F4 lid, F11 fullscreen...) and Command
 * shortcuts on a Mac. Escape and a controller's Guide button always open
 * the settings menu (the game pauses while it is open); L3 does too by
 * default, and is the way in on a Steam Deck. Controllers use the Nintendo
 * layout: east button = A, south = B, north = X, west = Y, shoulders = L/R.
 *
 * Settings are kept in nds_prefs.ini in the per-user app data folder,
 * shared by all DS games; bindings are stored only where they differ from
 * the platform's defaults. The console's own memory (the firmware flash:
 * the owner's nickname and language, and whatever games store there) is
 * nds_firmware.bin in the same folder; the menu changes the nickname and
 * language. The card's save is <name>.sav beside the executable (--save-dir
 * puts it elsewhere). Both files are written a second after the game stops
 * writing to them, when the menu opens and on exit, never part-way: a new
 * file replaces the old one whole. The real-time clock starts at this
 * computer's local time.
 */
#include "nds_sdl.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> /* MoveFileExA, for replace_file() */
#endif

#define SDL_MAIN_HANDLED /* callers keep their own main() */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include "nds.h"
#include "nds_input.h"
#include "nds_recomp.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_dir(path) mkdir(path, 0777)
#endif

#define FRAME_SECONDS ((double)NDS_CYCLES_PER_FRAME / NDS_CLOCK_HZ) /* 59.8261 Hz */
/* The mixer makes 32,728.5 frames a second; SDL's resampler takes a whole
 * number, and the pacing below absorbs the half. */
#define AUDIO_RATE ((NDS_CLOCK_HZ + NDS_CYCLES_PER_SAMPLE / 2) / NDS_CYCLES_PER_SAMPLE)

/* Plain ints, not enumerators: they are multiplied by scales, and C++20
 * frowns on arithmetic between an enumeration and a floating-point type. */
static const int SCREEN_W = NDS_SCREEN_WIDTH, SCREEN_H = NDS_SCREEN_HEIGHT;

static const char *const k_headless_flags[] = {
    /* The GBA frontend's. */
    "--headless", "--frames", "--until-idle", "--expect", "--expect-text", "--screen-text",
    "--dump-frame", "--dump-audio", "--differential", "--coverage", "--trace-out", "--auto-input", "--quiet",
    "--hash-frames",
    /* What the DS runner adds (nds_runner.c). --interpreter, --bios9 and
     * --bios7 are the window's as well, so they decide nothing. A new
     * runner option goes here too: tests/nds/test_nds_input.cpp reads the
     * runner's source and fails if one is missing. */
    "--keys", "--touch", "--lid", "--dump-every", "--dump-from", "--sha-frames", "--state-hash", "--audio-hash",
    "--dump-ram", "--dump-mem", "--load-save", "--dump-save", "--regs", "--differential-step", "--trace", "--profile",
    "--dump-large",
};

/* Runner options that carry their value in the same word: --auto-input=RUN. */
static const char *const k_headless_prefixes[] = { "--auto-input=" };

bool nds_sdl_wants_headless(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        for (size_t k = 0; k < sizeof k_headless_flags / sizeof k_headless_flags[0]; k++) {
            if (!strcmp(argv[i], k_headless_flags[k])) return true;
        }
        for (size_t k = 0; k < sizeof k_headless_prefixes / sizeof k_headless_prefixes[0]; k++) {
            if (!strncmp(argv[i], k_headless_prefixes[k], strlen(k_headless_prefixes[k]))) return true;
        }
    }
    return false;
}

/* SDL_Quit, with what has been printed sent on its way first. On Windows,
 * with a controller plugged in, a program that quits within a moment of
 * SDL_Init(SDL_INIT_GAMECONTROLLER) loses whatever is still in stdout's
 * buffer about one time in fifty: SDL_Quit returns, the exit code is right,
 * and the text never arrives (measured with a twenty-line program; flushed
 * first, it arrives every time). The self-test's verdict is such text. */
static void quit_sdl(void)
{
    fflush(stdout);
    fflush(stderr);
    SDL_Quit();
}

static FILE *open_file(const char *path, const char *mode)
{
#ifdef _MSC_VER
    FILE *f = NULL;
    return fopen_s(&f, path, mode) == 0 ? f : NULL;
#else
    return fopen(path, mode);
#endif
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = open_file(path, "rb");
    *size = 0;
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

/* `from` takes the place of `to`, which may exist. */
static bool replace_file(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}

/* Both screens as the machine drew them, the top one above the bottom one:
 * 256x384, the picture the headless runner's --dump-frame writes. */
static void screenshot(const NdsMachine *m, const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)m->framebuffer, SCREEN_W, 2 * SCREEN_H, 32, SCREEN_W * 4,
                                                        SDL_PIXELFORMAT_RGB888);
    if (s) {
        if (SDL_SaveBMP(s, path) == 0) printf("screenshot: %s\n", path);
        SDL_FreeSurface(s);
    }
}

/* This computer's local time as the clock chip counts it: seconds since
 * 1970-01-01 00:00 of local time, not of UTC. */
static int64_t local_clock_seconds(void)
{
    const time_t now = time(NULL);
    struct tm t;
#ifdef _WIN32
    if (localtime_s(&t, &now) != 0) return NDS_RTC_DEFAULT_BASE;
#else
    if (!localtime_r(&now, &t)) return NDS_RTC_DEFAULT_BASE;
#endif
    const int year = t.tm_year + 1900;
    /* Leap days from 1970 up to the start of this year. */
    const int64_t leap_days = (year - 1) / 4 - (year - 1) / 100 + (year - 1) / 400 - (1969 / 4 - 1969 / 100 + 1969 / 400);
    const int64_t days = (int64_t)(year - 1970) * 365 + leap_days + t.tm_yday;
    return days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

/* The game's name from the banner in the ROM (GBATEK, "DS Cartridge
 * Icon/Title"): the English title is up to three lines of UTF-16, the last
 * of them the publisher. Without a banner, the header's 12-character title. */
static std::string game_title(const NdsMachine *m)
{
    std::vector<std::string> lines(1);
    const uint32_t banner = m->rom_size >= 0x200 ? nds_get_le32(m->rom + 0x68) : 0;
    if (banner && banner <= m->rom_size && m->rom_size - banner >= 0x440) {
        const uint8_t *text = m->rom + banner + 0x340;
        for (int i = 0; i < 128; i++) {
            const unsigned c = nds_get_le16(text + i * 2);
            if (!c) break;
            if (c == '\n') {
                lines.push_back(std::string());
            } else if (c < 0x80) {
                lines.back() += (char)c;
            } else if (c < 0x800) {
                lines.back() += (char)(0xC0 | (c >> 6));
                lines.back() += (char)(0x80 | (c & 0x3F));
            } else {
                lines.back() += (char)(0xE0 | (c >> 12));
                lines.back() += (char)(0x80 | ((c >> 6) & 0x3F));
                lines.back() += (char)(0x80 | (c & 0x3F));
            }
        }
    }
    if (lines.size() > 1) lines.pop_back();
    std::string title;
    for (const std::string &line : lines) {
        if (line.empty()) continue;
        if (!title.empty()) title += ' ';
        title += line;
    }
    if (title.empty() && m->rom_size >= 12) {
        for (int i = 0; i < 12 && m->rom[i] >= 0x20 && m->rom[i] < 0x7F; i++) title += (char)m->rom[i];
    }
    return title.empty() ? "Nintendo DS" : title;
}

/* ------------------------------------------------------------------ */
/* Frontend state                                                      */
/* ------------------------------------------------------------------ */

enum { FILTER_NEAREST, FILTER_LINEAR, FILTER_SHARP_LINEAR, FILTER_COUNT };
static const char *const k_filter_names[] = { "Nearest", "Linear", "Sharp Linear" };
enum { DEFAULT_SCALE = 2, MAX_SCALE = 8 };
/* How large the 3D engine's picture is drawn, in multiples of a screen's own
 * 256 by 192 (nds.h, nds_set_internal_scale). */
static const char *const k_internal_scale_names[] = { "1x (the console's own)", "2x (512 by 384)", "3x (768 by 576)",
                                                      "4x (1024 by 768)" };

/* Master volume slider position, 0-100, as in the Game Boy runtime. The
 * sound unit's output is its 10-bit level across the whole int16 range,
 * far too loud on real speakers, so the output applies VOLUME_GAIN_DIVISOR
 * on top of the slider: 50 % plays at 2 % of the raw output, as on GBA. */
enum { VOLUME_DEFAULT = 50, VOLUME_MAX = 100, VOLUME_GAIN_DIVISOR = 2500 };

/* The controller's stylus cursor: moved about by the stick, or following
 * it out from the middle of the screen and back. */
enum { CURSOR_MOVE, CURSOR_FROM_CENTRE, CURSOR_MODE_COUNT };
static const char *const k_cursor_mode_names[] = { "Move With the Stick", "Follow the Stick From the Centre" };
enum { CURSOR_SPEED_MIN = 25, CURSOR_SPEED_MAX = 300 }; /* percent */
#define CURSOR_PIXELS_PER_SECOND 320.0 /* at 100 %, the stick pushed all the way */
#define CURSOR_DEAD_ZONE 0.15

/* A stick or trigger pushed this far counts as a button held. */
enum { AXIS_PRESSED = 16000 };

/* The audio queue is held at a target of 80 ms; sound is dropped rather
 * than queued once it is this many targets deep. */
enum { AUDIO_QUEUE_LIMIT = 6 };

/* The microphone: a 12-bit level, silent at the middle. This computer's
 * microphone is amplified MIC_BOOST times before the game hears it (the
 * console's own amplifier has gains of 20 to 160). */
enum { MIC_SILENCE = 0x800, MIC_BOOST_DEFAULT = 4, MIC_BOOST_MAX = 16 };

/* The save and the firmware are written once the game has left them alone
 * for this long. */
enum { SETTLE_FRAMES = 60 };

/* A file that follows part of the machine. */
struct KeptFile {
    std::string path;
    bool pending = false; /* the game changed it and the file does not have that yet */
    int quiet = 0;        /* frames since it last did */
    uint64_t writes = 0;
};
/* The menu's "close the lid" keeps it shut this long. */
enum { LID_MENU_FRAMES = 120 };

static const char *const k_language_names[] = { "Japanese", "English", "French", "German", "Italian", "Spanish" };

struct ScreenTextures {
    SDL_Texture *frame = nullptr;    /* the screen as the machine drew it */
    SDL_Texture *large = nullptr;    /* ... and with its 3D drawn larger, when there is such a frame */
    int large_w = 0, large_h = 0;
    SDL_Texture *prescale = nullptr; /* render target for Sharp Linear's whole-number step */
    int prescale_w = 0, prescale_h = 0;
};

/* The binding button the menu's keyboard or controller focus is on, which
 * the self-test steers by. */
struct BindingFocus {
    bool on = false; /* focus is on a binding's button, or on its Clear */
    bool keyboard = false, clear = false;
    int action = 0, slot = 0;
};

/* Frontend state kept between frames. */
struct Frontend {
    NdsMachine *m = nullptr;
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    ScreenTextures tex[2];    /* [0] top, [1] bottom */
    NdsScreens screens = {};  /* where they go in the window */
    int out_w = 0, out_h = 0; /* the renderer's output, in pixels */
    int win_w = 0, win_h = 0; /* the window, in the units mouse events use */
    SDL_GameController *pad = nullptr;
    bool pad_fixed = false; /* the self-test's controller: kept as others come and go */
    SDL_AudioDeviceID audio = 0;
    SDL_AudioSpec have = {};
    std::string title;

    /* Kept in nds_prefs.ini. */
    int scale = DEFAULT_SCALE;
    int scaling = NDS_SCALING_PIXEL_PERFECT;
    int filter = FILTER_NEAREST;
    int internal_scale = 1;          /* the setting */
    int internal_scale_this_run = 0; /* --internal-scale: this run only */
    int layout = NDS_LAYOUT_STACKED;
    bool swapped[NDS_LAYOUT_COUNT] = {};
    int gap = 0;
    bool show_overlay = false;
    bool show_fps = false;
    bool muted = false;
    int volume = VOLUME_DEFAULT;
    int cursor_mode = CURSOR_MOVE;
    int cursor_speed = 100;
    bool host_mic = false; /* pass this computer's microphone to the game */
    int mic_boost = MIC_BOOST_DEFAULT;
    int pad_slot = 0; /* which connected controller to read (0 = first), -1 = none */
    NdsInputMap map = {};

    /* Controls. Keys are followed from their events, not asked of SDL, so
     * that events the self-test makes up count like any others. */
    NdsInputPlatform platform = nds_input_host_platform();
    NdsPadProfile pad_profile = NDS_PAD_GENERIC;
    NdsInputMap defaults = {};
    bool key_down[SDL_NUM_SCANCODES] = {};
    bool action_down[NDS_ACTION_COUNT] = {};   /* held at the last poll */
    bool action_tapped[NDS_ACTION_COUNT] = {}; /* pressed by an event since the last poll */
    bool input_resync = false;                 /* take the next poll as the baseline, firing nothing */
    bool capturing = false;                    /* waiting for a key or button to bind */
    bool capture_keyboard = true;
    int capture_action = 0, capture_slot = 0;
    int capture_modifier = SDL_SCANCODE_UNKNOWN; /* modifier pressed alone so far */
    bool pad_nav_blocked = false; /* controller menu navigation off until its buttons are released */
    std::string bind_note;
    BindingFocus focus;

    bool fullscreen = false;
    /* What the settings file holds (display.fullscreen): the last choice made in
     * the menu, with the key or by the system - not --fullscreen, which is one
     * run's. */
    bool fullscreen_saved = false;
    bool window_stale = false;  /* the layout changed while the window could not be resized for it */
    bool mute_this_run = false; /* --mute: silent without changing the saved setting */
    bool show_menu = false;
    uint32_t held_from_menu = 0; /* console actions still down from using the menu; ignored until released */
    int speed = 100; /* percent; audio plays only at 100 */
    bool fast = false; /* fast forward held: unthrottled, no audio */
    bool unthrottled = false; /* the self-test: as fast as the loop goes */

    /* The stylus. Positions are renderer output pixels. */
    bool mouse_left = false, pen_mouse = false;
    double mouse_x = 0.0, mouse_y = 0.0;
    bool finger_down = false, pen_finger = false;
    SDL_FingerID finger_id = 0;
    double finger_x = 0.0, finger_y = 0.0;
    bool pen_blocked = false; /* down since the menu closed: no touch until it is lifted */
    double cursor_x = SCREEN_W / 2.0, cursor_y = SCREEN_H / 2.0; /* the controller's cursor, bottom-screen pixels */
    Uint32 cursor_shown_until = 0;

    /* Microphone and lid. */
    bool blowing = false;
    bool mic_live = false; /* the game was given something other than silence last frame */
    uint32_t noise = 0x2545F491u;
    SDL_AudioDeviceID capture = 0;
    std::vector<int16_t> mic_queue; /* captured samples not yet given to the game */
    size_t mic_read = 0;
    int mic_last = MIC_SILENCE, mic_peak = 0;
    std::string mic_error;
    bool lid_closed = false;
    int lid_frames = 0; /* closed from the menu for this many more frames */

    /* Files. */
    std::string prefs_path, shot_path;
    KeptFile save, firmware;
    bool save_keep_old = false; /* the save file was not what the card holds: keep it as .bak before writing over it */

    std::string status;      /* last message for the player */
    Uint32 status_until = 0; /* shown over the game until then */

    /* Emulated frames per second, measured while the game runs. */
    double fps = 0.0;
    Uint32 fps_since = 0;
    int fps_frames = 0;
    uint64_t interpreted_last_frame[2] = {};
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

static void set_status(Frontend &f, const std::string &message)
{
    f.status = message;
    f.status_until = SDL_GetTicks() + 2500;
    printf("%s\n", message.c_str());
}

/* ------------------------------------------------------------------ */
/* Settings file                                                       */
/* ------------------------------------------------------------------ */

/* Settings, then bindings as keyboard.<action>.<slot> = <binding> (and
 * controller.*) where they differ from the platform's defaults, so a later
 * change of default reaches everyone who has not rebound that control. */
static void load_prefs(Frontend &f)
{
    nds_input_defaults(f.defaults, f.platform);
    f.map = f.defaults;
    FILE *file = f.prefs_path.empty() ? NULL : open_file(f.prefs_path.c_str(), "r");
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
        else if (!strcmp(key, "display.scaling")) f.scaling = clamp_int(number, 0, NDS_SCALING_COUNT - 1);
        else if (!strcmp(key, "display.filter")) f.filter = clamp_int(number, 0, FILTER_COUNT - 1);
        else if (!strcmp(key, "display.internal_scale")) f.internal_scale = clamp_int(number, 1, NDS_INTERNAL_SCALE_MAX);
        else if (!strcmp(key, "display.layout")) {
            if (nds_layout_from_config(value) >= 0) f.layout = nds_layout_from_config(value);
        } else if (!strncmp(key, "display.swapped.", 16)) {
            if (nds_layout_from_config(key + 16) >= 0) f.swapped[nds_layout_from_config(key + 16)] = number != 0;
        } else if (!strcmp(key, "display.gap")) f.gap = clamp_int(number, 0, NDS_GAP_MAX);
        else if (!strcmp(key, "display.overlay")) f.show_overlay = number != 0;
        else if (!strcmp(key, "display.fps")) f.show_fps = number != 0;
        else if (!strcmp(key, "audio.muted")) f.muted = number != 0;
        else if (!strcmp(key, "audio.master_volume")) f.volume = clamp_int(number, 0, VOLUME_MAX);
        else if (!strcmp(key, "touch.cursor_mode")) f.cursor_mode = clamp_int(number, 0, CURSOR_MODE_COUNT - 1);
        else if (!strcmp(key, "touch.cursor_speed")) f.cursor_speed = clamp_int(number, CURSOR_SPEED_MIN, CURSOR_SPEED_MAX);
        else if (!strcmp(key, "microphone.host")) f.host_mic = number != 0;
        else if (!strcmp(key, "microphone.boost")) f.mic_boost = clamp_int(number, 1, MIC_BOOST_MAX);
        else if (!strcmp(key, "input.controller")) f.pad_slot = clamp_int(number - 1, -1, 3);
        else if (!strncmp(key, "keyboard.", 9) || !strncmp(key, "controller.", 11)) {
            const bool keyboard = key[0] == 'k';
            char *name = key + (keyboard ? 9 : 11), *dot = strrchr(name, '.');
            if (!dot) continue;
            *dot = '\0';
            int slot = atoi(dot + 1);
            NdsBinding b;
            if (slot < 0 || slot > 1 || !nds_binding_from_config(value, b)) continue;
            if (b.kind != NDS_BIND_NONE && (b.kind == NDS_BIND_KEY) != keyboard) continue;
            for (int a = 0; a < NDS_ACTION_COUNT; a++) {
                if (!strcmp(name, nds_action_config(a))) (keyboard ? f.map.keyboard : f.map.controller)[a][slot] = b;
            }
        }
    }
    fclose(file);
}

static void save_prefs(const Frontend &f)
{
    FILE *file = f.prefs_path.empty() ? NULL : open_file(f.prefs_path.c_str(), "w");
    if (!file) return;
    fprintf(file, "# Native Console Nintendo DS settings\n");
    fprintf(file, "display.window_scale = %d\n", f.scale);
    fprintf(file, "display.fullscreen = %d\n", f.fullscreen_saved ? 1 : 0);
    fprintf(file, "display.scaling = %d\n", f.scaling);
    fprintf(file, "display.filter = %d\n", f.filter);
    fprintf(file, "display.internal_scale = %d\n", f.internal_scale);
    fprintf(file, "display.layout = %s\n", nds_layout_config(f.layout));
    for (int i = 0; i < NDS_LAYOUT_COUNT; i++) {
        fprintf(file, "display.swapped.%s = %d\n", nds_layout_config(i), f.swapped[i] ? 1 : 0);
    }
    fprintf(file, "display.gap = %d\n", f.gap);
    fprintf(file, "display.overlay = %d\n", f.show_overlay ? 1 : 0);
    fprintf(file, "display.fps = %d\n", f.show_fps ? 1 : 0);
    fprintf(file, "audio.muted = %d\n", f.muted ? 1 : 0);
    fprintf(file, "audio.master_volume = %d\n", f.volume);
    fprintf(file, "touch.cursor_mode = %d\n", f.cursor_mode);
    fprintf(file, "touch.cursor_speed = %d\n", f.cursor_speed);
    fprintf(file, "microphone.host = %d\n", f.host_mic ? 1 : 0);
    fprintf(file, "microphone.boost = %d\n", f.mic_boost);
    fprintf(file, "input.controller = %d\n", f.pad_slot + 1);
    for (int a = 0; a < NDS_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            if (f.map.keyboard[a][s] != f.defaults.keyboard[a][s]) {
                fprintf(file, "keyboard.%s.%d = %s\n", nds_action_config(a), s,
                        nds_binding_to_config(f.map.keyboard[a][s]).c_str());
            }
            if (f.map.controller[a][s] != f.defaults.controller[a][s]) {
                fprintf(file, "controller.%s.%d = %s\n", nds_action_config(a), s,
                        nds_binding_to_config(f.map.controller[a][s]).c_str());
            }
        }
    }
    fclose(file);
}

/* ------------------------------------------------------------------ */
/* The save and the firmware                                           */
/* ------------------------------------------------------------------ */

/* Bytes to a file by way of a temporary one that then takes its place, so
 * that a crash or a full disk part-way cannot leave half a file where a
 * whole one was. */
static bool write_whole_file(KeptFile &kept, const uint8_t *data, size_t size)
{
    const std::string temp = kept.path + ".tmp";
    FILE *file = open_file(temp.c_str(), "wb");
    bool ok = file && fwrite(data, 1, size, file) == size;
    if (file && fclose(file) != 0) ok = false;
    if (ok) ok = replace_file(temp.c_str(), kept.path.c_str());
    if (ok) {
        kept.writes++;
    } else {
        fprintf(stderr, "cannot write %s\n", kept.path.c_str());
        remove(temp.c_str());
    }
    return ok;
}

/* The save file into the card's memory, before the reset that starts the
 * game. A file that is not a size the card could hold gives up its whole
 * power-of-two part (DeSmuME's .dsv is the save followed by a footer), and
 * the file is then kept as .bak when the game first saves over it. */
static void read_save(Frontend &f)
{
    size_t size = 0;
    uint8_t *data = read_file(f.save.path.c_str(), &size);
    if (!data) return;
    uint32_t have = 0;
    if (!nds_save_memory(f.m, &have)) {
        fprintf(stderr, "%s ignored: this game has no save memory\n", f.save.path.c_str());
    } else if (!nds_load_save(f.m, data, size)) {
        size_t whole = 1;
        while (whole * 2 <= size) whole *= 2;
        f.save_keep_old = true;
        if (whole != size && nds_load_save(f.m, data, whole)) {
            fprintf(stderr, "%s: using the first %zu of its %zu bytes; the file is kept as .bak when the game saves\n",
                    f.save.path.c_str(), whole, size);
        } else {
            fprintf(stderr, "%s is not a save this card could hold (%zu bytes); it is kept as .bak when the game saves\n",
                    f.save.path.c_str(), size);
        }
    }
    free(data);
}

/* The card's save memory to the save file. A card whose chip is not known
 * yet has nothing to write. */
static void write_save(Frontend &f)
{
    uint32_t size = 0;
    const uint8_t *data = nds_save_memory(f.m, &size);
    f.m->save_dirty = false;
    f.save.pending = false;
    f.save.quiet = 0;
    if (!data || !size || f.save.path.empty()) return;
    if (f.save_keep_old) replace_file(f.save.path.c_str(), (f.save.path + ".bak").c_str());
    f.save_keep_old = false;
    write_whole_file(f.save, data, size);
}

/* The console's own memory: nds_firmware.bin, beside the settings file.
 * Without one the machine has the image it built for itself. A user's own
 * dump put there is used as it is. */
static void read_firmware(Frontend &f)
{
    size_t size = 0;
    uint8_t *data = f.firmware.path.empty() ? NULL : read_file(f.firmware.path.c_str(), &size);
    if (data && !nds_load_firmware(f.m, data, size)) {
        fprintf(stderr, "%s is not a DS firmware image (%zu bytes, not %d); it is left alone until a game or the menu changes "
                        "the console's settings\n",
                f.firmware.path.c_str(), size, (int)NDS_FIRMWARE_SIZE);
    }
    free(data);
}

static void write_firmware(Frontend &f)
{
    f.m->firmware_dirty = false;
    f.firmware.pending = false;
    f.firmware.quiet = 0;
    if (!f.firmware.path.empty()) write_whole_file(f.firmware, nds_firmware_image(f.m), NDS_FIRMWARE_SIZE);
}

/* Whether it is time to write a file: `changed` is the machine's flag for
 * it, cleared here. A game writes its save a page at a time over many
 * frames; the file is written once, when a second has gone by without
 * another change. */
static bool settled(KeptFile &kept, bool &changed)
{
    if (changed) {
        changed = false;
        kept.pending = true;
        kept.quiet = 0;
        return false;
    }
    return kept.pending && ++kept.quiet >= SETTLE_FRAMES;
}

/* After each frame. */
static void track_files(Frontend &f)
{
    if (settled(f.save, f.m->save_dirty)) write_save(f);
    if (settled(f.firmware, f.m->firmware_dirty)) write_firmware(f);
}

/* Write now whatever the game has changed since the last write: the menu
 * is opening, or the program is ending. */
static void flush_files(Frontend &f)
{
    if (f.m->save_dirty || f.save.pending) write_save(f);
    if (f.m->firmware_dirty || f.firmware.pending) write_firmware(f);
}

/* The owner's settings, as the menu changes them. The machine builds a new
 * firmware image around them (what games had stored in the old one is
 * gone), which is written at once; the running game finds them when it
 * next starts. */
static void set_owner(Frontend &f, const NdsUserSettings &settings)
{
    nds_set_user_settings(f.m, &settings);
    write_firmware(f);
}

static void set_owner_language(Frontend &f, int language)
{
    NdsUserSettings settings = f.m->settings;
    settings.language = (uint8_t)clamp_int(language, NDS_LANGUAGE_JAPANESE, NDS_LANGUAGE_SPANISH);
    set_owner(f, settings);
}

/* A nickname from the keyboard: the printable ASCII among its first ten
 * characters. */
static void set_owner_nickname(Frontend &f, const char *nickname)
{
    NdsUserSettings settings = f.m->settings;
    settings.nickname_length = 0;
    for (; *nickname && settings.nickname_length < NDS_NICKNAME_MAX; nickname++) {
        if (*nickname >= 0x20 && *nickname < 0x7F) settings.nickname[settings.nickname_length++] = (uint8_t)*nickname;
    }
    set_owner(f, settings);
}

/* The nickname as text, for the menu: characters it cannot show become '?'. */
static void owner_nickname(const Frontend &f, char out[NDS_NICKNAME_MAX + 1])
{
    const NdsUserSettings &settings = f.m->settings;
    size_t n = 0;
    for (; n < settings.nickname_length && n < NDS_NICKNAME_MAX; n++) {
        out[n] = settings.nickname[n] >= 0x20 && settings.nickname[n] < 0x7F ? (char)settings.nickname[n] : '?';
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Controls                                                            */
/* ------------------------------------------------------------------ */

static uint8_t held_mods(const Frontend &f)
{
    const bool *k = f.key_down;
    return (uint8_t)(((k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]) ? NDS_MOD_CTRL : 0) |
                     ((k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) ? NDS_MOD_SHIFT : 0) |
                     ((k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]) ? NDS_MOD_ALT : 0) |
                     ((k[SDL_SCANCODE_LGUI] || k[SDL_SCANCODE_RGUI]) ? NDS_MOD_GUI : 0));
}

static bool binding_down(const Frontend &f, const NdsBinding &b)
{
    switch (b.kind) {
    case NDS_BIND_KEY: return f.key_down[b.code] && nds_key_mods_match(b, held_mods(f));
    case NDS_BIND_BUTTON: return f.pad && SDL_GameControllerGetButton(f.pad, (SDL_GameControllerButton)b.code);
    case NDS_BIND_AXIS_POSITIVE:
        return f.pad && SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code) > AXIS_PRESSED;
    case NDS_BIND_AXIS_NEGATIVE:
        return f.pad && SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code) < -AXIS_PRESSED;
    default: return false;
    }
}

static bool action_held(const Frontend &f, int action)
{
    for (int s = 0; s < 2; s++) {
        if (binding_down(f, f.map.keyboard[action][s]) || binding_down(f, f.map.controller[action][s])) return true;
    }
    return false;
}

/* How far an action is pushed, 0 to 1: a stick's or trigger's travel, or 1
 * for a key or a button. */
static double action_amount(const Frontend &f, int action)
{
    double most = 0.0;
    for (int s = 0; s < 2; s++) {
        const NdsBinding &c = f.map.controller[action][s];
        if (binding_down(f, f.map.keyboard[action][s]) || (c.kind == NDS_BIND_BUTTON && binding_down(f, c))) most = 1.0;
        if (f.pad && (c.kind == NDS_BIND_AXIS_POSITIVE || c.kind == NDS_BIND_AXIS_NEGATIVE)) {
            double v = SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)c.code) / 32767.0;
            if (c.kind == NDS_BIND_AXIS_NEGATIVE) v = -v;
            if (v > most) most = v;
        }
    }
    return most > 1.0 ? 1.0 : most;
}

/* What the player is holding for the console: a bit per action below
 * NDS_ACTION_CONSOLE_COUNT. */
static uint32_t console_actions(const Frontend &f)
{
    uint32_t held = 0;
    for (int a = 0; a < NDS_ACTION_CONSOLE_COUNT; a++) {
        if (action_held(f, a)) held |= 1u << a;
    }
    return held;
}

/* A shortcut pressed and released between two polls still counts. */
static void note_shortcut_press(Frontend &f, const SDL_Event &ev)
{
    for (int a = NDS_ACTION_CONSOLE_COUNT; a < NDS_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            const NdsBinding &k = f.map.keyboard[a][s], &c = f.map.controller[a][s];
            if (ev.type == SDL_KEYDOWN && k.kind == NDS_BIND_KEY && k.code == ev.key.keysym.scancode &&
                nds_key_mods_match(k, held_mods(f)))
                f.action_tapped[a] = true;
            if (ev.type == SDL_CONTROLLERBUTTONDOWN && c.kind == NDS_BIND_BUTTON && c.code == ev.cbutton.button)
                f.action_tapped[a] = true;
        }
    }
}

static void assign_binding(Frontend &f, bool keyboard, int action, int slot, const NdsBinding &b)
{
    NdsBinding(*map)[2] = keyboard ? f.map.keyboard : f.map.controller;
    f.bind_note.clear();
    /* One control does one thing: take the binding off any other action. */
    for (int a = 0; a < NDS_ACTION_COUNT && b.kind != NDS_BIND_NONE; a++) {
        for (int s = 0; s < 2; s++) {
            if ((a != action || s != slot) && map[a][s] == b) {
                map[a][s].kind = NDS_BIND_NONE;
                f.bind_note = nds_binding_label(b, f.platform, f.pad_profile) + " was moved from " + nds_action_label(a) + ".";
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

static void finish_capture(Frontend &f, const NdsBinding *b)
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
        else if (nds_scancode_is_modifier(sc)) f.capture_modifier = sc;
        else {
            NdsBinding b = { NDS_BIND_KEY, held_mods(f), (int16_t)sc };
            finish_capture(f, &b);
        }
        return true;
    }
    case SDL_KEYUP:
        if (f.capture_keyboard && ev.key.keysym.scancode == f.capture_modifier) {
            NdsBinding b = { NDS_BIND_KEY, 0, (int16_t)f.capture_modifier };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) finish_capture(f, NULL);
        else if (!f.capture_keyboard) {
            NdsBinding b = { NDS_BIND_BUTTON, 0, (int16_t)ev.cbutton.button };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERAXISMOTION:
        if (!f.capture_keyboard && (ev.caxis.value >= AXIS_PRESSED || ev.caxis.value <= -AXIS_PRESSED)) {
            NdsBinding b = { (uint8_t)(ev.caxis.value > 0 ? NDS_BIND_AXIS_POSITIVE : NDS_BIND_AXIS_NEGATIVE), 0,
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
        if (v >= AXIS_PRESSED || v <= -AXIS_PRESSED) return true;
    }
    return false;
}

/* The menu is steered by the controller the game reads, not by whichever
 * one Dear ImGui would open for itself. */
static void set_pad(Frontend &f, SDL_GameController *pad)
{
    static SDL_GameController *none[1] = { NULL };
    if (f.pad && f.pad != pad) SDL_GameControllerClose(f.pad);
    f.pad = pad;
    f.pad_profile = nds_pad_profile(pad, f.platform);
    /* With no controller the list holds a null one, which is never read:
     * controller navigation is switched off while f.pad is null. */
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, pad ? &f.pad : none, 1);
}

/* Read the pad_slot-th connected controller (none when there are fewer).
 * Called at start and whenever controllers come and go. */
static void choose_pad(Frontend &f)
{
    if (f.pad_fixed) return;
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

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

/* Where the screens go inside the window (renderer output pixels). */
static void update_layout(Frontend &f)
{
    SDL_GetRendererOutputSize(f.ren, &f.out_w, &f.out_h);
    SDL_GetWindowSize(f.win, &f.win_w, &f.win_h);
    f.screens = nds_layout_screens(f.layout, f.swapped[f.layout], f.gap, f.scaling, f.out_w, f.out_h);
}

/* The largest window scale up to `wanted` at which the layout, with a
 * title bar, fits the display the window is on. */
static int fitting_scale(const Frontend &f, int wanted)
{
    SDL_Rect usable;
    const int display = f.win ? SDL_GetWindowDisplayIndex(f.win) : 0;
    if (SDL_GetDisplayUsableBounds(display < 0 ? 0 : display, &usable) != 0) return wanted;
    for (int k = wanted; k > 1; k--) {
        int w = 0, h = 0;
        nds_layout_size(f.layout, f.gap, k, &w, &h);
        if (w <= usable.w && h + 40 <= usable.h) return k;
    }
    return 1;
}

/* Make the window the layout's size at the chosen scale. A fullscreen or
 * maximized window keeps its size, with the screens placed in it anew, and
 * takes the layout's when it next becomes an ordinary window. */
static void apply_window_size(Frontend &f)
{
    f.window_stale = f.fullscreen || (SDL_GetWindowFlags(f.win) & SDL_WINDOW_MAXIMIZED) != 0;
    if (!f.window_stale) {
        int w = 0, h = 0, min_w = 0, min_h = 0;
        nds_layout_size(f.layout, f.gap, fitting_scale(f, f.scale), &w, &h);
        nds_layout_size(f.layout, f.gap, 1, &min_w, &min_h);
        SDL_SetWindowMinimumSize(f.win, min_w, min_h);
        SDL_SetWindowSize(f.win, w, h);
        SDL_SetWindowPosition(f.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    update_layout(f);
}

static void set_layout(Frontend &f, int layout)
{
    f.layout = (layout % NDS_LAYOUT_COUNT + NDS_LAYOUT_COUNT) % NDS_LAYOUT_COUNT;
    apply_window_size(f);
    save_prefs(f);
}

static bool create_screen_textures(Frontend &f)
{
    for (ScreenTextures &t : f.tex) {
        t.frame = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
        if (!t.frame) return false;
        SDL_SetTextureBlendMode(t.frame, SDL_BLENDMODE_NONE);
    }
    return true;
}

/* Sharp Linear's render targets; made again on first use. */
static void release_filter_textures(Frontend &f)
{
    for (ScreenTextures &t : f.tex) {
        if (t.prescale) SDL_DestroyTexture(t.prescale);
        t.prescale = nullptr;
        t.prescale_w = t.prescale_h = 0;
    }
}

static void destroy_screen_textures(Frontend &f)
{
    release_filter_textures(f);
    for (ScreenTextures &t : f.tex) {
        if (t.frame) SDL_DestroyTexture(t.frame);
        if (t.large) SDL_DestroyTexture(t.large);
        t.frame = t.large = nullptr;
        t.large_w = t.large_h = 0;
    }
}

/* The internal resolution in force: the setting, or --internal-scale. */
static int wanted_internal_scale(const Frontend &f)
{
    return clamp_int(f.internal_scale_this_run > 0 ? f.internal_scale_this_run : f.internal_scale, 1,
                     NDS_INTERNAL_SCALE_MAX);
}

static void apply_internal_scale(Frontend &f)
{
    if (f.m) nds_set_internal_scale(f.m, wanted_internal_scale(f));
}

/* A screen's larger frame as a texture, or null when the screen has none
 * (nds_fine_frame) or the texture cannot be made: then the caller draws the
 * screen's own. */
static SDL_Texture *large_texture(Frontend &f, int screen, int *w, int *h)
{
    ScreenTextures &t = f.tex[screen];
    const uint32_t *pixels = nds_fine_frame(f.m, screen, w, h);
    if (!pixels) return nullptr;
    if (!t.large || t.large_w != *w || t.large_h != *h) {
        if (t.large) SDL_DestroyTexture(t.large);
        t.large = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, *w, *h);
        t.large_w = t.large ? *w : 0;
        t.large_h = t.large ? *h : 0;
        if (!t.large) return nullptr;
        SDL_SetTextureBlendMode(t.large, SDL_BLENDMODE_NONE);
    }
    SDL_UpdateTexture(t.large, NULL, pixels, *w * 4);
    return t.large;
}

/* A render target of w x h for Sharp Linear's whole-number step. */
static SDL_Texture *prescale_target(Frontend &f, ScreenTextures &t, int w, int h)
{
    if (t.prescale && t.prescale_w == w && t.prescale_h == h) return t.prescale;
    if (t.prescale) SDL_DestroyTexture(t.prescale);
    t.prescale = SDL_RenderTargetSupported(f.ren)
                     ? SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h)
                     : nullptr;
    t.prescale_w = t.prescale ? w : 0;
    t.prescale_h = t.prescale ? h : 0;
    return t.prescale;
}

/* Draws one screen's current frame into dest on target (NULL: the window)
 * through the chosen filter. Sharp Linear without a render target is
 * Linear. */
static void draw_screen(Frontend &f, SDL_Texture *target, int screen, const SDL_Rect &dest)
{
    ScreenTextures &t = f.tex[screen];
    /* The screen's own 256 by 192, or the frame with its 3D drawn larger:
     * the filters work from whichever it is. */
    int src_w = SCREEN_W, src_h = SCREEN_H;
    SDL_Texture *src = large_texture(f, screen, &src_w, &src_h);
    if (!src) {
        src_w = SCREEN_W;
        src_h = SCREEN_H;
        SDL_UpdateTexture(t.frame, NULL, f.m->framebuffer[screen], SCREEN_W * 4);
        src = t.frame;
    }
    SDL_ScaleMode mode = f.filter == FILTER_NEAREST ? SDL_ScaleModeNearest : SDL_ScaleModeLinear;
    /* Shown smaller than it was drawn, nearest would drop whole pixels. */
    if (dest.w < src_w || dest.h < src_h) mode = SDL_ScaleModeLinear;
    if (f.filter == FILTER_SHARP_LINEAR) {
        const int k = dest.w / src_w < dest.h / src_h ? dest.w / src_w : dest.h / src_h;
        if (k >= 1 && dest.w == src_w * k && dest.h == src_h * k) {
            mode = SDL_ScaleModeNearest; /* a whole multiple needs no smoothing */
        } else if (k >= 2) {
            if (SDL_Texture *pre = prescale_target(f, t, src_w * k, src_h * k)) {
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
}

/* Both screens (or the one the layout shows) where `screens` puts them. */
static void draw_game(Frontend &f, SDL_Texture *target, const NdsScreens &screens)
{
    for (int i = 0; i < 2; i++) {
        if (screens.rect[i].w > 0) draw_screen(f, target, i, screens.rect[i]);
    }
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
    draw_game(f, shot, nds_layout_screens(f.layout, f.swapped[f.layout], f.gap, f.scaling, w, h));
    const bool read = SDL_RenderReadPixels(f.ren, NULL, SDL_PIXELFORMAT_ARGB8888, pixels.data(), w * 4) == 0;
    SDL_SetRenderTarget(f.ren, NULL);
    SDL_DestroyTexture(shot);
    SDL_Surface *s = read ? SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_RGB888)
                          : nullptr;
    const bool ok = s && SDL_SaveBMP(s, path) == 0;
    if (s) SDL_FreeSurface(s);
    if (ok) {
        printf("display shot: %s (%dx%d, %s%s, %s, %s)\n", path, w, h, nds_layout_label(f.layout),
               f.swapped[f.layout] ? " swapped" : "", nds_scaling_label(f.scaling), k_filter_names[f.filter]);
    } else {
        fprintf(stderr, "display shot: %s\n", SDL_GetError());
    }
    return ok;
}

static void set_fullscreen(Frontend &f, bool on)
{
    if (SDL_SetWindowFullscreen(f.win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0) f.fullscreen = on;
    if (f.fullscreen_saved != f.fullscreen) {
        f.fullscreen_saved = f.fullscreen;
        save_prefs(f);
    }
    update_layout(f);
}

/* ------------------------------------------------------------------ */
/* The stylus                                                          */
/* ------------------------------------------------------------------ */

static bool on_touch_screen(const Frontend &f, double x, double y)
{
    int tx = 0, ty = 0;
    return nds_layout_touch(f.screens, x, y, &tx, &ty);
}

/* Whether a pointer that is down may become the pen: not while the menu is
 * open, and not if it has been down since the menu closed. */
static bool pen_allowed(const Frontend &f)
{
    return !f.show_menu && !f.pen_blocked;
}

/* The mouse as the pen, down or up. While it is down the window keeps
 * hearing the mouse wherever it goes, so that a drag off the window still
 * ends when the button is let go. (SDL does this by itself from 2.0.22.) */
static void set_pen_mouse(Frontend &f, bool down)
{
    if (down != f.pen_mouse) SDL_CaptureMouse(down ? SDL_TRUE : SDL_FALSE);
    f.pen_mouse = down;
    if (down) f.cursor_shown_until = 0; /* the mouse has taken over from the controller's cursor */
}

/* The mouse and a finger on a touch screen are both the stylus. The pen
 * goes down when the pointer is pressed on the bottom screen, or slides on
 * to it while pressed, and stays down wherever it is dragged until it is
 * lifted. Positions arrive in window units and are kept in output pixels. */
static void pointer_event(Frontend &f, const SDL_Event &ev)
{
    const double sx = f.win_w > 0 ? (double)f.out_w / f.win_w : 1.0, sy = f.win_h > 0 ? (double)f.out_h / f.win_h : 1.0;
    switch (ev.type) {
    case SDL_MOUSEMOTION:
        if (ev.motion.which == SDL_TOUCH_MOUSEID) break; /* a finger: handled below */
        f.mouse_x = ev.motion.x * sx;
        f.mouse_y = ev.motion.y * sy;
        if (f.mouse_left && !f.pen_mouse && pen_allowed(f) && on_touch_screen(f, f.mouse_x, f.mouse_y)) set_pen_mouse(f, true);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (ev.button.which == SDL_TOUCH_MOUSEID || ev.button.button != SDL_BUTTON_LEFT) break;
        f.mouse_x = ev.button.x * sx;
        f.mouse_y = ev.button.y * sy;
        f.mouse_left = ev.type == SDL_MOUSEBUTTONDOWN;
        if (!f.mouse_left) {
            set_pen_mouse(f, false);
            if (!f.finger_down) f.pen_blocked = false;
        } else if (pen_allowed(f) && on_touch_screen(f, f.mouse_x, f.mouse_y)) {
            set_pen_mouse(f, true);
        }
        break;
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION: {
        /* A laptop's touchpad also reports fingers, in its own coordinates. */
        const SDL_TouchDeviceType type = SDL_GetTouchDeviceType(ev.tfinger.touchId);
        if (type == SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE || type == SDL_TOUCH_DEVICE_INDIRECT_RELATIVE) break;
        if (ev.type == SDL_FINGERDOWN && !f.finger_down) {
            f.finger_down = true; /* the stylus is one point: the first finger down is it */
            f.finger_id = ev.tfinger.fingerId;
        } else if (!f.finger_down || ev.tfinger.fingerId != f.finger_id) {
            break;
        }
        f.finger_x = ev.tfinger.x * f.out_w;
        f.finger_y = ev.tfinger.y * f.out_h;
        if (!f.pen_finger && pen_allowed(f) && on_touch_screen(f, f.finger_x, f.finger_y)) {
            f.pen_finger = true;
            f.cursor_shown_until = 0;
        }
        break;
    }
    case SDL_FINGERUP:
        if (!f.finger_down || ev.tfinger.fingerId != f.finger_id) break;
        f.finger_down = f.pen_finger = false;
        if (!f.mouse_left) f.pen_blocked = false;
        break;
    default: break;
    }
}

/* Lift every pointer: the window lost the keyboard, or the display changed
 * under them. */
static void release_pointers(Frontend &f)
{
    set_pen_mouse(f, false);
    f.mouse_left = f.finger_down = f.pen_finger = f.pen_blocked = false;
}

/* The controller's cursor, once a frame. In Move mode the stick pushes it
 * about, slowly near the stick's centre and faster towards its rim; in
 * From Centre mode it sits where the stick points, measured from the
 * middle of the screen, and returns there when the stick is let go. */
static void update_cursor(Frontend &f, bool touching)
{
    double dx = action_amount(f, NDS_ACTION_CURSOR_RIGHT) - action_amount(f, NDS_ACTION_CURSOR_LEFT);
    double dy = action_amount(f, NDS_ACTION_CURSOR_DOWN) - action_amount(f, NDS_ACTION_CURSOR_UP);
    const double length = sqrt(dx * dx + dy * dy);
    const bool pushed = length > CURSOR_DEAD_ZONE;
    if (pushed) {
        double amount = (length - CURSOR_DEAD_ZONE) / (1.0 - CURSOR_DEAD_ZONE);
        if (amount > 1.0) amount = 1.0;
        dx /= length;
        dy /= length;
        if (f.cursor_mode == CURSOR_FROM_CENTRE) {
            /* A little past the edges, so that the corners are in reach of
             * a stick that moves in a circle. */
            f.cursor_x = SCREEN_W / 2.0 + dx * amount * SCREEN_W * 0.625;
            f.cursor_y = SCREEN_H / 2.0 + dy * amount * SCREEN_H * 0.625;
        } else {
            const double step = CURSOR_PIXELS_PER_SECOND * (f.cursor_speed / 100.0) * amount * amount * FRAME_SECONDS;
            f.cursor_x += dx * step;
            f.cursor_y += dy * step;
        }
    } else if (f.cursor_mode == CURSOR_FROM_CENTRE) {
        f.cursor_x = SCREEN_W / 2.0;
        f.cursor_y = SCREEN_H / 2.0;
    }
    f.cursor_x = f.cursor_x < 0.0 ? 0.0 : f.cursor_x > SCREEN_W - 0.01 ? SCREEN_W - 0.01 : f.cursor_x;
    f.cursor_y = f.cursor_y < 0.0 ? 0.0 : f.cursor_y > SCREEN_H - 0.01 ? SCREEN_H - 0.01 : f.cursor_y;
    if (pushed || touching) f.cursor_shown_until = SDL_GetTicks() + 3000;
}

/* ------------------------------------------------------------------ */
/* Microphone                                                          */
/* ------------------------------------------------------------------ */

static void close_capture(Frontend &f)
{
    if (f.capture) SDL_CloseAudioDevice(f.capture);
    f.capture = 0;
    f.mic_queue.clear();
    f.mic_read = 0;
    f.mic_peak = 0;
}

/* Open this computer's microphone: one channel at the rate the game is fed
 * (SDL converts from whatever the device records at). Only ever called
 * because the player switched it on. */
static bool open_capture(Frontend &f)
{
    close_capture(f);
    SDL_AudioSpec want = {}, got = {};
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    f.capture = SDL_OpenAudioDevice(NULL, 1, &want, &got, 0);
    if (!f.capture) {
        f.mic_error = SDL_GetError();
        fprintf(stderr, "microphone: %s\n", f.mic_error.c_str());
        return false;
    }
    f.mic_error.clear();
    SDL_PauseAudioDevice(f.capture, f.show_menu ? 1 : 0);
    return true;
}

/* Collect what the microphone recorded since the last frame. */
static void pump_capture(Frontend &f)
{
    if (!f.capture) return;
    int16_t chunk[1024];
    Uint32 got;
    while ((got = SDL_DequeueAudio(f.capture, chunk, sizeof chunk)) > 0) {
        f.mic_queue.insert(f.mic_queue.end(), chunk, chunk + got / sizeof chunk[0]);
    }
    /* What is more than a tenth of a second old the game would hear late:
     * drop it (the game ran slow, or was fast-forwarded past). */
    const size_t most = AUDIO_RATE / 10;
    if (f.mic_queue.size() - f.mic_read > most) f.mic_read = f.mic_queue.size() - most;
    if (f.mic_read > 8192) {
        f.mic_queue.erase(f.mic_queue.begin(), f.mic_queue.begin() + (std::ptrdiff_t)f.mic_read);
        f.mic_read = 0;
    }
    f.mic_peak = f.mic_peak * 7 / 8; /* the menu's level meter falls back between peaks */
}

/* The next level for the microphone, one per mixer period. Blowing is loud
 * white noise from a xorshift generator: a breath across a microphone is
 * noise, and a game that listens for one measures how loud and how
 * sustained the signal is. Otherwise this computer's microphone if it is
 * on, repeating its last sample when the recording has not caught up. */
static uint16_t next_microphone_sample(Frontend &f)
{
    if (f.blowing) {
        f.noise ^= f.noise << 13;
        f.noise ^= f.noise >> 17;
        f.noise ^= f.noise << 5;
        return (uint16_t)(f.noise >> 20);
    }
    if (f.capture && f.mic_read < f.mic_queue.size()) {
        const int level = f.mic_queue[f.mic_read++] * f.mic_boost / 16; /* 16 bits to 12, amplified */
        f.mic_last = clamp_int(MIC_SILENCE + level, 0, 0xFFF);
        if (abs(level) > f.mic_peak) f.mic_peak = abs(level) > 0x7FF ? 0x7FF : abs(level);
    }
    return (uint16_t)(f.capture ? f.mic_last : MIC_SILENCE);
}

/* One frame of the game. */
static void run_frame(Frontend &f)
{
    NdsMachine *m = f.m;
    if (!f.blowing && !f.capture) {
        if (f.mic_live) nds_set_microphone(m, MIC_SILENCE);
        f.mic_live = false;
        nds_run_frame(m);
        return;
    }
    f.mic_live = true;
    pump_capture(f);
    /* NEEDS FROM nds.h: a way to give the microphone samples at the rate
     * the game reads them. nds_set_microphone() sets one level that holds
     * until the next call, and a level that holds for a whole frame is
     * silence to a game. Until there is one, this is nds_run_frame() spelt
     * out with nds_run_slice() and NdsMachine.frame_ready, stopping every
     * mixer period (1024 cycles, 32.7 kHz) to set the next level. */
    m->frame_ready = false;
    uint64_t next_sample = m->now;
    while (!m->frame_ready) {
        if (m->now >= next_sample) {
            nds_set_microphone(m, next_microphone_sample(f));
            next_sample += NDS_CYCLES_PER_SAMPLE;
        }
        nds_run_slice(m, next_sample);
    }
}

/* ------------------------------------------------------------------ */
/* Menu and shortcuts                                                  */
/* ------------------------------------------------------------------ */

/* The game runs only while the menu is closed; the audio device pauses with
 * it so what is queued plays on when the game resumes, and the microphone
 * stops recording. Whatever is held as the menu closes (Enter or a face
 * button that chose Resume, a mouse button) must not reach the game. */
static void set_menu(Frontend &f, bool open)
{
    f.show_menu = open;
    if (f.audio) SDL_PauseAudioDevice(f.audio, open ? 1 : 0);
    if (f.capture) {
        SDL_PauseAudioDevice(f.capture, open ? 1 : 0);
        SDL_ClearQueuedAudio(f.capture);
    }
    if (open) {
        flush_files(f);
        set_pen_mouse(f, false);
        f.pen_finger = false;
    } else {
        f.held_from_menu = console_actions(f);
        f.pen_blocked = f.mouse_left || f.finger_down;
        if (f.capturing) finish_capture(f, NULL);
    }
}

/* Fires shortcuts pressed since the last call and updates fast forward.
 * Runs once per loop, after events, whether or not the game is paused.
 * Nothing fires while a binding is being chosen or a text box is being
 * typed in. */
static void run_shortcuts(Frontend &f)
{
    const bool baseline = f.input_resync || f.capturing || ImGui::GetIO().WantTextInput;
    f.input_resync = false;
    for (int a = NDS_ACTION_CONSOLE_COUNT; a < NDS_ACTION_COUNT; a++) {
        const bool held = action_held(f, a);
        const bool pressed = (held || f.action_tapped[a]) && !f.action_down[a];
        f.action_down[a] = held;
        f.action_tapped[a] = false;
        if (!pressed || baseline) continue;
        switch (a) {
        case NDS_ACTION_NEXT_LAYOUT:
            set_layout(f, f.layout + 1);
            set_status(f, std::string("Layout: ") + nds_layout_label(f.layout));
            break;
        case NDS_ACTION_SWAP_SCREENS:
            f.swapped[f.layout] = !f.swapped[f.layout];
            update_layout(f);
            save_prefs(f);
            break;
        case NDS_ACTION_OVERLAY:
            f.show_overlay = !f.show_overlay;
            save_prefs(f);
            break;
        case NDS_ACTION_FPS:
            f.show_fps = !f.show_fps;
            save_prefs(f);
            break;
        case NDS_ACTION_MUTE:
            f.muted = !f.muted;
            save_prefs(f);
            break;
        case NDS_ACTION_MENU: set_menu(f, !f.show_menu); break;
        case NDS_ACTION_FULLSCREEN: set_fullscreen(f, !f.fullscreen); break;
        case NDS_ACTION_SCREENSHOT: screenshot(f.m, f.shot_path.c_str()); break;
        default: break;
        }
    }
    f.fast = f.action_down[NDS_ACTION_FAST_FORWARD] && !baseline;
}

static void reset_display(Frontend &f)
{
    f.scale = DEFAULT_SCALE;
    f.scaling = NDS_SCALING_PIXEL_PERFECT;
    f.filter = FILTER_NEAREST;
    f.internal_scale = 1;
    f.internal_scale_this_run = 0;
    apply_internal_scale(f);
    f.layout = NDS_LAYOUT_STACKED;
    for (bool &swapped : f.swapped) swapped = false;
    f.gap = 0;
    f.show_overlay = false;
    f.show_fps = false;
    f.speed = 100;
    if (f.fullscreen) set_fullscreen(f, false);
    apply_window_size(f);
    save_prefs(f);
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

/* The first binding of an action, for hints such as "Hold F4 to close the
 * lid": keyboard, else controller; empty when unbound. */
static std::string shortcut_hint(const Frontend &f, int action)
{
    for (int device = 0; device < 2; device++) {
        for (int s = 0; s < 2; s++) {
            const NdsBinding &b = (device == 0 ? f.map.keyboard : f.map.controller)[action][s];
            if (b.kind != NDS_BIND_NONE) return nds_binding_label(b, f.platform, f.pad_profile);
        }
    }
    return std::string();
}

/* The first controller binding of an action, or `otherwise`. */
static std::string pad_hint(const Frontend &f, int action, const char *otherwise)
{
    for (int s = 0; s < 2; s++) {
        const NdsBinding &b = f.map.controller[action][s];
        if (b.kind != NDS_BIND_NONE) return nds_binding_label(b, f.platform, f.pad_profile);
    }
    return otherwise;
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
        ImGui::TextUnformatted(nds_action_label(a));
        for (int s = 0; s < 2; s++) {
            ImGui::TableNextColumn();
            ImGui::PushID(s);
            const NdsBinding b = (keyboard ? f.map.keyboard : f.map.controller)[a][s];
            const bool waiting = f.capturing && f.capture_keyboard == keyboard && f.capture_action == a &&
                                 f.capture_slot == s;
            const std::string label =
                (waiting ? std::string("Press...") : nds_binding_label(b, f.platform, f.pad_profile)) + "###bind";
            float width = ImGui::GetContentRegionAvail().x - clear_width - style.ItemSpacing.x;
            if (ImGui::Button(label.c_str(), ImVec2(width > 40.0f ? width : 40.0f, 0.0f))) start_capture(f, keyboard, a, s);
            if (ImGui::IsItemFocused()) f.focus = BindingFocus{ true, keyboard, false, a, s };
            ImGui::SameLine();
            ImGui::BeginDisabled(b.kind == NDS_BIND_NONE);
            if (ImGui::Button("Clear")) {
                NdsBinding none = { NDS_BIND_NONE, 0, 0 };
                assign_binding(f, keyboard, a, s, none);
            }
            if (ImGui::IsItemFocused()) f.focus = BindingFocus{ true, keyboard, true, a, s };
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/* The two buttons under the settings. */
static void draw_menu_footer(Frontend &f)
{
    ImGui::Separator();
    const float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    if (ImGui::Button("Reset Display", ImVec2(button_width, 0.0f))) reset_display(f);
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(button_width, 0.0f))) request_quit();
}

static void draw_menu(Frontend &f)
{
    ImGuiIO &io = ImGui::GetIO();
    const float ui_scale = io.FontGlobalScale;
    const float footer_height = ImGui::GetFrameHeightWithSpacing() * 3.0f;
    /* A window can be as small as one screen at 1x (256x192). Then only
     * the title and Resume stay put, and the hints and the footer's
     * buttons scroll with the settings, which would otherwise have no room
     * left at all. */
    const bool compact = io.DisplaySize.y < 560.0f * ui_scale;
    f.focus = BindingFocus();

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
    ImGui::TextDisabled("Nintendo DS Settings");
    ImGui::SameLine();
    if (ImGui::Button("Resume")) set_menu(f, false);
    ImGui::Separator();
    /* Flattened so arrow keys and the D-pad move into the settings and
     * scroll them, not just between Resume and the footer buttons. */
    if (compact) ImGui::BeginChild("SettingsScroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    ImGui::TextWrapped("%s", nds_menu_hint(f.platform));
    ImGui::TextWrapped("The game is paused while the menu is open.");
    ImGui::Separator();
    if (!compact) {
        ImGui::BeginChild("SettingsScroll", ImVec2(0.0f, -footer_height), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NavFlattened);
    }

    ImGui::Text("Performance: %.1f FPS", f.fps);
    if (f.m->rc_program) {
        ImGui::Text("Recompiled code: %u ARM9 and %u ARM7 entry points", f.m->rc_program->entry_count[NDS_ARM9],
                    f.m->rc_program->entry_count[NDS_ARM7]);
    } else {
        ImGui::Text("Recompiled code: not in use (interpreter only)");
    }
    ImGui::Text("Interpreted instructions last frame: %llu ARM9, %llu ARM7",
                (unsigned long long)f.interpreted_last_frame[NDS_ARM9], (unsigned long long)f.interpreted_last_frame[NDS_ARM7]);

    ImGui::Separator();
    ImGui::TextDisabled("Graphics");
    ImGui::Text("Window: %d x %d", f.out_w, f.out_h);
    for (int i = 0; i < 2; i++) {
        const SDL_Rect &r = f.screens.rect[i];
        if (r.w > 0) ImGui::Text("%s screen: %d x %d (%.2fx)", i ? "Bottom" : "Top", r.w, r.h, (double)r.w / SCREEN_W);
        else ImGui::Text("%s screen: not shown", i ? "Bottom" : "Top");
    }
    bool fullscreen = f.fullscreen;
    if (ImGui::Checkbox("Fullscreen", &fullscreen)) set_fullscreen(f, fullscreen);
    const char *layout_names[NDS_LAYOUT_COUNT];
    for (int i = 0; i < NDS_LAYOUT_COUNT; i++) layout_names[i] = nds_layout_label(i);
    int layout = f.layout;
    if (ImGui::Combo("Screen Layout", &layout, layout_names, NDS_LAYOUT_COUNT)) set_layout(f, layout);
    if (ImGui::Checkbox("Swap Screens", &f.swapped[f.layout])) {
        update_layout(f);
        save_prefs(f);
    }
    if (ImGui::SliderInt("Screen Gap", &f.gap, 0, NDS_GAP_MAX, "%d DS pixels")) {
        apply_window_size(f);
        save_prefs(f);
    }
    const char *scaling_names[NDS_SCALING_COUNT];
    for (int i = 0; i < NDS_SCALING_COUNT; i++) scaling_names[i] = nds_scaling_label(i);
    if (ImGui::Combo("Scaling Mode", &f.scaling, scaling_names, NDS_SCALING_COUNT)) {
        update_layout(f);
        save_prefs(f);
    }
    if (ImGui::Combo("Scale Filter", &f.filter, k_filter_names, IM_ARRAYSIZE(k_filter_names))) save_prefs(f);
    {
        int index = wanted_internal_scale(f) - 1;
        if (ImGui::Combo("Internal Resolution", &index, k_internal_scale_names, IM_ARRAYSIZE(k_internal_scale_names))) {
            f.internal_scale = index + 1;
            f.internal_scale_this_run = 0;
            apply_internal_scale(f);
            save_prefs(f);
        }
        ImGui::TextDisabled("How large the 3D picture is drawn: sharper edges and finer texture detail,\n"
                            "for more of the processor's time. Backgrounds and sprites stay as they are,\n"
                            "and the game itself still sees the console's own picture.");
    }
    if (!f.fullscreen) {
        /* The sizes that fit this display, for the layout in use. */
        char label[48];
        int w = 0, h = 0;
        const int largest = fitting_scale(f, MAX_SCALE);
        nds_layout_size(f.layout, f.gap, f.scale, &w, &h);
        snprintf(label, sizeof label, "%dx (%dx%d)", f.scale, w, h);
        if (ImGui::BeginCombo("Window Size", label)) {
            for (int k = 1; k <= largest; k++) {
                nds_layout_size(f.layout, f.gap, k, &w, &h);
                snprintf(label, sizeof label, "%dx (%dx%d)", k, w, h);
                if (ImGui::Selectable(label, k == f.scale)) {
                    f.scale = k;
                    apply_window_size(f);
                    save_prefs(f);
                }
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("Window Size is disabled while fullscreen is active.");
    }
    const std::string layout_hint = shortcut_hint(f, NDS_ACTION_NEXT_LAYOUT), swap_hint = shortcut_hint(f, NDS_ACTION_SWAP_SCREENS);
    if (!layout_hint.empty() && !swap_hint.empty()) {
        ImGui::TextDisabled("%s goes to the next layout and %s swaps the screens while playing.", layout_hint.c_str(),
                            swap_hint.c_str());
    }

    ImGui::Separator();
    ImGui::TextDisabled("Runtime");
    if (ImGui::Checkbox("Show FPS Counter", &f.show_fps)) save_prefs(f);
    if (ImGui::Checkbox("Show Overlay", &f.show_overlay)) save_prefs(f);
    ImGui::SliderInt("Speed %", &f.speed, 10, 500);
    if (ImGui::Button("Reset Speed")) f.speed = 100;
    const std::string fast_forward = shortcut_hint(f, NDS_ACTION_FAST_FORWARD);
    if (!fast_forward.empty()) ImGui::TextDisabled("Hold %s to fast forward at full speed.", fast_forward.c_str());
    ImGui::TextWrapped("Save file: %s", f.save.path.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Touch Screen");
    ImGui::TextWrapped("The mouse is the stylus: hold the left button on the bottom screen to touch it, and drag. A "
                       "touch screen works the same way. On a controller, the right stick moves a cursor over the "
                       "bottom screen and %s puts the pen down on it.",
                       pad_hint(f, NDS_ACTION_TOUCH, "a button you bind below").c_str());
    if (ImGui::Combo("Controller Cursor", &f.cursor_mode, k_cursor_mode_names, IM_ARRAYSIZE(k_cursor_mode_names))) save_prefs(f);
    if (ImGui::SliderInt("Cursor Speed (%)", &f.cursor_speed, CURSOR_SPEED_MIN, CURSOR_SPEED_MAX)) save_prefs(f);

    ImGui::Separator();
    ImGui::TextDisabled("Microphone");
    const std::string blow = shortcut_hint(f, NDS_ACTION_BLOW);
    if (!blow.empty()) ImGui::TextWrapped("Hold %s to blow or shout into the microphone.", blow.c_str());
    else ImGui::TextWrapped("No key or button blows into the microphone: bind one below.");
    if (ImGui::Checkbox("Use This Computer's Microphone", &f.host_mic)) {
        if (f.host_mic) open_capture(f);
        else close_capture(f);
        save_prefs(f);
    }
    if (f.host_mic) {
        if (f.capture) ImGui::Text("Microphone level: %d%%", f.mic_peak * 100 / 0x7FF);
        else ImGui::TextWrapped("The microphone could not be opened: %s", f.mic_error.c_str());
        if (ImGui::SliderInt("Microphone Boost", &f.mic_boost, 1, MIC_BOOST_MAX, "%dx")) save_prefs(f);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Lid");
    const std::string lid = shortcut_hint(f, NDS_ACTION_LID);
    if (!lid.empty()) ImGui::TextWrapped("Hold %s to close the lid; it opens when you let go.", lid.c_str());
    else ImGui::TextWrapped("No key or button closes the lid: bind one below, or use the button here.");
    if (ImGui::Button("Close the Lid for Two Seconds")) {
        f.lid_frames = LID_MENU_FRAMES;
        set_menu(f, false);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Console");
    int language = clamp_int(f.m->settings.language, 0, IM_ARRAYSIZE(k_language_names) - 1);
    if (ImGui::Combo("Language", &language, k_language_names, IM_ARRAYSIZE(k_language_names))) set_owner_language(f, language);
    char nickname[NDS_NICKNAME_MAX + 1];
    owner_nickname(f, nickname);
    const auto ascii_only = [](ImGuiInputTextCallbackData *data) -> int {
        return data->EventChar >= 0x20 && data->EventChar < 0x7F ? 0 : 1;
    };
    if (ImGui::InputText("Nickname", nickname, sizeof nickname, ImGuiInputTextFlags_CallbackCharFilter, ascii_only)) {
        set_owner_nickname(f, nickname);
    }
    ImGui::TextDisabled("The console's own settings, which games read as they start: used the next time this one does. "
                        "Changing them clears what games have stored in the console (Wi-Fi set-up).");

    ImGui::Separator();
    ImGui::TextDisabled("Audio");
    if (ImGui::Checkbox("Mute", &f.muted)) save_prefs(f);
    if (f.mute_this_run) ImGui::TextDisabled("This run was started with --mute and is silent whatever is chosen here.");
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
    ImGui::TextDisabled("Controls");
    static const char *const platform_names[] = { "Windows", "Mac", "Steam Deck" };
    ImGui::Text("Default layout: %s", platform_names[f.platform]);
    static const char *const pad_choices[] = { "Controller 1", "Controller 2", "Controller 3", "Controller 4",
                                               "None (keyboard only)" };
    int pad_choice = f.pad_slot < 0 ? 4 : f.pad_slot;
    ImGui::BeginDisabled(f.pad_fixed);
    if (ImGui::Combo("Controller", &pad_choice, pad_choices, IM_ARRAYSIZE(pad_choices))) {
        f.pad_slot = pad_choice == 4 ? -1 : pad_choice;
        choose_pad(f);
        save_prefs(f);
    }
    ImGui::EndDisabled();
    if (f.pad) {
        const char *name = SDL_GameControllerName(f.pad);
        ImGui::Text("Detected Controller: %s", name ? name : "(unnamed)");
    } else {
        ImGui::TextDisabled(f.pad_slot < 0 ? "Keyboard only." : "No controller detected in that position.");
    }
    ImGui::TextWrapped("Choose a binding, then press the new key or button. Escape or Guide cancels. Hold %s with a "
                       "key to bind a combination.",
                       f.platform == NDS_PLATFORM_MAC ? "Command, Control, Option or Shift" : "Ctrl, Alt or Shift");
    if (f.capturing) {
        ImGui::Text("Waiting for a %s for %s...", f.capture_keyboard ? "key" : "controller button",
                    nds_action_label(f.capture_action));
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) finish_capture(f, NULL);
    }
    if (!f.bind_note.empty()) ImGui::TextWrapped("%s", f.bind_note.c_str());
    draw_binding_table(f, "Keyboard Buttons", true, 0, NDS_ACTION_GAMEPLAY_COUNT);
    draw_binding_table(f, "Controller Buttons", false, 0, NDS_ACTION_GAMEPLAY_COUNT);
    draw_binding_table(f, "Keyboard Stylus, Microphone and Lid", true, NDS_ACTION_GAMEPLAY_COUNT, NDS_ACTION_CONSOLE_COUNT);
    draw_binding_table(f, "Controller Stylus, Microphone and Lid", false, NDS_ACTION_GAMEPLAY_COUNT, NDS_ACTION_CONSOLE_COUNT);
    draw_binding_table(f, "Keyboard Shortcuts", true, NDS_ACTION_CONSOLE_COUNT, NDS_ACTION_COUNT);
    draw_binding_table(f, "Controller Shortcuts", false, NDS_ACTION_CONSOLE_COUNT, NDS_ACTION_COUNT);
    ImGui::TextDisabled("Escape and the controller's Guide button always open the menu as well.");
    if (ImGui::Button("Reset Controls")) {
        f.map = f.defaults;
        f.bind_note = "Controls reset to the " + std::string(platform_names[f.platform]) + " defaults.";
        f.input_resync = true;
        save_prefs(f);
    }

    if (compact) draw_menu_footer(f);
    ImGui::EndChild();
    if (!compact) draw_menu_footer(f);
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
        ImGui::TextUnformatted(nds_menu_hint(f.platform));
        const std::string overlay = shortcut_hint(f, NDS_ACTION_OVERLAY);
        if (!overlay.empty()) ImGui::Text("Use %s for Overlay", overlay.c_str());
        ImGui::Text("Layout: %s%s, %s", nds_layout_label(f.layout), f.swapped[f.layout] ? " (swapped)" : "",
                    nds_scaling_label(f.scaling));
        ImGui::Text("Speed: %s", f.fast ? "fast forward" : (std::to_string(f.speed) + "%").c_str());
        if (f.audio) ImGui::Text("Audio queued: %.0f ms", audio_queue_ms(f));
        ImGui::Text("Interpreted: %llu ARM9, %llu ARM7 instructions/frame", (unsigned long long)f.interpreted_last_frame[NDS_ARM9],
                    (unsigned long long)f.interpreted_last_frame[NDS_ARM7]);
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

/* The controller's cursor: a ring on the bottom screen, filled while the
 * pen is down. Shown for a few seconds after the stick or the pen button
 * was last used. */
static void draw_cursor(const Frontend &f)
{
    double x = 0.0, y = 0.0;
    if (SDL_TICKS_PASSED(SDL_GetTicks(), f.cursor_shown_until)) return;
    if (!nds_layout_touch_position(f.screens, f.cursor_x, f.cursor_y, &x, &y)) return;
    /* Dear ImGui draws in window units. */
    const ImGuiIO &io = ImGui::GetIO();
    const ImVec2 at((float)(x / (io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f)),
                    (float)(y / (io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f)));
    const float pixel = (float)f.screens.rect[1].w / SCREEN_W;
    const float radius = pixel * 3.0f < 6.0f ? 6.0f : pixel * 3.0f;
    const bool down = (console_actions(f) & ~f.held_from_menu & (1u << NDS_ACTION_TOUCH)) != 0;
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    if (down) draw->AddCircleFilled(at, radius, IM_COL32(255, 255, 255, 110));
    draw->AddCircle(at, radius + 1.5f, IM_COL32(0, 0, 0, 200), 0, 2.0f);
    draw->AddCircle(at, radius, IM_COL32(255, 255, 255, 255), 0, 2.0f);
    draw->AddCircleFilled(at, 2.0f, IM_COL32(255, 255, 255, 255));
}

/* The window's picture: the screens, then the menu or what is shown over
 * the game. */
static void draw_window(Frontend &f)
{
    SDL_SetRenderDrawColor(f.ren, 0, 0, 0, 255);
    SDL_RenderClear(f.ren);
    draw_game(f, NULL, f.screens);

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.FontGlobalScale = ui_scale_for_size(io.DisplaySize);
    if (f.show_menu) {
        draw_menu(f);
    } else {
        draw_cursor(f);
        if (f.show_overlay) draw_overlay(f);
        if (f.show_fps) draw_fps(f);
        if (!SDL_TICKS_PASSED(SDL_GetTicks(), f.status_until)) draw_status(f);
    }
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
}

static void present(Frontend &f)
{
    draw_window(f);
    SDL_RenderPresent(f.ren);
}

/* --menu-shot: the window as it looks with the settings menu open over the
 * game, read back from the renderer. The menu is drawn a few times first:
 * Dear ImGui sizes a window on the frames after it first appears. */
static bool save_menu_shot(Frontend &f, const char *path)
{
    if (f.out_w <= 0 || f.out_h <= 0) return false;
    const bool was_open = f.show_menu;
    f.show_menu = true;
    for (int i = 0; i < 4; i++) draw_window(f);
    std::vector<uint32_t> pixels((size_t)f.out_w * f.out_h);
    const bool read = SDL_RenderReadPixels(f.ren, NULL, SDL_PIXELFORMAT_ARGB8888, pixels.data(), f.out_w * 4) == 0;
    f.show_menu = was_open;
    SDL_Surface *surface =
        read ? SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), f.out_w, f.out_h, 32, f.out_w * 4, SDL_PIXELFORMAT_RGB888)
             : nullptr;
    const bool ok = surface && SDL_SaveBMP(surface, path) == 0;
    if (surface) SDL_FreeSurface(surface);
    if (ok) printf("menu shot: %s (%dx%d)\n", path, f.out_w, f.out_h);
    else fprintf(stderr, "menu shot: %s\n", SDL_GetError());
    return ok;
}

/* ------------------------------------------------------------------ */
/* Menu self-test                                                      */
/* ------------------------------------------------------------------ */

/*
 * --menu-selftest drives the frontend with made-up input, delivered the way
 * real input is: key, mouse and finger events through SDL's event queue,
 * and a virtual controller (SDL_JoystickAttachVirtual) whose buttons and
 * sticks SDL reports like any other's. It runs on SDL's dummy video and
 * audio drivers, so it can be a CTest. In order, it checks that:
 *
 * - Escape, F10, the controller's Guide button and L3 each open and close
 *   the menu, and the game stops while it is open;
 * - what is held while the menu is open, and still held as it closes, does
 *   not reach the game until it is let go;
 * - the menu can be walked with the arrow keys and with the D-pad to a
 *   binding, which Enter (or the south button) then waits to change; the
 *   lid is rebound from the controller alone, the next-layout shortcut and
 *   the stylus button from the keyboard, each change is in the settings
 *   file at once, a binding taken from another action leaves it, and the
 *   new bindings do what the old ones did;
 * - in every layout, a click at a known place in the window touches the
 *   bottom-screen pixel under it, a pen dragged off the screen stays down
 *   at the edge, and a screen that is not shown cannot be touched; a finger
 *   does the same; the controller's stick moves the cursor and its trigger
 *   puts the pen down there;
 * - the blow key makes noise on the microphone and the lid key closes the
 *   lid, each only while held;
 * - a change to the save is written to the save file once the game has
 *   left it alone for a second, not before;
 * - the console's language and nickname, changed as the menu changes them,
 *   are written to the firmware file;
 * - --mute does not reach the saved settings;
 * - at the end the bindings are put back as they were found, in memory and
 *   in the file.
 *
 * It presses whatever is bound, so it runs on any platform's defaults, and
 * begins by putting the display and cursor settings back to their defaults.
 * It leaves the volume at 37, the side-by-side layout chosen, the console's
 * language French and its nickname "Selftest", and a marked save behind; a
 * second run with --menu-selftest-verify checks that all of them survived
 * the restart. Pass --prefs FILE and --save-dir DIR so the player's own
 * settings and save are not touched. The save steps need a card whose save
 * chip is known and whose save is blank (tests/nds/menu_selftest.py makes
 * one); a save with anything in it is never written to.
 *
 * The checks read the machine's own record of its inputs (NdsMachine.
 * keyinput, extkeyin, touch_down, touch_x, touch_y, lid_closed and
 * spi.microphone), and one step writes its save memory directly, standing
 * in for a game that saves.
 */
enum { SELFTEST_VOLUME = 37, SELFTEST_LAYOUT = NDS_LAYOUT_SIDE_BY_SIDE };
static const int SELFTEST_SETTLE = 4; /* loops an input is given to take effect */
static const char k_selftest_save_mark[] = "Native Console menu self-test";
enum { SELFTEST_SAVE_OFFSET = 0x40, SELFTEST_LANGUAGE = NDS_LANGUAGE_FRENCH };
static const char k_selftest_nickname[] = "Selftest";

struct MenuSelfTest {
    bool enabled = false;
    size_t step = 0; /* index into k_selftest_steps */
    int calls = 0;   /* times the current step has run */
    int failures = 0;
    uint64_t frame_mark = 0;
    Uint32 ticks_mark = 0;
    SDL_Joystick *joy = nullptr; /* the virtual controller */
    int joy_index = -1;
    NdsInputMap found_map = {};   /* the bindings before the test changed any */
    std::string found_bindings;   /* and their lines in the settings file */
    int part = 0;                 /* progress inside a step that has several parts */
    int nav_key = 0;              /* the key or button a navigation step is holding */
    int slot = 0;                 /* the binding slot the last navigation ended on */
    bool swapped_mark = false;
    double cursor_mark = 0.0;
    int mic_distinct = 0, mic_last = -1;
    uint64_t save_writes_mark = 0;
};

/* What the test binds: keys and a button that the defaults leave free or,
 * for R3, give to another action, so that taking a binding away is tested. */
static const SDL_Scancode k_selftest_layout_key = SDL_SCANCODE_F9, k_selftest_touch_key = SDL_SCANCODE_T;
enum { SELFTEST_LID_BUTTON = SDL_CONTROLLER_BUTTON_RIGHTSTICK };

static std::string read_text(const std::string &path)
{
    size_t size = 0;
    uint8_t *data = read_file(path.c_str(), &size);
    std::string text(data ? (const char *)data : "", data ? size : 0);
    free(data);
    return text;
}

/* The binding lines of a settings file, sorted, so that two files can be
 * compared whatever order their lines are in. */
static std::string binding_lines(const std::string &text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.compare(0, 9, "keyboard.") || !line.compare(0, 11, "controller.")) lines.push_back(line);
        start = end + 1;
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const std::string &line : lines) out += line + "\n";
    return out;
}

/* Whether a text file has this line (Windows ends lines with CR LF). */
static bool file_has_line(const std::string &path, const std::string &line)
{
    std::string text = "\n" + read_text(path);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text.find("\n" + line + "\n") != std::string::npos;
}

static void push_key(SDL_Scancode sc, bool down)
{
    SDL_Event e = {};
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    SDL_PushEvent(&e);
}

static void tap_key(SDL_Scancode sc)
{
    push_key(sc, true);
    push_key(sc, false);
}

static void push_mouse(Uint32 type, int x, int y)
{
    SDL_Event e = {};
    e.type = type;
    if (type == SDL_MOUSEMOTION) {
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.button.button = SDL_BUTTON_LEFT;
        e.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
        e.button.clicks = 1;
        e.button.x = x;
        e.button.y = y;
    }
    SDL_PushEvent(&e);
}

static void push_finger(Uint32 type, float x, float y)
{
    SDL_Event e = {};
    e.type = type;
    e.tfinger.fingerId = 7;
    e.tfinger.x = x;
    e.tfinger.y = y;
    e.tfinger.pressure = type == SDL_FINGERUP ? 0.0f : 1.0f;
    SDL_PushEvent(&e);
}

static void pad_button(MenuSelfTest &t, int button, bool down)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    SDL_JoystickSetVirtualButton(t.joy, button, down ? SDL_PRESSED : SDL_RELEASED);
#else
    (void)t, (void)button, (void)down;
#endif
}

/* A stick from -1 to 1; a trigger from 0 (let go) to 1. */
static void pad_axis(MenuSelfTest &t, int axis, double value)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const bool trigger = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
    const double raw = trigger ? value * 65535.0 - 32768.0 : value * 32767.0;
    SDL_JoystickSetVirtualAxis(t.joy, axis, (Sint16)clamp_int((int)raw, -32768, 32767));
#else
    (void)t, (void)axis, (void)value;
#endif
}

/* The self-test's controller: a virtual one, read in place of any real one
 * so that nothing plugged in can disturb the run. */
static bool selftest_attach_pad(Frontend &f, MenuSelfTest &t)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    t.joy_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                            SDL_CONTROLLER_BUTTON_MAX, 0);
    SDL_GameController *pad = t.joy_index >= 0 ? SDL_GameControllerOpen(t.joy_index) : NULL;
    if (!pad) return false;
    t.joy = SDL_GameControllerGetJoystick(pad);
    set_pad(f, pad);
    f.pad_fixed = true;
    for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++) pad_axis(t, axis, 0.0);
    return true;
#else
    (void)f, (void)t;
    return false;
#endif
}

/* The first key bound to an action, as the keyboard would send it: its
 * modifiers before it on the way down and after it on the way up. False if
 * the action has no key. */
static bool key_action(const Frontend &f, int action, bool down)
{
    static const struct {
        uint8_t mod;
        SDL_Scancode sc;
    } mods[] = { { NDS_MOD_CTRL, SDL_SCANCODE_LCTRL }, { NDS_MOD_SHIFT, SDL_SCANCODE_LSHIFT },
                 { NDS_MOD_ALT, SDL_SCANCODE_LALT },   { NDS_MOD_GUI, SDL_SCANCODE_LGUI } };
    for (int s = 0; s < 2; s++) {
        const NdsBinding &b = f.map.keyboard[action][s];
        if (b.kind != NDS_BIND_KEY) continue;
        for (const auto &mod : mods) {
            if (down && (b.mods & mod.mod)) push_key(mod.sc, true);
        }
        push_key((SDL_Scancode)b.code, down);
        for (const auto &mod : mods) {
            if (!down && (b.mods & mod.mod)) push_key(mod.sc, false);
        }
        return true;
    }
    return false;
}

static bool tap_action_key(const Frontend &f, int action)
{
    return key_action(f, action, true) && key_action(f, action, false);
}

/* The same for the first controller binding: a button, or a stick or
 * trigger pushed all the way. */
static bool pad_action(const Frontend &f, MenuSelfTest &t, int action, bool down)
{
    for (int s = 0; s < 2; s++) {
        const NdsBinding &b = f.map.controller[action][s];
        if (b.kind == NDS_BIND_BUTTON) pad_button(t, b.code, down);
        else if (b.kind == NDS_BIND_AXIS_POSITIVE) pad_axis(t, b.code, down ? 1.0 : 0.0);
        else if (b.kind == NDS_BIND_AXIS_NEGATIVE) pad_axis(t, b.code, down ? -1.0 : 0.0);
        else continue;
        return true;
    }
    return false;
}

/* "F10", "L3": what the action's first key or controller binding is called. */
static std::string bound_name(const Frontend &f, bool keyboard, int action)
{
    for (int s = 0; s < 2; s++) {
        const NdsBinding &b = (keyboard ? f.map.keyboard : f.map.controller)[action][s];
        if (b.kind != NDS_BIND_NONE) return nds_binding_label(b, f.platform, f.pad_profile);
    }
    return std::string("(nothing bound to ") + nds_action_label(action) + ")";
}

static void selftest_check(MenuSelfTest &t, bool ok, const std::string &what)
{
    printf("menu selftest: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) t.failures++;
}

/* Nothing pressed, no pen, lid open: the machine's own view. */
static bool machine_sees_nothing(const NdsMachine *m)
{
    return m->keyinput == 0x03FF && (m->extkeyin & 0x0003) == 0x0003 && !m->touch_down && !m->lid_closed;
}

static bool binding_focused(const Frontend &f, bool keyboard, int action)
{
    return f.focus.on && !f.focus.clear && f.focus.keyboard == keyboard && f.focus.action == action;
}

/* Where a binding's row comes in the menu's six tables, top to bottom. */
static int binding_row(bool keyboard, int action)
{
    const int group = action < NDS_ACTION_GAMEPLAY_COUNT ? 0 : action < NDS_ACTION_CONSOLE_COUNT ? 1 : 2;
    return (group * 2 + (keyboard ? 0 : 1)) * NDS_ACTION_COUNT + action;
}

/* One step of walking the menu's focus to a binding's button, with the
 * arrow keys or with the controller's D-pad: a press on one call and the
 * release on the next, steering by where the focus is now. 1 when the
 * focus is on the binding, -1 if it never got there, 0 while on the way. */
static int selftest_walk(Frontend &f, MenuSelfTest &t, bool keyboard_binding, int action, bool with_pad)
{
    if (t.calls % 2 == 1) {
        if (with_pad) pad_button(t, t.nav_key, false);
        else push_key((SDL_Scancode)t.nav_key, false);
        return 0;
    }
    if (binding_focused(f, keyboard_binding, action)) {
        t.slot = f.focus.slot;
        return 1;
    }
    if (t.calls > 1200) return -1;
    /* Down until the focus is in the tables; then towards the row, and
     * left off a Clear button on to the binding beside it. */
    enum { UP, DOWN, LEFT } way = DOWN;
    if (f.focus.on) {
        const int here = binding_row(f.focus.keyboard, f.focus.action), there = binding_row(keyboard_binding, action);
        way = here < there ? DOWN : here > there ? UP : LEFT;
    }
    static const int pad_buttons[] = { SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                       SDL_CONTROLLER_BUTTON_DPAD_LEFT };
    static const int keys[] = { SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT };
    t.nav_key = with_pad ? pad_buttons[way] : keys[way];
    if (with_pad) pad_button(t, t.nav_key, true);
    else push_key((SDL_Scancode)t.nav_key, true);
    return 0;
}

/* Each step is called once per loop, before events are polled, until it
 * returns true; t.calls counts the calls. */
typedef bool (*SelfTestStep)(Frontend &f, MenuSelfTest &t);

static bool st_start(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) {
        t.found_map = f.map;
        t.found_bindings = binding_lines(read_text(f.prefs_path));
        selftest_check(t, t.joy != nullptr, "a virtual controller is attached and read");
        /* The settings the steps below count on. */
        f.layout = NDS_LAYOUT_STACKED;
        for (bool &swapped : f.swapped) swapped = false;
        f.gap = 0;
        f.scaling = NDS_SCALING_PIXEL_PERFECT;
        f.cursor_mode = CURSOR_MOVE;
        f.cursor_speed = 100;
        apply_window_size(f);
    }
    if (f.m->frame_count < 20 && t.calls < 2000) return false;
    selftest_check(t, f.m->frame_count >= 20, "the game runs");
    return true;
}

static bool st_escape_opens(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) tap_key(SDL_SCANCODE_ESCAPE);
    if (t.calls < SELFTEST_SETTLE) return false;
    selftest_check(t, f.show_menu, "Escape opens the menu");
    t.frame_mark = f.m->frame_count;
    return true;
}

/* With the menu open: hold a button's key, the lid's key and the
 * controller's pen button, and leave them held. */
static bool st_menu_pauses(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) {
        const bool pressed = key_action(f, NDS_ACTION_A, true) && key_action(f, NDS_ACTION_LID, true) &&
                             pad_action(f, t, NDS_ACTION_TOUCH, true);
        selftest_check(t, pressed, "A and the lid have keys and the pen has a controller button");
    }
    if (t.calls < 20) return false;
    selftest_check(t, f.m->frame_count == t.frame_mark, "the game is paused while the menu is open");
    selftest_check(t, machine_sees_nothing(f.m), "a button, the lid and the pen held in the menu do not reach the game");
    return true;
}

static bool st_escape_closes(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) tap_key(SDL_SCANCODE_ESCAPE);
    if (t.calls < 12) return false;
    selftest_check(t, !f.show_menu, "Escape closes the menu");
    selftest_check(t, f.m->frame_count > t.frame_mark, "the game runs again");
    selftest_check(t, machine_sees_nothing(f.m), "what was held as the menu closed still does not reach the game");
    key_action(f, NDS_ACTION_A, false);
    key_action(f, NDS_ACTION_LID, false);
    pad_action(f, t, NDS_ACTION_TOUCH, false);
    return true;
}

static bool st_keys_reach_game(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) return false; /* one loop with everything let go */
    if (t.calls == 1) {
        key_action(f, NDS_ACTION_A, true);
        key_action(f, NDS_ACTION_Y, true);
    }
    if (t.calls < 1 + SELFTEST_SETTLE) return false;
    /* A is KEYINPUT bit 0 and Y is EXTKEYIN bit 1, both low when pressed. */
    selftest_check(t, f.m->keyinput == 0x03FE && (f.m->extkeyin & 0x0003) == 0x0001,
                   "let go and pressed again, " + bound_name(f, true, NDS_ACTION_A) + " and " +
                       bound_name(f, true, NDS_ACTION_Y) + " press A and Y");
    key_action(f, NDS_ACTION_A, false);
    key_action(f, NDS_ACTION_Y, false);
    return true;
}

static bool st_menu_key_opens(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) tap_action_key(f, NDS_ACTION_MENU);
    if (t.calls < SELFTEST_SETTLE) return false;
    selftest_check(t, f.show_menu, bound_name(f, true, NDS_ACTION_MENU) + " opens the menu");
    return true;
}

static bool st_menu_key_closes(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) tap_action_key(f, NDS_ACTION_MENU);
    if (t.calls < SELFTEST_SETTLE) return false;
    selftest_check(t, !f.show_menu, bound_name(f, true, NDS_ACTION_MENU) + " closes the menu");
    return true;
}

/* SDL holds a Guide button's release back until it has been down for a
 * quarter of a second, and drops a press that comes sooner: the test, which
 * does not otherwise wait for the clock, waits that out. */
static bool guide_tap(MenuSelfTest &t)
{
    if (t.calls == 0) {
        pad_button(t, SDL_CONTROLLER_BUTTON_GUIDE, true);
        t.ticks_mark = SDL_GetTicks();
    }
    if (t.calls == 1) pad_button(t, SDL_CONTROLLER_BUTTON_GUIDE, false);
    return t.calls >= 1 + SELFTEST_SETTLE && SDL_GetTicks() - t.ticks_mark >= 300;
}

static bool st_guide_opens(Frontend &f, MenuSelfTest &t)
{
    if (!guide_tap(t)) return false;
    selftest_check(t, f.show_menu, "a controller's Guide button opens the menu");
    t.frame_mark = f.m->frame_count;
    return true;
}

static bool st_guide_closes(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) selftest_check(t, f.m->frame_count == t.frame_mark, "the game is paused (opened from the controller)");
    if (!guide_tap(t)) return false;
    selftest_check(t, !f.show_menu, "Guide closes the menu");
    return true;
}

/* The controller's own menu button: L3 by default, the way in on a Steam
 * Deck, where Steam keeps Guide. */
static bool st_pad_menu_opens(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) pad_action(f, t, NDS_ACTION_MENU, true);
    if (t.calls == 1) pad_action(f, t, NDS_ACTION_MENU, false);
    if (t.calls < 1 + SELFTEST_SETTLE) return false;
    selftest_check(t, f.show_menu, bound_name(f, false, NDS_ACTION_MENU) + " opens the menu (Steam Deck)");
    return true;
}

/* From the controller alone: D-pad to the lid's controller binding, the
 * south button to choose it, then R3, which until now swapped the screens. */
static bool st_pad_rebinds_lid(Frontend &f, MenuSelfTest &t)
{
    if (t.part == 0) {
        const int walked = selftest_walk(f, t, false, NDS_ACTION_LID, true);
        if (walked == 0) return false;
        selftest_check(t, walked > 0, "the D-pad walks the menu to the lid's controller binding");
        if (walked < 0) return true;
        t.part = 1;
        t.calls = -1; /* count the next part's calls from 0 */
        return false;
    }
    if (t.calls == 0) pad_button(t, SDL_CONTROLLER_BUTTON_A, true);
    if (t.calls == 1) pad_button(t, SDL_CONTROLLER_BUTTON_A, false);
    if (t.calls == 5) {
        selftest_check(t, f.capturing && !f.capture_keyboard && f.capture_action == NDS_ACTION_LID,
                       "the south button chooses the binding to change");
        pad_button(t, SELFTEST_LID_BUTTON, true);
    }
    if (t.calls == 6) pad_button(t, SELFTEST_LID_BUTTON, false);
    if (t.calls < 10) return false;
    const NdsBinding want = { NDS_BIND_BUTTON, 0, SELFTEST_LID_BUTTON };
    selftest_check(t, !f.capturing && f.map.controller[NDS_ACTION_LID][t.slot] == want, "R3 is now the lid's button");
    int others = 0;
    for (int a = 0; a < NDS_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            if ((a != NDS_ACTION_LID || s != t.slot) && f.map.controller[a][s] == want) others++;
        }
    }
    selftest_check(t, others == 0 && !f.bind_note.empty(), "and was taken from the action that had it: " + f.bind_note);
    selftest_check(t, file_has_line(f.prefs_path, "controller.lid." + std::to_string(t.slot) + " = button:" +
                                                      std::to_string((int)SELFTEST_LID_BUTTON)),
                   "the new binding is in the settings file");
    return true;
}

static bool st_pad_menu_closes(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) pad_action(f, t, NDS_ACTION_MENU, true);
    if (t.calls == 1) pad_action(f, t, NDS_ACTION_MENU, false);
    if (t.calls < 1 + SELFTEST_SETTLE) return false;
    selftest_check(t, !f.show_menu, bound_name(f, false, NDS_ACTION_MENU) + " closes the menu");
    return true;
}

static bool st_pad_lid(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) {
        t.swapped_mark = f.swapped[f.layout];
        pad_button(t, SELFTEST_LID_BUTTON, true);
    }
    if (t.calls == SELFTEST_SETTLE) {
        selftest_check(t, f.m->lid_closed && (f.m->extkeyin & 0x0080), "the rebound button closes the lid while it is held");
        selftest_check(t, f.swapped[f.layout] == t.swapped_mark, "and no longer swaps the screens");
        pad_button(t, SELFTEST_LID_BUTTON, false);
    }
    if (t.calls < 2 * SELFTEST_SETTLE) return false;
    selftest_check(t, !f.m->lid_closed, "the lid opens when it is let go");
    return true;
}

/* From the keyboard: Escape, arrows to a binding, Enter, the new key. */
static bool selftest_key_rebind(Frontend &f, MenuSelfTest &t, int action, SDL_Scancode key, const char *config)
{
    if (t.part == 0) {
        const int walked = selftest_walk(f, t, true, action, false);
        if (walked == 0) return false;
        selftest_check(t, walked > 0, std::string("the arrow keys walk the menu to ") + nds_action_label(action));
        if (walked < 0) return true;
        t.part = 1;
        t.calls = -1;
        return false;
    }
    if (t.calls == 0) tap_key(SDL_SCANCODE_RETURN);
    if (t.calls == 5) {
        selftest_check(t, f.capturing && f.capture_keyboard && f.capture_action == action, "Enter chooses the binding to change");
        tap_key(key);
    }
    if (t.calls < 9) return false;
    const NdsBinding want = { NDS_BIND_KEY, 0, (int16_t)key };
    selftest_check(t, !f.capturing && f.map.keyboard[action][t.slot] == want,
                   std::string(SDL_GetScancodeName(key)) + " is now bound to " + nds_action_label(action));
    selftest_check(t, file_has_line(f.prefs_path, std::string("keyboard.") + config + "." + std::to_string(t.slot) +
                                                      " = key:" + std::to_string((int)key)),
                   "the new binding is in the settings file");
    return true;
}

static bool st_key_rebinds_layout(Frontend &f, MenuSelfTest &t)
{
    return selftest_key_rebind(f, t, NDS_ACTION_NEXT_LAYOUT, k_selftest_layout_key, "next_layout");
}

/* The stylus button is in a table above the shortcuts: the walk goes up. */
static bool st_key_rebinds_touch(Frontend &f, MenuSelfTest &t)
{
    return selftest_key_rebind(f, t, NDS_ACTION_TOUCH, k_selftest_touch_key, "touch");
}

/* The middle of a bottom-screen pixel as a window position, straight from
 * the rectangle (the mapping itself is tested in test_nds_input.cpp). */
static void window_point(const Frontend &f, int x, int y, int *wx, int *wy)
{
    const SDL_Rect &r = f.screens.rect[1];
    *wx = (int)((r.x + (x + 0.5) * r.w / SCREEN_W) * f.win_w / f.out_w);
    *wy = (int)((r.y + (y + 0.5) * r.h / SCREEN_H) * f.win_h / f.out_h);
}

/* In each layout in turn (the rebound key steps through them): click on
 * the bottom screen, drag off its top left and bottom right, let go. */
static bool st_mouse_in_each_layout(Frontend &f, MenuSelfTest &t)
{
    const NdsMachine *m = f.m;
    const SDL_Rect &r = f.screens.rect[1];
    const std::string where = std::string(" (") + nds_layout_label(f.layout) + ")";
    int wx = 0, wy = 0;
    switch (t.calls) {
    case 0:
        window_point(f, 37, 150, &wx, &wy);
        push_mouse(SDL_MOUSEMOTION, wx, wy);
        push_mouse(SDL_MOUSEBUTTONDOWN, wx, wy);
        return false;
    case SELFTEST_SETTLE:
        window_point(f, 37, 150, &wx, &wy);
        selftest_check(t, m->touch_down && m->touch_x == 37 && m->touch_y == 150,
                       "a click at window " + std::to_string(wx) + "," + std::to_string(wy) + " touches pixel 37,150" + where);
        selftest_check(t, !(m->extkeyin & 0x0040), "the pen-down line is low" + where);
        push_mouse(SDL_MOUSEMOTION, r.x * f.win_w / f.out_w - 60, r.y * f.win_h / f.out_h - 60);
        return false;
    case 2 * SELFTEST_SETTLE:
        selftest_check(t, m->touch_down && m->touch_x == 0 && m->touch_y == 0,
                       "dragged off the top left, the pen stays down at 0,0" + where);
        push_mouse(SDL_MOUSEMOTION, (r.x + r.w) * f.win_w / f.out_w + 60, (r.y + r.h) * f.win_h / f.out_h + 60);
        return false;
    case 3 * SELFTEST_SETTLE:
        selftest_check(t, m->touch_down && m->touch_x == 255 && m->touch_y == 191,
                       "dragged off the bottom right, it stays down at 255,191" + where);
        push_mouse(SDL_MOUSEBUTTONUP, (r.x + r.w) * f.win_w / f.out_w + 60, (r.y + r.h) * f.win_h / f.out_h + 60);
        return false;
    case 4 * SELFTEST_SETTLE:
        selftest_check(t, !m->touch_down && (m->extkeyin & 0x0040), "the pen lifts when the button is let go" + where);
        if (++t.part == NDS_LAYOUT_COUNT) return true;
        tap_key(k_selftest_layout_key); /* the rebound next-layout key */
        return false;
    case 5 * SELFTEST_SETTLE: {
        int w = 0, h = 0;
        nds_layout_size(f.layout, f.gap, fitting_scale(f, f.scale), &w, &h);
        selftest_check(t, f.layout == t.part && f.out_w == w && f.out_h == h,
                       "the rebound key goes to the next layout and the window takes its size, " + std::to_string(f.out_w) +
                           "x" + std::to_string(f.out_h) + where);
        t.calls = -1;
        return false;
    }
    default: return false;
    }
}

/* The single-screen layout, swapped, shows only the top screen. */
static bool st_hidden_screen(Frontend &f, MenuSelfTest &t)
{
    const std::string key = bound_name(f, true, NDS_ACTION_SWAP_SCREENS);
    if (t.calls == 0) {
        selftest_check(t, f.layout == NDS_LAYOUT_SINGLE && f.screens.rect[1].w > 0, "the single layout shows the touch screen");
        tap_action_key(f, NDS_ACTION_SWAP_SCREENS);
    }
    if (t.calls == SELFTEST_SETTLE) {
        selftest_check(t, f.swapped[NDS_LAYOUT_SINGLE] && f.screens.rect[1].w == 0 && f.screens.rect[0].w > 0,
                       key + " swaps the screens: the single layout now shows the top one");
        push_mouse(SDL_MOUSEMOTION, f.win_w / 2, f.win_h / 2);
        push_mouse(SDL_MOUSEBUTTONDOWN, f.win_w / 2, f.win_h / 2);
    }
    if (t.calls == 2 * SELFTEST_SETTLE) {
        selftest_check(t, !f.m->touch_down, "a click cannot touch a screen that is not shown");
        push_mouse(SDL_MOUSEBUTTONUP, f.win_w / 2, f.win_h / 2);
        tap_action_key(f, NDS_ACTION_SWAP_SCREENS);
    }
    if (t.calls < 3 * SELFTEST_SETTLE) return false;
    selftest_check(t, !f.swapped[NDS_LAYOUT_SINGLE] && f.screens.rect[1].w > 0, key + " swaps them back");
    return true;
}

static bool st_finger(Frontend &f, MenuSelfTest &t)
{
    const SDL_Rect &r = f.screens.rect[1];
    const float x = (float)((r.x + 200.5 * r.w / SCREEN_W) / f.out_w), y = (float)((r.y + 20.5 * r.h / SCREEN_H) / f.out_h);
    if (t.calls == 0) push_finger(SDL_FINGERDOWN, x, y);
    if (t.calls == SELFTEST_SETTLE) {
        selftest_check(t, f.m->touch_down && f.m->touch_x == 200 && f.m->touch_y == 20, "a finger on the screen touches pixel 200,20");
        push_finger(SDL_FINGERMOTION, x, 2.0f);
    }
    if (t.calls == 2 * SELFTEST_SETTLE) {
        selftest_check(t, f.m->touch_down && f.m->touch_x == 200 && f.m->touch_y == 191,
                       "dragged off the bottom, the finger stays down at the edge");
        push_finger(SDL_FINGERUP, x, 2.0f);
    }
    if (t.calls < 3 * SELFTEST_SETTLE) return false;
    selftest_check(t, !f.m->touch_down, "the pen lifts with the finger");
    return true;
}

/* The controller's cursor: the stick moves it, and the controller's pen
 * button and the key bound a moment ago put the pen down on it. */
static bool st_cursor(Frontend &f, MenuSelfTest &t)
{
    const NdsMachine *m = f.m;
    const int PUSHED = 12; /* frames the stick is held over */
    if (t.calls == 0) {
        t.cursor_mark = f.cursor_x;
        pad_action(f, t, NDS_ACTION_CURSOR_RIGHT, true);
    }
    if (t.calls == PUSHED) {
        pad_action(f, t, NDS_ACTION_CURSOR_RIGHT, false);
        /* 320 pixels a second at full tilt: 64 in twelve frames. */
        selftest_check(t, f.cursor_x > t.cursor_mark + 50.0 && f.cursor_x < t.cursor_mark + 75.0 &&
                              fabs(f.cursor_y - SCREEN_H / 2.0) < 1.0,
                       bound_name(f, false, NDS_ACTION_CURSOR_RIGHT) + " moves the stylus cursor (" +
                           std::to_string((int)t.cursor_mark) + " to " + std::to_string((int)f.cursor_x) + " in 12 frames)");
        selftest_check(t, !m->touch_down, "moving the cursor does not touch");
        pad_action(f, t, NDS_ACTION_TOUCH, true);
    }
    if (t.calls == PUSHED + SELFTEST_SETTLE) {
        selftest_check(t, m->touch_down && m->touch_x == (int)f.cursor_x && m->touch_y == (int)f.cursor_y,
                       bound_name(f, false, NDS_ACTION_TOUCH) + " puts the pen down at the cursor (" +
                           std::to_string(m->touch_x) + "," + std::to_string(m->touch_y) + ")");
        pad_action(f, t, NDS_ACTION_TOUCH, false);
    }
    if (t.calls == PUSHED + 2 * SELFTEST_SETTLE) {
        selftest_check(t, !m->touch_down, "and lifts it");
        pad_action(f, t, NDS_ACTION_CURSOR_UP, true);
        push_key(k_selftest_touch_key, true);
    }
    if (t.calls == 2 * PUSHED + 2 * SELFTEST_SETTLE) {
        selftest_check(t, m->touch_down && m->touch_y < SCREEN_H / 2 - 40 && m->touch_y == (int)f.cursor_y,
                       "the key bound to the pen holds it down while the stick drags it up (to row " +
                           std::to_string(m->touch_y) + ")");
        pad_action(f, t, NDS_ACTION_CURSOR_UP, false);
        push_key(k_selftest_touch_key, false);
    }
    if (t.calls < 2 * PUSHED + 3 * SELFTEST_SETTLE) return false;
    selftest_check(t, !m->touch_down, "and lets it up");
    return true;
}

static bool st_blow(Frontend &f, MenuSelfTest &t)
{
    const int level = f.m->spi.microphone;
    if (t.calls == 0) {
        selftest_check(t, level == MIC_SILENCE, "the microphone is silent");
        key_action(f, NDS_ACTION_BLOW, true);
    }
    if (t.calls > 2 && t.calls <= 22) {
        if (level != t.mic_last) t.mic_distinct++;
        t.mic_last = level;
    }
    if (t.calls == 22) {
        selftest_check(t, f.blowing && t.mic_distinct >= 15,
                       bound_name(f, true, NDS_ACTION_BLOW) + " feeds the microphone noise (" +
                           std::to_string(t.mic_distinct) + " changes of level in 20 frames)");
        key_action(f, NDS_ACTION_BLOW, false);
    }
    if (t.calls < 22 + SELFTEST_SETTLE) return false;
    selftest_check(t, !f.blowing && level == MIC_SILENCE, "and it is silent again when the key is let go");
    return true;
}

static bool st_lid_key(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) key_action(f, NDS_ACTION_LID, true);
    if (t.calls == SELFTEST_SETTLE) {
        selftest_check(t, f.m->lid_closed, bound_name(f, true, NDS_ACTION_LID) + " closes the lid while it is held");
        key_action(f, NDS_ACTION_LID, false);
    }
    if (t.calls < 2 * SELFTEST_SETTLE) return false;
    selftest_check(t, !f.m->lid_closed, "and it opens when the key is let go");
    return true;
}

/* A game saving: its save memory changes over a few frames. The file is
 * written once that has stopped for a second, with what was saved. Only a
 * blank save is written to (one the driver made): a card that holds a
 * player's save is left alone. */
static bool st_save(Frontend &f, MenuSelfTest &t)
{
    NdsMachine *m = f.m;
    uint32_t size = 0;
    const uint8_t *save = nds_save_memory(m, &size);
    if (t.calls == 0) {
        bool blank = save && size >= SELFTEST_SAVE_OFFSET + sizeof k_selftest_save_mark;
        for (uint32_t i = 0; blank && i < size; i++) blank = save[i] == 0xFF;
        if (!blank) {
            printf("menu selftest: skip the save file (this card has no blank save of a known size to write to)\n");
            return true;
        }
        t.save_writes_mark = f.save.writes;
    }
    /* The mark, a few bytes every third frame for half a second: a game
     * writes its save a page at a time, with pauses between the pages. */
    enum { PAGES = 10, PAGE_FRAMES = 3, SAVING = PAGES * PAGE_FRAMES };
    if (t.calls < SAVING) {
        if (t.calls % PAGE_FRAMES == 0) {
            const size_t part = sizeof k_selftest_save_mark / PAGES + 1, from = (size_t)(t.calls / PAGE_FRAMES) * part;
            for (size_t i = from; i < from + part && i < sizeof k_selftest_save_mark; i++) {
                m->save_data[SELFTEST_SAVE_OFFSET + i] = (uint8_t)k_selftest_save_mark[i];
            }
            m->save_dirty = true;
        }
        return false;
    }
    if (t.calls == SAVING + 30) {
        selftest_check(t, f.save.writes == t.save_writes_mark,
                       "the save file is not written while the game is saving, nor in the half second after");
    }
    if (t.calls < SAVING + 90) return false;
    const std::string file = read_text(f.save.path);
    selftest_check(t, f.save.writes == t.save_writes_mark + 1, "it is written once, about a second after the game stops");
    selftest_check(t, file.size() == size &&
                          !memcmp(file.data() + SELFTEST_SAVE_OFFSET, k_selftest_save_mark, sizeof k_selftest_save_mark),
                   "and holds what the game saved (" + std::to_string(file.size()) + " bytes)");
    return true;
}

/* The owner's settings, changed as the menu's Console section changes
 * them, reach the firmware file at once. */
static bool st_owner(Frontend &f, MenuSelfTest &t)
{
    set_owner_language(f, SELFTEST_LANGUAGE);
    set_owner_nickname(f, k_selftest_nickname);
    selftest_check(t, read_text(f.firmware.path).size() == NDS_FIRMWARE_SIZE,
                   "the language and nickname set in the menu are written to the firmware file");
    return true;
}

/* The settings the second run looks for, set as the menu sets them; then
 * the bindings back as they were found. */
static bool st_finish(Frontend &f, MenuSelfTest &t)
{
    if (t.calls == 0) {
        f.volume = SELFTEST_VOLUME; /* as the menu's volume slider sets it */
        save_prefs(f);
        set_layout(f, SELFTEST_LAYOUT); /* as its layout list does */
        set_fullscreen(f, true);        /* as its Fullscreen box does */
        return false;
    }
    if (t.calls < SELFTEST_SETTLE) return false;
    selftest_check(t, file_has_line(f.prefs_path, "audio.master_volume = " + std::to_string((int)SELFTEST_VOLUME)) &&
                          file_has_line(f.prefs_path, std::string("display.layout = ") + nds_layout_config(SELFTEST_LAYOUT)),
                   "the volume and layout set in the menu are in the settings file");
    if (f.mute_this_run) {
        selftest_check(t, !f.muted && file_has_line(f.prefs_path, "audio.muted = 0"), "--mute did not reach the saved settings");
    }
    selftest_check(t, binding_lines(read_text(f.prefs_path)) != t.found_bindings, "the file holds the test's bindings");
    f.map = t.found_map;
    f.input_resync = true;
    save_prefs(f);
    selftest_check(t, binding_lines(read_text(f.prefs_path)) == t.found_bindings,
                   "the bindings are back as they were found, in the settings file too");
    return true;
}

static const SelfTestStep k_selftest_steps[] = {
    st_start,
    st_escape_opens, st_menu_pauses, st_escape_closes, st_keys_reach_game,
    st_menu_key_opens, st_menu_key_closes,
    st_guide_opens, st_guide_closes,
    st_pad_menu_opens, st_pad_rebinds_lid, st_pad_menu_closes, st_pad_lid,
    st_escape_opens, st_key_rebinds_layout, st_key_rebinds_touch, st_menu_key_closes,
    st_mouse_in_each_layout, st_hidden_screen, st_finger, st_cursor,
    st_blow, st_lid_key,
    st_save, st_owner,
    st_finish,
};

/* One call per loop, before events are polled; false when done. */
static bool menu_selftest_step(Frontend &f, MenuSelfTest &t)
{
    const size_t count = sizeof k_selftest_steps / sizeof k_selftest_steps[0];
    if (t.step < count && k_selftest_steps[t.step](f, t)) {
        t.step++;
        t.calls = 0;
        t.part = 0;
    } else {
        t.calls++;
    }
    if (t.step < count) return true;
    printf("menu selftest: %s\n", t.failures ? "FAIL" : "PASS");
    return false;
}

/* --menu-selftest-verify: a new run finds what the last one left. */
static int menu_selftest_verify(const Frontend &f)
{
    int failures = 0;
    const auto check = [&failures](bool ok, const std::string &what) {
        printf("menu selftest: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok) failures++;
    };
    check(f.volume == SELFTEST_VOLUME, "the volume set in the menu is " + std::to_string(f.volume) + " after a restart");
    check(f.layout == SELFTEST_LAYOUT, std::string("the layout is ") + nds_layout_label(f.layout) + " after a restart");
    check(!f.muted, "the last run's --mute was not saved");
    check(f.fullscreen_saved, "Fullscreen, turned on in the menu, is still on after a restart");
    uint32_t size = 0;
    const uint8_t *save = nds_save_memory(f.m, &size);
    check(save && size >= SELFTEST_SAVE_OFFSET + sizeof k_selftest_save_mark &&
              !memcmp(save + SELFTEST_SAVE_OFFSET, k_selftest_save_mark, sizeof k_selftest_save_mark),
          "the save written by the last run is loaded into the card (" + std::to_string(size) + " bytes)");
    char nickname[NDS_NICKNAME_MAX + 1];
    owner_nickname(f, nickname);
    check(f.m->settings.language == SELFTEST_LANGUAGE && !strcmp(nickname, k_selftest_nickname),
          std::string("the console's language and nickname come back from the firmware file (") +
              k_language_names[clamp_int(f.m->settings.language, 0, 5)] + ", " + nickname + ")");
    printf("menu selftest: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

static int usage(const char *exe)
{
    fprintf(stderr,
            "usage: %s [ROM] [--bios9 FILE] [--bios7 FILE] [--scale N] [--layout stacked|side|large|single]\n"
            "          [--save-dir DIR] [--fullscreen] [--mute] [--interpreter] [--limit-frames N]\n"
            "          [--screenshot-prefix P] [--audio-stats] [--prefs FILE] [--display-shot FILE.bmp]\n"
            "          [--display-size WxH] [--internal-scale N] [--menu-shot FILE.bmp]\n"
            "          [--menu-selftest | --menu-selftest-verify]\n",
            exe);
    return 2;
}

int nds_sdl_main(int argc, char **argv, const NdsRcProgram *program, const char *default_rom)
{
    const char *rom_path = NULL, *bios_paths[2] = { NULL, NULL }, *save_dir = NULL, *shot_prefix = NULL;
    const char *prefs_file = NULL, *display_shot = NULL, *menu_shot = NULL;
    int cli_scale = 0, cli_layout = -1, display_w = 3840, display_h = 2160;
    int cli_internal_scale = 0;
    long limit_frames = -1;
    bool cli_fullscreen = false, cli_muted = false, interpreter_only = false, audio_stats = false;
    bool selftest_verify = false;
    MenuSelfTest selftest;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--bios9") && i + 1 < argc) bios_paths[NDS_ARM9] = argv[++i];
        else if (!strcmp(a, "--bios7") && i + 1 < argc) bios_paths[NDS_ARM7] = argv[++i];
        else if (!strcmp(a, "--scale") && i + 1 < argc) cli_scale = atoi(argv[++i]);
        else if (!strcmp(a, "--layout") && i + 1 < argc) {
            cli_layout = nds_layout_from_config(argv[++i]);
            if (cli_layout < 0) {
                fprintf(stderr, "--layout wants stacked, side, large or single\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--internal-scale") && i + 1 < argc) {
            cli_internal_scale = atoi(argv[++i]);
            if (cli_internal_scale < 1 || cli_internal_scale > NDS_INTERNAL_SCALE_MAX) {
                fprintf(stderr, "--internal-scale takes 1 to %d, not %s\n", NDS_INTERNAL_SCALE_MAX, argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--save-dir") && i + 1 < argc) save_dir = argv[++i];
        else if (!strcmp(a, "--fullscreen")) cli_fullscreen = true;
        else if (!strcmp(a, "--mute")) cli_muted = true;
        else if (!strcmp(a, "--audio-stats")) audio_stats = true;
        else if (!strcmp(a, "--interpreter")) interpreter_only = true;
        else if (!strcmp(a, "--limit-frames") && i + 1 < argc) limit_frames = atol(argv[++i]);
        else if (!strcmp(a, "--screenshot-prefix") && i + 1 < argc) shot_prefix = argv[++i];
        else if (!strcmp(a, "--prefs") && i + 1 < argc) prefs_file = argv[++i];
        else if (!strcmp(a, "--display-shot") && i + 1 < argc) display_shot = argv[++i];
        else if (!strcmp(a, "--menu-shot") && i + 1 < argc) menu_shot = argv[++i];
        else if (!strcmp(a, "--display-size") && i + 1 < argc) {
            char *x = NULL;
            display_w = (int)strtol(argv[++i], &x, 10);
            display_h = x && *x == 'x' ? (int)strtol(x + 1, NULL, 10) : 0;
            if (display_w < 1 || display_h < 1 || display_w > 16384 || display_h > 16384) {
                fprintf(stderr, "--display-size wants WxH, e.g. 3440x1440\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--menu-selftest")) selftest.enabled = true;
        else if (!strcmp(a, "--menu-selftest-verify")) selftest_verify = true;
        else if (a[0] != '-' && !rom_path) rom_path = a;
        else {
            fprintf(stderr, "unknown argument %s\n", a);
            return usage(argv[0]);
        }
    }

    SDL_SetMainReady();
    /* The self-test's controller must be heard whether or not the window
     * has the keyboard. */
    if (selftest.enabled) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* The self-test's second run only reads files back: no window, sound
     * or controller. */
    if (SDL_Init(selftest_verify ? 0 : SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
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
        quit_sdl();
        return usage(argv[0]);
    }

    NdsMachine *m = nds_create();
    if (!m) {
        fprintf(stderr, "out of memory\n");
        quit_sdl();
        return 1;
    }
    size_t size = 0;
    bool loaded;
    if (embedded) {
        const uint8_t *data = program->embedded_rom(&size);
        loaded = data && nds_load_rom(m, data, size);
    } else {
        uint8_t *rom = read_file(rom_path, &size);
        loaded = rom && nds_load_rom(m, rom, size);
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
        nds_destroy(m);
        quit_sdl();
        return 1;
    }
    /* Saves and screenshots are named after the executable for an
     * embedded ROM (the hub keeps <game id>.sav next to <game id>.exe). */
    if (embedded) rom_path = argv[0];
    for (int cpu = 0; cpu < 2; cpu++) {
        if (!bios_paths[cpu]) continue;
        uint8_t *bios = read_file(bios_paths[cpu], &size);
        if (!bios || !(cpu == NDS_ARM9 ? nds_load_bios9(m, bios, size) : nds_load_bios7(m, bios, size))) {
            fprintf(stderr, "cannot load BIOS %s; using the built-in one\n", bios_paths[cpu]);
        }
        free(bios);
    }
    if (program && !interpreter_only && !nds_rc_install(m, program)) {
        fprintf(stderr, "out of memory for the recompiled program; running interpreted\n");
    }

    Frontend f;
    f.m = m;
    char save_path[1100], shot_path[1100];
    if (save_dir) make_dir(save_dir); /* fails harmlessly if it exists */
    sibling_path(save_path, sizeof save_path, save_dir, rom_path, ".sav");
    sibling_path(shot_path, sizeof shot_path, save_dir, rom_path, ".bmp");
    f.save.path = save_path;
    f.shot_path = shot_path;

    if (prefs_file) {
        f.prefs_path = prefs_file;
    } else {
        char *pref_dir = SDL_GetPrefPath("gbrecompiled", "runtime-nds");
        if (pref_dir) f.prefs_path = std::string(pref_dir) + "nds_prefs.ini";
        SDL_free(pref_dir);
    }
    /* The firmware lives beside the settings, wherever those are. */
    if (!f.prefs_path.empty()) {
        const size_t slash = f.prefs_path.find_last_of("/\\");
        f.firmware.path = (slash == std::string::npos ? std::string() : f.prefs_path.substr(0, slash + 1)) + "nds_firmware.bin";
    }
    load_prefs(f);

    /* What the game finds when it starts: its save, the console's own
     * memory and the time. All three are taken at the reset. */
    read_save(f);
    read_firmware(f);
    if (selftest_verify) {
        const int result = menu_selftest_verify(f);
        nds_destroy(m);
        quit_sdl();
        return result;
    }
    nds_set_rtc_base(m, local_clock_seconds());
    nds_reset(m);

    if (cli_scale > 0) f.scale = clamp_int(cli_scale, 1, MAX_SCALE);
    if (cli_layout >= 0) f.layout = cli_layout;
    f.internal_scale_this_run = cli_internal_scale;
    apply_internal_scale(f);
    f.mute_this_run = cli_muted;
    f.fullscreen = cli_fullscreen || f.fullscreen_saved;
    f.unthrottled = selftest.enabled;

    f.title = game_title(m);
    char title[200];
    snprintf(title, sizeof title, "%s - Native Console", f.title.c_str());
    int win_w = 0, win_h = 0, min_w = 0, min_h = 0;
    nds_layout_size(f.layout, f.gap, fitting_scale(f, f.scale), &win_w, &win_h);
    nds_layout_size(f.layout, f.gap, 1, &min_w, &min_h);
    f.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                             SDL_WINDOW_RESIZABLE | (f.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    f.ren = f.win ? SDL_CreateRenderer(f.win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (f.win && !f.ren) f.ren = SDL_CreateRenderer(f.win, -1, SDL_RENDERER_SOFTWARE); /* no GPU (VMs, remote desktop) */
    if (!f.ren || !create_screen_textures(f)) {
        fprintf(stderr, "SDL video: %s\n", SDL_GetError());
        nds_destroy(m);
        quit_sdl();
        return 1;
    }
    SDL_SetWindowMinimumSize(f.win, min_w, min_h);
    update_layout(f);

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
    SDL_AudioStream *stream = f.audio ? SDL_NewAudioStream(AUDIO_S16SYS, 2, AUDIO_RATE, have.format, have.channels, have.freq) : NULL;
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
    /* This computer's microphone, only if the player switched it on in the
     * menu; never for the self-test. */
    if (f.host_mic && !selftest.enabled) open_capture(f);

    if (selftest.enabled) selftest_attach_pad(f, selftest);
    choose_pad(f);

    const double freq = (double)SDL_GetPerformanceFrequency();
    double next_frame = (double)SDL_GetPerformanceCounter() / freq;
    f.fps_since = SDL_GetTicks();
    bool running = true;
    static int16_t samples[4096 * 2];
    static uint8_t converted[65536];
    uint64_t stat_dry = 0, stat_sum = 0, stat_n = 0, stat_dropped = 0;
    Uint32 stat_min = UINT32_MAX;

    while (running) {
        if (selftest.enabled && !menu_selftest_step(f, selftest)) running = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                const int sc = ev.key.keysym.scancode;
                if (sc > SDL_SCANCODE_UNKNOWN && sc < SDL_NUM_SCANCODES) f.key_down[sc] = ev.type == SDL_KEYDOWN;
            }
            /* Keys pressed while a binding is captured belong to it, not to
             * menu navigation (key releases still go through, so ImGui does
             * not think a key stays down). */
            if (!(f.capturing && (ev.type == SDL_KEYDOWN || ev.type == SDL_TEXTINPUT))) ImGui_ImplSDL2_ProcessEvent(&ev);
            if (handle_capture(f, ev)) continue;
            if (ev.type == SDL_QUIT) running = false;
            else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                if (ev.type == SDL_CONTROLLERDEVICEREMOVED && f.pad && !f.pad_fixed &&
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
                 * used to write that run's fullscreen as the setting. A
                 * window back from fullscreen or maximized takes the size
                 * of a layout chosen meanwhile. */
                const bool fullscreen_now = (SDL_GetWindowFlags(f.win) & SDL_WINDOW_FULLSCREEN) != 0;
                if (fullscreen_now != f.fullscreen) {
                    f.fullscreen = fullscreen_now;
                    if (f.fullscreen_saved != f.fullscreen) {
                        f.fullscreen_saved = f.fullscreen;
                        save_prefs(f);
                    }
                }
                if (f.window_stale) apply_window_size(f);
                else update_layout(f);
            } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                /* SDL lets go of the keys itself; the pen is ours. */
                release_pointers(f);
            } else if (ev.type == SDL_RENDER_DEVICE_RESET) {
                /* The renderer lost its textures (Direct3D device reset). */
                destroy_screen_textures(f);
                if (!create_screen_textures(f)) running = false;
            } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat && !io.WantTextInput &&
                       (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE || ev.key.keysym.scancode == SDL_SCANCODE_AC_BACK)) {
                set_menu(f, !f.show_menu);
            } else if (ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP ||
                       ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION || ev.type == SDL_FINGERUP) {
                pointer_event(f, ev);
            } else if ((ev.type == SDL_KEYDOWN && !ev.key.repeat && !io.WantTextInput) || ev.type == SDL_CONTROLLERBUTTONDOWN) {
                note_shortcut_press(f, ev);
            }
        }
        run_shortcuts(f);
        /* Controller navigation of the menu waits while a binding is being
         * captured, and until the buttons used for it are released. */
        if (f.pad_nav_blocked && !f.capturing && !any_pad_input(f)) f.pad_nav_blocked = false;
        if (f.capturing || f.pad_nav_blocked || !f.pad) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        const bool paused = f.show_menu;
        if (!paused) {
            /* What is held: buttons, the pen, the microphone and the lid.
             * Anything still down from the menu waits to be let go. */
            uint32_t held = console_actions(f);
            f.held_from_menu &= held;
            held &= ~f.held_from_menu;
            uint32_t keys = 0;
            for (int a = 0; a < NDS_ACTION_GAMEPLAY_COUNT; a++) {
                if (held & (1u << a)) keys |= nds_action_key_bit(a);
            }
            nds_set_keys(m, keys);

            /* The mouse or a finger where it points; else the controller's
             * cursor while its pen button is held. */
            const bool pad_pen = (held & (1u << NDS_ACTION_TOUCH)) != 0;
            update_cursor(f, pad_pen);
            int touch_x = 0, touch_y = 0;
            if (f.pen_mouse) nds_layout_touch(f.screens, f.mouse_x, f.mouse_y, &touch_x, &touch_y);
            else if (f.pen_finger) nds_layout_touch(f.screens, f.finger_x, f.finger_y, &touch_x, &touch_y);
            else if (pad_pen) {
                touch_x = (int)f.cursor_x;
                touch_y = (int)f.cursor_y;
            }
            nds_set_touch(m, f.pen_mouse || f.pen_finger || pad_pen, touch_x, touch_y);

            const bool lid = (held & (1u << NDS_ACTION_LID)) != 0 || f.lid_frames > 0;
            if (f.lid_frames > 0) f.lid_frames--;
            if (lid != f.lid_closed) set_status(f, lid ? "Lid closed" : "Lid open");
            f.lid_closed = lid;
            nds_set_lid(m, lid);
            f.blowing = (held & (1u << NDS_ACTION_BLOW)) != 0;

            const uint64_t interpreted[2] = { m->interpreted[NDS_ARM9], m->interpreted[NDS_ARM7] };
            run_frame(f);
            f.interpreted_last_frame[NDS_ARM9] = m->interpreted[NDS_ARM9] - interpreted[NDS_ARM9];
            f.interpreted_last_frame[NDS_ARM7] = m->interpreted[NDS_ARM7] - interpreted[NDS_ARM7];
            track_files(f);
            if (limit_frames > 0 && (long)m->frame_count >= limit_frames) running = false;
            if (m->powered_off) {
                printf("the game switched the console off\n");
                running = false;
            }
            f.fps_frames++;
            Uint32 elapsed = SDL_GetTicks() - f.fps_since;
            if (elapsed >= 1000) {
                f.fps = f.fps_frames * 1000.0 / elapsed;
                f.fps_frames = 0;
                f.fps_since = SDL_GetTicks();
            }

            size_t frames = nds_audio_read(m, samples, 4096);
            if (stream && !f.fast && !f.unthrottled && f.speed == 100) {
                const Uint32 queued = SDL_GetQueuedAudioSize(f.audio);
                if (audio_stats) {
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
                /* The pacing below holds the queue at the target against a
                 * device whose clock is within half a percent of ours. One
                 * that takes much less than it is given (stalled, or SDL's
                 * dummy driver) would run the queue, and the delay before
                 * anything is heard, up without end: past AUDIO_QUEUE_LIMIT
                 * targets this frame's sound is dropped instead. */
                if (queued > AUDIO_QUEUE_LIMIT * audio_target) {
                    stat_dropped++;
                    frames = 0;
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

        /* Pace to the DS frame rate, steered by the audio queue: below half
         * the target, run the next frame at once (catch up); otherwise run up
         * to 0.5 % fast or slow to hold the queue at the target, since the
         * audio device's clock and the performance counter drift apart.
         * Other speeds pace by the clock alone; the menu redraws at about
         * 60 Hz while the game is paused. */
        double now = (double)SDL_GetPerformanceCounter() / freq;
        if (f.unthrottled) {
            next_frame = now;
        } else if (paused) {
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
        if (stat_dropped) {
            fprintf(stderr, "audio: the device fell behind; %llu frames' sound was dropped to keep the queue short\n",
                    (unsigned long long)stat_dropped);
        }
    }
    if (shot_prefix) {
        char path[1100];
        snprintf(path, sizeof path, "%s.bmp", shot_prefix);
        screenshot(m, path);
    }
    if (display_shot) save_display_shot(f, display_shot, display_w, display_h);
    if (menu_shot) save_menu_shot(f, menu_shot);
    flush_files(f);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (f.pad) SDL_GameControllerClose(f.pad);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (selftest.joy_index >= 0) SDL_JoystickDetachVirtual(selftest.joy_index);
#endif
    close_capture(f);
    if (stream) SDL_FreeAudioStream(stream);
    if (f.audio) SDL_CloseAudioDevice(f.audio);
    destroy_screen_textures(f);
    SDL_DestroyRenderer(f.ren);
    SDL_DestroyWindow(f.win);
    nds_destroy(m);
    quit_sdl();
    return selftest.failures ? 1 : 0;
}
