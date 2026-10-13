#include "gbrt_voxel.h"

#include "gbrt.h"
#include "gbrt_hash.h"
#include "gbrt_texture_pack.h"
#include "ppu.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAP_CELLS 16          /* the 256x256 tilemap is 16x16 cells of 16 pixels */
#define CELL_PX 16
#define MAX_CLASSES 32
#define MAX_REQUIRES 8
#define MAX_ROMS 8
#define MAX_GRID 256
#define MAX_DEPTH 64
#define MAX_STRIPS 2048
#define MAX_CUBES 8192
#define MAX_PROP_BLOCKS 64
#define MAX_PROPS 192
#define PROP_MAX_PX 32
#define PROP_MIN_PIXELS 6
#define NO_CLASS -1
#define OVERLAY_CLASS -2
#define PENDING_TIMEOUT 60u   /* frames an in-place grid change waits for its tiles */
#define TRANSITION_TIMEOUT 240u
#define OVERLAY_FALLBACK_PERCENT 35u

typedef enum ShapeType {
    SHAPE_GROUND = 0,
    SHAPE_RELIEF,
    SHAPE_SUNKEN,
    SHAPE_BOX,
    SHAPE_DOME,
    SHAPE_FACADE,
    SHAPE_PROP,
} ShapeType;

typedef struct VoxelClass {
    char name[32];
    ShapeType type;
    int height;
    int radius;
    int groove;
    int raise;
    int min_shade;
    int rows;
    bool cutout;
    int span;      /* prop: cells per side of the object's art */
    int top_rows;  /* prop: art rows that are the top (cap, canopy); the rest wrap the sides */
    int canopy;    /* prop: thickness of a slab carrying the top art; 0 = top face on the body */
} VoxelClass;

typedef struct Require {
    uint16_t addr;
    uint8_t value;
} Require;

typedef struct Profile {
    int playfield_width;
    int playfield_height;
    uint16_t grid_addr;
    int grid_cols;
    int grid_rows;
    int grid_stride;
    uint16_t room_addr;
    Require requires[MAX_REQUIRES];
    int require_count;
    int ground;
    float pitch;
    float distance;
    VoxelClass classes[MAX_CLASSES];
    int class_count;
    int8_t object_class[256];
    uint8_t object_quadrant[256];  /* prop: cell of the object within its art, x | y << 4 */
    uint8_t roms[MAX_ROMS][32];  /* SHA-256 of ROMs the profile is for; none = any */
    int rom_count;
} Profile;

typedef struct Cell {
    uint8_t obj;
    uint8_t pending_obj;
    bool valid;
    bool pending;
    bool pending_room;    /* part of a room scrolling in; commits when the scroll lands */
    uint16_t pending_age;
    uint32_t sig;         /* the four tilemap entries the object was seen with */
    uint32_t pending_sig; /* tilemap entries when the pending object arrived */
} Cell;

/* One occurrence of a prop object's art on screen, split into its separate
 * objects (a cell of two posts holds two). */
typedef struct PropBlock {
    uint16_t cell;              /* tilemap cell of the art's top-left corner */
    int16_t sx, sy;             /* screen position of that corner */
    uint8_t cls;
    uint8_t size;               /* art pixels per side */
    uint8_t shade[PROP_MAX_PX * PROP_MAX_PX];
    int8_t part[PROP_MAX_PX * PROP_MAX_PX];  /* object index, -1 background */
} PropBlock;

typedef struct Prop {
    uint8_t block;
    int8_t part;
    int8_t body_x0, body_x1;    /* art columns of the body; x0 > x1 = no body */
    int8_t body_last;           /* last art row of the body */
    int8_t top_x0, top_x1;      /* art extent of the top; x0 > x1 = no top */
    int8_t top_y0, top_y1;
    uint8_t top_fill;           /* commonest non-outline shade of the top, for its corners */
} Prop;

typedef struct Snapshot {
    uint8_t vram[VRAM_SIZE];
    uint8_t oam[0xA0];
    uint8_t io[0x80];
    uint32_t sources[GB_FRAMEBUFFER_SIZE];
    bool has_sources;
    Cell cells[MAP_CELLS * MAP_CELLS];
    bool valid;           /* conditions held and the grid is tracked */
} Snapshot;

typedef struct VoxelState {
    Profile profile;
    bool enabled;
    uint32_t own_sources[GB_FRAMEBUFFER_SIZE];

    /* grid tracking */
    Cell cells[MAP_CELLS * MAP_CELLS];
    bool tracking;
    int origin_x;
    int origin_y;
    int last_room;
    uint8_t last_grid[MAX_GRID];
    bool transition;
    uint32_t transition_age;

    Snapshot snap;

    /* composition scratch */
    uint8_t shade[GB_FRAMEBUFFER_SIZE];
    int8_t cls[GB_FRAMEBUFFER_SIZE];
    uint8_t dome[GB_FRAMEBUFFER_SIZE];
    uint8_t top[GB_FRAMEBUFFER_SIZE];
    uint8_t top_shade[GB_FRAMEBUFFER_SIZE];
    int16_t strip[GB_FRAMEBUFFER_SIZE];
    uint8_t strips[MAX_STRIPS][MAX_DEPTH];
    int strip_count;
    uint16_t cube_x[MAX_CUBES];
    uint16_t cube_y[MAX_CUBES];
    uint8_t cube_z[MAX_CUBES];
    uint8_t cube_shade[MAX_CUBES];
    int cube_count;
    bool cutout_ready[MAP_CELLS * MAP_CELLS];
    uint8_t cutout_bits[MAP_CELLS * MAP_CELLS][32];
    PropBlock prop_blocks[MAX_PROP_BLOCKS];
    int prop_block_count;
    Prop props[MAX_PROPS];
    int prop_count;
    float* depth;

    /* camera for the current compose */
    float cam_y, cam_z, sin_p, cos_p, fit_scale, fit_x, fit_y;
    float light_rgb[4][3];
    uint32_t* out;
    int region_height;
    uint32_t faces;

    GBVoxelStats stats;
} VoxelState;

static VoxelState* g_voxel = NULL;

/* ------------------------------------------------------------------------ */
/* Profile parsing                                                          */

static int find_class(const Profile* profile, const char* name) {
    for (int i = 0; i < profile->class_count; ++i) {
        if (strcmp(profile->classes[i].name, name) == 0) {
            return i;
        }
    }
    return NO_CLASS;
}

