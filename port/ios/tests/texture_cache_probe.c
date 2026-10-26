/* texture_cache.c: the recipe key and the spec's invalidation table ("Cache and versioning") */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "texture_cache.h"

static struct texture_recipe_globals globals(void)
{
	struct texture_recipe_globals g = { "S4g", "ab12", { 1, 1, 1, 1, 1, 1 }, 12, 30, 1, 3, 1, "rgba8",
		0.10f, -1, -1, 1 };
	return g;
}

static struct texture_cache_record record(const struct texture_recipe_globals *g, enum texture_policy_result result,
	enum texture_policy_treatment treatment)
{
	struct texture_recipe r = { 0x1234, 64, 64, result, treatment, 1, 0, 0, 2 };
	struct texture_cache_record out = { 0x1234, 64, 64, result, treatment, texture_recipe_key(g, &r),
		texture_recipe_global_key(g, treatment, result), 32768 };
	return out;
}

static enum texture_cache_action after(const struct texture_recipe_globals *old_globals,
	enum texture_policy_result old_result, const struct texture_recipe_globals *new_globals,
	enum texture_policy_result new_result, enum texture_policy_treatment treatment)
{
	struct texture_cache_record stored = record(old_globals, old_result, treatment);
	struct texture_cache_record desired = record(new_globals, new_result, treatment);

	return texture_cache_action(&stored, &desired, texture_recipe_global_key(new_globals, treatment, stored.result));
}

