/*
 * SDL2 frontend for GameCube programs: window, input, sound, frame pacing,
 * the memory card and the settings menu. Recompiled games call
 * gc_sdl_main(); headless options go to the runner instead (see
 * gc_sdl_wants_headless()). Adapted from the SNES frontend
 * (runtime-snes/platform/snes_sdl.cpp), whose menu this one follows.
 *
 *   <exe> [DISC] [--scale N] [--save-dir DIR] [--fullscreen] [--mute]
 *         [--interpreter] [--limit-frames N] [--screenshot-prefix P]
 *         [--audio-stats] [--prefs FILE] [--load-state FILE]
 *         [--renderer software|opengl|fast] [--gl] [--gl-fast] [--internal-scale N]
 *         [--anisotropy N] [--internal-shot FILE.bmp] [--gl-shaders MODE] [--gl-cache DIR|off]
 *         [--unthrottled] [--play-input SCRIPT] [--pace-log FILE] [--field-log FILE]
 *         [--shown-log FILE] [--shown-every N] [--wii-aspect 4:3|16:9]
 *         [--menu-selftest | --menu-selftest-verify]
 *
 * --shown-log writes a line a field about the picture the window shows:
 * "field hash source kept width height", source being the field whose
 * display buffer it is (earlier than the field when a present kept the
 * picture before). --shown-every N writes it as P-NNNNNN.bmp every N fields
 * (P from --screenshot-prefix). run_checks.py's skip stage compares the log
 * with the graphics thread on and off.
 *
 * --audio-stats prints, on exit, the sound ring's fill and how often the
 * device ran dry, the latency before the system mixer, how the pacing went,
 * the speed and a state hash. --pace-log writes one line a field: field,
 * start, run, present and wait times, lateness against the clock (ms), the
 * sound ring (ms), whether the machine was behind and whether a debt was
 * written off.
 *
 * --scale N is the menu's Window Size, 1 to 4 times 640 by 480. A size the
 * screen has no room for - title bar and taskbar counted - is brought down
 * to the largest that fits, for the window only: the setting stays as it
 * was chosen, and the menu says which size is in use.
 *
 * --limit-frames exits after N fields; --screenshot-prefix writes P.bmp of
 * the last frame on exit (the hub uses both to capture cover art).
 * --unthrottled runs as fast forward from start to end, without pacing or
 * sound, which is how the windowed frontend's speed is measured.
 * --play-input plays a headless --input script (gc_runner.h) on port 1 instead
 * of the keyboard and controllers, so a window can be timed on the same play
 * as a headless run. The menu
 * self-test is described above menu_selftest_step().
 *
 * The renderer is chosen in the menu (Software, OpenGL or OpenGL (fast),
 * kept in gc_prefs.ini); --renderer, or --gl and --gl-fast, chooses it for
 * one run without changing the setting. OpenGL draws with the hybrid shader
 * mode unless --gl-shaders says otherwise, and keeps its compiled programs
 * beside the game (<id>.pipelines, <id>.<hash>.glbin; in --save-dir when
 * given), so a second start compiles nothing. Software and OpenGL draw the
 * same picture, byte for byte (docs/GC-PLAN.md, "The OpenGL path"); the
 * fast one lets the card rasterise and blend, and is close and not the
 * same ("The fast path"). It can also draw at two to four times the
 * console's size (Internal Resolution in the menu, display.internal_scale,
 * --internal-scale N for one run), and filter a texture seen at a slant
 * with up to sixteen samples where the reference takes one (Anisotropic
 * Filtering, display.anisotropy, --anisotropy N). The display buffer still gets the
 * picture brought down to the console's size, which is what the game can
 * read back; the window shows the larger one (show_larger).
 * --screenshot-prefix still writes the display buffer's picture, so a cover
 * is the same size whatever the setting; --internal-shot FILE.bmp writes
 * the larger one on exit, when there is one.
 * The screenshot key (F12) saves what the window is showing, as <id>.bmp
 * beside the game or in --save-dir: the larger picture when one is on
 * screen, the display buffer's otherwise.
 *
 * A Wii is set to a 16:9 television unless the menu's "Widescreen (16:9)"
 * is off (wii.widescreen in gc_prefs.ini; --wii-aspect 4:3|16:9 for one
 * run). The setting is the console's - IPL.AR in its SYSCONF - written
 * before the game starts, which is when a game reads it; the game then
 * draws its 640 pixels for a picture sixteen wide to nine high, and the
 * window shows them that shape. A game that is running keeps the shape it
 * started in, and so does a save state. The display buffer, a screenshot
 * and a cover are still the console's 640 pixels across.
 *
 * Controls are bindable in the menu (gc_input.h). The one thing worth
 * knowing before reading them: the arrow keys are the *analogue stick*, not
 * the d-pad, because that is what a GameCube game is steered with. Escape
 * and a controller's Guide button always open the settings menu, and the
 * game pauses while it is open; L3 does too by default, which is the way in
 * on a Steam Deck.
 *
 * Display, audio and control choices are kept in gc_prefs.ini in the
 * per-user app data folder, shared by all GameCube games; bindings are
 * stored only where they differ from the platform's defaults. The memory
 * card is a file beside the game, and the runtime writes it back itself.
 * Games built from different versions of this runtime share that file, so
 * a line for a setting this build does not know is kept and written back.
 *
 * Save states go in ten slots, <id>.state1 to .state10 beside the game (or
 * in --save-dir), from the menu or the shortcuts (F5 save, F8 load, F6/F7
 * slot; the Deck's back paddles). F5 opens a dialog rather than writing at
 * once: it says what the slot already holds before replacing it, and takes a
 * name, which is written into the state's own header. Enter saves, Escape
 * cancels. A state holds the memory card's image, so loading one puts the
 * card back as it was when the state was saved. See gc_state.h.
 */
#include "gc_sdl.h"

#define SDL_MAIN_HANDLED /* callers keep their own main() */
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include "gc.h"
#include "gc_audio.h"
#include "gc_audio_out.h"
#include "gc_card.h"
#include "gc_disc.h"
#include "gc_efb.h"
#include "gc_gl.h"
#include "gc_input.h"
#include "gc_pad.h"
#include "gc_perf.h"
#include "gc_runner.h"
#include "gc_state.h"
#include "wii.h"
#include "wii_nand.h"

#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_dir(path) mkdir(path, 0777)
#endif

/* A field, not a frame: the machine counts the halves of an interlaced
 * picture and so does everything that paces against it. 59.94 fields a
 * second is the NTSC rate the video interface actually runs at. */
#define FIELD_SECONDS (1.0 / 59.94)
#define SCREEN_W 640
#define SCREEN_H 480

static const char *const k_headless_flags[] = {
    "--frames",   "--dump-frame", "--dump-frame-every", "--dump-state", "--trace-io", "--quiet",
    "--profile",  "--branches",   "--stop-at",    "--stop-after", "--watch",
    "--pad-test", "--no-apploader", "--stop-on-bad-access", "--allow-bad-access",
    "--differential", "--coverage", "--trace-out", "--input", "--auto-input",
    "--save-state", "--save-at", "--state-hash", "--state-uncompressed",
};

bool gc_sdl_wants_headless(int argc, char **argv)
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

/* <dir>/<disc file name without extension><ext>; dir defaults to the
 * executable's directory. Inside a macOS .app that folder belongs to the
 * signed bundle, so the default there is the per-user app data folder. */
static void sibling_path(char *out, size_t cap, const char *dir, const char *disc_path, const char *ext)
{
    const char *leaf = disc_path;
    for (const char *p = disc_path; *p; p++) {
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

/* ------------------------------------------------------------------ */
/* Settings menu and overlay                                           */
/* ------------------------------------------------------------------ */

enum { SCALING_PIXEL_PERFECT, SCALING_ASPECT_FIT, SCALING_ASPECT_FILL, SCALING_STRETCH };
static const char *const k_scaling_names[] = { "Pixel Perfect", "Aspect Fit", "Aspect Fill", "Stretch" };
static const char *const k_filter_names[] = { "Nearest", "Linear" };
static const char *const k_scale_names[] = { "1x (640x480)", "2x (1280x960)", "3x (1920x1440)", "4x (2560x1920)" };
enum { DEFAULT_SCALE = 1, MAX_SCALE = 4 };

enum { SLOT_COUNT = 10 };

/* What a Wii game is played with. The remote's buttons are the GameCube
 * actions under other names, so one set of bindings serves both consoles;
 * build_remote says which is which for each of these. */
enum { WII_STYLE_NUNCHUK, WII_STYLE_SIDEWAYS, WII_STYLE_CLASSIC, WII_STYLE_GAMECUBE, WII_STYLE_COUNT };
static const char *const k_wii_style_names[] = { "Wii Remote + Nunchuk", "Wii Remote, held sideways",
                                                 "Classic Controller", "GameCube Controller" };

enum { RENDERER_SOFTWARE = 0, RENDERER_OPENGL = 1, RENDERER_OPENGL_FAST = 2 };
static const char *const k_renderer_names[] = { "Software", "OpenGL", "OpenGL (fast)" };
/* How large OpenGL (fast) draws, in multiples of the console's own 640 by
 * 528 (gc_gl.h). */
enum { INTERNAL_SCALE_MAX = 4 };
static const char *const k_internal_scale_names[] = { "1x (the console's own)", "2x (1280 by 1056)",
                                                      "3x (1920 by 1584)", "4x (2560 by 2112)" };
/* How many samples OpenGL (fast) lets the card take along a texture seen at
 * a slant (gc_gl.h); the setting is the number, and the menu offers these. */
enum { ANISOTROPY_MAX = 16 };
static const int k_anisotropy_values[] = { 1, 2, 4, 8, 16 };
static const char *const k_anisotropy_names[] = { "Off", "2x", "4x", "8x", "16x" };

/* The entry of k_anisotropy_values a number stands for: the largest one not
 * above it. */
static int anisotropy_index(int value)
{
    int index = 0;
    for (int i = 0; i < (int)(sizeof(k_anisotropy_values) / sizeof(k_anisotropy_values[0])); i++) {
        if (k_anisotropy_values[i] <= value) index = i;
    }
    return index;
}

/* Master volume slider position, 0-100. The mixed output is already at the
 * scale a television would play, so the slider is a straight percentage
 * rather than the heavy attenuation the older consoles' runtimes need. */
enum { VOLUME_DEFAULT = 80, VOLUME_MAX = 100 };

/* Frontend state kept between fields. */
struct Frontend {
    GcMachine *m = nullptr;
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    SDL_Texture *tex = nullptr;
    SDL_Rect viewport = { 0, 0, SCREEN_W, SCREEN_H };
    SDL_GameController *pad = nullptr;
    GcAudioOut *audio = nullptr; /* gc_audio_out.h; null when there is no device */
    std::string title;

    /* Kept in gc_prefs.ini. */
    int scale = DEFAULT_SCALE;
    int scaling = SCALING_ASPECT_FIT;
    int filter = 1; /* index into k_filter_names */
    bool show_overlay = false;
    bool show_fps = false;
    /* The video interface at twice its rate: a game that draws on every
     * second retrace then draws every field. Whether that is sixty right
     * pictures a second or the game at double speed depends on the game
     * (gc.h), so it is off unless a player turns it on and looks. */
    bool vi_double = false;
    int renderer = RENDERER_SOFTWARE;
    bool muted = false;
    int volume = VOLUME_DEFAULT;
    GcInputMap map = {};

    /* A Wii game: how the player's controls reach the remote (kept in
     * gc_prefs.ini), and where the remote is pointing - 0..65535 across and
     * down the picture - which the mouse sets and the right stick moves. */
    int wii_style = WII_STYLE_NUNCHUK;
    /* A Wii's television: the setting (kept in gc_prefs.ini; the console's
     * own is written from it before a game starts), one run's
     * (--wii-aspect: 0 for none, 1 for 4:3, 2 for 16:9), and the shape the
     * game that is running was started in, which is the shape the window
     * shows. */
    bool wii_wide = true;
    int wii_aspect_this_run = 0;
    bool wide_now = false;
    int pointer_x = 32768, pointer_y = 32768;
    bool pointer_visible = false;
    int mouse_x = -1, mouse_y = -1;
    unsigned shake_phase = 0;

    /* Controls. */
    GcInputPlatform platform = gc_input_host_platform();
    GcPadProfile pad_profile = GC_PAD_GENERIC;
    GcInputMap defaults = {};
    bool action_down[GC_ACTION_COUNT] = {};   /* held at the last poll */
    bool action_tapped[GC_ACTION_COUNT] = {}; /* pressed by an event since the last poll */
    bool input_resync = false;                /* take the next poll as the baseline, firing nothing */
    bool capturing = false;                   /* waiting for a key or button to bind */
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
    int speed = 100;   /* percent; sound plays only at 100 */
    bool fast = false; /* fast forward held: unthrottled, no sound */
    bool unthrottled = false; /* --unthrottled: fast forward for the whole run, for measuring */
    GcInputScript *play = NULL; /* --play-input: port 1 follows a script, for measuring */

    std::string prefs_path, shot_path, card_path;
    /* The settings file's lines for settings this build does not know, each
     * as it was read and ending in a newline (load_prefs). */
    std::string prefs_other;
    /* The OpenGL path: whether it is drawing, how it was asked for, and why
     * it is not when it was asked for and could not start. */
    bool gl_on = false;
    bool gl_fast = false; /* ... and it is the fast path (gc_gl.h) */
    int gl_scale = 1;     /* ... drawing at this many times the console's size */
    int internal_scale = 1;          /* the setting, for OpenGL (fast) */
    int gl_anisotropy = 1;           /* ... and filtering slanted textures with this many samples */
    int anisotropy = 1;              /* the setting, for OpenGL (fast) */
    int anisotropy_this_run = 0;     /* --anisotropy: this run only */
    /* The picture as large as OpenGL (fast) drew it, when that is larger
     * than the console's (gc_gl_display_picture): shown in place of the
     * display buffer's, which holds it brought down to the console's size. */
    SDL_Texture *big = nullptr;
    int big_w = 0, big_h = 0;
    uint64_t big_serial = 0;
    bool big_on = false;
    /* The two halves of a present (present_take, present_show): the buffer
     * on screen as the first half found it, for the second to fetch the
     * larger picture of; whether the second half is still owed, to be done
     * while the next field runs; and what it has been costing, in seconds. */
    bool larger_due = false, place_ok = false;
    uint32_t place_first = 0, place_width = 0, place_height = 0;
    bool show_pending = false;
    double show_cost = 0.002;
    int internal_scale_this_run = 0; /* --internal-scale: this run only */
    int renderer_this_run = -1; /* --renderer / --gl: this run only */
    std::string gl_shaders, gl_cache, gl_id, gl_note;
    std::string status;      /* the last thing worth saying */
    Uint32 status_until = 0; /* shown over the game until then */

    /* Save states: the selected slot (kept in gc_prefs.ini), the path every
     * slot's file name starts with, and which game a state must belong to. */
    int slot = 0;
    /* What each slot holds, read from the states' headers rather than by
     * loading them. Stale after any save, delete, or a file changing under
     * us, so it is read again whenever it is about to be shown. */
    bool slot_present[SLOT_COUNT] = {};
    GcStateInfo slot_info[SLOT_COUNT] = {};
    /* The save dialog: open, and the name being typed into it. The game is
     * paused while it is up, as it is for the menu. */
    bool save_dialog = false;
    bool save_dialog_focus = false;
    char save_name[GC_STATE_NAME_CHARS] = "";
    std::string state_stem;
    GcStateIdentity state_id = {};
    bool audio_restart = false; /* a state was loaded: what was queued is from somewhere else */
    struct SaveJob *save_job = nullptr; /* a save still being written */
    SDL_Thread *save_thread = nullptr;
    bool selftest = false;
    uint64_t selftest_saved_hash = 0, selftest_loaded_hash = 0;
    uint64_t selftest_saved_field = 0, selftest_loaded_field = 0;

    /* Fields a second, measured while the game runs. */
    double fps = 0.0;
    Uint32 fps_since = 0;
    int fps_fields = 0;
    unsigned frame_width = SCREEN_W, frame_height = SCREEN_H;

    /* --shown-every N: what the window shows, kept on this side so it can be
     * written out every N fields (P-NNNNNN.bmp with --screenshot-prefix P);
     * --shown-log FILE: a line a field of its hash ("field hash kept width
     * height", kept 1 when the present kept the picture before). And how
     * often a present kept the picture before (the display buffer was still
     * being drawn), and the longest run of those. */
    long shown_every = 0;
    std::string shown_prefix;
    FILE *shown_log = nullptr;
    uint32_t *shown = nullptr;
    uint64_t presents = 0, presents_kept = 0, kept_run = 0, kept_run_max = 0, present_waits = 0;
    double present_wait_ms = 0.0;
    uint64_t shown_source = 0; /* the field whose buffer the window shows */
};

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

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
 *
 * The file is shared: every GameCube and Wii game reads and rewrites the
 * same one, and each is a build of its own, made when its player last
 * rebuilt it. A setting or a control this build has no name for is another
 * build's, so its line is kept as it was read and written back
 * (prefs_other); rewriting the file from this build's own settings alone
 * meant a game built before Fullscreen was kept erased it for the rest
 * whenever anything was changed in its menu. */
static void load_prefs(Frontend &f)
{
    gc_input_defaults(f.defaults, f.platform);
    f.map = f.defaults;
    FILE *file = f.prefs_path.empty() ? NULL : fopen(f.prefs_path.c_str(), "r");
    if (!file) return;
    f.prefs_other.clear();
    char line[2048]; /* room for a setting that is a path */
    while (fgets(line, sizeof line, file)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        const std::string whole = trim(line); /* as written, for a line that turns out to be another build's */
        if (whole[0] == '#') continue;
        *eq = '\0';
        char *key = trim(line), *value = trim(eq + 1);
        int number = atoi(value);
        if (!strcmp(key, "display.window_scale")) f.scale = clamp_int(number, 1, MAX_SCALE);
        else if (!strcmp(key, "display.fullscreen")) f.fullscreen_saved = number != 0;
        else if (!strcmp(key, "display.scaling")) f.scaling = clamp_int(number, 0, SCALING_STRETCH);
        else if (!strcmp(key, "display.filter")) f.filter = clamp_int(number, 0, 1);
        else if (!strcmp(key, "display.overlay")) f.show_overlay = number != 0;
        else if (!strcmp(key, "display.fps")) f.show_fps = number != 0;
        else if (!strcmp(key, "display.vi_double")) f.vi_double = number != 0;
        else if (!strcmp(key, "display.renderer")) f.renderer = clamp_int(number, 0, RENDERER_OPENGL_FAST);
        else if (!strcmp(key, "display.internal_scale")) f.internal_scale = clamp_int(number, 1, INTERNAL_SCALE_MAX);
        else if (!strcmp(key, "display.anisotropy")) f.anisotropy = k_anisotropy_values[anisotropy_index(number)];
        else if (!strcmp(key, "audio.muted")) f.muted = number != 0;
        else if (!strcmp(key, "audio.master_volume")) f.volume = clamp_int(number, 0, VOLUME_MAX);
        else if (!strcmp(key, "savestate.slot")) f.slot = clamp_int(number - 1, 0, SLOT_COUNT - 1);
        else if (!strcmp(key, "wii.controller")) f.wii_style = clamp_int(number, 0, WII_STYLE_COUNT - 1);
        else if (!strcmp(key, "wii.widescreen")) f.wii_wide = number != 0;
        else if (!strncmp(key, "keyboard.", 9) || !strncmp(key, "controller.", 11)) {
            const bool keyboard = key[0] == 'k';
            char *name = key + (keyboard ? 9 : 11), *dot = strrchr(name, '.');
            if (!dot) continue;
            *dot = '\0';
            int action = -1;
            for (int a = 0; a < GC_ACTION_COUNT; a++) {
                if (!strcmp(name, gc_action_config(a))) action = a;
            }
            if (action < 0) {
                f.prefs_other += whole + "\n"; /* a control this build does not have */
                continue;
            }
            int slot = atoi(dot + 1);
            GcBinding b;
            if (slot < 0 || slot > 1 || !gc_binding_from_config(value, b)) continue;
            if (b.kind != GC_BIND_NONE && (b.kind == GC_BIND_KEY) != keyboard) continue;
            (keyboard ? f.map.keyboard : f.map.controller)[action][slot] = b;
        }
        else if (key[0]) f.prefs_other += whole + "\n";
    }
    fclose(file);
}

static void save_prefs(const Frontend &f)
{
    FILE *file = f.prefs_path.empty() ? NULL : fopen(f.prefs_path.c_str(), "w");
    if (!file) return;
    fprintf(file, "# Native Console GameCube settings\n");
    fprintf(file, "display.window_scale = %d\n", f.scale);
    fprintf(file, "display.fullscreen = %d\n", f.fullscreen_saved ? 1 : 0);
    fprintf(file, "display.scaling = %d\n", f.scaling);
    fprintf(file, "display.filter = %d\n", f.filter);
    fprintf(file, "display.overlay = %d\n", f.show_overlay ? 1 : 0);
    fprintf(file, "display.fps = %d\n", f.show_fps ? 1 : 0);
    fprintf(file, "display.vi_double = %d\n", f.vi_double ? 1 : 0);
    fprintf(file, "display.renderer = %d\n", f.renderer);
    fprintf(file, "display.internal_scale = %d\n", f.internal_scale);
    fprintf(file, "display.anisotropy = %d\n", f.anisotropy);
    fprintf(file, "audio.muted = %d\n", f.muted ? 1 : 0);
    fprintf(file, "audio.master_volume = %d\n", f.volume);
    fprintf(file, "savestate.slot = %d\n", f.slot + 1);
    fprintf(file, "wii.controller = %d\n", f.wii_style);
    fprintf(file, "wii.widescreen = %d\n", f.wii_wide ? 1 : 0);
    for (int a = 0; a < GC_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            if (f.map.keyboard[a][s] != f.defaults.keyboard[a][s]) {
                fprintf(file, "keyboard.%s.%d = %s\n", gc_action_config(a), s,
                        gc_binding_to_config(f.map.keyboard[a][s]).c_str());
            }
            if (f.map.controller[a][s] != f.defaults.controller[a][s]) {
                fprintf(file, "controller.%s.%d = %s\n", gc_action_config(a), s,
                        gc_binding_to_config(f.map.controller[a][s]).c_str());
            }
        }
    }
    if (!f.prefs_other.empty()) {
        fprintf(file, "# Settings of another build of the runtime, kept for it\n");
        fputs(f.prefs_other.c_str(), file);
    }
    fclose(file);
}

/* ------------------------------------------------------------------ */
/* Controls                                                            */
/* ------------------------------------------------------------------ */

static bool binding_down(const Frontend &f, const GcBinding &b, const Uint8 *kb, uint8_t mods)
{
    switch (b.kind) {
    case GC_BIND_KEY: return kb[b.code] && gc_key_mods_match(b, mods);
    case GC_BIND_BUTTON: return f.pad && SDL_GameControllerGetButton(f.pad, (SDL_GameControllerButton)b.code);
    case GC_BIND_AXIS_POSITIVE:
        return f.pad && SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code) > 16000;
    case GC_BIND_AXIS_NEGATIVE:
        return f.pad && SDL_GameControllerGetAxis(f.pad, (SDL_GameControllerAxis)b.code) < -16000;
    default: return false;
    }
}

