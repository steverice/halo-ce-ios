#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "../../linux/src/astc_store.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *fp = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static void append_orphan(const char *root)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/astc-cache/%s.pack", root, fp);
	int fd = open(path, O_WRONLY | O_APPEND);
	assert(fd >= 0);
	assert(write(fd, "orphan", 6) == 6);
	close(fd);
}
int main(void)
{
	char root[] = "/tmp/astc-store-XXXXXX";
	assert(mkdtemp(root));
	unsigned char source[32] = {1}, output[32] = {2};
	struct astc_store_span spans[] = {{source, 16}, {source + 16, 16}};
	struct astc_store_key key, changed;
	astc_store_key_compute(&key, 1, 4, 4, 2, 1, spans, 2);
	astc_store_key_compute(&changed, 1, 8, 4, 2, 1, spans, 2);
	assert(memcmp(&key, &changed, sizeof(key)));
	source[31]++;
	astc_store_key_compute(&changed, 1, 4, 4, 2, 1, spans, 2);
	assert(memcmp(&key, &changed, sizeof(key)));
	source[31]--;
	assert(astc_store_init(root));
	assert(!astc_store_select_map("../bad"));
	assert(astc_store_select_map(fp));
	struct astc_store_record record = {0};
	assert(!astc_store_lookup(&key, &record));
	assert(astc_store_write_encoder(fp, &key, output, 32));
	uint64_t generation = astc_store_generation();
	assert(astc_store_lookup(&key, &record));
	assert(record.size == 32 && !memcmp(record.bytes, output, 32));
	/* A remap and shutdown cannot invalidate a pinned upload. */
	assert(astc_store_write_encoder(fp, &changed, output, 16));
	assert(astc_store_generation() > generation);
	assert(!memcmp(record.bytes, output, 32));
	assert(astc_store_mark_complete(fp));
	astc_store_shutdown();
	assert(!memcmp(record.bytes, output, 32));
	astc_store_release(&record);
	append_orphan(root);
	assert(astc_store_init(root));
	assert(astc_store_map_complete(fp));
	assert(astc_store_lookup_map(fp, &key, &record));
	astc_store_release(&record);
	char path[1024];
	snprintf(path, sizeof(path), "%s/astc-cache/%s.pack", root, fp);
	struct stat st;
	assert(!stat(path, &st) && st.st_size == 48);
	/* Simulate a torn record: the checksum must force fallback. */
	astc_store_shutdown();
	int fd = open(path, O_WRONLY);
	assert(fd >= 0);
	assert(write(fd, "x", 1) == 1);
	close(fd);
	assert(astc_store_init(root));
	assert(astc_store_select_map(fp));
	assert(!astc_store_map_complete(fp));
	assert(!astc_store_lookup(&key, &record));
	assert(astc_store_write_encoder(fp, &key, output, 32));
	assert(astc_store_lookup(&key, &record));
	assert(!memcmp(record.bytes, output, 32));
	astc_store_release(&record);
	/* A torn or stale manifest invalidates the entire pack. */
	astc_store_shutdown();
	snprintf(path, sizeof(path), "%s/astc-cache/%s.manifest", root, fp);
	fd = open(path, O_WRONLY);
	assert(fd >= 0);
	assert(lseek(fd, 8, SEEK_SET) == 8);
	assert(write(fd, "stale", 5) == 5);
	close(fd);
	assert(astc_store_init(root));
	assert(!astc_store_map_complete(fp));
	assert(astc_store_select_map(fp));
	assert(!astc_store_lookup(&key, &record));
	assert(astc_store_write_encoder(fp, &key, output, 32));
	assert(astc_store_mark_complete(fp));
	/* An exclusion-only map resumes without creating ASTC output bytes. */
	const char *empty = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
	generation = astc_store_generation();
	assert(astc_store_write_exclusion(empty, &changed));
	assert(astc_store_generation() == generation);
	assert(astc_store_is_excluded(empty, &changed));
	assert(astc_store_mark_complete(empty));
	astc_store_shutdown();
	assert(astc_store_init(root));
	assert(astc_store_map_complete(empty));
	assert(astc_store_is_excluded(empty, &changed));
	assert(astc_store_select_map(empty));
	assert(astc_store_select_shared_map(fp));
	assert(astc_store_lookup(&key, &record));
	astc_store_release(&record);
	assert(!astc_store_has_space(UINT64_MAX));
	astc_store_cleanup(NULL, 0);
	assert(access(path, F_OK));
	astc_store_shutdown();
	snprintf(path, sizeof(path), "%s/astc-cache", root);
	assert(!rmdir(path));
	assert(!rmdir(root));
	puts("ASTC store crash/resume checks passed");
	return 0;
}
