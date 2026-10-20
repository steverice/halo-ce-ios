/*
XISO.C

Copies the maps folder out of an Xbox disc image (an "xiso"), for the
desktop and Android builds.

The image's file system is XDVDFS, read from its public descriptions
(https://xboxdevwiki.net/XDVDFS, https://multimedia.cx/xdvdfs.html):
2048-byte sectors, little-endian integers; a volume descriptor at sector 32
of the game partition, opening and closing with MICROSOFT*XBOX*MEDIA and
naming the root directory's sector and size; each directory a table of
entries (left and right child offsets in units of 4 bytes, start sector,
size, attributes, name length, name, padded to 4 bytes with 0xFF) that forms
a binary search tree; a file's bytes contiguous from its start sector.
Sectors count from the game partition, which starts at the beginning of a
bare image and further in a whole-disc dump.

Checked against a retail image rather than taken from those descriptions: a
directory's size is the bytes its entries use, not a whole number of
sectors, and its children's offsets count from the start of that
directory's own table.

Nothing here trusts the image: every offset and length is checked against
the image's size, a directory's tree is walked with each node visited once,
and names that could leave the destination folder are refused.

port/runtime/xiso.c does the same for the Apple builds, with their checks of
the maps and their movies.
*/

#include "xiso.h"

#include <stdlib.h>
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048u
#define VOLUME_OFFSET 0x10000ull
#define MAGIC "MICROSOFT*XBOX*MEDIA"
#define MAGIC_SIZE 20
#define MAGIC_TAIL 0x7ECu
#define ENTRY_HEADER 14u
#define DIRECTORY_BIT 0x10u
/* a directory table larger than this is no disc's */
#define DIRECTORY_MAXIMUM (8u << 20)
#define COPY_BUFFER (1u << 20)
#define NOT_THERE (~0ull)

/* where the game partition starts (bytes) in a bare image, and in the whole-disc dumps of the disc
generations that have been seen; the volume descriptor tells which is right */
static const unsigned long long partition_bases[] = {
	0ull, 0x02080000ull, 0x02090000ull, 0x0FD90000ull, 0x18300000ull, 0x89D80000ull,
};

struct entry
{
	const unsigned char *name;
	unsigned int sector, size;
	unsigned int attributes, length;
};

struct directory
{
	unsigned char *table;
	struct entry *entries;
	size_t count;
};

struct image
{
	SDL_IOStream *stream;
	unsigned long long length, base;
	char *error;
	int error_size;
};

static int fail(struct image *image, const char *format, ...)
{
	if (image->error && image->error_size > 0)
	{
		va_list arguments;

		va_start(arguments, format);
		vsnprintf(image->error, (size_t)image->error_size, format, arguments);
		va_end(arguments);
	}
	return 0;
}

static unsigned int read16(const unsigned char *p)
{
	return (unsigned int)p[0] | (unsigned int)p[1] << 8;
}

static unsigned int read32(const unsigned char *p)
{
	return read16(p) | read16(p + 2) << 16;
}

static int read_at(struct image *image, unsigned long long offset, void *buffer, size_t size)
{
	return SDL_SeekIO(image->stream, (Sint64)offset, SDL_IO_SEEK_SET) >= 0 && SDL_ReadIO(image->stream, buffer, size) == size;
}

/* the offset of a sector's data, or NOT_THERE when it and `size` bytes after it do not lie within the image */
static unsigned long long extent(const struct image *image, unsigned int sector, unsigned long long size)
{
	unsigned long long start = image->base + (unsigned long long)sector * SECTOR;

	if (start > image->length || size > image->length - start)
		return NOT_THERE;
	return start;
}

static int equal_folded(const unsigned char *a, size_t length, const char *b)
{
	size_t i;

	if (length != strlen(b))
		return 0;
	for (i = 0; i < length; i++)
		if (tolower(a[i]) != tolower((unsigned char)b[i]))
			return 0;
	return 1;
}

