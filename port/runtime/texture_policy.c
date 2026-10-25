/* The device classifier: step-tools/upscale_lib/policy.py and xbox_map.py in C (texture_policy.h). Keep the two in
step: the plan's Task 6 compares their tables over all 24 maps. */
#include "texture_policy.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "texture_override.h"   /* texture_override_hash: 64-bit FNV-1a */

#define TAG_BASE 0x803A6000u
#define TAG_REGION 0x1600000u
#define POWER_OF_TWO 0x1u
#define LINEAR 0x10u
#define TABLE_SIZE (1u << 17)

struct tag_ref { char *tag; int index; };
struct entry
{
	int has_hash;
	uint64_t hash;
	uint32_t width, height;
	struct tag_ref *tags;
	size_t tag_count;
	struct texture_policy_reference *refs;
	size_t ref_count;
	int type, format, usage, group_type, alpha_tested;
	unsigned flags;
	int out_of_scope;
	enum texture_policy_treatment treatment;
};
struct texture_policy_catalog
{
	struct entry *entries;
	size_t count, capacity;
	unsigned *slots;          /* open addressing: entry index + 1, 0 empty */
};

static const char *const hud_groups[] = { "unhi", "wphi", "grhi", "hudg", "hud#", "DeLa", "mgs2", "vcky", "mply", 0 };
static const char *const effect_groups[] = { "part", "pctl", "cont", "lens", "rain", "elec", "glw!", "ligh", "spla", 0 };
static const char *const color_slots[] = { "env.base", "model.base", "env.self_illumination", "schi", "sotr", "sgla",
	"smet", "swat", "dobc", "ant!", "fog ", 0 };
static const char *const detail_slots[] = { "model.detail", "env.primary_detail", "env.secondary_detail",
	"env.micro_detail", 0 };

/* tag paths are latin-1: policy.py folds case with str.lower(), and these fold ASCII only, whatever the host's locale,
so non-ASCII bytes compare as bytes */
static int ascii_lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int ascii_digit(int c)
{
	return c >= '0' && c <= '9';
}

static int same_text(const char *a, const char *b)
{
	for (; ascii_lower((unsigned char)*a) == ascii_lower((unsigned char)*b); a++, b++)
		if (!*a)
			return 1;
	return 0;
}

static int starts_with(const char *text, const char *prefix)
{
	for (; *prefix; text++, prefix++)
		if (ascii_lower((unsigned char)*text) != ascii_lower((unsigned char)*prefix))
			return 0;
	return 1;
}

static int listed(const char *value, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(value, *list))
			return 1;
	return 0;
}

static int has_slot(const struct entry *e, const char *slot)
{
	size_t i;

	for (i = 0; i < e->ref_count; i++)
		if (!strcmp(e->refs[i].slot, slot))
			return 1;
	return 0;
}

static int any_slot(const struct entry *e, const char *const *list)
{
	size_t i;

	for (i = 0; i < e->ref_count; i++)
		if (listed(e->refs[i].slot, list))
			return 1;
	return 0;
}

static int has_group(const struct entry *e, const char *group)
{
	size_t i;

	for (i = 0; i < e->ref_count; i++)
		if (!strcmp(e->refs[i].group, group))
			return 1;
	return 0;
}

/* every group in the list (or matg), and at least one in the list itself (only_list: no matg needed) */
static int groups_within(const struct entry *e, const char *const *list, int need_one_in_list)
{
	size_t i;
	int one = 0;

	if (!e->ref_count)
		return 0;
	for (i = 0; i < e->ref_count; i++)
	{
		if (listed(e->refs[i].group, list))
			one = 1;
		else if (strcmp(e->refs[i].group, "matg"))
			return 0;
	}
	return need_one_in_list ? one : 1;
}

static int only_group(const struct entry *e, const char *group)
{
	size_t i;

	if (!e->ref_count)
		return 0;
	for (i = 0; i < e->ref_count; i++)
		if (strcmp(e->refs[i].group, group))
			return 0;
	return 1;
}

