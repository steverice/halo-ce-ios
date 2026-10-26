"""`objects_update` with an active, automatically deactivated object outside the map (no cluster).

A weapon that never touched the map, dropped by a unit that died outside it, is reconnected with no
cluster and then activated by `object_detach`. The game asserted on it (objects.c #369) the next time
the players' view changed. This runs `objects_update`, taken from the source, over such objects.
"""
import shutil
import subprocess

from test_network_controller_aim import ROOT, run_probe
from test_mp_synthetic_input import function

PROLOGUE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int boolean;
typedef float real;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define SHORT_BITS 16
#define LONG_BITS 32
#define LONG_BITS_BITS 5
#define FLAG(b) (1u<<(b))
#define TEST_FLAG(f,b) (((f)&(1u<<(b)))!=0)
#define SET_FLAG(f,b,v) ((f)=(v)?((f)|(1u<<(b))):((f)&~(1u<<(b))))
#define BIT_VECTOR_SIZE_IN_LONGS(n) (((n)+(LONG_BITS-1))>>LONG_BITS_BITS)
#define BIT_VECTOR_SIZE_IN_BYTES(n) (sizeof(unsigned long)*BIT_VECTOR_SIZE_IN_LONGS(n))
#define BIT_VECTOR_TEST_FLAG(v,b) (TEST_FLAG((v)[(b)>>LONG_BITS_BITS],((b)&(LONG_BITS-1))))
#define DATUM_INDEX_NEW(i,salt) ((i)|((salt)<<SHORT_BITS))
#define csmemcmp memcmp
/* as in a checked build, a failed assertion stops the game */
#define match_assert(file,line,value) do { if (!(value)) { fprintf(stderr, "halt: objects.c #%d\n", line); exit(3); } } while (0)
enum { _object_header_active_bit, _object_header_automatically_deactivate_bit,
       _object_header_connected_to_map_bit, _object_header_child_bit,
       _object_header_being_created_bit, _object_header_being_deleted_bit,
       _object_header_do_not_update_bit };
enum { _object_deleted_when_deactivated_bit };
#define _object_mask_unit 0
struct profile_section { char const *name; long index; boolean enabled; };
struct object_datum { struct { unsigned long flags; long parent_object_index, next_object_index; } object; };
struct unit_datum { struct { long player_index; } unit; };
struct object_header_datum { short identifier; unsigned long flags; short type; short cluster_index;
                             struct object_datum *datum; };
#define OBJECTS 6
#define CLUSTERS 40
static struct object_datum objects[OBJECTS];
static struct object_header_datum headers[OBJECTS];
static struct { struct object_header_datum *data; short count; } object_header_storage = { headers, OBJECTS };
#define object_header_data (&object_header_storage)
static unsigned long last_bits[2], active_bits[2], pvs[2];
static struct { long active_garbage_object_count; unsigned long *last_active_cluster_bits, *active_cluster_bits; }
    globals = { 0, last_bits, active_bits }, *object_globals = &globals;
