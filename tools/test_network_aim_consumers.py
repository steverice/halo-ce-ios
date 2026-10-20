"""Run ordinary unit aiming and its shot, grenade and melee direction consumers."""
from test_network_controller_aim import ROOT, run_probe
from test_mp_synthetic_input import function


MATH = r'''
#include <assert.h>
#include <math.h>
#include <stddef.h>
#define TRUE 1
#define FALSE 0
#define NONE -1
#define TICKS_PER_SECOND 30.f
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(i) ((i)&65535)
#define match_assert_valid_real_normal3d(f,l,v) assert(isfinite((v)->i))
#define match_assert_valid_real_vector3d(f,l,v) assert(isfinite((v)->i))
#define match_assert_valid_real(f,l,v) assert(isfinite(v))
typedef int boolean;
typedef float real;
typedef struct {float i,j,k;} real_vector3d;
typedef real_vector3d real_point3d;
typedef struct {float yaw,pitch;} real_euler_angles2d;
typedef struct {float x0,x1,y0,y1;} real_rectangle2d;
typedef struct {float scale;real_vector3d forward,left,up,position;} real_matrix4x3;
static real_vector3d zero;
static real_vector3d *global_zero_vector3d=&zero,*global_origin3d=&zero;
static real_vector3d up={0,0,1},right={0,-1,0},down={0,0,-1};
static real_vector3d *global_right3d=&right,*global_down3d=&down;
static float dot_product3d(const real_vector3d *a,const real_vector3d *b){return a->i*b->i+a->j*b->j+a->k*b->k;}
static real_vector3d *cross_product3d(const real_vector3d *a,const real_vector3d *b,real_vector3d *v){*v=(real_vector3d){a->j*b->k-a->k*b->j,a->k*b->i-a->i*b->k,a->i*b->j-a->j*b->i};return v;}
static float magnitude_squared3d(const real_vector3d *v){return dot_product3d(v,v);}
static float normalize3d(real_vector3d *v){float l=sqrtf(magnitude_squared3d(v));if(l){v->i/=l;v->j/=l;v->k/=l;}return l;}
static void scale_vector3d(const real_vector3d *a,float n,real_vector3d *v){*v=(real_vector3d){a->i*n,a->j*n,a->k*n};}
static void subtract_vectors3d(const real_vector3d *a,const real_vector3d *b,real_vector3d *v){*v=(real_vector3d){a->i-b->i,a->j-b->j,a->k-b->k};}
static void vector_from_points3d(const real_point3d *a,const real_point3d *b,real_vector3d *v){subtract_vectors3d(b,a,v);}
static void point_from_line3d(const real_point3d *a,const real_vector3d *v,float t,real_point3d *out){*out=(real_point3d){a->i+v->i*t,a->j+v->j*t,a->k+v->k*t};}
static void vector3d_from_euler_angles2d(real_vector3d *v,const real_euler_angles2d *a){*v=(real_vector3d){cosf(a->pitch)*cosf(a->yaw),cosf(a->pitch)*sinf(a->yaw),sinf(a->pitch)};}
static void euler_angles2d_from_vector3d(real_euler_angles2d *a,const real_vector3d *v){a->yaw=atan2f(v->j,v->i);a->pitch=asinf(v->k);}
static void matrix4x3_transform_normal(const real_matrix4x3 *m,const real_vector3d *v,real_vector3d *o){real_vector3d x=*v;*o=(real_vector3d){x.i*m->forward.i+x.j*m->left.i+x.k*m->up.i,x.i*m->forward.j+x.j*m->left.j+x.k*m->up.j,x.i*m->forward.k+x.j*m->left.k+x.k*m->up.k};}
static void matrix4x3_inverse_transform_normal(const real_matrix4x3 *m,const real_vector3d *v,real_vector3d *o){real_vector3d x=*v;*o=(real_vector3d){dot_product3d(&x,&m->forward),dot_product3d(&x,&m->left),dot_product3d(&x,&m->up)};}
static void matrix4x3_rotation_from_vectors(real_matrix4x3 *m,const real_vector3d *f,const real_vector3d *u){m->forward=*f;m->up=*u;cross_product3d(u,f,&m->left);}
#define arccosine acosf
#define square_root sqrtf
#define sine sinf
#define cosine cosf
'''


