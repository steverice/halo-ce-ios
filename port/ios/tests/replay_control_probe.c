/* The replay implementation plus the production player-control/frame and direct
   camera consumers. The Python driver inserts those function bodies verbatim;
   only their surrounding game dependencies are stubbed here. */
#define main replay_lifecycle_main
#include "input_replay_probe.c"
#undef main
#include <math.h>

typedef float real;
typedef unsigned char boolean;
typedef struct { float yaw, pitch; } real_euler_angles2d;
typedef struct { float i, j; } real_vector2d;
typedef struct { float i, j, k; } real_vector3d;
#define NONE (-1)
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 2
#define MAXIMUM_LOCAL_PLAYERS 2
#define TICKS_PER_SECOND 30.0f
#define csmemset memset
#define PIN(x, low, high) fminf(high, fmaxf(low, x))
#define TEST_FLAG(flags, bit) ((flags) & (1u << (bit)))
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(index) ((index) & 0xffff)
#define match_assert_valid_real(file, line, value) assert(isfinite(value))
#define match_assert(file, line, value) assert(value)
enum { _game_connection_local, _director_perspective_first_person };
enum { _player_control_debug_rotate_units_bit, _player_control_debug_rotate_all_units_bit,
	_player_control_debug_ninja_rope_bit, _player_control_rotate_weapons_bit,
	_player_control_rotate_grenades_bit, _player_control_input_zoom_bit };
struct player_control {
	real_euler_angles2d desired_angles;
	real_vector2d throttle;
	float primary_trigger, magnetism_level;
	long unit_index, control_flags;
	short zoom_level, desired_weapon_index, desired_grenade_index;
	char autolevel_ticks;
	boolean use_autolevel;
};
struct input_blob {
	real_euler_angles2d facing_delta;
	real_vector2d throttle;
	float primary_trigger;
	unsigned long player_control_flags, unit_control_flags;
};
struct player_action {
	real_euler_angles2d desired_facing;
	real_vector2d throttle;
	float primary_trigger;
	unsigned long control_flags;
	short desired_weapon_index, desired_grenade_index, desired_zoom_level, pad;
};
struct unit_datum {
	long definition_index;
	struct { short current_weapon_index, desired_weapon_index, desired_grenade_index; } unit;
	struct { long parent_object_index; } object;
};
struct game_globals_player_control { int minimum_autolevel_enabled_ticks; } probe_constants;
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&probe_constants)
static struct player_control controls[2];
static struct input_blob raw_input;
static struct unit_datum unit;
static struct player_action queued;
static int ready_weapon = NONE, local_mapping[2] = {1, 0};
static float player_control_angle_step_ticks, player_control_autolevel_time[2];
static struct player_control *player_control_get(short local) { return &controls[local]; }
static void get_local_player_input_blob(short local, float dt, struct input_blob *input)
{ (void)local; (void)dt; *input = raw_input; }
static long local_player_get_player_index(short local) { return local_mapping[local]; }
static int director_inhibited_input(short local) { (void)local; return 0; }
static int director_inhibited_facing(short local) { (void)local; return 0; }
static int game_connection(void) { return _game_connection_local; }
static long units_debug_get_next_unit(long index) { return index; }
static long units_debug_get_closest_unit(long index) { return index; }
static void players_set_local_player_unit(short local, long index) { controls[local].unit_index = index; }
static void unit_debug_ninja_rope(long index) { (void)index; }
static struct unit_datum *unit_get(long index) { (void)index; return &unit; }
static void unit_definition_get(long index) { (void)index; }
static long unit_inventory_get_weapon(long index, short slot) { (void)index; return slot; }
static short unit_inventory_next_weapon(long index, short slot, int rotate)
{ (void)index; return rotate ? (slot + 1) % 2 : 0; }
static short unit_inventory_get_must_be_readied_weapon(long index) { (void)index; return ready_weapon; }
static int unit_get_grenade_count(long index, short slot) { (void)index; (void)slot; return 1; }
static short unit_inventory_next_grenade(long index, short slot, int rotate)
{ (void)index; (void)rotate; return slot; }
static int player_control_camera_control_is_active(void) { return 1; }
static int cinematic_in_progress(void) { return 0; }
static short weapon_rotate_zoom_level(long index, short zoom) { (void)index; return zoom + 1; }
static void player_control_modify_desired_angles(short local, float yaw, float pitch)
{ controls[local].desired_angles.yaw += yaw; controls[local].desired_angles.pitch += pitch; }
int halo_controller_action_aim(short local, float look_yaw, float look_pitch, unsigned long flags, float *yaw, float *pitch, short *pad)
{ (void)local; (void)look_yaw; (void)look_pitch; (void)flags; (void)yaw; (void)pitch; (void)pad; return 0; }
void halo_controller_network_throttle(short local, float *i, float *j) { (void)local; (void)i; (void)j; }
static int unit_exit_seat_holds_unzoomed(long index) { (void)index; return 0; }
static int player_ui_autolevel_enabled(short local) { (void)local; return 0; }
static int halo_stereo_head_drives_look(short local) { (void)local; return 0; }
int halo_controller_network_head_camera(short local) { (void)local; return 0; }
static void update_client_queue(const struct player_action *action) { queued = *action; }
struct observer_result { real_vector3d forward, up; };
static struct observer_result direct_cameras[2];
/* Force direct/late look on; the replay predicate must prevent the override. */
static int direct_config_boolean(const char *name) { (void)name; return 1; }
static int director_get_perspective(short local) { (void)local; return _director_perspective_first_person; }
static long player_control_get_unit_index(short local) { return controls[local].unit_index; }
static struct unit_datum *object_get(long index) { (void)index; return &unit; }
static void player_control_get_facing_direction(short local, real_vector3d *forward)
{ *forward = (real_vector3d){cosf(controls[local].desired_angles.yaw), sinf(controls[local].desired_angles.yaw), 0}; }
static void observer_up_from_forward(real_vector3d *forward, real_vector3d *up)
{ (void)forward; *up = (real_vector3d){0, 0, 1}; }
static int late_calls;
static void late_look(struct observer_result *camera) { late_calls++; camera->forward.k += 0.25f; }
static float halo_stereo_head_yaw_taken(void) { return 0; }
static void camera_noted(float yaw, const char *source, int held) { (void)yaw; (void)source; (void)held; }