enum research_class { LIGHTMAP, HUD_UI, CUBE, VOLUME, VECTOR, BUMP, MULTIPURPOSE, DETAIL, EFFECTS, DECAL, COLOR,
	GLOBALS, OTHER };

/* classes.research_class */
static enum research_class research_class(const struct entry *e)
{
	const char *tag = e->tags[0].tag;

	if (e->usage == 4)
		return LIGHTMAP;
	if (!strncmp(tag, "ui\\", 3) || e->group_type == 4 || groups_within(e, hud_groups, 1))
		return HUD_UI;
	if (e->type == 2)
		return CUBE;
	if (e->type == 1)
		return VOLUME;
	if (e->usage == 5)
		return VECTOR;
	if (e->usage == 2 || e->format == 17 || has_slot(e, "env.bump"))
		return BUMP;
	if (has_slot(e, "model.multipurpose") && !has_slot(e, "model.base") && !has_slot(e, "env.base"))
		return MULTIPURPOSE;
	if (e->usage == 3 || any_slot(e, detail_slots))
		return DETAIL;
	if (e->group_type == 3 || groups_within(e, effect_groups, 0))
		return EFFECTS;
	if (only_group(e, "deca"))
		return DECAL;
	if (any_slot(e, color_slots))
		return COLOR;
	if (has_group(e, "deca"))
		return DECAL;
	if (only_group(e, "matg"))
		return GLOBALS;
	return OTHER;
}

/* classes.treatment(bitmap, "none") */
static enum texture_policy_treatment treatment(const struct entry *e)
{
	static const char *const base3[] = { "env.base", "model.base", "env.self_illumination", 0 };
	enum research_class kind = research_class(e);

	if (kind == BUMP)
		return TEXTURE_POLICY_BUMP_MAP;
	if (kind == MULTIPURPOSE)
		return TEXTURE_POLICY_MULTIPURPOSE;
	if (kind != COLOR && kind != DECAL && !any_slot(e, base3))
		return TEXTURE_POLICY_SKIP;
	if (strstr(e->tags[0].tag, "\\fp\\"))
		return TEXTURE_POLICY_FIRST_PERSON;
	if (has_slot(e, "env.self_illumination") && !has_slot(e, "env.base") && !has_slot(e, "model.base"))
		return TEXTURE_POLICY_EMISSIVE;
	return TEXTURE_POLICY_COLOR;
}

static int decodable(int format)
{
	return format == 14 || format == 15 || format == 16 || format == 17;
}

static void rescope(struct entry *e)
{
	e->treatment = treatment(e);
	e->out_of_scope = e->type != 0 || (e->flags & LINEAR) || !(e->flags & POWER_OF_TWO) ||
		e->treatment == TEXTURE_POLICY_SKIP || !decodable(e->format);
}

static uint64_t key_of(const struct texture_policy_bitmap *b)
{
	if (b->has_hash)
		return b->hash ^ ((uint64_t)b->width << 40) ^ ((uint64_t)b->height << 20);
	return texture_override_hash((const unsigned char *)b->tag, strlen(b->tag)) ^ (uint64_t)b->index;
}

static int same_key(const struct entry *e, const struct texture_policy_bitmap *b)
{
	if (e->has_hash != b->has_hash)
		return 0;
	if (b->has_hash)
		return e->hash == b->hash && e->width == b->width && e->height == b->height;
	return e->tags[0].index == b->index && !strcmp(e->tags[0].tag, b->tag);
}

struct texture_policy_catalog *texture_policy_catalog_new(void)
{
	struct texture_policy_catalog *c = calloc(1, sizeof(*c));

	if (c && !(c->slots = calloc(TABLE_SIZE, sizeof(*c->slots))))
	{
		free(c);
		return NULL;
	}
	return c;
}

void texture_policy_catalog_free(struct texture_policy_catalog *c)
{
	size_t i, j;

	if (!c)
		return;
	for (i = 0; i < c->count; i++)
	{
		for (j = 0; j < c->entries[i].tag_count; j++)
			free(c->entries[i].tags[j].tag);
		free(c->entries[i].tags);
		free(c->entries[i].refs);
	}
	free(c->entries);
	free(c->slots);
	free(c);
}

