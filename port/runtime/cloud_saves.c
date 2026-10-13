/* The cloud save rules (cloud_saves.h). */
#include "cloud_saves.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define CHECKPOINT_FILE "savegame.bin"
#define META_FILE "SaveMeta.xbx"
#define PROFILE_FILE "blam.sav"
#define VARIANT_FILE "blam.lst"
#define SAVE_FILE_SIZE 512
#define TEMP_PREFIX ".cloud-"
#define PAYLOAD_MAGIC "VISRSAV1"
#define MAX_BASES (CLOUD_MAX_SLOTS * 3)
#define MAX_NOTICES 16
#define STATE_HEADER "visr-cloud-saves\t1"

/* ---------- SHA-256 */

struct sha256
{
	uint32_t state[8];
	uint64_t length;
	uint8_t block[64];
	size_t used;
};

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
	0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
	0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
	0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
	0xc67178f2 };

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(struct sha256 *sha, const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (; i < 64; i++)
	{
		uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = sha->state[0]; b = sha->state[1]; c = sha->state[2]; d = sha->state[3];
	e = sha->state[4]; f = sha->state[5]; g = sha->state[6]; h = sha->state[7];
	for (i = 0; i < 64; i++)
	{
		uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
		uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));

		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	sha->state[0] += a; sha->state[1] += b; sha->state[2] += c; sha->state[3] += d;
	sha->state[4] += e; sha->state[5] += f; sha->state[6] += g; sha->state[7] += h;
}

static void sha256_begin(struct sha256 *sha)
{
	static const uint32_t initial[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
		0x1f83d9ab, 0x5be0cd19 };

	memcpy(sha->state, initial, sizeof(initial));
	sha->length = 0;
	sha->used = 0;
}

static void sha256_add(struct sha256 *sha, const void *data, size_t size)
{
	const uint8_t *p = data;

	sha->length += size;
	if (sha->used)
	{
		size_t take = 64 - sha->used < size ? 64 - sha->used : size;

		memcpy(sha->block + sha->used, p, take);
		sha->used += take; p += take; size -= take;
		if (sha->used < 64)
			return;
		sha256_block(sha, sha->block);
		sha->used = 0;
	}
	for (; size >= 64; p += 64, size -= 64)
		sha256_block(sha, p);
	if (size)
		memcpy(sha->block, p, size);
	sha->used = size;
}

static void sha256_end(struct sha256 *sha, uint8_t digest[32])
{
	uint64_t bits = sha->length * 8;
	uint8_t pad[72] = { 0x80 }, tail[8];
	size_t pad_size = (sha->used < 56 ? 56 : 120) - sha->used;
	int i;

	for (i = 0; i < 8; i++)
		tail[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha256_add(sha, pad, pad_size);
	sha256_add(sha, tail, 8);
	for (i = 0; i < 8; i++)
	{
		digest[i * 4] = (uint8_t)(sha->state[i] >> 24); digest[i * 4 + 1] = (uint8_t)(sha->state[i] >> 16);
		digest[i * 4 + 2] = (uint8_t)(sha->state[i] >> 8); digest[i * 4 + 3] = (uint8_t)sha->state[i];
	}
}

static void hex_of(const uint8_t *bytes, size_t count, char *hex)
{
	size_t i;

	for (i = 0; i < count; i++)
		snprintf(hex + i * 2, 3, "%02x", bytes[i]);
}

void cloud_sha256_hex(const uint8_t *data, size_t size, char hex[CLOUD_HASH_SIZE])
{
	struct sha256 sha;
	uint8_t digest[32];

	sha256_begin(&sha);
	sha256_add(&sha, data, size);
	sha256_end(&sha, digest);
	hex_of(digest, 32, hex);
}

/* ---------- small helpers */

static void copy_text(char *out, size_t size, const char *text)
{
	snprintf(out, size, "%s", text ? text : "");
}

int cloud_slot_id_valid(const char *id)
{
	int i;

	for (i = 0; i < 12; i++)
		if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'A' && id[i] <= 'F')))
			return 0;
	return id[12] == 0;
}

static int key_valid(const char *key)
{
	int i;

	for (i = 0; i < 12; i++)
		if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f')))
			return 0;
	return key[12] == 0;
}

int cloud_file_name_valid(const char *name)
{
	size_t length = strlen(name), i;

	if (!length || length >= CLOUD_NAME_SIZE || name[0] == '.')
		return 0;
	for (i = 0; i < length; i++)
		if (!isalnum((unsigned char)name[i]) && name[i] != '.' && name[i] != '_' && name[i] != '-')
			return 0;
	return 1;
}

/* slot-<id> or save-<id>-<key>: writes the slot (and the key, "" for a profile); 0 when it is neither */
static int unit_parse(const char *unit, char *slot, char *key)
{
	if (strlen(unit) == 17 && !strncmp(unit, "slot-", 5))
	{
		memcpy(slot, unit + 5, 13);
		key[0] = 0;
		return cloud_slot_id_valid(slot);
	}
	if (strlen(unit) == 30 && !strncmp(unit, "save-", 5) && unit[17] == '-')
	{
		memcpy(slot, unit + 5, 12); slot[12] = 0;
		memcpy(key, unit + 18, 13);
		return cloud_slot_id_valid(slot) && key_valid(key);
	}
	return 0;
}

int cloud_meta_get(const char *meta, const char *key, char *value, size_t size)
{
	size_t key_length = strlen(key);
	const char *line = meta;

	if (size)
		value[0] = 0;
	while (line && *line)
	{
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);

		if (length > key_length && !strncmp(line, key, key_length) && line[key_length] == '=')
		{
			size_t take = length - key_length - 1;

			if (take >= size)
				take = size ? size - 1 : 0;
			if (size)
			{
				memcpy(value, line + key_length + 1, take);
				value[take] = 0;
			}
			return 1;
		}
		line = end ? end + 1 : NULL;
	}
	return 0;
}

/* appends key=value, the value without line breaks */
static void meta_add(char *meta, const char *key, const char *value)
{
	size_t used = strlen(meta);
	size_t i;

	if (used + strlen(key) + strlen(value) + 3 > CLOUD_META_SIZE)
		return;
	used += (size_t)snprintf(meta + used, CLOUD_META_SIZE - used, "%s=", key);
	for (i = 0; value[i]; i++)
		meta[used++] = (unsigned char)value[i] < 0x20 ? ' ' : value[i];
	meta[used++] = '\n';
	meta[used] = 0;
}

static void meta_add_number(char *meta, const char *key, long long value)
{
	char text[32];

	snprintf(text, sizeof(text), "%lld", value);
	meta_add(meta, key, text);
}

/* replaces (or adds) one key */
static void meta_set(char *meta, const char *key, const char *value)
{
	char out[CLOUD_META_SIZE] = "";
	size_t key_length = strlen(key);
	const char *line = meta;

	while (line && *line)
	{
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);

		if (!(length > key_length && !strncmp(line, key, key_length) && line[key_length] == '=') &&
			strlen(out) + length + 2 <= CLOUD_META_SIZE)
		{
			strncat(out, line, length);
			strcat(out, "\n");
		}
		line = end ? end + 1 : NULL;
	}
	meta_add(out, key, value);
	memcpy(meta, out, CLOUD_META_SIZE);
}

static long long meta_number(const char *meta, const char *key)
{
	char text[32];

	return cloud_meta_get(meta, key, text, sizeof(text)) ? strtoll(text, NULL, 10) : 0;
}

/* ---------- the game's files */

static const char *const campaign_maps[10] = { "a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40" };
static const char *const campaign_titles[10] = { "The Pillar of Autumn", "Halo", "The Truth and Reconciliation",
	"The Silent Cartographer", "Assault on the Control Room", "343 Guilty Spark", "The Library", "Two Betrayals",
	"Keyes", "The Maw" };

static int campaign_level(const char *map)
{
	const char *name = strrchr(map, '\\');
	int i;

	name = name ? name + 1 : map;
	for (i = 0; i < 10; i++)
		if (!strcmp(name, campaign_maps[i]))
			return i;
	return -1;
}

const char *cloud_level_title(const char *map)
{
	int level = campaign_level(map);

	return level < 0 ? map : campaign_titles[level];
}

/* source/memory/crc.c */
static uint32_t game_crc(uint32_t crc, const uint8_t *data, size_t size)
{
	static uint32_t table[256];
	size_t i;

	if (!table[1])
	{
		uint32_t n;
		int bit;

		for (n = 0; n < 256; n++)
		{
			uint32_t value = n;

			for (bit = 0; bit < 8; bit++)
				value = (value & 1) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
			table[n] = value;
		}
	}
	for (i = 0; i < size; i++)
		crc = (crc >> 8) ^ table[(data[i] ^ crc) & 0xff];
	return crc;
}

/* struct game_state_header (source/saved games/game_state.h) */
enum
{
	HEADER_LAYOUT = 0,
	HEADER_MAP = 4,
	HEADER_MAP_SIZE = 256,
	HEADER_BUILD = 0x104,
	HEADER_BUILD_SIZE = 32,
	HEADER_DIFFICULTY = 0x126,
	HEADER_MAP_CHECKSUM = 0x128,
	HEADER_CHECKSUM = 0x148,
	HEADER_SIZE = 0x14c,
};

static uint32_t read_u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int cloud_checkpoint_check(const uint8_t *data, size_t size)
{
	static const uint8_t zero[4] = { 0 };
	uint32_t crc;
	size_t i;

	if (size < HEADER_SIZE)
		return -1;
	/* the checksum is over the file with its own field zero */
	crc = game_crc(0xFFFFFFFFu, data, HEADER_CHECKSUM);
	crc = game_crc(crc, zero, 4);
	crc = game_crc(crc, data + HEADER_CHECKSUM + 4, size - HEADER_CHECKSUM - 4);
	if (crc == read_u32(data + HEADER_CHECKSUM) && data[HEADER_MAP])
		return 1;
	for (i = 0; i < size; i++)
		if (data[i])
			return -1;
	return 0;
}

/* A valid saved game's summary, and its key: what the game requires of a saved game before it
   loads it (the disc's build, the game state's layout checksum and size, the map file's checksum:
   game_state_header_valid), and the version this build gives the contents, which nothing in the
   file says. Saved games with different keys are different records and never replace each other. */
static void checkpoint_describe(const uint8_t *data, size_t size, int version, char key[CLOUD_KEY_SIZE],
	struct cloud_summary *summary)
{
	char text[HEADER_BUILD_SIZE + 96], hash[CLOUD_HASH_SIZE];

	memcpy(summary->checkpoint_map, data + HEADER_MAP, sizeof(summary->checkpoint_map) - 1);
	summary->checkpoint_map[sizeof(summary->checkpoint_map) - 1] = 0;
	memcpy(summary->game_build, data + HEADER_BUILD, HEADER_BUILD_SIZE - 1);
	summary->game_build[HEADER_BUILD_SIZE - 1] = 0;
	summary->checkpoint = 1;
	summary->checkpoint_level = campaign_level(summary->checkpoint_map);
	summary->difficulty = (int16_t)(data[HEADER_DIFFICULTY] | data[HEADER_DIFFICULTY + 1] << 8);
	summary->state_layout = read_u32(data + HEADER_LAYOUT);
	summary->state_size = (long long)size;
	snprintf(text, sizeof(text), "%s|%08lx|%zu|%08lx|%d", summary->game_build, summary->state_layout, size,
		(unsigned long)read_u32(data + HEADER_MAP_CHECKSUM), version);
	cloud_sha256_hex((const uint8_t *)text, strlen(text), hash);
	memcpy(key, hash, 12);
	key[12] = 0;
}

/* struct player_profile at the head of blam.sav (source/saved games/player_profile.h) */
enum
{
	PROFILE_NAME_CHARACTERS = 12,
	PROFILE_LEVEL_FLAGS = 0x1c,
	PROFILE_LEVELS = 10,
	PROFILE_MINIMUM_SIZE = 0x30,
};