static bool parse_hex(const char* text, long max, long* out) {
    char* end = NULL;
    const long value = strtol(text, &end, 16);
    if (!text[0] || *end || value < 0 || value > max) {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_int(const char* text, long min, long max, long* out) {
    char* end = NULL;
    const long value = strtol(text, &end, 10);
    if (!text[0] || *end || value < min || value > max) {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_class(Profile* profile, char** tokens, int count) {
    if (count < 3 || profile->class_count >= MAX_CLASSES || strlen(tokens[1]) >= 32 ||
        find_class(profile, tokens[1]) != NO_CLASS) {
        return false;
    }
    VoxelClass* cls = &profile->classes[profile->class_count];
    memset(cls, 0, sizeof(*cls));
    strcpy(cls->name, tokens[1]);
    static const char* const type_names[] = {"ground", "relief", "sunken", "box", "dome", "facade", "prop"};
    int type = -1;
    for (int i = 0; i < 7; ++i) {
        if (strcmp(tokens[2], type_names[i]) == 0) {
            type = i;
        }
    }
    if (type < 0) {
        return false;
    }
    cls->type = (ShapeType)type;
    cls->raise = 1;
    cls->min_shade = 2;
    cls->rows = 1;
    cls->radius = 1;
    cls->span = 1;
    for (int i = 3; i < count; ++i) {
        char* eq = strchr(tokens[i], '=');
        if (!eq) {
            return false;
        }
        *eq = '\0';
        long value = 0;
        if (!parse_int(eq + 1, 0, MAX_DEPTH - 8, &value)) {
            return false;
        }
        if (strcmp(tokens[i], "height") == 0) cls->height = (int)value;
        else if (strcmp(tokens[i], "radius") == 0 && value > 0) cls->radius = (int)value;
        else if (strcmp(tokens[i], "groove") == 0) cls->groove = (int)value;
        else if (strcmp(tokens[i], "raise") == 0) cls->raise = (int)value;
        else if (strcmp(tokens[i], "min_shade") == 0 && value <= 3) cls->min_shade = (int)value;
        else if (strcmp(tokens[i], "rows") == 0 && value > 0 && value <= 3) cls->rows = (int)value;
        else if (strcmp(tokens[i], "cutout") == 0) cls->cutout = value != 0;
        else if (strcmp(tokens[i], "span") == 0 && value >= 1 && value <= 2) cls->span = (int)value;
        else if (strcmp(tokens[i], "top_rows") == 0 && value <= PROP_MAX_PX) cls->top_rows = (int)value;
        else if (strcmp(tokens[i], "canopy") == 0 && value <= PROP_MAX_PX) cls->canopy = (int)value;
        else return false;
    }
    profile->class_count++;
    return true;
}

static bool parse_line(Profile* profile, char* line, bool* saw_header) {
    char* hash = strchr(line, '#');
    if (hash) {
        *hash = '\0';
    }
    char* tokens[24];
    int count = 0;
    for (char* p = line; *p && count < 24;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        tokens[count++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    if (count == 0) {
        return true;
    }
    long a = 0, b = 0, c = 0, d = 0;
    if (!*saw_header) {
        *saw_header = count == 2 && strcmp(tokens[0], "voxel-profile") == 0 &&
                      strcmp(tokens[1], "1") == 0;
        return *saw_header;
    }
    if (strcmp(tokens[0], "playfield") == 0 && count == 3) {
        if (!parse_int(tokens[1], 160, 160, &a) || !parse_int(tokens[2], 16, 144, &b)) return false;
        profile->playfield_width = (int)a;
        profile->playfield_height = (int)b;
    } else if (strcmp(tokens[0], "grid") == 0 && count == 5) {
        if (!parse_hex(tokens[1], 0xFFFF, &a) || !parse_int(tokens[2], 1, MAP_CELLS, &b) ||
            !parse_int(tokens[3], 1, MAP_CELLS, &c) || !parse_int(tokens[4], 1, MAP_CELLS, &d) ||
            d < b) {
            return false;
        }
        profile->grid_addr = (uint16_t)a;
        profile->grid_cols = (int)b;
        profile->grid_rows = (int)c;
        profile->grid_stride = (int)d;
    } else if (strcmp(tokens[0], "room") == 0 && count == 2) {
        if (!parse_hex(tokens[1], 0xFFFF, &a)) return false;
        profile->room_addr = (uint16_t)a;
    } else if (strcmp(tokens[0], "require") == 0 && count == 3) {
        if (profile->require_count >= MAX_REQUIRES || !parse_hex(tokens[1], 0xFFFF, &a) ||
            !parse_hex(tokens[2], 0xFF, &b)) {
            return false;
        }
        profile->requires[profile->require_count].addr = (uint16_t)a;
        profile->requires[profile->require_count].value = (uint8_t)b;
        profile->require_count++;
    } else if (strcmp(tokens[0], "ground") == 0 && count == 2) {
        if (!parse_int(tokens[1], 1, 16, &a)) return false;
        profile->ground = (int)a;
    } else if (strcmp(tokens[0], "camera") == 0 && count == 3) {
        const float pitch = (float)atof(tokens[1]);
        const float distance = (float)atof(tokens[2]);
        if (pitch < 20.0f || pitch > 89.0f || distance < 0.5f || distance > 8.0f) return false;
        profile->pitch = pitch;
        profile->distance = distance;
    } else if (strcmp(tokens[0], "class") == 0) {
        return parse_class(profile, tokens, count);
    } else if (strcmp(tokens[0], "object") == 0 && (count == 3 || count == 5)) {
        const int index = find_class(profile, tokens[2]);
        if (!parse_hex(tokens[1], 0xFF, &a) || index == NO_CLASS) return false;
        profile->object_class[a] = (int8_t)index;
        if (count == 5) {
            const int span = profile->classes[index].span;
            if (!parse_int(tokens[3], 0, span - 1, &b) || !parse_int(tokens[4], 0, span - 1, &c)) return false;
            profile->object_quadrant[a] = (uint8_t)(b | (c << 4));
        }
    } else if (strcmp(tokens[0], "rom") == 0 && count == 2) {
        if (profile->rom_count >= MAX_ROMS || strlen(tokens[1]) != 64) return false;
        uint8_t* digest = profile->roms[profile->rom_count];
        for (int i = 0; i < 32; ++i) {
            char pair[3] = {tokens[1][i * 2], tokens[1][i * 2 + 1], '\0'};
            if (!isxdigit((unsigned char)pair[0]) || !isxdigit((unsigned char)pair[1])) return false;
            digest[i] = (uint8_t)strtol(pair, NULL, 16);
        }
        profile->rom_count++;
    } else if (strcmp(tokens[0], "title") == 0 && count >= 2) {
        /* A name for launchers; the view does not use it. */
    } else {
        return false;
    }
    return true;
}

GBVoxelStatus gbrt_voxel_load_memory(GBContext* context, const char* text, size_t size,
                                     int* error_line) {
    if (error_line) *error_line = 0;
    if (!context || !text) {
        return GB_VOXEL_ERROR_ARGUMENT;
    }
    VoxelState* state = (VoxelState*)calloc(1, sizeof(VoxelState));
    if (!state) {
        return GB_VOXEL_ERROR_MEMORY;
    }
    state->depth = (float*)malloc(sizeof(float) * GB_VOXEL_OUTPUT_WIDTH * GB_VOXEL_OUTPUT_HEIGHT);
    if (!state->depth) {
        free(state);
        return GB_VOXEL_ERROR_MEMORY;
    }
    Profile* profile = &state->profile;
    memset(profile->object_class, NO_CLASS, sizeof(profile->object_class));
    profile->playfield_width = 160;
    profile->playfield_height = 144;
    profile->ground = 3;
    profile->pitch = 55.0f;
    profile->distance = 2.0f;

    char line[512];
    bool saw_header = false;
    bool saw_grid = false;
    int line_number = 0;
    size_t pos = 0;
    while (pos < size) {
        size_t len = 0;
        while (pos < size && text[pos] != '\n') {
            if (len + 1 < sizeof(line)) line[len++] = text[pos];
            pos++;
        }
        pos++;
        line[len] = '\0';
        if (len && line[len - 1] == '\r') line[len - 1] = '\0';
        line_number++;
        if (strncmp(line, "grid", 4) == 0) saw_grid = true;
        if (!parse_line(profile, line, &saw_header)) {
            if (error_line) *error_line = line_number;
            free(state->depth);
            free(state);
            return GB_VOXEL_ERROR_FORMAT;
        }
    }
    if (!saw_header || !saw_grid || profile->room_addr == 0) {
        if (error_line) *error_line = line_number;
        free(state->depth);
        free(state);
        return GB_VOXEL_ERROR_FORMAT;
    }
    if (profile->rom_count > 0) {
        bool matched = false;
        if (context->rom) {
            uint8_t digest[32];
            gbrt_sha256(context->rom, context->rom_size, digest);
            for (int i = 0; i < profile->rom_count && !matched; ++i) {
                matched = memcmp(digest, profile->roms[i], 32) == 0;
            }
        }
        if (!matched) {
            free(state->depth);
            free(state);
            return GB_VOXEL_ERROR_ROM_MISMATCH;
        }
    }

    gbrt_voxel_unload();
    g_voxel = state;
    g_voxel->enabled = true;
    g_voxel->last_room = -1;
    if (!gbrt_pixel_sources) {
        gbrt_pixel_sources = g_voxel->own_sources;
    }
    return GB_VOXEL_OK;
}

GBVoxelStatus gbrt_voxel_load_file(GBContext* context, const char* path, int* error_line) {
    if (error_line) *error_line = 0;
    if (!context || !path) {
        return GB_VOXEL_ERROR_ARGUMENT;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        return GB_VOXEL_ERROR_IO;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0 || size > (1 << 20)) {
        fclose(file);
        return GB_VOXEL_ERROR_IO;
    }
    char* text = (char*)malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        return GB_VOXEL_ERROR_MEMORY;
    }
    const size_t read = fread(text, 1, (size_t)size, file);
    fclose(file);
    GBVoxelStatus status = read == (size_t)size
        ? gbrt_voxel_load_memory(context, text, read, error_line)
        : GB_VOXEL_ERROR_IO;
    free(text);
    return status;
}

void gbrt_voxel_unload(void) {
    if (!g_voxel) {
        return;
    }
    if (gbrt_pixel_sources == g_voxel->own_sources) {
        gbrt_pixel_sources = NULL;
    }
    free(g_voxel->depth);
    free(g_voxel);
    g_voxel = NULL;
}

bool gbrt_voxel_is_loaded(void) { return g_voxel != NULL; }

void gbrt_voxel_set_enabled(bool enabled) {
    if (g_voxel) g_voxel->enabled = enabled;
}

bool gbrt_voxel_is_enabled(void) { return g_voxel && g_voxel->enabled; }

void gbrt_voxel_set_camera(float pitch_degrees, float distance_scale) {
    if (!g_voxel) return;
    if (pitch_degrees < 20.0f) pitch_degrees = 20.0f;
    if (pitch_degrees > 89.0f) pitch_degrees = 89.0f;
    if (distance_scale < 0.5f) distance_scale = 0.5f;
    if (distance_scale > 8.0f) distance_scale = 8.0f;
    g_voxel->profile.pitch = pitch_degrees;
    g_voxel->profile.distance = distance_scale;
}

void gbrt_voxel_get_camera(float* pitch_degrees, float* distance_scale) {
    if (pitch_degrees) *pitch_degrees = g_voxel ? g_voxel->profile.pitch : 0.0f;
    if (distance_scale) *distance_scale = g_voxel ? g_voxel->profile.distance : 0.0f;
}

GBVoxelStats gbrt_voxel_stats(void) {
    GBVoxelStats empty;
    memset(&empty, 0, sizeof(empty));
    return g_voxel ? g_voxel->stats : empty;
}

const char* gbrt_voxel_status_string(GBVoxelStatus status) {
    switch (status) {
        case GB_VOXEL_OK: return "ok";
        case GB_VOXEL_ERROR_ARGUMENT: return "invalid argument";
        case GB_VOXEL_ERROR_IO: return "could not read file";
        case GB_VOXEL_ERROR_FORMAT: return "malformed voxel profile";
        case GB_VOXEL_ERROR_MEMORY: return "out of memory";
        case GB_VOXEL_ERROR_ROM_MISMATCH: return "voxel profile is for a different ROM";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------ */
/* Frame snapshot and grid tracking                                         */

static uint8_t read_guest(const GBContext* ctx, uint16_t addr) {
    if (addr >= 0xC000 && addr < 0xD000) return ctx->wram[addr - 0xC000];
    if (addr >= 0xD000 && addr < 0xE000) {
        const unsigned bank = ctx->wram_bank ? ctx->wram_bank : 1u;
        return ctx->wram[bank * 0x1000u + (addr - 0xD000u)];
    }
    if (addr >= 0xFF80 && addr < 0xFFFF) return ctx->hram[addr - 0xFF80];
    if (addr >= 0xFF00 && addr < 0xFF80) return ctx->io[addr - 0xFF00];
    return 0xFF;
}

static uint16_t tilemap_base(const uint8_t* io) {
    return (io[0x40] & 0x08) ? 0x1C00u : 0x1800u; /* VRAM offsets of 9C00 / 9800 */
}

static uint32_t cell_signature(const Snapshot* snap, int cx, int cy) {
    const uint16_t base = tilemap_base(snap->io);
    const int tx = (cx & (MAP_CELLS - 1)) * 2;
    const int ty = (cy & (MAP_CELLS - 1)) * 2;
    return (uint32_t)snap->vram[base + ty * 32 + tx] |
           ((uint32_t)snap->vram[base + ty * 32 + tx + 1] << 8) |
           ((uint32_t)snap->vram[base + (ty + 1) * 32 + tx] << 16) |
           ((uint32_t)snap->vram[base + (ty + 1) * 32 + tx + 1] << 24);
}

static Cell* cell_at(VoxelState* state, int cx, int cy) {
    return &state->cells[(cy & (MAP_CELLS - 1)) * MAP_CELLS + (cx & (MAP_CELLS - 1))];
}

static void reset_tracking(VoxelState* state, const uint8_t* grid, int origin_x, int origin_y) {
    const Profile* profile = &state->profile;
    memset(state->cells, 0, sizeof(state->cells));
    state->origin_x = origin_x & (MAP_CELLS - 1);
    state->origin_y = origin_y & (MAP_CELLS - 1);
    for (int r = 0; r < profile->grid_rows; ++r) {
        for (int c = 0; c < profile->grid_cols; ++c) {
            Cell* cell = cell_at(state, state->origin_x + c, state->origin_y + r);
            cell->obj = grid[r * profile->grid_stride + c];
            cell->valid = true;
            cell->sig = cell_signature(&state->snap, state->origin_x + c, state->origin_y + r);
        }
    }
    state->tracking = true;
    state->transition = false;
}

static void commit(VoxelState* state, Cell* cell, uint32_t sig) {
    (void)state;
    cell->obj = cell->pending_obj;
    cell->valid = true;
    cell->pending = false;
    cell->pending_room = false;
    cell->sig = sig;
}

void gbrt_voxel_frame_complete(const GBContext* context) {
    VoxelState* state = g_voxel;
    if (!state || !context || !context->vram) {
        return;
    }
    const Profile* profile = &state->profile;
    Snapshot* snap = &state->snap;
    memcpy(snap->vram, context->vram, VRAM_SIZE);
    memcpy(snap->oam, context->oam, sizeof(snap->oam));
    memcpy(snap->io, context->io, sizeof(snap->io));
    snap->has_sources = gbrt_pixel_sources != NULL;
    if (snap->has_sources) {
        memcpy(snap->sources, gbrt_pixel_sources, sizeof(snap->sources));
    }
    snap->valid = false;

    bool conditions = (snap->io[0x40] & 0x80) != 0;
    for (int i = 0; i < profile->require_count && conditions; ++i) {
        conditions = read_guest(context, profile->requires[i].addr) == profile->requires[i].value;
    }
    if (!conditions) {
        state->tracking = false;
        state->last_room = -1;
        return;
    }

    uint8_t grid[MAX_GRID];
    for (int i = 0; i < profile->grid_rows * profile->grid_stride; ++i) {
        grid[i] = read_guest(context, (uint16_t)(profile->grid_addr + i));
    }
    const int room = read_guest(context, profile->room_addr);
    const int scx = snap->io[0x43];
    const int scy = snap->io[0x42];
    const bool aligned = (scx % CELL_PX) == 0 && (scy % CELL_PX) == 0;
    const size_t grid_bytes = (size_t)(profile->grid_rows * profile->grid_stride);

    if (!state->tracking || state->last_room < 0) {
        if (aligned) {
            reset_tracking(state, grid, scx / CELL_PX, scy / CELL_PX);
        } else {
            state->tracking = false;
        }
    } else if (room != state->last_room) {
        const int delta = room - state->last_room;
        int dx = 0, dy = 0;
        if (delta == 1) dx = profile->grid_cols;
        else if (delta == -1) dx = -profile->grid_cols;
        else if (delta == 16) dy = profile->grid_rows;
        else if (delta == -16) dy = -profile->grid_rows;
        if (dx == 0 && dy == 0) {
            if (aligned) {
                reset_tracking(state, grid, scx / CELL_PX, scy / CELL_PX);
            } else {
                state->tracking = false;
            }
        } else {
            state->origin_x = (state->origin_x + dx) & (MAP_CELLS - 1);
            state->origin_y = (state->origin_y + dy) & (MAP_CELLS - 1);
            for (int r = 0; r < profile->grid_rows; ++r) {
                for (int c = 0; c < profile->grid_cols; ++c) {
                    Cell* cell = cell_at(state, state->origin_x + c, state->origin_y + r);
                    cell->pending_obj = grid[r * profile->grid_stride + c];
                    cell->pending = true;
                    cell->pending_room = true;
                    cell->pending_age = 0;
                    cell->pending_sig = cell_signature(snap, state->origin_x + c, state->origin_y + r);
                }
            }
            state->transition = true;
            state->transition_age = 0;
        }
    } else if (memcmp(grid, state->last_grid, grid_bytes) != 0) {
        for (int r = 0; r < profile->grid_rows; ++r) {
            for (int c = 0; c < profile->grid_cols; ++c) {
                const uint8_t value = grid[r * profile->grid_stride + c];
                if (value == state->last_grid[r * profile->grid_stride + c]) continue;
                Cell* cell = cell_at(state, state->origin_x + c, state->origin_y + r);
                const uint32_t sig = cell_signature(snap, state->origin_x + c, state->origin_y + r);
                cell->pending_obj = value;
                cell->pending = true;
                cell->pending_room = false;
                cell->pending_age = 0;
                cell->pending_sig = sig;
                if (cell->valid && sig != cell->sig) {
                    commit(state, cell, sig); /* the tiles changed first */
                }
            }
        }
    }
    memcpy(state->last_grid, grid, grid_bytes);
    state->last_room = room;

    if (state->tracking) {
        const bool landed = aligned && (scx / CELL_PX) == state->origin_x &&
                            (scy / CELL_PX) == state->origin_y;
        if (state->transition) {
            if (landed) {
                for (int i = 0; i < MAP_CELLS * MAP_CELLS; ++i) {
                    Cell* cell = &state->cells[i];
                    if (cell->pending && cell->pending_room) {
                        commit(state, cell, cell_signature(snap, i % MAP_CELLS, i / MAP_CELLS));
                    }
                }
                state->transition = false;
            } else if (++state->transition_age > TRANSITION_TIMEOUT) {
                state->tracking = false;
            }
        } else if (aligned && !landed) {
            reset_tracking(state, grid, scx / CELL_PX, scy / CELL_PX);
        }
    }
    if (state->tracking) {
        for (int i = 0; i < MAP_CELLS * MAP_CELLS; ++i) {
            Cell* cell = &state->cells[i];
            if (!cell->pending) continue;
            const uint32_t sig = cell_signature(snap, i % MAP_CELLS, i / MAP_CELLS);
            if (sig != cell->pending_sig ||
                (!cell->pending_room && ++cell->pending_age > PENDING_TIMEOUT)) {
                commit(state, cell, sig);
            }
        }
        memcpy(snap->cells, state->cells, sizeof(snap->cells));
    }
    snap->valid = state->tracking;
}

/* ------------------------------------------------------------------------ */
/* Scene building                                                           */

static uint8_t bg_shade(const Snapshot* snap, int tx, int ty) {
    const uint8_t index = snap->vram[tilemap_base(snap->io) + (ty >> 3) * 32 + (tx >> 3)];
    const uint16_t data = (snap->io[0x40] & 0x10)
        ? (uint16_t)(index * 16)
        : (uint16_t)(0x1000 + (int8_t)index * 16);
    const int bit = 7 - (tx & 7);
    const int row = (ty & 7) * 2;
    const int color = ((snap->vram[data + row] >> bit) & 1) |
                      (((snap->vram[data + row + 1] >> bit) & 1) << 1);
    return (uint8_t)((snap->io[0x47] >> (color * 2)) & 3);
}

static const uint8_t* cutout_mask(VoxelState* state, int cx, int cy) {
    const int index = cy * MAP_CELLS + cx;
    if (state->cutout_ready[index]) {
        return state->cutout_bits[index];
    }
    uint8_t light[256];
    for (int y = 0; y < CELL_PX; ++y) {
        for (int x = 0; x < CELL_PX; ++x) {
            light[y * CELL_PX + x] = bg_shade(&state->snap, cx * CELL_PX + x, cy * CELL_PX + y) == 0;
        }
    }
    uint8_t* bits = state->cutout_bits[index];
    memset(bits, 0, 32);
    int queue[256];
    int head = 0, tail = 0;
    for (int i = 0; i < 256; ++i) {
        const int x = i % CELL_PX, y = i / CELL_PX;
        if (light[i] && (x == 0 || y == 0 || x == CELL_PX - 1 || y == CELL_PX - 1)) {
            bits[i >> 3] |= (uint8_t)(1u << (i & 7));
            queue[tail++] = i;
        }
    }
    while (head < tail) {
        const int i = queue[head++];
        const int x = i % CELL_PX, y = i / CELL_PX;
        const int next[4] = {x > 0 ? i - 1 : -1, x < CELL_PX - 1 ? i + 1 : -1,
                             y > 0 ? i - CELL_PX : -1, y < CELL_PX - 1 ? i + CELL_PX : -1};
        for (int k = 0; k < 4; ++k) {
            const int n = next[k];
            if (n >= 0 && light[n] && !(bits[n >> 3] & (1u << (n & 7)))) {
                bits[n >> 3] |= (uint8_t)(1u << (n & 7));
                queue[tail++] = n;
            }
        }
    }
    state->cutout_ready[index] = true;
    return bits;
}

static int class_of_cell(const Snapshot* snap, const Profile* profile, int cx, int cy) {
    const Cell* cell = &snap->cells[(cy & 15) * MAP_CELLS + (cx & 15)];
    const uint32_t sig = cell_signature(snap, cx, cy);
    if (cell->pending && sig != cell->pending_sig) {
        return profile->object_class[cell->pending_obj];
    }
    if (!cell->valid) {
        return NO_CLASS;
    }
    if (cell->sig != sig && !cell->pending) {
        return OVERLAY_CLASS;
    }
    return profile->object_class[cell->obj];
}

/* Chebyshev distance from each pixel of `wanted` to the nearest other pixel. */
static void dome_distances(VoxelState* state, int wanted, int width, int height) {
    static uint16_t dist[GB_FRAMEBUFFER_SIZE];
    const uint16_t far = 1000;
    for (int i = 0; i < width * height; ++i) {
        dist[i] = state->cls[i] == wanted ? far : 0;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint16_t* d = &dist[y * width + x];
            if (!*d) continue;
            if (x > 0 && dist[y * width + x - 1] + 1 < *d) *d = dist[y * width + x - 1] + 1;
            if (y > 0) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    if (nx >= 0 && nx < width && dist[(y - 1) * width + nx] + 1 < *d) {
                        *d = dist[(y - 1) * width + nx] + 1;
                    }
                }
            }
        }
    }
    for (int y = height - 1; y >= 0; --y) {
        for (int x = width - 1; x >= 0; --x) {
            uint16_t* d = &dist[y * width + x];
            if (!*d) continue;
            if (x < width - 1 && dist[y * width + x + 1] + 1 < *d) *d = dist[y * width + x + 1] + 1;
            if (y < height - 1) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    if (nx >= 0 && nx < width && dist[(y + 1) * width + nx] + 1 < *d) {
                        *d = dist[(y + 1) * width + nx] + 1;
                    }
                }
            }
        }
    }
    for (int i = 0; i < width * height; ++i) {
        if (state->cls[i] == wanted) {
            state->dome[i] = (uint8_t)(dist[i] > 255 ? 255 : dist[i]);
        }
    }
}

static int16_t strip_for(VoxelState* state, int index) {
    if (state->strip[index] >= 0) {
        return state->strip[index];
    }
    if (state->strip_count >= MAX_STRIPS) {
        return -1;
    }
    const int16_t strip = (int16_t)state->strip_count++;
    memset(state->strips[strip], 0xFF, MAX_DEPTH);
    state->strip[index] = strip;
    return strip;
}

static int set_top(VoxelState* state, int index, int top, uint8_t shade) {
    if (top > MAX_DEPTH) top = MAX_DEPTH;
    state->top[index] = (uint8_t)top;
    state->top_shade[index] = shade;
    return top;
}

/* Vertical run of cells sharing `cls` around (cx, cy): cells above and below. */
static void facade_run(const Snapshot* snap, const Profile* profile, int cx, int cy, int cls,
                       int* up, int* down) {
    *up = 0;
    while (*up < 8 && class_of_cell(snap, profile, cx, cy - *up - 1) == cls) (*up)++;
    *down = 0;
    while (*down < 8 && class_of_cell(snap, profile, cx, cy + *down + 1) == cls) (*down)++;
}

/* ---- props: objects drawn as boxes with their art wrapped around ---- */

static void split_prop_block(VoxelState* state, PropBlock* block) {
    const VoxelClass* cls = &state->profile.classes[block->cls];
    const int n = block->size;
    int queue[PROP_MAX_PX * PROP_MAX_PX];
    /* Background: the lightest shade reachable from the art's edge. */
    for (int i = 0; i < n * n; ++i) block->part[i] = -2;
    int head = 0, tail = 0;
    for (int i = 0; i < n * n; ++i) {
        const int x = i % n, y = i / n;
        if (block->shade[i] == 0 && (x == 0 || y == 0 || x == n - 1 || y == n - 1)) {
            block->part[i] = -1;
            queue[tail++] = i;
        }
    }
    while (head < tail) {
        const int i = queue[head++];
        const int x = i % n, y = i / n;
        const int next[4] = {x > 0 ? i - 1 : -1, x < n - 1 ? i + 1 : -1, y > 0 ? i - n : -1, y < n - 1 ? i + n : -1};
        for (int k = 0; k < 4; ++k) {
            if (next[k] >= 0 && block->part[next[k]] == -2 && block->shade[next[k]] == 0) {
                block->part[next[k]] = -1;
                queue[tail++] = next[k];
            }
        }
    }
    /* Objects: 8-connected groups of the remaining pixels. */
    int parts = 0;
    for (int start = 0; start < n * n; ++start) {
        if (block->part[start] != -2) continue;
        const int8_t id = (int8_t)(parts < 100 ? parts : 100);
        head = tail = 0;
        block->part[start] = id;
        queue[tail++] = start;
        while (head < tail) {
            const int i = queue[head++];
            const int x = i % n, y = i / n;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
                    const int j = ny * n + nx;
                    if (block->part[j] == -2) {
                        block->part[j] = id;
                        queue[tail++] = j;
                    }
                }
            }
        }
        if (tail < PROP_MIN_PIXELS) {
            for (int k = 0; k < tail; ++k) block->part[queue[k]] = -1;
            continue;
        }
        parts++;
        if (state->prop_count >= MAX_PROPS) continue;

        const int top_rows = cls->top_rows < n ? cls->top_rows : n;
        Prop prop;
        memset(&prop, 0, sizeof(prop));
        prop.block = (uint8_t)(block - state->prop_blocks);
        prop.part = id;
        /* Body: columns present in at least half of the side rows (a trunk, not its roots). */
        int column_rows[PROP_MAX_PX] = {0};
        int side_rows = 0, last = -1;
        for (int y = top_rows; y < n; ++y) {
            bool any = false;
            for (int x = 0; x < n; ++x) {
                if (block->part[y * n + x] == id) {
                    column_rows[x]++;
                    any = true;
                }
            }
            if (any) {
                side_rows++;
                last = y;
            }
        }
        int x0 = n, x1 = -1;
        for (int x = 0; x < n; ++x) {
            if (side_rows > 0 && column_rows[x] * 2 >= side_rows) {
                if (x < x0) x0 = x;
                x1 = x;
            }
        }
        prop.body_x0 = (int8_t)(x0 < n ? x0 : 1);
        prop.body_x1 = (int8_t)(x1 >= 0 ? x1 : 0);
        prop.body_last = (int8_t)last;
        int tx0 = n, tx1 = -1, ty0 = n, ty1 = -1;
        for (int y = 0; y < top_rows; ++y) {
            for (int x = 0; x < n; ++x) {
                if (block->part[y * n + x] != id) continue;
                if (x < tx0) tx0 = x;
                if (x > tx1) tx1 = x;
                if (y < ty0) ty0 = y;
                if (y > ty1) ty1 = y;
            }
        }
        prop.top_x0 = (int8_t)(tx1 >= 0 ? tx0 : 1);
        prop.top_x1 = (int8_t)(tx1 >= 0 ? tx1 : 0);
        prop.top_y0 = (int8_t)(ty1 >= 0 ? ty0 : 0);
        prop.top_y1 = (int8_t)(ty1 >= 0 ? ty1 : 0);
        int shade_counts[4] = {0};
        for (int y = 0; y < top_rows; ++y) {
            for (int x = 0; x < n; ++x) {
                if (block->part[y * n + x] == id) shade_counts[block->shade[y * n + x]]++;
            }
        }
        prop.top_fill = 1;
        for (int k = 0; k < 3; ++k) {
            if (shade_counts[k] > shade_counts[prop.top_fill]) prop.top_fill = (uint8_t)k;
        }
        if (x1 < 0 && tx1 < 0) continue;
        state->props[state->prop_count++] = prop;
    }
}

