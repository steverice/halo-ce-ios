/* The upscale cache's recipe key, the action for each texture after an app update, and the manifest (spec
2026-10-04-halo-ce-ios-texture-upscale-policy-design.md, "Cache and versioning"). The cache never keys on
`policy_version`: a policy edit changes only the textures whose result changes. Host-side C; the import pipeline
(piece 3) calls it. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "texture_policy.h"

struct texture_recipe_globals
{
	const char *model_id;              /* "S4g: PBRify SPAN V4 per channel" */
	const char *model_sha256;          /* the weights' SHA-256, hex */
	uint32_t pipeline_version[6];      /* by enum texture_policy_treatment; raised when that path's code changes */
	float low, high, sigma;            /* bpf only */
	uint32_t iterations, post_version; /* bpf only: the bp and bpf kernels' version */
	const char *encoder;               /* piece 2's encoding and its settings, e.g. "rgba8" until then */
	/* classifier-S4g only: the policy's damage block (texture_policy_table; negative: a test turned off) and its
	measures' version. An entry whose classifier result is S4g may hold S4g, bpf or (later) original bytes, so its
	global key carries these and the bpf parameters above: a change to any of them redoes every such entry. */
	float damage_share, damage_structure_loss, damage_bpf_structure_loss;
	uint32_t damage_measure_version;
};
struct texture_recipe
{
	uint64_t source_hash;
	uint32_t width, height;
	enum texture_policy_result result; /* the policy table's (classifier, pins included), not the damage rule's */
	enum texture_policy_treatment treatment;
	int wrap, dilates;
	int alpha;                         /* 0 none, 1 binary, 2 graded */
	uint32_t scale;                    /* 2, or 4 for first-person paths */
};
struct texture_cache_record
{
	uint64_t hash;
	uint32_t width, height;
	enum texture_policy_result result;
	enum texture_policy_treatment treatment;
	uint64_t recipe_key, global_key, bytes;
};
enum texture_cache_action
{
	TEXTURE_CACHE_KEEP,                /* up to date, or original and absent */
	TEXTURE_CACHE_MISSING,             /* not cached: the blocking screen or the background queue makes it */
	TEXTURE_CACHE_DELETE_NOW,          /* now original: deleted at the next level load, no queue */
	TEXTURE_CACHE_REPLACE_BACKGROUND,  /* S4g <-> bpf or a per-texture input changed: the old entry serves until replaced */
	TEXTURE_CACHE_REDO_BLOCKING        /* on an old global recipe: redone on the blocking screen, never mixed */
};
uint64_t texture_recipe_global_key(const struct texture_recipe_globals *, enum texture_policy_treatment,
	enum texture_policy_result);
uint64_t texture_recipe_key(const struct texture_recipe_globals *, const struct texture_recipe *);
enum texture_cache_action texture_cache_action(const struct texture_cache_record *stored /* NULL: none */,
	const struct texture_cache_record *desired, uint64_t stored_global_now);
/* the manifest: <directory>/manifest.tsv, one record per line; load returns 0 with *records malloc'd and *count set (a missing manifest: 0 records), or -1 if it exists but cannot be read (no records) */
int texture_cache_manifest_load(const char *directory, struct texture_cache_record **records, size_t *count);
int texture_cache_manifest_save(const char *directory, const struct texture_cache_record *records, size_t count);

/* ---------- storage (spec "Cache and versioning": skips, low storage, the storage floor)
The floor: a level that has entries to make first starts only if free storage covers them plus this slack. */
#define TEXTURE_CACHE_FLOOR_SLACK (256ull << 20)
#define TEXTURE_CACHE_LOW_STORAGE_TEXT "There is not enough free storage to upscale this level's textures, so it will " \
	"play with the original textures. To free space, turn on Delete upscaled textures in this app's page of the " \
	"Settings app. Prepared original textures are kept."
/* a level's actions counted (enum texture_cache_action), and the projected bytes of what the blocking screen makes */
struct texture_cache_level_plan
{
	size_t keep, missing, delete_now, replace_background, redo_blocking;
	uint64_t blocking_bytes;
};
enum texture_cache_level_start
{
	TEXTURE_CACHE_LOAD_UPSCALED,       /* nothing to make first */
	TEXTURE_CACHE_UPSCALE_FIRST,       /* the blocking screen ("Upscaling assets", press A to skip) */
	TEXTURE_CACHE_LOW_STORAGE          /* the notice, then the level plays original */
};
/* new levels only: the 2x level, plus the 4x level for scale 4 */
uint64_t texture_cache_entry_bytes(uint32_t width, uint32_t height, uint32_t scale, uint32_t bits_per_texel);
/* free storage covers the missing bytes plus TEXTURE_CACHE_FLOOR_SLACK */
int texture_cache_floor_ok(uint64_t free_bytes, uint64_t missing_bytes);
/* How a level starts. The caller's rules (piece 3):
   - a failed write during the blocking upscale ends it, and the level plays original
     (texture_upscale_state_request_original before the map loads); a skip does the same;
   - entries finished before either are kept and the manifest saved;
   - the background queue checks texture_cache_background_may_run before each entry and pauses while it is false. */
enum texture_cache_level_start texture_cache_level_start(const struct texture_cache_level_plan *, uint64_t free_bytes);
int texture_cache_background_may_run(uint64_t free_bytes, uint64_t next_entry_bytes);
/* <directory>/<hash>-<w>x<h>.bin through a .tmp and a rename; on any failure nothing is left behind; 0 on success */
int texture_cache_write_entry(const char *directory, const struct texture_cache_record *record, const void *bytes,
	size_t size);
void texture_cache_set_write_fault(long fail_after_bytes);   /* tests only: -1 off */

/* ---------- the storage control (the Settings app's "Delete upscaled textures")
The cache lives in this folder under the data root, excluded from backup. */
#define TEXTURE_CACHE_DIRECTORY "texture-cache"
/* the bytes of every regular file in the folder (0 if it is missing) */
uint64_t texture_cache_size(const char *directory);
/* deletes only the cache's own files (manifest, entries, leftover temporaries); a missing folder is 0, any failure -1 */
int texture_cache_delete_all(const char *directory);