/* UTF-16LE to UTF-8, the Basic Multilingual Plane (the Xbox keyboard's) */
static void utf16_text(const uint8_t *data, size_t characters, char *out, size_t size)
{
	size_t i, used = 0;

	for (i = 0; i < characters && used + 4 < size; i++)
	{
		unsigned code = data[i * 2] | data[i * 2 + 1] << 8;

		if (!code)
			break;
		if (code < 0x20 || (code >= 0xd800 && code < 0xe000))
			code = '?';
		if (code < 0x80)
			out[used++] = (char)code;
		else if (code < 0x800)
		{
			out[used++] = (char)(0xc0 | code >> 6);
			out[used++] = (char)(0x80 | (code & 0x3f));
		}
		else
		{
			out[used++] = (char)(0xe0 | code >> 12);
			out[used++] = (char)(0x80 | ((code >> 6) & 0x3f));
			out[used++] = (char)(0x80 | (code & 0x3f));
		}
	}
	out[used] = 0;
}

int cloud_summary_compare(const struct cloud_summary *a, const struct cloud_summary *b)
{
	if (a->profile && b->profile && a->levels != b->levels)
		return a->levels > b->levels ? 1 : -1;
	if (a->checkpoint && b->checkpoint && a->checkpoint_level != b->checkpoint_level)
		return a->checkpoint_level > b->checkpoint_level ? 1 : -1;
	if (a->modified != b->modified)
		return a->modified > b->modified ? 1 : -1;
	return 0;
}

static void summary_to_meta(const struct cloud_summary *summary, char *meta)
{
	meta_add(meta, "player", summary->player);
	meta_add(meta, "device", summary->device);
	meta_add_number(meta, "profile", summary->profile);
	meta_add_number(meta, "levels", summary->levels);
	meta_add_number(meta, "checkpoint", summary->checkpoint);
	meta_add_number(meta, "checkpointLevel", summary->checkpoint_level);
	meta_add(meta, "checkpointMap", summary->checkpoint_map);
	meta_add_number(meta, "difficulty", summary->difficulty);
	meta_add(meta, "gameBuild", summary->game_build);
	meta_add_number(meta, "stateLayout", (long long)summary->state_layout);
	meta_add_number(meta, "stateSize", summary->state_size);
	meta_add_number(meta, "modified", summary->modified);
}

static void summary_from_meta(const char *meta, struct cloud_summary *summary)
{
	memset(summary, 0, sizeof(*summary));
	cloud_meta_get(meta, "player", summary->player, sizeof(summary->player));
	cloud_meta_get(meta, "device", summary->device, sizeof(summary->device));
	summary->profile = (int)meta_number(meta, "profile");
	summary->levels = (int)meta_number(meta, "levels");
	summary->checkpoint = (int)meta_number(meta, "checkpoint");
	summary->checkpoint_level = (int)meta_number(meta, "checkpointLevel");
	cloud_meta_get(meta, "checkpointMap", summary->checkpoint_map, sizeof(summary->checkpoint_map));
	summary->difficulty = (int)meta_number(meta, "difficulty");
	cloud_meta_get(meta, "gameBuild", summary->game_build, sizeof(summary->game_build));
	summary->state_layout = (unsigned long)meta_number(meta, "stateLayout");
	summary->state_size = meta_number(meta, "stateSize");
	summary->modified = meta_number(meta, "modified");
}

/* ---------- a unit's files in memory, and its payload */

struct unit_file
{
	char name[CLOUD_NAME_SIZE];
	uint8_t *data;
	size_t size;
};

struct unit_files
{
	int count;
	struct unit_file files[CLOUD_MAX_FILES];
};

static void files_free(struct unit_files *files)
{
	int i;

	for (i = 0; i < files->count; i++)
		free(files->files[i].data);
	files->count = 0;
}

static int file_order(const void *a, const void *b)
{
	return strcmp(((const struct unit_file *)a)->name, ((const struct unit_file *)b)->name);
}

/* the unit's hash: its files' names, sizes and contents, in name order */
static void files_hash(struct unit_files *files, char hash[CLOUD_HASH_SIZE])
{
	struct sha256 sha;
	uint8_t digest[32];
	int i, b;

	qsort(files->files, (size_t)files->count, sizeof(files->files[0]), file_order);
	sha256_begin(&sha);
	for (i = 0; i < files->count; i++)
	{
		uint8_t size[8];

		for (b = 0; b < 8; b++)
			size[b] = (uint8_t)((uint64_t)files->files[i].size >> (8 * b));
		sha256_add(&sha, files->files[i].name, strlen(files->files[i].name) + 1);
		sha256_add(&sha, size, 8);
		sha256_add(&sha, files->files[i].data, files->files[i].size);
	}
	sha256_end(&sha, digest);
	hex_of(digest, 32, hash);
}

static void put_u64(uint8_t *p, uint64_t value)
{
	int b;

	for (b = 0; b < 8; b++)
		p[b] = (uint8_t)(value >> (8 * b));
}

static uint64_t get_u64(const uint8_t *p)
{
	uint64_t value = 0;
	int b;

	for (b = 0; b < 8; b++)
		value |= (uint64_t)p[b] << (8 * b);
	return value;
}

/* magic, the raw size, then deflated: a count, each file's name, size and bytes */
static int payload_pack(const struct unit_files *files, uint8_t **payload, size_t *payload_size)
{
	size_t raw_size = 1, used = 0, capacity;
	uint8_t *raw, *out;
	z_stream stream;
	int i, result;

	for (i = 0; i < files->count; i++)
		raw_size += 1 + strlen(files->files[i].name) + 8 + files->files[i].size;
	raw = malloc(raw_size);
	if (!raw)
		return 0;
	raw[used++] = (uint8_t)files->count;
	for (i = 0; i < files->count; i++)
	{
		size_t name_length = strlen(files->files[i].name);

		raw[used++] = (uint8_t)name_length;
		memcpy(raw + used, files->files[i].name, name_length); used += name_length;
		put_u64(raw + used, files->files[i].size); used += 8;
		memcpy(raw + used, files->files[i].data, files->files[i].size); used += files->files[i].size;
	}
	memset(&stream, 0, sizeof(stream));
	if (deflateInit(&stream, 6) != Z_OK)
	{
		free(raw);
		return 0;
	}
	capacity = deflateBound(&stream, (uLong)raw_size) + 16;
	out = malloc(capacity);
	if (!out)
	{
		deflateEnd(&stream);
		free(raw);
		return 0;
	}
	memcpy(out, PAYLOAD_MAGIC, 8);
	put_u64(out + 8, raw_size);
	stream.next_in = raw; stream.avail_in = (uInt)raw_size;
	stream.next_out = out + 16; stream.avail_out = (uInt)(capacity - 16);
	result = deflate(&stream, Z_FINISH);
	*payload_size = 16 + stream.total_out;
	deflateEnd(&stream);
	free(raw);
	if (result != Z_STREAM_END)
	{
		free(out);
		return 0;
	}
	*payload = out;
	return 1;
}

/* a payload from the cloud is checked as untrusted: sizes, names, and the unit's hash */
static int payload_unpack(const uint8_t *payload, size_t payload_size, const char *expected_hash, struct unit_files *files)
{
	uint64_t raw_size;
	uint8_t *raw;
	uLongf out_size;
	size_t used = 1;
	char hash[CLOUD_HASH_SIZE];
	int count, i, j;

	files->count = 0;
	if (payload_size < 17 || memcmp(payload, PAYLOAD_MAGIC, 8))
		return 0;
	raw_size = get_u64(payload + 8);
	if (!raw_size || raw_size > CLOUD_MAX_SLOT_BYTES + 4096)
		return 0;
	raw = malloc((size_t)raw_size);
	if (!raw)
		return 0;
	out_size = (uLongf)raw_size;
	if (uncompress(raw, &out_size, payload + 16, (uLong)(payload_size - 16)) != Z_OK || out_size != raw_size)
	{
		free(raw);
		return 0;
	}
	count = raw[0];
	if (count < 1 || count > CLOUD_MAX_FILES)
	{
		free(raw);
		return 0;
	}
	for (i = 0; i < count; i++)
	{
		struct unit_file *file = &files->files[files->count];
		size_t name_length;
		uint64_t size;

		if (used + 1 > raw_size)
			break;
		name_length = raw[used++];
		if (!name_length || name_length >= CLOUD_NAME_SIZE || used + name_length + 8 > raw_size)
			break;
		memcpy(file->name, raw + used, name_length); file->name[name_length] = 0; used += name_length;
		size = get_u64(raw + used); used += 8;
		if (size > raw_size - used || !cloud_file_name_valid(file->name))
			break;
		for (j = 0; j < files->count; j++)
			if (!strcasecmp(files->files[j].name, file->name))
				break;
		if (j < files->count)
			break;
		file->data = malloc(size ? (size_t)size : 1);
		if (!file->data)
			break;
		memcpy(file->data, raw + used, (size_t)size); file->size = (size_t)size; used += (size_t)size;
		files->count++;
	}
	free(raw);
	if (i < count || used != raw_size)
	{
		files_free(files);
		return 0;
	}
	files_hash(files, hash);
	if (strcmp(hash, expected_hash))
	{
		files_free(files);
		return 0;
	}
	return 1;
}

/* ---------- local slots */

enum local_status
{
	LOCAL_ABSENT,     /* no directory, or one that is not a slot yet (no SaveMeta.xbx) */
	LOCAL_OK,
	LOCAL_BUSY,       /* being written; look again later */
	LOCAL_FOREIGN,    /* holds something these rules do not cover: left alone entirely */
};

struct saved_game_cache
{
	char slot[CLOUD_SLOT_ID_SIZE];
	char signature[64];                /* size and time of the file this describes */
	int check;                         /* cloud_checkpoint_check */
	char hash[CLOUD_HASH_SIZE], key[CLOUD_KEY_SIZE];
	struct cloud_summary summary;
};

struct local_slot
{
	char id[CLOUD_SLOT_ID_SIZE];
	enum local_status status;
	struct unit_files profile;         /* every file but the saved game */
	char profile_hash[CLOUD_HASH_SIZE];
	struct cloud_summary profile_summary;
	int saved_game;                    /* 1 valid, 0 none (absent or all zeros), -1 neither or busy */
	char saved_game_hash[CLOUD_HASH_SIZE], saved_game_key[CLOUD_KEY_SIZE];
	struct cloud_summary saved_game_summary;
};

struct cloud_engine
{
	struct cloud_config config;
	struct cloud_store store;
	char *save_root, *state_path, *device, *platform, *app, *image;

	/* the state file */
	char device_id[33];
	char account[CLOUD_HASH_SIZE], pending_account[CLOUD_HASH_SIZE];
	int paused;                        /* 0, or the CLOUD_STATE_ it waits in */
	int quota_noticed, newer_noticed;
	/* hash: the version this device last had in common with the cloud ("" for none yet). sent: a
	   version whose upload was begun; the cloud holding it means the upload arrived, whatever the reply */
	struct { char unit[CLOUD_RECORD_NAME_SIZE], hash[CLOUD_HASH_SIZE], tag[CLOUD_TAG_SIZE], sent[CLOUD_HASH_SIZE]; } bases[MAX_BASES];
	int base_count;
	struct { char slot[CLOUD_SLOT_ID_SIZE], hash[CLOUD_HASH_SIZE]; } deletes[CLOUD_MAX_SLOTS];
	int delete_count;
	struct { char slot[CLOUD_SLOT_ID_SIZE], key[CLOUD_KEY_SIZE]; } keys[CLOUD_MAX_SLOTS];
	int key_count;
	struct cloud_notice notices[MAX_NOTICES];
	int notice_count;
	int dirty;

	struct saved_game_cache cache[CLOUD_MAX_SLOTS];
	int cache_count;

	/* one pass */
	struct cloud_record *records;
	int record_count;
	int launch, settle, pending, quota, newer, failed, uploaded;
	enum cloud_state state;
};

void cloud_engine_local_delete(struct cloud_engine *engine, const char *slot);
static void deletes_take(struct cloud_engine *engine);

