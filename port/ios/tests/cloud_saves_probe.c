/* The cloud save rules (port/runtime/cloud_saves.c) against a fake cloud: one in-memory store that
   several simulated devices share, each device a temporary folder with its own engine. Every row of
   the rule table, every conflict order and every failure the store can answer with has a test.

   The harness checks the first rule itself, whatever a test asserts: each time an engine is let
   change a device's files (local_write_begin to local_write_end), every file that changed or went
   away must hold bytes the cloud was given at some time, or nothing at all (all zeros). */
#include <assert.h>
#include <dirent.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include "cloud_saves.h"

#define SLOT "C7201D882DAE"
#define SLOT2 "0123456789AB"
#define STATE_SIZE 2048

/* starts at the real time, since files the engine writes carry it, and only moves forward */
static long long clock_now;
static int verbose;
static const char *test_name = "";

#define CHECK(condition) do { if (!(condition)) { \
	fprintf(stderr, "cloud_saves_probe: %s: line %d: %s\n", test_name, __LINE__, #condition); exit(1); } } while (0)

/* ---------- the fake cloud */

enum { OP_ACCOUNT, OP_LIST, OP_ZONE, OP_FETCH, OP_SAVE, OP_REMOVE, OP_COUNT };

struct fake_record
{
	struct cloud_record record;
	uint8_t *payload;
	size_t size;
};

static struct
{
	struct fake_record records[256];
	int count, zone, zone_deleted;
	char account[64];
	long version;
	enum cloud_status fail[OP_COUNT];
	int fail_count[OP_COUNT];
	int lose_reply;                    /* the next save is done but answers "retry" */
	int saves, fetches;
	void (*before_save)(void);         /* run once, before the next save is looked at */
	/* every file the cloud was ever given: name and hash */
	char seen[1024][CLOUD_NAME_SIZE + CLOUD_HASH_SIZE];
	int seen_count;
} cloud;

static void cloud_reset(void)
{
	int i;

	for (i = 0; i < cloud.count; i++)
		free(cloud.records[i].payload);
	memset(&cloud, 0, sizeof(cloud));
	strcpy(cloud.account, "person-one");
}

static int cloud_fails(int op, enum cloud_status *status)
{
	if (cloud.fail_count[op] <= 0)
		return 0;
	cloud.fail_count[op]--;
	*status = cloud.fail[op];
	return 1;
}

static void fail_next(int op, enum cloud_status status, int count)
{
	cloud.fail[op] = status;
	cloud.fail_count[op] = count;
}

static struct fake_record *cloud_find(const char *name)
{
	int i;

	for (i = 0; i < cloud.count; i++)
		if (!strcmp(cloud.records[i].record.name, name))
			return &cloud.records[i];
	return NULL;
}

/* the files of a payload, as the core packs them (PAYLOAD_MAGIC, raw size, deflate) */
static void cloud_remember(const uint8_t *payload, size_t size)
{
	uLongf raw_size = (uLongf)(payload[8] | payload[9] << 8 | payload[10] << 16 | (uLongf)payload[11] << 24);
	uint8_t *raw = malloc(raw_size);
	size_t used = 1;
	int i;

	assert(uncompress(raw, &raw_size, payload + 16, (uLong)(size - 16)) == Z_OK);
	for (i = 0; i < raw[0]; i++)
	{
		char name[CLOUD_NAME_SIZE], hash[CLOUD_HASH_SIZE];
		size_t name_length = raw[used++], file_size;

		memcpy(name, raw + used, name_length); name[name_length] = 0; used += name_length;
		file_size = raw[used] | raw[used + 1] << 8 | raw[used + 2] << 16 | (size_t)raw[used + 3] << 24; used += 8;
		cloud_sha256_hex(raw + used, file_size, hash); used += file_size;
		assert(cloud.seen_count < 1024);
		snprintf(cloud.seen[cloud.seen_count++], sizeof(cloud.seen[0]), "%s %s", name, hash);
	}
	free(raw);
}

static enum cloud_status fake_account(void *context, char *identifier, size_t size)
{
	enum cloud_status status;

	(void)context;
	if (cloud_fails(OP_ACCOUNT, &status))
		return status;
	if (!cloud.account[0])
		return CLOUD_NO_ACCOUNT;
	snprintf(identifier, size, "%s", cloud.account);
	return CLOUD_OK;
}

static enum cloud_status fake_list(void *context, struct cloud_record **records, int *count)
{
	enum cloud_status status;
	int i;

	(void)context;
	if (cloud_fails(OP_LIST, &status))
		return status;
	if (cloud.zone_deleted)
		return CLOUD_ZONE_DELETED;
	if (!cloud.zone)
		return CLOUD_NO_ZONE;
	*records = malloc(sizeof(**records) * (size_t)(cloud.count + 1));
	for (i = 0; i < cloud.count; i++)
		(*records)[i] = cloud.records[i].record;
	*count = cloud.count;
	return CLOUD_OK;
}

static enum cloud_status fake_create_zone(void *context)
{
	enum cloud_status status;

	(void)context;
	if (cloud_fails(OP_ZONE, &status))
		return status;
	cloud.zone = 1;
	cloud.zone_deleted = 0;
	return CLOUD_OK;
}

static enum cloud_status fake_fetch(void *context, const char *name, struct cloud_record *record, uint8_t **payload,
	size_t *size)
{
	struct fake_record *found = cloud_find(name);
	enum cloud_status status;

	(void)context;
	cloud.fetches++;
	if (cloud_fails(OP_FETCH, &status))
		return status;
	if (!found)
		return CLOUD_NOT_FOUND;
	*record = found->record;
	*payload = malloc(found->size);
	memcpy(*payload, found->payload, found->size);
	*size = found->size;
	return CLOUD_OK;
}

static enum cloud_status fake_save(void *context, const struct cloud_record *record, const uint8_t *payload, size_t size,
	const char *expected_tag, char *new_tag, size_t tag_size)
{
	struct fake_record *found;
	enum cloud_status status;

	(void)context;
	if (cloud.before_save)
	{
		void (*hook)(void) = cloud.before_save;

		cloud.before_save = NULL;
		hook();
	}
	if (cloud_fails(OP_SAVE, &status))
		return status;
	if (!cloud.zone)
		return CLOUD_NO_ZONE;
	found = cloud_find(record->name);
	if (expected_tag[0] ? (!found || strcmp(found->record.tag, expected_tag)) : found != NULL)
		return CLOUD_CHANGED;
	if (!found)
	{
		assert(payload && cloud.count < 256);
		found = &cloud.records[cloud.count++];
		memset(found, 0, sizeof(*found));
	}
	if (payload)
	{
		free(found->payload);
		found->payload = malloc(size);
		memcpy(found->payload, payload, size);
		found->size = size;
		cloud_remember(payload, size);
	}
	found->record = *record;
	snprintf(found->record.tag, sizeof(found->record.tag), "v%ld", ++cloud.version);
	snprintf(new_tag, tag_size, "%s", found->record.tag);
	cloud.saves++;
	if (cloud.lose_reply)
	{
		cloud.lose_reply = 0;
		return CLOUD_RETRY;
	}
	return CLOUD_OK;
}

static enum cloud_status fake_remove(void *context, const char *name)
{
	struct fake_record *found = cloud_find(name);
	enum cloud_status status;

	(void)context;
	if (cloud_fails(OP_REMOVE, &status))
		return status;
	if (!found)
		return CLOUD_NOT_FOUND;
	free(found->payload);
	*found = cloud.records[--cloud.count];
	return CLOUD_OK;
}

static enum cloud_status fake_delete_zone(void *context)
{
	int i;

	(void)context;
	for (i = 0; i < cloud.count; i++)
		free(cloud.records[i].payload);
	cloud.count = 0;
	cloud.zone = 0;
	return CLOUD_OK;
}

static const struct cloud_store fake_store = { NULL, fake_account, fake_list, fake_create_zone, fake_fetch, fake_save,
	fake_remove, fake_delete_zone };

static const char *cloud_hash(const char *name)
{
	struct fake_record *found = cloud_find(name);

	return found ? found->record.hash : "";
}

static int cloud_is_deleted(const char *name)
{
	char state[16] = "";
	struct fake_record *found = cloud_find(name);

	return found && cloud_meta_get(found->record.meta, "state", state, sizeof(state)) && !strcmp(state, "deleted");
}

static int cloud_copies(const char *unit)
{
	int i, count = 0;

	for (i = 0; i < cloud.count; i++)
		count += cloud.records[i].record.copy && !strcmp(cloud.records[i].record.unit, unit);
	return count;
}

/* ---------- a device */

struct snapshot_file
{
	char path[512], name[CLOUD_NAME_SIZE], hash[CLOUD_HASH_SIZE];
	int zeros;
};

struct device
{
	char label[32], root[256], save_root[300], state_path[300], image[32];
	int saved_game_version;
	struct cloud_engine *engine;
	int game_started, random;
	struct snapshot_file before[64];
	int before_count;
};

static uint8_t *file_bytes(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	uint8_t *data;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	*size = (size_t)ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc(*size + 1);
	assert(fread(data, 1, *size, file) == *size);
	fclose(file);
	return data;
}

static void snapshot(struct device *device, struct snapshot_file *files, int *count)
{
	char directory[400], slot_path[450];
	struct dirent *slot, *entry;
	DIR *slots, *dir;

	*count = 0;
	snprintf(directory, sizeof(directory), "%s/u/UDATA", device->save_root);
	slots = opendir(directory);
	while (slots && (slot = readdir(slots)) != NULL)
	{
		if (slot->d_name[0] == '.')
			continue;
		snprintf(slot_path, sizeof(slot_path), "%s/%s", directory, slot->d_name);
		dir = opendir(slot_path);
		while (dir && (entry = readdir(dir)) != NULL)
		{
			struct snapshot_file *file = &files[*count];
			size_t size, i;
			uint8_t *data;

			if (entry->d_name[0] == '.')
				continue;
			snprintf(file->path, sizeof(file->path), "%s/%s", slot_path, entry->d_name);
			snprintf(file->name, sizeof(file->name), "%s", entry->d_name);
			data = file_bytes(file->path, &size);
			if (!data)
				continue;
			cloud_sha256_hex(data, size, file->hash);
			file->zeros = 1;
			for (i = 0; i < size; i++)
				file->zeros &= !data[i];
			free(data);
			assert(++*count < 64);
		}
		if (dir)
			closedir(dir);
	}
	if (slots)
		closedir(slots);
}

static long long device_now(void *context) { (void)context; return clock_now; }

static void device_log(void *context, const char *line)
{
	if (verbose)
		fprintf(stderr, "    [%s] %s\n", ((struct device *)context)->label, line);
}

static int device_write_begin(void *context)
{
	struct device *device = context;

	if (device->game_started)
		return 0;
	snapshot(device, device->before, &device->before_count);
	return 1;
}

/* the first rule: what changed or went away was in the cloud, or held nothing */
static void device_write_end(void *context)
{
	struct device *device = context;
	static struct snapshot_file after[64];
	int after_count, i, j;

	snapshot(device, after, &after_count);
	for (i = 0; i < device->before_count; i++)
	{
		const struct snapshot_file *file = &device->before[i];
		char key[CLOUD_NAME_SIZE + CLOUD_HASH_SIZE];

		for (j = 0; j < after_count; j++)
			if (!strcmp(after[j].path, file->path) && !strcmp(after[j].hash, file->hash))
				break;
		if (j < after_count || file->zeros)
			continue;
		snprintf(key, sizeof(key), "%s %s", file->name, file->hash);
		for (j = 0; j < cloud.seen_count; j++)
			if (!strcmp(cloud.seen[j], key))
				break;
		if (j == cloud.seen_count)
		{
			fprintf(stderr, "cloud_saves_probe: %s: [%s] %s was replaced or removed and the cloud never held it\n", test_name,
				device->label, file->path);
			exit(1);
		}
	}
}

static void device_random(void *context, uint8_t *bytes, size_t size)
{
	struct device *device = context;
	size_t i;

	for (i = 0; i < size; i++)
		bytes[i] = (uint8_t)(device->label[0] + device->random++ * 7);
}

static void device_start(struct device *device)
{
	struct cloud_config config = { 0 };

	config.save_root = device->save_root;
	config.state_path = device->state_path;
	config.device = device->label;
	config.platform = "test";
	config.app = "1.0 (1)";
	config.image = device->image;
	config.state_size = STATE_SIZE;
	config.saved_game_version = device->saved_game_version;
	config.network_version = 26;
	config.context = device;
	config.now = device_now;
	config.log = device_log;
	config.local_write_begin = device_write_begin;
	config.local_write_end = device_write_end;
	config.random = device_random;
	device->engine = cloud_engine_create(&config, &fake_store);
	device->game_started = 0;
}

/* the app ends and starts again: what the engine knows is what its state file says */
static void device_restart(struct device *device)
{
	cloud_engine_destroy(device->engine);
	device_start(device);
}

static void device_make(struct device *device, const char *label)
{
	char template[] = "/tmp/cloud-saves-probe-XXXXXX";

	memset(device, 0, sizeof(*device));
	snprintf(device->label, sizeof(device->label), "%s", label);
	snprintf(device->image, sizeof(device->image), "image-one");
	device->saved_game_version = 1;
	assert(mkdtemp(template));
	snprintf(device->root, sizeof(device->root), "%s", template);
	snprintf(device->save_root, sizeof(device->save_root), "%s/save", device->root);
	snprintf(device->state_path, sizeof(device->state_path), "%s/cloud-saves.state", device->root);
	mkdir(device->save_root, 0755);
	device_start(device);
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *walk)
{
	(void)st; (void)type; (void)walk;
	return remove(path);
}

static void remove_tree(const char *path)
{
	nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void device_end(struct device *device)
{
	cloud_engine_destroy(device->engine);
	remove_tree(device->root);
}

/* tvOS empties Caches: the saves and the state file are gone */
static void device_purge(struct device *device)
{
	cloud_engine_destroy(device->engine);
	remove_tree(device->root);
	mkdir(device->root, 0755);
	mkdir(device->save_root, 0755);
	device_start(device);
}

static void slot_path(const struct device *device, const char *slot, const char *name, char *path, size_t size)
{
	snprintf(path, size, "%s/u/UDATA/%s%s%s", device->save_root, slot, name ? "/" : "", name ? name : "");
}

/* writes a file whose time is `age` seconds before now */
static void put_file(const struct device *device, const char *slot, const char *name, const uint8_t *data, size_t size, int age)
{
	char path[512];
	struct timeval times[2];
	FILE *file;

	snprintf(path, sizeof(path), "%s/u", device->save_root); mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/u/UDATA", device->save_root); mkdir(path, 0755);
	slot_path(device, slot, NULL, path, sizeof(path)); mkdir(path, 0755);
	slot_path(device, slot, name, path, sizeof(path));
	file = fopen(path, "wb");
	assert(file && fwrite(data, 1, size, file) == size);
	fclose(file);
	times[0].tv_sec = times[1].tv_sec = (time_t)(clock_now - age);
	times[0].tv_usec = times[1].tv_usec = 0;
	assert(utimes(path, times) == 0);
	slot_path(device, slot, NULL, path, sizeof(path));
	assert(utimes(path, times) == 0);
}

/* a profile as the game writes it: the name, and blam.sav with `levels` campaign levels finished */
static void put_profile(const struct device *device, const char *slot, const char *name, int levels, int variation, int age)
{
	uint8_t meta[64] = { 0 }, profile[512] = { 0 };
	size_t i;

	for (i = 0; name[i]; i++)
		meta[i * 2] = profile[i * 2] = (uint8_t)name[i];
	for (i = 0; i < (size_t)levels; i++)
		profile[0x1c + i] = 0x2;
	profile[0x28] = (uint8_t)variation;    /* a control setting */
	put_file(device, slot, "blam.sav", profile, sizeof(profile), age);
	put_file(device, slot, "SaveMeta.xbx", meta, strlen(name) * 2, age);
}

static uint32_t crc_of(uint32_t crc, const uint8_t *data, size_t size)
{
	size_t i;
	int bit;

	for (i = 0; i < size; i++)
	{
		crc ^= data[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
	}
	return crc;
}

/* a saved game the game would accept: its header, some state, and the checksum over all of it */
static void make_saved_game(uint8_t *data, size_t size, const char *map, uint32_t layout, int variation)
{
	uint32_t crc;

	memset(data, 0, size);
	memcpy(data, &layout, 4);
	snprintf((char *)data + 4, 256, "levels\\%s\\%s", map, map);
	snprintf((char *)data + 0x104, 32, "01.01.14.2342");
	data[0x124] = 1; data[0x126] = 1;
	data[0x200] = (uint8_t)variation; data[size - 1] = 0x5a;
	crc = crc_of(0xFFFFFFFFu, data, size);
	memcpy(data + 0x148, &crc, 4);
}

static void put_saved_game_of(const struct device *device, const char *slot, const char *map, uint32_t layout, size_t size,
	int variation, int age)
{
	uint8_t *data = malloc(size);

	make_saved_game(data, size, map, layout, variation);
	put_file(device, slot, "savegame.bin", data, size, age);
	free(data);
}

static void put_saved_game(const struct device *device, const char *slot, const char *map, int variation, int age)
{
	put_saved_game_of(device, slot, map, 0x11111111, STATE_SIZE, variation, age);
}

static int has_file(const struct device *device, const char *slot, const char *name)
{
	char path[512];

	slot_path(device, slot, name, path, sizeof(path));
	return access(path, F_OK) == 0;
}

static int same_file(const struct device *a, const struct device *b, const char *slot, const char *name)
{
	char path_a[512], path_b[512];
	size_t size_a = 0, size_b = 0;
	uint8_t *data_a, *data_b;
	int same;

	slot_path(a, slot, name, path_a, sizeof(path_a));
	slot_path(b, slot, name, path_b, sizeof(path_b));
	data_a = file_bytes(path_a, &size_a);
	data_b = file_bytes(path_b, &size_b);
	same = data_a && data_b && size_a == size_b && !memcmp(data_a, data_b, size_a);
	free(data_a); free(data_b);
	return same;
}

/* the first byte that tells two test files of one kind apart */
static int file_variation(const struct device *device, const char *slot, const char *name)
{
	char path[512];
	size_t size = 0;
	uint8_t *data;
	int variation;

	slot_path(device, slot, name, path, sizeof(path));
	data = file_bytes(path, &size);
	assert(data && size >= 512);
	variation = data[strcmp(name, "blam.sav") ? 0x200 : 0x28];
	free(data);
	return variation;
}

/* the game deletes a save: its files, its directory, and the host's note of it */
static void game_deletes(struct device *device, const char *slot)
{
	char path[512];

	slot_path(device, slot, NULL, path, sizeof(path));
	remove_tree(path);
	cloud_note_local_delete(device->state_path, slot);
}

static enum cloud_state launch(struct device *device)
{
	enum cloud_state state;

	device_restart(device);
	state = cloud_engine_sync(device->engine, CLOUD_PASS_LAUNCH);
	device->game_started = 1;
	clock_now += 10;                   /* what the launch wrote is no longer fresh */
	return state;
}

static enum cloud_state running(struct device *device)
{
	device->game_started = 1;
	return cloud_engine_sync(device->engine, CLOUD_PASS_RUNNING);
}

static int notices(struct device *device, struct cloud_notice *first)
{
	struct cloud_notice notice;
	int count = 0;

	while (cloud_engine_notice(device->engine, &notice))
	{
		if (!count && first)
			*first = notice;
		count++;
		cloud_engine_notice_done(device->engine);
	}
	return count;
}

static char saved_unit[CLOUD_RECORD_NAME_SIZE];

/* the one saved-game record of a slot (the tests that have two look them up themselves) */
static const char *saved_game_unit(const char *slot)
{
	char prefix[32];
	int i;

	snprintf(prefix, sizeof(prefix), "save-%s-", slot);
	saved_unit[0] = 0;
	for (i = 0; i < cloud.count; i++)
		if (!cloud.records[i].record.copy && !strncmp(cloud.records[i].record.name, prefix, strlen(prefix)))
			snprintf(saved_unit, sizeof(saved_unit), "%s", cloud.records[i].record.name);
	return saved_unit;
}

static int saved_game_records(const char *slot)
{
	char prefix[32];
	int i, count = 0;

	snprintf(prefix, sizeof(prefix), "save-%s-", slot);
	for (i = 0; i < cloud.count; i++)
		count += !cloud.records[i].record.copy && !strncmp(cloud.records[i].record.name, prefix, strlen(prefix));
	return count;
}

/* two devices in step on one profile with a saved game on a30 */
static void two_in_step(struct device *a, struct device *b)
{
	cloud_reset();
	device_make(a, "iPhone");
	device_make(b, "Vision Pro");
	put_profile(a, SLOT, "Chief", 2, 1, 100);
	put_saved_game(a, SLOT, "a30", 1, 100);
	CHECK(launch(a) == CLOUD_STATE_IDLE);
	CHECK(launch(b) == CLOUD_STATE_IDLE);
	CHECK(same_file(a, b, SLOT, "blam.sav") && same_file(a, b, SLOT, "savegame.bin") && same_file(a, b, SLOT, "SaveMeta.xbx"));
	clock_now += 1000;
}

/* ---------- the tests */

static void test_pieces(void)
{
	uint8_t game[STATE_SIZE], zeros[STATE_SIZE] = { 0 };
	char hash[CLOUD_HASH_SIZE], value[32], slot[CLOUD_SLOT_ID_SIZE];
	struct cloud_summary a = { 0 }, b = { 0 };

	cloud_sha256_hex((const uint8_t *)"abc", 3, hash);
	CHECK(!strcmp(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	cloud_sha256_hex((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hash);
	CHECK(!strcmp(hash, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	make_saved_game(game, sizeof(game), "b30", 7, 1);
	CHECK(cloud_checkpoint_check(game, sizeof(game)) == 1);
	game[900] ^= 1;                                   /* torn */
	CHECK(cloud_checkpoint_check(game, sizeof(game)) == -1);
	CHECK(cloud_checkpoint_check(zeros, sizeof(zeros)) == 0);
	zeros[1000] = 1;                                  /* a header zeroed and a body written */
	CHECK(cloud_checkpoint_check(zeros, sizeof(zeros)) == -1);
	CHECK(cloud_checkpoint_check(zeros, 16) == -1);
	CHECK(cloud_meta_get("a=1\nplayer=Chief\nb=2\n", "player", value, sizeof(value)) && !strcmp(value, "Chief"));
	CHECK(!cloud_meta_get("a=1\nplayers=2\n", "player", value, sizeof(value)));
	CHECK(cloud_slot_id_valid(SLOT) && !cloud_slot_id_valid("c7201d882dae") && !cloud_slot_id_valid("C7201D882DA") &&
		!cloud_slot_id_valid("C7201D882DAE0") && !cloud_slot_id_valid("../../../etc/"));
	CHECK(cloud_file_name_valid("blam.sav") && !cloud_file_name_valid("..") && !cloud_file_name_valid(".hidden") &&
		!cloud_file_name_valid("a/b") && !cloud_file_name_valid("") && !cloud_file_name_valid("a\\b"));
	CHECK(cloud_slot_of_path("/data/save", "/data/save/u/UDATA/" SLOT, slot) && !strcmp(slot, SLOT));
	CHECK(cloud_slot_of_path("/data/save", "/data/save/U/udata/c7201d882dae/", slot) && !strcmp(slot, SLOT));
	CHECK(!cloud_slot_of_path("/data/save", "/data/save/u/UDATA/" SLOT "/blam.sav", slot));
	CHECK(!cloud_slot_of_path("/data/save", "/data/save/z/saved", slot));
	CHECK(!cloud_slot_of_path("/data/save", "/data/saves/u/UDATA/" SLOT, slot));
	/* further along: levels finished for profiles, the level for saved games, then the newer */
	a.profile = b.profile = 1; a.levels = 3; b.levels = 2; b.modified = 99;
	CHECK(cloud_summary_compare(&a, &b) == 1 && cloud_summary_compare(&b, &a) == -1);
	b.levels = 3;
	CHECK(cloud_summary_compare(&a, &b) == -1);
	memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
	a.checkpoint = b.checkpoint = 1; a.checkpoint_level = 3; b.checkpoint_level = 1; b.modified = 99;
	CHECK(cloud_summary_compare(&a, &b) == 1);
	b.checkpoint_level = 3; b.modified = 0;
	CHECK(cloud_summary_compare(&a, &b) == 0);
	CHECK(!strcmp(cloud_level_title("levels\\b30\\b30"), "The Silent Cartographer") && !strcmp(cloud_level_title("ui"), "ui"));
}

static void test_first_upload_and_new_device(void)
{
	struct device a, b;

	cloud_reset();
	device_make(&a, "iPhone");
	put_profile(&a, SLOT, "Chief", 2, 1, 100);
	put_saved_game(&a, SLOT, "a30", 1, 100);
	CHECK(cloud_local_slot_count(a.save_root) == 1);
	CHECK(launch(&a) == CLOUD_STATE_IDLE);
	CHECK(cloud.zone && cloud.count == 2 && cloud_hash("slot-" SLOT)[0] && saved_game_records(SLOT) == 1);
	CHECK(cloud_engine_synced_slots(a.engine) == 1 && !notices(&a, NULL));
	/* in step: nothing more is sent */
	cloud.saves = 0;
	CHECK(running(&a) == CLOUD_STATE_IDLE && launch(&a) == CLOUD_STATE_IDLE && cloud.saves == 0);
	/* a new device takes it all before its game starts */
	device_make(&b, "Apple TV");
	CHECK(cloud_local_slot_count(b.save_root) == 0);
	CHECK(launch(&b) == CLOUD_STATE_IDLE);
	CHECK(same_file(&a, &b, SLOT, "blam.sav") && same_file(&a, &b, SLOT, "SaveMeta.xbx") && same_file(&a, &b, SLOT, "savegame.bin"));
	CHECK(cloud.saves == 0 && !notices(&b, NULL));
	device_end(&a); device_end(&b);
}

static void test_purge(void)
{
	struct device a, b;
	char path[512];

	two_in_step(&a, &b);
	/* everything gone, the state file too */
	device_purge(&b);
	cloud.saves = 0;
	CHECK(launch(&b) == CLOUD_STATE_IDLE);
	CHECK(same_file(&a, &b, SLOT, "blam.sav") && same_file(&a, &b, SLOT, "savegame.bin") && cloud.saves == 0);
	/* the saves gone and the state file left: not a deletion, since the game deleted nothing */
	slot_path(&b, SLOT, NULL, path, sizeof(path));
	remove_tree(path);
	CHECK(launch(&b) == CLOUD_STATE_IDLE);
	CHECK(same_file(&a, &b, SLOT, "blam.sav") && same_file(&a, &b, SLOT, "savegame.bin") && !cloud_is_deleted("slot-" SLOT));
	/* only the saved game gone */
	slot_path(&b, SLOT, "savegame.bin", path, sizeof(path));
	unlink(path);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "savegame.bin"));
	/* the state file gone and the saves left: in step again without sending anything */
	unlink(b.state_path);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && cloud.saves == 0 && !notices(&b, NULL));
	device_end(&a); device_end(&b);
}

static void test_one_device_moves_on(void)
{
	struct device a, b;

	two_in_step(&a, &b);
	put_profile(&a, SLOT, "Chief", 3, 1, 10);
	put_saved_game(&a, SLOT, "a50", 2, 10);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	CHECK(!cloud_copies("slot-" SLOT) && !cloud_copies(saved_game_unit(SLOT)));
	/* the other device's game runs: its files are not touched, and it says something waits */
	CHECK(running(&b) == CLOUD_STATE_PENDING);
	CHECK(!same_file(&a, &b, SLOT, "blam.sav") && !same_file(&a, &b, SLOT, "savegame.bin"));
	/* at its next launch it has them */
	CHECK(launch(&b) == CLOUD_STATE_IDLE);
	CHECK(same_file(&a, &b, SLOT, "blam.sav") && same_file(&a, &b, SLOT, "savegame.bin"));
	CHECK(!notices(&a, NULL) && !notices(&b, NULL) && cloud.count == 2);
	device_end(&a); device_end(&b);
}

static void test_game_started_stops_local_writes(void)
{
	struct device a, b;

	two_in_step(&a, &b);
	put_profile(&a, SLOT, "Chief", 3, 1, 10);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	/* the launch pass was still going when the wait ran out and the game started */
	device_restart(&b);
	b.game_started = 1;
	CHECK(cloud_engine_sync(b.engine, CLOUD_PASS_LAUNCH) == CLOUD_STATE_PENDING);
	CHECK(!same_file(&a, &b, SLOT, "blam.sav"));
	CHECK(launch(&b) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "blam.sav"));
	device_end(&a); device_end(&b);
}

static void test_conflict_local_wins(void)
{
	struct device a, b;
	struct cloud_notice notice;

	two_in_step(&a, &b);
	put_profile(&a, SLOT, "Chief", 3, 1, 50);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	/* the other device played offline and got further */
	put_profile(&b, SLOT, "Chief", 5, 2, 60);
	CHECK(running(&b) == CLOUD_STATE_IDLE);
	CHECK(cloud_copies("slot-" SLOT) == 1);
	CHECK(notices(&b, &notice) == 1 && notice.kind == CLOUD_NOTICE_CONFLICT && notice.kept_local == 1 &&
		notice.kept.levels == 5 && notice.aside.levels == 3 && !strcmp(notice.aside.device, "iPhone") &&
		!strcmp(notice.kept.player, "Chief") && !strcmp(notice.unit, "slot-" SLOT));
	/* the first takes the winner at its next launch; its own version is the copy */
	CHECK(launch(&a) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "blam.sav") && file_variation(&a, SLOT, "blam.sav") == 2);
	CHECK(!notices(&a, NULL) && cloud_copies("slot-" SLOT) == 1);
	device_end(&a); device_end(&b);
}

static void test_conflict_cloud_wins(void)
{
	struct device a, b;
	struct cloud_notice notice;

	two_in_step(&a, &b);
	put_profile(&a, SLOT, "Chief", 5, 1, 50);
	put_saved_game(&a, SLOT, "b30", 1, 50);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	put_profile(&b, SLOT, "Chief", 3, 2, 10);       /* newer, but not as far */
	put_saved_game(&b, SLOT, "a50", 2, 10);
	CHECK(launch(&b) == CLOUD_STATE_IDLE);
	CHECK(same_file(&a, &b, SLOT, "blam.sav") && same_file(&a, &b, SLOT, "savegame.bin"));
	CHECK(cloud_copies("slot-" SLOT) == 1 && cloud_copies(saved_game_unit(SLOT)) == 1);
	CHECK(notices(&b, &notice) == 2 && notice.kept_local == 0 && notice.kept.levels == 5 && notice.aside.levels == 3 &&
		!strcmp(notice.kept.device, "iPhone") && !strcmp(notice.aside.device, "Vision Pro"));
	/* settled: nothing more moves */
	cloud.saves = 0;
	CHECK(launch(&a) == CLOUD_STATE_IDLE && launch(&b) == CLOUD_STATE_IDLE && cloud.saves == 0);
	device_end(&a); device_end(&b);
}

static void test_conflict_found_while_running(void)
{
	struct device a, b;
	char winner[CLOUD_HASH_SIZE];

	two_in_step(&a, &b);
	put_profile(&a, SLOT, "Chief", 5, 1, 50);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	snprintf(winner, sizeof(winner), "%s", cloud_hash("slot-" SLOT));
	/* the loser's game is running: its version is set aside in the cloud, its files stay */
	put_profile(&b, SLOT, "Chief", 3, 2, 10);
	CHECK(running(&b) == CLOUD_STATE_PENDING);
	CHECK(cloud_copies("slot-" SLOT) == 1 && file_variation(&b, SLOT, "blam.sav") == 2 && !notices(&b, NULL));
	/* however often it looks, and across a restart, the loser never goes up over the winner */
	CHECK(running(&b) == CLOUD_STATE_PENDING);
	device_restart(&b);
	CHECK(running(&b) == CLOUD_STATE_PENDING);
	CHECK(!strcmp(cloud_hash("slot-" SLOT), winner) && cloud_copies("slot-" SLOT) == 1);
	/* the launch finishes it, with one notice */
	CHECK(launch(&b) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "blam.sav") && notices(&b, NULL) == 1);
	CHECK(!strcmp(cloud_hash("slot-" SLOT), winner) && cloud_copies("slot-" SLOT) == 1);
	device_end(&a); device_end(&b);
}

static void test_same_name_on_two_devices(void)
{
	struct device a, b, c;
	struct cloud_notice notice;

	cloud_reset();
	device_make(&a, "iPhone"); device_make(&b, "Vision Pro"); device_make(&c, "iPad");
	/* each made a profile called New001 before they ever synced */
	put_profile(&a, SLOT, "New001", 1, 1, 500);
	put_profile(&b, SLOT, "New001", 4, 2, 900);
	put_saved_game(&b, SLOT, "b40", 2, 900);
	put_profile(&c, SLOT, "New001", 1, 1, 300);      /* the same bytes as the first */
	CHECK(launch(&a) == CLOUD_STATE_IDLE);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && notices(&b, &notice) == 1 && notice.kept_local == 1);
	CHECK(file_variation(&b, SLOT, "blam.sav") == 2 && cloud_copies("slot-" SLOT) == 1);
	CHECK(launch(&a) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "blam.sav") && same_file(&a, &b, SLOT, "savegame.bin"));
	/* a third whose version is the one already set aside: settled without a second copy */
	CHECK(launch(&c) == CLOUD_STATE_IDLE && same_file(&c, &b, SLOT, "blam.sav") && cloud_copies("slot-" SLOT) == 1);
	device_end(&a); device_end(&b); device_end(&c);
}