def test_actual_ordinary_aim_and_weapon_direction_consumers(tmp_path):
    units = (ROOT / "source/units/units.c").read_text()
    math = (ROOT / "source/math/real_math.c").read_text()
    # Run the exact ordinary aiming branch, not a second implementation.
    start = units.index("if (unit->unit.player_index != NONE &&\n\t\t\thalo_player_hand_aim(")
    end = units.index("\n\t\t{\n\t\t\treal angle =", start)
    grenade = function(units, "static void unit_throw_grenade_release(\n\tlong unit_index,\n\tboolean premature)\n{")
    grenade_start = grenade.index("real scale = unit_definition->unit.grenade_velocity/TICKS_PER_SECOND;")
    grenade_end = grenade.index(";", grenade.index("scale_vector3d(", grenade_start)) + 1
    melee = function(units, "void unit_cause_player_melee_damage(")
    melee_start = melee.index("ray.i = facing->i * 0.8f")
    melee_end = melee.index(";", melee.index("ray.k =", melee_start)) + 1
    run_probe(tmp_path, "consumers", MATH + r'''
struct unit_datum {struct {real_vector3d forward,up;} object;struct {long player_index;
 real_vector3d aiming_vector,desired_aiming_vector,aiming_velocity;
 struct {int aiming_with_euler_screen,looking_with_euler_screen;real_rectangle2d aiming_screen_bounds,looking_screen_bounds;} animation;} unit;};
static struct unit_datum datum;
static struct unit_datum *unit_get(long index){assert(index==17);return &datum;}
static void object_get_orientation(long index,real_vector3d *f,real_vector3d *u){(void)index;*f=datum.object.forward;*u=datum.object.up;}
static void unit_verify_vectors(long index,const char *reason){(void)index;(void)reason;}
static int halo_player_hand_aim(long index){assert(index==1);return 0;}
static void unit_euler_aiming_update(const real_matrix4x3 *m,real_vector3d *a,const real_vector3d *d,real_vector3d *v,const real_rectangle2d *b,float l,float c){(void)m;(void)a;(void)d;(void)v;(void)b;(void)l;(void)c;assert(0);}
static void unit_get_camera_position(long index,real_point3d *p){(void)index;*p=(real_point3d){0,0,2};}
static void object_get_velocities(long index,real_vector3d *v,void *angular){(void)index;(void)angular;*v=zero;}
''' + function(math, "real_vector3d *rotate_vector_about_axis(") +
              function(math, "void angular_accelerate_to_position(") +
              function(units, "boolean unit_clip_to_aiming_bounds(") +
              function(units, "void unit_adjust_projectile_ray(") + r'''
static void consume(float aiming_velocity_limit,float aiming_angular_acceleration_limit){
 long unit_index=17;struct unit_datum *unit=&datum;
''' + units[start:end] + r'''
}
static real_vector3d grenade_direction(void){
 struct {struct {float grenade_velocity;} unit;} definition={.unit.grenade_velocity=15};
 typeof(definition) *unit_definition=&definition;struct unit_datum *unit=&datum;
 real_vector3d initial_velocity;
''' + grenade[grenade_start:grenade_end] + r'''
 return initial_velocity;
}
static real_vector3d central_melee_ray(void){
 struct unit_datum *unit=&datum;real_vector3d *facing=&unit->unit.aiming_vector;
 real_vector3d cross={0,0,0},perpendicular={0,0,0},ray;
 int inner_index=0,outer_index=0;double inner_component_i=0;
''' + melee[melee_start:melee_end] + r'''
 return ray;
}
int main(void){
 datum.object.forward=(real_vector3d){1,0,0};datum.object.up=up;datum.unit.player_index=1;
 datum.unit.animation.aiming_with_euler_screen=1;
 /* Shipped standing rifle screen in both A10 and Damnation: +/- pi/2.
    Both player units have zero aiming velocity/acceleration limits. */
 datum.unit.animation.aiming_screen_bounds=(real_rectangle2d){-1.57079637f,1.57079637f,-1.57079637f,1.57079637f};
 real_euler_angles2d requested={2.2f,.4f};vector3d_from_euler_angles2d(&datum.unit.desired_aiming_vector,&requested);
 datum.unit.aiming_vector=datum.object.forward;consume(0,0);
 real_euler_angles2d actual;euler_angles2d_from_vector3d(&actual,&datum.unit.aiming_vector);
 assert(fabsf(actual.yaw-1.57079637f)<.00001f&&fabsf(actual.pitch-.4f)<.00001f);
 real_point3d origin={0,0,2};real_vector3d shot={1,0,0};float velocity;
 for(int scoped=0;scoped<2;scoped++){
  unit_adjust_projectile_ray(17,&origin,&shot,&velocity,1,1);
  assert(fabsf(shot.j-datum.unit.aiming_vector.j)<.00001f&&shot.k==datum.unit.aiming_vector.k);
 }
 real_vector3d grenade=grenade_direction(),melee=central_melee_ray();
 assert(fabsf(grenade.j-datum.unit.aiming_vector.j*.5f)<.00001f);
 assert(fabsf(melee.j-datum.unit.aiming_vector.j*.8f)<.00001f&&fabsf(melee.k-datum.unit.aiming_vector.k*.8f)<.00001f);
 /* Cinematic unit's real 2*pi radians/s, 4*pi radians/s^2: still ordinary. */
 datum.unit.animation.aiming_with_euler_screen=0;datum.unit.aiming_vector=datum.object.forward;
 datum.unit.desired_aiming_vector=(real_vector3d){0,1,0};datum.unit.aiming_velocity=zero;
 consume(6.28318548f/30.f,12.566371f/900.f);
 assert(datum.unit.aiming_vector.j>0&&datum.unit.aiming_vector.j<.02f);
}
''')


