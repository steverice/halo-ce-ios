"""Exercise production controller gates, action encoding and camera ownership."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest

from test_mp_synthetic_input import function

ROOT = Path(__file__).resolve().parents[1]


def run_probe(tmp_path, name, source):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the controller aim probe")
    path = tmp_path / (name + ".c")
    path.write_text(source)
    output = tmp_path / name
    compiled = subprocess.run([compiler, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                               str(path), "-lm", "-o", str(output)], capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([str(output)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


GLUE = r'''
#include <assert.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#define NONE -1
#define TEST_FLAG(flags,bit) ((flags)&(1u<<(bit)))
#define HALO_STEREO_HEAD 1
#define HALO_STEREO_SIDE_BY_SIDE 2
#define _object_dead_bit 0
#define TRUE 1
enum {_game_connection_local, _game_connection_network_client, _game_connection_network_server};
enum {_director_perspective_first_person, _director_perspective_third_person};
typedef int director_perspective;
typedef struct {float x,y,z;} real_point3d;
typedef struct {float i,j,k;} real_vector3d;
typedef struct {float yaw,pitch;} real_euler_angles2d;
struct halo_stereo_frame {int mode, eye_count;};
struct unit_datum {struct {int damage_flags;long parent_object_index;} object;
 struct {real_vector3d aiming_vector;} unit;};
static struct unit_datum unit;
static struct halo_stereo_frame frame={HALO_STEREO_HEAD,2};
static int connection, test_controller, look_disabled, head_drives=1, perspective, cinematic;
static int aim_controller=1, sampled_hand=1, feedback_calls, reset_calls;
static long unit_index_now=17, aiming_unit=17;
static int own_passenger_weapon;
static struct {int last_unit,hand_aiming;} aim;
static struct halo_stereo_frame *halo_stereo_frame(void){return &frame;}
static struct unit_datum *unit_get(long index){assert(index==17);return &unit;}
static int game_connection(void){return connection;}
static int halo_stereo_head_drives_look(short local){return local==0&&head_drives;}
static int halo_stereo_film(void){return cinematic;}
static int halo_look_disabled_first_person(void){return look_disabled;}
static int director_desired_perspective(long index,director_perspective *out){(void)index;*out=perspective;return perspective;}
static int director_peek_perspective(short local){(void)local;return perspective;}
static long unit_get_aiming_unit_index(long index){(void)index;return aiming_unit;}
static int halo_zoom_ordinary_aim_ray(short local,float ray[3]){
 if(local!=0||!own_passenger_weapon||unit.object.parent_object_index==NONE||aiming_unit!=unit_index_now)return 0;
 ray[0]=unit.unit.aiming_vector.i;ray[1]=unit.unit.aiming_vector.j;ray[2]=unit.unit.aiming_vector.k;return 1;
}
static void settings(void){}
static long player_control_get_unit_index(short local){return local==0?unit_index_now:NONE;}
static unsigned long get_random_seed(void){return 0;}
static unsigned long *get_global_local_random_seed_address(void){static unsigned long seed;return &seed;}
static void platform_log(const char *format,...){(void)format;}
static void feedback(void){feedback_calls++;}
static void reset(const char *reason){(void)reason;reset_calls++;}
static void euler_angles2d_from_vector3d(real_euler_angles2d *a,const real_vector3d *v){a->yaw=atan2f(v->j,v->i);a->pitch=asinf(v->k);}
static void player_control_network_seat_facing(short local,float *yaw,float *pitch){assert(local==0);*yaw=.3f;*pitch=.1f;}
static int action_aim(long index,float ly,float lp,unsigned long flags,float *y,float *p,short *pad){
 (void)index;(void)ly;(void)lp;(void)flags;*y=1.57079632679f;*p=.25f;*pad=sampled_hand?123:0;return sampled_hand;
}
static void unit_get_camera_position(long index,real_point3d *eye){assert(index==17);*eye=(real_point3d){3,4,5};}
'''


def test_production_gate_opens_network_on_foot_only(tmp_path):
    glue = (ROOT / "port/linux/game/controller_aim_game.c").read_text()
    run_probe(tmp_path, "gate", GLUE + function(glue, "static int gate_target(") + r'''
int main(void){
 const char *reason;unit.object.parent_object_index=NONE;
 for(connection=0;connection<=2;connection++){
  assert(gate_target(17,&reason));
  unit.object.parent_object_index=23;
  assert(gate_target(17,&reason)==(connection==_game_connection_local));
  unit.object.parent_object_index=NONE;
  unit.object.damage_flags=1;assert(!gate_target(17,&reason));unit.object.damage_flags=0;
  look_disabled=1;assert(!gate_target(17,&reason));look_disabled=0;
  perspective=1;assert(!gate_target(17,&reason));perspective=0;
  aiming_unit=23;assert(!gate_target(17,&reason));aiming_unit=17;
  head_drives=0;assert(!gate_target(17,&reason));head_drives=1;
 }
}
''')


def test_production_action_preserves_local_pad_and_zeros_network_pad(tmp_path):
    glue = (ROOT / "port/linux/game/controller_aim_game.c").read_text()
    run_probe(tmp_path, "action", GLUE + function(glue, "static int network_controller_aim(") +
              function(glue, "static int gate_target(") +
              function(glue, "int halo_controller_action_aim(") + r'''
int main(void){
 float yaw,pitch;short pad;unit.object.parent_object_index=NONE;
 for(connection=0;connection<=2;connection++){
  assert(halo_controller_action_aim(0,0,0,7,&yaw,&pitch,&pad));
  assert(fabsf(yaw-1.57079632679f)<.00001f&&pitch==.25f);
  assert(pad==(connection==_game_connection_local?123:0));
  assert(aim.hand_aiming);
  assert(!halo_controller_action_aim(1,0,0,7,&yaw,&pitch,&pad)&&pad==0);
  if(connection){
   unit.object.parent_object_index=23;
   assert(halo_controller_action_aim(0,.1f,.2f,7,&yaw,&pitch,&pad));
   assert(pad==0&&yaw==.3f&&pitch==.1f&&!aim.hand_aiming);
   unit.object.parent_object_index=NONE;
  }
  sampled_hand=0;unit.unit.aiming_vector=(real_vector3d){1,0,0};
  assert(halo_controller_action_aim(0,.1f,.2f,7,&yaw,&pitch,&pad)==(connection!=0));
  assert(pad==0&&yaw==(connection?0:.1f)&&pitch==(connection?0:.2f));sampled_hand=1;
  unit_index_now=NONE;assert(halo_controller_action_aim(0,.4f,.5f,7,&yaw,&pitch,&pad)==(connection!=0));
  if(connection)assert(yaw==0&&pitch==0&&pad==0);
  assert(!aim.hand_aiming);unit_index_now=17;
 }
}
''')


def test_production_network_targeting_uses_simulated_aim(tmp_path):
    glue = (ROOT / "port/linux/game/controller_aim_game.c").read_text()
    # The local hand ray is intentionally different from the rate-limited unit.
    glue_test = GLUE.replace("int last_unit,hand_aiming;", "int last_unit,hand_aiming;float ray_direction[3];")
    eligible = function(glue, "static int network_gameplay_aim_eligible(") if \
        "static int network_gameplay_aim_eligible(" in glue else ""
    run_probe(tmp_path, "ray", glue_test + function(glue, "static int gate_target(") +
              function(glue, "static int network_controller_aim(") + eligible +
              function(glue, "int halo_controller_aim_ray(") + r'''
int main(void){
 float origin[3],direction[3];aim.hand_aiming=1;aim.ray_direction[1]=1;
 unit.unit.aiming_vector=(real_vector3d){1,0,0};unit.object.parent_object_index=NONE;
 for(connection=0;connection<=2;connection++){
  assert(halo_controller_aim_ray(0,origin,direction));assert(origin[0]==3&&origin[2]==5);
  assert(direction[connection?0:1]==1&&direction[connection?1:0]==0);
 }
 assert(!halo_controller_aim_ray(1,origin,direction));
 aim.hand_aiming=0;connection=0;assert(!halo_controller_aim_ray(0,origin,direction));
 connection=1;assert(halo_controller_aim_ray(0,origin,direction)&&direction[0]==1);
 unit.object.parent_object_index=23;aiming_unit=17;own_passenger_weapon=1;
 assert(halo_controller_aim_ray(0,origin,direction)&&direction[0]==1);
 own_passenger_weapon=0;aiming_unit=23;
 assert(!halo_controller_aim_ray(0,origin,direction));
 unit.object.parent_object_index=NONE;assert(!halo_controller_aim_ray(0,origin,direction));
 aiming_unit=17;perspective=1;assert(!halo_controller_aim_ray(0,origin,direction));
}
''')


def test_production_direct_camera_keeps_network_head_without_direct_setting(tmp_path):
    camera = (ROOT / "port/linux/game/render_interpolation.c").read_text()
    run_probe(tmp_path, "camera", GLUE + r'''
struct observer_result {real_vector3d forward,up;};
static struct observer_result direct_cameras[2];
static int direct_setting, replay, inhibited;
static unsigned long changes;
static unsigned long config_changes(void){return changes;}
static int config_boolean(const char *name){(void)name;return direct_setting;}
static int input_replay_controls_facing(short local){(void)local;return replay;}
static int director_get_perspective(short local){(void)local;return perspective;}
static int director_inhibited_facing(short local){(void)local;return inhibited;}
static int cinematic_in_progress(void){return cinematic;}
static struct unit_datum *object_get(long index){return unit_get(index);}
static void player_control_get_facing_direction(short local,real_vector3d *v){(void)local;*v=(real_vector3d){1,0,0};}
static void observer_up_from_forward(real_vector3d *f,real_vector3d *u){(void)f;*u=(real_vector3d){0,0,1};}
static void late_look(struct observer_result *c){(void)c;}
static float halo_stereo_head_yaw_taken(void){return 0;}
static void camera_noted(float yaw,const char *source,int held){(void)yaw;(void)source;(void)held;}
static int halo_controller_network_head_camera(short local){return local==0&&connection&&aim_controller&&head_drives;}
''' + function(camera, "static struct observer_result const *render_interpolation_direct_camera(\n\tshort local_player_index,\n\tstruct observer_result const *observer)\n{") + r'''
int main(void){
 struct observer_result observer={{0,1,0},{0,0,1}};unit.object.parent_object_index=NONE;
 for(connection=1;connection<=2;connection++){
  for(int frame_number=0;frame_number<60;frame_number++){
   /* Old observers retain hand-facing ticks after tracking/gate ease ends. */
   aim.hand_aiming=frame_number<10;
   const struct observer_result *out=render_interpolation_direct_camera(0,&observer);
   assert(out!=&observer&&out->forward.i==1&&out->forward.j==0);
  }
  assert(render_interpolation_direct_camera(1,&observer)==&observer);
  inhibited=1;assert(render_interpolation_direct_camera(0,&observer)==&observer);inhibited=0;
  cinematic=1;assert(render_interpolation_direct_camera(0,&observer)==&observer);cinematic=0;
  perspective=1;assert(render_interpolation_direct_camera(0,&observer)==&observer);perspective=0;
  unit.object.parent_object_index=23;assert(render_interpolation_direct_camera(0,&observer)==&observer);unit.object.parent_object_index=NONE;
  unit_index_now=NONE;assert(render_interpolation_direct_camera(0,&observer)==&observer);unit_index_now=17;
  replay=1;assert(render_interpolation_direct_camera(0,&observer)==&observer);replay=0;
 }
 connection=0;assert(render_interpolation_direct_camera(0,&observer)==&observer);
 direct_setting=1;changes++;assert(render_interpolation_direct_camera(0,&observer)!=&observer);
}
''')


def test_production_head_camera_session_outlives_tracking_and_gate(tmp_path):
    glue = (ROOT / "port/linux/game/controller_aim_game.c").read_text()
    run_probe(tmp_path, "camera-session", GLUE + function(glue, "static int network_controller_aim(") +
              function(glue, "int halo_controller_network_head_camera(") + r'''
int main(void){
 for(connection=0;connection<=2;connection++){
  for(int frame_number=0;frame_number<60;frame_number++){
   aim.hand_aiming=frame_number<10;
   assert(halo_controller_network_head_camera(0)==(connection!=0));
   assert(!halo_controller_network_head_camera(1));
  }
  aim_controller=0;assert(!halo_controller_network_head_camera(0));aim_controller=1;
  head_drives=0;assert(halo_controller_network_head_camera(0)==(connection!=0));head_drives=1;
 }
}
''')


def test_production_drawing_uses_own_unit_aim_and_excludes_borrowed_seats(tmp_path):
    glue = (ROOT / "port/linux/game/controller_aim_game.c").read_text()
    eligible = function(glue, "static int network_gameplay_aim_eligible(") if \
        "static int network_gameplay_aim_eligible(" in glue else ""
    run_probe(tmp_path, "draw", GLUE + r'''
#define HALO_SPATIAL_READY 1
#define HALO_SPATIAL_NONE 0
#define CONTROLLER_PI 3.14159265f
#define CONTROLLER_RAY_UNITS 100.f
#define CONTROLLER_SKY_UNITS 200.f
#define CONTROLLER_NEAR_UNITS 1.f
#define CONTROLLER_PAST_COVER_UNITS .1f
struct halo_spatial_hand {int state;};
struct halo_spatial_sample {struct halo_spatial_hand hands[2];int aiming_hand,reason,head_level;float head_yaw;};
struct hand_memory {int known;};
struct controller_near {int known;};
struct controller_range {int known;};
struct controller_zoom_filter {int known;};
struct controller_body {int known;float yaw;};
static struct halo_spatial_sample sample;
static struct {struct controller_body body;struct hand_memory location;struct controller_near near;
 struct controller_range range;struct controller_zoom_filter zoom_filter;
 int seat_clamped;float ray_range,eased_range;} drawing;
static struct {float eye[3],barrel_origin[3],barrel_forward[3],barrel_up[3],sightline[3],aim_point[3],hit_point[3];
 float hit_distance;int past_cover;} draw;
static int pose_known=1;
static struct halo_spatial_sample *halo_controller_draw_sample(void){return &sample;}
static float halo_stereo_eye_yaw(void){return 0;}
static float controller_body_yaw(struct controller_body *b,float l,float h,int level){(void)b;(void)h;(void)level;return l;}
static float steady_strength(void){return 0;}
static float player_control_seat_yaw_clamped(short local,float yaw){(void)local;return unit.object.parent_object_index==NONE?yaw:0.f;}
static void controller_angles(const float v[3],float *yaw,float *pitch){*yaw=atan2f(v[1],v[0]);*pitch=asinf(v[2]);}
static long object_get_ultimate_parent(long index){return index;}
static int ray_hit(const float start[3],const float delta[3],long ignore,float hit[3]){
 (void)ignore;for(int i=0;i<3;i++)hit[i]=start[i]+delta[i]*.1f;return 1;
}
''' + function(glue, "struct hand_sightline\n") + ";\n" + r'''
static int hand_sightline(long index,const struct halo_spatial_hand *h,struct hand_memory *m,
 struct controller_near *n,struct controller_range *r,struct controller_zoom_filter *z,float body,
 const float head[3],const float eye[3],float dt,float steady,struct hand_sightline *out){
 (void)index;(void)h;(void)m;(void)n;(void)r;(void)z;(void)body;(void)head;(void)eye;(void)dt;(void)steady;
 if(!pose_known)return 0;memset(out,0,sizeof(*out));out->direction[1]=1;out->forward[1]=1;
 out->up[2]=1;out->aim_distance=out->surface_distance=100;return 1;
}
''' + function(glue, "static int gate_target(") + function(glue, "static int network_controller_aim(") + eligible +
              function(glue, "static int draw_sightline(") + r'''
int main(void){
 const float camera[3]={3,4,5};aim.hand_aiming=1;drawing.body.known=1;
 unit.unit.aiming_vector=(real_vector3d){.6f,0,.8f};
 for(connection=1;connection<=2;connection++)for(pose_known=0;pose_known<=1;pose_known++){
  unit.object.parent_object_index=NONE;aim.hand_aiming=pose_known;
  assert(draw_sightline(camera,.016f));
  assert(fabsf(draw.sightline[0]-.6f)<.00001f&&draw.sightline[1]==0&&fabsf(draw.sightline[2]-.8f)<.00001f);
  assert(fabsf(draw.hit_point[0]-9)<.00001f&&fabsf(draw.hit_point[2]-13)<.00001f);
  assert(!draw.past_cover&&!drawing.seat_clamped);
 }
 connection=1;unit.object.parent_object_index=23;aiming_unit=17;own_passenger_weapon=1;
 pose_known=0;aim.hand_aiming=0;
 assert(draw_sightline(camera,.016f)&&fabsf(draw.sightline[0]-.6f)<.00001f);
 own_passenger_weapon=0;aiming_unit=23;
 assert(!draw_sightline(camera,.016f));
 unit.object.parent_object_index=NONE;assert(!draw_sightline(camera,.016f));
 aiming_unit=17;perspective=1;assert(!draw_sightline(camera,.016f));perspective=0;
 look_disabled=1;assert(!draw_sightline(camera,.016f));look_disabled=0;
 unit.object.damage_flags=1;assert(!draw_sightline(camera,.016f));unit.object.damage_flags=0;
 connection=0;unit.object.parent_object_index=NONE;pose_known=1;aim.hand_aiming=1;
 assert(draw_sightline(camera,.016f)&&draw.sightline[1]==1);
 pose_known=0;assert(!draw_sightline(camera,.016f));
 unit_index_now=NONE;connection=1;assert(!draw_sightline(camera,.016f));
}
''')


def test_network_zoom_reticle_shows_own_passenger_and_excludes_vehicle_owned_aim(tmp_path):
    glue = (ROOT / "port/linux/game/controller_aim_game.c").read_text()
    eligible = function(glue, "static int network_gameplay_aim_eligible(")
    run_probe(tmp_path, "zoom-ownership", GLUE + r'''
static struct {int active;float sightline[3];} draw;
static struct {long active_frame;} drawing;
static struct {int known;long frame;float forward[3],up[3],tangents[2],magnification;} zoom_view;
static struct {long frame_index;} render;
static int zoom_takeover, project_calls;
static int controller_zoom_project(const float ray[3],const float forward[3],const float up[3],
 const float tangents[2],float magnification,int inside,float shown[3]){
 (void)ray;(void)forward;(void)up;(void)tangents;(void)magnification;(void)inside;
 project_calls++;shown[2]=-1;return 1;
}
static int controller_zoom_project_clamped(const float ray[3],const float forward[3],const float up[3],
 const float tangents[2],float magnification,float shown[3]){
 (void)ray;(void)forward;(void)up;(void)tangents;(void)magnification;(void)shown;
 project_calls++;return 1;
}
''' + function(glue, "static int gate_target(") +
              function(glue, "static int network_controller_aim(") + eligible +
              function(glue, "int halo_controller_zoom_reticle(") + r'''
int main(void){
 float shown[3];connection=1;draw.active=zoom_view.known=1;
 render.frame_index=drawing.active_frame=zoom_view.frame=7;
 unit.object.parent_object_index=23;aiming_unit=23;
 assert(!halo_controller_zoom_reticle(shown)&&project_calls==0);
 aiming_unit=17;own_passenger_weapon=1;
 assert(halo_controller_zoom_reticle(shown)==1&&project_calls==1);
 own_passenger_weapon=0;aiming_unit=23;
 unit.object.parent_object_index=NONE;assert(!halo_controller_zoom_reticle(shown)&&project_calls==1);
 aiming_unit=17;assert(halo_controller_zoom_reticle(shown)==1&&project_calls==2);
 connection=2;unit.object.parent_object_index=23;aiming_unit=23;
 assert(!halo_controller_zoom_reticle(shown)&&project_calls==2);
}
''')


def test_production_network_aim_does_not_blend_toward_head(tmp_path):
    glue = (ROOT / "port/linux/game/controller_aim_game.c").read_text()
    action = function(glue, "static int action_aim(")
    start = action.index("blend = aim.body.known ?")
    end = action.index("/* the facing and the look turn back", start)
    run_probe(tmp_path, "head-independent", r'''
#include <assert.h>
#include <math.h>
static int network;
static struct {struct {int known;} body;int good_aim_known;float good_yaw,good_pitch;} aim;
static int network_controller_aim(void){return network;}
static float yaw_in_range(float yaw){return yaw;}
static float controller_yaw_blend(float from,float to,float blend){return from+blend*(to-from);}
static void tilted_look(float ly,float lp,float *y,float *p){*y=ly;*p=lp;}
static void consume(float look_yaw,float look_pitch,float tracking,float gate,float *yaw,float *pitch){
 struct {float blend;} track={tracking};float blend,stick_yaw=look_yaw,stick_pitch=look_pitch;
 float hand_yaw=.75f,hand_pitch=-.2f;
''' + action[start:end] + r'''
}
int main(void){
 aim.body.known=aim.good_aim_known=1;aim.good_yaw=.75f;aim.good_pitch=-.2f;
 network=1;
 for(int h=0;h<12;h++)for(int t=0;t<=10;t++)for(int g=0;g<=10;g++){
  float yaw=h*.2f,pitch=h*.04f;
  consume(yaw,pitch,t*.1f,g*.1f,&yaw,&pitch);
  assert(yaw==.75f&&pitch==-.2f);
 }
 network=0;float yaw=0,pitch=0;consume(0,0,.5f,1,&yaw,&pitch);
 assert(yaw==.375f&&pitch==-.1f);
}
''')


def test_production_seat_stick_facing_clamps_and_resets_without_head(tmp_path):
    control = (ROOT / "source/game/player_control.c").read_text()
    run_probe(tmp_path, "seat", r'''
#include <assert.h>
#include <stddef.h>
#include <math.h>
#define NONE -1
#define TRUE 1
#define FALSE 0
#define _pi 3.14159265358979323846f
#define DEGREES_TO_RADIANS(d) ((d)*_pi/180.f)
#define PIN(v,lo,hi) fminf(hi,fmaxf(lo,v))
typedef float real;
typedef int boolean;
typedef struct {float yaw,pitch;} real_euler_angles2d;
typedef struct {float i,j,k;} real_vector3d;
struct unit_datum {struct {long parent_object_index;real_vector3d forward,up;} object;struct {short parent_seat_index;} unit;};
struct camera {float pitch_minimum,pitch_maximum;};
struct player_control_unit_camera_info {struct camera *camera;long unit_index;short seat_index;};
static struct unit_datum child={.object.parent_object_index=23},parent={.object.forward={1,0,0},.object.up={0,0,1}};
static struct camera camera={-.4f,.4f};
static real_euler_angles2d network_seat_stick_delta[4];
static int inhibited,active=1;static long current_unit=17;
static long player_control_get_unit_index(short local){assert(local==0);return current_unit;}
static struct unit_datum *unit_get(long index){assert(index==17||index==23);return index==17?&child:&parent;}
static void player_control_get_unit_camera_info(short local,struct player_control_unit_camera_info *out){assert(local==0);*out=(struct player_control_unit_camera_info){&camera,23,child.unit.parent_seat_index};}
static int player_control_seat_yaw_arc(short local,float *marker,float *lo,float *hi){assert(local==0);*marker=.5f;*lo=-.25f;*hi=.25f;return 1;}
static float signed_angular_difference(float from,float to){return remainderf(to-from,2*_pi);}
static void euler_angles2d_from_vector3d(real_euler_angles2d *a,const real_vector3d *v){a->yaw=atan2f(v->j,v->i);a->pitch=asinf(v->k);}
static void vector3d_from_euler_angles2d(real_vector3d *v,const real_euler_angles2d *a){*v=(real_vector3d){cosf(a->pitch)*cosf(a->yaw),cosf(a->pitch)*sinf(a->yaw),sinf(a->pitch)};}
static float angle_between_vectors3d(const real_vector3d *a,const real_vector3d *b){return acosf(a->i*b->i+a->j*b->j+a->k*b->k);}
static int director_inhibited_input(short local){(void)local;return inhibited;}
static int director_inhibited_facing(short local){(void)local;return inhibited;}
static int player_control_camera_control_is_active(void){return active;}
''' + function(control, "static real player_control_yaw_in_seat_arc(\n\treal yaw,\n\treal marker_yaw,\n\treal seat_yaw_minimum,\n\treal seat_yaw_maximum)\n{") +
              function(control, "real player_control_seat_yaw_clamped(\n\tshort local_player_index,\n\treal yaw)\n{") +
              function(control, "void player_control_network_seat_facing(") + r'''
int main(void){
 float yaw,pitch;network_seat_stick_delta[0]=(real_euler_angles2d){.1f,.05f};
 player_control_network_seat_facing(0,&yaw,&pitch);assert(fabsf(yaw-.6f)<.00001f&&pitch==.05f);
 network_seat_stick_delta[0]=(real_euler_angles2d){2,2};
 player_control_network_seat_facing(0,&yaw,&pitch);assert(fabsf(yaw-.75f)<.00001f&&pitch==.4f);
 inhibited=1;network_seat_stick_delta[0]=(real_euler_angles2d){-.1f,-.1f};
 player_control_network_seat_facing(0,&yaw,&pitch);assert(fabsf(yaw-.75f)<.00001f&&pitch==.4f);inhibited=0;
 active=0;player_control_network_seat_facing(0,&yaw,&pitch);assert(pitch==.4f);active=1;
 child.object.parent_object_index=NONE;player_control_network_seat_facing(0,&yaw,&pitch);assert(yaw==0&&pitch==0);
 child.object.parent_object_index=23;network_seat_stick_delta[0]=(real_euler_angles2d){0,0};
 player_control_network_seat_facing(0,&yaw,&pitch);assert(fabsf(yaw-.5f)<.00001f&&pitch==0);
 /* The same actual tilt correction as ordinary player-control bounds. */
 parent.object.up=(real_vector3d){.6f,0,.8f};camera=(struct camera){-.4f,.4f};
 network_seat_stick_delta[0].pitch=2;
 player_control_network_seat_facing(0,&yaw,&pitch);assert(pitch<0&&pitch>-.2f);
 current_unit=NONE;player_control_network_seat_facing(0,&yaw,&pitch);assert(yaw==0&&pitch==0);
}
''')


def test_production_seat_input_is_saved_before_hmd_and_excludes_magnetism(tmp_path):
    control = (ROOT / "source/game/player_control.c").read_text()
    reader = function(control, "static void get_local_player_input_blob(\n\tshort local_player_index,\n\treal time_delta_sec,\n\tstruct input_blob *input)\n{")
    stick_start = reader.index("if (replay_head_look)")
    stick_end = reader.index("if (player_control_camera_control_is_active())", stick_start)
    saved_start = reader.index("network_seat_stick_delta[local_player_index] = input->facing_delta;")
    saved_end = reader.index("\n\t\t\t\t}\n\t\t\t\telse", saved_start)
    magnetism = function(reader, "if (halo_controller_hand_aiming(local_player_index)")
    run_probe(tmp_path, "seat-input", r'''
#include <assert.h>
#define NONE -1
#define PIN(v,lo,hi) (v)
typedef float real;
typedef struct {float yaw,pitch;} real_euler_angles2d;
struct input_blob {real_euler_angles2d facing_delta;};
static struct {long unit_index;} player_storage={17},*player=&player_storage;
static struct {struct {long parent_object_index;} object;} unit;
static struct {float pitch,magnetism_level;real_euler_angles2d desired_angles;} controls,*control=&controls;
static struct {float yaw;} input_storage,*input_state=&input_storage;
static struct {struct {int count;void *address;} look_function;} constant_storage,*constants=&constant_storage;
static real_euler_angles2d network_seat_stick_delta[4];
static int network,head_active,stick_calls;
static float head_yaw_now,head_pitch_now;
static int halo_controller_network_head_camera(short local){return local==0&&network;}
static int halo_controller_hand_aiming(short local){(void)local;return 0;}
static void *unit_get(long index){assert(index==17);return &unit;}
#define unit_get(index) (&unit)
static float evaluate_piecewise_linear_function(int count,void *address,float v){(void)count;(void)address;return v;}
static void halo_stereo_replay_look(short local,float yaw){(void)local;(void)yaw;}
static void halo_stereo_stick_look(short local,float response,float dt,float *yaw,float *pitch){
 (void)local;(void)response;(void)dt;stick_calls++;*yaw=*pitch=0;
}
static int halo_stereo_head_look(short local,float current,float *yaw,float *pitch){
 (void)local;(void)current;*yaw=head_yaw_now;*pitch=head_pitch_now;return head_active;
}
static void read_input(struct input_blob *input){
 short local_player_index=0,gamepad_index=0;int replay_head_look=0;
 float clamped_yaw=.3f,clamped_pitch=-.2f,time_delta_sec=.016f;
''' + reader[stick_start:stick_end] + r'''
 input->facing_delta=(real_euler_angles2d){clamped_yaw,clamped_pitch};
''' + reader[saved_start:saved_end] + r'''
}
static void apply_magnetism(void){short local_player_index=0;
''' + magnetism + r'''
}
int main(void){
 struct input_blob input;unit.object.parent_object_index=23;network=head_active=1;
 for(int h=0;h<20;h++){
  head_yaw_now=h*.1f;head_pitch_now=h*.02f;
  read_input(&input);
  assert(network_seat_stick_delta[0].yaw==.3f&&network_seat_stick_delta[0].pitch==-.2f);
  assert(input.facing_delta.yaw==.3f+head_yaw_now&&input.facing_delta.pitch==-.2f+head_pitch_now);
  assert(stick_calls==0);
  controls.magnetism_level=1;apply_magnetism();assert(controls.magnetism_level==0);
 }
 unit.object.parent_object_index=NONE;read_input(&input);assert(stick_calls==1);
 network=0;controls.magnetism_level=1;apply_magnetism();assert(controls.magnetism_level==1);
}
''')