static int fold_compare(const struct entry *x, const struct entry *y)
{
	unsigned int n = x->length < y->length ? x->length : y->length, i;

	for (i = 0; i < n; i++)
	{
		int c = tolower(x->name[i]) - tolower(y->name[i]);

		if (c)
			return c;
	}
	return (int)x->length - (int)y->length;
}

static int compare_pointers(const void *a, const void *b)
{
	return fold_compare(*(const struct entry *const *)a, *(const struct entry *const *)b);
}

/* finds the game partition among the known places for it: the descriptor opens and closes with the magic text */
static int find_partition(struct image *image)
{
	size_t i;

	for (i = 0; i < sizeof(partition_bases) / sizeof(*partition_bases); i++)
	{
		unsigned long long at = partition_bases[i] + VOLUME_OFFSET;
		char head[MAGIC_SIZE], tail[MAGIC_SIZE];

		if (at + SECTOR > image->length || !read_at(image, at, head, sizeof(head)) || memcmp(head, MAGIC, MAGIC_SIZE) ||
			!read_at(image, at + MAGIC_TAIL, tail, sizeof(tail)) || memcmp(tail, MAGIC, MAGIC_SIZE))
			continue;
		image->base = partition_bases[i];
		return 1;
	}
	return 0;
}

static void directory_free(struct directory *directory)
{
	free(directory->table);
	free(directory->entries);
	memset(directory, 0, sizeof(*directory));
}

/* reads the directory table at `sector` (`size` bytes) and visits its search tree from the first entry, each
node once; a node whose left offset is 0xFFFF is padding, so an empty table has no entries */
static int directory_load(struct image *image, unsigned int sector, unsigned int size, struct directory *directory)
{
	size_t words = size / 4, stack_count = 0, capacity = 0;
	unsigned char *seen = NULL;
	unsigned int *stack = NULL;
	unsigned long long at;
	int ok = 0;

	memset(directory, 0, sizeof(*directory));
	if (!size)
		return 1;
	if (size > DIRECTORY_MAXIMUM)
		return fail(image, "The disc image has a damaged directory tree.");
	at = extent(image, sector, size);
	if (at == NOT_THERE)
		return fail(image, "The disc image is truncated. Download it again, then choose it.");
	directory->table = malloc(size);
	seen = calloc(words / 8 + 1, 1);
	stack = malloc((words + 1) * sizeof(*stack));
	if (!directory->table || !seen || !stack)
	{
		fail(image, "Not enough memory to read the disc image.");
		goto done;
	}
	if (!read_at(image, at, directory->table, size))
	{
		fail(image, "Could not read the disc image.");
		goto done;
	}
	stack[stack_count++] = 0;
	seen[0] |= 1;
	while (stack_count)
	{
		size_t position = (size_t)stack[--stack_count] * 4;
		const unsigned char *node = directory->table + position;
		unsigned int left, right, name_length, child;
		int side;

		if (position + ENTRY_HEADER > size)
			goto damaged;
		left = read16(node);
		right = read16(node + 2);
		if (left == 0xFFFF)
			continue;
		name_length = node[13];
		if (!name_length || position + ENTRY_HEADER + name_length > size)
			goto damaged;
		if (directory->count == capacity)
		{
			struct entry *grown;

			capacity = capacity ? capacity * 2 : 16;
			grown = realloc(directory->entries, capacity * sizeof(*grown));
			if (!grown)
			{
				fail(image, "Not enough memory to read the disc image.");
				goto done;
			}
			directory->entries = grown;
		}
		directory->entries[directory->count].name = node + ENTRY_HEADER;
		directory->entries[directory->count].sector = read32(node + 4);
		directory->entries[directory->count].size = read32(node + 8);
		directory->entries[directory->count].attributes = node[12];
		directory->entries[directory->count].length = name_length;
		directory->count++;
		for (side = 0; side < 2; side++)
		{
			child = side ? right : left;
			if (!child)
				continue;
			if (child >= words || ((seen[child / 8] >> (child % 8)) & 1))
				goto damaged;
			seen[child / 8] |= (unsigned char)(1u << (child % 8));
			stack[stack_count++] = child;
		}
	}
	ok = 1;
	goto done;
damaged:
	fail(image, "The disc image has a damaged directory tree.");
done:
	free(seen);
	free(stack);
	if (!ok)
		directory_free(directory);
	return ok;
}