/* PRODUCTION_FUNCTIONS */

int main(void)
{
	char folder[] = "/tmp/visr-control-probe-XXXXXX";
	assert(mkdtemp(folder) && !chdir(folder));
	FILE *file = fopen("camera.input", "w");
	assert(file);
	fputs("halo input recording 1\n0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", file); fclose(file);
	file = fopen("camera.actions", "w"); assert(file);
	fputs("halo action recording 1\n0 10 0 3f800000 bf000000 0 0 0 0 00000002\n", file); fclose(file);
	assert(input_replay_begin("./camera.input", 0));
	clock_ready = 1; clock_tick = 10;
	unsigned int action[ACTION_WORDS] = {0};
	input_replay_tick_action(action);
	controls[0].desired_angles = (real_euler_angles2d){-2, 0.7f};
	controls[1].desired_angles = controls[0].desired_angles;
	player_control_apply_replay(update_client_local_player_index(0));
	assert(controls[1].desired_angles.yaw == 1 && controls[1].desired_angles.pitch == -0.5f);
	assert(controls[1].zoom_level == 2 && controls[0].desired_angles.yaw == -2);
	unit.object.parent_object_index = NONE;
	unit.unit.current_weapon_index = 0;
	raw_input.facing_delta = (real_euler_angles2d){0.2f, 0.1f};
	raw_input.throttle = (real_vector2d){0.75f, -0.5f};
	raw_input.primary_trigger = 0.8f; raw_input.unit_control_flags = 17;
	for (int frame = 0; frame < 12; frame++) {
		raw_input.player_control_flags = 1u << (frame % 2 ? _player_control_rotate_weapons_bit : _player_control_input_zoom_bit);
		ready_weapon = frame % 3 == 0 ? 1 : NONE;
		handle_one_player_input(1, frame % 2 ? 1.0f / 30 : 1.0f / 120);
		assert(controls[1].desired_angles.yaw == 1 && controls[1].desired_angles.pitch == -0.5f);
		assert(controls[1].zoom_level == 2);
		assert(queued.throttle.i == 0.75f && queued.control_flags == 17 && queued.primary_trigger == 0.8f);
	}
	real_euler_angles2d impulse = {0.125f, -0.0625f};
	player_control_permanent_impulse(1, &impulse);
	handle_one_player_input(1, 1.0f / 120);
	assert(controls[1].desired_angles.yaw == 1.125f && controls[1].desired_angles.pitch == -0.5625f);
	struct observer_result observer = {{0, 1, 0}, {0, 0, 1}};
	assert(render_interpolation_direct_camera(1, &observer) == &observer);
	assert(late_calls == 0);
	/* The other local player retains ordinary direct camera and late input. */
	assert(render_interpolation_direct_camera(0, &observer) != &observer && late_calls == 1);
	clock_ready = 0; input_replay_begin(NULL, 0); clock_ready = 1;
	raw_input.player_control_flags = 0; ready_weapon = NONE;
	handle_one_player_input(1, 1.0f / 30);
	assert(fabsf(controls[1].desired_angles.yaw - 1.325f) < 0.00001f);
	assert(render_interpolation_direct_camera(1, &observer) != &observer);
	clock_ready = 0; input_replay_begin(NULL, 0);
	unlink("camera.input"); unlink("camera.actions");
	assert(!chdir("/") && !rmdir(folder));
	puts("replay control consumers: PASS");
	return 0;
}