def test_actual_peer_facing_conversion_in_tilted_seat(tmp_path):
    players = (ROOT / "source/game/players.c").read_text()
    run_probe(tmp_path, "tilted-seat", MATH + r'''
#define TEST_FLAG(flags,bit) ((flags)&(1u<<(bit)))
#define _object_mask_vehicle 1
#define _unit_seat_third_person_camera_bit 4
struct unit_seat {unsigned int flags;};
struct unit_definition {struct {int seats;} unit;};
struct unit_datum {struct {long parent_object_index;} object;struct {short parent_seat_index;} unit;};
struct player_datum {long unit_index;};
struct players_vehicle_datum {long definition_index;struct {real_vector3d up;} object;};
static struct player_datum player={17};static struct unit_datum unit={.object.parent_object_index=23};
static struct players_vehicle_datum vehicle={.object.up={0,.6f,.8f}};
static struct unit_definition definition;static struct unit_seat probe_seat;
#define TAG_BLOCK_GET_ELEMENT(block,index,type) (&probe_seat)
static struct player_datum *player_get(long index){assert(index==1);return &player;}
static struct unit_datum *unit_get(long index){assert(index==17);return &unit;}
static void *object_try_and_get_and_verify_type(long index,int type){assert(index==23);(void)type;return &vehicle;}
static struct unit_definition *vehicle_definition_get(long index){(void)index;return &definition;}
''' + function(players, "void player_aiming_vector_from_facing(") + r'''
int main(void){
 real_euler_angles2d facing={1.57079632679f,0};real_vector3d aimed;
 player_aiming_vector_from_facing(1,&aimed,&facing);
 assert(fabsf(aimed.i)<.00001f&&fabsf(aimed.j-.8f)<.00001f&&fabsf(aimed.k+.6f)<.00001f);
 probe_seat.flags=1u<<4;player_aiming_vector_from_facing(1,&aimed,&facing);
 assert(fabsf(aimed.i)<.00001f&&fabsf(aimed.j-1)<.00001f&&aimed.k==0);
 unit.object.parent_object_index=NONE;player_aiming_vector_from_facing(1,&aimed,&facing);
 assert(fabsf(aimed.j-1)<.00001f&&aimed.k==0);
}
''')


