/* The per-texture upscale policy on the device (spec 2026-10-04-halo-ce-ios-texture-upscale-policy-design.md):
the same rules as step-tools/upscale_lib/policy.py, over the imported maps, keyed by (level-0 hash, width, height)
as the runtime override lookup is. Host-side C; the import pipeline (piece 3) calls it. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum texture_policy_result { TEXTURE_POLICY_S4G, TEXTURE_POLICY_BPF, TEXTURE_POLICY_ORIGINAL };
enum texture_policy_kind
{
	TEXTURE_POLICY_GRAPHIC, TEXTURE_POLICY_SURFACE, TEXTURE_POLICY_COMPANION, TEXTURE_POLICY_BUMP,
	TEXTURE_POLICY_KIND_ORIGINAL
};
/* classes.treatment with alpha folded (cutout is color) */
enum texture_policy_treatment
{
	TEXTURE_POLICY_SKIP, TEXTURE_POLICY_COLOR, TEXTURE_POLICY_FIRST_PERSON, TEXTURE_POLICY_EMISSIVE,
	TEXTURE_POLICY_MULTIPURPOSE, TEXTURE_POLICY_BUMP_MAP
};

struct texture_policy_override
{
	const char *tag;
	int index;        /* -1: every bitmap of the tag */
	const char *hash; /* NULL, or the retail disc's 16 hex digits */
	enum texture_policy_result result;
};

/* one texture by its key, "HASH-WIDTHxHEIGHT" in the policy file */
struct texture_policy_key
{
	uint64_t hash;
	uint32_t width, height;
};

struct texture_policy_table
{
	int schema, policy_version;
	enum texture_policy_result graphic, surface, companion, bump;
	float low, high, sigma;
	int iterations;
	const char *const *graphic_names;
	size_t graphic_name_count;
	const char *const *graphic_groups;
	size_t graphic_group_count;
	const char *const *decal_surface_prefixes;
	size_t decal_surface_prefix_count;
	const struct texture_policy_override *overrides;
	size_t override_count;
	/* The damage rule (design 2026-10-07-halo-ce-ios-texture-upscale-damage-rule-design.md): a classifier-S4g texture
	(a graphic not pinned by an override) falls back to bpf when its S4g is damaged, damage_share >= this share or,
	when enabled, structure_loss(S4g) >= damage_structure_loss; and to the original when, enabled, bpf loses markedly
	more structure than a plain resize, structure_loss(bpf) - structure_loss(resize) >= damage_bpf_structure_loss.
	A negative value is a test turned off (null in the policy file). damage_measure_version is the measures'
	definition version. The Mac reference is step-tools/upscale_lib/damage.py (damage_share, structure_loss) and its
	policy/final.tsv; the device's measure (piece 3, a GPU reduction beside the bpf pass) must match damage_share
	within 0.005, and its tests cover every key within 0.02 of the threshold. Until piece 3, the device carries only
	these parameters (in the cache recipe key) and the table's results stay the classifier's. */
	float damage_share, damage_structure_loss, damage_bpf_structure_loss;
	int damage_measure_version;
	/* The material rule: a surface or bump (never a graphic, companion or original; a bump whose shader's base map
	is original stays original) with a name token in material_names, tokenized as graphic_names is, gets
	material_result. Overrides still win. */
	const char *const *material_names;
	size_t material_name_count;
	enum texture_policy_result material_result;
	/* The scale rule ("scales"): an upscaled texture is scale_base times the original's size, scale_first_person
	for the first-person treatment (its Mac base is already 4x), and twice that when doubled: its kind's bit is in
	doubled_kinds (1 << enum texture_policy_kind), its treatment's in doubled_treatments (1 << enum
	texture_policy_treatment), one of its tags is in doubled_tags, or its key is in doubled_textures. The shipping
	selection (step-tools texture-resolution-set.py compose, sets/policy-game-selective-round3) is locked to it by
	port/assets/texture-selection-lock.tsv. */
	uint32_t scale_base, scale_first_person;
	unsigned doubled_kinds, doubled_treatments;
	const char *const *doubled_tags;
	size_t doubled_tag_count;
	const struct texture_policy_key *doubled_textures;
	size_t doubled_texture_count;
};
/* port/assets/texture-policy.json, by tools/embed_texture_policy.py */
extern const struct texture_policy_table texture_policy_embedded;

struct texture_policy_reference
{
	char group[5];   /* the referencing tag's group, e.g. "senv" */
	char slot[40];   /* "env.base", "model.multipurpose", "deca", ... as xbox_map.SLOTS names them */
	char shader[256];/* the referencing tag's path */
};

/* one bitmap of one map, as xbox_map.bitmaps reads it */
struct texture_policy_bitmap
{
	const char *tag;
	int index;
	int has_hash;    /* a 2D, non-linear texture: keyed by its level-0 hash */
	uint64_t hash;
	uint32_t width, height;
	int type;        /* 0 2d, 1 3d, 2 cube, 3 white */
	int format;      /* the bitmap format code (14 dxt1, 15 dxt3, 16 dxt5, 17 p8 bump, ...) */
	unsigned flags;  /* 0x1 power of two, 0x10 linear */
	int usage;       /* 2 height map, 3 detail map, 4 light map, 5 vector map */
	int group_type;  /* 3 sprites, 4 interface bitmaps */
	int alpha_tested;
	const struct texture_policy_reference *refs;
	size_t ref_count;
};

struct texture_policy_decision
{
	uint64_t hash;
	uint32_t width, height;
	enum texture_policy_kind kind;
	enum texture_policy_result result;
	enum texture_policy_treatment treatment;
	int wrap, dilates, pinned; /* addressing, coverage dilation (binary alpha also dilates), explicit override */
	uint32_t scale;  /* the replacement's size over the original's: 2, 4 or 8; 1 for an original */
};

typedef void (*texture_policy_log_proc)(void *context, const char *line);
struct texture_policy_catalog;

struct texture_policy_catalog *texture_policy_catalog_new(void);
void texture_policy_catalog_free(struct texture_policy_catalog *catalog);
/* 0 on success; maps must be added in sorted name order, as the Mac tool does, so first-seen tags agree */
int texture_policy_add_bitmap(struct texture_policy_catalog *catalog, const struct texture_policy_bitmap *bitmap);
/* a cache-version-5 map already inflated (0x800-byte header, then the zlib body inflated) */
int texture_policy_add_map(struct texture_policy_catalog *catalog, const unsigned char *data, size_t size,
	char *error, size_t error_size);
/* reads and inflates a .map file, then texture_policy_add_map */
int texture_policy_add_map_file(struct texture_policy_catalog *catalog, const char *path, char *error,
	size_t error_size);
/* the decisions for every keyed bitmap, sorted by hash, width and height; free() *out. log may be NULL */
size_t texture_policy_classify(struct texture_policy_catalog *catalog, const struct texture_policy_table *table,
	struct texture_policy_decision **out, texture_policy_log_proc log, void *log_context);
/* the scale rule (texture_policy_table.scale_base) for one texture: 1 for an original, else 2, 4 or 8. tags are the
tag paths that reference it */
uint32_t texture_policy_scale(const struct texture_policy_table *table, enum texture_policy_kind kind,
	enum texture_policy_result result, enum texture_policy_treatment treatment, uint64_t hash, uint32_t width,
	uint32_t height, const char *const *tags, size_t tag_count);
const char *texture_policy_result_name(enum texture_policy_result result);
const char *texture_policy_kind_name(enum texture_policy_kind kind);