/* a name that is one plain file name; false for anything that could reach outside its folder */
static int safe_name(const struct entry *entry)
{
	unsigned int i;

	if (entry->length == 1 && entry->name[0] == '.')
		return 0;
	if (entry->length == 2 && entry->name[0] == '.' && entry->name[1] == '.')
		return 0;
	for (i = 0; i < entry->length; i++)
		if (entry->name[i] == '/' || entry->name[i] == '\\' || !entry->name[i])
			return 0;
	return 1;
}

/* 1 if every name is safe and none repeats (ignoring case); else 0 with the reason */
static int names_usable(struct image *image, const struct directory *directory)
{
	const struct entry **sorted;
	size_t i;

	for (i = 0; i < directory->count; i++)
		if (!safe_name(&directory->entries[i]))
			return fail(image, "The disc image has an unsafe filename.");
	if (directory->count < 2)
		return 1;
	sorted = malloc(directory->count * sizeof(*sorted));
	if (!sorted)
		return fail(image, "Not enough memory to read the disc image.");
	for (i = 0; i < directory->count; i++)
		sorted[i] = &directory->entries[i];
	qsort(sorted, directory->count, sizeof(*sorted), compare_pointers);
	for (i = 1; i < directory->count; i++)
	{
		if (!fold_compare(sorted[i - 1], sorted[i]))
		{
			free(sorted);
			return fail(image, "The disc image has duplicate filenames.");
		}
	}
	free(sorted);
	return 1;
}

static SDL_EnumerationResult remove_entry(void *folder, const char *directory, const char *name)
{
	char path[4096];

	(void)folder;
	snprintf(path, sizeof(path), "%s%s", directory, name);
	SDL_RemovePath(path);
	return SDL_ENUM_CONTINUE;
}

/* a staging folder left by a copy that did not finish */
static void remove_folder(const char *path)
{
	SDL_EnumerateDirectory(path, remove_entry, NULL);
	SDL_RemovePath(path);
}

/* copies one file of the image into `folder` under its name in lower case */
static int copy_file(struct image *image, const struct entry *entry, const char *folder, unsigned char *buffer,
	unsigned long long *done, unsigned long long total, xiso_progress_proc progress, void *context)
{
	char path[4096], name[256];
	unsigned long long at = extent(image, entry->sector, entry->size), left = entry->size;
	SDL_IOStream *file;
	unsigned int i;

	for (i = 0; i < entry->length; i++)
		name[i] = (char)tolower(entry->name[i]);
	name[entry->length] = 0;
	snprintf(path, sizeof(path), "%s/%s", folder, name);
	if (at == NOT_THERE)
		return fail(image, "The disc image is truncated. Download it again, then choose it.");
	file = SDL_IOFromFile(path, "wb");
	if (!file)
		return fail(image, "Could not create %s (%s). Check available storage.", name, SDL_GetError());
	while (left)
	{
		size_t chunk = left < COPY_BUFFER ? (size_t)left : COPY_BUFFER;

		if (!read_at(image, at, buffer, chunk))
		{
			SDL_CloseIO(file);
			return fail(image, "The disc image is truncated. Download it again, then choose it.");
		}
		if (SDL_WriteIO(file, buffer, chunk) != chunk)
		{
			SDL_CloseIO(file);
			return fail(image, "Could not write %s. Check available storage.", name);
		}
		at += chunk;
		left -= chunk;
		*done += chunk;
		if (progress)
			progress(context, name, *done, total);
	}
	if (!SDL_CloseIO(file))
		return fail(image, "Could not write %s. Check available storage.", name);
	return 1;
}

