/* The real render_contrail function with a nonempty two-point contrail. */

#include "harness.h"
#include <math.h>

typedef unsigned long pixel32;
typedef struct { real x, y; } real_point2d;
typedef struct { real i, j; } real_vector2d;
typedef struct { real alpha, red, green, blue; } real_argb_color;
typedef struct { real red, green, blue; } real_rgb_color;

#include "config.inc"

enum { _contrail_first_point_unfaded_bit, _contrail_last_point_unfaded_bit,
       _contrail_fades_slowly_bit = 6 };
enum { _contrail_state_width_bit = 4, _contrail_state_color_bit };
enum { _contrail_texture_repeats_u_bit = 6, _contrail_texture_repeats_v_bit };
enum { _contrail_point_transitioning_bit = 1 };
enum { _rasterizer_lock_none, _rasterizer_lock_contrail };
enum { _error_silent };

struct contrail_vertex { real_point3d point; real_point2d texture; pixel32 color; };
struct bitmap_data { int unused; };
struct shader { int unused; };
struct contrail_shader { short framebuffer_fade_mode; };
struct contrail_point_state
{
    unsigned long scale_flags;
    real width;
    real_argb_color color_lower_bound, color_upper_bound;
};
struct contrail_definition
{
    unsigned long flags, scale_flags;
    short render_type;
    real texture_repeats_u, texture_repeats_v;
    struct { long index; } bitmap;
    struct contrail_shader shader;
    struct { struct contrail_point_state *data; } states;
};
struct contrail_datum
{
    long definition_index, object_index;
    short sequence_index, frame_index, attachment_index;
    short contrail_point_counts[4];
    long first_contrail_point_indices[4];
    real texture_offset_u, texture_offset_v, density;
};
struct contrail_point_datum
{
    byte flags;
    char state_index;
    real time, density;
    real_point3d position;
    long next_contrail_point_index;
};
struct object_attachment_definition { short change_color_reference; };
struct object_definition { struct { struct { struct object_attachment_definition *data; } attachments; } object; };
struct object_datum { long definition_index; struct { real_rgb_color outgoing_change_colors[4]; } object; };

static struct contrail_point_datum points[2];
static struct contrail_point_state states[1];
static struct object_datum fake_object;
static struct object_definition fake_object_definition;
static boolean reported_bad_change_color;
static struct bitmap_data bitmap;
static real_point3d origin = { 0, 0, 0 };
static real_vector3d up = { 0, 0, 1 };
static real_point3d *global_origin3d = &origin;
static real_vector3d *global_up3d = &up;
static struct { struct { real_point3d position; } camera; } render;
static struct { int current_lock_operation; } rasterizer_globals;
static int events[16], event_count, draws, errors, next_index;
static short triangles[6];
static struct contrail_vertex vertices[4];

enum { TRIANGLE_NEW = 1, VERTEX_NEW, TRIANGLE_LOCK, VERTEX_LOCK,
       TRIANGLE_UNLOCK, VERTEX_UNLOCK, DRAW, TRIANGLE_DELETE, VERTEX_DELETE };

static void event(int kind, long index)
{
    CHECK(event_count < 16, "event log overflow");
    events[event_count++] = kind * 1000 + (int)index;
}
static struct bitmap_data *bitmap_group_get_bitmap_from_sequence(long index, short sequence, short frame)
{ (void)index; (void)sequence; (void)frame; return &bitmap; }
static int _texture_cache_bitmap_get_hardware_format(struct bitmap_data *value, int a, int b)
{ (void)value; (void)a; (void)b; return 1; }
static long rasterizer_dynamic_triangles_new(short count)
{ CHECK(count == 2, "triangle count %d", count); event(TRIANGLE_NEW, next_index); return next_index; }
static long rasterizer_dynamic_vertices_new(int type, short count)
{ CHECK(type == 6 && count == 4, "vertex request %d/%d", type, count); event(VERTEX_NEW, next_index + 100); return next_index + 100; }
static short *rasterizer_dynamic_triangles_lock(long index)
{ event(TRIANGLE_LOCK, index); return triangles; }
static struct contrail_vertex *rasterizer_dynamic_vertices_lock(long index)
{ event(VERTEX_LOCK, index); return vertices; }
static void rasterizer_dynamic_triangles_unlock(long index) { event(TRIANGLE_UNLOCK, index); }
static void rasterizer_dynamic_vertices_unlock(long index) { event(VERTEX_UNLOCK, index); }
static void rasterizer_dynamic_triangles_delete(long index) { event(TRIANGLE_DELETE, index); }
static void rasterizer_dynamic_vertices_delete(long index) { event(VERTEX_DELETE, index); }
static void rasterizer_dynamic_unlit_geometry_draw(struct shader const *shader, struct bitmap_data *image,
    int first, long triangle_index, long vertex_index, short count, real_point3d const *position, int flags)
{ (void)shader; (void)image; (void)first; (void)count; (void)position; (void)flags;
  event(DRAW, triangle_index); CHECK(vertex_index == next_index + 100, "draw vertex index"); draws++; }