static void register_prop_block(VoxelState* state, int cls_index, int cell_x, int cell_y, int sx, int sy) {
    const uint16_t cell = (uint16_t)(cell_y * MAP_CELLS + cell_x);
    for (int b = 0; b < state->prop_block_count; ++b) {
        if (state->prop_blocks[b].cell == cell) return;
    }
    if (state->prop_block_count >= MAX_PROP_BLOCKS) return;
    PropBlock* block = &state->prop_blocks[state->prop_block_count++];
    block->cell = cell;
    block->sx = (int16_t)sx;
    block->sy = (int16_t)sy;
    block->cls = (uint8_t)cls_index;
    block->size = (uint8_t)(state->profile.classes[cls_index].span * CELL_PX);
    for (int y = 0; y < block->size; ++y) {
        for (int x = 0; x < block->size; ++x) {
            /* Cells of the art that hold another class (the screen edge's stale
             * tilemap, a neighbor) count as background. */
            const bool own = class_of_cell(&state->snap, &state->profile, (cell_x + x / CELL_PX) & 15,
                                           (cell_y + y / CELL_PX) & 15) == cls_index;
            block->shade[y * block->size + x] = own
                ? bg_shade(&state->snap, (cell_x * CELL_PX + x) & 0xFF, (cell_y * CELL_PX + y) & 0xFF)
                : 0;
        }
    }
    split_prop_block(state, block);
}