static int same_ref(const struct texture_policy_reference *a, const struct texture_policy_reference *b)
{
	return !strcmp(a->group, b->group) && !strcmp(a->slot, b->slot) && !strcmp(a->shader, b->shader);
}

/* one more reference at the end of *refs; 0 on success */
static int push_ref(struct texture_policy_reference **refs, size_t *count, const struct texture_policy_reference *ref)
{
	struct texture_policy_reference *grown = realloc(*refs, (*count + 1) * sizeof(*grown));

	if (!grown)
		return -1;
	*refs = grown;
	(*refs)[(*count)++] = *ref;
	return 0;
}

int texture_policy_add_bitmap(struct texture_policy_catalog *c, const struct texture_policy_bitmap *b)
{
	uint64_t key = key_of(b);
	unsigned slot = (unsigned)(key ^ (key >> 29)) & (TABLE_SIZE - 1);
	struct entry *e = NULL;
	size_t i, j;

	while (c->slots[slot])
	{
		if (same_key(&c->entries[c->slots[slot] - 1], b))
		{
			e = &c->entries[c->slots[slot] - 1];
			break;
		}
		slot = (slot + 1) & (TABLE_SIZE - 1);
	}
	if (!e)
	{
		if (c->count * 4 >= (size_t)TABLE_SIZE * 3)
			return -1;
		if (c->count == c->capacity)
		{
			size_t capacity = c->capacity ? c->capacity * 2 : 1024;
			struct entry *grown = realloc(c->entries, capacity * sizeof(*grown));

			if (!grown)
				return -1;
			c->entries = grown;
			c->capacity = capacity;
		}
		e = &c->entries[c->count++];
		memset(e, 0, sizeof(*e));
		e->has_hash = b->has_hash; e->hash = b->hash; e->width = b->width; e->height = b->height;
		e->type = b->type; e->format = b->format; e->usage = b->usage; e->group_type = b->group_type;
		e->flags = b->flags;
		c->slots[slot] = (unsigned)c->count;
	}
	e->alpha_tested |= b->alpha_tested;
	for (i = 0; i < e->tag_count; i++)
		if (e->tags[i].index == b->index && !strcmp(e->tags[i].tag, b->tag))
			break;
	if (i == e->tag_count)
	{
		struct tag_ref *grown = realloc(e->tags, (e->tag_count + 1) * sizeof(*grown));

		if (!grown)
			return -1;
		e->tags = grown;   /* realloc may have freed the old block: keep the new one before anything else can fail */
		if (!(e->tags[e->tag_count].tag = strdup(b->tag)))
			return -1;
		e->tags[e->tag_count].index = b->index;
		e->tag_count++;
	}
	for (j = 0; j < b->ref_count; j++)
	{
		for (i = 0; i < e->ref_count; i++)
			if (same_ref(&e->refs[i], &b->refs[j]))
				break;
		if (i == e->ref_count && push_ref(&e->refs, &e->ref_count, &b->refs[j]))
			return -1;
	}
	rescope(e);
	return 0;
}

/* ---------- reading a map (xbox_map.py) */

static uint32_t u32(const unsigned char *d, size_t o) { return d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; }
static int s16(const unsigned char *d, size_t o) { return (int16_t)(d[o] | d[o + 1] << 8); }
static unsigned u16(const unsigned char *d, size_t o) { return d[o] | d[o + 1] << 8; }

struct map_tag { char group[5]; uint32_t id, pointer; const char *name; };
struct holder { uint32_t bitmap; int alpha_tested; struct texture_policy_reference ref; };   /* alpha_tested: a senv with its flag holds it as env.base */

static int by_pointer(const void *a, const void *b)
{
	uint32_t x = (*(const struct map_tag *const *)a)->pointer, y = (*(const struct map_tag *const *)b)->pointer;

	return x < y ? -1 : x > y;
}