static void engine_log(struct cloud_engine *engine, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void engine_log(struct cloud_engine *engine, const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (engine->config.log)
		engine->config.log(engine->config.context, line);
}

static void slot_directory(const char *save_root, const char *id, char *path, size_t size)
{
	snprintf(path, size, "%s/u/UDATA%s%s", save_root, id ? "/" : "", id ? id : "");
}

static uint8_t *read_whole(const char *path, size_t *size, struct stat *before)
{
	struct stat after;
	uint8_t *data;
	size_t used = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return NULL;
	if (fstat(fd, before) != 0 || !S_ISREG(before->st_mode) || (uint64_t)before->st_size > CLOUD_MAX_SLOT_BYTES)
	{
		close(fd);
		return NULL;
	}
	data = malloc(before->st_size ? (size_t)before->st_size : 1);
	while (data && used < (size_t)before->st_size)
	{
		ssize_t got = read(fd, data + used, (size_t)before->st_size - used);

		if (got <= 0)
			break;
		used += (size_t)got;
	}
	/* the file as it was when the read began: same size and time after it */
	if (!data || used != (size_t)before->st_size || fstat(fd, &after) != 0 || after.st_size != before->st_size ||
		after.st_mtimespec.tv_sec != before->st_mtimespec.tv_sec || after.st_mtimespec.tv_nsec != before->st_mtimespec.tv_nsec)
	{
		free(data);
		data = NULL;
	}
	close(fd);
	*size = used;
	return data;
}

static int still_settling(const struct stat *st, long long now)
{
	return now - (long long)st->st_mtimespec.tv_sec < CLOUD_SETTLE_SECONDS;
}

static void local_free(struct local_slot *local)
{
	files_free(&local->profile);
}

static struct saved_game_cache *cache_for(struct cloud_engine *engine, const char *slot)
{
	int i;

	for (i = 0; i < engine->cache_count; i++)
		if (!strcmp(engine->cache[i].slot, slot))
			return &engine->cache[i];
	if (engine->cache_count == CLOUD_MAX_SLOTS)
		engine->cache_count = 0;
	memset(&engine->cache[engine->cache_count], 0, sizeof(engine->cache[0]));
	copy_text(engine->cache[engine->cache_count].slot, CLOUD_SLOT_ID_SIZE, slot);
	return &engine->cache[engine->cache_count++];
}

/* the saved game's bytes when they are still what the slot was read as; NULL otherwise */
static uint8_t *saved_game_bytes(struct cloud_engine *engine, const struct local_slot *local, size_t *size)
{
	char path[1200], directory[1100], hash[CLOUD_HASH_SIZE];
	struct stat st;
	uint8_t *data;
	struct unit_files one = { .count = 1 };

	slot_directory(engine->save_root, local->id, directory, sizeof(directory));
	snprintf(path, sizeof(path), "%s/%s", directory, CHECKPOINT_FILE);
	data = read_whole(path, size, &st);
	if (!data)
		return NULL;
	copy_text(one.files[0].name, CLOUD_NAME_SIZE, CHECKPOINT_FILE);
	one.files[0].data = data; one.files[0].size = *size;
	files_hash(&one, hash);
	if (strcmp(hash, local->saved_game_hash))
	{
		free(data);
		return NULL;
	}
	return data;
}

/* Reads a slot: its profile files into memory, and what its saved game is (from the cache when
   the file has not changed). settle: files changed in the last seconds make the slot busy. */
static void local_read(struct cloud_engine *engine, const char *id, int settle, struct local_slot *local)
{
	char directory[1100], path[1200];
	long long now = engine->config.now(engine->config.context);
	struct dirent *entry;
	struct stat st;
	int has_meta = 0;
	DIR *dir;

	memset(local, 0, sizeof(*local));
	copy_text(local->id, sizeof(local->id), id);
	slot_directory(engine->save_root, id, directory, sizeof(directory));
	if (lstat(directory, &st) != 0)
		return;
	if (!S_ISDIR(st.st_mode))
	{
		local->status = LOCAL_FOREIGN;
		return;
	}
	if (settle && still_settling(&st, now))
		local->status = LOCAL_BUSY;
	dir = opendir(directory);
	if (!dir)
	{
		local->status = LOCAL_FOREIGN;
		return;
	}
	local->profile_summary.modified = 0;
	while ((entry = readdir(dir)) != NULL)
	{
		struct unit_file *file;
		size_t size;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		if (!strncmp(entry->d_name, TEMP_PREFIX, strlen(TEMP_PREFIX)))
		{
			/* a write of ours that did not finish */
			if (engine->launch)
				unlink(path);
			continue;
		}
		if (lstat(path, &st) != 0)
		{
			local->status = LOCAL_BUSY;
			continue;
		}
		if (!S_ISREG(st.st_mode) || !cloud_file_name_valid(entry->d_name) || local->profile.count == CLOUD_MAX_FILES ||
			(uint64_t)st.st_size > CLOUD_MAX_SLOT_BYTES)
		{
			local->status = LOCAL_FOREIGN;
			break;
		}
		if (settle && still_settling(&st, now) && local->status != LOCAL_FOREIGN)
			local->status = LOCAL_BUSY;
		if (!strcmp(entry->d_name, CHECKPOINT_FILE))
		{
			struct saved_game_cache *cache = cache_for(engine, id);
			char signature[64];

			snprintf(signature, sizeof(signature), "%lld:%lld:%ld", (long long)st.st_size, (long long)st.st_mtimespec.tv_sec,
				(long)st.st_mtimespec.tv_nsec);
			if (strcmp(cache->signature, signature) && local->status != LOCAL_BUSY)
			{
				struct stat read_at;
				uint8_t *data = read_whole(path, &size, &read_at);

				if (!data)
				{
					local->saved_game = -1;
					continue;
				}
				memset(&cache->summary, 0, sizeof(cache->summary));
				cache->check = cloud_checkpoint_check(data, size);
				cache->hash[0] = cache->key[0] = 0;
				if (cache->check == 1)
				{
					struct unit_files one = { .count = 1 };

					copy_text(one.files[0].name, CLOUD_NAME_SIZE, CHECKPOINT_FILE);
					one.files[0].data = data; one.files[0].size = size;
					files_hash(&one, cache->hash);
					checkpoint_describe(data, size, engine->config.saved_game_version, cache->key, &cache->summary);
					cache->summary.modified = (long long)read_at.st_mtimespec.tv_sec;
				}
				free(data);
				snprintf(cache->signature, sizeof(cache->signature), "%lld:%lld:%ld", (long long)read_at.st_size,
					(long long)read_at.st_mtimespec.tv_sec, (long)read_at.st_mtimespec.tv_nsec);
			}
			if (!strcmp(cache->signature, signature))
			{
				local->saved_game = cache->check;
				copy_text(local->saved_game_hash, CLOUD_HASH_SIZE, cache->hash);
				copy_text(local->saved_game_key, CLOUD_KEY_SIZE, cache->key);
				local->saved_game_summary = cache->summary;
			}
			else
				local->saved_game = -1;
			continue;
		}
		file = &local->profile.files[local->profile.count];
		file->data = read_whole(path, &size, &st);
		if (!file->data)
		{
			if (local->status != LOCAL_FOREIGN)
				local->status = LOCAL_BUSY;
			continue;
		}
		copy_text(file->name, sizeof(file->name), entry->d_name);
		file->size = size;
		local->profile.count++;
		if ((long long)st.st_mtimespec.tv_sec > local->profile_summary.modified)
			local->profile_summary.modified = (long long)st.st_mtimespec.tv_sec;
		if (!strcmp(entry->d_name, META_FILE))
			has_meta = 1;
	}
	closedir(dir);
	if (local->status == LOCAL_FOREIGN || local->status == LOCAL_BUSY)
	{
		files_free(&local->profile);
		return;
	}
	if (!has_meta)
	{
		/* not a save the game lists (yet) */
		files_free(&local->profile);
		local->saved_game = 0;
		return;
	}
	{
		/* whole as the game writes it: a name, and one 512-byte profile or game type. Anything else
		   is a save being made, removed, or cut short, and never leaves the device */
		int i, bodies = 0, whole = 1;

		for (i = 0; i < local->profile.count; i++)
		{
			const struct unit_file *file = &local->profile.files[i];

			if (!strcmp(file->name, META_FILE))
				whole &= file->size >= 2 && file->size <= 128 && !(file->size & 1);
			else if (!strcmp(file->name, PROFILE_FILE) || !strcmp(file->name, VARIANT_FILE))
			{
				bodies++;
				whole &= file->size == SAVE_FILE_SIZE;
			}
		}
		if (!whole || bodies != 1)
		{
			files_free(&local->profile);
			local->status = LOCAL_BUSY;
			return;
		}
	}
	local->status = LOCAL_OK;
	files_hash(&local->profile, local->profile_hash);
	{
		int i, level;

		for (i = 0; i < local->profile.count; i++)
		{
			const struct unit_file *file = &local->profile.files[i];

			if (!strcmp(file->name, META_FILE))
				utf16_text(file->data, file->size / 2, local->profile_summary.player, sizeof(local->profile_summary.player));
			if (!strcmp(file->name, PROFILE_FILE) && file->size >= PROFILE_MINIMUM_SIZE)
			{
				local->profile_summary.profile = 1;
				for (level = 0; level < PROFILE_LEVELS; level++)
					if (file->data[PROFILE_LEVEL_FLAGS + level])
						local->profile_summary.levels++;
			}
		}
	}
	copy_text(local->profile_summary.device, sizeof(local->profile_summary.device), engine->device);
	copy_text(local->saved_game_summary.device, sizeof(local->saved_game_summary.device), engine->device);
	copy_text(local->saved_game_summary.player, sizeof(local->saved_game_summary.player), local->profile_summary.player);
}

static int slot_order(const void *a, const void *b) { return strcmp(a, b); }

/* the ids of the directories under UDATA that are named as slots */
static int local_slot_ids(const char *save_root, char ids[][CLOUD_SLOT_ID_SIZE], int capacity)
{
	char directory[1100];
	struct dirent *entry;
	int count = 0;
	DIR *dir;

	slot_directory(save_root, NULL, directory, sizeof(directory));
	dir = opendir(directory);
	if (!dir)
		return 0;
	while ((entry = readdir(dir)) != NULL && count < capacity)
		if (strlen(entry->d_name) == 12 && cloud_slot_id_valid(entry->d_name))
			copy_text(ids[count++], CLOUD_SLOT_ID_SIZE, entry->d_name);
	closedir(dir);
	qsort(ids, (size_t)count, CLOUD_SLOT_ID_SIZE, slot_order);
	return count;
}

int cloud_local_slot_count(const char *save_root)
{
	char ids[CLOUD_MAX_SLOTS][CLOUD_SLOT_ID_SIZE];
	int count = local_slot_ids(save_root, ids, CLOUD_MAX_SLOTS), i, found = 0;

	for (i = 0; i < count; i++)
	{
		char path[1200];

		snprintf(path, sizeof(path), "%s/u/UDATA/%s/%s", save_root, ids[i], META_FILE);
		found += access(path, F_OK) == 0;
	}
	return found;
}

void cloud_local_signature(const char *save_root, const char *state_path, char hash[CLOUD_HASH_SIZE])
{
	char ids[CLOUD_MAX_SLOTS][CLOUD_SLOT_ID_SIZE], directory[1100], path[1400], line[1500];
	int count = local_slot_ids(save_root, ids, CLOUD_MAX_SLOTS), i;
	struct sha256 sha;
	uint8_t digest[32];
	struct stat st;

	sha256_begin(&sha);
	for (i = 0; i < count; i++)
	{
		struct dirent *entry;
		DIR *dir;

		slot_directory(save_root, ids[i], directory, sizeof(directory));
		dir = opendir(directory);
		if (!dir)
			continue;
		if (stat(directory, &st) == 0)
		{
			snprintf(line, sizeof(line), "%s %lld.%ld\n", ids[i], (long long)st.st_mtimespec.tv_sec, (long)st.st_mtimespec.tv_nsec);
			sha256_add(&sha, line, strlen(line));
		}
		while ((entry = readdir(dir)) != NULL)
		{
			snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
			if (entry->d_name[0] == '.' || stat(path, &st) != 0)
				continue;
			/* (the directory's order is not sorted, and need not be: it is the same until something changes) */
			snprintf(line, sizeof(line), "%s/%s %lld %lld.%ld\n", ids[i], entry->d_name, (long long)st.st_size,
				(long long)st.st_mtimespec.tv_sec, (long)st.st_mtimespec.tv_nsec);
			sha256_add(&sha, line, strlen(line));
		}
		closedir(dir);
	}
	snprintf(path, sizeof(path), "%s.deleted", state_path);
	if (stat(path, &st) == 0)
	{
		snprintf(line, sizeof(line), "deleted %lld\n", (long long)st.st_size);
		sha256_add(&sha, line, strlen(line));
	}
	sha256_end(&sha, digest);
	hex_of(digest, 32, hash);
}

int cloud_slot_of_path(const char *save_root, const char *path, char *slot)
{
	char directory[1100];
	size_t length;

	size_t i;

	/* the game finds each folder whatever its case (port/linux/src/xbox_files.c) */
	slot_directory(save_root, NULL, directory, sizeof(directory));
	length = strlen(save_root);
	if (strncmp(path, save_root, length) || strncasecmp(path + length, directory + length, strlen(directory) - length))
		return 0;
	length = strlen(directory);
	if (path[length] != '/')
		return 0;
	path += length + 1;
	length = strlen(path);
	while (length && path[length - 1] == '/')
		length--;
	if (length != 12)
		return 0;
	for (i = 0; i < 12; i++)
		slot[i] = (char)toupper((unsigned char)path[i]);
	slot[12] = 0;
	return cloud_slot_id_valid(slot);
}

/* writes one file through a temporary name in the same directory */
static int write_file(const char *directory, const char *name, const uint8_t *data, size_t size)
{
	char temp[1300], path[1300];
	size_t used = 0;
	int fd;

	snprintf(temp, sizeof(temp), "%s/%s%s", directory, TEMP_PREFIX, name);
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return 0;
	while (used < size)
	{
		ssize_t put = write(fd, data + used, size - used);

		if (put <= 0)
			break;
		used += (size_t)put;
	}
	if (used != size || fsync(fd) != 0)
	{
		close(fd);
		unlink(temp);
		return 0;
	}
	close(fd);
	if (rename(temp, path) != 0)
	{
		unlink(temp);
		return 0;
	}
	return 1;
}

static int make_slot_directory(const char *save_root, const char *id, char *directory, size_t size)
{
	char path[1100];

	snprintf(path, sizeof(path), "%s/u", save_root);
	mkdir(path, 0755);
	slot_directory(save_root, NULL, path, sizeof(path));
	mkdir(path, 0755);
	slot_directory(save_root, id, directory, size);
	return mkdir(directory, 0755) == 0 || errno == EEXIST;
}

/* Makes the slot's profile files those given: each written whole, SaveMeta.xbx (which makes the
   directory a save the game lists) last, and the profile files that are not among them removed. */
static int local_write_profile(struct cloud_engine *engine, const char *id, const struct unit_files *files)
{
	char directory[1100], path[1300];
	struct dirent *entry;
	DIR *dir;
	int i, pass;

	if (!make_slot_directory(engine->save_root, id, directory, sizeof(directory)))
		return 0;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < files->count; i++)
			if ((strcmp(files->files[i].name, META_FILE) == 0) == pass &&
				!write_file(directory, files->files[i].name, files->files[i].data, files->files[i].size))
				return 0;
	dir = opendir(directory);
	while (dir && (entry = readdir(dir)) != NULL)
	{
		if (entry->d_name[0] == '.' || !strcmp(entry->d_name, CHECKPOINT_FILE))
			continue;
		for (i = 0; i < files->count; i++)
			if (!strcmp(files->files[i].name, entry->d_name))
				break;
		if (i == files->count)
		{
			snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
			unlink(path);
		}
	}
	if (dir)
		closedir(dir);
	return 1;
}

