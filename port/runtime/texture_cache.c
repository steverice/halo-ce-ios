/* The upscale cache's bookkeeping (texture_cache.h): the recipe key, the action for each texture after an app update,
the manifest, the storage rules and entry writes. A cache entry holds only the new levels; the original levels are uploaded natively. */
#include "texture_cache.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "texture_override.h"

static const char *const result_names[] = { "s4g", "bpf", "original" };

static void manifest_path(const char *directory, char *path, size_t size)
{
	snprintf(path, size, "%s/manifest.tsv", directory);
}

uint64_t texture_recipe_global_key(const struct texture_recipe_globals *g, enum texture_policy_treatment treatment,
	enum texture_policy_result result)
{
	char text[512];
	int length;

	if (result == TEXTURE_POLICY_ORIGINAL)
		return 0;
	length = snprintf(text, sizeof(text), "model=%s;weights=%s;pipeline=%u;encoder=%s", g->model_id, g->model_sha256,
		g->pipeline_version[treatment], g->encoder);
	if (length < 0 || (size_t)length >= sizeof(text))
		length = (int)sizeof(text) - 1;
	/* bpf, and classifier S4g, whose damage rule may fall back to bpf (or, later, the original) */
	if (result != TEXTURE_POLICY_ORIGINAL && (size_t)length < sizeof(text) - 1)
		length += snprintf(text + length, sizeof(text) - (size_t)length, ";low=%.3f;high=%.3f;sigma=%.3f;iterations=%u;post=%u",
			g->low, g->high, g->sigma, g->iterations, g->post_version);
	if (result == TEXTURE_POLICY_S4G && (size_t)length < sizeof(text) - 1)
		length += snprintf(text + length, sizeof(text) - (size_t)length,
			";damage_share=%.4f;damage_structure_loss=%.4f;damage_bpf_structure_loss=%.4f;damage_measure=%u",
			g->damage_share < 0 ? -1.0 : (double)g->damage_share,
			g->damage_structure_loss < 0 ? -1.0 : (double)g->damage_structure_loss,
			g->damage_bpf_structure_loss < 0 ? -1.0 : (double)g->damage_bpf_structure_loss, g->damage_measure_version);
	if ((size_t)length >= sizeof(text))
		length = (int)sizeof(text) - 1;
	return texture_override_hash((const unsigned char *)text, (size_t)length);
}

uint64_t texture_recipe_key(const struct texture_recipe_globals *g, const struct texture_recipe *r)
{
	char text[256];
	int length = snprintf(text, sizeof(text),
		"source=%016" PRIx64 ";size=%ux%u;result=%s;treatment=%d;address=%s;dilate=%d;alpha=%d;scale=%u;global=%016" PRIx64,
		r->source_hash, r->width, r->height, result_names[r->result], (int)r->treatment, r->wrap ? "wrap" : "clamp",
		r->dilates, r->alpha, r->scale, texture_recipe_global_key(g, r->treatment, r->result));

	if (length < 0 || (size_t)length >= sizeof(text))
		length = (int)sizeof(text) - 1;
	return texture_override_hash((const unsigned char *)text, (size_t)length);
}

enum texture_cache_action texture_cache_action(const struct texture_cache_record *stored,
	const struct texture_cache_record *desired, uint64_t stored_global_now)
{
	if (desired->result == TEXTURE_POLICY_ORIGINAL)
		return stored ? TEXTURE_CACHE_DELETE_NOW : TEXTURE_CACHE_KEEP;
	if (!stored)
		return TEXTURE_CACHE_MISSING;
	if (stored->global_key != stored_global_now)
		return TEXTURE_CACHE_REDO_BLOCKING;
	if (stored->recipe_key != desired->recipe_key)
		return TEXTURE_CACHE_REPLACE_BACKGROUND;
	return TEXTURE_CACHE_KEEP;
}

static int key_compare(uint64_t hash_a, uint32_t width_a, uint32_t height_a, uint64_t hash_b, uint32_t width_b,
	uint32_t height_b)
{
	if (hash_a != hash_b)
		return hash_a < hash_b ? -1 : 1;
	if (width_a != width_b)
		return width_a < width_b ? -1 : 1;
	return height_a < height_b ? -1 : height_a > height_b;
}