static void test_identical_without_base(void)
{
	struct device a, b;

	cloud_reset();
	device_make(&a, "iPhone"); device_make(&b, "Vision Pro");
	put_profile(&a, SLOT, "Chief", 2, 1, 500);
	put_profile(&b, SLOT, "Chief", 2, 1, 300);
	CHECK(launch(&a) == CLOUD_STATE_IDLE);
	cloud.saves = 0;
	CHECK(launch(&b) == CLOUD_STATE_IDLE && cloud.saves == 0 && !notices(&b, NULL) && cloud_engine_synced_slots(b.engine) == 1);
	device_end(&a); device_end(&b);
}

/* equal on every count: both devices must pick the same side, whichever settles it */
static void test_tie_is_the_same_from_both_sides(void)
{
	char kept[2][CLOUD_HASH_SIZE];
	int order;

	for (order = 0; order < 2; order++)
	{
		struct device a, b;

		cloud_reset();
		device_make(&a, "iPhone"); device_make(&b, "Vision Pro");
		put_profile(&a, SLOT, "Chief", 2, 1, 100);
		put_profile(&b, SLOT, "Chief", 2, 2, 100);
		CHECK(launch(order ? &b : &a) == CLOUD_STATE_IDLE);
		CHECK(launch(order ? &a : &b) == CLOUD_STATE_IDLE);
		CHECK(launch(order ? &b : &a) == CLOUD_STATE_IDLE);
		CHECK(same_file(&a, &b, SLOT, "blam.sav") && cloud_copies("slot-" SLOT) == 1);
		snprintf(kept[order], CLOUD_HASH_SIZE, "%s", cloud_hash("slot-" SLOT));
		device_end(&a); device_end(&b);
	}
	CHECK(!strcmp(kept[0], kept[1]));
}

