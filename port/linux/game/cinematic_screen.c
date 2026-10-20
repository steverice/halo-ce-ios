/*
CINEMATIC_SCREEN.C

What stereo's cutscene screen reads from the game (port/linux/src/halo_stereo.h):
whether the letterbox is in, which makes a cutscene the 3D film on a 16:9
screen (stereo.c); whether the camera is scripted or third person, which
make the film too (in SCREEN mode only the director's scripted camera); and
the script fade, which tints the immersive space around the screen
(port/ios/host/gpu_metal.m).

The letterbox, not cinematic_in_progress, marks the cutscene: the bars are
what frame Bungie's 16:9 composition, and scripts toggle them on their own
(cinematic_show_letterbox). Only script fades (fade_in, fade_out) tint the
room: the screen flashes of damage, pickups and telefrags stay on the game's
picture.
*/

#include "cseries.h"
#include <string.h>
#include "camera/camera_scripting.h"
#include "camera/director.h"
#include "coop_spectate.h"
#include "camera/observer.h"
#include "cutscene/cinematics.h"
#include "game/players.h"
#include "items/weapons.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "objects/objects.h"
#include "tag_files/tag_files.h"
#include "effects/player_effects.h"
#include "game/game.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "main/console.h"
#include "math/periodic_functions.h"
#include "math/real_math.h"
#include "../src/halo_stereo.h"

int halo_cinematic_screen(void)
{
	return cinematic_globals && cinematic_globals->show_letterbox;
}

/* A camera the script drives: the script has taken the camera from the
player (player_camera_control false: a first-person moment the player can't
look around in, as a10's Chief climbing out of the cryo pod), or it runs a
scripted camera (camera_control, director_script_camera). The game's own
flags, rather than whether the player's input is off: input is also off
where the player still looks through their own eyes (a10's "use the right
stick to look around" in the pod, with camera control on). */
int halo_scripted_camera(void)
{
	return player_control_camera_control_disabled() || halo_scripted_director_camera();
}

/* The director's own scripted camera, without the player's camera with its
look taken away: SCREEN mode's film (stereo.c), where the player's camera
under a script stays gameplay. a10's pod toggles player_camera_control with
no cut, and the director's cameras begin and end on cuts. */
int halo_scripted_director_camera(void)
{
	return (director_camera_scripted && *director_camera_scripted) ||
		director_peek_perspective(0) == _director_perspective_scripted;
}

/* the director's third-person camera (following_camera_update): a vehicle
seat whose camera isn't first person */
int halo_third_person_camera(void)
{
	return director_peek_perspective(0) == _director_perspective_third_person;
}

/* A network co-op player with no unit watches through the game's dead
camera, whether it targets the local corpse or a teammate. Its head turns
the picture locally; a scripted host camera does not pass this test. */
int halo_coop_dead_camera(void)
{
	long player_index = local_player_get_player_index(0);

	return coop_spectating() && player_index != NONE && player_get(player_index)->unit_index == NONE &&
		director_dead_camera_active(0);
}

/* A first-person cutscene camera (the stereo spec's session 4 "Cutscenes"):
the director's own first person, with the look on or off, or a scripted
camera in first-person mode (scripted_camera_set_first_person). In HEAD mode
the film is only for the other cameras (stereo.c) */
int halo_cutscene_camera_first_person(void)
{
	director_perspective perspective = director_peek_perspective(0);

	return perspective == _director_perspective_first_person ||
		(perspective == _director_perspective_scripted && scripted_camera_first_person());
}

/* A first-person camera the player can't look around in: the player's own
with its look taken away (player_camera_control false: a10's cryo pod and its
climb-out), or a scripted camera in first-person mode. HEAD mode turns the
picture by the head and never the look there (stereo.c) */
int halo_look_disabled_first_person(void)
{
	director_perspective perspective = director_peek_perspective(0);

	return (perspective == _director_perspective_first_person && player_control_camera_control_disabled()) ||
		(perspective == _director_perspective_scripted && scripted_camera_first_person());
}

/* The player's own first-person camera with the look enabled: the
director's first person with player_camera_control on. Narrower than
halo_cutscene_camera_first_person (no scripted camera), and the complement of
halo_look_disabled_first_person for the director's first person: a camera
whose look the player has, which the head can drive. HEAD mode releases a
cutscene's film for it (stereo.c) */
int halo_player_camera_first_person(void)
{
	return director_peek_perspective(0) == _director_perspective_first_person &&
		!player_control_camera_control_disabled();
}