static void build_scene(VoxelState* state, uint32_t* overlay_cells, uint32_t* visible_cells) {
    const Profile* profile = &state->profile;
    const Snapshot* snap = &state->snap;
    const int width = profile->playfield_width;
    const int height = profile->playfield_height;
    const int ground = profile->ground;
    const int scx = snap->io[0x43];
    const int scy = snap->io[0x42];

    memset(state->cutout_ready, 0, sizeof(state->cutout_ready));
    state->strip_count = 0;
    state->cube_count = 0;
    state->prop_block_count = 0;
    state->prop_count = 0;

    uint8_t seen[MAP_CELLS * MAP_CELLS];
    memset(seen, 0, sizeof(seen));
    *overlay_cells = 0;
    *visible_cells = 0;
    bool dome_used[MAX_CLASSES];
    memset(dome_used, 0, sizeof(dome_used));

    for (int y = 0; y < height; ++y) {
        const int ty = (y + scy) & 0xFF;
        for (int x = 0; x < width; ++x) {
            const int tx = (x + scx) & 0xFF;
            const int i = y * width + x;
            const int cx = tx >> 4, cy = ty >> 4;
            state->shade[i] = bg_shade(snap, tx, ty);
            state->cls[i] = (int8_t)class_of_cell(snap, profile, cx, cy);
            state->strip[i] = -1;
            set_top(state, i, ground, state->shade[i]);
            if (!seen[cy * MAP_CELLS + cx]) {
                seen[cy * MAP_CELLS + cx] = 1;
                (*visible_cells)++;
                if (state->cls[i] == OVERLAY_CLASS) (*overlay_cells)++;
            }
            if (state->cls[i] >= 0 && profile->classes[state->cls[i]].type == SHAPE_DOME) {
                dome_used[state->cls[i]] = true;
            }
        }
    }
    for (int c = 0; c < profile->class_count; ++c) {
        if (dome_used[c]) dome_distances(state, c, width, height);
    }

    for (int y = 0; y < height; ++y) {
        const int ty = (y + scy) & 0xFF;
        for (int x = 0; x < width; ++x) {
            const int tx = (x + scx) & 0xFF;
            const int i = y * width + x;
            const int8_t cls_index = state->cls[i];
            const uint8_t shade = state->shade[i];
            if (cls_index < 0) {
                continue; /* ground, unknown, or overlay: already flat */
            }
            const VoxelClass* cls = &profile->classes[cls_index];
            const int cx = tx >> 4, cy = ty >> 4;
            if (cls->cutout && (cls->type == SHAPE_BOX || cls->type == SHAPE_DOME)) {
                const int bit = (ty & 15) * CELL_PX + (tx & 15);
                if (cutout_mask(state, cx, cy)[bit >> 3] & (1u << (bit & 7))) {
                    continue;
                }
            }
            switch (cls->type) {
                case SHAPE_GROUND:
                    break;
                case SHAPE_PROP: {
                    const Cell* cell = &snap->cells[cy * MAP_CELLS + cx];
                    const uint8_t obj = cell->pending && cell_signature(snap, cx, cy) != cell->pending_sig
                        ? cell->pending_obj : cell->obj;
                    const int quadrant = profile->object_quadrant[obj];
                    const int qx = quadrant & 15, qy = quadrant >> 4;
                    register_prop_block(state, cls_index, (cx - qx) & 15, (cy - qy) & 15,
                                        x - (tx & 15) - qx * CELL_PX, y - (ty & 15) - qy * CELL_PX);
                    set_top(state, i, ground, 0); /* the object is drawn by render_props */
                    break;
                }
                case SHAPE_RELIEF:
                    if (shade >= cls->min_shade) set_top(state, i, ground + cls->raise, shade);
                    break;
                case SHAPE_SUNKEN:
                    set_top(state, i, 1, shade);
                    break;
                case SHAPE_BOX:
                    set_top(state, i, ground + cls->height, shade);
                    break;
                case SHAPE_DOME: {
                    float d = (float)state->dome[i] / (float)cls->radius;
                    if (d > 1.0f) d = 1.0f;
                    float h = (float)cls->height * (float)sqrt(1.0 - (1.0 - d) * (1.0 - d));
                    if (shade == 3) h -= (float)cls->groove;
                    int hi = (int)(h + 0.5f);
                    set_top(state, i, ground + (hi < 1 ? 1 : hi), shade);
                    break;
                }
                case SHAPE_FACADE: {
                    int up = 0, down = 0;
                    facade_run(snap, profile, cx, cy, cls_index, &up, &down);
                    const int total = up + down + 1;
                    const int wall_rows = cls->rows < total ? cls->rows : total;
                    const int wall_px = wall_rows * CELL_PX;
                    const int cell_top = y - (ty & 15);
                    const int first_top = cell_top - up * CELL_PX;
                    const int block_end = cell_top + (down + 1) * CELL_PX;
                    const int wall_top = block_end - wall_px;
                    if (y < first_top + wall_px && y != block_end - 1) {
                        set_top(state, i, ground, 0); /* footprint the roof slid off */
                    }
                    if (y >= wall_top) {
                        const int wy = block_end - 1;
                        if (wy >= 0 && wy < height) {
                            const int wi = wy * width + x;
                            const int16_t strip = strip_for(state, wi);
                            if (strip >= 0) {
                                const int z = ground + (block_end - 1 - y);
                                if (z < MAX_DEPTH) state->strips[strip][z] = shade;
                                if (y == block_end - 1) {
                                    for (int zz = 0; zz < ground; ++zz) state->strips[strip][zz] = shade;
                                }
                            }
                            if (y == wall_top) {
                                set_top(state, wi, ground + wall_px, shade);
                            }
                        }
                    } else if (y + wall_px < height) {
                        set_top(state, (y + wall_px) * width + x, ground + wall_px, shade);
                    }
                    break;
                }
            }
        }
    }

    /* Sprites: group touching OAM entries and stand each group on its lowest row. */
    if (snap->io[0x40] & 0x02) {
        const int sprite_h = (snap->io[0x40] & 0x04) ? 16 : 8;
        int sx[40], sy[40], parent[40], count = 0, index_of[40];
        for (int n = 0; n < 40; ++n) {
            const int oy = snap->oam[n * 4] - 16, ox = snap->oam[n * 4 + 1] - 8;
            if (oy <= -sprite_h || oy >= height || ox <= -8 || ox >= width) continue;
            sx[count] = ox;
            sy[count] = oy;
            parent[count] = count;
            index_of[count] = n;
            count++;
        }
        for (int a = 0; a < count; ++a) {
            for (int b = a + 1; b < count; ++b) {
                const bool touch_x = sx[a] <= sx[b] + 8 && sx[b] <= sx[a] + 8;
                const bool touch_y = sy[a] <= sy[b] + sprite_h && sy[b] <= sy[a] + sprite_h;
                if (touch_x && touch_y) {
                    int ra = a, rb = b;
                    while (parent[ra] != ra) ra = parent[ra];
                    while (parent[rb] != rb) rb = parent[rb];
                    parent[rb] = ra;
                }
            }
        }
        int bottom[40];
        for (int a = 0; a < count; ++a) bottom[a] = -1000;
        for (int a = 0; a < count; ++a) {
            int r = a;
            while (parent[r] != r) r = parent[r];
            if (sy[a] + sprite_h - 1 > bottom[r]) bottom[r] = sy[a] + sprite_h - 1;
        }
        for (int a = 0; a < count; ++a) {
            int r = a;
            while (parent[r] != r) r = parent[r];
            int plane = bottom[r];
            if (plane >= height) plane = height - 1;
            if (plane < 0) continue;
            const int n = index_of[a];
            const uint8_t tile = snap->oam[n * 4 + 2];
            const uint8_t flags = snap->oam[n * 4 + 3];
            const uint8_t obp = snap->io[(flags & 0x10) ? 0x49 : 0x48];
            for (int py = 0; py < sprite_h; ++py) {
                const int screen_y = sy[a] + py;
                if (screen_y >= height) break;
                const int src_y = (flags & 0x40) ? sprite_h - 1 - py : py;
                const uint16_t data = (uint16_t)(((tile + (src_y >> 3)) & 0xFF) * 16);
                const int row = (src_y & 7) * 2;
                for (int px = 0; px < 8; ++px) {
                    const int x = sx[a] + px;
                    if (x < 0 || x >= width) continue;
                    const int bit = 7 - ((flags & 0x20) ? 7 - px : px);
                    const int color = ((snap->vram[data + row] >> bit) & 1) |
                                      (((snap->vram[data + row + 1] >> bit) & 1) << 1);
                    if (!color || state->cube_count >= MAX_CUBES) continue;
                    const int z = ground + (plane - screen_y);
                    if (z < 0 || z >= MAX_DEPTH) continue;
                    state->cube_x[state->cube_count] = (uint16_t)x;
                    state->cube_y[state->cube_count] = (uint16_t)plane;
                    state->cube_z[state->cube_count] = (uint8_t)z;
                    state->cube_shade[state->cube_count] = (uint8_t)((obp >> (color * 2)) & 3);
                    state->cube_count++;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Rendering                                                                */

typedef struct Vertex {
    float x, y, iz;
} Vertex;

static Vertex project(const VoxelState* state, float wx, float wy, float wz) {
    const float ry = wy - state->cam_y;
    const float rz = wz - state->cam_z;
    const float vx = wx - (float)state->profile.playfield_width * 0.5f;
    const float vy = ry * state->sin_p + rz * state->cos_p;
    float vz = ry * state->cos_p - rz * state->sin_p;
    if (vz < 1.0f) vz = 1.0f;
    Vertex v;
    v.x = state->fit_x + state->fit_scale * vx / vz;
    v.y = state->fit_y - state->fit_scale * vy / vz;
    v.iz = 1.0f / vz;
    return v;
}

static void raster_triangle(VoxelState* state, Vertex a, Vertex b, Vertex c, uint32_t color) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area > -1e-6f && area < 1e-6f) return;
    const int width = (int)GB_VOXEL_OUTPUT_WIDTH;
    const int height = state->region_height;
    float min_x = a.x < b.x ? a.x : b.x; if (c.x < min_x) min_x = c.x;
    float max_x = a.x > b.x ? a.x : b.x; if (c.x > max_x) max_x = c.x;
    float min_y = a.y < b.y ? a.y : b.y; if (c.y < min_y) min_y = c.y;
    float max_y = a.y > b.y ? a.y : b.y; if (c.y > max_y) max_y = c.y;
    int x0 = (int)floor(min_x), x1 = (int)ceil(max_x);
    int y0 = (int)floor(min_y), y1 = (int)ceil(max_y);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width - 1) x1 = width - 1;
    if (y1 > height - 1) y1 = height - 1;
    const float inv = 1.0f / area;
    for (int py = y0; py <= y1; ++py) {
        const float fy = (float)py + 0.5f;
        for (int px = x0; px <= x1; ++px) {
            const float fx = (float)px + 0.5f;
            float w0 = ((c.x - b.x) * (fy - b.y) - (c.y - b.y) * (fx - b.x)) * inv;
            float w1 = ((a.x - c.x) * (fy - c.y) - (a.y - c.y) * (fx - c.x)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
            const float depth = w0 * a.iz + w1 * b.iz + w2 * c.iz;
            const size_t index = (size_t)py * (size_t)width + (size_t)px;
            if (depth > state->depth[index]) {
                state->depth[index] = depth;
                state->out[index] = color;
            }
        }
    }
}

static uint32_t lit(const VoxelState* state, uint8_t shade, float factor) {
    const float r = state->light_rgb[shade & 3][0] * factor;
    const float g = state->light_rgb[shade & 3][1] * factor;
    const float b = state->light_rgb[shade & 3][2] * factor;
    return 0xFF000000u | ((uint32_t)(r > 255 ? 255 : r) << 16) |
           ((uint32_t)(g > 255 ? 255 : g) << 8) | (uint32_t)(b > 255 ? 255 : b);
}

static void quad(VoxelState* state, float p[4][3], uint32_t color) {
    Vertex v[4];
    for (int i = 0; i < 4; ++i) v[i] = project(state, p[i][0], p[i][1], p[i][2]);
    raster_triangle(state, v[0], v[1], v[2], color);
    raster_triangle(state, v[0], v[2], v[3], color);
    state->faces++;
}

static void top_face(VoxelState* state, float x0, float x1, float y0, float z, uint32_t color) {
    float p[4][3] = {{x0, y0, z}, {x1, y0, z}, {x1, y0 + 1, z}, {x0, y0 + 1, z}};
    quad(state, p, color);
}

static void front_face(VoxelState* state, float x0, float x1, float y0, float za, float zb,
                       uint32_t color) {
    float p[4][3] = {{x0, y0, za}, {x1, y0, za}, {x1, y0, zb}, {x0, y0, zb}};
    quad(state, p, color);
}

static void side_face(VoxelState* state, float x, float y0, float za, float zb, uint32_t color) {
    float p[4][3] = {{x, y0, za}, {x, y0 + 1, za}, {x, y0 + 1, zb}, {x, y0, zb}};
    quad(state, p, color);
}

#define TOP_LIGHT 1.0f
#define FRONT_LIGHT 0.78f
#define SIDE_LIGHT 0.62f

/* The shade of art pixel (u, v) of a prop, or of the nearest pixel of the
 * same object in that row within columns [x0, x1], then in nearby rows. */
static uint8_t prop_texel(const PropBlock* block, int part, int u, int v, int x0, int x1) {
    const int n = block->size;
    if (u < 0) u = 0;
    if (u >= n) u = n - 1;
    if (v < 0) v = 0;
    if (v >= n) v = n - 1;
    for (int dv = 0; dv < n; ++dv) {
        for (int sign = 0; sign < (dv ? 2 : 1); ++sign) {
            const int row = sign ? v - dv : v + dv;
            if (row < 0 || row >= n) continue;
            for (int du = 0; du < n; ++du) {
                const int left = u - du, right = u + du;
                if (left >= x0 && left >= 0 && block->part[row * n + left] == part) return block->shade[row * n + left];
                if (right <= x1 && right < n && block->part[row * n + right] == part) return block->shade[row * n + right];
                if (left < x0 && right > x1) break;
            }
        }
    }
    return 3;
}

/* The top art stretched square: pixels outside the object's round outline take
 * its main color instead of smearing the outline into the corners. */
static uint8_t prop_top_texel(const PropBlock* block, int part, int u, int v, uint8_t fill) {
    const int n = block->size;
    if (u < 0 || v < 0 || u >= n || v >= n || block->part[v * n + u] != part) return fill;
    return block->shade[v * n + u];
}

/* Whether footprint cell (i, j) of a w x d prop lies inside its ellipse. */
static bool prop_inside(int i, int j, int w, int d) {
    if (w <= 2 || d <= 2) return true;
    const float fx = ((float)i + 0.5f - (float)w * 0.5f) / ((float)w * 0.5f);
    const float fy = ((float)j + 0.5f - (float)d * 0.5f) / ((float)d * 0.5f);
    return fx * fx + fy * fy <= 1.08f;
}

/* A round prop: an elliptical column whose sides wrap art rows counting up
 * from `side_row_bottom` (art column follows the X position, so the front
 * reads like the 2D art) and whose top shows art rows [top_y0, top_y1]
 * stretched over the ellipse. Columns [x0, x1] of the art set the width; the
 * depth equals the width, centred on the art. Sides are shaded by their
 * facing so the column reads as round. */
static void render_prop_box(VoxelState* state, const PropBlock* block, int part, int x0, int x1,
                            int side_row_bottom, int levels, int side_step, float z0,
                            bool draw_top, int top_x0, int top_x1, int top_y0, int top_y1,
                            uint8_t top_fill) {
    const int height = state->profile.playfield_height;
    const float cam_x = (float)state->profile.playfield_width * 0.5f;
    const int w = x1 - x0 + 1;
    const int d = w;
    const int centre = block->sy + block->size / 2;
    const int back_row = centre - d / 2;
    const float front_y = (float)(height - (back_row + d));  /* world Y of the front-most row */
    const float left_x = (float)(block->sx + x0);
    /* Light from the front, a little from the left. */
    const float light_x = -0.33f, light_y = -0.94f;

    for (int j = 0; j < d; ++j) {  /* j = 0 is the front row */
        for (int i = 0; i < w; ++i) {
            if (!prop_inside(i, j, w, d)) continue;
            const float x = left_x + (float)i;
            const float y = front_y + (float)j;
            const bool open_front = j == 0 || !prop_inside(i, j - 1, w, d);
            const bool open_left = (i == 0 || !prop_inside(i - 1, j, w, d)) && x >= cam_x;
            const bool open_right = (i == w - 1 || !prop_inside(i + 1, j, w, d)) && x + 1.0f <= cam_x;
            if (!open_front && !open_left && !open_right) continue;

            float nx = ((float)i + 0.5f - (float)w * 0.5f) / ((float)w * 0.5f);
            float ny = ((float)j + 0.5f - (float)d * 0.5f) / ((float)d * 0.5f);
            const float len = (float)sqrt(nx * nx + ny * ny);
            if (len > 1e-3f) {
                nx /= len;
                ny /= len;
            } else {
                nx = 0.0f;
                ny = -1.0f;
            }
            float facing = nx * light_x + ny * light_y;
            if (facing < 0.0f) facing = 0.0f;
            const float light = 0.5f + 0.4f * facing;

            const int u = x0 + i;
            for (int k = 0; k < levels;) {
                const uint8_t shade = prop_texel(block, part, u, side_row_bottom - k * side_step, x0, x1);
                int top = k + 1;
                while (top < levels && prop_texel(block, part, u, side_row_bottom - top * side_step, x0, x1) == shade) top++;
                const float za = z0 + (float)k, zb = z0 + (float)top;
                const uint32_t color = lit(state, shade, light);
                if (open_front) front_face(state, x, x + 1.0f, y, za, zb, color);
                if (open_left || open_right) {
                    const float fx = open_left ? x : x + 1.0f;
                    float p[4][3] = {{fx, y, za}, {fx, y + 1.0f, za}, {fx, y + 1.0f, zb}, {fx, y, zb}};
                    quad(state, p, color);
                }
                k = top;
            }
        }
    }
    if (!draw_top) return;
    const float zt = z0 + (float)levels;
    const int tw = top_x1 - top_x0 + 1, th = top_y1 - top_y0 + 1;
    for (int j = 0; j < d; ++j) {
        const int v = top_y1 - (j * th) / d;  /* the art's bottom row at the front */
        for (int i = 0; i < w;) {
            if (!prop_inside(i, j, w, d)) {
                i++;
                continue;
            }
            const uint8_t shade = prop_top_texel(block, part, top_x0 + (i * tw) / w, v, top_fill);
            int end = i + 1;
            while (end < w && prop_inside(end, j, w, d) &&
                   prop_top_texel(block, part, top_x0 + (end * tw) / w, v, top_fill) == shade) {
                end++;
            }
            top_face(state, left_x + (float)i, left_x + (float)end, front_y + (float)j, zt, lit(state, shade, TOP_LIGHT));
            i = end;
        }
    }
}

static void render_props(VoxelState* state) {
    const float ground = (float)state->profile.ground;
    for (int n = 0; n < state->prop_count; ++n) {
        const Prop* prop = &state->props[n];
        const PropBlock* block = &state->prop_blocks[prop->block];
        const VoxelClass* cls = &state->profile.classes[block->cls];
        const bool has_body = prop->body_x0 <= prop->body_x1 && prop->body_last >= cls->top_rows;
        const bool has_top = prop->top_x0 <= prop->top_x1;
        const int body_levels = has_body ? prop->body_last - cls->top_rows + 1 : 0;
        if (has_body) {
            const bool top_on_body = cls->canopy == 0;
            render_prop_box(state, block, prop->part, prop->body_x0, prop->body_x1, prop->body_last,
                            body_levels, 1, ground, top_on_body,
                            has_top ? prop->top_x0 : prop->body_x0, has_top ? prop->top_x1 : prop->body_x1,
                            has_top ? prop->top_y0 : cls->top_rows, has_top ? prop->top_y1 : cls->top_rows,
                            prop->top_fill);
        }
        if (has_top && (cls->canopy > 0 || !has_body)) {
            const int levels = cls->canopy > 0 ? cls->canopy : 1;
            const int rows = prop->top_y1 - prop->top_y0 + 1;
            const int step = rows >= levels ? 1 : 0;
            render_prop_box(state, block, prop->part, prop->top_x0, prop->top_x1, prop->top_y1,
                            levels < rows ? levels : rows, step, ground + (float)body_levels, true,
                            prop->top_x0, prop->top_x1, prop->top_y0, prop->top_y1, prop->top_fill);
        }
    }
}

static void render_scene(VoxelState* state) {
    const Profile* profile = &state->profile;
    const int width = profile->playfield_width;
    const int height = profile->playfield_height;
    const float cam_x = (float)width * 0.5f;

    for (int y = 0; y < height; ++y) {
        const float wy = (float)(height - 1 - y);
        /* top faces, merged along equal runs */
        for (int x = 0; x < width;) {
            const int i = y * width + x;
            int end = x + 1;
            while (end < width && state->top[i + end - x] == state->top[i] &&
                   state->top_shade[i + end - x] == state->top_shade[i]) {
                end++;
            }
            top_face(state, (float)x, (float)end, wy, (float)state->top[i],
                     lit(state, state->top_shade[i], TOP_LIGHT));
            x = end;
        }
        /* front faces above the neighbor in front */
        for (int x = 0; x < width;) {
            const int i = y * width + x;
            const int front = y + 1 < height ? state->top[i + width] : 0;
            const int top = state->top[i];
            if (top <= front) { x++; continue; }
            if (state->strip[i] >= 0) {
                const uint8_t* strip = state->strips[state->strip[i]];
                for (int z = front; z < top; ++z) {
                    const uint8_t shade = strip[z] != 0xFF ? strip[z] : state->top_shade[i];
                    front_face(state, (float)x, (float)x + 1, wy, (float)z, (float)z + 1,
                               lit(state, shade, FRONT_LIGHT));
                }
                x++;
                continue;
            }
            int end = x + 1;
            while (end < width) {
                const int j = y * width + end;
                const int jf = y + 1 < height ? state->top[j + width] : 0;
                if (state->strip[j] >= 0 || state->top[j] != top || jf != front ||
                    state->top_shade[j] != state->top_shade[i]) {
                    break;
                }
                end++;
            }
            front_face(state, (float)x, (float)end, wy, (float)front, (float)top,
                       lit(state, state->top_shade[i], FRONT_LIGHT));
            x = end;
        }
        /* side faces the camera can see */
        for (int x = 0; x < width; ++x) {
            const int i = y * width + x;
            const int top = state->top[i];
            if ((float)x >= cam_x) {
                const int left = x > 0 ? state->top[i - 1] : 0;
                if (top > left) side_face(state, (float)x, wy, (float)left, (float)top,
                                          lit(state, state->top_shade[i], SIDE_LIGHT));
            }
            if ((float)x + 1 <= cam_x) {
                const int right = x + 1 < width ? state->top[i + 1] : 0;
                if (top > right) side_face(state, (float)x + 1, wy, (float)right, (float)top,
                                           lit(state, state->top_shade[i], SIDE_LIGHT));
            }
        }
    }

    render_props(state);

    for (int n = 0; n < state->cube_count; ++n) {
        const float x = (float)state->cube_x[n];
        const float wy = (float)(height - 1 - state->cube_y[n]);
        const float z = (float)state->cube_z[n];
        const uint8_t shade = state->cube_shade[n];
        top_face(state, x, x + 1, wy, z + 1, lit(state, shade, TOP_LIGHT));
        front_face(state, x, x + 1, wy, z, z + 1, lit(state, shade, FRONT_LIGHT));
        if (x >= cam_x) side_face(state, x, wy, z, z + 1, lit(state, shade, SIDE_LIGHT));
        else side_face(state, x + 1, wy, z, z + 1, lit(state, shade, SIDE_LIGHT));
    }
}

static void setup_camera(VoxelState* state) {
    const Profile* profile = &state->profile;
    const float pi = 3.14159265f;
    const float pitch = profile->pitch * pi / 180.0f;
    const float width = (float)profile->playfield_width;
    const float height = (float)profile->playfield_height;
    const float distance = profile->distance * height;
    state->sin_p = (float)sin(pitch);
    state->cos_p = (float)cos(pitch);
    state->cam_y = height * 0.5f - distance * state->cos_p;
    state->cam_z = distance * state->sin_p;

    /* Fit with unit scale first, then scale so the near edge spans the width
     * and the far edge (plus some height) stays inside the region. */
    state->fit_scale = 1.0f;
    state->fit_x = 0.0f;
    state->fit_y = 0.0f;
    const Vertex near_left = project(state, 0.0f, 0.0f, 0.0f);
    const Vertex near_right = project(state, width, 0.0f, 0.0f);
    const Vertex far_top = project(state, 0.0f, height, 20.0f);
    const float span_x = near_right.x - near_left.x;
    const float span_y = near_left.y - far_top.y;
    const float region_w = (float)GB_VOXEL_OUTPUT_WIDTH;
    const float region_h = (float)state->region_height;
    float scale = region_w / span_x;
    if (span_y * scale > region_h) scale = region_h / span_y;
    state->fit_scale = scale;
    state->fit_x = region_w * 0.5f;
    state->fit_y = region_h - near_left.y * scale;
}

bool gbrt_voxel_compose(const uint32_t* base, const uint32_t shade_rgb[4], uint32_t* output) {
    VoxelState* state = g_voxel;
    if (!state || !state->enabled || !base || !output || !shade_rgb || !state->snap.valid) {
        if (state) state->stats.frames_fallback++;
        return false;
    }
    const struct timespec start = {0, 0};
    struct timespec t0 = start;
    timespec_get(&t0, TIME_UTC);

    uint32_t overlay_cells = 0, visible_cells = 0;
    build_scene(state, &overlay_cells, &visible_cells);
    state->stats.overlay_cells = overlay_cells;
    if (visible_cells == 0 || overlay_cells * 100u > visible_cells * OVERLAY_FALLBACK_PERCENT) {
        state->stats.frames_fallback++;
        return false;
    }

    const Profile* profile = &state->profile;
    const int out_w = (int)GB_VOXEL_OUTPUT_WIDTH;
    const int out_h = (int)GB_VOXEL_OUTPUT_HEIGHT;
    const int scale = (int)GB_VOXEL_OUTPUT_SCALE;
    state->out = output;
    state->region_height = profile->playfield_height * scale;
    for (int s = 0; s < 4; ++s) {
        state->light_rgb[s][0] = (float)((shade_rgb[s] >> 16) & 0xFF);
        state->light_rgb[s][1] = (float)((shade_rgb[s] >> 8) & 0xFF);
        state->light_rgb[s][2] = (float)(shade_rgb[s] & 0xFF);
    }
    for (int y = 0; y < out_h; ++y) {
        /* Backdrop: the darkest shade at the top fading toward the next one. */
        const float t = (float)y / (float)state->region_height;
        const float k = t > 1.0f ? 1.0f : t;
        uint32_t sky = 0xFF000000u;
        for (int channel = 0; channel < 3; ++channel) {
            const float v = state->light_rgb[3][channel] * (1.0f - k) +
                            state->light_rgb[2][channel] * k * 0.8f;
            sky |= (uint32_t)(v > 255.0f ? 255.0f : v) << (16 - 8 * channel);
        }
        for (int x = 0; x < out_w; ++x) {
            output[y * out_w + x] = sky;
            state->depth[y * out_w + x] = 0.0f;
        }
    }
    setup_camera(state);
    state->faces = 0;
    render_scene(state);

    /* Flat overlay: window layer, rows below the playfield, and cells whose
     * tiles no longer match their room object (text boxes). */
    const Snapshot* snap = &state->snap;
    const int scx = snap->io[0x43];
    const int scy = snap->io[0x42];
    for (int y = 0; y < 144; ++y) {
        for (int x = 0; x < 160; ++x) {
            bool flat = y >= profile->playfield_height;
            const uint32_t source = snap->has_sources ? snap->sources[y * 160 + x] : 0u;
            if (!flat && snap->has_sources && GB_PIXEL_SOURCE_LAYER(source) == GB_PIXEL_LAYER_WINDOW) {
                flat = true;
            }
            if (!flat && state->cls[y * profile->playfield_width + x] == OVERLAY_CLASS) {
                flat = true;
            }
            (void)scx;
            (void)scy;
            if (!flat) continue;
            const uint32_t color = base[y * 160 + x];
            for (int dy = 0; dy < scale; ++dy) {
                uint32_t* row = output + (size_t)(y * scale + dy) * (size_t)out_w + (size_t)(x * scale);
                for (int dx = 0; dx < scale; ++dx) row[dx] = color;
            }
        }
    }

    struct timespec t1 = start;
    timespec_get(&t1, TIME_UTC);
    state->stats.compose_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                              (double)(t1.tv_nsec - t0.tv_nsec) / 1.0e6;
    state->stats.faces_drawn = state->faces;
    state->stats.frames_composed++;
    if (getenv("GBRT_VOXEL_TRACE")) {
        fprintf(stderr, "[VOXEL] compose %.2f ms faces=%u overlay_cells=%u fallbacks=%llu\n",
                state->stats.compose_ms, state->faces, overlay_cells,
                (unsigned long long)state->stats.frames_fallback);
    }
    return true;
}