static void test_reply_lost(void)
{
	struct device a, b;

	two_in_step(&a, &b);
	/* the upload arrives, its reply does not, and the app is killed */
	put_profile(&a, SLOT, "Chief", 3, 1, 50);
	cloud.lose_reply = 1;
	CHECK(running(&a) == CLOUD_STATE_PENDING);
	device_restart(&a);
	/* played on before the next pass: still this device's own line, not a conflict with itself */
	put_profile(&a, SLOT, "Chief", 3, 7, 5);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	CHECK(!cloud_copies("slot-" SLOT) && !notices(&a, NULL));
	CHECK(launch(&b) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "blam.sav"));
	/* a first upload whose reply is lost */
	put_profile(&a, SLOT2, "Second", 1, 1, 50);
	cloud.lose_reply = 1;
	CHECK(running(&a) == CLOUD_STATE_PENDING);
	put_profile(&a, SLOT2, "Second", 1, 2, 5);
	CHECK(running(&a) == CLOUD_STATE_IDLE && !cloud_copies("slot-" SLOT2) && !notices(&a, NULL));
	device_end(&a); device_end(&b);
}

static void save_from_b_first(void);
static struct device *racing_device;

static void test_another_device_saves_first(void)
{
	struct device a, b;

	two_in_step(&a, &b);
	put_profile(&a, SLOT, "Chief", 3, 1, 50);
	put_profile(&b, SLOT, "Chief", 6, 2, 40);
	/* between this device's look at the cloud and its save, the other's save lands */
	racing_device = &b;
	cloud.before_save = save_from_b_first;
	CHECK(running(&a) == CLOUD_STATE_PENDING);       /* now a conflict, the cloud's further along, the game running */
	CHECK(file_variation(&a, SLOT, "blam.sav") == 1 && cloud_copies("slot-" SLOT) == 1);
	CHECK(launch(&a) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "blam.sav") && file_variation(&a, SLOT, "blam.sav") == 2);
	device_end(&a); device_end(&b);
}

