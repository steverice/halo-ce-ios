/*
FIRST_PERSON_VIEW.C

Which of the two ways of drawing the player's own body applies to the frame
being drawn:

- a flat view (a mono frame: an iPhone, an iPad, the Mac's window, a
  simulator's window, the Vision Pro's window): display.first_person_legs,
  the legs from the waist down (port/linux/game/first_person_legs.c);
- a stereo frame: display.first_person_body in head-tracked stereo, the
  whole body below the view (port/linux/game/first_person_body.c), and
  never the legs, whose set-back follows one picture's bottom edge.

The frame decides, not the device: the Vision Pro's switch between its
window and the immersive space changes it between two frames, and each
frame draws the one that fits it. The two never draw together. Both
settings may be set; the one that doesn't fit the frame is ignored, and the
log says so once each time the view changes.
*/

#include "../src/halo_stereo.h"

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
/* port/linux/src/sdl_platform.c (the host's on iOS) */
void platform_log(const char *format, ...);

int halo_first_person_legs_apply(void)
{
	static int last_flat = -1;
	int flat = halo_stereo_frame()->eye_count == 0;

	if (flat != last_flat) {
		last_flat = flat;
		if (flat && config_boolean("display.first_person_body"))
			platform_log("first_person_view: a flat view: display.first_person_body is ignored (it draws in "
				"head-tracked stereo); display.first_person_legs decides");
		if (!flat && config_boolean("display.first_person_legs"))
			platform_log("first_person_view: a stereo view: display.first_person_legs is ignored (it draws on a "
				"flat view); display.first_person_body decides");
	}
	return flat;
}