static const char *slot_name(const char *group, size_t position, char *buffer, size_t size)
{
	static const struct { const char *group; size_t offset; const char *slot; } slots[] = {
		{ "soso", 0xA4, "model.base" }, { "soso", 0xBC, "model.multipurpose" }, { "soso", 0xDC, "model.detail" },
		{ "soso", 0x164, "model.reflection_cube" }, { "senv", 0x88, "env.base" },
		{ "senv", 0xB8, "env.primary_detail" }, { "senv", 0xCC, "env.secondary_detail" },
		{ "senv", 0xFC, "env.micro_detail" }, { "senv", 0x128, "env.bump" },
		{ "senv", 0x254, "env.self_illumination" }, { "senv", 0x324, "env.reflection_cube" } };
	size_t i;

	if (strcmp(group, "soso") && strcmp(group, "senv"))
		return group;
	for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
		if (!strcmp(slots[i].group, group) && slots[i].offset == position)
			return slots[i].slot;
	snprintf(buffer, size, "%s@%zx", group, position);
	return buffer;
}

static size_t level0_bytes(int format, uint32_t width, uint32_t height)
{
	width = width ? width : 1;     /* texels.level_dims: at least one texel */
	height = height ? height : 1;
	switch (format)
	{
	case 14: return ((width + 3) / 4) * ((height + 3) / 4) * 8;
	case 15: case 16: return ((width + 3) / 4) * ((height + 3) / 4) * 16;
	case 0: case 1: case 2: case 17: return (size_t)width * height;
	case 3: case 6: case 8: case 9: return (size_t)width * height * 2;
	case 10: case 11: return (size_t)width * height * 4;
	default: return 0;
	}
}