static int decision_order(const void *a, const void *b)
{
	const struct texture_policy_decision *x = a, *y = b;

	return key_compare(x->hash, x->width, x->height, y->hash, y->width, y->height);
}

static int record_order(const void *a, const void *b)
{
	const struct texture_cache_record *x = a, *y = b;

	return key_compare(x->hash, x->width, x->height, y->hash, y->width, y->height);
}

static const struct texture_policy_decision *find_decision(const struct texture_policy_decision *decisions,
	size_t count, uint64_t hash, uint32_t width, uint32_t height)
{
	size_t low = 0, high = count;

	while (low < high)
	{
		size_t middle = low + (high - low) / 2;
		int order = key_compare(decisions[middle].hash, decisions[middle].width, decisions[middle].height, hash,
			width, height);

		if (!order)
			return &decisions[middle];
		if (order < 0)
			low = middle + 1;
		else
			high = middle;
	}
	return NULL;
}

static void progress_add(const struct texture_policy_decision *d, const struct texture_cache_record *sorted,
	size_t record_count, const struct texture_recipe_globals *globals, struct texture_cache_progress *progress)
{
	struct texture_cache_record wanted;
	const struct texture_cache_record *found;
	uint64_t units;

	if (d->result == TEXTURE_POLICY_ORIGINAL)
		return;
	units = (uint64_t)d->width * d->height * (d->scale == 8 ? 4 : 1);
	progress->textures++;
	progress->units += units;
	memset(&wanted, 0, sizeof(wanted));
	wanted.hash = d->hash;
	wanted.width = d->width;
	wanted.height = d->height;
	found = sorted ? bsearch(&wanted, sorted, record_count, sizeof(*sorted), record_order) : NULL;
	if (found && found->global_key == texture_recipe_global_key(globals, d->treatment, d->result))
	{
		progress->ready++;
		progress->ready_units += units;
	}
}

void texture_cache_progress(const struct texture_cache_key *keys, size_t key_count,
	const struct texture_policy_decision *decisions, size_t decision_count,
	const struct texture_cache_record *records, size_t record_count, const struct texture_recipe_globals *globals,
	struct texture_cache_progress *progress)
{
	struct texture_cache_record *sorted = NULL;
	size_t i;

	memset(progress, 0, sizeof(*progress));
	if (record_count && (sorted = malloc(record_count * sizeof(*sorted))))
	{
		memcpy(sorted, records, record_count * sizeof(*sorted));
		qsort(sorted, record_count, sizeof(*sorted), record_order);
	}
	if (!keys)
		for (i = 0; i < decision_count; i++)
			progress_add(&decisions[i], sorted, sorted ? record_count : 0, globals, progress);
	else
		for (i = 0; i < key_count; i++)
		{
			const struct texture_policy_decision *d = find_decision(decisions, decision_count, keys[i].hash,
				keys[i].width, keys[i].height);

			if (d)
				progress_add(d, sorted, sorted ? record_count : 0, globals, progress);
			else
				progress->textures++;   /* never classified (a map added since): not known to be original, so not ready */
		}
	free(sorted);
}

int texture_cache_manifest_load(const char *directory, struct texture_cache_record **records, size_t *count_out)
{
	char path[1024], line[256];
	FILE *file;
	size_t count = 0, capacity = 0;
	int failed = 0;

	*records = NULL;
	*count_out = 0;
	manifest_path(directory, path, sizeof(path));
	if (!(file = fopen(path, "r")))
		return errno == ENOENT ? 0 : -1;   /* no manifest: an empty cache; one that cannot be read is an error */
	while (fgets(line, sizeof(line), file))
	{
		struct texture_cache_record r;
		unsigned result, treatment;

		memset(&r, 0, sizeof(r));
		if (sscanf(line, "%" SCNx64 "\t%u\t%u\t%u\t%u\t%" SCNx64 "\t%" SCNx64 "\t%" SCNu64, &r.hash, &r.width,
			&r.height, &result, &treatment, &r.recipe_key, &r.global_key, &r.bytes) != 8 || result > 2 || treatment > 5)
			continue;   /* a damaged line: that texture is simply not cached */
		r.result = (enum texture_policy_result)result;
		r.treatment = (enum texture_policy_treatment)treatment;
		if (count == capacity)
		{
			struct texture_cache_record *grown = realloc(*records, (capacity = capacity ? capacity * 2 : 256) * sizeof(r));

			if (!grown)
			{
				failed = 1;
				break;
			}
			*records = grown;
		}
		(*records)[count++] = r;
	}
	failed |= ferror(file) != 0;
	fclose(file);
	if (failed)
	{
		free(*records);
		*records = NULL;
		return -1;
	}
	*count_out = count;
	return 0;
}