static int local_remove_slot(struct cloud_engine *engine, const char *id)
{
	char directory[1100], path[1400];
	struct dirent *entry;
	DIR *dir;

	slot_directory(engine->save_root, id, directory, sizeof(directory));
	dir = opendir(directory);
	if (!dir)
		return errno == ENOENT;
	/* SaveMeta.xbx first: without it the directory is no longer a save */
	snprintf(path, sizeof(path), "%s/%s", directory, META_FILE);
	unlink(path);
	while ((entry = readdir(dir)) != NULL)
	{
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		unlink(path);
	}
	closedir(dir);
	return rmdir(directory) == 0;
}

/* ---------- the state file */

static void escape_field(const char *text, char *out, size_t size)
{
	size_t used = 0;

	for (; *text && used + 4 < size; text++)
	{
		if ((unsigned char)*text < 0x20 || *text == '%')
			used += (size_t)snprintf(out + used, size - used, "%%%02X", (unsigned char)*text);
		else
			out[used++] = *text;
	}
	out[used] = 0;
}

static void unescape_field(char *text)
{
	char *out = text;

	for (; *text; text++)
	{
		if (*text == '%' && isxdigit((unsigned char)text[1]) && isxdigit((unsigned char)text[2]))
		{
			char hex[3] = { text[1], text[2], 0 };

			*out++ = (char)strtol(hex, NULL, 16);
			text += 2;
		}
		else
			*out++ = *text;
	}
	*out = 0;
}

static void state_save(struct cloud_engine *engine)
{
	char temp[1100], meta[CLOUD_META_SIZE], escaped[CLOUD_META_SIZE * 3];
	FILE *file;
	int i;

	if (!engine->dirty)
		return;
	snprintf(temp, sizeof(temp), "%s.tmp", engine->state_path);
	file = fopen(temp, "w");
	if (!file)
	{
		engine_log(engine, "cannot write %s (errno %d)", temp, errno);
		return;
	}
	fprintf(file, "%s\ndevice\t%s\n", STATE_HEADER, engine->device_id);
	if (engine->account[0])
		fprintf(file, "account\t%s\n", engine->account);
	if (engine->paused)
		fprintf(file, "paused\t%d\t%s\n", engine->paused, engine->pending_account);
	if (engine->quota_noticed)
		fprintf(file, "quota\t1\n");
	if (engine->newer_noticed)
		fprintf(file, "newer\t1\n");
	for (i = 0; i < engine->base_count; i++)
		fprintf(file, "base\t%s\t%s\t%s\t%s\n", engine->bases[i].unit, engine->bases[i].hash[0] ? engine->bases[i].hash : "-",
			engine->bases[i].tag[0] ? engine->bases[i].tag : "-", engine->bases[i].sent[0] ? engine->bases[i].sent : "-");
	for (i = 0; i < engine->delete_count; i++)
		fprintf(file, "deleted\t%s\t%s\n", engine->deletes[i].slot, engine->deletes[i].hash[0] ? engine->deletes[i].hash : "-");
	for (i = 0; i < engine->key_count; i++)
		fprintf(file, "key\t%s\t%s\n", engine->keys[i].slot, engine->keys[i].key);
	for (i = 0; i < engine->notice_count; i++)
	{
		const struct cloud_notice *notice = &engine->notices[i];

		fprintf(file, "notice\t%d\t%s\t%d", (int)notice->kind, notice->unit[0] ? notice->unit : "-", notice->kept_local);
		meta[0] = 0; summary_to_meta(&notice->kept, meta); escape_field(meta, escaped, sizeof(escaped));
		fprintf(file, "\t%s", escaped);
		meta[0] = 0; summary_to_meta(&notice->aside, meta); escape_field(meta, escaped, sizeof(escaped));
		fprintf(file, "\t%s\n", escaped);
	}
	if (fflush(file) != 0 || fsync(fileno(file)) != 0)
	{
		fclose(file);
		unlink(temp);
		return;
	}
	fclose(file);
	if (rename(temp, engine->state_path) == 0)
		engine->dirty = 0;
}

static int split_fields(char *line, char **fields, int capacity)
{
	int count = 0;

	while (count < capacity)
	{
		fields[count++] = line;
		line = strchr(line, '\t');
		if (!line)
			break;
		*line++ = 0;
	}
	return count;
}

static void state_load(struct cloud_engine *engine)
{
	static char line[CLOUD_META_SIZE * 8];
	FILE *file = fopen(engine->state_path, "r");
	char *fields[8];
	int first = 1;

	while (file && fgets(line, sizeof(line), file))
	{
		int count;

		line[strcspn(line, "\r\n")] = 0;
		if (first)
		{
			first = 0;
			/* a state file of another layout is not read: without bases every unit is compared afresh */
			if (strcmp(line, STATE_HEADER))
				break;
			continue;
		}
		count = split_fields(line, fields, 8);
		if (!strcmp(fields[0], "device") && count == 2 && strlen(fields[1]) == 32)
			copy_text(engine->device_id, sizeof(engine->device_id), fields[1]);
		else if (!strcmp(fields[0], "account") && count == 2)
			copy_text(engine->account, sizeof(engine->account), fields[1]);
		else if (!strcmp(fields[0], "paused") && count == 3)
		{
			engine->paused = atoi(fields[1]);
			copy_text(engine->pending_account, sizeof(engine->pending_account), fields[2]);
			if (engine->paused != CLOUD_STATE_ACCOUNT_CHANGED && engine->paused != CLOUD_STATE_CLOUD_DELETED)
				engine->paused = 0;
		}
		else if (!strcmp(fields[0], "quota"))
			engine->quota_noticed = 1;
		else if (!strcmp(fields[0], "newer"))
			engine->newer_noticed = 1;
		else if (!strcmp(fields[0], "base") && count == 5 && engine->base_count < MAX_BASES)
		{
			char slot[CLOUD_SLOT_ID_SIZE], key[CLOUD_KEY_SIZE];

			if (strlen(fields[1]) < CLOUD_RECORD_NAME_SIZE && unit_parse(fields[1], slot, key) &&
				(strlen(fields[2]) == 64 || !strcmp(fields[2], "-")) && strlen(fields[3]) < CLOUD_TAG_SIZE &&
				(strlen(fields[4]) == 64 || !strcmp(fields[4], "-")))
			{
				memset(&engine->bases[engine->base_count], 0, sizeof(engine->bases[0]));
				copy_text(engine->bases[engine->base_count].unit, CLOUD_RECORD_NAME_SIZE, fields[1]);
				copy_text(engine->bases[engine->base_count].hash, CLOUD_HASH_SIZE, strcmp(fields[2], "-") ? fields[2] : "");
				copy_text(engine->bases[engine->base_count].tag, CLOUD_TAG_SIZE, strcmp(fields[3], "-") ? fields[3] : "");
				copy_text(engine->bases[engine->base_count++].sent, CLOUD_HASH_SIZE, strcmp(fields[4], "-") ? fields[4] : "");
			}
		}
		else if (!strcmp(fields[0], "deleted") && count == 3 && engine->delete_count < CLOUD_MAX_SLOTS &&
			strlen(fields[1]) == 12 && cloud_slot_id_valid(fields[1]))
		{
			copy_text(engine->deletes[engine->delete_count].slot, CLOUD_SLOT_ID_SIZE, fields[1]);
			copy_text(engine->deletes[engine->delete_count++].hash, CLOUD_HASH_SIZE, strlen(fields[2]) == 64 ? fields[2] : "");
		}
		else if (!strcmp(fields[0], "key") && count == 3 && engine->key_count < CLOUD_MAX_SLOTS && strlen(fields[1]) == 12 &&
			cloud_slot_id_valid(fields[1]) && strlen(fields[2]) == 12 && key_valid(fields[2]))
		{
			copy_text(engine->keys[engine->key_count].slot, CLOUD_SLOT_ID_SIZE, fields[1]);
			copy_text(engine->keys[engine->key_count++].key, CLOUD_KEY_SIZE, fields[2]);
		}
		else if (!strcmp(fields[0], "notice") && count == 6 && engine->notice_count < MAX_NOTICES)
		{
			struct cloud_notice *notice = &engine->notices[engine->notice_count];

			memset(notice, 0, sizeof(*notice));
			notice->kind = (enum cloud_notice_kind)atoi(fields[1]);
			if (notice->kind < CLOUD_NOTICE_CONFLICT || notice->kind > CLOUD_NOTICE_RESTORE_INCOMPLETE)
				continue;
			if (strcmp(fields[2], "-") && strlen(fields[2]) < CLOUD_RECORD_NAME_SIZE)
				copy_text(notice->unit, sizeof(notice->unit), fields[2]);
			notice->kept_local = atoi(fields[3]);
			unescape_field(fields[4]); summary_from_meta(fields[4], &notice->kept);
			unescape_field(fields[5]); summary_from_meta(fields[5], &notice->aside);
			engine->notice_count++;
		}
	}
	if (file)
		fclose(file);
	if (!engine->device_id[0])
	{
		uint8_t bytes[16] = { 0 };

		if (engine->config.random)
			engine->config.random(engine->config.context, bytes, sizeof(bytes));
		hex_of(bytes, 16, engine->device_id);
		engine->dirty = 1;
	}
}