int texture_policy_add_map(struct texture_policy_catalog *c, const unsigned char *data, size_t size, char *error,
	size_t error_size)
{
	uint32_t tag_data, instances, count, i;
	struct map_tag *tags = NULL, **placed = NULL;
	struct holder *holders = NULL;
	size_t holder_count = 0, holder_capacity = 0, placed_count = 0, k;
	int result = -1;

#define OFF(p) ((size_t)tag_data + ((size_t)(uint32_t)(p) - TAG_BASE))
#define FAIL(message) do { snprintf(error, error_size, "%s", message); goto done; } while (0)
#define NEED(o, n) do { if ((size_t)(o) > size || (size_t)(n) > size - (size_t)(o)) FAIL("map: offset past the end"); } while (0)
	if (error_size)
		error[0] = 0;
	if (size < 0x800 || u32(data, 4) != 5)
	{
		snprintf(error, error_size, "not an Xbox map (cache version 5)");
		return -1;
	}
	tag_data = u32(data, 0x10);
	NEED(tag_data, 16);
	instances = u32(data, tag_data);
	count = u32(data, tag_data + 12);
	if (!(tags = calloc(count ? count : 1, sizeof(*tags))) || !(placed = calloc(count ? count : 1, sizeof(*placed))))
		FAIL("out of memory");
	for (i = 0; i < count; i++)
	{
		size_t entry = OFF(instances) + (size_t)i * 0x20, name_at;

		NEED(entry, 0x20);
		tags[i].group[0] = (char)data[entry + 3]; tags[i].group[1] = (char)data[entry + 2];
		tags[i].group[2] = (char)data[entry + 1]; tags[i].group[3] = (char)data[entry];
		tags[i].id = u32(data, entry + 0xC);
		name_at = OFF(u32(data, entry + 0x10));
		NEED(name_at, 1);
		if (!memchr(data + name_at, 0, size - name_at))
			FAIL("map: a tag name runs past the end");
		tags[i].name = (const char *)data + name_at;
		tags[i].pointer = u32(data, entry + 0x14);
		if (tags[i].pointer >= TAG_BASE && tags[i].pointer < TAG_BASE + TAG_REGION)
			placed[placed_count++] = &tags[i];
	}
	qsort(placed, placed_count, sizeof(*placed), by_pointer);
	for (k = 0; k < placed_count; k++)   /* xbox_map._references */
	{
		const struct map_tag *t = placed[k];
		size_t start = OFF(t->pointer), end, position;
		int alpha_tested;

		if (!strcmp(t->group, "bitm") || start >= size)
			continue;
		end = k + 1 < placed_count ? OFF(placed[k + 1]->pointer) : start + 0x100000;
		if (end > size)
			end = size;
		if (end <= start)
			continue;
		alpha_tested = !strcmp(t->group, "senv") && end - start > 0x2A && (u16(data, start + 0x28) & 1);
		for (position = 0; position + 4 <= end - start; position++)
		{
			const unsigned char *at = data + start + position;
			uint32_t target, j;

			if (memcmp(at, "mtib", 4) || position + 16 > end - start)
				continue;
			target = u32(data, start + position + 12);
			for (j = 0; j < count; j++)
				if (tags[j].id == target && !strcmp(tags[j].group, "bitm"))
					break;
			if (j == count)
				continue;
			if (holder_count == holder_capacity)
			{
				size_t capacity = holder_capacity ? holder_capacity * 2 : 4096;
				struct holder *grown = realloc(holders, capacity * sizeof(*grown));

				if (!grown)
					FAIL("out of memory");
				holders = grown;
				holder_capacity = capacity;
			}
			{
				struct holder *h = &holders[holder_count++];
				char buffer[40];

				memset(h, 0, sizeof(*h));
				h->bitmap = target;
				snprintf(h->ref.group, sizeof(h->ref.group), "%s", t->group);
				snprintf(h->ref.slot, sizeof(h->ref.slot), "%s", slot_name(t->group, position, buffer, sizeof(buffer)));
				snprintf(h->ref.shader, sizeof(h->ref.shader), "%s", t->name);
				h->alpha_tested = alpha_tested && !strcmp(h->ref.slot, "env.base");
			}
		}
	}
	for (i = 0; i < count; i++)   /* xbox_map.bitmaps */
	{
		size_t at, n, index;
		uint32_t bitmap_count, array;
		struct texture_policy_reference *refs = NULL;
		size_t ref_count = 0;
		int tested = 0;

		if (strcmp(tags[i].group, "bitm"))
			continue;
		at = OFF(tags[i].pointer);
		NEED(at, 0x68);
		for (n = 0; n < holder_count; n++)
		{
			if (holders[n].bitmap != tags[i].id)
				continue;
			if (push_ref(&refs, &ref_count, &holders[n].ref))
			{
				free(refs);
				FAIL("out of memory");
			}
			tested |= holders[n].alpha_tested;
		}
		bitmap_count = u32(data, at + 0x60);
		array = u32(data, at + 0x64);
		for (index = 0; index < bitmap_count; index++)
		{
			size_t b = OFF(array) + index * 0x30;
			struct texture_policy_bitmap bitmap;

			NEED(b, 0x30);
			memset(&bitmap, 0, sizeof(bitmap));
			bitmap.tag = tags[i].name;
			bitmap.index = (int)index;
			bitmap.width = (uint32_t)s16(data, b + 4);
			bitmap.height = (uint32_t)s16(data, b + 6);
			bitmap.type = s16(data, b + 10);
			bitmap.format = s16(data, b + 12);
			bitmap.flags = u16(data, b + 14);
			bitmap.usage = s16(data, at + 4);
			bitmap.group_type = s16(data, at);
			bitmap.alpha_tested = tested;
			bitmap.refs = refs;
			bitmap.ref_count = ref_count;
			if (bitmap.type == 0 && !(bitmap.flags & LINEAR))
			{
				int32_t pixels = (int32_t)u32(data, b + 0x18);
				size_t bytes = level0_bytes(bitmap.format, bitmap.width, bitmap.height);

				/* texels.override_hash slices data[pixels:pixels + bytes], which the end of the map cuts short */
				if (bytes && pixels >= 0)
				{
					size_t start = (size_t)pixels < size ? (size_t)pixels : size;

					bitmap.has_hash = 1;
					bitmap.hash = texture_override_hash(data + start, bytes < size - start ? bytes : size - start);
				}
			}
			if (texture_policy_add_bitmap(c, &bitmap))
			{
				free(refs);
				snprintf(error, error_size, "catalog full or out of memory");
				goto done;
			}
		}
		free(refs);
	}
	result = 0;
done:
	free(tags);
	free(placed);
	free(holders);
	return result;
#undef OFF
#undef FAIL
#undef NEED
}