static void invalidation_table(void)
{
	struct texture_recipe_globals g = globals(), threshold = g, model = g, encoder = g, bumpfix = g, share = g;
	struct texture_cache_record desired = record(&g, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR);
	struct texture_cache_record original = record(&g, TEXTURE_POLICY_ORIGINAL, TEXTURE_POLICY_COLOR);

	threshold.low = 10;
	share.damage_share = 0.12f;
	model.model_sha256 = "cd34";
	encoder.encoder = "astc-6x6";
	bumpfix.pipeline_version[TEXTURE_POLICY_BUMP_MAP] = 2;
	/* an app update with none of these: nothing */
	assert(after(&g, TEXTURE_POLICY_BPF, &g, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_KEEP);
	/* overrides or rules: only a texture whose result changed */
	assert(after(&g, TEXTURE_POLICY_S4G, &g, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_REPLACE_BACKGROUND);
	assert(texture_cache_action(&desired, &original, desired.global_key) == TEXTURE_CACHE_DELETE_NOW);
	assert(texture_cache_action(NULL, &original, 0) == TEXTURE_CACHE_KEEP);
	assert(texture_cache_action(NULL, &desired, 0) == TEXTURE_CACHE_MISSING);
	/* the threshold or the bp/bpf kernels: bpf entries, and classifier-S4g entries (the damage rule may give them bpf
	bytes) */
	assert(after(&g, TEXTURE_POLICY_BPF, &threshold, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_REDO_BLOCKING);
	assert(after(&g, TEXTURE_POLICY_S4G, &threshold, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_REDO_BLOCKING);
	/* the damage block: classifier-S4g entries only */
	assert(after(&g, TEXTURE_POLICY_S4G, &share, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_REDO_BLOCKING);
	assert(after(&g, TEXTURE_POLICY_BPF, &share, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_KEEP);
	/* the bump fix: bump entries only */
	assert(after(&g, TEXTURE_POLICY_BPF, &bumpfix, TEXTURE_POLICY_BPF, TEXTURE_POLICY_BUMP_MAP) == TEXTURE_CACHE_REDO_BLOCKING);
	assert(after(&g, TEXTURE_POLICY_BPF, &bumpfix, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_KEEP);
	/* the model or the encoder: every upscaled entry */
	assert(after(&g, TEXTURE_POLICY_S4G, &model, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_REDO_BLOCKING);
	assert(after(&g, TEXTURE_POLICY_BPF, &encoder, TEXTURE_POLICY_BPF, TEXTURE_POLICY_MULTIPURPOSE) == TEXTURE_CACHE_REDO_BLOCKING);
	/* a stale global recipe and a result change at once: never mix recipes */
	assert(after(&g, TEXTURE_POLICY_S4G, &model, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_REDO_BLOCKING);
}

static void keys(void)
{
	struct texture_recipe_globals g = globals();
	struct texture_recipe a = { 0x1234, 64, 64, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR, 1, 0, 0, 2 }, b = a;

	assert(texture_recipe_key(&g, &a) == texture_recipe_key(&g, &b));
	b.wrap = 0;
	assert(texture_recipe_key(&g, &a) != texture_recipe_key(&g, &b));   /* the address mode is in the key */
	b = a; b.scale = 4;
	assert(texture_recipe_key(&g, &a) != texture_recipe_key(&g, &b));
	b = a; b.alpha = 1;
	assert(texture_recipe_key(&g, &a) != texture_recipe_key(&g, &b));
	assert(texture_recipe_global_key(&g, TEXTURE_POLICY_COLOR, TEXTURE_POLICY_ORIGINAL) == 0);
}

/* a classifier-S4g entry's key carries the damage block, its measure's version and the bpf parameters, so a change to
any of them changes it; a bpf entry's key ignores the damage block */
static void damage_keys(void)
{
	struct texture_recipe_globals g = globals(), changed[10];
	struct texture_recipe s4g = { 0x1234, 64, 64, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR, 1, 0, 0, 2 }, bpf = s4g;
	size_t i;

	bpf.result = TEXTURE_POLICY_BPF;
	for (i = 0; i < 10; i++)
		changed[i] = g;
	changed[0].damage_share = 0.11f;
	changed[1].damage_share = -1;
	changed[2].damage_structure_loss = 0.39f;
	changed[3].damage_bpf_structure_loss = 0.05f;
	changed[4].damage_measure_version = 2;
	changed[5].low = 11;
	changed[6].high = 31;
	changed[7].sigma = 1.5f;
	changed[8].iterations = 4;
	changed[9].post_version = 2;
	for (i = 0; i < 10; i++)
	{
		assert(texture_recipe_key(&g, &s4g) != texture_recipe_key(&changed[i], &s4g));
		assert(texture_recipe_global_key(&g, TEXTURE_POLICY_COLOR, TEXTURE_POLICY_S4G) !=
			texture_recipe_global_key(&changed[i], TEXTURE_POLICY_COLOR, TEXTURE_POLICY_S4G));
	}
	for (i = 0; i < 5; i++)
		assert(texture_recipe_key(&g, &bpf) == texture_recipe_key(&changed[i], &bpf));
}

static void manifest_round_trip(void)
{
	char directory[] = "/tmp/texture-cache-probe-XXXXXX";
	struct texture_recipe_globals g = globals();
	struct texture_cache_record records[2] = { record(&g, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR),
		record(&g, TEXTURE_POLICY_BPF, TEXTURE_POLICY_BUMP_MAP) }, *loaded;
	char path[256];
	size_t count;

	assert(mkdtemp(directory));
	records[1].hash = 0xfedcba9876543210ull;
	assert(texture_cache_manifest_save(directory, records, 2) == 0);
	assert(texture_cache_manifest_load(directory, &loaded, &count) == 0 && count == 2);
	assert(!memcmp(&loaded[0], &records[0], sizeof(records[0])) && !memcmp(&loaded[1], &records[1], sizeof(records[1])));
	free(loaded);
	snprintf(path, sizeof(path), "%s/manifest.tsv", directory);
	unlink(path);
	assert(texture_cache_manifest_load(directory, &loaded, &count) == 0 && count == 0 && !loaded);   /* no manifest: an empty cache */
	assert(mkdir(path, 0700) == 0);                                     /* a manifest that cannot be read: an error, not empty */
	count = 7;
	assert(texture_cache_manifest_load(directory, &loaded, &count) == -1 && count == 0 && !loaded);
	rmdir(path);
	assert(texture_cache_manifest_save(directory, records, 2) == 0);
	assert(chmod(path, 0) == 0);
	if (geteuid())                                                       /* root reads anything */
		assert(texture_cache_manifest_load(directory, &loaded, &count) == -1 && count == 0 && !loaded);
	chmod(path, 0600);
	unlink(path);
	assert(texture_cache_manifest_load("/nonexistent-texture-cache-dir", &loaded, &count) == 0 && count == 0 && !loaded);
	rmdir(directory);
}

static void storage(void)
{
	struct texture_cache_level_plan plan = { 0 };

	assert(texture_cache_entry_bytes(256, 256, 2, 32) == 512ull * 512 * 4);
	assert(texture_cache_entry_bytes(256, 256, 4, 32) == (1024ull * 1024 + 512ull * 512) * 4);
	assert(texture_cache_entry_bytes(256, 256, 8, 32) == (2048ull * 2048 + 1024ull * 1024 + 512ull * 512) * 4);
	assert(texture_cache_entry_bytes(256, 256, 2, 8) == 512ull * 512);           /* BC3 or ASTC 4x4: a byte per texel */
	assert(texture_cache_floor_ok(TEXTURE_CACHE_FLOOR_SLACK + 100, 100));
	assert(!texture_cache_floor_ok(TEXTURE_CACHE_FLOOR_SLACK + 99, 100));
	assert(texture_cache_level_start(&plan, 0) == TEXTURE_CACHE_LOAD_UPSCALED);   /* nothing to make: no check */
	plan.missing = 3; plan.blocking_bytes = 1 << 20;
	assert(texture_cache_level_start(&plan, TEXTURE_CACHE_FLOOR_SLACK + (1 << 20)) == TEXTURE_CACHE_UPSCALE_FIRST);
	assert(texture_cache_level_start(&plan, TEXTURE_CACHE_FLOOR_SLACK) == TEXTURE_CACHE_LOW_STORAGE);
	plan.missing = 0; plan.redo_blocking = 1;
	assert(texture_cache_level_start(&plan, TEXTURE_CACHE_FLOOR_SLACK) == TEXTURE_CACHE_LOW_STORAGE);
	assert(texture_cache_background_may_run(TEXTURE_CACHE_FLOOR_SLACK + 10, 10));
	assert(!texture_cache_background_may_run(TEXTURE_CACHE_FLOOR_SLACK + 9, 10));
	assert(strstr(TEXTURE_CACHE_LOW_STORAGE_TEXT, "Delete upscaled textures"));
}

static void write_fails_cleanly(void)
{
	char directory[] = "/tmp/texture-cache-write-XXXXXX", path[256];
	struct texture_cache_record r = { 0xabc, 4, 4, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR, 1, 2, 64 };
	unsigned char bytes[64] = { 1 };

	assert(mkdtemp(directory));
	assert(texture_cache_write_entry(directory, &r, bytes, sizeof(bytes)) == 0);
	snprintf(path, sizeof(path), "%s/0000000000000abc-4x4.bin", directory);
	assert(access(path, F_OK) == 0);
	unlink(path);
	texture_cache_set_write_fault(10);              /* the disk fills ten bytes in */
	assert(texture_cache_write_entry(directory, &r, bytes, sizeof(bytes)) != 0);
	texture_cache_set_write_fault(-1);
	assert(access(path, F_OK) != 0);
	snprintf(path, sizeof(path), "%s/0000000000000abc-4x4.bin.tmp", directory);
	assert(access(path, F_OK) != 0);                /* nothing partial is kept */
	rmdir(directory);                               /* fails if anything was left behind */
	assert(access(directory, F_OK) != 0);
}

static void size_and_delete(void)
{
	char directory[] = "/tmp/texture-cache-size-XXXXXX", path[256];
	struct texture_cache_record r = { 0xabc, 4, 4, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR, 1, 2, 64 };
	unsigned char bytes[100] = { 0 };
	FILE *other;

	assert(mkdtemp(directory));
	assert(texture_cache_size(directory) == 0);
	assert(texture_cache_write_entry(directory, &r, bytes, sizeof(bytes)) == 0);
	assert(texture_cache_manifest_save(directory, &r, 1) == 0);
	assert(texture_cache_size(directory) > 100);
	snprintf(path, sizeof(path), "%s/notes.txt", directory);          /* not ours: left alone */
	other = fopen(path, "w"); fputs("x", other); fclose(other);
	assert(texture_cache_delete_all(directory) == 0);
	assert(texture_cache_size(directory) == 1);
	unlink(path);
	assert(rmdir(directory) == 0);
	assert(texture_cache_size("/tmp/no-such-texture-cache") == 0);
	assert(texture_cache_delete_all("/tmp/no-such-texture-cache") == 0);
}

/* readiness: a level is ready only when every texture its policy upscales has a current entry */
static void progress(void)
{
	struct texture_recipe_globals g = globals(), newer = g;
	struct texture_policy_decision d[3] = {
		{ 0x10, 64, 64, TEXTURE_POLICY_GRAPHIC, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR, 1, 0, 0, 2 },
		{ 0x20, 32, 32, TEXTURE_POLICY_SURFACE, TEXTURE_POLICY_ORIGINAL, TEXTURE_POLICY_COLOR, 1, 0, 0, 1 },
		{ 0x30, 16, 16, TEXTURE_POLICY_SURFACE, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR, 1, 0, 0, 8 } };
	struct texture_cache_record r[2] = {
		{ 0x30, 16, 16, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR, 1, texture_recipe_global_key(&g, TEXTURE_POLICY_COLOR, TEXTURE_POLICY_BPF), 8 },
		{ 0x99, 8, 8, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR, 1, 1, 8 } };
	struct texture_cache_key level[2] = { { 0x30, 16, 16 }, { 0x20, 32, 32 } }, unknown = { 0x40, 4, 4 };
	struct texture_cache_progress p;

	texture_cache_progress(NULL, 0, d, 3, r, 2, &g, &p);
	assert(p.textures == 2 && p.ready == 1 && p.units == 64 * 64 + 16 * 16 * 4 && p.ready_units == 16 * 16 * 4);
	/* a level whose upscaled textures are all made (an original one counts for nothing) */
	texture_cache_progress(level, 2, d, 3, r, 2, &g, &p);
	assert(p.textures == 1 && p.ready == 1);
	/* a key the decisions do not list (a map added since) is a texture not ready, so its level is never "ready" on
	   the shared textures alone; no records at all is nothing ready */
	texture_cache_progress(&unknown, 1, d, 3, NULL, 0, &g, &p);
	assert(p.textures == 1 && p.ready == 0);
	{
		struct texture_cache_key added[2] = { { 0x30, 16, 16 }, { 0x40, 4, 4 } };

		texture_cache_progress(added, 2, d, 3, r, 2, &g, &p);
		assert(p.textures == 2 && p.ready == 1);
	}
	texture_cache_progress(level, 1, d, 3, NULL, 0, &g, &p);
	assert(p.textures == 1 && p.ready == 0);
	/* a global recipe change makes the entry stale: not ready until it is redone */
	newer.iterations = 4;
	texture_cache_progress(level, 2, d, 3, r, 2, &newer, &p);
	assert(p.textures == 1 && p.ready == 0);
	/* an entry the compression gate kept original counts as made */
	r[0].result = TEXTURE_POLICY_ORIGINAL;
	texture_cache_progress(level, 2, d, 3, r, 2, &g, &p);
	assert(p.ready == 1);
}

/* the decisions beside the manifest: round trip in classify order, refused when damaged, deleted with the cache */
static void decisions(void)
{
	char directory[] = "/tmp/texture-cache-decisions-XXXXXX", path[256];
	struct texture_policy_decision d[2] = {
		{ 0x30, 16, 16, TEXTURE_POLICY_SURFACE, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR, 1, 0, 0, 2 },
		{ 0x10, 64, 64, TEXTURE_POLICY_GRAPHIC, TEXTURE_POLICY_ORIGINAL, TEXTURE_POLICY_BUMP_MAP, 1, 0, 0, 1 } };
	struct texture_policy_decision *loaded = NULL;
	size_t count = 0;
	FILE *file;

	assert(mkdtemp(directory));
	assert(texture_cache_decisions_load(directory, &loaded, &count) == -1 && !loaded);
	assert(texture_cache_decisions_save(directory, d, 2) == 0);
	assert(texture_cache_decisions_load(directory, &loaded, &count) == 0 && count == 2);
	assert(loaded[0].hash == 0x10 && loaded[0].result == TEXTURE_POLICY_ORIGINAL && loaded[0].treatment == TEXTURE_POLICY_BUMP_MAP);
	assert(loaded[1].hash == 0x30 && loaded[1].width == 16 && loaded[1].result == TEXTURE_POLICY_BPF);
	free(loaded);
	snprintf(path, sizeof(path), "%s/decisions.tsv", directory);
	file = fopen(path, "a"); fputs("not a decision\n", file); fclose(file);
	assert(texture_cache_decisions_load(directory, &loaded, &count) == -1 && !loaded && !count);
	assert(texture_cache_delete_all(directory) == 0);
	assert(access(path, F_OK) != 0);
	assert(rmdir(directory) == 0);
}

int main(void)
{
	keys();
	damage_keys();
	invalidation_table();
	manifest_round_trip();
	storage();
	write_fails_cleanly();
	size_and_delete();
	progress();
	decisions();
	puts("texture_cache_probe: ok");
	return 0;
}