static int base_index(const struct cloud_engine *engine, const char *unit)
{
	int i;

	for (i = 0; i < engine->base_count; i++)
		if (!strcmp(engine->bases[i].unit, unit))
			return i;
	return -1;
}

static const char *base_hash(const struct cloud_engine *engine, const char *unit)
{
	int index = base_index(engine, unit);

	return index < 0 ? "" : engine->bases[index].hash;
}

static void base_set(struct cloud_engine *engine, const char *unit, const char *hash, const char *tag)
{
	int index = base_index(engine, unit);

	if (index >= 0 && !strcmp(engine->bases[index].hash, hash) && !strcmp(engine->bases[index].tag, tag) &&
		!engine->bases[index].sent[0])
		return;
	if (index < 0)
	{
		if (engine->base_count == MAX_BASES)
			return;
		index = engine->base_count++;
		memset(&engine->bases[index], 0, sizeof(engine->bases[0]));
		copy_text(engine->bases[index].unit, CLOUD_RECORD_NAME_SIZE, unit);
	}
	copy_text(engine->bases[index].hash, CLOUD_HASH_SIZE, hash);
	copy_text(engine->bases[index].tag, CLOUD_TAG_SIZE, tag);
	engine->bases[index].sent[0] = 0;
	engine->dirty = 1;
}

static void state_save(struct cloud_engine *engine);

/* Written to the state file before an upload begins. If the reply is lost, or the app ends first,
   the next pass finds the cloud holding this hash and knows the version there is this device's own. */
static void base_sending(struct cloud_engine *engine, const char *unit, const char *hash)
{
	int index = base_index(engine, unit);

	if (index < 0)
	{
		if (engine->base_count == MAX_BASES)
			return;
		index = engine->base_count++;
		memset(&engine->bases[index], 0, sizeof(engine->bases[0]));
		copy_text(engine->bases[index].unit, CLOUD_RECORD_NAME_SIZE, unit);
	}
	copy_text(engine->bases[index].sent, CLOUD_HASH_SIZE, hash);
	engine->dirty = 1;
	state_save(engine);
}

static void base_drop(struct cloud_engine *engine, const char *unit)
{
	int index = base_index(engine, unit);

	if (index < 0)
		return;
	if (engine->bases[index].sent[0])
	{
		/* an upload may still be on its way: only the common version is forgotten */
		if (engine->bases[index].hash[0])
			engine->dirty = 1;
		engine->bases[index].hash[0] = engine->bases[index].tag[0] = 0;
		return;
	}
	engine->bases[index] = engine->bases[--engine->base_count];
	engine->dirty = 1;
}

/* forgets what this device had in common with the cloud */
static void bases_forget(struct cloud_engine *engine)
{
	if (engine->base_count || engine->delete_count)
		engine->dirty = 1;
	engine->base_count = 0;
	engine->delete_count = 0;
}

static int delete_index(const struct cloud_engine *engine, const char *slot)
{
	int i;

	for (i = 0; i < engine->delete_count; i++)
		if (!strcmp(engine->deletes[i].slot, slot))
			return i;
	return -1;
}

static void delete_drop(struct cloud_engine *engine, const char *slot)
{
	int index = delete_index(engine, slot);

	if (index < 0)
		return;
	engine->deletes[index] = engine->deletes[--engine->delete_count];
	engine->dirty = 1;
}

static const char *key_of(const struct cloud_engine *engine, const char *slot)
{
	int i;

	for (i = 0; i < engine->key_count; i++)
		if (!strcmp(engine->keys[i].slot, slot))
			return engine->keys[i].key;
	return "";
}

static void key_set(struct cloud_engine *engine, const char *slot, const char *key)
{
	int i;

	for (i = 0; i < engine->key_count; i++)
		if (!strcmp(engine->keys[i].slot, slot))
			break;
	if (i < engine->key_count && !strcmp(engine->keys[i].key, key))
		return;
	if (i == engine->key_count)
	{
		if (engine->key_count == CLOUD_MAX_SLOTS)
			return;
		copy_text(engine->keys[engine->key_count++].slot, CLOUD_SLOT_ID_SIZE, slot);
	}
	copy_text(engine->keys[i].key, CLOUD_KEY_SIZE, key);
	engine->dirty = 1;
}

static void notice_add(struct cloud_engine *engine, const struct cloud_notice *notice)
{
	int i;

	for (i = 0; i < engine->notice_count; i++)
		if (engine->notices[i].kind == notice->kind && !strcmp(engine->notices[i].unit, notice->unit))
		{
			engine->notices[i] = *notice;
			engine->dirty = 1;
			return;
		}
	if (engine->notice_count == MAX_NOTICES)
	{
		memmove(engine->notices, engine->notices + 1, sizeof(engine->notices[0]) * (MAX_NOTICES - 1));
		engine->notice_count--;
	}
	engine->notices[engine->notice_count++] = *notice;
	engine->dirty = 1;
}

static void notice_simple(struct cloud_engine *engine, enum cloud_notice_kind kind)
{
	struct cloud_notice notice;

	memset(&notice, 0, sizeof(notice));
	notice.kind = kind;
	notice_add(engine, &notice);
}

int cloud_engine_notice(const struct cloud_engine *engine, struct cloud_notice *notice)
{
	if (!engine->notice_count)
		return 0;
	*notice = engine->notices[0];
	return 1;
}

void cloud_engine_notice_done(struct cloud_engine *engine)
{
	if (!engine->notice_count)
		return;
	memmove(engine->notices, engine->notices + 1, sizeof(engine->notices[0]) * (size_t)(engine->notice_count - 1));
	engine->notice_count--;
	engine->dirty = 1;
	state_save(engine);
}

/* ---------- records */

static struct cloud_record *record_find(struct cloud_engine *engine, const char *name)
{
	int i;

	for (i = 0; i < engine->record_count; i++)
		if (!strcmp(engine->records[i].name, name))
			return &engine->records[i];
	return NULL;
}

/* keeps the list as the cloud is after a save or a fetch of ours */
static void record_put(struct cloud_engine *engine, const struct cloud_record *record)
{
	struct cloud_record *found = record_find(engine, record->name);
	struct cloud_record *grown;

	if (found)
	{
		*found = *record;
		return;
	}
	grown = realloc(engine->records, sizeof(*grown) * (size_t)(engine->record_count + 1));
	if (!grown)
		return;
	engine->records = grown;
	engine->records[engine->record_count++] = *record;
}

static void record_forget(struct cloud_engine *engine, const char *name)
{
	struct cloud_record *found = record_find(engine, name);

	if (found)
		*found = engine->records[--engine->record_count];
}

/* the record as the cloud has it now, after a save of ours found another version there */
static void record_refresh(struct cloud_engine *engine, const char *name)
{
	struct cloud_record fetched;
	uint8_t *payload = NULL;
	size_t size = 0;
	enum cloud_status status = engine->store.fetch(engine->store.context, name, &fetched, &payload, &size);

	free(payload);
	if (status == CLOUD_OK)
		record_put(engine, &fetched);
	else if (status == CLOUD_NOT_FOUND)
		record_forget(engine, name);
}

static int record_deleted(const struct cloud_record *record)
{
	char state[16];

	return cloud_meta_get(record->meta, "state", state, sizeof(state)) && !strcmp(state, "deleted");
}

/* a record this build may not touch: written in a layout it does not know, or not a well-formed one */
static int record_usable(const struct cloud_record *record)
{
	char slot[CLOUD_SLOT_ID_SIZE], key[CLOUD_KEY_SIZE];

	return record->min_format <= CLOUD_FORMAT && unit_parse(record->unit, slot, key) && strlen(record->hash) == 64;
}

/* what the store's answer means for the pass; 1 when the call succeeded */
static int store_ok(struct cloud_engine *engine, enum cloud_status status, const char *what, const char *name)
{
	switch (status)
	{
	case CLOUD_OK:
		return 1;
	case CLOUD_QUOTA:
		engine->quota = 1;
		break;
	case CLOUD_FAILED:
		engine->failed = 1;
		break;
	default:
		engine->pending = 1;
		break;
	}
	engine_log(engine, "%s %s: %s", what, name,
		status == CLOUD_QUOTA ? "the iCloud storage is full" : status == CLOUD_CHANGED ? "the cloud's version changed meanwhile" :
		status == CLOUD_RETRY ? "not reachable now, will retry" : status == CLOUD_NOT_FOUND ? "not found" : "failed");
	return 0;
}

static void record_fill(struct cloud_engine *engine, struct cloud_record *record, const char *name, const char *unit,
	const char *hash, const struct cloud_summary *summary)
{
	memset(record, 0, sizeof(*record));
	copy_text(record->name, sizeof(record->name), name);
	copy_text(record->unit, sizeof(record->unit), unit);
	record->copy = strcmp(name, unit) != 0;
	record->format = CLOUD_FORMAT;
	record->min_format = CLOUD_MIN_FORMAT;
	copy_text(record->hash, sizeof(record->hash), hash);
	meta_add(record->meta, "state", "live");
	summary_to_meta(summary, record->meta);
	meta_add(record->meta, "deviceID", engine->device_id);
	meta_add(record->meta, "platform", engine->platform);
	meta_add(record->meta, "app", engine->app);
	meta_add(record->meta, "image", engine->image);
	meta_add_number(record->meta, "net", engine->config.network_version);
	meta_add_number(record->meta, "savedGameVersion", engine->config.saved_game_version);
}

static void copy_name(const char *unit, const char *hash, char *name, size_t size)
{
	char slot[CLOUD_SLOT_ID_SIZE] = "", key[CLOUD_KEY_SIZE] = "";

	unit_parse(unit, slot, key);
	snprintf(name, size, "copy-%s-%s-%.16s", slot, key[0] ? key : "profile", hash);
}

/* ---------- one unit against the cloud */

struct unit
{
	char name[CLOUD_RECORD_NAME_SIZE];
	const struct local_slot *slot;
	int saved_game;                    /* the slot's saved game, not its profile */
	int local;                         /* present on disk */
	const char *hash;
	const struct cloud_summary *summary;
};

static int unit_pack(struct cloud_engine *engine, const struct unit *unit, uint8_t **payload, size_t *size)
{
	struct unit_files one = { .count = 1 };
	int packed;

	if (!unit->saved_game)
		return payload_pack(&unit->slot->profile, payload, size);
	copy_text(one.files[0].name, CLOUD_NAME_SIZE, CHECKPOINT_FILE);
	one.files[0].data = saved_game_bytes(engine, unit->slot, &one.files[0].size);
	if (!one.files[0].data)
		return 0;
	packed = payload_pack(&one, payload, size);
	free(one.files[0].data);
	return packed;
}

/* Stores the unit's local version under name. expected_tag as the store's save. */
static enum cloud_status unit_save(struct cloud_engine *engine, const struct unit *unit, const char *name,
	const char *expected_tag, const char *reason)
{
	struct cloud_record record;
	uint8_t *payload = NULL;
	size_t size = 0;
	enum cloud_status status;

	if (!unit_pack(engine, unit, &payload, &size))
	{
		engine_log(engine, "%s changed while it was read; it is looked at again later", unit->name);
		return CLOUD_RETRY;
	}
	record_fill(engine, &record, name, unit->name, unit->hash, unit->summary);
	if (reason)
	{
		meta_add(record.meta, "reason", reason);
		meta_add_number(record.meta, "copied", engine->config.now(engine->config.context));
	}
	status = engine->store.save(engine->store.context, &record, payload, size, expected_tag, record.tag, sizeof(record.tag));
	free(payload);
	if (status == CLOUD_OK)
	{
		record_put(engine, &record);
		engine_log(engine, "uploaded %s (%zu bytes)%s%s", name, size, reason ? " as a copy: " : "", reason ? reason : "");
	}
	return status;
}

