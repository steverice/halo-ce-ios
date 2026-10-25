/* Durable, disposable encoder cache. Disk operations never hold the lookup lock. */
#include "../linux/src/astc_store.h"
#include "texture_cache.h"
#define XXH_INLINE_ALL
#include "../third_party/xxhash/xxhash.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define ENTRY_SIZE 40u
#define MANIFEST_HEADER 128u
#define MAX_RECORDS 1048576u
struct item
{
	struct astc_store_key key;
	uint64_t offset, size, checksum;
	int checked;
};
struct view
{
	void *bytes;
	size_t size, refs;
};
struct map
{
	char fingerprint[65];
	struct item *items;
	size_t count;
	bool complete;
	struct view *view;
	struct map *next;
};
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static struct map *maps, *active, *shared;
static char directory[PATH_MAX];
static _Atomic uint64_t generation;
static void put64(uint8_t *p, uint64_t v)
{
	for (unsigned i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t get64(const uint8_t *p)
{
	uint64_t v = 0;
	for (unsigned i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}
static bool fingerprint_valid(const char *s)
{
	if (!s || strlen(s) != 64)
		return false;
	for (unsigned i = 0; i < 64; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return false;
	return true;
}
static bool path_for(char *path, const char *fp, const char *suffix)
{
	return directory[0] && snprintf(path, PATH_MAX, "%s/%s%s", directory, fp, suffix) < PATH_MAX;
}
static bool write_all(int fd, const void *bytes, size_t size)
{
	const uint8_t *p = bytes;
	while (size)
	{
		ssize_t n = write(fd, p, size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		p += n;
		size -= (size_t)n;
	}
	return true;
}
static void sync_directory(void)
{
	int fd = open(directory, O_RDONLY);
	if (fd >= 0)
	{
		fsync(fd);
		close(fd);
	}
}
static void unpin(struct view *v)
{
	if (v && !--v->refs)
	{
		if (v->size)
			munmap(v->bytes, v->size);
		free(v);
	}
}
static struct map *find_map(const char *fp)
{
	for (struct map *m = maps; m; m = m->next)
		if (!strcmp(m->fingerprint, fp))
			return m;
	return NULL;
}
static uint64_t version_hash(void)
{
	return XXH3_64bits(ASTC_STORE_VERSION, strlen(ASTC_STORE_VERSION));
}
void astc_store_key_compute(struct astc_store_key *key, uint32_t format, uint32_t width,
							uint32_t height, uint32_t levels, uint32_t faces,
							const struct astc_store_span *spans, size_t count)
{
	XXH3_state_t state;
	XXH3_128bits_reset(&state);
	XXH3_128bits_update(&state, ASTC_STORE_VERSION, strlen(ASTC_STORE_VERSION));
	uint64_t fields[] = {format, width, height, levels, faces, count};
	uint8_t b[8];
	for (size_t i = 0; i < 6; i++)
	{
		put64(b, fields[i]);
		XXH3_128bits_update(&state, b, 8);
	}
	for (size_t i = 0; i < count; i++)
	{
		put64(b, spans[i].size);
		XXH3_128bits_update(&state, b, 8);
		XXH3_128bits_update(&state, spans[i].bytes, spans[i].size);
	}
	XXH128_hash_t hash = XXH3_128bits_digest(&state);
	put64(key->bytes, hash.low64);
	put64(key->bytes + 8, hash.high64);
}
/* io_lock serializes publications. state_lock protects only in-memory views. */
static struct map *load_map(const char *fp)
{
	pthread_mutex_lock(&state_lock);
	struct map *m = find_map(fp);
	pthread_mutex_unlock(&state_lock);
	if (m)
		return m;
	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	strcpy(m->fingerprint, fp);
	char path[PATH_MAX];
	uint8_t *manifest = NULL;
	size_t length = 0;
	int fd = -1;
	if (!path_for(path, fp, ".manifest"))
		goto publish;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		goto publish;
	struct stat st;
	if (fstat(fd, &st) || st.st_size < MANIFEST_HEADER ||
		(uint64_t)st.st_size > MANIFEST_HEADER + (uint64_t)MAX_RECORDS * ENTRY_SIZE)
		goto close_manifest;
	length = (size_t)st.st_size;
	manifest = malloc(length);
	if (!manifest)
		goto close_manifest;
	size_t got = 0;
	while (got < length)
	{
		ssize_t n = read(fd, manifest + got, length - got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	if (got != length || memcmp(manifest, "ASTCMAP2", 8) || get64(manifest + 8) != version_hash() ||
		get64(manifest + 24) > MAX_RECORDS ||
		length != MANIFEST_HEADER + get64(manifest + 24) * ENTRY_SIZE ||
		memcmp(manifest + 64, fp, 64) ||
		get64(manifest + 40) !=
			(XXH3_64bits(manifest, 40) ^ XXH3_64bits(manifest + 48, length - 48)))
		goto close_manifest;
	m->count = (size_t)get64(manifest + 24);
	m->complete = get64(manifest + 32) == 1;
	m->items = calloc(m->count ? m->count : 1, sizeof(*m->items));
	if (!m->items)
	{
		m->count = 0;
		m->complete = false;
		goto close_manifest;
	}
	for (size_t i = 0; i < m->count; i++)
	{
		uint8_t *p = manifest + MANIFEST_HEADER + i * ENTRY_SIZE;
		memcpy(m->items[i].key.bytes, p, 16);
		m->items[i].offset = get64(p + 16);
		m->items[i].size = get64(p + 24);
		m->items[i].checksum = get64(p + 32);
	}
close_manifest:
	close(fd);
	free(manifest);
	fd = -1;
	if (!path_for(path, fp, ".pack"))
		goto publish;
	fd = open(path, O_RDWR);
	if (fd < 0)
	{
		if (!m->count)
			goto publish;
		goto invalidate;
	}
	if (fstat(fd, &st) || st.st_size < 0)
		goto invalidate;
	uint64_t end = 0;
	for (size_t i = 0; i < m->count; i++)
	{
		struct item *it = m->items + i;
		if ((!it->size && it->checksum != UINT64_MAX) || it->size % 16 || it->offset != end ||
			end > (uint64_t)st.st_size || it->size > (uint64_t)st.st_size - end)
			goto invalidate;
		end += it->size;
	}
	/* Discard an append that reached disk before its manifest commit. */
	if ((uint64_t)st.st_size != end && ftruncate(fd, (off_t)end))
		goto invalidate;
	if (end)
	{
		m->view = calloc(1, sizeof(*m->view));
		if (!m->view)
			goto invalidate;
		m->view->bytes = mmap(NULL, (size_t)end, PROT_READ, MAP_SHARED, fd, 0);
		if (m->view->bytes == MAP_FAILED)
		{
			free(m->view);
			m->view = NULL;
			goto invalidate;
		}
		m->view->size = (size_t)end;
		m->view->refs = 1;
	}
	close(fd);
	goto publish;
invalidate:
	if (fd >= 0)
		close(fd);
	free(m->items);
	m->items = NULL;
	m->count = 0;
	m->complete = false;
publish:
	pthread_mutex_lock(&state_lock);
	m->next = maps;
	maps = m;
	pthread_mutex_unlock(&state_lock);
	return m;
}
static bool save_manifest(struct map *m, struct item *items, size_t count, bool complete)
{
	if (count > MAX_RECORDS)
		return false;
	size_t size = MANIFEST_HEADER + count * ENTRY_SIZE;
	uint8_t *data = calloc(1, size);
	if (!data)
		return false;
	memcpy(data, "ASTCMAP2", 8);
	put64(data + 8, version_hash());
	put64(data + 24, count);
	put64(data + 32, complete);
	for (size_t i = 0; i < count; i++)
	{
		uint8_t *p = data + MANIFEST_HEADER + i * ENTRY_SIZE;
		memcpy(p, items[i].key.bytes, 16);
		put64(p + 16, items[i].offset);
		put64(p + 24, items[i].size);
		put64(p + 32, items[i].checksum);
	}
	memcpy(data + 64, m->fingerprint, 64);
	put64(data + 40, XXH3_64bits(data, 40) ^ XXH3_64bits(data + 48, size - 48));
	char temp[PATH_MAX] = {0}, path[PATH_MAX];
	bool ok = path_for(temp, m->fingerprint, ".manifest.tmp") &&
			  path_for(path, m->fingerprint, ".manifest");
	int fd = ok ? open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600) : -1;
	ok = fd >= 0 && write_all(fd, data, size) && fsync(fd) == 0;
	if (fd >= 0)
		close(fd);
	free(data);
	if (ok)
		ok = rename(temp, path) == 0;
	if (ok)
		sync_directory();
	else if (temp[0])
		unlink(temp);
	return ok;
}
bool astc_store_init(const char *root)
{
	if (!root)
		return false;
	pthread_mutex_lock(&io_lock);
	bool ok =
		snprintf(directory, sizeof(directory), "%s/astc-cache", root) < (int)sizeof(directory);
	if (ok)
		ok = mkdir(directory, 0700) == 0 || errno == EEXIST;
	if (!ok)
		directory[0] = 0;
	pthread_mutex_unlock(&io_lock);
	return ok;
}
void astc_store_shutdown(void)
{
	pthread_mutex_lock(&io_lock);
	pthread_mutex_lock(&state_lock);
	while (maps)
	{
		struct map *m = maps;
		maps = m->next;
		unpin(m->view);
		free(m->items);
		free(m);
	}
	active = NULL;
	shared = NULL;
	directory[0] = 0;
	atomic_fetch_add(&generation, 1);
	pthread_mutex_unlock(&state_lock);
	pthread_mutex_unlock(&io_lock);
}
bool astc_store_select_map(const char *fp)
{
	if (!fingerprint_valid(fp))
		return false;
	pthread_mutex_lock(&io_lock);
	struct map *m = directory[0] ? load_map(fp) : NULL;
	pthread_mutex_lock(&state_lock);
	active = m;
	atomic_fetch_add(&generation, 1);
	pthread_mutex_unlock(&state_lock);
	pthread_mutex_unlock(&io_lock);
	return m != NULL;
}
bool astc_store_select_shared_map(const char *fp)
{
	if (!fingerprint_valid(fp))
		return false;
	pthread_mutex_lock(&io_lock);
	struct map *m = directory[0] ? load_map(fp) : NULL;
	pthread_mutex_lock(&state_lock);
	shared = m;
	atomic_fetch_add(&generation, 1);
	pthread_mutex_unlock(&state_lock);
	pthread_mutex_unlock(&io_lock);
	return m != NULL;
}
static bool lookup_locked(struct map *m, const struct astc_store_key *key,
						  struct astc_store_record *record)
{
	memset(record, 0, sizeof(*record));
	if (!m || !m->view)
		return false;
	for (size_t i = m->count; i > 0; i--)
	{
		struct item *it = m->items + i - 1;
		if (memcmp(it->key.bytes, key->bytes, 16))
			continue;
		if (!it->size)
			return false;
		if (!it->checked)
			it->checked = XXH3_64bits((uint8_t *)m->view->bytes + it->offset, (size_t)it->size) ==
								  it->checksum
							  ? 1
							  : -1;
		if (it->checked < 0)
			return false;
		m->view->refs++;
		record->bytes = (uint8_t *)m->view->bytes + it->offset;
		record->size = (size_t)it->size;
		record->pin = m->view;
		return true;
	}
	return false;
}
bool astc_store_lookup(const struct astc_store_key *key, struct astc_store_record *record)
{
	pthread_mutex_lock(&state_lock);
	bool ok = lookup_locked(active, key, record);
	if (!ok)
		ok = lookup_locked(shared, key, record);
	pthread_mutex_unlock(&state_lock);
	return ok;
}
bool astc_store_lookup_map(const char *fp, const struct astc_store_key *key,
						   struct astc_store_record *record)
{
	memset(record, 0, sizeof(*record));
	if (!fingerprint_valid(fp))
		return false;
	pthread_mutex_lock(&io_lock);
	struct map *m = directory[0] ? load_map(fp) : NULL;
	pthread_mutex_lock(&state_lock);
	bool ok = lookup_locked(m, key, record);
	pthread_mutex_unlock(&state_lock);
	pthread_mutex_unlock(&io_lock);
	return ok;
}
void astc_store_release(struct astc_store_record *record)
{
	pthread_mutex_lock(&state_lock);
	unpin(record->pin);
	memset(record, 0, sizeof(*record));
	pthread_mutex_unlock(&state_lock);
}
static bool write_record(const char *fp, const struct astc_store_key *key, const void *bytes,
						 size_t size)
{
	if (!fingerprint_valid(fp) || (size && !bytes) || size % 16)
		return false;
	bool ok = false;
	pthread_mutex_lock(&io_lock);
	if (!directory[0])
		goto done;
	struct map *m = load_map(fp);
	if (!m)
		goto done;
	/* A checksum failure is replaced by a fresh entry; old views stay pinned. */
	struct item *items = calloc(m->count + 1, sizeof(*items));
	if (!items)
		goto done;
	pthread_mutex_lock(&state_lock);
	if (m->count)
		memcpy(items, m->items, m->count * sizeof(*items));
	pthread_mutex_unlock(&state_lock);
	uint64_t end = m->view ? m->view->size : 0;
	if (size > SIZE_MAX - end)
	{
		free(items);
		goto done;
	}
	struct item *item = items + m->count;
	item->key = *key;
	item->offset = end;
	item->size = size;
	item->checksum = size ? XXH3_64bits(bytes, size) : UINT64_MAX;
	item->checked = 1;
	char path[PATH_MAX];
	if (!path_for(path, fp, ".pack"))
	{
		free(items);
		goto done;
	}
	int fd = open(path, O_RDWR | O_CREAT, 0600);
	if (fd < 0)
	{
		free(items);
		goto done;
	}
	if (ftruncate(fd, (off_t)end) || lseek(fd, (off_t)end, SEEK_SET) < 0 ||
		!write_all(fd, bytes, size) || fsync(fd))
	{
		close(fd);
		free(items);
		goto done;
	}
	struct view *view = calloc(1, sizeof(*view));
	if (!view)
	{
		close(fd);
		free(items);
		goto done;
	}
	view->size = (size_t)end + size;
	view->refs = 1;
	view->bytes = view->size ? mmap(NULL, view->size, PROT_READ, MAP_SHARED, fd, 0) : NULL;
	close(fd);
	if (view->bytes == MAP_FAILED)
	{
		free(view);
		free(items);
		goto done;
	}
	if (!save_manifest(m, items, m->count + 1, false))
	{
		if (view->size)
			munmap(view->bytes, view->size);
		free(view);
		free(items);
		goto done;
	}
	pthread_mutex_lock(&state_lock);
	unpin(m->view);
	free(m->items);
	m->view = view;
	m->items = items;
	m->count++;
	m->complete = false;
	if (m == active || m == shared)
		atomic_fetch_add(&generation, 1);
	pthread_mutex_unlock(&state_lock);
	ok = true;
done:
	pthread_mutex_unlock(&io_lock);
	return ok;
}
bool astc_store_write_encoder(const char *fp, const struct astc_store_key *key, const void *bytes,
							  size_t size)
{
	return bytes && size && write_record(fp, key, bytes, size);
}
bool astc_store_write_exclusion(const char *fp, const struct astc_store_key *key)
{
	return write_record(fp, key, NULL, 0);
}
bool astc_store_is_excluded(const char *fp, const struct astc_store_key *key)
{
	if (!fingerprint_valid(fp))
		return false;
	pthread_mutex_lock(&io_lock);
	struct map *m = directory[0] ? load_map(fp) : NULL;
	bool excluded = false;
	if (m)
		for (size_t i = m->count; i > 0; i--)
			if (!memcmp(m->items[i - 1].key.bytes, key->bytes, 16))
			{
				excluded = m->items[i - 1].size == 0;
				break;
			}
	pthread_mutex_unlock(&io_lock);
	return excluded;
}
bool astc_store_map_complete(const char *fp)
{
	if (!fingerprint_valid(fp))
		return false;
	pthread_mutex_lock(&io_lock);
	struct map *m = directory[0] ? load_map(fp) : NULL;
	bool ok = m && m->complete;
	/* The launch scan runs off the render thread. Validate completed packs here
	 * so a corrupt record schedules conversion again instead of staying missing. */
	if (ok)
	{
		int *checked = calloc(m->count ? m->count : 1, sizeof(*checked));
		if (!checked)
			ok = false;
		else
		{
			for (size_t i = 0; i < m->count; i++)
			{
				struct item *it = m->items + i;
				checked[i] =
					!it->size || (m->view && XXH3_64bits((uint8_t *)m->view->bytes + it->offset,
														 (size_t)it->size) == it->checksum)
						? 1
						: -1;
				if (checked[i] < 0)
				{
					bool replaced = false;
					for (size_t j = i + 1; j < m->count; j++)
						if (!memcmp(it->key.bytes, m->items[j].key.bytes, 16))
						{
							replaced = true;
							break;
						}
					if (!replaced)
						ok = false;
				}
			}
			pthread_mutex_lock(&state_lock);
			for (size_t i = 0; i < m->count; i++)
				m->items[i].checked = checked[i];
			m->complete = ok;
			pthread_mutex_unlock(&state_lock);
			free(checked);
		}
	}
	pthread_mutex_unlock(&io_lock);
	return ok;
}
bool astc_store_mark_complete(const char *fp)
{
	if (!fingerprint_valid(fp))
		return false;
	pthread_mutex_lock(&io_lock);
	struct map *m = directory[0] ? load_map(fp) : NULL;
	bool ok = m && save_manifest(m, m->items, m->count, true);
	if (ok)
		m->complete = true;
	pthread_mutex_unlock(&io_lock);
	return ok;
}
bool astc_store_has_space(uint64_t projected_bytes)
{
	pthread_mutex_lock(&io_lock);
	struct statvfs s;
	bool ok = directory[0] && statvfs(directory, &s) == 0;
	if (ok)
	{
		uint64_t free_bytes = (uint64_t)s.f_bavail * s.f_frsize;
		ok = projected_bytes <= UINT64_MAX - TEXTURE_CACHE_FLOOR_SLACK &&
			 free_bytes >= projected_bytes + TEXTURE_CACHE_FLOOR_SLACK;
	}
	pthread_mutex_unlock(&io_lock);
	return ok;
}
uint64_t astc_store_generation(void) { return atomic_load(&generation); }
void astc_store_cleanup(const char *const *fingerprints, size_t count)
{
	pthread_mutex_lock(&io_lock);
	DIR *d = directory[0] ? opendir(directory) : NULL;
	if (!d)
	{
		pthread_mutex_unlock(&io_lock);
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(d)))
	{
		size_t n = strlen(entry->d_name);
		if (n < 65)
			continue;
		char fp[65];
		memcpy(fp, entry->d_name, 64);
		fp[64] = 0;
		if (!fingerprint_valid(fp))
			continue;
		const char *suffix = entry->d_name + 64;
		if (strcmp(suffix, ".pack") && strcmp(suffix, ".manifest") &&
			strcmp(suffix, ".manifest.tmp"))
			continue;
		bool keep = false;
		for (size_t i = 0; i < count; i++)
			if (!strcmp(fp, fingerprints[i]))
			{
				keep = true;
				break;
			}
		if (keep)
			continue;
		char path[PATH_MAX];
		if (path_for(path, fp, suffix))
			unlink(path);
		pthread_mutex_lock(&state_lock);
		struct map **cursor = &maps;
		while (*cursor)
		{
			struct map *m = *cursor;
			if (!strcmp(m->fingerprint, fp))
			{
				*cursor = m->next;
				if (active == m)
					active = NULL;
				if (shared == m)
					shared = NULL;
				unpin(m->view);
				free(m->items);
				free(m);
				atomic_fetch_add(&generation, 1);
				break;
			}
			cursor = &m->next;
		}
		pthread_mutex_unlock(&state_lock);
	}
	closedir(d);
	sync_directory();
	pthread_mutex_unlock(&io_lock);
}