def test_actual_biped_locomotion_follows_ordinary_facing(tmp_path):
    bipeds = (ROOT / "source/units/bipeds.c").read_text()
    start = bipeds.index("if (cinematic_in_progress() ||\n\t\t\t\t\tTEST_FLAG(")
    end = bipeds.index("\n\t\t\t\tif (game_players_are_double_speed())", start)
    rotation_start = bipeds.index("facing.i = physics->new_velocity.i =")
    rotation_end = bipeds.index(";", bipeds.index("facing.j = physics->new_velocity.j =", rotation_start)) + 1
    run_probe(tmp_path, "locomotion", MATH + r'''
#define TEST_FLAG(flags,bit) ((flags)&(1u<<(bit)))
#define _biped_uses_old_player_physics_bit 0
#define _unit_base_seat_alert 1
#define _unit_control_look_dont_turn_bit 2
struct game_globals_player_information {float walking_speed,run_forward_speed,run_backward_speed,
 run_sideways_speed,run_acceleration,sneak_forward_speed,sneak_backward_speed,sneak_sideways_speed,
 sneak_acceleration,stun_movement_penalty,airborne_acceleration;};
static struct {float speed_multiplier;} player={1};
static void *player_get(long index){(void)index;return &player;}
#define player_get(index) (&player)
static int cinematic_in_progress(void){return 0;}
struct biped_fixture {struct {long player_index;float body_stun;real_vector3d throttle,desired_facing_vector;
 unsigned long control_flags;struct {int base_seat_index;} animation;} unit;struct {float crouch;} biped;};
struct physics_fixture {float acceleration_maximum,airborne_acceleration_maximum;real_vector3d movement_desired,forward,new_velocity;};
static struct physics_fixture speeds(struct biped_fixture *biped){
 struct game_globals_player_information source={.sneak_forward_speed=.9f,.sneak_backward_speed=.6f,.sneak_sideways_speed=.5f,.sneak_acceleration=.1f};
 struct game_globals_player_information *player_information=&source,player_information_block;
 struct {struct {int flags;} biped;} def={.biped.flags=1},*definition=&def;
 float body_stun_scale=1,crouch,uncrouch,stun_scale,forward_speed,sideways_speed,acceleration,movement_scale=1;
 struct physics_fixture physics={0};
''' + bipeds[start:end] + r'''
 return physics;
}
static real_vector3d rotate(struct physics_fixture *physics){real_vector3d facing;
''' + bipeds[rotation_start:rotation_end] + r'''
 return physics->new_velocity;
}
int main(void){
 const float fx[4]={1,0,-1,0},fy[4]={0,1,0,-1};
 for(int heading=0;heading<4;heading++)for(int crouch=0;crouch<2;crouch++)for(int move=0;move<4;move++){
  struct biped_fixture biped={.unit.player_index=1};
  biped.unit.desired_facing_vector=(real_vector3d){fx[heading],fy[heading],0};biped.biped.crouch=crouch;
  biped.unit.throttle=(real_vector3d){move<2?(move==0?1:-1):0,move>=2?(move==2?1:-1):0,0};
  struct physics_fixture physics=speeds(&biped);real_vector3d world=rotate(&physics);
  float forward=move==0?(crouch?.9f:2.25f):move==1?-(crouch?.6f:2.f):0;
  float side=move==2?(crouch?.5f:2.f):move==3?-(crouch?.5f:2.f):0;
  assert(fabsf(world.i-(forward*fx[heading]-side*fy[heading])/30.f)<.00001f);
  assert(fabsf(world.j-(side*fx[heading]+forward*fy[heading])/30.f)<.00001f);
 }
}
''')
