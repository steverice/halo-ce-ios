/* Head-tracked stereo and stereo on the theater screen (display.stereo =
"head" or "screen", visionOS 26 and later): host_stereo.m */
#pragma once

#include "halo_stereo.h"
#include "host_stereo_hud.h"

/* the guest's import (guest_host.h): at the game's frame begin, in HEAD or
SCREEN mode, opens the Compositor's frame and fills the eyes (and in HEAD
mode the head's turn) from it; eye_count stays 0 without one, and on every
other platform */
void host_stereo_frame(struct halo_stereo_frame *frame);
/* the eyes' picture size in pixels while the head drives the view (the view's
size in the Compositor's texture); 0 otherwise */
int host_stereo_picture_size(int *width, int *height);
/* the immersive space opened (again) with this layer renderer (a
cp_layer_renderer_t, unretained): sets its frame repeat count
(host_stereo_frame_repeat), and the once-only stereo logs repeat */
void host_stereo_space_opened(void *layer_renderer);
/* display.frame_repeat in stereo (HEAD or SCREEN mode), 0 to 3: each
Compositor frame stays up for that many refreshes more; 0 otherwise */
int host_stereo_frame_repeat(void);
/* foveated eye passes (display.foveation, debug.foveation_eye_passes, HEAD
mode's full view with the dedicated layout): 1 while this frame's eyes
render through the Compositor's rate maps, with the eyes' screen (logical)
size, eye_width by eye_height, and the size their targets are allocated at,
the drawable's color texture per view (the largest physical size the maps
can reach as the render quality eases); 0 otherwise */
int host_stereo_foveated_size(int *screen_width, int *screen_height, int *allocated_width, int *allocated_height);

#ifdef __OBJC__
#import <Metal/Metal.h>
/* whether a stereo frame can present: host_stereo_frame opened it and the
space is still open */
int host_stereo_ready(void);
/* this frame's rate map for an eye (0 left, 1 right) while its passes
render through it (host_stereo_foveated_size), else nil; its screen size is
the view's logical size, and it fills the top left of the eye's targets.
The parameter buffer is its copyParameterDataToBuffer data, for a shader's
rasterization_rate_map_decoder, made once a frame */
id<MTLRasterizationRateMap> host_stereo_rate_map(int eye);
id<MTLBuffer> host_stereo_rate_map_parameters(int eye);
/* draws each eye's picture and depth full view into its Compositor view, the
HUD's quads over both (host_stereo_hud.h), and presents the frame; the
game's command buffer, which drew them, is committed already. Brightness (0
to 1) dims both toward black, for the fade of a cut to or from the screen;
vignette (0 to 1) darkens the eyes' edges, not the HUD, for
input.comfort_vignette. hud_layers holds HOST_STEREO_HUD_LAYER_COUNT
textures (nil: nothing drew it this frame): the HUD layer, the crosshairs'
layer and each HUD group's target, with each group's rectangle in
hud_group_extent (gpu_stereo_present); without the HUD layer there's no HUD.
The HUD is laid out at hud_aspect's shape (its width over its height); with
hud_ui the HUD layer holds a menu, the console or a progress bar and
everything goes whole on the UI's quad; reticle is where the crosshair
points in the eyes' frame, and hud_tangents the HUD pass's projection, for
the catch-all quad. zoom (nil: none) is the zoomed picture, which shows in
place of the eyes' pictures over the whole view, on an opaque quad on the
HUD's plane zoom_tangents wide and tall, under the HUD's quads
(host_stereo_hud_zoom). While expanding (a cutscene's
film handing over to the full view, halo_stereo_window.h) the eyes show only
inside the window growing from the theater screen's rectangle at expansion
(0 to 1), black on the bars it carries (expansion_bars), with the theater's
surroundings and the script fade's tint (fade) outside it, and of the HUD
only a menu's UI quad. With cutscene (Task 12k's immersive cutscene,
halo_stereo_cutscene.h) the eyes are a cutscene camera turned by the head:
inside the director's frame (cutscene_forward and cutscene_up in the
device's frame at the render, cutscene_tangents its half tangents) they stay
sharp, outside they show a quarter-size blur of themselves, darkened and
desaturated by cutscene_dim, over a soft edge; the HUD layer goes whole on
the frame (a menu keeps the usual layout). look_dim and look_blur (0 to 1,
a seat's look fade, display.seat_look_fade) veil the eyes' pictures
(halo_stereo_veil.h: toward the same blur, then desaturated and darkened)
and darken the zoomed picture and the HUD's pieces, the crosshair's
included, by the same dim; a menu's UI quad stays as it is */
void host_stereo_present(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> left_depth, id<MTLTexture> right_depth, __unsafe_unretained id<MTLTexture> const *hud_layers,
	const float (*hud_group_extent)[4], float hud_aspect, int hud_ui, const float reticle[3],
	const float hud_tangents[2], id<MTLTexture> zoom, const float zoom_tangents[2], float near_meters,
	float far_meters, float brightness, float vignette, float ui_dim, int expanding, float expansion,
	float expansion_bars, const float fade[4], int cutscene, const float cutscene_forward[3],
	const float cutscene_up[3], const float cutscene_tangents[2], float cutscene_dim, float look_dim,
	float look_blur);
/* HEAD mode without eyes this frame (a load: a mono picture), or with a
menu over the film (the main menu's scripted scene, the pause menu in a
cutscene): 1 while the Compositor's frame host_stereo_frame opened can take
host_stereo_present_ui */
int host_stereo_ui_ready(void);
/* that picture, with the HUD layer (nil: none) over it, on the UI's quad,
level and turning with the head's yaw, inside foveation's sharp region
(host_stereo_hud.h), over the dark surroundings or the room. Brightness
(0 to 1) dims the quad toward black, for the fade of a cut to or from it */
void host_stereo_present_ui(id<MTLCommandQueue> queue, id<MTLTexture> picture, id<MTLTexture> hud, float brightness);
#endif