static bool action_held(const Frontend &f, int action, const Uint8 *kb, uint8_t mods)
{
    for (int s = 0; s < 2; s++) {
        if (binding_down(f, f.map.keyboard[action][s], kb, mods) ||
            binding_down(f, f.map.controller[action][s], kb, mods))
            return true;
    }
    return false;
}

/* A host stick axis as the GameCube reports one: signed, centred on zero,
 * and with a dead zone, because a controller at rest is never exactly at
 * rest and a game reading a stick that is almost centred walks slowly in a
 * direction nobody asked for.
 *
 * The GameCube's own range is about Â±80 rather than the Â±127 the byte
 * allows - the stick's gate stops it going further - and a game calibrated
 * for that treats anything beyond as full deflection. Scaling to 80 rather
 * than to 127 means a host stick pushed all the way reads as a GameCube
 * stick pushed all the way, instead of as one pushed impossibly far. */
enum { STICK_DEAD_ZONE = 5000, STICK_RANGE = 80 };

static int8_t axis_to_stick(int value)
{
    int magnitude = value < 0 ? -value : value;
    if (magnitude <= STICK_DEAD_ZONE) return 0;
    if (magnitude > 32000) magnitude = 32000;
    /* Rescaled from the edge of the dead zone so that the first movement
     * past it is a small push rather than a jump. */
    int scaled = (magnitude - STICK_DEAD_ZONE) * STICK_RANGE / (32000 - STICK_DEAD_ZONE);
    if (scaled > STICK_RANGE) scaled = STICK_RANGE;
    return (int8_t)(value < 0 ? -scaled : scaled);
}

/* A trigger: unsigned, and pressed all the way is 255. */
static uint8_t axis_to_trigger(int value)
{
    if (value <= STICK_DEAD_ZONE) return 0;
    int scaled = (value - STICK_DEAD_ZONE) * 255 / (32767 - STICK_DEAD_ZONE);
    return (uint8_t)(scaled > 255 ? 255 : scaled);
}

/* The controller the game will read. The buttons come from the bindings;
 * the sticks come from the host's sticks directly, and from the keyboard's
 * bound directions when a key is pushing them instead. */