int halo_director_inhibited_facing(void)
{
	return director_inhibited_facing(0) ? 1 : 0;
}

/* the observer's camera's distance from the player's unit's camera position
(world units), negative without a unit */
static float camera_distance_from_eyes(void)
{
	long unit_index = player_control_get_unit_index(0);
	struct observer_result const *camera = observer_get_camera(0);
	real_point3d eyes;
	real dx, dy, dz;

	if (unit_index == NONE || !camera)
		return -1.0f;
	unit_get_camera_position(unit_index, &eyes);
	dx = camera->position.x - eyes.x;
	dy = camera->position.y - eyes.y;
	dz = camera->position.z - eyes.z;
	return (float)sqrt(dx * dx + dy * dy + dz * dz);
}

/* The camera has come to the player's eyes after a cutscene: the observer's
command finished (director_script_camera(FALSE) leaves it gliding from the
cutscene camera's last pose for up to 2 s, observer_update_command), or the
camera within HALO_CUTSCENE_SETTLED_DISTANCE of the unit's camera position
with its orientation at its command (observer_orientation_settled, which
render_interpolation.c's facing-posed cameras need too) */
int halo_cutscene_camera_settled(void)
{
	float distance;

	if (observer_command_has_finished(0))
		return 1;
	distance = camera_distance_from_eyes();
	return distance >= 0.0f && distance <= HALO_CUTSCENE_SETTLED_DISTANCE && observer_orientation_settled(0);
}

/* the observer's field of view, horizontal (the game's 4:3 frame shrinks
it by 0.85 on the tangent, render_cameras.c): a cutscene camera point's own,
or 70 degrees (camera_scripting.c) */
float halo_cutscene_camera_field_of_view(void)
{
	struct observer_result const *camera = observer_get_camera(0);

	return camera ? (float)camera->field_of_view : 0.0f;
}

void halo_cutscene_state(struct halo_cutscene_state *state)
{
	state->letterbox = halo_cinematic_screen();
	state->director_scripted = director_camera_scripted && *director_camera_scripted;
	state->perspective = director_peek_perspective(0);
	state->script_mode = scripted_camera_mode();
	state->look_disabled = player_control_camera_control_disabled();
	state->player_first_person = halo_player_camera_first_person();
	state->observer_finished = observer_command_has_finished(0);
	state->orientation_settled = observer_orientation_settled(0);
	state->distance = camera_distance_from_eyes();
}

/* The intensity player_effect_get_screen_flash gives the script fade (a
cosine over the fade's ticks, inverted while fading in), computed here
without that function, which ends a finished fade in the game state. The
fade is active exactly when the game's is (whole ticks); its intensity is
taken at the picture's time, the tick before this one plus the fraction, as
the render's interpolation shows objects (render_interpolation_game_time_
sec), so the room changes smoothly at the display's rate and in step with
the picture. */
int halo_screen_fade(float tick_fraction, float rgb_intensity[4])
{
	real_rgb_color color;
	long start_time;
	short ticks;
	boolean fading_out;
	long elapsed_ticks;
	real intensity;

	rgb_intensity[0] = rgb_intensity[1] = rgb_intensity[2] = rgb_intensity[3] = 0.0f;
	/* the game draws no fade while the console is up */
	if (console_is_active() || !player_effect_get_screen_fade(&color, &start_time, &ticks, &fading_out))
		return 0;
	elapsed_ticks = game_time_get() - start_time;
	if (!fading_out && elapsed_ticks > ticks)
		return 0;
	intensity = ticks > 0
		? transition_function_evaluate(
			_transition_function_cosine,
			PIN(((real)elapsed_ticks - 1.0f + PIN(tick_fraction, 0.0f, 1.0f)) / ticks, 0.0f, 1.0f))
		: 1.0f;
	if (!fading_out)
		intensity = 1.0f - intensity;
	rgb_intensity[0] = color.red;
	rgb_intensity[1] = color.green;
	rgb_intensity[2] = color.blue;
	rgb_intensity[3] = PIN(intensity, 0.0f, 1.0f);
	return 1;
}

/* 1 from a map's load until HALO_LEVEL_START_TICKS of its ticks have run: the
level's opening script needs them to start its cutscene (a30 and a10 put up
the cutscene's camera on the third tick, their fade a tick before it), and
until then the camera shows the player's start, which the level never meant
to show (a30's beach before the dropship ride). stereo.c holds the eyes
black over it (halo_stereo_level_start_hold) */
#define HALO_LEVEL_START_TICKS 3
int halo_level_first_tick_pending(void)
{
	return game_time_get() < HALO_LEVEL_START_TICKS;
}