static enum cloud_status unit_upload(struct cloud_engine *engine, const struct unit *unit, const char *expected_tag)
{
	enum cloud_status status;

	base_sending(engine, unit->name, unit->hash);
	status = unit_save(engine, unit, unit->name, expected_tag, NULL);

	if (status == CLOUD_OK)
	{
		base_set(engine, unit->name, unit->hash, record_find(engine, unit->name)->tag);
		engine->uploaded = 1;
	}
	return status;
}

/* The unit's local version as a set-aside copy; done already when a copy of those bytes exists. */
static enum cloud_status unit_copy_local(struct cloud_engine *engine, const struct unit *unit, const char *reason)
{
	char name[CLOUD_RECORD_NAME_SIZE];
	enum cloud_status status;

	copy_name(unit->name, unit->hash, name, sizeof(name));
	if (record_find(engine, name))
		return CLOUD_OK;
	status = unit_save(engine, unit, name, "", reason);
	return status == CLOUD_CHANGED ? CLOUD_OK : status;
}

/* The cloud's current record as a set-aside copy. */
static enum cloud_status unit_copy_cloud(struct cloud_engine *engine, const struct cloud_record *record, const char *reason)
{
	/* (the list the record may be in moves when a record is added to it) */
	const struct cloud_record held = *record, *current = &held;
	struct cloud_record fetched, copy;
	uint8_t *payload = NULL;
	size_t size = 0;
	enum cloud_status status;

	copy_name(current->unit, current->hash, copy.name, sizeof(copy.name));
	if (record_find(engine, copy.name))
		return CLOUD_OK;
	status = engine->store.fetch(engine->store.context, current->name, &fetched, &payload, &size);
	if (status != CLOUD_OK)
		return status;
	if (strcmp(fetched.hash, current->hash) || strcmp(fetched.tag, current->tag))
	{
		free(payload);
		record_put(engine, &fetched);
		return CLOUD_CHANGED;
	}
	copy = fetched;
	copy_name(current->unit, current->hash, copy.name, sizeof(copy.name));
	copy.copy = 1;
	copy.tag[0] = 0;
	meta_set(copy.meta, "state", "live");
	meta_set(copy.meta, "reason", reason);
	{
		char text[32];

		snprintf(text, sizeof(text), "%lld", engine->config.now(engine->config.context));
		meta_set(copy.meta, "copied", text);
	}
	status = engine->store.save(engine->store.context, &copy, payload, size, "", copy.tag, sizeof(copy.tag));
	free(payload);
	if (status == CLOUD_CHANGED)
		status = CLOUD_OK;
	if (status == CLOUD_OK)
	{
		record_put(engine, &copy);
		engine_log(engine, "kept the cloud's %s as %s: %s", current->name, copy.name, reason);
	}
	return status;
}

/* Fetches a record and makes its payload the unit's local files. The local files change only
   between local_write_begin and _end, and only when the payload is whole. */
static enum cloud_status unit_download(struct cloud_engine *engine, const char *slot, int saved_game,
	const struct cloud_record *record)
{
	struct cloud_record fetched;
	struct unit_files files;
	uint8_t *payload = NULL;
	size_t size = 0;
	enum cloud_status status = engine->store.fetch(engine->store.context, record->name, &fetched, &payload, &size);
	char directory[1100];
	int written = 0;

	if (status != CLOUD_OK)
		return status;
	if (strcmp(fetched.hash, record->hash) || strcmp(fetched.tag, record->tag))
	{
		free(payload);
		record_put(engine, &fetched);
		return CLOUD_CHANGED;
	}
	if (!payload_unpack(payload, size, record->hash, &files))
	{
		free(payload);
		engine_log(engine, "%s is damaged in the cloud; the local files stay as they are", record->name);
		return CLOUD_NOT_FOUND;
	}
	free(payload);
	{
		/* the payload must be what the record's name says: one valid saved game of this build's size,
		   or a profile's files with its name file and without a saved game */
		int i, good, has_meta = 0;

		if (saved_game)
			good = files.count == 1 && !strcmp(files.files[0].name, CHECKPOINT_FILE) &&
				cloud_checkpoint_check(files.files[0].data, files.files[0].size) == 1 &&
				(!engine->config.state_size || (long long)files.files[0].size == engine->config.state_size);
		else
		{
			good = 1;
			for (i = 0; i < files.count; i++)
			{
				good &= strcmp(files.files[i].name, CHECKPOINT_FILE) != 0;
				has_meta |= !strcmp(files.files[i].name, META_FILE);
			}
			good &= has_meta;
		}
		if (!good)
		{
			files_free(&files);
			engine_log(engine, "%s does not hold what its name says; the local files stay as they are", record->name);
			return CLOUD_NOT_FOUND;
		}
	}
	if (!engine->config.local_write_begin(engine->config.context))
	{
		files_free(&files);
		return CLOUD_RETRY;
	}
	if (saved_game)
	{
		slot_directory(engine->save_root, slot, directory, sizeof(directory));
		written = write_file(directory, CHECKPOINT_FILE, files.files[0].data, files.files[0].size);
	}
	else
		written = local_write_profile(engine, slot, &files);
	engine->config.local_write_end(engine->config.context);
	files_free(&files);
	if (!written)
	{
		engine_log(engine, "could not write %s to this device (errno %d)", record->name, errno);
		return CLOUD_RETRY;
	}
	base_set(engine, record->unit, record->hash, record->tag);
	engine_log(engine, "downloaded %s to this device", record->name);
	return CLOUD_OK;
}

static void conflict_notice(struct cloud_engine *engine, const struct unit *unit, const struct cloud_summary *cloud,
	int kept_local)
{
	struct cloud_notice notice;

	memset(&notice, 0, sizeof(notice));
	notice.kind = CLOUD_NOTICE_CONFLICT;
	copy_text(notice.unit, sizeof(notice.unit), unit->name);
	notice.kept_local = kept_local;
	notice.kept = kept_local ? *unit->summary : *cloud;
	notice.aside = kept_local ? *cloud : *unit->summary;
	if (!notice.kept.player[0])
		copy_text(notice.kept.player, sizeof(notice.kept.player), notice.aside.player);
	notice_add(engine, &notice);
}

/* The rule table for one unit. removable: whether a deleted cloud record may take the local unit
   with it (a saved game always; a profile, and so its whole slot, only when its saved game is
   nothing the cloud lacks). */
static void unit_sync(struct cloud_engine *engine, struct unit *unit, int removable)
{
	char slot[CLOUD_SLOT_ID_SIZE], key[CLOUD_KEY_SIZE];
	int attempt;

	unit_parse(unit->name, slot, key);
	for (attempt = 0; attempt < 3; attempt++)
	{
		struct cloud_record *found = record_find(engine, unit->name), cloud;
		const char *base;
		struct cloud_summary cloud_summary;
		enum cloud_status status;
		int deleted, verdict;

		if (found && !record_usable(found))
		{
			engine->newer = 1;
			return;
		}
		{
			int sending = base_index(engine, unit->name);

			/* an upload of ours whose reply never came did arrive */
			if (found && sending >= 0 && engine->bases[sending].sent[0] && !strcmp(engine->bases[sending].sent, found->hash) &&
				!record_deleted(found))
				base_set(engine, unit->name, found->hash, found->tag);
			base = base_hash(engine, unit->name);
		}
		if (!found)
		{
			if (!unit->local)
			{
				base_drop(engine, unit->name);
				return;
			}
			/* the cloud has none: this device's version goes up */
			status = unit_upload(engine, unit, "");
			if (status == CLOUD_CHANGED)
			{
				record_refresh(engine, unit->name);
				continue;
			}
			store_ok(engine, status, "uploading", unit->name);
			return;
		}
		cloud = *found;
		deleted = record_deleted(&cloud);
		if (!unit->local)
		{
			if (deleted)
			{
				base_drop(engine, unit->name);
				return;
			}
			/* a new device, a purge, or a saved game this device has none of: restore */
			if (!engine->launch)
			{
				engine->pending = 1;
				return;
			}
			status = unit_download(engine, slot, unit->saved_game, &cloud);
			if (status == CLOUD_CHANGED)
			{
				record_refresh(engine, unit->name);
				continue;
			}
			store_ok(engine, status, "restoring", unit->name);
			return;
		}
		if (!deleted && !strcmp(unit->hash, cloud.hash))
		{
			base_set(engine, unit->name, cloud.hash, cloud.tag);
			return;
		}
		if (deleted)
		{
			if (removable && (!strcmp(unit->hash, base) || !strcmp(unit->hash, cloud.hash)))
			{
				/* deleted elsewhere, and not played here since */
				if (!engine->launch || !engine->config.local_write_begin(engine->config.context))
				{
					engine->pending = 1;
					return;
				}
				if (unit->saved_game)
				{
					char directory[1100], path[1200];

					slot_directory(engine->save_root, slot, directory, sizeof(directory));
					snprintf(path, sizeof(path), "%s/%s", directory, CHECKPOINT_FILE);
					if (unlink(path) == 0)
						engine_log(engine, "removed %s's saved game from this device: its profile was deleted on another", slot);
				}
				else if (local_remove_slot(engine, slot))
					engine_log(engine, "removed %s from this device: it was deleted on another", slot);
				engine->config.local_write_end(engine->config.context);
				base_drop(engine, unit->name);
				return;
			}
			/* this device has something the deleted version is not: it comes back, and the deleted one is kept */
			status = unit_copy_cloud(engine, &cloud, "deleted");
			if (status == CLOUD_OK)
				status = unit_upload(engine, unit, cloud.tag);
			if (status == CLOUD_CHANGED)
			{
				record_refresh(engine, unit->name);
				continue;
			}
			store_ok(engine, status, "uploading", unit->name);
			return;
		}
		if (base[0] && !strcmp(unit->hash, base))
		{
			/* the cloud moved on and this device did not */
			if (!engine->launch)
			{
				engine->pending = 1;
				return;
			}
			status = unit_download(engine, slot, unit->saved_game, &cloud);
			if (status == CLOUD_CHANGED)
			{
				record_refresh(engine, unit->name);
				continue;
			}
			store_ok(engine, status, "downloading", unit->name);
			return;
		}
		if (base[0] && !strcmp(cloud.hash, base))
		{
			/* this device moved on and the cloud did not */
			status = unit_upload(engine, unit, cloud.tag);
			if (status == CLOUD_CHANGED)
			{
				record_refresh(engine, unit->name);
				continue;
			}
			store_ok(engine, status, "uploading", unit->name);
			return;
		}
		/* both changed, or they differ and were never in step: keep the further along, set the other aside */
		summary_from_meta(cloud.meta, &cloud_summary);
		verdict = cloud_summary_compare(unit->summary, &cloud_summary);
		if (!verdict)
			verdict = strcmp(unit->hash, cloud.hash) > 0 ? 1 : -1;
		if (verdict > 0)
		{
			status = unit_copy_cloud(engine, &cloud, "conflict");
			if (status == CLOUD_OK)
				status = unit_upload(engine, unit, cloud.tag);
			if (status == CLOUD_CHANGED)
			{
				record_refresh(engine, unit->name);
				continue;
			}
			if (store_ok(engine, status, "settling", unit->name))
			{
				engine_log(engine, "%s: two devices differed; this device's version was kept, the cloud's set aside", unit->name);
				conflict_notice(engine, unit, &cloud_summary, 1);
				state_save(engine);
			}
			return;
		}
		status = unit_copy_local(engine, unit, "conflict");
		if (!store_ok(engine, status, "setting aside", unit->name))
			return;
		if (!engine->launch)
		{
			/* the copy is safe in the cloud; the local files change at the next launch */
			engine->pending = 1;
			return;
		}
		status = unit_download(engine, slot, unit->saved_game, &cloud);
		if (status == CLOUD_CHANGED)
		{
			record_refresh(engine, unit->name);
			continue;
		}
		if (store_ok(engine, status, "settling", unit->name))
		{
			engine_log(engine, "%s: two devices differed; the cloud's version was kept, this device's set aside", unit->name);
			conflict_notice(engine, unit, &cloud_summary, 0);
			state_save(engine);
		}
		return;
	}
	engine->pending = 1;
}

