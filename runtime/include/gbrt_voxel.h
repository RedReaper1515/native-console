#ifndef GBRT_VOXEL_H
#define GBRT_VOXEL_H

/*
 * Voxel view: a host-side 3D rendering of a tile-based game's playfield.
 *
 * A voxel profile (a text file, see VOXEL_VIEW.md) tells the runtime where the
 * game keeps its grid of room objects and what shape each object ID has. When
 * a frame completes, the runtime snapshots VRAM, OAM, the I/O registers, and
 * the pixel sources, and follows the object grid through scrolling room
 * transitions. Composition rebuilds the visible playfield as columns of
 * voxels, stands sprites up as cutouts, renders the scene with a software
 * depth-buffered rasterizer, and draws window-layer pixels (HUD, menus) and
 * background cells that no longer match their room object (text boxes) flat
 * on top.
 *
 * Like texture packs, the view never feeds back into guest-visible state.
 * Composition returns false whenever the profile's conditions do not hold
 * (indoors, a menu, an unexpected scroll); the host then shows the 2D frame.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;

#define GB_VOXEL_OUTPUT_SCALE 4u
#define GB_VOXEL_OUTPUT_WIDTH (160u * GB_VOXEL_OUTPUT_SCALE)
#define GB_VOXEL_OUTPUT_HEIGHT (144u * GB_VOXEL_OUTPUT_SCALE)

typedef enum GBVoxelStatus {
    GB_VOXEL_OK = 0,
    GB_VOXEL_ERROR_ARGUMENT,
    GB_VOXEL_ERROR_IO,
    GB_VOXEL_ERROR_FORMAT,
    GB_VOXEL_ERROR_MEMORY,
    GB_VOXEL_ERROR_ROM_MISMATCH,
} GBVoxelStatus;

typedef struct GBVoxelStats {
    uint64_t frames_composed;
    uint64_t frames_fallback;   /* compose calls that returned false */
    uint32_t faces_drawn;       /* last composed frame */
    uint32_t overlay_cells;     /* last composed frame */
    double compose_ms;          /* last composed frame */
} GBVoxelStats;

/*
 * Load a profile and activate the view. On a format error, `error_line`
 * (when non-NULL) receives the 1-based line number.
 */
GBVoxelStatus gbrt_voxel_load_file(GBContext* context, const char* path, int* error_line);
GBVoxelStatus gbrt_voxel_load_memory(GBContext* context, const char* text, size_t size,
                                     int* error_line);
void gbrt_voxel_unload(void);
bool gbrt_voxel_is_loaded(void);
const char* gbrt_voxel_status_string(GBVoxelStatus status);

/* A loaded view can be switched off and on (hotkey, menu) without reloading. */
void gbrt_voxel_set_enabled(bool enabled);
bool gbrt_voxel_is_enabled(void);

/* Camera tilt in degrees above the horizon (20-89) and zoom factor. */
void gbrt_voxel_set_camera(float pitch_degrees, float distance_scale);
void gbrt_voxel_get_camera(float* pitch_degrees, float* distance_scale);

GBVoxelStats gbrt_voxel_stats(void);

/* Called by the PPU when a frame completes: snapshot and grid tracking. */
void gbrt_voxel_frame_complete(const GBContext* context);

/*
 * Render the last completed frame into `output` (GB_VOXEL_OUTPUT_WIDTH x
 * GB_VOXEL_OUTPUT_HEIGHT, 0xAARRGGBB). `base` is the 160x144 image the host
 * would otherwise show, used for the flat overlay; `shade_rgb` gives the host
 * color of DMG shades 0-3. Returns false when the view is off or does not
 * apply to this frame.
 */
bool gbrt_voxel_compose(const uint32_t* base, const uint32_t shade_rgb[4], uint32_t* output);

#ifdef __cplusplus
}
#endif

#endif