int halo_level_start_campaign(void)
{
	return game_in_progress() && global_scenario_index != NONE && global_scenario_get()->type == _scenario_type_solo;
}

/* 1 while local player 0 sits in a10's cryo pod: the unit the player rides
(its ultimate parent, not itself) is the level's cryotube, by its tag's name.
For the pod's own look fade (stereo.c) */
int halo_seat_cryo_pod(void)
{
	long player_index = local_player_get_player_index(0);
	long unit_index, parent_index;
	const char *name;

	if (player_index == NONE)
		return 0;
	unit_index = player_get(player_index)->unit_index;
	if (unit_index == NONE)
		return 0;
	parent_index = object_get_ultimate_parent(unit_index);
	if (parent_index == NONE || parent_index == unit_index)
		return 0;
	name = tag_get_name(object_get(parent_index)->definition_index);
	return name && strstr(name, "cryotube") != NULL;
}

/* A seated player's first-person passenger view may move around the seat
independently of the weapon's yaw arc. Driver views and chase cameras keep
their existing comfort rule. */
int halo_seat_first_person_passenger(void)
{
	long player_index = local_player_get_player_index(0);
	long unit_index, parent_index;
	struct unit_datum *unit, *vehicle;
	struct unit_definition *definition;
	struct unit_seat *seat;

	if (player_index == NONE)
		return 0;
	unit_index = player_get(player_index)->unit_index;
	if (unit_index == NONE)
		return 0;
	unit = unit_get(unit_index);
	parent_index = unit->object.parent_object_index;
	if (parent_index == NONE || !object_try_and_get_and_verify_type(parent_index, _object_mask_vehicle))
		return 0;
	vehicle = unit_get(parent_index);
	definition = unit_definition_get(vehicle->definition_index);
	if (vehicle->unit.driver_object_index == unit_index || unit->unit.parent_seat_index < 0 ||
		unit->unit.parent_seat_index >= definition->unit.seats.count)
		return 0;
	seat = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, unit->unit.parent_seat_index, struct unit_seat);
	return TEST_FLAG(seat->flags, _unit_seat_first_person_camera_bit) &&
		!TEST_FLAG(seat->flags, _unit_seat_driver_bit) &&
		!TEST_FLAG(seat->flags, _unit_seat_gunner_bit) &&
		!TEST_FLAG(seat->flags, _unit_seat_invisible_bit);
}

/* The zoom reticle can use the unit's ordinary aim only when the player
fires their own held weapon. A gunner or vehicle weapon may fire from its
marker instead; leave those reticles on their existing camera rule. */
int halo_zoom_ordinary_aim_ray(short local_player_index, float ray[3])
{
	long unit_index = player_control_get_unit_index(local_player_index);
	long weapon_index;
	struct unit_datum *unit;

	if (unit_index == NONE || unit_get_aiming_unit_index(unit_index) != unit_index)
		return 0;
	unit = unit_get(unit_index);
	if ((unit->object.parent_object_index != NONE && !halo_seat_first_person_passenger()) ||
		unit->unit.gunner_object_index != NONE)
		return 0;
	weapon_index = unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index);
	if (weapon_index == NONE || object_get(weapon_index)->object.parent_object_index != unit_index)
		return 0;
	ray[0] = unit->unit.aiming_vector.i;
	ray[1] = unit->unit.aiming_vector.j;
	ray[2] = unit->unit.aiming_vector.k;
	return 1;
}

/* The zoom's magnification for the local player's zoomed pass (render.c), as the
HUD reads it for the aim assist's range (hud.c): the current weapon of the
unit that aims (a gunner's seat's too) at the player's zoom level; 1 when
unzoomed or unarmed. The game's own zoomed camera divides the field of view
by the same figure (weapon_get_field_of_view). */
float halo_zoom_magnification(short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);
	short zoom_level = player_control_get_zoom_level(local_player_index);
	long unit_index;
	long weapon_index;

	if (player_index == NONE || zoom_level == NONE)
		return 1.0f;
	unit_index = unit_get_aiming_unit_index(player_get(player_index)->unit_index);
	if (unit_index == NONE)
		return 1.0f;
	weapon_index = unit_inventory_get_weapon(unit_index, unit_get(unit_index)->unit.current_weapon_index);
	if (weapon_index == NONE)
		return 1.0f;
	return weapon_get_zoom_magnification(weapon_index, zoom_level);
}

