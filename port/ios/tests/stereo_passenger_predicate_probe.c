#include <stdio.h>
#include <string.h>

#define NONE (-1)
#define FLAG(bit) (1UL << (bit))
#define TEST_FLAG(flags, bit) (((flags) & FLAG(bit)) != 0)
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->address)[index])

#include "config.inc"

enum { _object_mask_vehicle = 1, _object_mask_unit = 2 };
struct unit_seat { unsigned long flags; };
struct tag_block { int count; void *address; };
struct unit_definition { struct { struct tag_block seats; } unit; };
struct object_datum { struct { long parent_object_index; } object; };
struct unit_datum {
	long definition_index;
	struct { long parent_object_index; } object;
	struct { long driver_object_index, gunner_object_index; short parent_seat_index, current_weapon_index;
		struct { float i, j, k; } aiming_vector; } unit;
};
struct player_datum { long unit_index; };

static struct player_datum player;
static struct unit_datum rider, vehicle;
static struct unit_seat seats[2];
static struct unit_definition definition;
static struct object_datum weapon;
static long weapon_index = 5;
static int player_present, parent_is_vehicle;

static long local_player_get_player_index(short local) { (void)local; return player_present ? 1 : NONE; }
static struct player_datum *player_get(long index) { (void)index; return &player; }
static struct unit_datum *unit_get(long index) { return index == 2 ? &rider : &vehicle; }
static struct unit_datum *object_try_and_get_and_verify_type(long index, int mask)
{
	if (index != 3) return NULL;
	if (mask == _object_mask_unit || (mask == _object_mask_vehicle && parent_is_vehicle)) return &vehicle;
	return NULL;
}
static struct unit_definition *unit_definition_get(long index) { (void)index; return &definition; }
static long player_control_get_unit_index(short local) { (void)local; return player.unit_index; }
static long unit_inventory_get_weapon(long index, short slot)
{
	return index == 2 && slot == 0 ? weapon_index : NONE;
}
static struct object_datum *object_get(long index) { return index == 5 ? &weapon : NULL; }

#include "aiming_unit.inc"
#include "under_test.inc"
#include "zoom_aim.inc"

static int failures;
static void check(int yes, const char *name)
{
	printf("%s: %s\n", yes ? "ok" : "FAIL", name);
	if (!yes) failures++;
}

int main(void)
{
	player_present = parent_is_vehicle = 1;
	player.unit_index = 2;
	rider.object.parent_object_index = 3;
	rider.unit.parent_seat_index = 1;
	vehicle.unit.driver_object_index = 99;
	rider.unit.gunner_object_index = NONE;
	rider.unit.current_weapon_index = 0;
	rider.unit.aiming_vector.i = 0.98f;
	rider.unit.aiming_vector.j = 0.20f;
	rider.unit.aiming_vector.k = 0.0f;
	weapon.object.parent_object_index = 2;
	definition.unit.seats.count = 2;
	definition.unit.seats.address = seats;
	seats[1].flags = FLAG(_unit_seat_first_person_camera_bit);
	check(halo_seat_first_person_passenger(), "first-person rider uses the passenger head arc");
	seats[1].flags |= FLAG(_unit_seat_driver_bit);
	check(!halo_seat_first_person_passenger(), "driver-flagged seat keeps its own comfort rule");
	seats[1].flags = FLAG(_unit_seat_first_person_camera_bit) | FLAG(_unit_seat_gunner_bit);
	check(!halo_seat_first_person_passenger(), "gunner-flagged seat keeps its own comfort rule");
	seats[1].flags = FLAG(_unit_seat_first_person_camera_bit) | FLAG(_unit_seat_invisible_bit);
	check(!halo_seat_first_person_passenger(), "invisible seat keeps its own comfort rule");
	seats[1].flags = FLAG(_unit_seat_first_person_camera_bit);
	vehicle.unit.driver_object_index = 2;
	check(!halo_seat_first_person_passenger(), "current driver identity is excluded");
	vehicle.unit.driver_object_index = 99;
	seats[1].flags = FLAG(_unit_seat_third_person_camera_bit);
	check(!halo_seat_first_person_passenger(), "third-person seat is excluded");
	seats[1].flags = FLAG(_unit_seat_first_person_camera_bit);
	parent_is_vehicle = 0;
	check(!halo_seat_first_person_passenger(), "nonvehicle parent is excluded");
	parent_is_vehicle = 1;
	rider.object.parent_object_index = NONE;
	check(!halo_seat_first_person_passenger(), "seat exit clears the passenger predicate");
	rider.object.parent_object_index = 3;
	check(halo_seat_first_person_passenger(), "seat entry restores the passenger predicate");
	player.unit_index = NONE;
	check(!halo_seat_first_person_passenger(), "death clears the passenger predicate");
	{
		float ray[3] = {0};

		player.unit_index = 2;
		check(unit_get_aiming_unit_index(2) == 2 && halo_zoom_ordinary_aim_ray(0, ray) &&
			ray[0] == 0.98f && ray[1] == 0.20f,
			"visible passenger's own sniper uses the rider's ordinary aim");
		seats[1].flags |= FLAG(_unit_seat_gunner_bit);
		check(unit_get_aiming_unit_index(2) == 3 && !halo_zoom_ordinary_aim_ray(0, ray),
			"vehicle gunner takes the marker-forward branch, not rider aim");
		seats[1].flags = FLAG(_unit_seat_first_person_camera_bit);
		seats[1].flags |= FLAG(_unit_seat_driver_bit);
		check(!halo_zoom_ordinary_aim_ray(0, ray),
			"a driver seat does not adopt the rider's zoom ray");
		seats[1].flags = FLAG(_unit_seat_first_person_camera_bit);
		rider.unit.gunner_object_index = 8;
		check(!halo_zoom_ordinary_aim_ray(0, ray),
			"a unit serving a gunner keeps its marker-forward reticle");
		rider.unit.gunner_object_index = NONE;
		weapon.object.parent_object_index = 3;
		check(!halo_zoom_ordinary_aim_ray(0, ray),
			"a vehicle-owned weapon keeps its marker-forward reticle");
		weapon.object.parent_object_index = 2;
		rider.object.parent_object_index = NONE;
		check(halo_zoom_ordinary_aim_ray(0, ray), "an on-foot weapon still uses ordinary unit aim");
	}
	return failures ? 1 : 0;
}
