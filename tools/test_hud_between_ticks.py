"""Exercise production HUD phases without advancing motion sensor history per frame."""
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_checkpoint_timing import function

ROOT = Path(__file__).resolve().parents[1]


def test_sweep_and_flash_follow_render_time():
    sensor = (ROOT / "source/interface/motion_sensor.c").read_text()
    hud = (ROOT / "source/interface/hud_draw.c").read_text()
    clock = (ROOT / "port/linux/game/render_interpolation.c").read_text()
    program = r'''
#include <assert.h>
#include <math.h>
#include <string.h>
typedef float real;
typedef float pixel32;
typedef int boolean;
typedef struct { real x, y; } real_point2d;
typedef real_point2d real_vector2d;
typedef struct { real r, g, b; } real_rgb_color;
typedef struct { real n[3]; } real_vector3d;
typedef struct { real n[4]; } real_argb_color;
#define TICKS_PER_SECOND 30
#define STACK_BUFFER_LENGTH 4
#define PIN(x,lo,hi) fminf(fmaxf((x),(lo)),(hi))
#define TEST_FLAG(x,bit) ((x) & (1 << (bit)))
#define _hud_flash_reverse_colors_bit 0
#define match_assert_stack_frame(...) ((void)0)
#define csmemset memset
#define square_root sqrtf
#define sine sinf
#define cosine cosf
#define match_assert(file,line,test) assert(test)
enum { _blip_type_none, _blip_type_custom, _blip_type_friend };
struct motion_sensor_datum { real yaw; };
static real_rgb_color blip_colors[3];
static struct { struct { real motion_sensor_range; } defaults; } hud_state = {{100}};
static typeof(hud_state) *hud_globals=&hud_state;
static real drawn_radius;
static void rotate_vector2d(const real_vector2d *a, real s, real c, real_vector2d *b) {
    real x=a->x*c-a->y*s, y=a->x*s+a->y*c; b->x=x; b->y=y;
}
static real magnitude_squared2d(const real_vector2d *a) { return a->x*a->x+a->y*a->y; }
static void scale_vector2d(const real_vector2d *a, real s, real_vector2d *b) { b->x=a->x*s; b->y=a->y*s; }
static real blip_size_get(char type) { return type+1; }
static void rasterizer_hud_motion_sensor_blip_draw(const real_point2d *p, real fade, real size, const real_rgb_color *c, boolean custom) { drawn_radius=size; }
struct hud_color_definition {
    real flash_period, flash_delay, flash_length;
    int number_of_flashes, flash_flags;
    pixel32 color, flash_color;
};
static struct { long last_update_time; } sensor_state;
static typeof(sensor_state) *motion_sensor_globals = &sensor_state;
static int interpolation_rendering = 1, updates;
static real interpolation_fraction = 1.0f, sweep_theta;
static real scale[6] = {0,0,0,0,0,1};
static long tick = 34;
static long game_time_get(void) { return tick; }
static real render_interpolation_fraction(void) { return interpolation_rendering ? interpolation_fraction : 1.0f; }
static void motion_sensor_update(void) { updates++; sensor_state.last_update_time = tick; }
static long get_return_eip(void) { return 0; }
static void pixel32_to_real_argb_color(pixel32 p, real_argb_color *c) {
    for (int i=0;i<4;i++) c->n[i]=p;
}
static pixel32 real_argb_color_to_pixel32(const real_argb_color *c) { return c->n[0]; }
static void vectors_interpolate(const real_vector3d *a, const real_vector3d *b, real t, real_vector3d *c) {
    for (int i=0;i<3;i++) c->n[i]=a->n[i]+(b->n[i]-a->n[i])*t;
}
static void scalars_interpolate(real a, real b, real t, real *c) { *c=a+(b-a)*t; }
/* FUNCTIONS */
int main(void) {
    struct hud_color_definition color = {2, 0.25f, 1, 1, 0, 0, 1};
    real last_sweep = 0, last_flash = 0;
    for (int frame=0;frame<4;frame++) {
        interpolation_fraction = (frame+1)*0.25f;
        motion_sensor_tick();
        real flash = get_flash_color(&color, 30);
        if (frame) { assert(sweep_theta != last_sweep); assert(flash != last_flash); }
        last_sweep=sweep_theta; last_flash=flash;
    }
    assert(updates == 1 && sensor_state.last_update_time == tick);
    real full_sweep=sweep_theta, full_flash=last_flash;
    interpolation_rendering=0; interpolation_fraction=0.25f;
    motion_sensor_tick();
    assert(sweep_theta == full_sweep && get_flash_color(&color,30) == full_flash);
    assert(updates == 1);
    interpolation_rendering=1; interpolation_fraction=1;
    motion_sensor_tick();
    assert(sweep_theta == full_sweep && get_flash_color(&color,30) == full_flash);
    tick=35; motion_sensor_tick(); assert(updates==2);
    assert(get_flash_color(&color,0)==1); /* Existing constant-color sentinel. */
    /* Countdown HUDs intentionally use a reference in the future. */
    interpolation_fraction=0.5f;
    real future=get_flash_color(&color,tick+3);
    real phase=(-3.5f/30.0f);
    real expected=sqrtf(1.0f-(cos(phase*6.283f)+1.0f)*0.5f);
    assert(fabsf(future-expected)<0.00001f && future>0);
    interpolation_rendering=0;
    phase=-3.0f/30.0f;
    expected=sqrtf(1.0f-(cos(phase*6.283f)+1.0f)*0.5f);
    assert(fabsf(get_flash_color(&color,tick+3)-expected)<0.00001f);
    real original_future=get_flash_color(&color,tick+3);
    interpolation_rendering=1;
    interpolation_fraction=1;
    assert(get_flash_color(&color,tick+3)==original_future);
    assert(get_flash_color(&color,tick)==0);
    struct motion_sensor_datum sensor={0.25f}, before=sensor;
    real_point2d point={1,2};
    real last_custom=0;
    for (int frame=0;frame<4;frame++) {
        interpolation_fraction=(frame+1)*0.25f;
        render_blip(&sensor,_blip_type_custom,0,&point,1,1,2);
        if (frame) assert(drawn_radius!=last_custom);
        last_custom=drawn_radius;
        render_blip(&sensor,_blip_type_friend,0,&point,1,1,2);
        assert(drawn_radius==3);
        assert(!memcmp(&sensor,&before,sizeof(sensor)));
    }
    interpolation_rendering=0;
    render_blip(&sensor,_blip_type_custom,0,&point,1,1,2);
    assert(drawn_radius==last_custom);
    assert(updates==2);
    interpolation_rendering=1;
    tick=0; interpolation_fraction=0;
    assert(render_interpolation_game_time_sec(0)==0);
    return 0;
}
'''
    functions = "\n".join([
        function(clock, "render_interpolation_game_time_sec"),
        function(sensor, "motion_sensor_tick"),
        function(sensor, "render_blip"),
        function(hud, "get_flash_color"),
    ])
    with tempfile.TemporaryDirectory(prefix="hud-between-ticks-") as directory:
        source = Path(directory) / "probe.c"
        binary = Path(directory) / "probe"
        source.write_text(program.replace("/* FUNCTIONS */", functions))
        subprocess.run([shutil.which("clang") or "cc", str(source), "-lm", "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    test_sweep_and_flash_follow_render_time()