int texture_policy_add_map_file(struct texture_policy_catalog *c, const char *path, char *error, size_t error_size)
{
	FILE *file = fopen(path, "rb");
	unsigned char *raw = NULL, *out = NULL;
	long length;
	size_t capacity;
	z_stream z;
	int status, result = -1;

	if (error_size)
		error[0] = 0;
	if (!file)
	{
		snprintf(error, error_size, "%s: cannot open", path);
		return -1;
	}
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (length < 0x800 || !(raw = malloc((size_t)length)) || fread(raw, 1, (size_t)length, file) != (size_t)length)
	{
		snprintf(error, error_size, "%s: cannot read", path);
		goto done;
	}
	capacity = (size_t)length * 4;
	if (!(out = malloc(capacity)))
	{
		snprintf(error, error_size, "%s: out of memory", path);
		goto done;
	}
	memcpy(out, raw, 0x800);
	memset(&z, 0, sizeof(z));
	if (inflateInit(&z) != Z_OK)
	{
		snprintf(error, error_size, "%s: inflateInit failed", path);
		goto done;
	}
	z.next_in = raw + 0x800;
	z.avail_in = (uInt)(length - 0x800);
	z.next_out = out + 0x800;
	z.avail_out = (uInt)(capacity - 0x800);
	while ((status = inflate(&z, Z_NO_FLUSH)) == Z_OK || (status == Z_BUF_ERROR && !z.avail_out))
	{
		if (!z.avail_out)
		{
			size_t used = 0x800 + z.total_out;
			unsigned char *grown = realloc(out, capacity * 2);

			if (!grown)
				break;
			out = grown;
			capacity *= 2;
			z.next_out = out + used;
			z.avail_out = (uInt)(capacity - used);
		}
	}
	inflateEnd(&z);
	if (status != Z_STREAM_END)
	{
		snprintf(error, error_size, "%s: inflate failed (%d)", path, status);
		goto done;
	}
	result = texture_policy_add_map(c, out, 0x800 + z.total_out, error, error_size);
done:
	fclose(file);
	free(raw);
	free(out);
	return result;
}

/* ---------- classifying (policy.classify) */

/* a token of the tag's last path component (split on ' ', '_' and '-', ASCII-lowercased, trailing digits dropped) is
in the list: policy.name_tokens */
static int token_in(const char *tag, const char *const *list, size_t count)
{
	const char *name = strrchr(tag, '\\');
	char token[128];
	size_t length = 0, i;

	name = name ? name + 1 : tag;
	for (;; name++)
	{
		if (!*name || *name == ' ' || *name == '_' || *name == '-')
		{
			while (length && ascii_digit((unsigned char)token[length - 1]))
				length--;
			token[length] = 0;
			for (i = 0; length && i < count; i++)
				if (!strcmp(token, list[i]))
					return 1;
			length = 0;
			if (!*name)
				return 0;
			continue;
		}
		if (length + 1 < sizeof(token))
			token[length++] = (char)ascii_lower((unsigned char)*name);
	}
}

static int graphic(const struct entry *e, const struct texture_policy_table *t)
{
	size_t i, j;

	if (has_group(e, "deca"))
		for (i = 0; i < e->tag_count; i++)
		{
			int surface = 0;

			for (j = 0; j < t->decal_surface_prefix_count; j++)
				if (starts_with(e->tags[i].tag, t->decal_surface_prefixes[j]))
					surface = 1;
			if (!surface)
				return 1;
		}
	for (i = 0; i < t->graphic_group_count; i++)
		if (has_group(e, t->graphic_groups[i]))
			return 1;
	for (i = 0; i < e->tag_count; i++)
		if (token_in(e->tags[i].tag, t->graphic_names, t->graphic_name_count))
			return 1;
	return 0;
}

/* the material rule's signal (policy._materials) */
static int material(const struct entry *e, const struct texture_policy_table *t)
{
	size_t i;

	for (i = 0; i < e->tag_count; i++)
		if (token_in(e->tags[i].tag, t->material_names, t->material_name_count))
			return 1;
	return 0;
}

static int pin_rank(enum texture_policy_result result)
{
	return result == TEXTURE_POLICY_ORIGINAL ? 0 : result == TEXTURE_POLICY_BPF ? 1 : 2;
}