/* writes <path> through <path>.tmp: fill, flush, fsync, close, rename; on any failure the .tmp is removed and the old
<path>, if any, is untouched; 0 on success */
static int write_atomically(const char *path, int (*fill)(FILE *, const void *), const void *context)
{
	char temporary[1040];
	FILE *file;
	int failed;

	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (!(file = fopen(temporary, "wb")))
		return -1;
	failed = fill(file, context) != 0;
	failed |= fflush(file) != 0;
	failed |= fsync(fileno(file)) != 0;
	failed |= fclose(file) != 0;
	if (failed || rename(temporary, path))
	{
		unlink(temporary);
		return -1;
	}
	return 0;
}

struct manifest_contents
{
	const struct texture_cache_record *records;
	size_t count;
};

static int fill_manifest(FILE *file, const void *context)
{
	const struct manifest_contents *m = context;
	size_t i;

	for (i = 0; i < m->count; i++)
		if (fprintf(file, "%016" PRIx64 "\t%u\t%u\t%u\t%u\t%016" PRIx64 "\t%016" PRIx64 "\t%" PRIu64 "\n",
			m->records[i].hash, m->records[i].width, m->records[i].height, (unsigned)m->records[i].result,
			(unsigned)m->records[i].treatment, m->records[i].recipe_key, m->records[i].global_key,
			m->records[i].bytes) < 0)
			return -1;
	return 0;
}

int texture_cache_manifest_save(const char *directory, const struct texture_cache_record *records, size_t count)
{
	char path[1024];
	struct manifest_contents m = { records, count };

	manifest_path(directory, path, sizeof(path));
	return write_atomically(path, fill_manifest, &m);
}

/* ---------- the whole-game decisions, beside the manifest (decisions.tsv) */
struct decision_contents { const struct texture_policy_decision *d; size_t count; };

static int fill_decisions(FILE *file, const void *context)
{
	const struct decision_contents *c = context;
	size_t i;

	for (i = 0; i < c->count; i++)
		if (fprintf(file, "%016" PRIx64 "\t%u\t%u\t%u\t%u\n", c->d[i].hash, c->d[i].width, c->d[i].height,
			(unsigned)c->d[i].result, (unsigned)c->d[i].treatment) < 0)
			return -1;
	return 0;
}

int texture_cache_decisions_save(const char *directory, const struct texture_policy_decision *decisions, size_t count)
{
	char path[1024];
	struct decision_contents c = { decisions, count };

	snprintf(path, sizeof(path), "%s/decisions.tsv", directory);
	return write_atomically(path, fill_decisions, &c);
}

int texture_cache_decisions_load(const char *directory, struct texture_policy_decision **decisions, size_t *count_out)
{
	char path[1024], line[128];
	FILE *file;
	size_t count = 0, capacity = 0;
	struct texture_policy_decision *list = NULL;

	*decisions = NULL;
	*count_out = 0;
	snprintf(path, sizeof(path), "%s/decisions.tsv", directory);
	if (!(file = fopen(path, "r")))
		return -1;
	while (fgets(line, sizeof(line), file))
	{
		struct texture_policy_decision d;
		unsigned result, treatment;

		memset(&d, 0, sizeof(d));
		if (sscanf(line, "%" SCNx64 "\t%u\t%u\t%u\t%u", &d.hash, &d.width, &d.height, &result, &treatment) != 5 ||
			result > TEXTURE_POLICY_ORIGINAL || treatment > TEXTURE_POLICY_BUMP_MAP)
		{
			free(list);
			fclose(file);
			return -1;
		}
		d.result = (enum texture_policy_result)result;
		d.treatment = (enum texture_policy_treatment)treatment;
		if (count == capacity)
		{
			struct texture_policy_decision *grown;

			capacity = capacity ? capacity * 2 : 256;
			if (!(grown = realloc(list, capacity * sizeof(*list))))
			{
				free(list);
				fclose(file);
				return -1;
			}
			list = grown;
		}
		list[count++] = d;
	}
	fclose(file);
	/* (in texture_policy_classify's order, which texture_cache_progress relies on) */
	qsort(list, count, sizeof(*list), decision_order);
	*decisions = list;
	*count_out = count;
	return 0;
}