static void save_from_b_first(void)
{
	CHECK(running(racing_device) == CLOUD_STATE_IDLE);
}

static void test_no_network_and_no_account(void)
{
	struct device a, b;

	two_in_step(&a, &b);
	put_profile(&a, SLOT, "Chief", 3, 1, 50);
	cloud.saves = 0;
	fail_next(OP_ACCOUNT, CLOUD_RETRY, 1);
	CHECK(running(&a) == CLOUD_STATE_PENDING);
	fail_next(OP_LIST, CLOUD_RETRY, 1);
	CHECK(launch(&a) == CLOUD_STATE_PENDING);
	fail_next(OP_SAVE, CLOUD_RETRY, 1);
	CHECK(running(&a) == CLOUD_STATE_PENDING && cloud.saves == 0);
	/* signed out: nothing happens, nothing is forgotten, and no question is asked */
	cloud.account[0] = 0;
	CHECK(launch(&a) == CLOUD_STATE_NO_ACCOUNT && running(&a) == CLOUD_STATE_NO_ACCOUNT && cloud.saves == 0);
	CHECK(file_variation(&a, SLOT, "blam.sav") == 1 && !notices(&a, NULL));
	/* signed in again: carries on from where it was, as an upload and not a conflict */
	strcpy(cloud.account, "person-one");
	CHECK(launch(&a) == CLOUD_STATE_IDLE && cloud.saves == 1 && !cloud_copies("slot-" SLOT));
	/* a download cut off leaves the device's files as they were */
	put_profile(&a, SLOT, "Chief", 4, 3, 20);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	fail_next(OP_FETCH, CLOUD_RETRY, 1);
	CHECK(launch(&b) == CLOUD_STATE_PENDING && file_variation(&b, SLOT, "blam.sav") == 1);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && file_variation(&b, SLOT, "blam.sav") == 3);
	device_end(&a); device_end(&b);
}