int xiso_extract_maps(const char *image_path, const char *destination, xiso_progress_proc progress, void *context,
	char *error, int error_size)
{
	struct image image;
	struct directory root, maps;
	const struct entry *maps_entry = NULL;
	unsigned char descriptor[SECTOR], *buffer = NULL;
	char maps_path[4096], partial_path[4096];
	unsigned long long done = 0, total = 0;
	SDL_PathInfo info;
	size_t i, files = 0;
	int ok = 0;

	memset(&image, 0, sizeof(image));
	memset(&root, 0, sizeof(root));
	memset(&maps, 0, sizeof(maps));
	image.error = error;
	image.error_size = error_size;
	snprintf(maps_path, sizeof(maps_path), "%s/maps", destination);
	snprintf(partial_path, sizeof(partial_path), "%s/maps.partial", destination);
	if (SDL_GetPathInfo(maps_path, &info))
		return fail(&image, "The folder already contains maps.");
	image.stream = SDL_IOFromFile(image_path, "rb");
	if (!image.stream)
		return fail(&image, "Could not open the disc image (%s).", SDL_GetError());
	{
		Sint64 length = SDL_GetIOSize(image.stream);

		image.length = length < 0 ? 0 : (unsigned long long)length;
	}
	if (!find_partition(&image) || !read_at(&image, image.base + VOLUME_OFFSET, descriptor, sizeof(descriptor)))
	{
		fail(&image, "This is not an Xbox XISO. Choose an uncompressed Halo: Combat Evolved .iso or .xiso file.");
		goto done;
	}
	if (!directory_load(&image, read32(descriptor + 0x14), read32(descriptor + 0x18), &root))
		goto done;
	for (i = 0; i < root.count; i++)
	{
		if ((root.entries[i].attributes & DIRECTORY_BIT) && equal_folded(root.entries[i].name, root.entries[i].length, "maps"))
		{
			maps_entry = &root.entries[i];
			break;
		}
	}
	if (!maps_entry)
	{
		fail(&image, "This disc image has no maps folder.");
		goto done;
	}
	if (!directory_load(&image, maps_entry->sector, maps_entry->size, &maps) || !names_usable(&image, &maps))
		goto done;
	for (i = 0; i < maps.count; i++)
	{
		if (maps.entries[i].attributes & DIRECTORY_BIT)
			continue;
		if (extent(&image, maps.entries[i].sector, maps.entries[i].size) == NOT_THERE)
		{
			fail(&image, "The disc image is truncated. Download it again, then choose it.");
			goto done;
		}
		total += maps.entries[i].size;
		files++;
	}
	if (!files)
	{
		fail(&image, "This disc image has no maps.");
		goto done;
	}
	buffer = malloc(COPY_BUFFER);
	if (!buffer)
	{
		fail(&image, "Not enough memory to import the disc image.");
		goto done;
	}
	remove_folder(partial_path);
	if (!SDL_CreateDirectory(partial_path))
	{
		fail(&image, "Could not create the maps folder (%s). Check available storage.", SDL_GetError());
		goto done;
	}
	for (i = 0; i < maps.count; i++)
		if (!(maps.entries[i].attributes & DIRECTORY_BIT) &&
			!copy_file(&image, &maps.entries[i], partial_path, buffer, &done, total, progress, context))
			goto done;
	if (!SDL_RenamePath(partial_path, maps_path))
	{
		fail(&image, "Could not finish the copy (%s).", SDL_GetError());
		goto done;
	}
	ok = 1;
done:
	free(buffer);
	directory_free(&root);
	directory_free(&maps);
	SDL_CloseIO(image.stream);
	return ok;
}