__attribute__((format(printf, 3, 4)))
static void say(texture_policy_log_proc log, void *context, const char *format, ...)
{
	char line[512];
	va_list arguments;

	if (!log)
		return;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	log(context, line);
}

static int by_key(const void *a, const void *b)
{
	const struct texture_policy_decision *x = a, *y = b;

	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	if (x->width != y->width)
		return x->width < y->width ? -1 : 1;
	return x->height < y->height ? -1 : x->height > y->height;
}

size_t texture_policy_classify(struct texture_policy_catalog *c, const struct texture_policy_table *t,
	struct texture_policy_decision **out, texture_policy_log_proc log, void *context)
{
	const struct texture_policy_override **pins = calloc(c->count ? c->count : 1, sizeof(*pins));
	enum texture_policy_result *results = calloc(c->count ? c->count : 1, sizeof(*results));
	enum texture_policy_kind *kinds = calloc(c->count ? c->count : 1, sizeof(*kinds));
	char *decided = calloc(c->count ? c->count : 1, 1);
	size_t i, j, k, m, n = 0;

	*out = NULL;
	if (!pins || !results || !kinds || !decided)
		goto done;
	for (k = 0; k < t->override_count; k++)   /* rule 2's table: _pins */
	{
		const struct texture_policy_override *o = &t->overrides[k];
		int matched = 0;

		for (i = 0; i < c->count; i++)
			for (j = 0; j < c->entries[i].tag_count; j++)
			{
				const struct tag_ref *r = &c->entries[i].tags[j];

				if (!same_text(r->tag, o->tag) || (o->index >= 0 && r->index != o->index))
					continue;
				matched = 1;
				if (o->hash && (!c->entries[i].has_hash || strtoull(o->hash, NULL, 16) != c->entries[i].hash))
				{
					char digest[17] = "-";   /* policy.py prints a key's first field: "-" for one keyed by tag */

					if (c->entries[i].has_hash)
						snprintf(digest, sizeof(digest), "%016llx", (unsigned long long)c->entries[i].hash);
					say(log, context, "override %s [%d]: this disc's bitmap hashes to %s, not %s; the pin "
						"applies by tag path", o->tag, r->index, digest, o->hash);
				}
				if (!pins[i] || pin_rank(o->result) < pin_rank(pins[i]->result))
					pins[i] = o;
			}
		if (!matched && o->index >= 0)
			say(log, context, "override %s [%d]: matches no bitmap", o->tag, o->index);
		else if (!matched)
			say(log, context, "override %s: matches no bitmap", o->tag);
	}
	for (i = 0; i < c->count; i++)   /* rules 1, 2, 5, 6 */
	{
		const struct entry *e = &c->entries[i];
		int companion = e->treatment == TEXTURE_POLICY_EMISSIVE || e->treatment == TEXTURE_POLICY_MULTIPURPOSE;

		if (e->out_of_scope)
			results[i] = TEXTURE_POLICY_ORIGINAL, kinds[i] = TEXTURE_POLICY_KIND_ORIGINAL;
		else if (pins[i])
		{
			results[i] = pins[i]->result;
			kinds[i] = results[i] == TEXTURE_POLICY_ORIGINAL ? TEXTURE_POLICY_KIND_ORIGINAL :
				e->treatment == TEXTURE_POLICY_BUMP_MAP ? TEXTURE_POLICY_BUMP :
				companion ? TEXTURE_POLICY_COMPANION :
				results[i] == TEXTURE_POLICY_S4G ? TEXTURE_POLICY_GRAPHIC : TEXTURE_POLICY_SURFACE;
		}
		else if (e->treatment == TEXTURE_POLICY_BUMP_MAP || companion)
			continue;
		else if (graphic(e, t))
			results[i] = t->graphic, kinds[i] = TEXTURE_POLICY_GRAPHIC;
		else
			results[i] = material(e, t) ? t->material_result : t->surface, kinds[i] = TEXTURE_POLICY_SURFACE;
		decided[i] = 1;
	}
	for (i = 0; i < c->count; i++)   /* rules 3 and 4: an original base map in any shader that uses it */
	{
		const struct entry *e = &c->entries[i];
		int original = 0;

		if (decided[i])
			continue;
		for (j = 0; j < e->ref_count && !original; j++)
			for (m = 0; m < c->count && !original; m++)
			{
				const struct entry *base = &c->entries[m];

				if (m == i || !decided[m] || results[m] != TEXTURE_POLICY_ORIGINAL)
					continue;
				for (k = 0; k < base->ref_count; k++)
					if ((!strcmp(base->refs[k].slot, "env.base") || !strcmp(base->refs[k].slot, "model.base")) &&
						same_text(base->refs[k].shader, e->refs[j].shader))
						original = 1;
			}
		if (original)
			results[i] = TEXTURE_POLICY_ORIGINAL, kinds[i] = TEXTURE_POLICY_KIND_ORIGINAL;
		else if (e->treatment == TEXTURE_POLICY_BUMP_MAP)
			results[i] = material(e, t) ? t->material_result : t->bump, kinds[i] = TEXTURE_POLICY_BUMP;
		else
			results[i] = t->companion, kinds[i] = TEXTURE_POLICY_COMPANION;
		decided[i] = 1;
	}
	if (!(*out = calloc(c->count ? c->count : 1, sizeof(**out))))
		goto done;
	for (i = 0; i < c->count; i++)
		if (c->entries[i].has_hash)
		{
			const struct entry *e = &c->entries[i];
			struct texture_policy_decision *d = &(*out)[n++];
			const char **tags = malloc((e->tag_count ? e->tag_count : 1) * sizeof(*tags));

			d->hash = e->hash; d->width = e->width; d->height = e->height;
			d->kind = kinds[i]; d->result = results[i]; d->treatment = e->treatment;
			enum research_class cls = research_class(e);
			d->wrap = cls != DECAL && cls != EFFECTS && cls != HUD_UI;
			d->dilates = e->alpha_tested || cls == DECAL || cls == EFFECTS;
			d->pinned = pins[i] != NULL;
			for (j = 0; tags && j < e->tag_count; j++)
				tags[j] = e->tags[j].tag;
			d->scale = texture_policy_scale(t, d->kind, d->result, d->treatment, d->hash, d->width, d->height,
				tags, tags ? e->tag_count : 0);
			free(tags);
		}
	qsort(*out, n, sizeof(**out), by_key);
done:
	free(pins);
	free(results);
	free(kinds);
	free(decided);
	return n;
}