static void test_storage_full(void)
{
	struct device a, b;
	struct cloud_notice notice;

	two_in_step(&a, &b);
	put_profile(&a, SLOT, "Chief", 3, 1, 50);
	fail_next(OP_SAVE, CLOUD_QUOTA, 100);
	CHECK(running(&a) == CLOUD_STATE_QUOTA && cloud_engine_state(a.engine) == CLOUD_STATE_QUOTA);
	CHECK(notices(&a, &notice) == 1 && notice.kind == CLOUD_NOTICE_QUOTA);
	/* said once, across restarts, and the local save stays */
	CHECK(launch(&a) == CLOUD_STATE_QUOTA && running(&a) == CLOUD_STATE_QUOTA && !notices(&a, NULL));
	CHECK(file_variation(&a, SLOT, "blam.sav") == 1);
	/* downloads still work with the storage full */
	fail_next(OP_SAVE, CLOUD_OK, 0);
	put_profile(&b, SLOT2, "Other", 1, 1, 50);
	CHECK(running(&b) == CLOUD_STATE_IDLE);
	fail_next(OP_SAVE, CLOUD_QUOTA, 100);
	CHECK(launch(&a) == CLOUD_STATE_QUOTA && same_file(&a, &b, SLOT2, "blam.sav"));
	/* space again: the upload goes, and a later full storage is said again */
	fail_next(OP_SAVE, CLOUD_OK, 0);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	put_profile(&a, SLOT, "Chief", 4, 1, 20);
	fail_next(OP_SAVE, CLOUD_QUOTA, 100);
	CHECK(running(&a) == CLOUD_STATE_QUOTA && notices(&a, NULL) == 1);
	device_end(&a); device_end(&b);
}

static void test_account_switched(void)
{
	struct device a, b;
	struct cloud_notice notice;

	two_in_step(&a, &b);
	/* another person signs in on this device; their cloud has a profile of the same name */
	cloud_reset();
	strcpy(cloud.account, "person-two");
	cloud.zone = 1;
	put_profile(&b, SLOT, "Chief", 9, 5, 10);
	unlink(b.state_path);
	device_restart(&b);
	CHECK(launch(&b) == CLOUD_STATE_IDLE);            /* device b is person two's from the start */
	cloud.saves = cloud.fetches = 0;
	put_profile(&a, SLOT, "Chief", 3, 1, 5);
	CHECK(launch(&a) == CLOUD_STATE_ACCOUNT_CHANGED);
	CHECK(notices(&a, &notice) == 1 && notice.kind == CLOUD_NOTICE_ACCOUNT_CHANGED);
	/* nothing moves either way while the question stands, across restarts */
	CHECK(running(&a) == CLOUD_STATE_ACCOUNT_CHANGED && launch(&a) == CLOUD_STATE_ACCOUNT_CHANGED);
	CHECK(cloud.saves == 0 && cloud.fetches == 0 && file_variation(&a, SLOT, "blam.sav") == 1);
	/* "leave them on this device": still nothing */
	cloud_engine_answer(a.engine, 0);
	CHECK(running(&a) == CLOUD_STATE_ACCOUNT_CHANGED && cloud.saves == 0);
	/* "add them to this account": the usual rules, here a conflict the cloud's version wins */
	cloud_engine_answer(a.engine, 1);
	CHECK(launch(&a) == CLOUD_STATE_IDLE && file_variation(&a, SLOT, "blam.sav") == 5 && cloud_copies("slot-" SLOT) == 1);
	CHECK(notices(&a, &notice) == 1 && notice.kind == CLOUD_NOTICE_CONFLICT);
	/* back to the first account without having asked to: the question again, not a silent sync */
	strcpy(cloud.account, "person-one");
	CHECK(launch(&a) == CLOUD_STATE_ACCOUNT_CHANGED);
	device_end(&a); device_end(&b);
}

static void test_cloud_data_deleted(void)
{
	struct device a, b, c;
	struct cloud_notice notice;
	int reported;

	/* deleted in the system's settings, or by another device's "Delete saves from iCloud" */
	for (reported = 0; reported < 2; reported++)
	{
		two_in_step(&a, &b);
		if (reported)
		{
			fake_delete_zone(NULL);
			cloud.zone_deleted = 1;
		}
		else
			CHECK(cloud_engine_delete_cloud(b.engine) == CLOUD_OK && cloud.count == 0);
		cloud.saves = 0;
		CHECK(launch(&a) == CLOUD_STATE_CLOUD_DELETED);
		CHECK(notices(&a, &notice) == 1 && notice.kind == CLOUD_NOTICE_CLOUD_DELETED);
		/* the saves on the device stay, and nothing goes up again unasked */
		CHECK(has_file(&a, SLOT, "blam.sav") && has_file(&a, SLOT, "savegame.bin"));
		CHECK(running(&a) == CLOUD_STATE_CLOUD_DELETED && launch(&a) == CLOUD_STATE_CLOUD_DELETED && cloud.saves == 0 && !cloud.zone);
		cloud_engine_answer(a.engine, 1);
		CHECK(launch(&a) == CLOUD_STATE_IDLE && cloud.zone && cloud.count == 2);
		device_end(&a); device_end(&b);
	}
	/* a device with nothing to lose or send stays quiet and does not make the zone again */
	two_in_step(&a, &b);
	device_make(&c, "Apple TV");
	fake_delete_zone(NULL);
	cloud.zone_deleted = 1;
	CHECK(launch(&c) == CLOUD_STATE_IDLE && !cloud.zone && !notices(&c, NULL));
	device_end(&a); device_end(&b); device_end(&c);
}

static void test_newer_format(void)
{
	struct device a, b;
	struct cloud_notice notice;
	char hash[CLOUD_HASH_SIZE];

	two_in_step(&a, &b);
	/* a later build wrote the profile in a layout this one must not touch */
	cloud_find("slot-" SLOT)->record.min_format = CLOUD_FORMAT + 1;
	snprintf(cloud_find("slot-" SLOT)->record.hash, CLOUD_HASH_SIZE, "%064d", 0);
	snprintf(hash, sizeof(hash), "%s", cloud_hash("slot-" SLOT));
	put_profile(&a, SLOT, "Chief", 5, 3, 10);
	cloud.saves = 0;
	CHECK(launch(&a) == CLOUD_STATE_NEWER_FORMAT && running(&a) == CLOUD_STATE_NEWER_FORMAT);
	CHECK(file_variation(&a, SLOT, "blam.sav") == 3 && !strcmp(cloud_hash("slot-" SLOT), hash) && cloud.saves == 0);
	CHECK(notices(&a, &notice) == 1 && notice.kind == CLOUD_NOTICE_NEWER_FORMAT);
	/* and a kind of record it does not know at all is passed over */
	strcpy(cloud_find("slot-" SLOT)->record.unit, "trophy-" SLOT);
	strcpy(cloud_find("slot-" SLOT)->record.name, "trophy-" SLOT);
	cloud_engine_delete_cloud(b.engine);
	device_end(&a); device_end(&b);
}

static void test_delete_reaches_the_others(void)
{
	struct device a, b;
	char hash[CLOUD_HASH_SIZE];

	two_in_step(&a, &b);
	snprintf(hash, sizeof(hash), "%s", cloud_hash("slot-" SLOT));
	game_deletes(&a, SLOT);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	/* marked, with the payload the cloud already had; its saved game with it */
	CHECK(cloud_is_deleted("slot-" SLOT) && !strcmp(cloud_hash("slot-" SLOT), hash) && cloud_is_deleted(saved_game_unit(SLOT)));
	CHECK(launch(&a) == CLOUD_STATE_IDLE && !has_file(&a, SLOT, "blam.sav"));
	/* the other device has not played it since: gone there at its next launch, not while its game runs */
	CHECK(running(&b) == CLOUD_STATE_PENDING && has_file(&b, SLOT, "blam.sav"));
	CHECK(launch(&b) == CLOUD_STATE_IDLE && !has_file(&b, SLOT, "SaveMeta.xbx") && !has_file(&b, SLOT, "savegame.bin"));
	CHECK(launch(&b) == CLOUD_STATE_IDLE && cloud_is_deleted("slot-" SLOT));
	device_end(&a); device_end(&b);
}