/* ---------- storage: entry sizes, the low-storage floor, the level-start decision, entry writes */
static long write_fault = -1;

void texture_cache_set_write_fault(long fail_after_bytes)
{
	write_fault = fail_after_bytes;
}

uint64_t texture_cache_entry_bytes(uint32_t width, uint32_t height, uint32_t scale, uint32_t bits_per_texel)
{
	uint64_t texels = 0;

	/* the 2x level, then the 4x and 8x ones the scale reaches */
	for (uint32_t level = 2; level <= scale && level <= 8; level *= 2)
		texels += (uint64_t)width * height * level * level;
	return texels * bits_per_texel / 8;
}

int texture_cache_floor_ok(uint64_t free_bytes, uint64_t missing_bytes)
{
	return free_bytes >= missing_bytes + TEXTURE_CACHE_FLOOR_SLACK;
}

enum texture_cache_level_start texture_cache_level_start(const struct texture_cache_level_plan *plan,
	uint64_t free_bytes)
{
	if (!plan->missing && !plan->redo_blocking)
		return TEXTURE_CACHE_LOAD_UPSCALED;
	return texture_cache_floor_ok(free_bytes, plan->blocking_bytes) ? TEXTURE_CACHE_UPSCALE_FIRST :
		TEXTURE_CACHE_LOW_STORAGE;
}

int texture_cache_background_may_run(uint64_t free_bytes, uint64_t next_entry_bytes)
{
	return texture_cache_floor_ok(free_bytes, next_entry_bytes);
}

struct entry_contents
{
	const void *bytes;
	size_t size;
};

static int fill_entry(FILE *file, const void *context)
{
	const struct entry_contents *e = context;
	size_t size = write_fault >= 0 && (size_t)write_fault < e->size ? (size_t)write_fault : e->size;

	return fwrite(e->bytes, 1, size, file) != e->size ? -1 : 0;
}

int texture_cache_write_entry(const char *directory, const struct texture_cache_record *record, const void *bytes,
	size_t size)
{
	char path[1024];
	struct entry_contents e = { bytes, size };

	snprintf(path, sizeof(path), "%s/%016" PRIx64 "-%ux%u.bin", directory, record->hash, record->width, record->height);
	return write_atomically(path, fill_entry, &e);
}

/* the cache's own files: the manifest, the entries, and the temporaries a crash can leave */
static int ours(const char *name)
{
	size_t length = strlen(name);

	return !strcmp(name, "manifest.tsv") || !strcmp(name, "manifest.tsv.tmp") ||
		!strcmp(name, "decisions.tsv") || !strcmp(name, "decisions.tsv.tmp") ||
		(length > 4 && !strcmp(name + length - 4, ".bin")) || (length > 8 && !strcmp(name + length - 8, ".bin.tmp"));
}

uint64_t texture_cache_size(const char *directory)
{
	DIR *folder = opendir(directory);
	struct dirent *item;
	struct stat info;
	char path[1024];
	uint64_t total = 0;

	if (!folder)
		return 0;
	while ((item = readdir(folder)))
	{
		snprintf(path, sizeof(path), "%s/%s", directory, item->d_name);
		if (!stat(path, &info) && S_ISREG(info.st_mode))
			total += (uint64_t)info.st_size;
	}
	closedir(folder);
	return total;
}

int texture_cache_delete_all(const char *directory)
{
	DIR *folder = opendir(directory);
	struct dirent *item;
	char path[1024];
	int failed = 0;

	if (!folder)
		return errno == ENOENT ? 0 : -1;
	while ((item = readdir(folder)))
		if (ours(item->d_name))
		{
			snprintf(path, sizeof(path), "%s/%s", directory, item->d_name);
			failed |= unlink(path) != 0;
		}
	closedir(folder);
	return failed ? -1 : 0;
}