/* ---------- one slot */

/* marks a record deleted (or live again), its payload kept */
static enum cloud_status record_mark(struct cloud_engine *engine, const struct cloud_record *record, const char *state)
{
	struct cloud_record marked = *record;
	enum cloud_status status;

	meta_set(marked.meta, "state", state);
	meta_set(marked.meta, "markedBy", engine->device_id);
	status = engine->store.save(engine->store.context, &marked, NULL, 0, record->tag, marked.tag, sizeof(marked.tag));
	if (status == CLOUD_OK)
		record_put(engine, &marked);
	return status;
}

/* the slot's saved-game records follow its profile into deletion, or back */
static void saved_games_mark(struct cloud_engine *engine, const char *slot, const char *state)
{
	char prefix[24];
	int i;

	snprintf(prefix, sizeof(prefix), "save-%s-", slot);
	for (i = 0; i < engine->record_count; i++)
	{
		struct cloud_record record = engine->records[i];

		if (!record.copy && !strncmp(record.name, prefix, strlen(prefix)) && record_usable(&record) &&
			record_deleted(&record) != !strcmp(state, "deleted"))
		{
			if (record_mark(engine, &record, state) != CLOUD_OK)
				engine->pending = 1;
			base_drop(engine, record.name);
		}
	}
}

/* The game deleted this slot here: the cloud's record is marked deleted, when it is still the
   version this device knew. */
static void slot_apply_delete(struct cloud_engine *engine, const char *slot)
{
	char unit[CLOUD_RECORD_NAME_SIZE];
	struct cloud_record *record;
	int index = delete_index(engine, slot);
	enum cloud_status status;

	if (index < 0)
		return;
	snprintf(unit, sizeof(unit), "slot-%s", slot);
	record = record_find(engine, unit);
	if (record && record_usable(record) && !record_deleted(record) && !strcmp(record->hash, engine->deletes[index].hash))
	{
		struct cloud_record current = *record;

		status = record_mark(engine, &current, "deleted");
		if (status == CLOUD_RETRY || status == CLOUD_QUOTA)
		{
			/* tried again at the next pass */
			engine->pending = 1;
			return;
		}
		if (status == CLOUD_OK)
		{
			engine_log(engine, "%s was deleted on this device; the cloud keeps it as a deleted copy", slot);
			saved_games_mark(engine, slot, "deleted");
		}
	}
	/* another device changed it meanwhile, or the cloud never had it: nothing to delete there */
	base_drop(engine, unit);
	delete_drop(engine, slot);
}

/* which of the cloud's saved games a slot without one takes: the one this device last had, else one
   written by this same game build, else the newest of this build's size */
static const struct cloud_record *saved_game_to_adopt(struct cloud_engine *engine, const char *slot)
{
	char prefix[24], image[CLOUD_TEXT_SIZE * 2];
	const struct cloud_record *best = NULL;
	long long best_rank = -1;
	const char *known = key_of(engine, slot);
	int i;

	snprintf(prefix, sizeof(prefix), "save-%s-", slot);
	for (i = 0; i < engine->record_count; i++)
	{
		const struct cloud_record *record = &engine->records[i];
		long long rank;

		if (record->copy || strncmp(record->name, prefix, strlen(prefix)) || !record_usable(record) || record_deleted(record))
			continue;
		if (engine->config.state_size && meta_number(record->meta, "stateSize") != engine->config.state_size)
			continue;
		rank = meta_number(record->meta, "modified") & 0xffffffffffLL;
		if (known[0] && !strcmp(record->name + strlen(prefix), known))
			rank += 2LL << 40;
		else if (cloud_meta_get(record->meta, "image", image, sizeof(image)) && engine->image[0] && !strcmp(image, engine->image))
			rank += 1LL << 40;
		if (rank > best_rank)
		{
			best_rank = rank;
			best = record;
		}
	}
	return best;
}

static void slot_sync(struct cloud_engine *engine, const char *id)
{
	struct local_slot local;
	struct unit profile, saved_game;
	char unit_name[CLOUD_RECORD_NAME_SIZE];
	int saved_game_in_step;

	local_read(engine, id, engine->settle, &local);
	if (local.status == LOCAL_BUSY || local.status == LOCAL_FOREIGN)
	{
		if (local.status == LOCAL_BUSY)
			engine->pending = 1;
		else
			engine_log(engine, "%s holds files these rules do not cover; it is left alone", id);
		local_free(&local);
		return;
	}
	if (local.status == LOCAL_ABSENT || delete_index(engine, id) >= 0)
		slot_apply_delete(engine, id);
	if (delete_index(engine, id) >= 0)
	{
		local_free(&local);
		return;
	}

	memset(&profile, 0, sizeof(profile));
	snprintf(profile.name, sizeof(profile.name), "slot-%s", id);
	profile.slot = &local;
	profile.local = local.status == LOCAL_OK;
	profile.hash = local.profile_hash;
	profile.summary = &local.profile_summary;
	/* a deletion elsewhere takes the directory only when its saved game is nothing the cloud lacks */
	snprintf(unit_name, sizeof(unit_name), "save-%s-%s", id, local.saved_game_key);
	saved_game_in_step = local.saved_game == 0 ||
		(local.saved_game == 1 && record_find(engine, unit_name) && !strcmp(record_find(engine, unit_name)->hash, local.saved_game_hash));
	unit_sync(engine, &profile, saved_game_in_step);
	local_free(&local);

	/* the saved game, in a slot that is (still, or now) on this device */
	local_read(engine, id, engine->settle, &local);
	if (local.status == LOCAL_OK && local.saved_game >= 0)
	{
		memset(&saved_game, 0, sizeof(saved_game));
		saved_game.slot = &local;
		saved_game.saved_game = 1;
		saved_game.summary = &local.saved_game_summary;
		if (local.saved_game == 1)
		{
			snprintf(saved_game.name, sizeof(saved_game.name), "save-%s-%s", id, local.saved_game_key);
			saved_game.local = 1;
			saved_game.hash = local.saved_game_hash;
			key_set(engine, id, local.saved_game_key);
			unit_sync(engine, &saved_game, 1);
		}
		else
		{
			const struct cloud_record *adopt = saved_game_to_adopt(engine, id);

			if (adopt)
			{
				copy_text(saved_game.name, sizeof(saved_game.name), adopt->name);
				saved_game.hash = "";
				unit_sync(engine, &saved_game, 0);
			}
		}
	}
	local_free(&local);
}

/* removes the oldest set-aside copies of each unit beyond CLOUD_MAX_COPIES */
static void copies_prune(struct cloud_engine *engine)
{
	int i, j;

	for (i = 0; i < engine->record_count; i++)
	{
		const struct cloud_record *record = &engine->records[i];
		long long copied = meta_number(record->meta, "copied");
		int newer = 0;

		if (!record->copy || !record_usable(record))
			continue;
		for (j = 0; j < engine->record_count; j++)
		{
			const struct cloud_record *other = &engine->records[j];
			long long other_copied = meta_number(other->meta, "copied");

			if (j != i && other->copy && !strcmp(other->unit, record->unit) &&
				(other_copied > copied || (other_copied == copied && strcmp(other->name, record->name) > 0)))
				newer++;
		}
		if (newer >= CLOUD_MAX_COPIES)
		{
			char name[CLOUD_RECORD_NAME_SIZE];

			copy_text(name, sizeof(name), record->name);
			if (engine->store.remove(engine->store.context, name) == CLOUD_OK)
			{
				engine_log(engine, "removed the oldest set-aside copy %s", name);
				record_forget(engine, name);
				i = -1;
			}
		}
	}
}

/* ---------- a pass */

static enum cloud_state pass_end(struct cloud_engine *engine, enum cloud_state state)
{
	engine->state = state;
	free(engine->records);
	engine->records = NULL;
	engine->record_count = 0;
	state_save(engine);
	return state;
}

/* the account, then the cloud's records; 0 with the pass's state set when the pass cannot go on */
static int pass_begin(struct cloud_engine *engine, enum cloud_state *state)
{
	char identifier[256] = "", hash[CLOUD_HASH_SIZE];
	enum cloud_status status;

	engine->pending = engine->quota = engine->newer = engine->failed = engine->uploaded = 0;
	deletes_take(engine);
	if (engine->paused)
	{
		*state = (enum cloud_state)engine->paused;
		return 0;
	}
	status = engine->store.account(engine->store.context, identifier, sizeof(identifier));
	if (status != CLOUD_OK || !identifier[0])
	{
		*state = status == CLOUD_NO_ACCOUNT ? CLOUD_STATE_NO_ACCOUNT : status == CLOUD_RETRY ? CLOUD_STATE_PENDING :
			CLOUD_STATE_FAILED;
		return 0;
	}
	cloud_sha256_hex((const uint8_t *)identifier, strlen(identifier), hash);
	if (!engine->account[0])
	{
		copy_text(engine->account, sizeof(engine->account), hash);
		engine->dirty = 1;
	}
	else if (strcmp(engine->account, hash))
	{
		/* another person's cloud: nothing moves either way until the player says so */
		engine->paused = CLOUD_STATE_ACCOUNT_CHANGED;
		copy_text(engine->pending_account, sizeof(engine->pending_account), hash);
		engine->dirty = 1;
		notice_simple(engine, CLOUD_NOTICE_ACCOUNT_CHANGED);
		engine_log(engine, "the iCloud account is not the one these saves were synced with; waiting for the player");
		*state = CLOUD_STATE_ACCOUNT_CHANGED;
		return 0;
	}
	status = engine->store.list(engine->store.context, &engine->records, &engine->record_count);
	/* a zone that is gone although this device synced with it was deleted, by the player in the
	   system's settings or from another device: the same question either way */
	if (status == CLOUD_ZONE_DELETED || (status == CLOUD_NO_ZONE && engine->base_count))
	{
		/* the player deleted the cloud's copy: nothing is uploaded again without asking */
		bases_forget(engine);
		if (!cloud_local_slot_count(engine->save_root))
		{
			*state = CLOUD_STATE_IDLE;
			return 0;
		}
		engine->paused = CLOUD_STATE_CLOUD_DELETED;
		engine->dirty = 1;
		notice_simple(engine, CLOUD_NOTICE_CLOUD_DELETED);
		engine_log(engine, "the saves were deleted from iCloud; waiting for the player");
		*state = CLOUD_STATE_CLOUD_DELETED;
		return 0;
	}
	if (status == CLOUD_NO_ZONE)
	{
		/* first use */
		status = engine->store.create_zone(engine->store.context);
		engine->records = NULL;
		engine->record_count = 0;
	}
	if (status != CLOUD_OK)
	{
		*state = status == CLOUD_NO_ACCOUNT ? CLOUD_STATE_NO_ACCOUNT : status == CLOUD_FAILED ? CLOUD_STATE_FAILED :
			status == CLOUD_QUOTA ? CLOUD_STATE_QUOTA : CLOUD_STATE_PENDING;
		return 0;
	}
	return 1;
}

static enum cloud_state pass_state(struct cloud_engine *engine)
{
	if (engine->quota)
	{
		if (!engine->quota_noticed)
		{
			engine->quota_noticed = 1;
			notice_simple(engine, CLOUD_NOTICE_QUOTA);
		}
	}
	else if (engine->uploaded && engine->quota_noticed)
	{
		engine->quota_noticed = 0;
		engine->dirty = 1;
	}
	if (engine->newer && !engine->newer_noticed)
	{
		engine->newer_noticed = 1;
		notice_simple(engine, CLOUD_NOTICE_NEWER_FORMAT);
	}
	else if (!engine->newer && engine->newer_noticed)
	{
		engine->newer_noticed = 0;
		engine->dirty = 1;
	}
	return engine->failed ? CLOUD_STATE_FAILED : engine->quota ? CLOUD_STATE_QUOTA : engine->newer ? CLOUD_STATE_NEWER_FORMAT :
		engine->pending ? CLOUD_STATE_PENDING : CLOUD_STATE_IDLE;
}