static void test_delete_noted_before_a_crash(void)
{
	struct device a, b;

	two_in_step(&a, &b);
	/* the game deletes, and the app dies before the engine's next pass */
	game_deletes(&a, SLOT);
	device_restart(&a);
	CHECK(cloud_engine_sync(a.engine, CLOUD_PASS_LAUNCH) == CLOUD_STATE_IDLE);
	CHECK(cloud_is_deleted("slot-" SLOT) && !has_file(&a, SLOT, "blam.sav"));
	device_end(&a); device_end(&b);
}

static void test_delete_loses_to_newer_progress(void)
{
	struct device a, b;

	two_in_step(&a, &b);
	put_profile(&b, SLOT, "Chief", 6, 4, 50);
	CHECK(running(&b) == CLOUD_STATE_IDLE);
	/* deleted here without having seen what the other device did since */
	game_deletes(&a, SLOT);
	CHECK(running(&a) == CLOUD_STATE_PENDING);
	CHECK(!cloud_is_deleted("slot-" SLOT));
	CHECK(launch(&a) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "blam.sav"));
	CHECK(launch(&b) == CLOUD_STATE_IDLE && file_variation(&b, SLOT, "blam.sav") == 4);
	device_end(&a); device_end(&b);
}

static void test_deleted_but_played_elsewhere(void)
{
	struct device a, b;

	two_in_step(&a, &b);
	game_deletes(&a, SLOT);
	CHECK(running(&a) == CLOUD_STATE_IDLE && cloud_is_deleted("slot-" SLOT));
	/* the other device played on, offline, before it saw the deletion: its progress wins */
	put_profile(&b, SLOT, "Chief", 4, 6, 20);
	CHECK(launch(&b) == CLOUD_STATE_IDLE);
	CHECK(file_variation(&b, SLOT, "blam.sav") == 6 && !cloud_is_deleted("slot-" SLOT) && cloud_copies("slot-" SLOT) == 1);
	CHECK(launch(&a) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "blam.sav"));
	/* only its saved game changed: that too keeps the slot */
	game_deletes(&a, SLOT);
	CHECK(running(&a) == CLOUD_STATE_IDLE && cloud_is_deleted("slot-" SLOT));
	put_saved_game(&b, SLOT, "c10", 9, 20);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && has_file(&b, SLOT, "blam.sav") && file_variation(&b, SLOT, "savegame.bin") == 9);
	CHECK(!cloud_is_deleted("slot-" SLOT));
	device_end(&a); device_end(&b);
}

static void test_made_again_after_delete(void)
{
	struct device a, b;
	char path[512];
	uint8_t zeros[STATE_SIZE] = { 0 };

	two_in_step(&a, &b);
	game_deletes(&a, SLOT);
	/* a new profile of the same name, made before the next pass, with the game's empty saved game */
	put_profile(&a, SLOT, "Chief", 0, 8, 10);
	put_file(&a, SLOT, "savegame.bin", zeros, sizeof(zeros), 10);
	CHECK(launch(&a) == CLOUD_STATE_IDLE);
	CHECK(!cloud_is_deleted("slot-" SLOT) && cloud_copies("slot-" SLOT) == 1 && file_variation(&a, SLOT, "blam.sav") == 8);
	/* the new profile does not inherit the deleted one's saved game */
	CHECK(cloud_is_deleted(saved_game_unit(SLOT)));
	slot_path(&a, SLOT, "savegame.bin", path, sizeof(path));
	CHECK(launch(&a) == CLOUD_STATE_IDLE && cloud_checkpoint_check(zeros, sizeof(zeros)) == 0 && file_variation(&a, SLOT, "blam.sav") == 8);
	{
		size_t size;
		uint8_t *data = file_bytes(path, &size);

		CHECK(data && size == STATE_SIZE && cloud_checkpoint_check(data, size) == 0);
		free(data);
	}
	/* the other device, which had the old one unchanged, gets the new one, and the old saved game goes */
	CHECK(launch(&b) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "blam.sav") && !has_file(&b, SLOT, "savegame.bin"));
	CHECK(cloud_is_deleted(saved_game_unit(SLOT)) && !cloud_copies(saved_game_unit(SLOT)));
	CHECK(launch(&a) == CLOUD_STATE_IDLE && launch(&b) == CLOUD_STATE_IDLE && cloud_is_deleted(saved_game_unit(SLOT)));
	device_end(&a); device_end(&b);
}

static void test_restore_deleted(void)
{
	struct device a, b;

	two_in_step(&a, &b);
	game_deletes(&a, SLOT);
	CHECK(running(&a) == CLOUD_STATE_IDLE && launch(&b) == CLOUD_STATE_IDLE && !has_file(&b, SLOT, "blam.sav"));
	/* "Restore the last set-aside save" brings the profile back, and its saved game with the next pass */
	device_restart(&b);
	CHECK(cloud_engine_restore_copy(b.engine, NULL) == CLOUD_OK);
	CHECK(has_file(&b, SLOT, "blam.sav") && !cloud_is_deleted("slot-" SLOT) && !cloud_is_deleted(saved_game_unit(SLOT)));
	CHECK(cloud_engine_sync(b.engine, CLOUD_PASS_LAUNCH) == CLOUD_STATE_IDLE && has_file(&b, SLOT, "savegame.bin"));
	CHECK(launch(&a) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "blam.sav") && same_file(&a, &b, SLOT, "savegame.bin"));
	/* nothing set aside: says so */
	device_restart(&b);
	CHECK(cloud_engine_restore_copy(b.engine, NULL) == CLOUD_NOT_FOUND);
	device_end(&a); device_end(&b);
}

static void test_use_the_other_one(void)
{
	struct device a, b;

	two_in_step(&a, &b);
	put_profile(&a, SLOT, "Chief", 5, 1, 50);
	put_saved_game(&a, SLOT, "b30", 1, 50);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	put_profile(&b, SLOT, "Chief", 3, 2, 10);
	put_saved_game(&b, SLOT, "a50", 2, 10);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && file_variation(&b, SLOT, "blam.sav") == 1);
	/* the notice's second button, for the profile: this device's own version is current again */
	device_restart(&b);
	CHECK(cloud_engine_restore_copy(b.engine, "slot-" SLOT) == CLOUD_OK);
	CHECK(file_variation(&b, SLOT, "blam.sav") == 2 && cloud_copies("slot-" SLOT) == 1);
	CHECK(cloud_engine_sync(b.engine, CLOUD_PASS_LAUNCH) == CLOUD_STATE_IDLE);
	CHECK(launch(&a) == CLOUD_STATE_IDLE && file_variation(&a, SLOT, "blam.sav") == 2);
	/* and for the saved game */
	device_restart(&b);
	CHECK(cloud_engine_restore_copy(b.engine, saved_game_unit(SLOT)) == CLOUD_OK);
	CHECK(file_variation(&b, SLOT, "savegame.bin") == 2 && cloud_copies(saved_game_unit(SLOT)) == 1);
	CHECK(launch(&a) == CLOUD_STATE_IDLE && file_variation(&a, SLOT, "savegame.bin") == 2);
	/* pressed again: back to the other, nothing lost on the way */
	device_restart(&a);
	CHECK(cloud_engine_restore_copy(a.engine, "slot-" SLOT) == CLOUD_OK && file_variation(&a, SLOT, "blam.sav") == 1);
	CHECK(cloud_copies("slot-" SLOT) == 1);
	/* a restore that cannot reach the cloud changes nothing */
	device_restart(&a);
	fail_next(OP_FETCH, CLOUD_RETRY, 5);
	CHECK(cloud_engine_restore_copy(a.engine, "slot-" SLOT) != CLOUD_OK && file_variation(&a, SLOT, "blam.sav") == 1);
	fail_next(OP_FETCH, CLOUD_OK, 0);
	/* nor once the game has started */
	a.game_started = 1;
	CHECK(cloud_engine_restore_copy(a.engine, "slot-" SLOT) != CLOUD_OK && file_variation(&a, SLOT, "blam.sav") == 1);
	device_end(&a); device_end(&b);
}

static void test_unfinished_slots_stay_home(void)
{
	struct device a;
	uint8_t meta[10] = { 'C', 0, 'h', 0 }, profile[512] = { 1 }, odd[7] = { 1 };
	char path[512];

	cloud_reset();
	device_make(&a, "iPhone");
	/* a profile being made: the directory, then the name */
	put_file(&a, SLOT, "SaveMeta.xbx", meta, 4, 100);
	CHECK(launch(&a) == CLOUD_STATE_PENDING && cloud.count == 0);
	/* its profile file cut short by a crash */
	put_file(&a, SLOT, "blam.sav", profile, 0, 100);
	CHECK(launch(&a) == CLOUD_STATE_PENDING && cloud.count == 0);
	put_file(&a, SLOT, "blam.sav", profile, 300, 100);
	CHECK(launch(&a) == CLOUD_STATE_PENDING && cloud.count == 0);
	/* whole */
	put_file(&a, SLOT, "blam.sav", profile, 512, 100);
	CHECK(launch(&a) == CLOUD_STATE_IDLE && cloud.count == 1);
	/* being deleted file by file: the profile file is gone, the directory is not yet */
	slot_path(&a, SLOT, "blam.sav", path, sizeof(path));
	unlink(path);
	cloud.saves = 0;
	CHECK(launch(&a) == CLOUD_STATE_PENDING && cloud.saves == 0 && !cloud_is_deleted("slot-" SLOT));
	/* no name file: not a save at all, and the cloud's copy comes back as after a purge */
	slot_path(&a, SLOT, "SaveMeta.xbx", path, sizeof(path));
	unlink(path);
	CHECK(launch(&a) == CLOUD_STATE_IDLE && has_file(&a, SLOT, "blam.sav") && has_file(&a, SLOT, "SaveMeta.xbx"));
	/* a game type is a slot too */
	put_file(&a, SLOT2, "blam.lst", profile, 512, 100);
	put_file(&a, SLOT2, "SaveMeta.xbx", meta, 4, 100);
	CHECK(launch(&a) == CLOUD_STATE_IDLE && cloud_hash("slot-" SLOT2)[0]);
	/* a name file of an odd length, and both kinds of body at once, are not the game's */
	put_file(&a, SLOT2, "SaveMeta.xbx", odd, 7, 100);
	cloud.saves = 0;
	CHECK(launch(&a) == CLOUD_STATE_PENDING && cloud.saves == 0);
	put_file(&a, SLOT2, "SaveMeta.xbx", meta, 4, 100);
	put_file(&a, SLOT2, "blam.sav", profile, 512, 100);
	CHECK(launch(&a) == CLOUD_STATE_PENDING && cloud.saves == 0);
	device_end(&a);
}