uint32_t texture_policy_scale(const struct texture_policy_table *t, enum texture_policy_kind kind,
	enum texture_policy_result result, enum texture_policy_treatment treatment, uint64_t hash, uint32_t width,
	uint32_t height, const char *const *tags, size_t tag_count)
{
	uint32_t scale = treatment == TEXTURE_POLICY_FIRST_PERSON ? t->scale_first_person : t->scale_base;
	int doubled = (t->doubled_kinds >> kind & 1u) || (t->doubled_treatments >> treatment & 1u);
	size_t i, j;

	if (result == TEXTURE_POLICY_ORIGINAL || kind == TEXTURE_POLICY_KIND_ORIGINAL)
		return 1;
	for (i = 0; i < t->doubled_texture_count && !doubled; i++)
		doubled = t->doubled_textures[i].hash == hash && t->doubled_textures[i].width == width &&
			t->doubled_textures[i].height == height;
	for (i = 0; i < t->doubled_tag_count && !doubled; i++)
		for (j = 0; j < tag_count && !doubled; j++)
			doubled = same_text(tags[j], t->doubled_tags[i]);
	return doubled ? scale * 2 : scale;
}

const char *texture_policy_result_name(enum texture_policy_result result)
{
	static const char *const names[] = { "s4g", "bpf", "original" };

	return (unsigned)result < 3 ? names[result] : "?";
}

const char *texture_policy_kind_name(enum texture_policy_kind kind)
{
	static const char *const names[] = { "graphic", "surface", "companion", "bump", "original" };

	return (unsigned)kind < 5 ? names[kind] : "?";
}