static void build_pad(const Frontend &f, GcPad &out)
{
    const Uint8 *kb = SDL_GetKeyboardState(NULL);
    const uint8_t mods = gc_mods_from_sdl(SDL_GetModState());

    memset(&out, 0, sizeof out);
    out.present = 1;
    for (int a = 0; a < GC_ACTION_GAMEPLAY_COUNT; a++) {
        if (action_held(f, a, kb, mods)) out.buttons |= gc_action_pad_bit(a);
    }

    if (f.pad) {
        out.stick_x = axis_to_stick(SDL_GameControllerGetAxis(f.pad, SDL_CONTROLLER_AXIS_LEFTX));
        /* The host's Y axis grows downwards and the GameCube's grows up. */
        out.stick_y = axis_to_stick(-SDL_GameControllerGetAxis(f.pad, SDL_CONTROLLER_AXIS_LEFTY));
        out.c_stick_x = axis_to_stick(SDL_GameControllerGetAxis(f.pad, SDL_CONTROLLER_AXIS_RIGHTX));
        out.c_stick_y = axis_to_stick(-SDL_GameControllerGetAxis(f.pad, SDL_CONTROLLER_AXIS_RIGHTY));
        out.trigger_l = axis_to_trigger(SDL_GameControllerGetAxis(f.pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        out.trigger_r = axis_to_trigger(SDL_GameControllerGetAxis(f.pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
        /* A trigger pressed all the way also closes the switch underneath
         * it, which is what a game reads as L or R held. */
        if (out.trigger_l > 230) out.buttons |= GC_PAD_L;
        if (out.trigger_r > 230) out.buttons |= GC_PAD_R;
    }

    /* A key can only be down or up, so a stick pushed by one goes all the
     * way. Opposite directions held together cancel, which is what a real
     * stick does and what a game expects. */
    struct {
        int action;
        int8_t *axis;
        int sign;
    } pushes[] = {
        { GC_ACTION_STICK_UP, &out.stick_y, 1 },     { GC_ACTION_STICK_DOWN, &out.stick_y, -1 },
        { GC_ACTION_STICK_RIGHT, &out.stick_x, 1 },  { GC_ACTION_STICK_LEFT, &out.stick_x, -1 },
        { GC_ACTION_C_UP, &out.c_stick_y, 1 },       { GC_ACTION_C_DOWN, &out.c_stick_y, -1 },
        { GC_ACTION_C_RIGHT, &out.c_stick_x, 1 },    { GC_ACTION_C_LEFT, &out.c_stick_x, -1 },
    };
    int pushed[4] = { 0, 0, 0, 0 };
    for (size_t i = 0; i < sizeof pushes / sizeof pushes[0]; i++) {
        if (action_held(f, pushes[i].action, kb, mods)) pushed[i / 2] += pushes[i].sign;
    }
    if (pushed[0]) out.stick_y = (int8_t)(pushed[0] > 0 ? STICK_RANGE : -STICK_RANGE);
    if (pushed[1]) out.stick_x = (int8_t)(pushed[1] > 0 ? STICK_RANGE : -STICK_RANGE);
    if (pushed[2]) out.c_stick_y = (int8_t)(pushed[2] > 0 ? STICK_RANGE : -STICK_RANGE);
    if (pushed[3]) out.c_stick_x = (int8_t)(pushed[3] > 0 ? STICK_RANGE : -STICK_RANGE);
}

/* Where the remote points. The mouse says so outright while it is over the
 * picture and moving; the right stick nudges it from wherever it is, for a
 * player with a controller and no mouse. Off the picture, the remote is
 * pointing away from the screen and a game sees no pointer. */
static void update_pointer(Frontend &f, const GcPad &pad)
{
    int mx = 0, my = 0, ww = 0, wh = 0, ow = 0, oh = 0;
    if (f.play) {
        /* A played script says where the remote points outright, as it does
         * headless (gc_runner.h): its second stick, the middle at rest. */
        f.pointer_x = clamp_int(32768 + pad.c_stick_x * 256, 0, 65535);
        f.pointer_y = clamp_int(32768 - pad.c_stick_y * 256, 0, 65535);
        f.pointer_visible = true;
        return;
    }
    SDL_GetMouseState(&mx, &my);
    SDL_GetWindowSize(f.win, &ww, &wh);
    SDL_GetRendererOutputSize(f.ren, &ow, &oh);
    if (ww > 0 && wh > 0 && ow > 0 && oh > 0) {
        mx = mx * ow / ww;
        my = my * oh / wh;
    }
    if (mx != f.mouse_x || my != f.mouse_y) {
        const bool first = f.mouse_x < 0;
        f.mouse_x = mx;
        f.mouse_y = my;
        if (!first && f.viewport.w > 0 && f.viewport.h > 0) {
            const int x = (mx - f.viewport.x) * 65535 / f.viewport.w;
            const int y = (my - f.viewport.y) * 65535 / f.viewport.h;
            f.pointer_visible = x >= 0 && x <= 65535 && y >= 0 && y <= 65535 && SDL_GetMouseFocus() == f.win;
            f.pointer_x = clamp_int(x, 0, 65535);
            f.pointer_y = clamp_int(y, 0, 65535);
        }
    }
    if (f.wii_style != WII_STYLE_CLASSIC && (pad.c_stick_x || pad.c_stick_y)) {
        /* Across the picture in about a second at full push. */
        f.pointer_x = clamp_int(f.pointer_x + pad.c_stick_x * 14, 0, 65535);
        f.pointer_y = clamp_int(f.pointer_y - pad.c_stick_y * 14, 0, 65535);
        f.pointer_visible = true;
    }
}

/* A GameCube stick's reach (STICK_RANGE) as an extension's, which goes to
 * about a hundred. */
static int8_t extension_stick(int8_t value)
{
    return (int8_t)clamp_int(value * 100 / STICK_RANGE, -127, 127);
}

/* The remote the game will read, from the controller already built and the
 * mouse. Which GameCube action is which of the remote's buttons depends on
 * how the game is held:
 *
 *   + Nunchuk   A, B and the d-pad are themselves; X and Y are 1 and 2;
 *               Start is +; the stick is the Nunchuk's; L and R are its Z
 *               and C. The mouse buttons are A and B as well, so a pointer
 *               game plays with the mouse alone.
 *   sideways    The remote turned a quarter left, as for a platformer: B
 *               and A are 1 and 2 (the two under the right thumb), X and Y
 *               are A and B, and the d-pad and the stick are both the
 *               remote's d-pad, turned to match.
 *   Classic     A Classic Controller in the remote: everything maps
 *               straight across, Z to ZR and the triggers to L and R.
 *   GameCube    The game is played on the GameCube controller (gc_pad_set);
 *               the remote is on and still, with the mouse as its pointer.
 *
 * Minus, Home and a shake are bindings of their own in every style. */
static void build_remote(Frontend &f, const GcPad &pad, WiiRemoteInput &out)
{
    const Uint8 *kb = SDL_GetKeyboardState(NULL);
    const uint8_t mods = gc_mods_from_sdl(SDL_GetModState());
    const Uint32 mouse = SDL_GetMouseState(NULL, NULL);
    const uint16_t b = pad.buttons;

    wii_remote_rest(&out, 1);
    if (action_held(f, GC_ACTION_WII_MINUS, kb, mods)) out.buttons |= WII_REMOTE_MINUS;
    if (action_held(f, GC_ACTION_WII_HOME, kb, mods)) out.buttons |= WII_REMOTE_HOME;
    out.pointer_visible = f.pointer_visible ? 1 : 0;
    out.pointer_x = (uint16_t)f.pointer_x;
    out.pointer_y = (uint16_t)f.pointer_y;
    if (f.pointer_visible && !ImGui::GetIO().WantCaptureMouse) {
        if (mouse & SDL_BUTTON_LMASK) out.buttons |= WII_REMOTE_A;
        if (mouse & SDL_BUTTON_RMASK) out.buttons |= WII_REMOTE_B;
    }

    switch (f.wii_style) {
    case WII_STYLE_NUNCHUK:
        if (b & GC_PAD_A) out.buttons |= WII_REMOTE_A;
        if (b & GC_PAD_B) out.buttons |= WII_REMOTE_B;
        if (b & GC_PAD_X) out.buttons |= WII_REMOTE_ONE;
        if (b & GC_PAD_Y) out.buttons |= WII_REMOTE_TWO;
        if (b & GC_PAD_START) out.buttons |= WII_REMOTE_PLUS;
        if (b & GC_PAD_UP) out.buttons |= WII_REMOTE_UP;
        if (b & GC_PAD_DOWN) out.buttons |= WII_REMOTE_DOWN;
        if (b & GC_PAD_LEFT) out.buttons |= WII_REMOTE_LEFT;
        if (b & GC_PAD_RIGHT) out.buttons |= WII_REMOTE_RIGHT;
        out.extension = WII_EXTENSION_NUNCHUK;
        out.nunchuk_stick_x = extension_stick(pad.stick_x);
        out.nunchuk_stick_y = extension_stick(pad.stick_y);
        if ((b & GC_PAD_L) || pad.trigger_l > 100) out.nunchuk_buttons |= WII_NUNCHUK_Z;
        if ((b & (GC_PAD_R | GC_PAD_Z)) || pad.trigger_r > 100) out.nunchuk_buttons |= WII_NUNCHUK_C;
        break;
    case WII_STYLE_SIDEWAYS: {
        /* Turned a quarter left: the player's up is the remote's right. */
        const bool up = (b & GC_PAD_UP) || pad.stick_y > 40, down = (b & GC_PAD_DOWN) || pad.stick_y < -40;
        const bool left = (b & GC_PAD_LEFT) || pad.stick_x < -40, right = (b & GC_PAD_RIGHT) || pad.stick_x > 40;
        if (up) out.buttons |= WII_REMOTE_RIGHT;
        if (down) out.buttons |= WII_REMOTE_LEFT;
        if (left) out.buttons |= WII_REMOTE_UP;
        if (right) out.buttons |= WII_REMOTE_DOWN;
        if (b & GC_PAD_B) out.buttons |= WII_REMOTE_ONE;
        if (b & GC_PAD_A) out.buttons |= WII_REMOTE_TWO;
        if (b & GC_PAD_X) out.buttons |= WII_REMOTE_A;
        if (b & GC_PAD_Y) out.buttons |= WII_REMOTE_B;
        if (b & GC_PAD_START) out.buttons |= WII_REMOTE_PLUS;
        /* Flat in two hands, face up: gravity is where it is at rest, and
         * the remote is not pointing at anything. */
        out.pointer_visible = 0;
        break;
    }
    case WII_STYLE_CLASSIC:
        out.extension = WII_EXTENSION_CLASSIC;
        out.classic_left_x = extension_stick(pad.stick_x);
        out.classic_left_y = extension_stick(pad.stick_y);
        out.classic_right_x = extension_stick(pad.c_stick_x);
        out.classic_right_y = extension_stick(pad.c_stick_y);
        out.classic_trigger_l = pad.trigger_l;
        out.classic_trigger_r = pad.trigger_r;
        if (b & GC_PAD_A) out.classic_buttons |= WII_CLASSIC_A;
        if (b & GC_PAD_B) out.classic_buttons |= WII_CLASSIC_B;
        if (b & GC_PAD_X) out.classic_buttons |= WII_CLASSIC_X;
        if (b & GC_PAD_Y) out.classic_buttons |= WII_CLASSIC_Y;
        if (b & GC_PAD_L) out.classic_buttons |= WII_CLASSIC_L;
        if (b & GC_PAD_R) out.classic_buttons |= WII_CLASSIC_R;
        if (b & GC_PAD_Z) out.classic_buttons |= WII_CLASSIC_ZR;
        if (b & GC_PAD_START) out.classic_buttons |= WII_CLASSIC_PLUS;
        if (b & GC_PAD_UP) out.classic_buttons |= WII_CLASSIC_UP;
        if (b & GC_PAD_DOWN) out.classic_buttons |= WII_CLASSIC_DOWN;
        if (b & GC_PAD_LEFT) out.classic_buttons |= WII_CLASSIC_LEFT;
        if (b & GC_PAD_RIGHT) out.classic_buttons |= WII_CLASSIC_RIGHT;
        if (out.buttons & WII_REMOTE_MINUS) out.classic_buttons |= WII_CLASSIC_MINUS;
        if (out.buttons & WII_REMOTE_HOME) out.classic_buttons |= WII_CLASSIC_HOME;
        break;
    default: break;
    }

    /* A shake: the remote swung hard one way and then the other, three
     * fields each, for as long as the control is held. Two g is past what
     * any game asks for and well inside what the sensor reports. */
    if (action_held(f, GC_ACTION_WII_SHAKE, kb, mods)) {
        const int swing = ((f.shake_phase++ / 3u) & 1u) ? -2000 : 2000;
        out.accel_x = (int16_t)swing;
        out.accel_z = (int16_t)(1000 + swing);
        if (out.extension == WII_EXTENSION_NUNCHUK) out.nunchuk_accel_x = (int16_t)swing;
    } else {
        f.shake_phase = 0;
    }
}

/* The buttons alone, for working out what is still held from the menu. */
static uint16_t gameplay_buttons(const Frontend &f)
{
    const Uint8 *kb = SDL_GetKeyboardState(NULL);
    const uint8_t mods = gc_mods_from_sdl(SDL_GetModState());
    uint16_t keys = 0;
    for (int a = 0; a < GC_ACTION_GAMEPLAY_COUNT; a++) {
        if (action_held(f, a, kb, mods)) keys |= gc_action_pad_bit(a);
    }
    return keys;
}

/* A shortcut pressed and released between two polls still counts. */
static void note_shortcut_press(Frontend &f, const SDL_Event &ev)
{
    for (int a = GC_ACTION_GAMEPLAY_COUNT; a < GC_ACTION_COUNT; a++) {
        for (int s = 0; s < 2; s++) {
            const GcBinding &k = f.map.keyboard[a][s], &c = f.map.controller[a][s];
            if (ev.type == SDL_KEYDOWN && k.kind == GC_BIND_KEY && k.code == ev.key.keysym.scancode &&
                gc_key_mods_match(k, gc_mods_from_sdl(ev.key.keysym.mod)))
                f.action_tapped[a] = true;
            if (ev.type == SDL_CONTROLLERBUTTONDOWN && c.kind == GC_BIND_BUTTON && c.code == ev.cbutton.button)
                f.action_tapped[a] = true;
        }
    }
}

static void assign_binding(Frontend &f, bool keyboard, int action, int slot, const GcBinding &b)
{
    GcBinding(*map)[2] = keyboard ? f.map.keyboard : f.map.controller;
    f.bind_note.clear();
    /* One control does one thing: take the binding off any other action. */
    for (int a = 0; a < GC_ACTION_COUNT && b.kind != GC_BIND_NONE; a++) {
        for (int s = 0; s < 2; s++) {
            if ((a != action || s != slot) && map[a][s] == b) {
                map[a][s].kind = GC_BIND_NONE;
                f.bind_note = gc_binding_label(b, f.platform, f.pad_profile) + " was moved from " +
                              gc_action_label(a) + ".";
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

static void finish_capture(Frontend &f, const GcBinding *b)
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
        else if (gc_scancode_is_modifier(sc)) f.capture_modifier = sc;
        else {
            GcBinding b = { GC_BIND_KEY, gc_mods_from_sdl(ev.key.keysym.mod), (int16_t)sc };
            finish_capture(f, &b);
        }
        return true;
    }
    case SDL_KEYUP:
        if (f.capture_keyboard && ev.key.keysym.scancode == f.capture_modifier) {
            GcBinding b = { GC_BIND_KEY, 0, (int16_t)f.capture_modifier };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) finish_capture(f, NULL);
        else if (!f.capture_keyboard) {
            GcBinding b = { GC_BIND_BUTTON, 0, (int16_t)ev.cbutton.button };
            finish_capture(f, &b);
        }
        return true;
    case SDL_CONTROLLERAXISMOTION:
        if (!f.capture_keyboard && (ev.caxis.value >= 16000 || ev.caxis.value <= -16000)) {
            GcBinding b = { (uint8_t)(ev.caxis.value > 0 ? GC_BIND_AXIS_POSITIVE : GC_BIND_AXIS_NEGATIVE), 0,
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
    f.pad_profile = gc_pad_profile(pad, f.platform);
}

/* ------------------------------------------------------------------ */
/* Window and display                                                  */
/* ------------------------------------------------------------------ */

/* How wide the picture is on a television, in the pixels SCREEN_H are
 * tall in: 640 for 4:3, and 853 for a Wii game started on a 16:9 one, whose
 * 640 pixels are each a third wider than they are tall. */
static double picture_width(const Frontend &f)
{
    return f.wide_now ? SCREEN_H * 16.0 / 9.0 : (double)SCREEN_W;
}

/* The shape the game that is running was started in: what its console's
 * settings say now, which nothing changes while it runs. */
static void read_picture_shape(Frontend &f)
{
    f.wide_now = gc_console_is_wii() && wii_console_widescreen(f.m) == 1;
}

/* Where the game goes inside the window (renderer output pixels). */
static void update_viewport(Frontend &f)
{
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(f.ren, &w, &h);
    if (w <= 0) w = SCREEN_W;
    if (h <= 0) h = SCREEN_H;
    const double wide = picture_width(f);
    double sx = (double)w / wide, sy = (double)h / SCREEN_H;
    int vw = w, vh = h;
    switch (f.scaling) {
    case SCALING_PIXEL_PERFECT: {
        /* Whole numbers of lines; across, a 16:9 picture's pixels are not
         * square, so only the 4:3 one is whole both ways. */
        int s = (int)(sx < sy ? sx : sy);
        if (s < 1) s = 1;
        vw = (int)(wide * s + 0.5);
        vh = SCREEN_H * s;
        break;
    }
    case SCALING_ASPECT_FIT:
    case SCALING_ASPECT_FILL: {
        double fit = sx < sy ? sx : sy, fill = sx < sy ? sy : sx;
        double s = f.scaling == SCALING_ASPECT_FIT ? fit : fill;
        vw = (int)(wide * s + 0.5);
        vh = (int)(SCREEN_H * s + 0.5);
        break;
    }
    default: break;
    }
    f.viewport.x = (w - vw) / 2;
    f.viewport.y = (h - vh) / 2;
    f.viewport.w = vw < 1 ? 1 : vw;
    f.viewport.h = vh < 1 ? 1 : vh;
}

/* The renderer this run should use: the command line's, or the setting. */
static int wanted_renderer(const Frontend &f)
{
    return f.renderer_this_run >= 0 ? f.renderer_this_run : f.renderer;
}

/* The name of the renderer that is drawing, which is not always the one
 * asked for: OpenGL may not have started. */
static const char *renderer_name(const Frontend &f)
{
    return k_renderer_names[!f.gl_on ? RENDERER_SOFTWARE : (f.gl_fast ? RENDERER_OPENGL_FAST : RENDERER_OPENGL)];
}

/* Starts or stops the OpenGL path to match, or restarts it to change
 * between its exact and fast ways of drawing. Switching between fields is
 * safe either way: stopping brings the framebuffer and every copy back into
 * memory first, and starting sends it to the card. */
static void apply_renderer(Frontend &f)
{
    const int wanted = wanted_renderer(f);
    const bool want = wanted != RENDERER_SOFTWARE;
    const bool fast = wanted == RENDERER_OPENGL_FAST;
    /* Only the fast path draws larger than the console does. */
    const int scale = fast ? clamp_int(f.internal_scale_this_run > 0 ? f.internal_scale_this_run : f.internal_scale,
                                       1, INTERNAL_SCALE_MAX)
                           : 1;
    /* ... and only it hands the card its textures to filter. */
    const int anisotropy = fast ? k_anisotropy_values[anisotropy_index(f.anisotropy_this_run > 0 ? f.anisotropy_this_run
                                                                                               : f.anisotropy)]
                                : 1;
    if (want == f.gl_on && (!want || (fast == f.gl_fast && scale == f.gl_scale && anisotropy == f.gl_anisotropy))) return;
    if (f.gl_on) {
        gc_gl_stop(f.m);
        f.gl_on = false;
        f.big_on = false;
        f.gl_note.clear();
    }
    if (!want) return;
    GcGlOptions options = {};
    options.shaders = f.gl_shaders.empty() ? "hybrid" : f.gl_shaders.c_str();
    options.cache_dir = f.gl_cache.c_str();
    options.id = f.gl_id.c_str();
    options.fast = fast ? 1 : 0;
    options.scale = scale;
    options.anisotropy = anisotropy;
    if (gc_gl_start_with(f.m, &options) == 0) {
        f.gl_on = true;
        f.gl_fast = fast;
        f.gl_scale = scale;
        f.gl_anisotropy = anisotropy;
        f.gl_note.clear();
    } else {
        f.gl_on = false;
        f.gl_note = "OpenGL could not start on this computer (it needs OpenGL 4.1); drawing in software.";
        fprintf(stderr, "gc: %s\n", f.gl_note.c_str());
    }
}

static void apply_filter(Frontend &f)
{
    SDL_SetTextureScaleMode(f.tex, f.filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
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

/* A window's title bar and frame as Windows draws them, for a window that
 * does not exist yet and cannot be asked; other desktops' are near enough. */
enum { FRAME_TOP_GUESS = 31, FRAME_SIDE_GUESS = 8 };

/* The room a window's picture has on its screen: the part the system leaves
 * free (no taskbar, no dock) less the window's own title bar and frame. A
 * window that does not exist yet goes on the first screen. False when the
 * system does not say. */
static bool window_room(const Frontend &f, SDL_Rect &room)
{
    SDL_Rect usable = { 0, 0, 0, 0 };
    int top = 0, left = 0, bottom = 0, right = 0;
    const int display = f.win ? SDL_GetWindowDisplayIndex(f.win) : 0;
    if (display < 0 || SDL_GetDisplayUsableBounds(display, &usable) != 0) return false;
    if (!f.win || SDL_GetWindowBordersSize(f.win, &top, &left, &bottom, &right) != 0) {
        top = FRAME_TOP_GUESS;
        left = right = bottom = FRAME_SIDE_GUESS;
    }
    room.x = usable.x + left;
    room.y = usable.y + top;
    room.w = usable.w - left - right;
    room.h = usable.h - top - bottom;
    return room.w > 0 && room.h > 0;
}

/* The Window Size in use: the setting, or the largest size below it that
 * fits when the setting's own does not. A window taller than its screen has
 * its title bar above the top edge and its last lines under the taskbar -
 * "3x (1920x1440)" on a screen of 1440 lines - and nothing on screen says
 * why. The setting itself is left as it was chosen. */
static int fitted_window_scale(const Frontend &f)
{
    SDL_Rect room;
    int scale = f.scale;
    if (!window_room(f, room)) return scale;
    while (scale > 1 && ((int)(picture_width(f) * scale + 0.5) > room.w || SCREEN_H * scale > room.h)) scale--;
    return scale;
}

/* A window of that size and where it goes: the middle of the room it has,
 * or of the screen when the system does not say what room that is. */
static void window_place(const Frontend &f, int &x, int &y, int &w, int &h)
{
    const int scale = fitted_window_scale(f);
    SDL_Rect room;
    w = (int)(picture_width(f) * scale + 0.5);
    h = SCREEN_H * scale;
    x = y = SDL_WINDOWPOS_CENTERED;
    if (window_room(f, room)) {
        x = room.x + (room.w > w ? (room.w - w) / 2 : 0);
        y = room.y + (room.h > h ? (room.h - h) / 2 : 0);
    }
}

static void apply_window_scale(Frontend &f)
{
    if (!f.fullscreen) {
        int x, y, w, h;
        window_place(f, x, y, w, h);
        SDL_SetWindowSize(f.win, w, h);
        SDL_SetWindowPosition(f.win, x, y);
    }
    update_viewport(f);
}

static void reset_display(Frontend &f)
{
    f.scale = DEFAULT_SCALE;
    f.scaling = SCALING_ASPECT_FIT;
    f.filter = 1;
    f.show_overlay = false;
    f.show_fps = false;
    f.speed = 100;
    apply_filter(f);
    if (f.fullscreen) set_fullscreen(f, false);
    apply_window_scale(f);
    save_prefs(f);
}

/* The larger picture, saved as it is - what the window is showing when an
 * internal resolution above 1x is on. False when there is none to save, or
 * it could not be written. */
static bool screenshot_larger(Frontend &f, const char *path)
{
    unsigned w = 0, h = 0;
    uint64_t serial = 0;
    bool saved = false;
    if (!f.big_on || gc_gl_display_picture(f.m, NULL, 0, 0, &w, &h, &serial) != 0) return false;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, (int)w, (int)h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return false;
    unsigned got_w = w, got_h = h;
    if (gc_gl_display_picture(f.m, (uint32_t *)s->pixels, (unsigned)(s->pitch / 4), h, &got_w, &got_h, &serial) == 0 &&
        got_w == w && got_h == h && SDL_SaveBMP(s, path) == 0) {
        printf("screenshot: %s\n", path);
        saved = true;
    }
    SDL_FreeSurface(s);
    return saved;
}

static void screenshot(Frontend &f, const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    unsigned w = 0, h = 0;
    if (gc_efb_frame_rgba(f.m, (uint32_t *)s->pixels, (unsigned)(s->pitch / 4), SCREEN_H, &w, &h) == 0) {
        if (w > SCREEN_W) w = SCREEN_W;
        if (h > SCREEN_H) h = SCREEN_H;
        SDL_Rect used = { 0, 0, (int)w, (int)h };
        SDL_Surface *cut = SDL_CreateRGBSurfaceWithFormat(0, (int)w, (int)h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (cut) {
            SDL_BlitSurface(s, &used, cut, NULL);
            if (SDL_SaveBMP(cut, path) == 0) printf("screenshot: %s\n", path);
            SDL_FreeSurface(cut);
        }
    }
    SDL_FreeSurface(s);
}

/* The game runs only while the menu is closed; the sound device pauses with
 * it so what is queued plays on when the game resumes. The button that
 * chose Resume is still down then, and must not reach the game. */
static void set_menu(Frontend &f, bool open)
{
    f.show_menu = open;
    gc_audio_out_pause(f.audio, open);
    if (!open) f.held_from_menu = gameplay_buttons(f);
    if (!open && f.capturing) finish_capture(f, NULL);
}

static void set_status(Frontend &f, const std::string &message)
{
    f.status = message;
    f.status_until = SDL_GetTicks() + 2500;
    printf("%s\n", message.c_str());
}

static std::string state_path(const Frontend &f, int slot)
{
    return f.state_stem + ".state" + std::to_string(slot + 1);
}

/* Every slot's header, read again. Ten small reads of 256 bytes; a menu that
 * showed a cached answer would tell a player a slot is empty after they had
 * just filled it. */
static void refresh_slots(Frontend &f)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        f.slot_present[i] = gc_state_peek_file(state_path(f, i).c_str(), &f.slot_info[i], NULL, 0) == 0;
    }
}

/* "today 14:22", "19 Sep 14:22", or nothing at all for a state saved by a
 * runtime too old to have written the time down. */
static std::string when_text(uint64_t saved_at)
{
    if (!saved_at) return "";
    const time_t when = (time_t)saved_at;
    struct tm broken, now_broken;
    char text[64];
    const time_t now = time(NULL);
#if defined(_WIN32)
    if (localtime_s(&broken, &when) != 0 || localtime_s(&now_broken, &now) != 0) return "";
#else
    if (!localtime_r(&when, &broken) || !localtime_r(&now, &now_broken)) return "";
#endif
    if (broken.tm_year == now_broken.tm_year && broken.tm_yday == now_broken.tm_yday) {
        snprintf(text, sizeof text, "today %02d:%02d", broken.tm_hour, broken.tm_min);
    } else {
        strftime(text, sizeof text, "%d %b %H:%M", &broken);
    }
    return text;
}

/* What a slot holds, in one line: the name if it has one, when it was saved,
 * and how far into the game it is. */
static std::string slot_summary(const Frontend &f, int slot)
{
    if (!f.slot_present[slot]) return "empty";
    const GcStateInfo &s = f.slot_info[slot];
    std::string text = s.name[0] ? std::string("\"") + s.name + "\"" : std::string("unnamed");
    const std::string when = when_text(s.saved_at);
    if (!when.empty()) text += ", " + when;
    text += ", field " + std::to_string((unsigned long long)s.field);
    return text;
}

static bool file_exists(const std::string &path)
{
    FILE *file = fopen(path.c_str(), "rb");
    if (file) fclose(file);
    return file != NULL;
}

static double ms_since(Uint64 started)
{
    return (SDL_GetPerformanceCounter() - started) * 1000.0 / SDL_GetPerformanceFrequency();
}

/* A save being written on its own thread. */
struct SaveJob {
    GcStateCapture *capture = nullptr;
    std::string path, slot;
    double capture_ms = 0.0, write_ms = 0.0;
    int result = -1;
    char error[512] = "";
    SDL_atomic_t done = {};
};

static int save_thread_main(void *argument)
{
    SaveJob *job = (SaveJob *)argument;
    const Uint64 started = SDL_GetPerformanceCounter();
    job->result = gc_state_write_capture(job->capture, job->path.c_str(), 1, job->error, sizeof job->error);
    job->write_ms = ms_since(started);
    gc_state_capture_free(job->capture);
    job->capture = nullptr;
    SDL_AtomicSet(&job->done, 1);
    return 0;
}

/* Reports a save that has finished writing - or waits for it, before
 * anything that reads a slot or quits. */
static void finish_save(Frontend &f, bool wait)
{
    if (!f.save_job) return;
    if (!wait && !SDL_AtomicGet(&f.save_job->done)) return;
    if (f.save_thread) SDL_WaitThread(f.save_thread, NULL);
    f.save_thread = nullptr;
    SaveJob *job = f.save_job;
    f.save_job = nullptr;
    if (job->result == 0) {
        char note[96];
        snprintf(note, sizeof note, " (%.0f ms, then %.0f ms writing)", job->capture_ms, job->write_ms);
        set_status(f, "Saved state to " + job->slot + note);
        refresh_slots(f);
    } else {
        set_status(f, "Could not save to " + job->slot + ": " + job->error);
    }
    delete job;
}

/* States are taken here, between fields, on the thread that runs the
 * machine - which is the only place one can be: nothing is half done. The
 * copy is all the game waits for; compressing it and writing it happen on a
 * thread of their own, because a fifth of a second of that is a stall the
 * sound device hears. */
/* Writes the state the dialog was opened for. The name goes into the file's
 * header, so it travels with the state rather than living in a list beside
 * it that a copy would leave behind. */
static void write_state_now(Frontend &f)
{
    finish_save(f, true);
    const Uint64 started = SDL_GetPerformanceCounter();
    SaveJob *job = new SaveJob;
    job->slot = "slot " + std::to_string(f.slot + 1);
    job->path = state_path(f, f.slot);
    job->capture = gc_state_capture(f.m, &f.state_id);
    gc_state_capture_set_name(job->capture, f.save_name);
    job->capture_ms = ms_since(started);
    if (!job->capture) {
        set_status(f, "Could not save to " + job->slot + ": out of memory");
        delete job;
        return;
    }
    if (f.selftest) {
        f.selftest_saved_hash = gc_state_hash(f.m);
        f.selftest_saved_field = f.m->field;
    }
    f.save_job = job;
    f.save_thread = SDL_CreateThread(save_thread_main, "gc-state-save", job);
    if (!f.save_thread) {
        save_thread_main(job); /* no thread to be had: write it here */
        finish_save(f, true);
    }
}

/* F5 opens this rather than writing at once: a slot that already holds
 * something says so before it is replaced, and a state can be given a name
 * while the moment it belongs to is still on the screen. Enter saves, so the
 * quick way is still two keys. */
static void set_save_dialog(Frontend &f, bool open)
{
    f.save_dialog = open;
    gc_audio_out_pause(f.audio, open || f.show_menu);
    if (!open) f.held_from_menu = gameplay_buttons(f);
}

static void open_save_dialog(Frontend &f)
{
    refresh_slots(f);
    /* Saving over a state keeps its name unless the player changes it: the
     * usual reason to save again is that the same place moved on. */
    snprintf(f.save_name, sizeof f.save_name, "%s",
             f.slot_present[f.slot] ? f.slot_info[f.slot].name : "");
    f.save_dialog_focus = true;
    set_save_dialog(f, true);
}

static void load_state_file(Frontend &f, const std::string &path, const std::string &what)
{
    char error[512];
    finish_save(f, true);
    const Uint64 started = SDL_GetPerformanceCounter();
    if (gc_state_load_file(f.m, &f.state_id, path.c_str(), error, sizeof error) == 0) {
        char note[64];
        snprintf(note, sizeof note, " (%.0f ms)", ms_since(started));
        set_status(f, "Loaded state from " + what + note);
        f.audio_restart = true;
        /* The state's console is the one the state's game was started on. */
        {
            const bool was = f.wide_now;
            read_picture_shape(f);
            if (f.wide_now != was && f.win) apply_window_scale(f);
        }
        if (f.selftest) {
            f.selftest_loaded_hash = gc_state_hash(f.m);
            f.selftest_loaded_field = f.m->field;
        }
    } else {
        set_status(f, "Could not load " + what + ": " + error);
    }
}

static void load_state(Frontend &f)
{
    finish_save(f, true);
    const std::string number = std::to_string(f.slot + 1);
    const std::string path = state_path(f, f.slot);
    refresh_slots(f);
    if (!f.slot_present[f.slot]) set_status(f, "Slot " + number + " is empty");
    else {
        const char *name = f.slot_info[f.slot].name;
        load_state_file(f, path, name[0] ? "slot " + number + " (\"" + name + "\")" : "slot " + number);
    }
}

static void delete_state(Frontend &f)
{
    finish_save(f, true);
    const std::string path = state_path(f, f.slot), slot = "Slot " + std::to_string(f.slot + 1);
    if (!file_exists(path)) set_status(f, slot + " is already empty");
    else if (remove(path.c_str()) == 0) {
        set_status(f, slot + " deleted");
        refresh_slots(f);
    }
    else set_status(f, "Could not delete " + path);
}

static void select_slot(Frontend &f, int slot)
{
    f.slot = (slot + SLOT_COUNT) % SLOT_COUNT;
    refresh_slots(f);
    set_status(f, "Slot " + std::to_string(f.slot + 1) + ": " + slot_summary(f, f.slot));
    save_prefs(f);
}

static void request_quit(void)
{
    SDL_Event quit = {};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
}

/* Fires shortcuts pressed since the last call and updates fast forward.
 * Runs once per loop, after events, whether or not the game is paused. */
static void run_shortcuts(Frontend &f)
{
    const Uint8 *kb = SDL_GetKeyboardState(NULL);
    const uint8_t mods = gc_mods_from_sdl(SDL_GetModState());
    /* While the save dialog is up the keyboard is typing a name, so a key
     * bound to mute or fast forward is a letter and nothing else. Taking
     * this as a baseline consumes the presses without acting on them, the
     * same as it does while a binding is being captured. */
    const bool baseline = f.input_resync || f.capturing || f.save_dialog;
    f.input_resync = false;
    for (int a = GC_ACTION_GAMEPLAY_COUNT; a < GC_ACTION_COUNT; a++) {
        const bool held = action_held(f, a, kb, mods);
        const bool pressed = (held || f.action_tapped[a]) && !f.action_down[a];
        f.action_down[a] = held;
        f.action_tapped[a] = false;
        if (!pressed || baseline) continue;
        switch (a) {
        case GC_ACTION_SAVE_STATE: open_save_dialog(f); break;
        case GC_ACTION_LOAD_STATE: load_state(f); break;
        case GC_ACTION_PREVIOUS_SLOT: select_slot(f, f.slot - 1); break;
        case GC_ACTION_NEXT_SLOT: select_slot(f, f.slot + 1); break;
        case GC_ACTION_OVERLAY:
            f.show_overlay = !f.show_overlay;
            save_prefs(f);
            break;
        case GC_ACTION_FPS:
            f.show_fps = !f.show_fps;
            save_prefs(f);
            break;
        case GC_ACTION_MUTE:
            f.muted = !f.muted;
            save_prefs(f);
            break;
        case GC_ACTION_MENU: set_menu(f, !f.show_menu); break;
        case GC_ACTION_FULLSCREEN: set_fullscreen(f, !f.fullscreen); break;
        case GC_ACTION_SCREENSHOT:
            /* What the window shows: the larger picture when one is on
             * screen, the console-sized one otherwise. */
            if (!screenshot_larger(f, f.shot_path.c_str())) screenshot(f, f.shot_path.c_str());
            break;
        default: break;
        }
    }
    f.fast = (f.action_down[GC_ACTION_FAST_FORWARD] && !f.capturing) || f.unthrottled;
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
    return gc_audio_out_queued_ms(f.audio);
}

/* The first binding of an action, for hints such as "Overlay (F1)":
 * keyboard, else controller; empty when unbound. */
static std::string shortcut_hint(const Frontend &f, int action)
{
    for (int device = 0; device < 2; device++) {
        for (int s = 0; s < 2; s++) {
            const GcBinding &b = (device == 0 ? f.map.keyboard : f.map.controller)[action][s];
            if (b.kind != GC_BIND_NONE) return gc_binding_label(b, f.platform, f.pad_profile);
        }
    }
    return std::string();
}

/* A line under a setting that says what it is doing now, in the grey of the
 * help texts and wrapped to the menu's width, which changes with the
 * window. */
static void menu_note(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabledV(format, arguments);
    ImGui::PopTextWrapPos();
    va_end(arguments);
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
        ImGui::TextUnformatted(gc_action_label(a));
        for (int s = 0; s < 2; s++) {
            ImGui::TableNextColumn();
            ImGui::PushID(s);
            const GcBinding b = (keyboard ? f.map.keyboard : f.map.controller)[a][s];
            const bool waiting =
                f.capturing && f.capture_keyboard == keyboard && f.capture_action == a && f.capture_slot == s;
            const std::string label =
                (waiting ? std::string("Press...") : gc_binding_label(b, f.platform, f.pad_profile)) + "###bind";
            float width = ImGui::GetContentRegionAvail().x - clear_width - style.ItemSpacing.x;
            if (ImGui::Button(label.c_str(), ImVec2(width > 40.0f ? width : 40.0f, 0.0f)))
                start_capture(f, keyboard, a, s);
            ImGui::SameLine();
            ImGui::BeginDisabled(b.kind == GC_BIND_NONE);
            if (ImGui::Button("Clear")) {
                GcBinding none = { GC_BIND_NONE, 0, 0 };
                assign_binding(f, keyboard, a, s, none);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/* The save dialog. Small, centred, and modal in the only sense that
 * matters here: the game is paused behind it and the keyboard belongs to
 * it. */
static void draw_save_dialog(Frontend &f)
{
    ImGuiIO &io = ImGui::GetIO();
    const float ui_scale = io.FontGlobalScale;
    const std::string number = std::to_string(f.slot + 1);
    bool save_now = false;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f * ui_scale, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * ui_scale, 16.0f * ui_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f * ui_scale, 8.0f * ui_scale));
    /* A fixed width and a height that follows the content: SetNextWindowSize
     * takes zero on an axis as "fit", and AlwaysAutoResize would throw the
     * width away with it. */
    ImGui::Begin("Save State", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("Save to slot %s", number.c_str());
    ImGui::Separator();
    if (f.slot_present[f.slot]) {
        /* The warning is the point of this dialog: ten slots fill up, and
         * the one thing a player must not do by accident is write over the
         * place they were keeping. */
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
        ImGui::TextWrapped("Slot %s already holds %s.", number.c_str(), slot_summary(f, f.slot).c_str());
        ImGui::TextWrapped("Saving replaces it. This cannot be undone.");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Slot %s is empty.", number.c_str());
    }
    ImGui::Separator();
    ImGui::TextDisabled("Name (optional)");
    if (f.save_dialog_focus) {
        ImGui::SetKeyboardFocusHere();
        f.save_dialog_focus = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##statename", f.save_name, sizeof f.save_name,
                         ImGuiInputTextFlags_EnterReturnsTrue))
        save_now = true;
    ImGui::TextDisabled("Enter saves, Escape cancels.");
    ImGui::Separator();
    if (ImGui::Button(f.slot_present[f.slot] ? "Replace" : "Save")) save_now = true;
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) set_save_dialog(f, false);
    ImGui::SameLine();
    ImGui::TextDisabled("F6 / F7 change slot from the menu");
    ImGui::End();
    ImGui::PopStyleVar(2);

    if (save_now) {
        set_save_dialog(f, false);
        write_state_now(f);
    }
}

/* Under a setting the command line chose for this run, which is how the
 * hub's own settings reach a game: the menu shows that choice as if it were
 * the setting, and the saved one is not otherwise to be seen. Choosing in
 * the menu replaces both, and the line goes. */
static void this_run_note(const char *saved)
{
    menu_note("Chosen for this run by whatever started the game; the saved setting is %s.", saved);
}

/* Under Internal Resolution: how the lines that were drawn meet the lines
 * the picture is shown on. One to one is as sharp as the setting gets; more
 * drawn than shown is fitted by one smooth sample, so 4x on a screen that 3x
 * fills is softer than 3x and not sharper; and where a setting would be one
 * to one at this size, it is named. Nothing while the larger picture is
 * still to come (the Picture line says so). */
static void draw_fit_note(const Frontend &f)
{
    const int scale = f.gl_on ? f.gl_scale : 1;
    const int drawn_w = f.big_on ? f.big_w : (int)f.frame_width;
    const int drawn = f.big_on ? f.big_h : (scale > 1 ? 0 : (int)f.frame_height);
    const int shown = f.viewport.h;
    if (drawn <= 0 || shown <= 0) return;
    /* The setting whose picture has as many lines as are shown, if one has. */
    char exact[64] = "";
    const double at = (double)shown * scale / drawn;
    const int whole = (int)(at + 0.5);
    if (whole >= 1 && whole <= INTERNAL_SCALE_MAX && whole != scale && at - whole > -0.005 && at - whole < 0.005)
        snprintf(exact, sizeof exact, " %dx fits this size exactly.", whole);
    if (drawn == shown && drawn_w == f.viewport.w) {
        menu_note("Shown pixel for pixel (%d x %d).", drawn_w, drawn);
    } else if (drawn == shown) {
        menu_note("Shown line for line (%d lines); across, %d pixels are fitted to %d.", drawn, drawn_w, f.viewport.w);
    } else if (drawn > shown) {
        menu_note("Scaled down to fit: %d lines drawn, %d shown, which %s.%s", drawn, shown,
                  shown * 2 < drawn ? "skips lines" : "is softer", exact);
    } else {
        menu_note("Scaled up to fit: %d lines drawn, %d shown.%s", drawn, shown, exact);
    }
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
    ImGui::TextDisabled("GameCube Settings");
    ImGui::SameLine();
    if (ImGui::Button("Resume")) set_menu(f, false);
    ImGui::Separator();
    ImGui::TextWrapped("%s", gc_menu_hint(f.platform));
    ImGui::TextWrapped("The game is paused while the menu is open.");
    ImGui::Separator();
    /* Flattened so arrow keys and the D-pad move into the settings and
     * scroll them, not just between Resume and the footer buttons. */
    ImGui::BeginChild("SettingsScroll", ImVec2(0.0f, -footer_height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NavFlattened);

    ImGui::Text("Performance: %.1f fields per second", f.fps);
    ImGui::Text("Drawn so far: %llu triangles, %llu copies", (unsigned long long)f.m->gx.triangles,
                (unsigned long long)f.m->efb.copies);

    int window_w = 0, window_h = 0;
    SDL_GetWindowSize(f.win, &window_w, &window_h);
    ImGui::Separator();
    ImGui::TextDisabled("Graphics");
    ImGui::Text("Window: %d x %d", window_w, window_h);
    /* At an internal resolution above 1x there is no larger picture of the
     * frame on screen until the game has drawn one: the renderer was just
     * restarted for a setting changed here, or a state was loaded, and the
     * game is paused. The frame on screen is then the console's own, and
     * the line says what comes next; left at the size alone it read as if
     * the setting had not taken. */
    if (f.big_on) {
        ImGui::Text("Picture: %d x %d (drawn at %dx)", f.big_w, f.big_h, f.gl_scale);
    } else if (f.gl_on && f.gl_scale > 1) {
        ImGui::TextWrapped("Picture: %u x %u for the frame on screen; drawn at %dx from the next frame the game draws",
                           f.frame_width, f.frame_height, f.gl_scale);
    } else {
        ImGui::Text("Picture: %u x %u (the console's own size)", f.frame_width, f.frame_height);
    }
    ImGui::Text("Viewport: %d x %d (%.2fx)", f.viewport.w, f.viewport.h, (double)f.viewport.h / SCREEN_H);
    /* Marked beta because whether it works is the game's business and not
     * ours: the mechanism is sound, the outcome is per-game, and a player
     * has to look at the result to find out. Anything else here either
     * works everywhere or is off. */
    if (ImGui::Checkbox("Double video rate", &f.vi_double)) {
        gc_vi_double = f.vi_double ? 1 : 0;
        save_prefs(f);
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
    ImGui::TextUnformatted("BETA");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Beta: works on some games and not others, and the game decides which.\n\n"
                        "Runs the console's video interface twice as fast, so a game that draws\n"
                        "every other field draws every field. A game that reads the clock then\n"
                        "plays at the right speed at sixty pictures a second. A game that counts\n"
                        "frames instead plays at double speed - Shrek 2 is one of those.\n\n"
                        "Turn it on and watch for a few seconds. If the game runs fast, turn it\n"
                        "off. The setting is shared by every GameCube game, so turn it off before\n"
                        "going back to one it does not suit.");
    {
        int renderer = wanted_renderer(f);
        if (ImGui::Combo("Renderer", &renderer, k_renderer_names, IM_ARRAYSIZE(k_renderer_names))) {
            f.renderer = renderer;
            f.renderer_this_run = -1;
            apply_renderer(f);
            save_prefs(f);
            set_status(f, std::string("Renderer: ") + renderer_name(f));
        }
        ImGui::TextDisabled("Software and OpenGL draw the same picture; OpenGL moves the drawing to the\n"
                            "graphics card. OpenGL (fast) lets the card rasterise and blend as well: much\n"
                            "quicker, and an edge can be a pixel out or a blend a shade out.");
        if (!f.gl_note.empty()) ImGui::TextWrapped("%s", f.gl_note.c_str());
        if (f.renderer_this_run >= 0 && f.renderer_this_run != f.renderer) this_run_note(k_renderer_names[f.renderer]);
        if (wanted_renderer(f) == RENDERER_OPENGL_FAST) {
            int index = (f.internal_scale_this_run > 0 ? f.internal_scale_this_run : f.internal_scale) - 1;
            if (ImGui::Combo("Internal Resolution", &index, k_internal_scale_names,
                             IM_ARRAYSIZE(k_internal_scale_names))) {
                f.internal_scale = index + 1;
                f.internal_scale_this_run = 0;
                apply_renderer(f);
                save_prefs(f);
                set_status(f, std::string("Internal resolution: ") + k_internal_scale_names[f.gl_scale - 1]);
            }
            ImGui::TextDisabled("Drawn larger on the graphics card and shown at that size: sharper edges and\n"
                                "finer texture detail, for more of the card's time. The game itself still sees\n"
                                "the console's own size. Flat art - the HUD, text, menus - keeps the game's own\n"
                                "resolution; a texture pack is what replaces it.");
            if (f.internal_scale_this_run > 0 && f.internal_scale_this_run != f.internal_scale)
                this_run_note(k_internal_scale_names[f.internal_scale - 1]);
            draw_fit_note(f);
            int samples = anisotropy_index(f.anisotropy_this_run > 0 ? f.anisotropy_this_run : f.anisotropy);
            if (ImGui::Combo("Anisotropic Filtering", &samples, k_anisotropy_names, IM_ARRAYSIZE(k_anisotropy_names))) {
                f.anisotropy = k_anisotropy_values[samples];
                f.anisotropy_this_run = 0;
                apply_renderer(f);
                save_prefs(f);
                set_status(f, std::string("Anisotropic filtering: ") + k_anisotropy_names[anisotropy_index(f.gl_anisotropy)]);
            }
            ImGui::TextDisabled("Floors, walls and roads seen at a slant keep more of their detail into the\n"
                                "distance. Off filters them as the other renderers do.");
            if (f.anisotropy_this_run > 0 &&
                k_anisotropy_values[anisotropy_index(f.anisotropy_this_run)] != f.anisotropy)
                this_run_note(k_anisotropy_names[anisotropy_index(f.anisotropy)]);
        } else {
            ImGui::TextDisabled("Internal Resolution and Anisotropic Filtering are for OpenGL (fast).");
        }
    }
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
    /* The larger picture's texture is always fitted smoothly (show_larger). */
    if (f.big_on) menu_note("For the console-sized picture only: one drawn larger is always fitted smoothly.");
    if (!f.fullscreen) {
        int scale_index = f.scale - 1;
        if (ImGui::Combo("Window Size", &scale_index, k_scale_names, IM_ARRAYSIZE(k_scale_names))) {
            f.scale = scale_index + 1;
            apply_window_scale(f);
            save_prefs(f);
        }
        const int fitted = fitted_window_scale(f);
        if (fitted < f.scale) {
            menu_note("%s does not fit this screen: choosing it gives %s. Fullscreen uses the whole screen.",
                      k_scale_names[f.scale - 1], k_scale_names[fitted - 1]);
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
    const std::string fast_forward = shortcut_hint(f, GC_ACTION_FAST_FORWARD);
    if (!fast_forward.empty()) ImGui::TextDisabled("Hold %s to fast forward at full speed.", fast_forward.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Audio");
    if (ImGui::Checkbox("Mute", &f.muted)) save_prefs(f);
    if (ImGui::SliderInt("Master Volume (%)", &f.volume, 0, VOLUME_MAX)) save_prefs(f);
    if (f.audio) {
        GcAudioOutStats st;
        gc_audio_out_stats(f.audio, &st);
        ImGui::Text("Output: %s, %d Hz, %d-frame device buffer", st.driver, st.device_rate, st.device_frames);
        ImGui::Text("Queued: %.0f ms (holding %.0f ms), ran dry %llu times", audio_queue_ms(f), st.target_ms,
                    (unsigned long long)st.underruns);
        ImGui::Text("Frames the machine produced that nobody took: %llu",
                    (unsigned long long)gc_audio_overruns(f.m));
    } else {
        ImGui::TextDisabled("No audio output device is available.");
    }
    if (f.speed != 100) ImGui::TextDisabled("Audio output pauses while Speed %% is not 100.");

    ImGui::Separator();
    ImGui::TextDisabled("Savestates");
    /* Read once for the whole section, so the list, the buttons and the
     * status line all say the same thing. */
    refresh_slots(f);
    int slot = f.slot + 1;
    if (ImGui::SliderInt("Active Slot", &slot, 1, SLOT_COUNT)) {
        f.slot = clamp_int(slot - 1, 0, SLOT_COUNT - 1);
        save_prefs(f);
    }
    /* Every slot by name, so a slot is chosen by what is in it rather than
     * by remembering which number it was. */
    for (int i = 0; i < SLOT_COUNT; i++) {
        char label[160];
        snprintf(label, sizeof label, "%d. %s###slot%d", i + 1, slot_summary(f, i).c_str(), i);
        if (ImGui::RadioButton(label, f.slot == i) && f.slot != i) {
            f.slot = i;
            save_prefs(f);
        }
    }
    const std::string save_hint = shortcut_hint(f, GC_ACTION_SAVE_STATE);
    const std::string load_hint = shortcut_hint(f, GC_ACTION_LOAD_STATE);
    const std::string save_label =
        (save_hint.empty() ? std::string("Save State") : "Save State (" + save_hint + ")") + "###save";
    const std::string load_label =
        (load_hint.empty() ? std::string("Load State") : "Load State (" + load_hint + ")") + "###load";
    if (ImGui::Button(save_label.c_str())) {
        set_menu(f, false);
        open_save_dialog(f);
    }
    ImGui::SameLine();
    if (ImGui::Button(load_label.c_str())) load_state(f);
    ImGui::SameLine();
    if (ImGui::Button("Delete Slot")) delete_state(f);
    const std::string slot_file = state_path(f, f.slot);
    ImGui::TextWrapped("Slot File: %s", slot_file.c_str());
    ImGui::Text("Slot Status: %s", file_exists(slot_file) ? "Present" : "Empty");
    ImGui::TextDisabled("A state includes the memory card: loading one puts the card back as it was then.");
    ImGui::TextDisabled("Saving asks before it replaces a slot, and a state can be given a name.");

    ImGui::Separator();
    ImGui::TextDisabled("Memory Card");
    if (gc_card_present(f.m)) {
        ImGui::TextWrapped("Card: %s", f.card_path.c_str());
        ImGui::TextDisabled("The card is written back a moment after the game stops writing to it, so a save "
                            "survives whatever happens next.");
    } else {
        ImGui::TextDisabled("No memory card. A game asked to save will say so.");
    }
    if (!f.status.empty()) ImGui::TextWrapped("%s", f.status.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Controls");
    static const char *const platform_names[] = { "Windows", "Mac", "Steam Deck" };
    ImGui::Text("Default layout: %s", platform_names[f.platform]);
    if (f.pad) {
        const char *name = SDL_GameControllerName(f.pad);
        ImGui::Text("Detected Controller: %s", name ? name : "(unnamed)");
        ImGui::TextDisabled("The sticks and triggers follow the controller's own and are not bindable: the left "
                            "stick is the stick, the right is the C-stick.");
    } else {
        ImGui::TextDisabled("No controller detected.");
    }
    ImGui::TextWrapped("Choose a binding, then press the new key or button. Escape or Guide cancels. Hold %s with a "
                       "key to bind a combination.",
                       f.platform == GC_PLATFORM_MAC ? "Command, Control, Option or Shift" : "Ctrl, Alt or Shift");
    if (f.capturing) {
        ImGui::Text("Waiting for a %s for %s...", f.capture_keyboard ? "key" : "controller button",
                    gc_action_label(f.capture_action));
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) finish_capture(f, NULL);
    }
    if (!f.bind_note.empty()) ImGui::TextWrapped("%s", f.bind_note.c_str());
    if (gc_console_is_wii()) {
        /* What the game is played with, and what that makes of the controls
         * below, which keep their GameCube names. */
        static const char *const k_wii_style_notes[] = {
            "A, B and the D-Pad are the remote's. X = 1, Y = 2, Start = +. The stick is the Nunchuk's; L = Z and "
            "R = C. The mouse (or the right stick) is the pointer, and its buttons are A and B.",
            "The remote on its side. B = 1 and A = 2; X = A, Y = B; Start = +. The D-Pad and the stick are the "
            "remote's D-Pad.",
            "A Classic Controller: sticks, triggers and buttons straight across, Z = ZR, Start = +. The mouse is "
            "still the remote's pointer.",
            "The game is played on a GameCube controller in port 1. The remote is on, with the mouse as its "
            "pointer.",
        };
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("Played with", &f.wii_style, k_wii_style_names, WII_STYLE_COUNT)) save_prefs(f);
        ImGui::TextWrapped("%s", k_wii_style_notes[f.wii_style]);
        if (ImGui::Checkbox("Widescreen (16:9)", &f.wii_wide)) save_prefs(f);
        ImGui::TextWrapped("The console's television setting, which a game reads when it starts: it takes effect "
                           "the next time this game is started. Now: %s.", f.wide_now ? "16:9" : "4:3");
    }
    draw_binding_table(f, "Keyboard Gameplay", true, 0, GC_ACTION_GAMEPLAY_COUNT);
    draw_binding_table(f, "Controller Gameplay", false, 0, GC_ACTION_GAMEPLAY_COUNT);
    if (gc_console_is_wii()) {
        draw_binding_table(f, "Keyboard Wii Remote", true, GC_ACTION_SHORTCUT_COUNT, GC_ACTION_COUNT);
        draw_binding_table(f, "Controller Wii Remote", false, GC_ACTION_SHORTCUT_COUNT, GC_ACTION_COUNT);
    }
    draw_binding_table(f, "Keyboard Shortcuts", true, GC_ACTION_GAMEPLAY_COUNT, GC_ACTION_SHORTCUT_COUNT);
    draw_binding_table(f, "Controller Shortcuts", false, GC_ACTION_GAMEPLAY_COUNT, GC_ACTION_SHORTCUT_COUNT);
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
        ImGui::Text("Fields per second: %.1f", f.fps);
        ImGui::Text("Renderer: %s", renderer_name(f));
        ImGui::TextUnformatted(gc_menu_hint(f.platform));
        const std::string overlay = shortcut_hint(f, GC_ACTION_OVERLAY);
        if (!overlay.empty()) ImGui::Text("Use %s for Overlay", overlay.c_str());
        ImGui::Text("Viewport: %d x %d (%s)", f.viewport.w, f.viewport.h, k_scaling_names[f.scaling]);
        ImGui::Text("Speed: %s", f.fast ? "fast forward" : (std::to_string(f.speed) + "%").c_str());
        if (f.audio) ImGui::Text("Audio queued: %.0f ms", audio_queue_ms(f));
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
        if (f.fps > 0.0) ImGui::Text("%.0f FPS  %.0f%%", f.fps, f.fps * FIELD_SECONDS * 100.0);
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

/* Presents in a row that may keep the picture before, while the buffer on
 * screen is still being drawn, before the next one waits for it. */
enum { PRESENTS_KEPT_MAX = 1 };

/* `deadline` is when the next field is due by the clock (seconds on the
 * performance counter), or 0 when nothing paces the run. */
/* Puts the larger picture of the buffer on screen into its texture, when
 * there is one (an internal resolution above 1x, and a display copy made by
 * the card). The display buffer's own picture has just been read, so the
 * copy that filled it has landed and its larger picture with it. */
static void show_larger(Frontend &f)
{
    unsigned w = 0, h = 0;
    uint64_t serial = 0;
    f.big_on = false;
    if (!f.gl_on || f.gl_scale <= 1 || !f.ren || !f.place_ok) return;
    /* By the place present_take read, not by asking the machine again: it
     * may be running its next field by now. */
    if (gc_gl_picture_at(f.m, f.place_first, f.place_width, f.place_height, NULL, 0, 0, &w, &h, &serial) != 0) return;
    if (!f.big || (int)w != f.big_w || (int)h != f.big_h) {
        if (f.big) SDL_DestroyTexture(f.big);
        f.big = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
        f.big_w = (int)w;
        f.big_h = (int)h;
        f.big_serial = 0;
        if (!f.big) return;
        /* Usually larger than the window it is shown in. */
        SDL_SetTextureScaleMode(f.big, SDL_ScaleModeLinear);
    }
    if (serial != f.big_serial) {
        void *pixels = NULL;
        int pitch = 0;
        if (SDL_LockTexture(f.big, NULL, &pixels, &pitch) != 0) return;
        const int got = gc_gl_picture_at(f.m, f.place_first, f.place_width, f.place_height, (uint32_t *)pixels,
                                         (unsigned)(pitch / 4), (unsigned)f.big_h, &w, &h, &serial);
        SDL_UnlockTexture(f.big);
        /* A picture of another size arrived between the two questions: the
         * next present makes a texture for it. */
        if (got != 0 || (int)w != f.big_w || (int)h != f.big_h) return;
        f.big_serial = serial;
    }
    f.big_on = true;
}

/* A present in two halves.
 *
 * present_take reads the machine: whether the buffer on screen has all its
 * bytes, the buffer itself into the console-sized texture, where it is. The
 * machine must not be running. present_show is the rest - the larger
 * picture into its texture, the frame drawn, the menu, the swap - and reads
 * nothing of the machine while the game is not paused, so it can be done
 * while the next field runs (run_field, the main loop). That rest is most
 * of what a present costs, and all of what an internal resolution adds to
 * it: 0.5 ms a field at 1x and 2.2 ms at 3x, which a scene that takes the
 * machine nearly the whole field has not got. */
static void present_take(Frontend &f, double deadline)
{
    void *pixels = NULL;
    int pitch = 0;
    /* Under OpenGL, and with the graphics thread, a display copy reaches
     * memory a moment after the field that made it, on a thread of its own;
     * reading the buffer now would wait for it. So when its bytes are not all
     * there, the present waits for them for the time the field would sleep
     * anyway, and past that the picture before stays up - one field later on
     * screen - for PRESENTS_KEPT_MAX presents in a row; after that it waits
     * for the buffer however long that takes.
     *
     * Before that bound the window kept the picture before for as long as
     * every present found the buffer on screen unfinished - which is every
     * present, while drawing trails the machine, for a game that copies a
     * frame into a buffer and points the video interface at it in the same
     * field: the black of a loading screen stayed up for three seconds of
     * Shrek 2's opening cutscene after the storybook was skipped, drawing in
     * software on the graphics thread (docs/GC-PLAN.md, "A picture that
     * stayed black"). Keeping two presents instead of one was measured too:
     * fewer waits unthrottled, more when paced, and pictures four fields
     * late. */
    f.presents++;
    bool ready = true;
    if ((f.gl_on || gc_gx_thread_active(f.m)) && !gc_efb_display_ready(f.m)) {
        const double freq = (double)SDL_GetPerformanceFrequency();
        const double started = (double)SDL_GetPerformanceCounter() / freq;
        bool wait = false;
        double budget_ms = -1.0; /* no limit */
        if (f.kept_run >= PRESENTS_KEPT_MAX) {
            wait = true;
        } else if (deadline > 0.0) {
            /* Leave a couple of milliseconds for converting and presenting. */
            budget_ms = (deadline - started) * 1000.0 - 2.0;
            wait = budget_ms > 0.0;
        }
        ready = false;
        if (wait) {
            ready = gc_efb_display_wait(f.m, budget_ms) != 0;
            f.present_waits++;
            f.present_wait_ms += ((double)SDL_GetPerformanceCounter() / freq - started) * 1000.0;
        }
    }
    if (!ready) {
        pixels = NULL;
        f.presents_kept++;
        if (++f.kept_run > f.kept_run_max) f.kept_run_max = f.kept_run;
    } else if (SDL_LockTexture(f.tex, NULL, &pixels, &pitch) == 0) {
        unsigned w = 0, h = 0;
        f.kept_run = 0;
        f.shown_source = f.m->field;
        if (gc_efb_frame_rgba(f.m, (uint32_t *)pixels, (unsigned)(pitch / 4), SCREEN_H, &w, &h) != 0) {
            memset(pixels, 0, (size_t)pitch * SCREEN_H);
        } else {
            f.frame_width = w < SCREEN_W ? w : SCREEN_W;
            f.frame_height = h < SCREEN_H ? h : SCREEN_H;
        }
        if (f.shown) {
            for (int y = 0; y < SCREEN_H; y++)
                memcpy(f.shown + y * SCREEN_W, (uint8_t *)pixels + (size_t)y * (size_t)pitch, SCREEN_W * 4u);
        }
        SDL_UnlockTexture(f.tex);
        f.place_ok = gc_efb_display_place(f.m, &f.place_first, &f.place_width, &f.place_height) == 0;
        f.larger_due = true;
    }
    if (f.shown && f.shown_log) {
        /* The picture, not the texture's rows beyond it; and the field whose
         * buffer it is. */
        uint64_t h = 1469598103934665603ull;
        for (unsigned y = 0; y < f.frame_height; y++) {
            const uint8_t *p = (const uint8_t *)(f.shown + (size_t)y * SCREEN_W);
            for (size_t i = 0, n = (size_t)f.frame_width * 4u; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
        }
        fprintf(f.shown_log, "%llu %016llx %llu %d %u %u\n", (unsigned long long)f.m->field, (unsigned long long)h,
                (unsigned long long)f.shown_source, ready ? 0 : 1, f.frame_width, f.frame_height);
    }
    if (f.shown && f.shown_every > 0 && f.m->field % (uint64_t)f.shown_every == 0) {
        unsigned w = f.frame_width ? f.frame_width : SCREEN_W, h = f.frame_height ? f.frame_height : SCREEN_H;
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(f.shown, (int)w, (int)h, 32, SCREEN_W * 4,
                                                            SDL_PIXELFORMAT_ARGB8888);
        if (s) {
            char path[1200];
            snprintf(path, sizeof path, "%s-%06llu.bmp", f.shown_prefix.c_str(), (unsigned long long)f.m->field);
            SDL_SaveBMP(s, path);
            SDL_FreeSurface(s);
        }
    }
}

static void present_show(Frontend &f)
{
    const Uint64 started = SDL_GetPerformanceCounter();
    f.show_pending = false;
    if (f.larger_due) {
        f.larger_due = false;
        show_larger(f);
    }
    SDL_Rect used = { 0, 0, (int)(f.frame_width ? f.frame_width : SCREEN_W),
                      (int)(f.frame_height ? f.frame_height : SCREEN_H) };
    SDL_SetRenderDrawColor(f.ren, 0, 0, 0, 255);
    SDL_RenderClear(f.ren);
    if (f.big_on && f.big) SDL_RenderCopy(f.ren, f.big, NULL, &f.viewport);
    else SDL_RenderCopy(f.ren, f.tex, &used, &f.viewport);

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.FontGlobalScale = ui_scale_for_size(io.DisplaySize);
    if (f.save_dialog) draw_save_dialog(f);
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
    {
        const double took = (double)(SDL_GetPerformanceCounter() - started) / (double)SDL_GetPerformanceFrequency();
        f.show_cost += (took - f.show_cost) * 0.1;
    }
}

/* ------------------------------------------------------------------ */
/* The machine's own thread                                            */
/* ------------------------------------------------------------------ */

/* A field is run on a thread of its own so that the window's thread can put
 * the picture before on screen meanwhile. The two never have the machine at
 * once: the window's thread starts a field (run_field_start), does only
 * what present_show does, and waits for the field to end (run_field_wait)
 * before it touches the machine again - input, sound, save states, the
 * menu and the first half of a present are all between fields, as they
 * always were. GC_SDL_RUN_INLINE=1 runs the field on the window's thread
 * instead, as before; so does a thread that would not start. */
struct FieldRunner {
    SDL_Thread *thread = NULL;
    SDL_sem *start = NULL, *finished = NULL;
    SDL_atomic_t quit;
    GcMachine *m = NULL;
    const GcSdlProgram *program = NULL;
    bool interpreter_only = false;
    uint64_t until = 0;
    bool running = false;
};

static void run_one_field(FieldRunner &r)
{
    gc_perf_field_begin(r.m); /* --field-log: this thread's processor time */
    if (r.program && r.program->run && !r.interpreter_only) r.program->run(r.m, r.until);
    else gc_machine_run(r.m, r.until);
    gc_perf_field_end(r.m);
}

static int field_runner_main(void *argument)
{
    FieldRunner &r = *(FieldRunner *)argument;
    for (;;) {
        SDL_SemWait(r.start);
        if (SDL_AtomicGet(&r.quit)) break;
        run_one_field(r);
        SDL_SemPost(r.finished);
    }
    return 0;
}

static void field_runner_open(FieldRunner &r, GcMachine *m, const GcSdlProgram *program, bool interpreter_only)
{
    const char *inline_run = getenv("GC_SDL_RUN_INLINE");
    r.m = m;
    r.program = program;
    r.interpreter_only = interpreter_only;
    SDL_AtomicSet(&r.quit, 0);
    if (inline_run && !strcmp(inline_run, "1")) return;
    r.start = SDL_CreateSemaphore(0);
    r.finished = SDL_CreateSemaphore(0);
    /* The stack the window's thread has: recompiled code and the
     * interpreter under it were sized for that. */
    if (r.start && r.finished)
        r.thread = SDL_CreateThreadWithStackSize(field_runner_main, "gc machine", 8u << 20, &r);
    if (!r.thread) fprintf(stderr, "gc: no thread for the machine; fields run on the window's thread\n");
}

/* Starts the field that ends at `until`; with no thread it runs here, now. */
static void run_field_start(FieldRunner &r, uint64_t until)
{
    r.until = until;
    if (!r.thread) {
        run_one_field(r);
        return;
    }
    r.running = true;
    SDL_SemPost(r.start);
}

static void run_field_wait(FieldRunner &r)
{
    if (!r.running) return;
    SDL_SemWait(r.finished);
    r.running = false;
}

static void field_runner_close(FieldRunner &r)
{
    run_field_wait(r);
    if (r.thread) {
        SDL_AtomicSet(&r.quit, 1);
        SDL_SemPost(r.start);
        SDL_WaitThread(r.thread, NULL);
        r.thread = NULL;
    }
    if (r.start) SDL_DestroySemaphore(r.start);
    if (r.finished) SDL_DestroySemaphore(r.finished);
    r.start = r.finished = NULL;
}

/* ------------------------------------------------------------------ */
/* Menu self-test                                                      */
/* ------------------------------------------------------------------ */

/* --menu-selftest drives the overlay menu with synthetic input through SDL's
 * event queue, as a keyboard and a controller would deliver it: Escape, then
 * the controller's Guide button, then L3 (the Steam Deck's way in). It checks
 * that each opens and closes the menu and that the game stops while the menu
 * is open, and sets the volume to 37 the way the menu's slider does. Then the
 * save-state keys: F7 and F6 step the slot, F5 opens the save dialog, the
 * dialog writes a named state, the game runs on, and F8 must bring back the
 * same field and a machine with the same state hash (states in --save-dir).
 * Last, the window's close button: a close event for the game's window has
 * to end the run by itself, with no SDL_QUIT behind it, which is how it
 * arrives while the OpenGL path has windows of its own open. A second
 * run with --menu-selftest-verify checks the setting was kept. Pass
 * --prefs FILE so the player's own settings are not touched. */
enum { SELFTEST_VOLUME = 37 };

struct MenuSelfTest {
    bool enabled = false;
    int loop = 0;
    uint64_t field_mark = 0;
    int failures = 0;
    bool closing = false; /* the close event is on its way; the verdict waits for what it does */
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

/* The window's close button as the queue carries it. Pushed here it is the
 * event alone: SDL adds its SDL_QUIT where the system reports a close, not
 * where an event is pushed. */
static void push_close(const Frontend &f)
{
    SDL_Event e = {};
    e.type = SDL_WINDOWEVENT;
    e.window.event = SDL_WINDOWEVENT_CLOSE;
    e.window.windowID = SDL_GetWindowID(f.win);
    SDL_PushEvent(&e);
}

static void selftest_check(MenuSelfTest &t, bool ok, const char *what)
{
    printf("menu selftest: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) t.failures++;
}

/* One step per loop, before events are polled; false when done. Each input
 * is given time to take effect. */
static bool menu_selftest_step(Frontend &f, MenuSelfTest &t)
{
    const GcMachine *m = f.m;
    switch (t.loop++) {
    case 30: push_key(SDL_SCANCODE_ESCAPE); break;
    case 32:
        selftest_check(t, f.show_menu, "Escape opens the menu");
        t.field_mark = m->field;
        break;
    case 62:
        selftest_check(t, m->field == t.field_mark, "the game is paused while the menu is open");
        push_key(SDL_SCANCODE_ESCAPE);
        break;
    case 64:
        selftest_check(t, !f.show_menu, "Escape closes the menu");
        t.field_mark = m->field;
        break;
    case 70: push_key(SDL_SCANCODE_F9); break;
    case 72: selftest_check(t, f.show_fps, "F9 turns the FPS counter on"); break;
    case 94:
        selftest_check(t, m->field > t.field_mark, "the game runs again");
        push_button(SDL_CONTROLLER_BUTTON_GUIDE);
        break;
    case 96:
        selftest_check(t, f.show_menu, "a controller's Guide button opens the menu");
        t.field_mark = m->field;
        break;
    case 126:
        selftest_check(t, m->field == t.field_mark, "the game is paused (opened from the controller)");
        f.volume = SELFTEST_VOLUME; /* as the menu's volume slider sets it */
        save_prefs(f);
        set_fullscreen(f, true); /* as its Fullscreen box does */
        push_button(SDL_CONTROLLER_BUTTON_GUIDE);
        break;
    case 128: selftest_check(t, !f.show_menu, "Guide closes the menu"); break;
    case 130: push_button(SDL_CONTROLLER_BUTTON_LEFTSTICK); break;
    case 132: selftest_check(t, f.show_menu, "L3 opens the menu (Steam Deck)"); break;
    case 134: push_button(SDL_CONTROLLER_BUTTON_LEFTSTICK); break;
    case 136:
        selftest_check(t, !f.show_menu, "L3 closes the menu");
        push_key(SDL_SCANCODE_F7);
        break;
    case 138:
        selftest_check(t, f.slot == 1, "F7 selects the next state slot");
        push_key(SDL_SCANCODE_F6);
        break;
    case 140:
        selftest_check(t, f.slot == 0, "F6 selects the previous state slot");
        push_key(SDL_SCANCODE_F5);
        break;
    case 150:
        /* F5 asks first now. The dialog's own Save is driven here rather
         * than by an Enter key, so the check is of what saving does and not
         * of where ImGui's keyboard focus happens to be. */
        selftest_check(t, f.save_dialog, "F5 asks before it writes a slot");
        snprintf(f.save_name, sizeof f.save_name, "%s", "selftest");
        set_save_dialog(f, false);
        write_state_now(f);
        break;
    case 152: {
        GcStateInfo info;
        finish_save(f, true); /* the file is written on its own thread */
        selftest_check(t, f.selftest_saved_hash != 0 && file_exists(state_path(f, 0)),
                       "the dialog saves a state to slot 1");
        selftest_check(t,
                       gc_state_peek_file(state_path(f, 0).c_str(), &info, NULL, 0) == 0 &&
                           strcmp(info.name, "selftest") == 0 && info.saved_at != 0,
                       "the state carries its name and the time it was saved");
        t.field_mark = m->field;
        push_key(SDL_SCANCODE_F5);
        break;
    }
    case 162:
        /* The second time, the slot is not empty. This is the warning the
         * dialog exists for, and the name it offers is the one the state it
         * is about to replace already carries. */
        selftest_check(t, f.save_dialog && f.slot_present[f.slot],
                       "the dialog knows the slot it would replace is not empty");
        selftest_check(t, strcmp(f.save_name, "selftest") == 0,
                       "and offers the name that state already has");
        push_key(SDL_SCANCODE_ESCAPE);
        break;
    case 164:
        selftest_check(t, !f.save_dialog, "Escape cancels the save without writing");
        break;
    case 172:
        selftest_check(t, m->field > f.selftest_saved_field, "the game runs on after a save");
        push_key(SDL_SCANCODE_F8);
        break;
    case 174:
        selftest_check(t, f.selftest_loaded_field == f.selftest_saved_field && f.selftest_loaded_hash ==
                              f.selftest_saved_hash,
                       "F8 loads it back: the same field and the same machine, hash for hash");
        t.field_mark = m->field;
        break;
    case 204:
        selftest_check(t, m->field > t.field_mark, "the game runs on after a load");
        /* The loop that takes this does not come back here: it ends, and
         * menu_selftest_closed gives the verdict. */
        push_close(f);
        t.closing = true;
        break;
    case 206:
        /* Still running two loops later: the close was not taken. */
        t.closing = false;
        selftest_check(t, false, "the window's close button ends the game");
        printf("menu selftest: FAIL\n");
        return false;
    default: break;
    }
    return true;
}

/* After the main loop: it ended while the self-test's close event was the
 * only thing asking it to. */
static void menu_selftest_closed(MenuSelfTest &t)
{
    if (!t.closing) return;
    selftest_check(t, true, "the window's close button ends the game");
    printf("menu selftest: %s\n", t.failures ? "FAIL" : "PASS");
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int gc_sdl_main(int argc, char **argv, const GcSdlProgram *program, const char *default_disc)
{
    const char *disc_path = NULL, *save_dir = NULL, *shot_prefix = NULL, *prefs_file = NULL, *load_path = NULL;
    const char *internal_shot = NULL; /* --internal-shot FILE.bmp: the larger picture, on exit */
    int cli_scale = 0;
    long limit_fields = -1;
    /* Wall clock rather than emulated time. A gate that says "thirty
     * seconds" means thirty seconds of somebody sitting in front of it,
     * and on a machine that does not yet run at full speed that is a
     * very different number of fields. */
    long limit_seconds = -1;
    bool cli_fullscreen = false, cli_muted = false, interpreter_only = false, audio_stats = false;
    bool selftest_verify = false;
    int cli_renderer = -1;
    int cli_internal_scale = 0;
    int cli_anisotropy = 0;
    bool cli_unthrottled = false;
    const char *cli_play = NULL;
    long cli_shown_every = 0;
    const char *cli_shown_log = NULL;
    const char *cli_gl_shaders = NULL, *cli_gl_cache = NULL;
    int cli_wii_aspect = 0; /* --wii-aspect: 1 for 4:3, 2 for 16:9 */
    MenuSelfTest selftest;
    FILE *pace_log = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--scale") && i + 1 < argc) cli_scale = atoi(argv[++i]);
        else if (!strcmp(a, "--prefs") && i + 1 < argc) prefs_file = argv[++i];
        else if (!strcmp(a, "--menu-selftest")) selftest.enabled = true;
        else if (!strcmp(a, "--menu-selftest-verify")) selftest_verify = true;
        else if (!strcmp(a, "--save-dir") && i + 1 < argc) save_dir = argv[++i];
        else if (!strcmp(a, "--load-state") && i + 1 < argc) load_path = argv[++i];
        else if (!strcmp(a, "--fullscreen")) cli_fullscreen = true;
        else if (!strcmp(a, "--mute")) cli_muted = true;
        else if (!strcmp(a, "--audio-stats")) audio_stats = true;
        else if (!strcmp(a, "--pace-log") && i + 1 < argc) pace_log = fopen(argv[++i], "w");
        else if (!strcmp(a, "--interpreter")) interpreter_only = true;
        else if (!strcmp(a, "--limit-frames") && i + 1 < argc) limit_fields = atol(argv[++i]);
        else if (!strcmp(a, "--limit-seconds") && i + 1 < argc) limit_seconds = atol(argv[++i]);
        else if (!strcmp(a, "--screenshot-prefix") && i + 1 < argc) shot_prefix = argv[++i];
        else if (!strcmp(a, "--internal-shot") && i + 1 < argc) internal_shot = argv[++i];
        else if (!strcmp(a, "--disc") && i + 1 < argc) disc_path = argv[++i];
        else if (!strcmp(a, "--gl")) cli_renderer = RENDERER_OPENGL;
        else if (!strcmp(a, "--gl-fast")) cli_renderer = RENDERER_OPENGL_FAST;
        else if (!strcmp(a, "--internal-scale") && i + 1 < argc) {
            cli_internal_scale = atoi(argv[++i]);
            if (cli_internal_scale < 1 || cli_internal_scale > INTERNAL_SCALE_MAX) {
                fprintf(stderr, "--internal-scale takes 1 to %d, not %s\n", INTERNAL_SCALE_MAX, argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--anisotropy") && i + 1 < argc) {
            cli_anisotropy = atoi(argv[++i]);
            if (cli_anisotropy < 1 || cli_anisotropy > ANISOTROPY_MAX) {
                fprintf(stderr, "--anisotropy takes 1 to %d, not %s\n", ANISOTROPY_MAX, argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--unthrottled")) cli_unthrottled = true;
        else if (!strcmp(a, "--play-input") && i + 1 < argc) cli_play = argv[++i];
        else if (!strcmp(a, "--shown-every") && i + 1 < argc) cli_shown_every = atol(argv[++i]);
        else if (!strcmp(a, "--shown-log") && i + 1 < argc) cli_shown_log = argv[++i];
        else if (!strcmp(a, "--renderer") && i + 1 < argc) {
            const char *name = argv[++i];
            if (!strcmp(name, "opengl") || !strcmp(name, "gl")) cli_renderer = RENDERER_OPENGL;
            else if (!strcmp(name, "fast") || !strcmp(name, "opengl-fast")) cli_renderer = RENDERER_OPENGL_FAST;
            else if (!strcmp(name, "software")) cli_renderer = RENDERER_SOFTWARE;
            else {
                fprintf(stderr, "--renderer takes software, opengl or fast, not %s\n", name);
                return 2;
            }
        }
        else if (!strcmp(a, "--gl-shaders") && i + 1 < argc) cli_gl_shaders = argv[++i];
        else if (!strcmp(a, "--gl-cache") && i + 1 < argc) cli_gl_cache = argv[++i];
        else if (!strcmp(a, "--wii-aspect") && i + 1 < argc) {
            const char *shape = argv[++i];
            cli_wii_aspect = !strcmp(shape, "16:9") ? 2 : (!strcmp(shape, "4:3") ? 1 : 0);
            if (!cli_wii_aspect) {
                fprintf(stderr, "--wii-aspect takes 4:3 or 16:9, not %s\n", shape);
                return 2;
            }
        }
        else if (!strcmp(a, "--field-log") && i + 1 < argc) {
            /* Measurement: a line a field of where the time went (gc_perf.h). */
            if (gc_perf_field_log_open(argv[++i]) != 0) {
                fprintf(stderr, "cannot write %s\n", argv[i]);
                return 2;
            }
        }
        else if (a[0] != '-' && !disc_path) disc_path = a;
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

    /* A disc named on the command line wins, then one beside the
     * executable. Code is embedded in a recompiled program; the disc's
     * content travels beside it as a file. */
    char default_path[1024] = "";
    const bool embedded = program && program->image;
    if (!disc_path && default_disc) {
        char *base = SDL_GetBasePath();
        snprintf(default_path, sizeof default_path, "%s%s", base ? base : "", default_disc);
        SDL_free(base);
        disc_path = default_path;
    }
    if (!disc_path && !embedded) {
        fprintf(stderr, "usage: %s DISC [--scale N] [--save-dir DIR] [--fullscreen] [--mute]\n", argv[0]);
        quit_sdl();
        return 2;
    }

    /* Which console, settled before the machine is made (wii.h): a generated
     * game has said already, and a disc named on the command line says for
     * itself. */
    if (disc_path && gc_disc_console(disc_path) == 2) gc_console_select(1);

    GcMachine *m = gc_machine_create();
    if (!m) {
        quit_sdl();
        return 1;
    }
    gc_install_boot_state(m);
    /* The command stream draws on a thread of its own unless GC_GX_THREAD=0. */
    gc_gx_thread_start(m);

    /* What was named: a disc, or a program on its own.
     *
     * A game arrives as a disc and is started by the disc's own loader,
     * which is the only way to get the memory layout it expects. A test
     * program arrives as an ELF and is simply loaded. Which one this is is
     * decided by looking at the file rather than at its name, because a
     * name is a guess and the first thirty-two bytes are not. */
    struct GcDisc *disc = NULL;
    uint32_t entry = 0;
    bool loaded = false;
    GcStateIdentity state_id = {};
    if (disc_path) {
        disc = gc_disc_open(disc_path, NULL);
        if (disc && gc_attach_disc(m, disc) != 0) disc = NULL;
    }

    /* A disc boots through its own loader even when the program carries a
     * copy of the code, because the loader is what decides where the game's
     * stack goes and therefore where free memory begins. Loading the
     * embedded image instead puts the right instructions in the right places
     * and leaves the machine with a memory map the game does not recognise.
     * The embedded image is for a program with no disc beside it at all. */
    if (disc) {
        loaded = gc_run_apploader(m, disc, &entry) == 0;
        if (!loaded) gc_machine_reset_after_failed_boot(m);
    }
    if (!loaded && embedded) {
        loaded = gc_load_elf(m, program->image, program->image_size, &entry) == 0 ||
                 gc_load_dol(m, program->image, program->image_size, &entry) == 0;
    } else if (!loaded && disc_path) {
        size_t size = 0;
        FILE *file = fopen(disc_path, "rb");
        uint8_t *data = NULL;
        if (file) {
            fseek(file, 0, SEEK_END);
            long length = ftell(file);
            fseek(file, 0, SEEK_SET);
            if (length > 0) {
                data = (uint8_t *)malloc((size_t)length);
                if (data && fread(data, 1, (size_t)length, file) == (size_t)length) size = (size_t)length;
                else {
                    free(data);
                    data = NULL;
                }
            }
            fclose(file);
        }
        if (data) {
            loaded = gc_load_elf(m, data, size, &entry) == 0 || gc_load_dol(m, data, size, &entry) == 0;
            const char *leaf = disc_path;
            for (const char *p = disc_path; *p; p++) {
                if (*p == '/' || *p == '\\') leaf = p + 1;
            }
            gc_state_identity(&state_id, m, leaf, data, size);
            free(data);
        }
    }
    if (loaded && embedded) {
        gc_state_identity(&state_id, m, program->name, program->image, program->image_size);
    } else if (loaded && !state_id.program_hash) {
        gc_state_identity(&state_id, m, NULL, NULL, 0); /* a disc booted by its own loader */
    }
    if (!loaded) {
        char msg[1200];
        snprintf(msg, sizeof msg, "Cannot start:\n%s", disc_path ? disc_path : "(embedded)");
        fprintf(stderr, "gc: %s\n", msg);
        /* A dialogue is the right answer for somebody who opened this from a
         * desktop and the wrong one for a script, which has nobody to click
         * it and would simply stop here for ever. */
        if (!selftest.enabled) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Native Console", msg, NULL);
        gc_machine_destroy(m);
        quit_sdl();
        return 1;
    }
    m->cpu.pc = entry;
    m->cpu.npc = entry + 4;
    /* The stack and the small-data pointers a program expects from its
     * loader. libogc sets its own almost immediately; a bare program may
     * not - and a program that starts without a stack pointer runs until its
     * first function call and then jumps to address zero, which looks like a
     * recompiler fault and is not one. */
    m->cpu.gpr[1] = 0x816ffff0u;
    m->cpu.gpr[2] = 0x81100000u;
    m->cpu.gpr[13] = 0x81100000u;

    Frontend f;
    f.m = m;
    /* Saves and screenshots are named after the executable for an embedded
     * image (the hub keeps <game id>.gcdata beside <game id>.exe). */
    const char *stem_from = embedded ? argv[0] : disc_path;
    char card_path[1200], shot_path[1200], state_stem[1200];
    if (save_dir) make_dir(save_dir); /* fails harmlessly if it exists */
    sibling_path(card_path, sizeof card_path, save_dir, stem_from, ".card");
    sibling_path(shot_path, sizeof shot_path, save_dir, stem_from, ".bmp");
    sibling_path(state_stem, sizeof state_stem, save_dir, stem_from, "");
    f.card_path = card_path;
    f.shot_path = shot_path;
    f.state_stem = state_stem;
    {
        /* The shader cache goes where the card goes, named after the game. */
        char cache_dir[1200];
        sibling_path(cache_dir, sizeof cache_dir, save_dir, stem_from, "");
        std::string stem = cache_dir;
        size_t cut = stem.find_last_of("/\\");
        f.gl_id = cut == std::string::npos ? stem : stem.substr(cut + 1);
        f.gl_cache = cut == std::string::npos ? std::string(".") : stem.substr(0, cut);
        if (f.gl_cache.empty()) f.gl_cache = ".";
    }
    if (cli_gl_cache) f.gl_cache = cli_gl_cache;
    if (cli_gl_shaders) f.gl_shaders = cli_gl_shaders;
    f.renderer_this_run = cli_renderer;
    f.internal_scale_this_run = cli_internal_scale;
    f.anisotropy_this_run = cli_anisotropy;
    f.unthrottled = cli_unthrottled;
    if (cli_shown_every > 0 || cli_shown_log) {
        f.shown_every = cli_shown_every;
        f.shown_prefix = shot_prefix ? shot_prefix : "shown";
        f.shown = (uint32_t *)calloc((size_t)SCREEN_W * SCREEN_H, sizeof(uint32_t));
        if (cli_shown_log) {
            f.shown_log = fopen(cli_shown_log, "w");
            if (!f.shown_log) fprintf(stderr, "gc_sdl: cannot write %s\n", cli_shown_log);
        }
    }
    if (cli_play) {
        f.play = gc_input_script_load("gc_sdl", cli_play);
        if (!f.play) {
            gc_machine_destroy(m);
            quit_sdl();
            return 2;
        }
    }
    f.state_id = state_id;
    f.selftest = selftest.enabled;
    if (gc_card_insert(m, card_path) != 0) {
        fprintf(stderr, "gc: %s is not a memory card image this machine can use; running without one\n", card_path);
    }
    if (gc_console_is_wii()) {
        /* A Wii's flash memory - its saves and its settings - beside the
         * card. A file that is there and will not load is left alone, and
         * the game runs on a new console's flash that is never written. */
        char nand_path[1200];
        sibling_path(nand_path, sizeof nand_path, save_dir, stem_from, ".nand");
        if (wii_nand_attach(m, nand_path, stderr) != 0) {
            fprintf(stderr, "gc: %s is not a flash memory file this machine can use; nothing will be saved\n",
                    nand_path);
        }
    }

    if (prefs_file) {
        f.prefs_path = prefs_file;
    } else {
        char *pref_dir = SDL_GetPrefPath("gbrecompiled", "runtime-gc");
        if (pref_dir) f.prefs_path = std::string(pref_dir) + "gc_prefs.ini";
        SDL_free(pref_dir);
    }
    load_prefs(f);
    gc_vi_double = f.vi_double ? 1 : 0;
    if (selftest_verify) {
        const bool volume_kept = f.volume == SELFTEST_VOLUME;
        const bool kept = volume_kept && f.show_fps && f.fullscreen_saved;
        printf("menu selftest: %s the volume set in the menu is %d after a restart\n", volume_kept ? "ok  " : "FAIL",
               f.volume);
        printf("menu selftest: %s the FPS counter is still on after a restart\n", f.show_fps ? "ok  " : "FAIL");
        printf("menu selftest: %s Fullscreen, turned on in the menu, is %s after a restart\n",
               f.fullscreen_saved ? "ok  " : "FAIL", f.fullscreen_saved ? "on" : "off");
        printf("menu selftest: %s\n", kept ? "PASS" : "FAIL");
        gc_machine_destroy(m);
        quit_sdl();
        return kept ? 0 : 1;
    }
    if (gc_console_is_wii()) {
        /* The television, before the game reads what it is: the setting, or
         * this run's. Nothing has run yet. */
        f.wii_aspect_this_run = cli_wii_aspect;
        if (wii_console_set_widescreen(m, cli_wii_aspect ? cli_wii_aspect == 2 : f.wii_wide) != 0) {
            fprintf(stderr, "gc: the console's settings have no picture shape to set; the game decides\n");
        }
        read_picture_shape(f);
    }
    if (cli_scale > 0) f.scale = clamp_int(cli_scale, 1, MAX_SCALE);
    f.mute_this_run = cli_muted;
    f.fullscreen = cli_fullscreen || f.fullscreen_saved;

    f.title = program && program->name ? program->name : (disc && disc->name[0] ? disc->name : "GameCube");
    char title[160];
    snprintf(title, sizeof title, "%s - Native Console", f.title.c_str());
    /* At the Window Size in use, which is the setting's unless that does not
     * fit the screen (fitted_window_scale). */
    const int made_scale = fitted_window_scale(f);
    int win_x, win_y, win_w, win_h;
    window_place(f, win_x, win_y, win_w, win_h);
    f.win = SDL_CreateWindow(title, win_x, win_y, win_w, win_h,
                             SDL_WINDOW_RESIZABLE | (f.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    f.ren = f.win ? SDL_CreateRenderer(f.win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (f.win && !f.ren) f.ren = SDL_CreateRenderer(f.win, -1, SDL_RENDERER_SOFTWARE); /* no GPU (VMs, remote desktop) */
    f.tex = f.ren ? SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H)
                  : NULL;
    if (!f.tex) {
        fprintf(stderr, "SDL video: %s\n", SDL_GetError());
        gc_machine_destroy(m);
        quit_sdl();
        return 1;
    }
    SDL_SetWindowMinimumSize(f.win, SCREEN_W / 2, SCREEN_H / 2);
    apply_filter(f);
    /* The window can be asked for its frame now; where that is not what was
     * guessed and changes the size that fits, the window takes that size. */
    if (!f.fullscreen && fitted_window_scale(f) != made_scale) apply_window_scale(f);
    update_viewport(f);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL; /* every window is placed each field; nothing to remember */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(f.win, f.ren);
    ImGui_ImplSDLRenderer2_Init(f.ren);

    /* The machine plays at whatever rate the game asked the hardware for -
     * 32 kHz for almost everything - and the device wants its own; the two
     * are joined by a resampler in gc_audio_out.cpp, which also owns how much
     * is kept ahead of the device and what a gap sounds like. */
    f.audio = gc_audio_out_open();

    for (int i = 0; i < SDL_NumJoysticks() && !f.pad; i++) {
        if (SDL_IsGameController(i)) set_pad(f, SDL_GameControllerOpen(i));
    }
    f.pad_profile = gc_pad_profile(f.pad, f.platform);

    apply_renderer(f);

    if (load_path) {
        load_state_file(f, load_path, load_path);
        f.audio_restart = false; /* nothing has been queued yet */
    }

    const double freq = (double)SDL_GetPerformanceFrequency();
    double next_field = (double)SDL_GetPerformanceCounter() / freq;
    f.fps_since = SDL_GetTicks();
    const Uint32 started_at = SDL_GetTicks();
    bool running = true;
    static int16_t samples[4096 * 2];
    /* How the pacing went, for --audio-stats: fields that started late
     * against the clock, and times the debt was written off. */
    uint64_t pace_late = 0, pace_resets = 0;
    double pace_forgiven = 0.0;
    bool behind = false;

    FieldRunner runner;
    field_runner_open(runner, m, program, interpreter_only);

    const double t_origin = (double)SDL_GetPerformanceCounter() / freq;
    while (running) {
        const double t_loop_start = (double)SDL_GetPerformanceCounter() / freq;
        if (selftest.enabled && !menu_selftest_step(f, selftest)) running = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            /* Keys pressed while a binding is captured belong to it, not to
             * menu navigation (key releases still go through, so ImGui does
             * not think a key stays down). */
            if (!(f.capturing && (ev.type == SDL_KEYDOWN || ev.type == SDL_TEXTINPUT)))
                ImGui_ImplSDL2_ProcessEvent(&ev);
            if (handle_capture(f, ev)) continue;
            if (ev.type == SDL_QUIT) running = false;
            else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                     ev.window.windowID == SDL_GetWindowID(f.win)) {
                /* The close button, or Alt+F4. SDL makes an SDL_QUIT of a
                 * closed window only when it is the last one open, and the
                 * OpenGL path keeps hidden windows of its own for its
                 * contexts (gc_gl.c): under either OpenGL renderer the close
                 * arrived as this event and nothing else, and did nothing. */
                running = false;
            }
            else if (ev.type == SDL_CONTROLLERDEVICEADDED && !f.pad)
                set_pad(f, SDL_GameControllerOpen(ev.cdevice.which));
            else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && f.pad &&
                     ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(f.pad))) {
                set_pad(f, NULL);
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                if (f.save_dialog) set_save_dialog(f, false);
                else set_menu(f, !f.show_menu);
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
                if (f.big) SDL_DestroyTexture(f.big);
                f.big = nullptr;
                f.big_on = false;
                SDL_DestroyTexture(f.tex);
                f.tex = SDL_CreateTexture(f.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W,
                                          SCREEN_H);
                if (!f.tex) running = false;
                else apply_filter(f);
            } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat &&
                       (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
                        ev.key.keysym.scancode == SDL_SCANCODE_AC_BACK)) {
                if (f.save_dialog) set_save_dialog(f, false);
                else set_menu(f, !f.show_menu);
            } else if ((ev.type == SDL_KEYDOWN && !ev.key.repeat) || ev.type == SDL_CONTROLLERBUTTONDOWN) {
                note_shortcut_press(f, ev);
            }
        }
        run_shortcuts(f);
        finish_save(f, false);
        /* Controller navigation of the menu waits while a binding is being
         * captured, and until the buttons used for it are released. */
        if (f.pad_nav_blocked && !f.capturing && !any_pad_input(f)) f.pad_nav_blocked = false;
        if (f.capturing || f.pad_nav_blocked) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        if (f.audio_restart) {
            /* A state was loaded: the sound queued ahead of the device, and
             * half-converted in the resampler, belongs to the moment the
             * game left. Start the queue again from silence. */
            f.audio_restart = false;
            gc_audio_out_clear(f.audio);
            next_field = (double)SDL_GetPerformanceCounter() / freq;
        }

        const bool paused = f.show_menu || f.save_dialog;
        if (!paused) {
            GcPad pad;
            if (f.play) gc_input_script_pad(f.play, m->field, &pad);
            else build_pad(f, pad);
            f.held_from_menu &= pad.buttons;
            pad.buttons = (uint16_t)(pad.buttons & ~f.held_from_menu);
            if (gc_console_is_wii()) {
                /* The remote, and a GameCube controller in the first port
                 * only when that is what the game is played with: a game
                 * that reads both would otherwise take every press twice. */
                WiiRemoteInput remote;
                update_pointer(f, pad);
                build_remote(f, pad, remote);
                wii_remote_set(m, 0, &remote);
                if (f.wii_style != WII_STYLE_GAMECUBE) pad.present = 0;
            }
            gc_pad_set(m, 0, &pad);

            /* The field, and while it runs the second half of the present
             * before, when that was put off for it. */
            run_field_start(runner, m->field + 1u);
            if (f.show_pending) present_show(f);
            run_field_wait(runner);
            if (m->finished || m->cpu.stopped) running = false;
            if (limit_fields > 0 && (long)m->field >= limit_fields) running = false;
            if (limit_seconds > 0 && SDL_GetTicks() >= started_at + (Uint32)limit_seconds * 1000u)
                running = false;
            f.fps_fields++;
            Uint32 elapsed = SDL_GetTicks() - f.fps_since;
            if (elapsed >= 1000) {
                f.fps = f.fps_fields * 1000.0 / elapsed;
                f.fps_fields = 0;
                f.fps_since = SDL_GetTicks();
            }

            const unsigned rate = gc_audio_rate(m);
            unsigned frames = 0, got;
            while ((got = gc_audio_take(m, samples + frames * 2u, 4096u - frames)) > 0 && frames < 4096u) frames += got;
            if (f.audio && !f.fast && f.speed == 100) {
                /* Muted sound still goes to the device, as silence, to keep
                 * its clock - which is what the field clock is steered by -
                 * running. */
                if (f.muted || f.mute_this_run || f.volume == 0) {
                    memset(samples, 0, (size_t)frames * 4u);
                } else if (f.volume != VOLUME_MAX) {
                    for (size_t i = 0; i < (size_t)frames * 2u; i++)
                        samples[i] = (int16_t)(samples[i] * f.volume / VOLUME_MAX);
                }
                if (frames || gc_audio_playing(m)) {
                    gc_audio_out_push(f.audio, samples, frames, rate, behind);
                } else {
                    /* A machine that is producing nothing at all - a game
                     * whose sound engine has not started - is silent on
                     * purpose: a field of silence, so the device does not
                     * run dry and the clock stays steered.
                     *
                     * Only then, though. A machine that *is* playing and is
                     * falling behind is a different thing, and padding it
                     * with silence would hide exactly what --audio-stats
                     * exists to measure. */
                    gc_audio_out_push_silence(f.audio, (unsigned)(rate * FIELD_SECONDS + 0.5), rate);
                }
            }
        } else {
            f.fps_fields = 0;
            f.fps_since = SDL_GetTicks();
        }

        const double t_run_end = (double)SDL_GetPerformanceCounter() / freq;
        {
            /* The picture is taken now. It goes on screen now as well when
             * there is time for that before the next field is due - or when
             * nothing will be running to do it beside (paused, the last
             * field, no thread); otherwise while the next field runs, which
             * starts at once and so shows it no later. */
            const double due = (!paused && !f.fast) ? next_field + FIELD_SECONDS * 100.0 / f.speed : 0.0;
            present_take(f, due);
            const double taken = (double)SDL_GetPerformanceCounter() / freq;
            const bool time_now = due > 0.0 && due - taken > f.show_cost * 1.5 + 0.001;
            if (paused || !running || !runner.thread || time_now) present_show(f);
            else f.show_pending = true;
        }
        const double t_present_end = (double)SDL_GetPerformanceCounter() / freq;

        /* Pace to the field rate by the clock, keeping a small debt: a field
         * that ran late is paid back by the ones after it not sleeping, so a
         * game that alternates a slow field with a cheap one still averages
         * the console's rate. The rate is steered to hold the sound ring at
         * its target (0.5 % fast at most, 2 % slow), because the sound
         * device's clock and the performance counter drift apart.
         *
         * A debt of more than six fields (100 ms) is written down to six.
         * Paying back a long stretch where the machine was slower than the
         * console ran the next scene visibly fast - and pushed everything it
         * played in that time into the sound ring, where it sat as latency:
         * 220 ms of it after Shrek 2's menu, draining at 0.5 % for most of a
         * minute. Clearing the debt altogether instead made a slow stretch
         * slower still, because the field after each write-off slept.
         *
         * There is deliberately no "the sound is short, so run the next field
         * at once" any more either. That made the field clock follow the
         * sound ring, which follows the field clock; the output now rides out
         * a short ring by fading to silence and waiting for a steady amount
         * (gc_audio_out.cpp), so the clock can simply be a clock.
         *
         * Other speeds pace by the clock alone; the menu redraws at about
         * 60 Hz while the game is paused. */
        double now = t_present_end;
        double lateness = 0.0;
        bool reset = false;
        if (paused) {
            SDL_Delay(16);
            next_field = (double)SDL_GetPerformanceCounter() / freq;
        } else if (!f.fast) {
            double field = FIELD_SECONDS * 100.0 / f.speed;
            if (f.audio && f.speed == 100) field *= 1.0 + 0.005 * gc_audio_out_steer(f.audio);
            next_field += field;
            lateness = now - next_field;
            behind = lateness > FIELD_SECONDS;
            if (lateness > 0.0) pace_late++;
            if (lateness > 6.0 * FIELD_SECONDS) {
                /* Held at six fields rather than cleared: a machine that is
                 * slower than the console here goes on not sleeping, and one
                 * that is fast again pays back six fields and no more. */
                reset = true;
                pace_resets++;
                pace_forgiven += lateness - 6.0 * FIELD_SECONDS;
                next_field = now - 6.0 * FIELD_SECONDS;
            } else if (lateness < 0.0) {
                /* SDL_Delay can oversleep by a millisecond or more: sleep
                 * short, then wait out the rest. */
                while (next_field - now > 0.002) {
                    SDL_Delay((Uint32)((next_field - now - 0.0015) * 1000.0));
                    now = (double)SDL_GetPerformanceCounter() / freq;
                }
                while (now < next_field) now = (double)SDL_GetPerformanceCounter() / freq;
            }
        } else {
            next_field = now;
        }
        if (pace_log && !paused) {
            /* One line a field: field, start (ms from the first field), run,
             * present and wait (ms), how late the field ended against the
             * clock (ms), the sound ring (ms), and whether the debt was
             * written off. */
            const double t_end = (double)SDL_GetPerformanceCounter() / freq;
            fprintf(pace_log, "%llu %.3f %.3f %.3f %.3f %.3f %.1f %d %d\n", (unsigned long long)m->field,
                    (t_loop_start - t_origin) * 1000.0, (t_run_end - t_loop_start) * 1000.0,
                    (t_present_end - t_run_end) * 1000.0, (t_end - t_present_end) * 1000.0, lateness * 1000.0,
                    audio_queue_ms(f), behind ? 1 : 0, reset ? 1 : 0);
        }
    }
    field_runner_close(runner);
    menu_selftest_closed(selftest);
    if (pace_log) fclose(pace_log);
    if (audio_stats || f.shown) {
        fprintf(stderr,
                "present: %llu presents, %llu kept the picture before (longest run %llu); waited for the "
                "display buffer %llu times, %.0f ms\n",
                (unsigned long long)f.presents, (unsigned long long)f.presents_kept, (unsigned long long)f.kept_run_max,
                (unsigned long long)f.present_waits, f.present_wait_ms);
    }

    if (audio_stats) {
        const Uint32 ran_for = SDL_GetTicks() - started_at;
        /* How fast the machine actually went, because the queue statistics
         * cannot be read without it: a machine running below the console's
         * rate starves the sound device for a reason that has nothing to do
         * with the sound path, and a report that does not say so invites the
         * wrong fix. */
        const double fields_a_second = ran_for ? (double)m->field * 1000.0 / (double)ran_for : 0.0;
        if (f.audio) {
            GcAudioOutStats st;
            gc_audio_out_stats(f.audio, &st);
            const double device_ms = st.device_frames * 1000.0 / (st.device_rate ? st.device_rate : 1);
            fprintf(stderr,
                    "audio: %s %d Hz, %d-frame device buffer; ring after each push: mean %.1f ms, min %.1f ms; "
                    "ran dry %llu times in %llu fields (%.0f ms of silence), %llu frames dropped\n",
                    st.driver, st.device_rate, st.device_frames, st.fill_mean_ms, st.fill_min_ms,
                    (unsigned long long)st.underruns, (unsigned long long)st.pushes, st.dry_ms,
                    (unsigned long long)st.dropped_frames);
            /* The latency this side of the operating system's mixer: a
             * sample waits in the ring for its mean fill (measured just after
             * a push, so less half a field), then for up to a device chunk.
             * The system mixer's own buffer comes on top and is not visible
             * from here. */
            fprintf(stderr,
                    "latency: ring %.1f ms + device buffer %.1f ms = %.1f ms before the system mixer; "
                    "target %.0f ms (started %.0f, highest %.0f); callback every %.1f ms (longest %.1f)\n",
                    st.fill_mean_ms - 500.0 * FIELD_SECONDS, device_ms,
                    st.fill_mean_ms - 500.0 * FIELD_SECONDS + device_ms, st.target_ms, st.target_start_ms,
                    st.target_max_ms, st.callback_period_mean_ms, st.callback_period_max_ms);
        } else {
            fprintf(stderr, "audio: no output device\n");
        }
        fprintf(stderr, "pace: %llu fields ended late; debt over six fields written down %llu times (%.0f ms)\n",
                (unsigned long long)pace_late, (unsigned long long)pace_resets, pace_forgiven * 1000.0);
        fprintf(stderr, "speed: %llu fields in %.1f s, %.1f a second - %.0f%% of the console's rate (%s)\n",
                (unsigned long long)m->field, ran_for / 1000.0, fields_a_second,
                fields_a_second * 100.0 * FIELD_SECONDS, renderer_name(f));
        /* And where the machine got to, as one number. A change meant to make
         * the machine faster has to leave this exactly where it was for the
         * same run, and a speed figure without it would not say whether the
         * faster machine was still running the same game.
         *
         * Recompiled code finishes the block it is in before it looks at the
         * field counter, so a run stops a few instructions later than the
         * interpreter does. Neither is wrong, and the guest cannot tell, but
         * a hash taken where each happened to stop would differ. So both
         * are first carried by the interpreter to the same instruction. */
        const uint64_t hash_at = m->field * GC_CYCLES_PER_FIELD + 100000u;
        while (m->cpu.cycles < hash_at && !m->finished && !m->cpu.stopped) gc_machine_step(m);
        gc_gl_settle(m); /* copies still on their way are part of memory */
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](const uint8_t *p, size_t n) {
            for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
        };
        mix(m->mem1, gc_ram_size);        mix((const uint8_t *)m->cpu.gpr, sizeof m->cpu.gpr);
        mix((const uint8_t *)m->cpu.fpr, sizeof m->cpu.fpr);
        fprintf(stderr, "state: field %llu, %llu instructions, pc %08x, hash %016llx\n",
                (unsigned long long)m->field, (unsigned long long)m->cpu.cycles, m->cpu.pc,
                (unsigned long long)h);
    }
    if (shot_prefix) {
        char path[1200];
        snprintf(path, sizeof path, "%s.bmp", shot_prefix);
        screenshot(f, path);
    }
    if (internal_shot) screenshot_larger(f, internal_shot);
    finish_save(f, true);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (f.pad) SDL_GameControllerClose(f.pad);
    gc_audio_out_close(f.audio);
    f.audio = nullptr;
    if (f.big) SDL_DestroyTexture(f.big);
    SDL_DestroyTexture(f.tex);
    SDL_DestroyRenderer(f.ren);
    SDL_DestroyWindow(f.win);
    free(f.shown);
    if (f.shown_log) fclose(f.shown_log);
    /* The machine writes the memory card back as it goes away. */
    gc_machine_destroy(m);
    gc_input_script_free(f.play);
    gc_perf_field_log_close();
    quit_sdl();
    (void)set_status;
    return selftest.failures ? 1 : 0;
}