static void test_foreign_files_are_left_alone(void)
{
	struct device a, b;
	char path[512];
	uint8_t profile[512] = { 3 };

	two_in_step(&a, &b);
	/* something in the slot the rules do not cover: the slot is not read, not sent, not replaced */
	slot_path(&b, SLOT, "notes", path, sizeof(path));
	mkdir(path, 0755);
	put_profile(&a, SLOT, "Chief", 5, 4, 50);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	cloud.saves = 0;
	CHECK(launch(&b) == CLOUD_STATE_IDLE && file_variation(&b, SLOT, "blam.sav") == 1 && cloud.saves == 0);
	rmdir(path);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && file_variation(&b, SLOT, "blam.sav") == 4);
	/* directories under UDATA that are not named as slots are not looked at */
	snprintf(path, sizeof(path), "%s/u/UDATA/c7201d882dae", b.save_root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/u/UDATA/README", b.save_root);
	mkdir(path, 0755);
	put_file(&b, "README", "blam.sav", profile, 512, 100);
	put_file(&b, "README", "SaveMeta.xbx", profile, 4, 100);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && cloud.count == 2);
	device_end(&a); device_end(&b);
}

static void test_files_must_be_still(void)
{
	struct device a;

	cloud_reset();
	device_make(&a, "iPhone");
	put_profile(&a, SLOT, "Chief", 2, 1, 0);         /* written this second */
	put_saved_game(&a, SLOT, "a30", 1, 0);
	CHECK(running(&a) == CLOUD_STATE_PENDING && cloud.count == 0);
	clock_now += 1;
	CHECK(running(&a) == CLOUD_STATE_PENDING && cloud.count == 0);
	clock_now += 2;
	CHECK(running(&a) == CLOUD_STATE_IDLE && cloud.count == 2);
	{
		/* what tells the host that a running pass has nothing to send */
		char before[CLOUD_HASH_SIZE], after[CLOUD_HASH_SIZE];

		cloud_local_signature(a.save_root, a.state_path, before);
		cloud_local_signature(a.save_root, a.state_path, after);
		CHECK(!strcmp(before, after));
		put_saved_game(&a, SLOT, "a30", 1, 1);       /* the same bytes, written again */
		cloud_local_signature(a.save_root, a.state_path, after);
		CHECK(strcmp(before, after));
		cloud_local_signature(a.save_root, a.state_path, before);
		cloud_note_local_delete(a.state_path, SLOT2);
		cloud_local_signature(a.save_root, a.state_path, after);
		CHECK(strcmp(before, after));
		clock_now += 5;
		CHECK(running(&a) == CLOUD_STATE_IDLE);
	}
	/* as the app leaves, the game has stopped writing: no wait */
	put_profile(&a, SLOT, "Chief", 3, 1, 0);
	a.game_started = 1;
	CHECK(cloud_engine_sync(a.engine, CLOUD_PASS_LEAVING) == CLOUD_STATE_IDLE && cloud.saves == 3);
	device_end(&a);
}

static void test_saved_game_that_is_not_one(void)
{
	struct device a, b;
	uint8_t torn[STATE_SIZE], zeros[STATE_SIZE] = { 0 };
	char saved[CLOUD_HASH_SIZE];

	two_in_step(&a, &b);
	snprintf(saved, sizeof(saved), "%s", cloud_hash(saved_game_unit(SLOT)));
	/* caught mid-write, or damaged: its profile still syncs, the cloud's saved game stays, the file stays */
	make_saved_game(torn, sizeof(torn), "b30", 0x11111111, 3);
	torn[700] ^= 0xff;
	put_file(&a, SLOT, "savegame.bin", torn, sizeof(torn), 50);
	put_profile(&a, SLOT, "Chief", 3, 2, 50);
	CHECK(launch(&a) == CLOUD_STATE_IDLE);
	CHECK(!strcmp(cloud_hash(saved_game_unit(SLOT)), saved) && saved_game_records(SLOT) == 1 && !cloud_copies(saved_game_unit(SLOT)));
	CHECK(file_variation(&a, SLOT, "savegame.bin") == 3);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && file_variation(&b, SLOT, "blam.sav") == 2 && file_variation(&b, SLOT, "savegame.bin") == 1);
	/* emptied (the game makes it again as zeros): never an upload; the cloud's comes back */
	put_file(&a, SLOT, "savegame.bin", zeros, sizeof(zeros), 50);
	CHECK(running(&a) == CLOUD_STATE_PENDING && !strcmp(cloud_hash(saved_game_unit(SLOT)), saved));
	CHECK(launch(&a) == CLOUD_STATE_IDLE && same_file(&a, &b, SLOT, "savegame.bin"));
	device_end(&a); device_end(&b);
}

static void test_layouts_keep_their_own_saved_games(void)
{
	struct device a, b, c;
	char first[CLOUD_RECORD_NAME_SIZE];

	two_in_step(&a, &b);
	snprintf(first, sizeof(first), "%s", saved_game_unit(SLOT));
	/* the second device runs another build of the game: it cannot load the first's saved game, and
	   saving there makes a record of its own instead of replacing the first's */
	put_saved_game_of(&b, SLOT, "c10", 0x22222222, STATE_SIZE, 5, 20);
	CHECK(running(&b) == CLOUD_STATE_IDLE);
	CHECK(saved_game_records(SLOT) == 2 && cloud_find(first) && !cloud_copies(first) && !notices(&b, NULL));
	CHECK(launch(&a) == CLOUD_STATE_IDLE && file_variation(&a, SLOT, "savegame.bin") == 1);
	put_saved_game(&a, SLOT, "a50", 6, 10);
	CHECK(running(&a) == CLOUD_STATE_IDLE && saved_game_records(SLOT) == 2);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && file_variation(&b, SLOT, "savegame.bin") == 5);
	/* a new device of the second build takes the one its own build wrote, though the other is newer */
	device_make(&c, "Apple TV");
	snprintf(b.image, sizeof(b.image), "image-two");
	snprintf(c.image, sizeof(c.image), "image-two");
	put_saved_game_of(&b, SLOT, "c20", 0x22222222, STATE_SIZE, 7, 500);
	CHECK(launch(&b) == CLOUD_STATE_IDLE);
	CHECK(launch(&c) == CLOUD_STATE_IDLE && file_variation(&c, SLOT, "savegame.bin") == 7);
	/* after a purge it takes the layout it last had */
	put_saved_game(&a, SLOT, "d40", 8, 5);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	device_purge(&c);
	snprintf(c.image, sizeof(c.image), "image-three");
	CHECK(launch(&c) == CLOUD_STATE_IDLE && file_variation(&c, SLOT, "savegame.bin") == 8);   /* nothing remembered: the newest */
	/* a build whose saved games mean something else in the same layout (the version raised by hand),
	   and one whose map file is another: records of their own too */
	b.saved_game_version = 2;
	device_restart(&b);
	CHECK(cloud_engine_sync(b.engine, CLOUD_PASS_LAUNCH) == CLOUD_STATE_IDLE && saved_game_records(SLOT) == 3);
	{
		uint8_t game[STATE_SIZE];
		uint32_t crc;

		make_saved_game(game, sizeof(game), "a30", 0x11111111, 3);
		game[0x128] = 0x77;                             /* the map file's checksum */
		memset(game + 0x148, 0, 4);
		crc = crc_of(0xFFFFFFFFu, game, sizeof(game));
		memcpy(game + 0x148, &crc, 4);
		put_file(&a, SLOT, "savegame.bin", game, sizeof(game), 5);
		CHECK(launch(&a) == CLOUD_STATE_IDLE && saved_game_records(SLOT) == 4 && !cloud_copies(saved_game_unit(SLOT)));
	}
	device_end(&a); device_end(&b); device_end(&c);
}

static void test_saved_game_of_another_size(void)
{
	struct device a, b;

	cloud_reset();
	device_make(&a, "iPhone"); device_make(&b, "Vision Pro");
	/* a build whose saved games are another size: the game would overwrite the start of such a file */
	put_profile(&a, SLOT, "Chief", 2, 1, 100);
	put_saved_game_of(&a, SLOT, "a30", 0x33333333, STATE_SIZE * 2, 1, 100);
	CHECK(launch(&a) == CLOUD_STATE_IDLE && saved_game_records(SLOT) == 1);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && has_file(&b, SLOT, "blam.sav") && !has_file(&b, SLOT, "savegame.bin"));
	/* nor by asking for it */
	put_saved_game(&b, SLOT, "a50", 2, 10);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && saved_game_records(SLOT) == 2);
	device_restart(&b);
	CHECK(cloud_engine_restore_copy(b.engine, NULL) == CLOUD_NOT_FOUND && file_variation(&b, SLOT, "savegame.bin") == 2);
	device_end(&a); device_end(&b);
}