static const char *tag_get_name(long index) { (void)index; return "test"; }
static void error(int kind, const char *format, ...) { (void)kind; (void)format; errors++; }
static real contrail_fade(struct contrail_definition *definition, short mode,
    real_point3d const *position, real_vector3d const *normal)
{ (void)definition; (void)mode; (void)position; (void)normal; return 1.f; }
static pixel32 real_argb_color_to_pixel32(real_argb_color const *color)
{ (void)color; return 0xFFFFFFFFUL; }
static void normalize2d(real_vector2d *vector) { (void)vector; }
static void normalize3d(real_vector3d *vector) { (void)vector; }
int halo_projectile_point_offset(long object_index, long contrail_index, long point_index,
    const float *position, float *offset)
{
    (void)object_index; (void)contrail_index; (void)point_index; (void)position; (void)offset;
    return 0;
}
#define PIN(value, low, high) ((value) < (low) ? (low) : ((value) > (high) ? (high) : (value)))
#define VALID_INDEX(index, limit) ((index) >= 0 && (index) < (limit))
#define NUMBER_OF_OBJECT_CHANGE_COLORS 4
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&(block)->data[(int)(index)])
#define datum_get(array, index) (&points[index])
#define contrail_point_data NULL
#define object_get(index) (&fake_object)
#define object_definition_get(index) (&fake_object_definition)

#include "under_test.inc"

static void check_run(short render_type, int expected_draws, int expected_errors)
{
    struct contrail_datum contrail = { 0 };
    struct contrail_definition definition = { 0 };
    int expected[9], expected_count, i;

    memset(events, 0, sizeof(events)); event_count = draws = errors = 0;
    memset(points, 0, sizeof(points)); memset(states, 0, sizeof(states));
    contrail.object_index = NONE;
    contrail.contrail_point_counts[0] = 2;
    contrail.first_contrail_point_indices[0] = 0;
    points[0].next_contrail_point_index = 1;
    points[1].next_contrail_point_index = NONE;
    states[0].width = 2.f;
    definition.states.data = states;
    definition.render_type = render_type;
    next_index += 7;
    render_contrail(&contrail, &definition, 0, 17);

    CHECK(draws == expected_draws, "draw count %d", draws);
    CHECK(errors == expected_errors, "error count %d", errors);
    CHECK(rasterizer_globals.current_lock_operation == _rasterizer_lock_none,
          "contrail lock remains active");
    expected[0] = TRIANGLE_NEW * 1000 + next_index;
    expected[1] = VERTEX_NEW * 1000 + next_index + 100;
    expected[2] = TRIANGLE_LOCK * 1000 + next_index;
    expected[3] = VERTEX_LOCK * 1000 + next_index + 100;
    expected[4] = TRIANGLE_UNLOCK * 1000 + next_index;
    expected[5] = VERTEX_UNLOCK * 1000 + next_index + 100;
    expected_count = 6;
    if (expected_draws) expected[expected_count++] = DRAW * 1000 + next_index;
    expected[expected_count++] = TRIANGLE_DELETE * 1000 + next_index;
    expected[expected_count++] = VERTEX_DELETE * 1000 + next_index + 100;
    CHECK(event_count == expected_count, "event count %d, expected %d", event_count, expected_count);
    for (i = 0; i < expected_count; i++)
        CHECK(events[i] == expected[i], "event %d = %d, expected %d", i, events[i], expected[i]);
}

int main(int argc, char **argv)
{
    const char *case_name = argc > 1 ? argv[1] : "";
    CASE("ground") { check_run(_contrail_render_type_ground, 0, 1); return 0; }
    CASE("unknown") { check_run(99, 0, 1); return 0; }
    CASE("repeat-then-supported")
    {
        check_run(_contrail_render_type_ground, 0, 1);
        check_run(_contrail_render_type_ground, 0, 1);
        check_run(_contrail_render_type_vertical, 1, 0);
        return 0;
    }
    fprintf(stderr, "unknown case '%s'\n", case_name);
    return 2;
}