static struct { struct { short count; } clusters; } bsp = { { CLUSTERS } };
static int deleted[OBJECTS], updated[OBJECTS];
static void profile_enter(struct profile_section s) { (void)s; }
static void profile_exit(struct profile_section s) { (void)s; }
/* as cseries/profile_sections.h has them outside the profiling build */
#define profile_scope_enter(variable)
#define profile_scope_exit(variable)
#define profile_scope(variable, statement) statement
static long game_time_get(void) { return 0; }
static boolean game_players_are_double_speed(void) { return FALSE; }
#define global_structure_bsp_get() (&bsp)
static unsigned long *players_get_combined_pvs(void) { return pvs; }
static void object_delete_initial_recursive(long i, boolean f) { (void)f; deleted[i & 0xffff]++; }
static void object_deactivate(long i) { SET_FLAG(headers[i].flags, _object_header_active_bit, FALSE); }
static void object_activate(long i) { SET_FLAG(headers[i].flags, _object_header_active_bit, TRUE); }
static void structure_decals_update(unsigned long *a, unsigned long *b, short c) { (void)a; (void)b; (void)c; }
static struct object_datum *object_get(long i) { return &objects[i & 0xffff]; }
static struct unit_datum *unit_get(long i) { (void)i; static struct unit_datum u = { { NONE } }; return &u; }
static void object_update(long i) { updated[i & 0xffff]++; }
static void object_delete_recursive(long i, boolean f) { (void)i; (void)f; }
static void objects_garbage_collection(void) {}
void platform_log(char const *format, ...) { (void)format; }
'''

EPILOGUE = r'''
static void object_new_test(short i, short cluster, boolean active, boolean deleted_when_deactivated)
{
    headers[i].identifier = (short)(0x100 + i);
    headers[i].datum = &objects[i];
    headers[i].cluster_index = cluster;
    headers[i].flags = FLAG(_object_header_automatically_deactivate_bit) | FLAG(_object_header_connected_to_map_bit);
    if (active) SET_FLAG(headers[i].flags, _object_header_active_bit, TRUE);
    objects[i].object.parent_object_index = NONE;
    objects[i].object.next_object_index = NONE;
    objects[i].object.flags = deleted_when_deactivated ? FLAG(_object_deleted_when_deactivated_bit) : 0;
}

int main(void)
{
    /* the players see clusters 3 and 35 */
    pvs[0] = FLAG(3);
    pvs[1] = FLAG(35 - 32);
    object_new_test(0, NONE, TRUE, FALSE); /* a dropped weapon outside the map */
    object_new_test(1, NONE, TRUE, TRUE);  /* the same, deleted when deactivated */
    object_new_test(2, 3, TRUE, FALSE);    /* in view: stays active */
    object_new_test(3, 7, TRUE, FALSE);    /* out of view: deactivated */
    object_new_test(4, 35, FALSE, FALSE);  /* inactive, in view: activated */
    object_new_test(5, NONE, FALSE, FALSE);/* inactive, outside the map: left alone */
    objects_update();
    assert(!TEST_FLAG(headers[0].flags, _object_header_active_bit) && !deleted[0]);
    assert(deleted[1] == 1);
    assert(TEST_FLAG(headers[2].flags, _object_header_active_bit));
    assert(!TEST_FLAG(headers[3].flags, _object_header_active_bit));
    assert(TEST_FLAG(headers[4].flags, _object_header_active_bit));
    assert(!TEST_FLAG(headers[5].flags, _object_header_active_bit));
    /* the view unchanged: no activation pass, and no assertion */
    object_new_test(0, NONE, TRUE, FALSE);
    objects_update();
    assert(TEST_FLAG(headers[0].flags, _object_header_active_bit));
    puts("objects_update cluster NONE: PASS");
    return 0;
}
'''


def program(objects: str) -> str:
    return PROLOGUE + function(objects, "void objects_update(\n") + EPILOGUE


def test_objects_update_deactivates_an_active_object_outside_the_map(tmp_path):
    objects = (ROOT / "source/objects/objects.c").read_text()
    run_probe(tmp_path, "objects_cluster_none", program(objects))


def test_negative_control_the_original_assertion_fires(tmp_path):
    objects = (ROOT / "source/objects/objects.c").read_text()
    original = objects.replace(
        "if (object_header->cluster_index==NONE ||\n\t\t\t\t\t\t!BIT_VECTOR_TEST_FLAG(",
        "match_assert(\"c:\\\\halo\\\\SOURCE\\\\objects\\\\objects.c\", 369, object_header->cluster_index!=NONE);\n"
        "\t\t\t\t\tif (!BIT_VECTOR_TEST_FLAG(")
    assert original != objects, "the fixed condition was not found"
    compiler = shutil.which("clang") or shutil.which("cc")
    source = tmp_path / "objects_cluster_none_original.c"
    source.write_text(program(original))
    binary = tmp_path / "objects_cluster_none_original"
    compiled = subprocess.run([compiler, "-std=gnu11", "-O1", "-g", str(source), "-o", str(binary)],
                              capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    # (stopped by the game's assertion itself, before the out-of-range bit is read)
    assert result.returncode == 3 and "halt: objects.c #369" in result.stderr, result.stderr