/* a payload as the core packs it, from files the core would refuse to make */
static size_t pack(uint8_t *out, int count, const char *const *names, const uint8_t *const *data, const size_t *sizes)
{
	static uint8_t raw[8192];
	size_t used = 0;
	uLongf packed = 8192;
	int i;

	raw[used++] = (uint8_t)count;
	for (i = 0; i < count; i++)
	{
		raw[used++] = (uint8_t)strlen(names[i]);
		memcpy(raw + used, names[i], strlen(names[i])); used += strlen(names[i]);
		memset(raw + used, 0, 8); raw[used] = (uint8_t)sizes[i]; raw[used + 1] = (uint8_t)(sizes[i] >> 8); used += 8;
		memcpy(raw + used, data[i], sizes[i]); used += sizes[i];
	}
	memcpy(out, "VISRSAV1", 8);
	memset(out + 8, 0, 8); out[8] = (uint8_t)used; out[9] = (uint8_t)(used >> 8);
	assert(compress(out + 16, &packed, raw, (uLong)used) == Z_OK);
	return 16 + packed;
}

static void test_damaged_and_hostile_payloads(void)
{
	struct device a, b;
	struct fake_record *record;
	static uint8_t payload[8192], profile[512] = { 9 }, meta[4] = { 'X', 0 };
	const char *names[3];
	const uint8_t *data[3] = { profile, meta, profile };
	size_t sizes[3] = { 512, 2, 512 }, size;
	char outside[512];
	int attempt;

	two_in_step(&a, &b);
	put_profile(&a, SLOT, "Chief", 5, 4, 50);
	CHECK(running(&a) == CLOUD_STATE_IDLE);
	record = cloud_find("slot-" SLOT);
	/* bytes flipped in the cloud, a cut payload, garbage: the device's files stay */
	record->payload[record->size - 3] ^= 0x55;
	CHECK(launch(&b) == CLOUD_STATE_PENDING && file_variation(&b, SLOT, "blam.sav") == 1);
	record->size = 20;
	CHECK(launch(&b) == CLOUD_STATE_PENDING && file_variation(&b, SLOT, "blam.sav") == 1);
	memset(record->payload, 0xee, record->size);
	CHECK(launch(&b) == CLOUD_STATE_PENDING && file_variation(&b, SLOT, "blam.sav") == 1);
	/* well-formed payloads whose names reach outside the slot, repeat, or put a saved game in a profile */
	snprintf(outside, sizeof(outside), "%s/u/UDATA/escaped", b.save_root);
	for (attempt = 0; attempt < 5; attempt++)
	{
		int count = 2;

		names[0] = attempt == 0 ? "../escaped" : attempt == 1 ? "/tmp/escaped" : attempt == 2 ? ".." : "blam.sav";
		names[1] = "SaveMeta.xbx";
		if (attempt == 3) { names[2] = "BLAM.SAV"; count = 3; }
		if (attempt == 4) { names[2] = "savegame.bin"; count = 3; }
		size = pack(payload, count, names, data, sizes);
		free(record->payload);
		record->payload = malloc(size);
		memcpy(record->payload, payload, size);
		record->size = size;
		/* (the hash the record claims cannot match a payload the core would not make; a matching one
		   is made by hashing as the core does, which a hostile writer could) */
		CHECK(launch(&b) == CLOUD_STATE_PENDING && file_variation(&b, SLOT, "blam.sav") == 1);
		CHECK(access(outside, F_OK) != 0 && access("/tmp/escaped", F_OK) != 0);
	}
	device_end(&a); device_end(&b);
}

static void test_copies_are_pruned(void)
{
	struct device a, b;
	int round;

	two_in_step(&a, &b);
	for (round = 0; round < CLOUD_MAX_COPIES + 3; round++)
	{
		/* each round both play, and the second gets further */
		put_profile(&a, SLOT, "Chief", 1, 10 + round, 50);
		CHECK(running(&a) == CLOUD_STATE_IDLE);
		put_profile(&b, SLOT, "Chief", 2, 100 + round, 40);
		CHECK(running(&b) == CLOUD_STATE_IDLE);
		CHECK(launch(&a) == CLOUD_STATE_IDLE && notices(&b, NULL) == 1);
		clock_now += 100;
	}
	CHECK(cloud_copies("slot-" SLOT) == CLOUD_MAX_COPIES);
	/* the newest is among those kept */
	device_restart(&a);
	CHECK(cloud_engine_restore_copy(a.engine, "slot-" SLOT) == CLOUD_OK &&
		file_variation(&a, SLOT, "blam.sav") == 10 + CLOUD_MAX_COPIES + 2);
	device_end(&a); device_end(&b);
}

static void test_state_file_that_is_not_one(void)
{
	struct device a, b;
	FILE *file;

	two_in_step(&a, &b);
	file = fopen(b.state_path, "w");
	fputs("visr-cloud-saves\t1\nbase\tslot-../../x\tzz\t\t\nbase\nnotice\t99\t-\t0\t\t\ndeleted\tnope\t-\nkey\t" SLOT "\tZZ\n\t\t\t\t\t\t\t\t\t\n", file);
	fclose(file);
	cloud.saves = 0;
	CHECK(launch(&b) == CLOUD_STATE_IDLE && cloud.saves == 0 && !notices(&b, NULL) && same_file(&a, &b, SLOT, "blam.sav"));
	file = fopen(b.state_path, "w");
	fputs("something else entirely\n", file);
	fclose(file);
	CHECK(launch(&b) == CLOUD_STATE_IDLE && cloud.saves == 0 && same_file(&a, &b, SLOT, "blam.sav"));
	device_end(&a); device_end(&b);
}

static void test_store_fails_for_good(void)
{
	struct device a;

	cloud_reset();
	device_make(&a, "iPhone");
	put_profile(&a, SLOT, "Chief", 2, 1, 100);
	/* the wrong container or no entitlement: the host stops for the run on this state */
	fail_next(OP_ACCOUNT, CLOUD_FAILED, 1);
	CHECK(launch(&a) == CLOUD_STATE_FAILED);
	fail_next(OP_LIST, CLOUD_FAILED, 1);
	CHECK(launch(&a) == CLOUD_STATE_FAILED);
	fail_next(OP_SAVE, CLOUD_FAILED, 1);
	CHECK(launch(&a) == CLOUD_STATE_FAILED && has_file(&a, SLOT, "blam.sav"));
	CHECK(launch(&a) == CLOUD_STATE_IDLE);
	device_end(&a);
}

int main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } tests[] = {
		{ "pieces", test_pieces },
		{ "first upload and a new device", test_first_upload_and_new_device },
		{ "purge", test_purge },
		{ "one device moves on", test_one_device_moves_on },
		{ "the game starting stops local writes", test_game_started_stops_local_writes },
		{ "conflict, this device further along", test_conflict_local_wins },
		{ "conflict, the cloud further along", test_conflict_cloud_wins },
		{ "conflict found while the game runs", test_conflict_found_while_running },
		{ "the same name on two devices", test_same_name_on_two_devices },
		{ "identical without a base", test_identical_without_base },
		{ "a tie is the same from both sides", test_tie_is_the_same_from_both_sides },
		{ "an upload's reply lost", test_reply_lost },
		{ "another device saves first", test_another_device_saves_first },
		{ "no network and no account", test_no_network_and_no_account },
		{ "storage full", test_storage_full },
		{ "account switched", test_account_switched },
		{ "cloud data deleted", test_cloud_data_deleted },
		{ "a newer format", test_newer_format },
		{ "a delete reaches the others", test_delete_reaches_the_others },
		{ "a delete noted before a crash", test_delete_noted_before_a_crash },
		{ "a delete loses to newer progress", test_delete_loses_to_newer_progress },
		{ "deleted but played elsewhere", test_deleted_but_played_elsewhere },
		{ "made again after a delete", test_made_again_after_delete },
		{ "restore a deleted profile", test_restore_deleted },
		{ "use the other one", test_use_the_other_one },
		{ "unfinished slots stay home", test_unfinished_slots_stay_home },
		{ "foreign files are left alone", test_foreign_files_are_left_alone },
		{ "files must be still", test_files_must_be_still },
		{ "a saved game that is not one", test_saved_game_that_is_not_one },
		{ "layouts keep their own saved games", test_layouts_keep_their_own_saved_games },
		{ "a saved game of another size", test_saved_game_of_another_size },
		{ "damaged and hostile payloads", test_damaged_and_hostile_payloads },
		{ "copies are pruned", test_copies_are_pruned },
		{ "a state file that is not one", test_state_file_that_is_not_one },
		{ "the store fails for good", test_store_fails_for_good },
	};
	size_t i;

	clock_now = (long long)time(NULL) + 5;
	verbose = argc > 1 && !strcmp(argv[1], "--verbose");
	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
	{
		if (argc > 2 && !strstr(tests[i].name, argv[2]))
			continue;
		test_name = tests[i].name;
		if (verbose)
			fprintf(stderr, "%s\n", test_name);
		tests[i].run();
	}
	cloud_reset();
	printf("cloud_saves_probe: ok (%zu tests)\n", sizeof(tests) / sizeof(tests[0]));
	return 0;
}