enum cloud_state cloud_engine_sync(struct cloud_engine *engine, enum cloud_pass launch)
{
	char ids[CLOUD_MAX_SLOTS * 2][CLOUD_SLOT_ID_SIZE];
	char slot[CLOUD_SLOT_ID_SIZE], key[CLOUD_KEY_SIZE];
	enum cloud_state state = CLOUD_STATE_IDLE;
	int count, i, j;

	engine->launch = launch == CLOUD_PASS_LAUNCH;
	/* the game writes its files while it runs, and at no other time */
	engine->settle = launch == CLOUD_PASS_RUNNING;
	if (!pass_begin(engine, &state))
		return pass_end(engine, state);
	/* every slot this device has, the cloud has, or this device remembers */
	count = local_slot_ids(engine->save_root, ids, CLOUD_MAX_SLOTS);
	for (i = 0; i < engine->record_count + engine->base_count + engine->delete_count; i++)
	{
		const char *unit = i < engine->record_count ? engine->records[i].unit :
			i < engine->record_count + engine->base_count ? engine->bases[i - engine->record_count].unit : NULL;

		if (unit)
		{
			if (!unit_parse(unit, slot, key))
			{
				/* a record of a kind this build does not know */
				if (i < engine->record_count && engine->records[i].min_format > CLOUD_FORMAT)
					engine->newer = 1;
				continue;
			}
		}
		else
			copy_text(slot, sizeof(slot), engine->deletes[i - engine->record_count - engine->base_count].slot);
		for (j = 0; j < count; j++)
			if (!strcmp(ids[j], slot))
				break;
		if (j == count && count < CLOUD_MAX_SLOTS * 2)
			copy_text(ids[count++], CLOUD_SLOT_ID_SIZE, slot);
	}
	for (i = 0; i < count; i++)
		slot_sync(engine, ids[i]);
	copies_prune(engine);
	return pass_end(engine, pass_state(engine));
}

enum cloud_state cloud_engine_state(const struct cloud_engine *engine)
{
	return engine->state;
}

int cloud_engine_synced_slots(const struct cloud_engine *engine)
{
	int i, count = 0;

	for (i = 0; i < engine->base_count; i++)
		count += !strncmp(engine->bases[i].unit, "slot-", 5);
	return count;
}

void cloud_note_local_delete(const char *state_path, const char *slot)
{
	char path[1100], line[CLOUD_SLOT_ID_SIZE + 1];
	int fd;

	if (!cloud_slot_id_valid(slot))
		return;
	snprintf(path, sizeof(path), "%s.deleted", state_path);
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return;
	snprintf(line, sizeof(line), "%s\n", slot);
	if (write(fd, line, strlen(line)) == (ssize_t)strlen(line))
		fsync(fd);
	close(fd);
}

static void deletes_take(struct cloud_engine *engine)
{
	char path[1100], taken[1100], line[64];
	FILE *file;

	snprintf(path, sizeof(path), "%s.deleted", engine->state_path);
	snprintf(taken, sizeof(taken), "%s.deleted.taken", engine->state_path);
	/* one left by a pass that ended before the state file was written is read again */
	if (access(taken, F_OK) != 0 && rename(path, taken) != 0)
		return;
	file = fopen(taken, "r");
	while (file && fgets(line, sizeof(line), file))
	{
		line[strcspn(line, "\r\n")] = 0;
		cloud_engine_local_delete(engine, line);
	}
	if (file)
		fclose(file);
	engine->dirty = 1;
	state_save(engine);
	if (!engine->dirty)
		unlink(taken);
}

void cloud_engine_local_delete(struct cloud_engine *engine, const char *slot)
{
	char unit[CLOUD_RECORD_NAME_SIZE];

	if (strlen(slot) != 12 || !cloud_slot_id_valid(slot) || delete_index(engine, slot) >= 0 ||
		engine->delete_count == CLOUD_MAX_SLOTS)
		return;
	snprintf(unit, sizeof(unit), "slot-%s", slot);
	copy_text(engine->deletes[engine->delete_count].slot, CLOUD_SLOT_ID_SIZE, slot);
	copy_text(engine->deletes[engine->delete_count++].hash, CLOUD_HASH_SIZE, base_hash(engine, unit));
	engine->dirty = 1;
	state_save(engine);
	engine_log(engine, "the game deleted %s", slot);
}

void cloud_engine_answer(struct cloud_engine *engine, int carry_on)
{
	int i;

	if (!engine->paused)
		return;
	for (i = 0; i < engine->notice_count; i++)
		if (engine->notices[i].kind == CLOUD_NOTICE_ACCOUNT_CHANGED || engine->notices[i].kind == CLOUD_NOTICE_CLOUD_DELETED)
		{
			memmove(engine->notices + i, engine->notices + i + 1, sizeof(engine->notices[0]) * (size_t)(engine->notice_count - i - 1));
			engine->notice_count--;
			i--;
		}
	if (carry_on)
	{
		bases_forget(engine);
		if (engine->paused == CLOUD_STATE_ACCOUNT_CHANGED)
			copy_text(engine->account, sizeof(engine->account), engine->pending_account);
		else if (engine->store.create_zone(engine->store.context) != CLOUD_OK)
			engine_log(engine, "the zone could not be made now; it is made at the next pass");
		engine->paused = 0;
		engine->pending_account[0] = 0;
		engine_log(engine, "the player chose to sync this device's saves");
	}
	else
		engine_log(engine, "the player chose to keep this device's saves out of iCloud");
	engine->dirty = 1;
	state_save(engine);
}

enum cloud_status cloud_engine_restore_copy(struct cloud_engine *engine, const char *unit_name)
{
	char slot[CLOUD_SLOT_ID_SIZE], key[CLOUD_KEY_SIZE], current_name[CLOUD_RECORD_NAME_SIZE];
	struct cloud_record chosen, *current;
	struct local_slot local;
	struct unit unit;
	enum cloud_state state;
	enum cloud_status status = CLOUD_NOT_FOUND;
	long long best = -1;
	int i, found = 0, saved_game;

	engine->launch = 1;
	engine->settle = 0;
	if (!pass_begin(engine, &state))
	{
		pass_end(engine, state);
		return state == CLOUD_STATE_NO_ACCOUNT ? CLOUD_NO_ACCOUNT : CLOUD_RETRY;
	}
	for (i = 0; i < engine->record_count; i++)
	{
		const struct cloud_record *record = &engine->records[i];
		long long when = meta_number(record->meta, record->copy ? "copied" : "modified");

		if (!record_usable(record) || (!record->copy && !(record_deleted(record) && !strncmp(record->name, "slot-", 5))) ||
			(unit_name && strcmp(record->unit, unit_name)))
			continue;
		if (when > best || !found)
		{
			best = when;
			chosen = *record;
			found = 1;
		}
	}
	if (!found)
	{
		pass_end(engine, CLOUD_STATE_IDLE);
		return CLOUD_NOT_FOUND;
	}
	unit_parse(chosen.unit, slot, key);
	saved_game = key[0] != 0;
	local_read(engine, slot, 0, &local);
	if (local.status == LOCAL_BUSY || local.status == LOCAL_FOREIGN || (saved_game && local.status != LOCAL_OK) ||
		(saved_game && local.saved_game < 0))
	{
		local_free(&local);
		pass_end(engine, CLOUD_STATE_PENDING);
		return CLOUD_FAILED;
	}
	/* what is on this device now must be in the cloud before it is replaced */
	memset(&unit, 0, sizeof(unit));
	unit.slot = &local;
	unit.saved_game = saved_game;
	unit.local = saved_game ? local.saved_game == 1 : local.status == LOCAL_OK;
	unit.hash = saved_game ? local.saved_game_hash : local.profile_hash;
	unit.summary = saved_game ? &local.saved_game_summary : &local.profile_summary;
	if (saved_game)
		snprintf(unit.name, sizeof(unit.name), "save-%s-%s", slot, local.saved_game == 1 ? local.saved_game_key : key);
	else
		snprintf(unit.name, sizeof(unit.name), "slot-%s", slot);
	status = CLOUD_OK;
	if (unit.local && strcmp(unit.hash, chosen.hash))
	{
		struct cloud_record *mine = record_find(engine, unit.name);

		if (!mine || record_deleted(mine) || strcmp(mine->hash, unit.hash))
			status = unit_copy_local(engine, &unit, "swap");
	}
	/* and the cloud's current version of the unit being restored is set aside */
	copy_text(current_name, sizeof(current_name), chosen.unit);
	current = record_find(engine, current_name);
	if (status == CLOUD_OK && chosen.copy && current && record_usable(current) && !record_deleted(current) &&
		strcmp(current->hash, chosen.hash))
		status = unit_copy_cloud(engine, current, "swap");
	if (status == CLOUD_OK)
		status = unit_download(engine, slot, saved_game, &chosen);
	local_free(&local);
	if (status == CLOUD_OK)
	{
		/* the restored bytes become the unit's current record */
		local_read(engine, slot, 0, &local);
		memset(&unit, 0, sizeof(unit));
		copy_text(unit.name, sizeof(unit.name), chosen.unit);
		unit.slot = &local;
		unit.saved_game = saved_game;
		unit.local = 1;
		unit.hash = saved_game ? local.saved_game_hash : local.profile_hash;
		unit.summary = saved_game ? &local.saved_game_summary : &local.profile_summary;
		current = record_find(engine, current_name);
		if (local.status != LOCAL_OK || strcmp(unit.hash, chosen.hash))
			status = CLOUD_FAILED;
		else if (chosen.copy)
		{
			status = unit_upload(engine, &unit, current ? current->tag : "");
			if (status == CLOUD_OK && engine->store.remove(engine->store.context, chosen.name) == CLOUD_OK)
				record_forget(engine, chosen.name);
		}
		else
		{
			/* a deleted profile comes back, with its saved games */
			status = record_mark(engine, &chosen, "live");
			if (status == CLOUD_OK)
			{
				base_set(engine, chosen.unit, chosen.hash, record_find(engine, chosen.unit)->tag);
				saved_games_mark(engine, slot, "live");
			}
		}
		if (saved_game && status == CLOUD_OK)
			key_set(engine, slot, key);
		local_free(&local);
	}
	if (status == CLOUD_OK)
		engine_log(engine, "restored %s", chosen.name);
	else
		engine_log(engine, "could not restore %s now", chosen.name);
	pass_end(engine, status == CLOUD_OK ? CLOUD_STATE_IDLE : CLOUD_STATE_PENDING);
	return status;
}

enum cloud_status cloud_engine_delete_cloud(struct cloud_engine *engine)
{
	enum cloud_status status = engine->store.delete_zone(engine->store.context);

	if (status == CLOUD_OK || status == CLOUD_NO_ZONE || status == CLOUD_ZONE_DELETED)
	{
		bases_forget(engine);
		engine->key_count = 0;
		engine->dirty = 1;
		state_save(engine);
		engine_log(engine, "deleted the saves from iCloud; the saves on this device stay");
		return CLOUD_OK;
	}
	return status;
}

static char *copy_string(const char *text)
{
	return strdup(text ? text : "");
}

struct cloud_engine *cloud_engine_create(const struct cloud_config *config, const struct cloud_store *store)
{
	struct cloud_engine *engine = calloc(1, sizeof(*engine));

	if (!engine)
		return NULL;
	engine->config = *config;
	engine->store = *store;
	engine->save_root = copy_string(config->save_root);
	engine->state_path = copy_string(config->state_path);
	engine->device = copy_string(config->device);
	engine->platform = copy_string(config->platform);
	engine->app = copy_string(config->app);
	engine->image = copy_string(config->image);
	state_load(engine);
	engine->state = engine->paused ? (enum cloud_state)engine->paused : CLOUD_STATE_PENDING;
	state_save(engine);
	return engine;
}

void cloud_engine_destroy(struct cloud_engine *engine)
{
	if (!engine)
		return;
	free(engine->records);
	free(engine->save_root); free(engine->state_path); free(engine->device); free(engine->platform);
	free(engine->app); free(engine->image);
	free(engine);
}
