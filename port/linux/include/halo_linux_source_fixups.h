/*
HALO_LINUX_SOURCE_FIXUPS.H

Game-only workarounds for source that MSVC accepts but clang rejects, where
editing the source itself would change the byte-matched MSVC output (see
port/linux/README.md for how each was checked).
*/

#ifndef __HALO_LINUX_SOURCE_FIXUPS_H
#define __HALO_LINUX_SOURCE_FIXUPS_H

/* rasterizer.h declares rasterizer_debug_drawing_begin(boolean opaque) while
rasterizer_xbox_debug.h declares a second `long zbias` parameter, and
rasterizer_debug.c includes both and passes two arguments. MSVC tolerates
the mismatch; the definition ignores zbias. Adding the parameter to
rasterizer.h perturbs MSVC's register allocation elsewhere, so instead every
declaration and call collapses to the one-parameter form here. */
#define rasterizer_debug_drawing_begin(opaque, ...) (rasterizer_debug_drawing_begin)(opaque)

/* frames between the 30 Hz ticks (port/linux/game/render_interpolation.c);
the platform layer reads the display.interpolation setting */
struct observer_result;
struct render_camera;
struct real_matrix4x3;
int halo_interpolation_enabled(void);
float game_time_get_tick_fraction(void);
void render_interpolation_tick(void);
void render_interpolation_reset(void);
void render_interpolation_frame_begin(void);
void render_interpolation_frame_end(void);
float render_interpolation_fraction(void);
struct real_matrix4x3 *render_interpolation_object_node_matrices(long object_index);
struct observer_result const *render_interpolation_camera(short local_player_index,
	struct observer_result const *observer);
void render_interpolation_first_person(short local_player_index, struct real_matrix4x3 *node_matrices,
	short node_count, struct render_camera const *camera);
float render_interpolation_game_time_sec(long ticks);

/* the port's menus' screen for one of ui.map's, by name, else the name
(port/linux/game/menu_tags.c) */
char const *pc_menus_screen(char const *name);
/* the Custom Edition tag cache window, or NULL unless the
game.custom_edition setting reserved it (port/linux/src/xbox_memory.c) */
void *halo_custom_edition_tag_cache(void);
/* where Halo PC keeps the channels of the pixels a Custom Edition bitmap
just arrived at (an enum custom_edition_channel_order,
port/linux/game/cache_file_formats.h), which the renderer then samples in
this build's order; forgotten together when the map goes
(port/linux/src/xbox_textures.c) */
void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order);
void halo_custom_edition_texels_forget(void);
/* whether a Halo Custom Edition map's multiplayer vehicles are chosen by
their placements' spawn flags, as in retail Halo, and whether a vehicle
placement is placed in the running game
(port/linux/game/custom_edition_objects.c) */
struct scenario_object_datum;
unsigned char custom_edition_vehicles_by_placement(void);
unsigned char custom_edition_vehicle_placement_allowed(struct scenario_object_datum const *placement);

/* the width of the screen the game draws, 480 lines tall: the device's or
the display's shape, or 640 (port/linux/src/d3d8_device.c) */
long halo_screen_width(void);
/* takes up a new width between frames (F11); returns the width */
long halo_screen_commit(void);
/* while TRUE, drawing shifts right to center 640-column layouts */
void halo_screen_ui_offset(unsigned char centered);
/* a player's view of the world is drawn and its HUD is next (render_window,
before interface_draw_screen): where a post-process pass over the view goes
(port/linux/src/d3d8_device.c) */
union rectangle2d;
void halo_render_before_hud(short local_player_index, short rasterizer_target,
	union rectangle2d const *viewport_bounds);
/* display.native_hud (port/linux/src/d3d8_device.c): render_window marks the
part of a view that is its scene, from before the window begins to after the
weapon's screen effects. What the frame draws into the back buffer outside
these spans (the HUD, the flash, menus, bars, loading screens, movies) is
drawn at the display's size when the scene's is smaller */
void halo_render_scene_begin(short rasterizer_target, long x0, long y0, long x1, long y1);
void halo_render_scene_end(void);
/* a map has loaded and its first frame is next (main_new_map): the shaders
and pipelines its list names are compiled now, under the loading screen
(port/linux/src/d3d8_device.c) */
void halo_shader_list_warm(char const *map_name);
/* the loading screen, held over a campaign level's load and the compiles
after it (port/linux/game/loading_screen.c): begun as the map changes,
finished at the end of main_new_map; its progress for render_frame_pregame,
and whether a loaded level waits behind it */
void halo_loading_screen_begin(char const *map_name);
void halo_loading_screen_finish(void);
unsigned char halo_loading_screen_progress(float *progress);
unsigned char halo_loading_screen_holding(void);
unsigned char halo_loading_screen_tail(void);
void halo_loading_screen_tail_frame(void);
unsigned char halo_loading_screen_smooth(void);
void halo_main_time_restart(void);
/* main_loop's passes that present nothing: debug.fixed_timestep's clock still
moves a frame (port/linux/src/xbox_kernel.c) */
void platform_clock_unpresented_frame(void);
/* an offscreen benchmark lead-in's frames that draw nothing (port/linux/src/benchmark_suite.c) */
int benchmark_suite_skips_drawing(void);
/* the mouse in the menus (source/interface/ui_widget.c) */
#include "halo_ui_pointer.h"
/* the frame's stereo state: source/render/render.c renders each eye and the
HUD into their own layers (port/linux/game/stereo.c) */
#include "../src/halo_stereo.h"

/* game code's time() reads debug.fixed_timestep's virtual clock when that is
on (port/linux/src/xbox_kernel.c); the platform layer keeps the C library's */
#include <time.h>
time_t halo_platform_time(time_t *timer);
#define time(timer) halo_platform_time(timer)

#endif
