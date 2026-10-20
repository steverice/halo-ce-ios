/*
XISO.C

Copies the maps folder out of an Xbox disc image (an "xiso"), for the
Apple importers, and checks the result.

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
*/

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "loading_picture.h"
#include "xiso.h"

#define SECTOR 2048u
#define VOLUME_OFFSET 0x10000u
#define MAGIC "MICROSOFT*XBOX*MEDIA"
#define MAGIC_SIZE 20u
#define MAGIC_TAIL 0x7ECu
#define ENTRY_HEADER 14u
#define DIRECTORY_BIT 0x10u
/* a directory table larger than this is no disc's */
#define DIRECTORY_MAXIMUM (8u << 20)
#define DEPTH_MAXIMUM 64
#define LIST_FILES_MAXIMUM (1u << 20)
#define COPY_BUFFER (1u << 20)
/* the executable that may carry the loading picture, at most */
#define EXECUTABLE_MAXIMUM (64u << 20)

/* where the game partition starts (bytes) in a bare image, and in the whole-disc dumps of the disc
   generations that have been seen; the volume descriptor tells which is right */
static const uint64_t partition_bases[] = {
    0, 0x02080000u, 0x02090000u, 0x0FD90000u, 0x18300000u, 0x89D80000u,
};

static const char *const campaign_maps[] = {
    "ui.map", "a10.map", "a30.map", "a50.map", "b30.map", "b40.map",
    "c10.map", "c20.map", "c40.map", "d20.map", "d40.map",
};
/* the map releases the game accepts: the build text in a map's header */
static const char *const supported_builds[] = {"01.10.12.2276", "01.01.14.2342"};

#define COUNT(array) (sizeof(array) / sizeof(*(array)))

static int fail(char *error, size_t size, const char *format, ...) {
    if (error && size) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(error, size, format, arguments);
        va_end(arguments);
    }
    return 0;
}

static uint32_t read16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t read32(const unsigned char *p) { return read16(p) | read16(p + 2) << 16; }

static int equal_folded(const unsigned char *a, size_t a_length, const char *b) {
    size_t i;
    if (a_length != strlen(b)) return 0;
    for (i = 0; i < a_length; i++) if (tolower(a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static int has_suffix_folded(const unsigned char *name, size_t length, const char *suffix) {
    size_t n = strlen(suffix);
    return length >= n && equal_folded(name + length - n, n, suffix);
}

static int has_prefix_folded(const unsigned char *name, size_t length, const char *prefix) {
    size_t n = strlen(prefix), i;
    if (length < n) return 0;
    for (i = 0; i < n; i++) if (tolower(name[i]) != tolower((unsigned char)prefix[i])) return 0;
    return 1;
}

/* ---- maps already on disk ---- */

int xiso_maps_ready(const char *maps, char *error, size_t error_size) {
    char build[33] = {0};
    int have_build = 0;
    size_t i, j;

    for (i = 0; i < COUNT(campaign_maps); i++) {
        char path[4096];
        unsigned char header[SECTOR];
        char text[33] = {0};
        struct stat info;
        FILE *file;
        size_t got;

        snprintf(path, sizeof(path), "%s/%s", maps, campaign_maps[i]);
        if (stat(path, &info) || !S_ISREG(info.st_mode)) return fail(error, error_size, "%s is missing.", campaign_maps[i]);
        file = fopen(path, "rb");
        if (!file) return fail(error, error_size, "%s could not be read.", campaign_maps[i]);
        got = fread(header, 1, sizeof(header), file);
        fclose(file);
        if (got != sizeof(header) || memcmp(header, "daeh", 4) || memcmp(header + SECTOR - 4, "toof", 4) || read32(header + 4) != 5)
            return fail(error, error_size, "%s is not an original Xbox Halo map.", campaign_maps[i]);
        memcpy(text, header + 64, 32);
        for (j = 0; j < COUNT(supported_builds); j++) if (!strcmp(text, supported_builds[j])) break;
        if (j == COUNT(supported_builds))
            return fail(error, error_size, "Unsupported Xbox map build %.32s in %s.", text, campaign_maps[i]);
        if (have_build && strcmp(build, text)) return fail(error, error_size, "The maps are from mixed map releases.");
        memcpy(build, text, sizeof(build));
        have_build = 1;
    }
    return 1;
}

/* ---- the image ---- */

struct image {
    int fd;
    uint64_t length;   /* bytes in the file */
    uint64_t limit;    /* the length the image is meant to have (xiso_plan's total) */
    uint64_t base;     /* where the game partition starts */
};

static int image_read(const struct image *image, uint64_t offset, void *buffer, size_t size) {
    unsigned char *out = buffer;
    while (size) {
        ssize_t got = pread(image->fd, out, size, (off_t)offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return 0;
        out += got; offset += (uint64_t)got; size -= (size_t)got;
    }
    return 1;
}

/* the offset of a sector's data, or UINT64_MAX when it and `size` bytes after it do not lie within the image */
static uint64_t extent(const struct image *image, uint32_t sector, uint64_t size) {
    uint64_t start = image->base + (uint64_t)sector * SECTOR;
    if (start > image->limit || size > image->limit - start) return UINT64_MAX;
    return start;
}

/* finds the game partition among the known places for it: the descriptor opens and closes with the magic text.
   Only the first `available` bytes of the file count. Returns 1 if found */
static int find_partition(struct image *image, uint64_t available) {
    size_t i;
    for (i = 0; i < COUNT(partition_bases); i++) {
        uint64_t at = partition_bases[i] + VOLUME_OFFSET;
        char head[MAGIC_SIZE], tail[MAGIC_SIZE];
        if (at + SECTOR > available) continue;
        if (!image_read(image, at, head, sizeof(head)) || memcmp(head, MAGIC, MAGIC_SIZE)) continue;
        if (!image_read(image, at + MAGIC_TAIL, tail, sizeof(tail)) || memcmp(tail, MAGIC, MAGIC_SIZE)) continue;
        image->base = partition_bases[i];
        return 1;
    }
    return 0;
}

static int image_open(struct image *image, const char *path, int writable) {
    struct stat info;
    memset(image, 0, sizeof(*image));
    image->fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (image->fd < 0) return 0;
    if (fstat(image->fd, &info) || info.st_size < 0) {close(image->fd); image->fd = -1; return 0;}
    image->length = image->limit = (uint64_t)info.st_size;
    return 1;
}

/* ---- directories ---- */

struct entry {
    const unsigned char *name;
    uint32_t sector, size;
    uint8_t attributes, length;
};

struct directory {
    unsigned char *table;
    struct entry *entries;
    size_t count;
};

enum { DIR_OK, DIR_IO, DIR_TRUNCATED, DIR_DAMAGED, DIR_WAITING };

static void directory_free(struct directory *directory) {
    free(directory->table);
    free(directory->entries);
    memset(directory, 0, sizeof(*directory));
}

static int directory_failure(int status, char *error, size_t error_size) {
    switch (status) {
    case DIR_TRUNCATED: return fail(error, error_size, "The disc image is truncated. Download it again, then choose it.");
    case DIR_DAMAGED: return fail(error, error_size, "The disc image has a damaged directory tree.");
    default: return fail(error, error_size, "Could not read the disc image.");
    }
}

/* reads the directory table at `sector` (`size` bytes) and visits its search tree from the first entry, each
   node once; a node whose left offset is 0xFFFF is padding, so an empty table has no entries.
   Only the first `available` bytes of the image count: a table not wholly there is DIR_WAITING */
static int directory_load(const struct image *image, uint64_t available, uint32_t sector, uint32_t size, struct directory *directory) {
    size_t words = size / 4, stack_count = 0, capacity = 0;
    unsigned char *seen = NULL;
    uint32_t *stack = NULL;
    uint64_t at;
    int status = DIR_OK;

    memset(directory, 0, sizeof(*directory));
    if (!size) return DIR_OK;
    if (size > DIRECTORY_MAXIMUM) return DIR_DAMAGED;
    at = extent(image, sector, size);
    if (at == UINT64_MAX) return DIR_TRUNCATED;
    if (at + size > available) return DIR_WAITING;
    directory->table = malloc(size);
    seen = calloc(words / 8 + 1, 1);
    stack = malloc((words + 1) * sizeof(*stack));
    if (!directory->table || !seen || !stack) {status = DIR_IO; goto done;}
    if (!image_read(image, at, directory->table, size)) {status = DIR_IO; goto done;}
    stack[stack_count++] = 0;
    seen[0] |= 1;
    while (stack_count) {
        size_t position = (size_t)stack[--stack_count] * 4;
        const unsigned char *node = directory->table + position;
        uint32_t left, right, name_length, child;
        int side;

        if (position + ENTRY_HEADER > size) {status = DIR_DAMAGED; goto done;}
        left = read16(node);
        right = read16(node + 2);
        if (left == 0xFFFF) continue;
        name_length = node[13];
        if (!name_length || position + ENTRY_HEADER + name_length > size) {status = DIR_DAMAGED; goto done;}
        if (directory->count == capacity) {
            struct entry *grown;
            capacity = capacity ? capacity * 2 : 16;
            grown = realloc(directory->entries, capacity * sizeof(*grown));
            if (!grown) {status = DIR_IO; goto done;}
            directory->entries = grown;
        }
        directory->entries[directory->count++] = (struct entry){
            .name = node + ENTRY_HEADER, .sector = read32(node + 4), .size = read32(node + 8),
            .attributes = node[12], .length = (uint8_t)name_length};
        for (side = 0; side < 2; side++) {
            child = side ? right : left;
            if (!child) continue;
            if (child >= words || (seen[child / 8] >> (child % 8) & 1)) {status = DIR_DAMAGED; goto done;}
            seen[child / 8] |= (unsigned char)(1u << (child % 8));
            stack[stack_count++] = child;
        }
    }
done:
    free(seen);
    free(stack);
    if (status != DIR_OK) directory_free(directory);
    return status;
}

static const struct entry *directory_find(const struct directory *directory, const char *name, int want_directory) {
    size_t i;
    for (i = 0; i < directory->count; i++) {
        const struct entry *entry = &directory->entries[i];
        if (!!(entry->attributes & DIRECTORY_BIT) == want_directory && equal_folded(entry->name, entry->length, name)) return entry;
    }
    return NULL;
}

/* a name that is one plain file name; false for anything that could reach outside its folder */
static int safe_name(const struct entry *entry) {
    size_t i;
    if (entry->length == 1 && entry->name[0] == '.') return 0;
    if (entry->length == 2 && entry->name[0] == '.' && entry->name[1] == '.') return 0;
    for (i = 0; i < entry->length; i++) if (entry->name[i] == '/' || entry->name[i] == '\\' || !entry->name[i]) return 0;
    return 1;
}

static int compare_names(const void *a, const void *b) {
    const struct entry *x = *(const struct entry *const *)a, *y = *(const struct entry *const *)b;
    size_t n = x->length < y->length ? x->length : y->length, i;
    for (i = 0; i < n; i++) {
        int c = tolower(x->name[i]) - tolower(y->name[i]);
        if (c) return c;
    }
    return (int)x->length - (int)y->length;
}

/* 1 if every name is safe and none repeats (ignoring case); else 0 with the reason */
static int names_usable(const struct directory *directory, char *error, size_t error_size) {
    const struct entry **sorted;
    size_t i;
    for (i = 0; i < directory->count; i++)
        if (!safe_name(&directory->entries[i])) return fail(error, error_size, "The disc image has an unsafe filename.");
    if (directory->count < 2) return 1;
    sorted = malloc(directory->count * sizeof(*sorted));
    if (!sorted) return fail(error, error_size, "Not enough memory to read the disc image.");
    for (i = 0; i < directory->count; i++) sorted[i] = &directory->entries[i];
    qsort(sorted, directory->count, sizeof(*sorted), compare_names);
    for (i = 1; i < directory->count; i++)
        if (!compare_names(&sorted[i - 1], &sorted[i])) {free(sorted); return fail(error, error_size, "The disc image has duplicate filenames.");}
    free(sorted);
    return 1;
}

/* ---- what an extraction reads ---- */

struct volume {
    uint32_t root_sector, root_size;
};

/* the volume descriptor of the partition find_partition found */
static int volume_read(const struct image *image, uint64_t available, struct volume *volume) {
    unsigned char descriptor[SECTOR];
    uint64_t at = image->base + VOLUME_OFFSET;
    if (at + SECTOR > available || !image_read(image, at, descriptor, sizeof(descriptor))) return 0;
    volume->root_sector = read32(descriptor + 0x14);
    volume->root_size = read32(descriptor + 0x18);
    return 1;
}

/* the folders and files an import copies */
struct analysis {
    struct volume volume;
    struct directory root, maps, movies;
    const struct entry *maps_entry;     /* maps, in root */
    const struct entry *movies_entry;   /* bink, in root */
    const struct entry *executable;     /* default.xbe, in root */
    const struct entry *picture;        /* loading.tga, in maps */
    int has_movies;
};

static void analysis_free(struct analysis *analysis) {
    directory_free(&analysis->root);
    directory_free(&analysis->maps);
    directory_free(&analysis->movies);
}

/* movies the import keeps: intro*.bik and credits*.bik */
static int wanted_movie(const struct entry *entry) {
    return !(entry->attributes & DIRECTORY_BIT) && safe_name(entry) && has_suffix_folded(entry->name, entry->length, ".bik") &&
        (has_prefix_folded(entry->name, entry->length, "intro") || has_prefix_folded(entry->name, entry->length, "credits"));
}

static int wanted_map(const struct entry *entry) {
    return !(entry->attributes & DIRECTORY_BIT) && has_suffix_folded(entry->name, entry->length, ".map");
}

/* opens the volume and the directories the import walks. Returns 1; or 0 with the reason in error;
   or -1 when `available` does not yet hold them (the reason is not an error) */
static int analyze(struct image *image, uint64_t available, struct analysis *analysis, char *error, size_t error_size) {
    int status;
    size_t i;

    memset(analysis, 0, sizeof(*analysis));
    if (!find_partition(image, available) || !volume_read(image, available, &analysis->volume))
        return available < image->limit ? -1 : fail(error, error_size, "This is not an Xbox XISO. Choose an uncompressed Halo: Combat Evolved .iso or .xiso file.");
    status = directory_load(image, available, analysis->volume.root_sector, analysis->volume.root_size, &analysis->root);
    if (status == DIR_WAITING) return -1;
    if (status != DIR_OK) return directory_failure(status, error, error_size);
    analysis->maps_entry = directory_find(&analysis->root, "maps", 1);
    if (!analysis->maps_entry) {analysis_free(analysis); return fail(error, error_size, "This disc image has no maps folder.");}
    status = directory_load(image, available, analysis->maps_entry->sector, analysis->maps_entry->size, &analysis->maps);
    if (status == DIR_WAITING) {analysis_free(analysis); return -1;}
    if (status != DIR_OK) {analysis_free(analysis); return directory_failure(status, error, error_size);}
    if (!names_usable(&analysis->maps, error, error_size)) {analysis_free(analysis); return 0;}
    for (i = 0; i < analysis->maps.count; i++) {
        const struct entry *entry = &analysis->maps.entries[i];
        if (wanted_map(entry) && extent(image, entry->sector, entry->size) == UINT64_MAX) {
            analysis_free(analysis);
            return directory_failure(DIR_TRUNCATED, error, error_size);
        }
    }
    analysis->picture = directory_find(&analysis->maps, "loading.tga", 0);
    analysis->executable = directory_find(&analysis->root, "default.xbe", 0);
#ifdef HALO_BINK
    analysis->movies_entry = directory_find(&analysis->root, "bink", 1);
    if (analysis->movies_entry) {
        status = directory_load(image, available, analysis->movies_entry->sector, analysis->movies_entry->size, &analysis->movies);
        if (status == DIR_WAITING) {analysis_free(analysis); return -1;}
        /* (a damaged movies folder is no reason to refuse the maps) */
        analysis->has_movies = status == DIR_OK;
    }
#endif
    return 1;
}

/* ---- the plan ---- */

static int compare_ranges(const void *a, const void *b) {
    const struct xiso_range *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

int xiso_free_range(int fd, uint64_t start, uint64_t end) {
#ifdef F_PUNCHHOLE
    struct statfs info;
    struct fpunchhole hole;
    uint64_t block;
    if (fstatfs(fd, &info) || !info.f_bsize) return 0;
    block = info.f_bsize;
    start = (start + block - 1) / block * block;
    end = end / block * block;
    if (end <= start) return 1;
    memset(&hole, 0, sizeof(hole));
    hole.fp_offset = (off_t)start;
    hole.fp_length = (off_t)(end - start);
    return fcntl(fd, F_PUNCHHOLE, &hole) == 0;
#else
    (void)fd; (void)start; (void)end;
    return 0;
#endif
}

struct plan {
    struct xiso_range *ranges;
    int maximum, count;
};

static void plan_add(struct plan *plan, const struct image *image, uint32_t sector, uint64_t size) {
    uint64_t at = extent(image, sector, size);
    if (!size || at == UINT64_MAX) return;
    if (plan->count < plan->maximum) plan->ranges[plan->count] = (struct xiso_range){at, at + size};
    plan->count++;
}

int xiso_plan(const char *image_path, uint64_t received, uint64_t total, struct xiso_range *ranges, int maximum) {
    struct image image;
    struct analysis analysis;
    struct plan plan = {ranges, maximum, 0};
    int merged = 0;
    size_t i;

    if (!image_open(&image, image_path, 0)) return 0;
    image.limit = total;
    if (analyze(&image, received < image.length ? received : image.length, &analysis, NULL, 0) != 1) {close(image.fd); return 0;}

    if (plan.count < maximum) plan.ranges[plan.count] = (struct xiso_range){image.base + VOLUME_OFFSET, image.base + VOLUME_OFFSET + SECTOR};
    plan.count++;
    plan_add(&plan, &image, analysis.volume.root_sector, analysis.volume.root_size);
    plan_add(&plan, &image, analysis.maps_entry->sector, analysis.maps_entry->size);
    for (i = 0; i < analysis.maps.count; i++) {
        const struct entry *entry = &analysis.maps.entries[i];
        if (wanted_map(entry) || entry == analysis.picture) plan_add(&plan, &image, entry->sector, entry->size);
    }
    if (analysis.executable) plan_add(&plan, &image, analysis.executable->sector, analysis.executable->size);
    if (analysis.has_movies) {
        plan_add(&plan, &image, analysis.movies_entry->sector, analysis.movies_entry->size);
        for (i = 0; i < analysis.movies.count; i++) {
            const struct entry *entry = &analysis.movies.entries[i];
            if (wanted_movie(entry)) plan_add(&plan, &image, entry->sector, entry->size);
        }
    }
    analysis_free(&analysis);
    close(image.fd);
    if (plan.count > maximum) return 0;
    qsort(ranges, (size_t)plan.count, sizeof(*ranges), compare_ranges);
    for (i = 0; i < (size_t)plan.count; i++) {
        if (merged && ranges[i].start <= ranges[merged - 1].end) {
            if (ranges[i].end > ranges[merged - 1].end) ranges[merged - 1].end = ranges[i].end;
        } else ranges[merged++] = ranges[i];
    }
    return merged;
}

/* ---- the extraction ---- */

struct run {
    struct image image;
    xiso_progress_proc progress;
    void *context;
    uint64_t done, total;
    unsigned char *buffer;
    int consume;
    char *error;
    size_t error_size;
    int cancelled;
};

/* reports the copy so far; 0 if the caller cancelled it */
static int tick(struct run *run, const char *file) {
    if (run->progress && !run->progress(run->context, file, run->done, run->total)) {
        run->cancelled = 1;
        return fail(run->error, run->error_size, "Import cancelled.");
    }
    return 1;
}

static void remove_folder_files(const char *path) {
    DIR *folder = opendir(path);
    struct dirent *item;
    if (folder) {
        while ((item = readdir(folder))) {
            char child[4096];
            if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
            snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
            unlink(child);
        }
        closedir(folder);
    }
    rmdir(path);
}

/* copies one file of the image into `folder` under its name in lower case; the file is new */
static int copy_file(struct run *run, const struct entry *entry, const char *folder) {
    char path[4096], name[256];
    uint64_t start = extent(&run->image, entry->sector, entry->size), at = start, left = entry->size;
    int fd;
    size_t i;

    for (i = 0; i < entry->length; i++) name[i] = (char)tolower(entry->name[i]);
    name[entry->length] = 0;
    snprintf(path, sizeof(path), "%s/%s", folder, name);
    if (start == UINT64_MAX) return directory_failure(DIR_TRUNCATED, run->error, run->error_size);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return fail(run->error, run->error_size, "Could not create %s. Check available storage.", name);
    while (left) {
        size_t chunk = left < COPY_BUFFER ? (size_t)left : COPY_BUFFER, written = 0;
        if (!image_read(&run->image, at, run->buffer, chunk)) {
            close(fd);
            return fail(run->error, run->error_size, "The disc image is truncated. Download it again, then choose it.");
        }
        while (written < chunk) {
            ssize_t put = write(fd, run->buffer + written, chunk - written);
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) {
                close(fd);
                return fail(run->error, run->error_size, "Could not write %s. Check available storage.", name);
            }
            written += (size_t)put;
        }
        at += chunk; left -= chunk; run->done += chunk;
        if (!tick(run, name)) {close(fd); return 0;}
    }
    if (fsync(fd) || close(fd)) return fail(run->error, run->error_size, "Could not write %s. Check available storage.", name);
    if (run->consume) xiso_free_range(run->image.fd, start, start + entry->size);
    return 1;
}

static int picture_header_valid(const unsigned char *h) {
    return h[0] == 0 && h[1] == 0 && h[2] == 2 && read16(h + 12) == LOADING_PICTURE_WIDTH &&
        read16(h + 14) == LOADING_PICTURE_HEIGHT && h[16] == 24;
}

/* whether the disc's own loading.tga is the 320 by 240 24-bit picture the game reads */
static int disc_picture_usable(struct run *run, const struct entry *entry) {
    unsigned char header[18];
    uint64_t at;
    if (!entry || entry->size < LOADING_PICTURE_TGA_BYTES) return 0;
    at = extent(&run->image, entry->sector, entry->size);
    return at != UINT64_MAX && image_read(&run->image, at, header, sizeof(header)) && picture_header_valid(header);
}

/* the disc without such a file (an NTSC disc) has the picture in its executable: write it into `folder` */
static void picture_from_executable(struct run *run, const struct entry *executable, const char *folder) {
    unsigned char *bytes, *pixels;
    char path[4096];
    uint64_t at;
    if (!executable || executable->size > EXECUTABLE_MAXIMUM || executable->size < LOADING_PICTURE_MINIMUM_RUNS) return;
    at = extent(&run->image, executable->sector, executable->size);
    if (at == UINT64_MAX) return;
    bytes = malloc(executable->size);
    pixels = malloc(LOADING_PICTURE_PIXELS);
    if (bytes && pixels && image_read(&run->image, at, bytes, executable->size) &&
        loading_picture_find(bytes, executable->size, pixels)) {
        snprintf(path, sizeof(path), "%s/loading.tga", folder);
        loading_picture_write(path, pixels);
    }
    free(bytes);
    free(pixels);
    if (run->consume) xiso_free_range(run->image.fd, at, at + executable->size);
}

static int extract(const char *image_path, const char *destination, xiso_progress_proc progress, void *context,
    char *error, size_t error_size, int consume) {
    struct run run;
    struct analysis analysis;
    char maps[4096], maps_partial[4096], movies[4096], movies_partial[4096];
    struct stat info;
    const struct entry **copies = NULL, **kept_movies = NULL;
    size_t i, copy_count = 0, movie_count = 0;
    int result = 0, analyzed = 0, picture_on_disc;

    memset(&run, 0, sizeof(run));
    run.progress = progress; run.context = context; run.consume = consume; run.error = error; run.error_size = error_size;
    run.image.fd = -1;
    snprintf(maps, sizeof(maps), "%s/maps", destination);
    snprintf(maps_partial, sizeof(maps_partial), "%s/maps.partial", destination);
    snprintf(movies, sizeof(movies), "%s/bink", destination);
    snprintf(movies_partial, sizeof(movies_partial), "%s/bink.partial", destination);
    if (!lstat(maps, &info)) return fail(error, error_size, "The import folder already contains maps.");
    if (!image_open(&run.image, image_path, consume)) return fail(error, error_size, "Could not open the disc image.");
    if (analyze(&run.image, run.image.length, &analysis, error, error_size) != 1) goto done;
    analyzed = 1;

    copies = malloc((analysis.maps.count + 1) * sizeof(*copies));
    kept_movies = malloc((analysis.movies.count + 1) * sizeof(*kept_movies));
    run.buffer = malloc(COPY_BUFFER);
    if (!copies || !kept_movies || !run.buffer) {fail(error, error_size, "Not enough memory to import the disc image."); goto done;}
    picture_on_disc = disc_picture_usable(&run, analysis.picture);
    for (i = 0; i < analysis.maps.count; i++) {
        const struct entry *entry = &analysis.maps.entries[i];
        if (wanted_map(entry) || (picture_on_disc && entry == analysis.picture)) {copies[copy_count++] = entry; run.total += entry->size;}
    }
    for (i = 0; analysis.has_movies && i < analysis.movies.count; i++) {
        const struct entry *entry = &analysis.movies.entries[i];
        if (wanted_movie(entry) && extent(&run.image, entry->sector, entry->size) != UINT64_MAX) {kept_movies[movie_count++] = entry; run.total += entry->size;}
    }

    if (mkdir(maps_partial, 0755)) {fail(error, error_size, "Could not create the import folder. Check available storage."); goto done;}
    if (!tick(&run, "")) goto done;
    for (i = 0; i < copy_count; i++) if (!copy_file(&run, copies[i], maps_partial)) goto done;
    if (!picture_on_disc) picture_from_executable(&run, analysis.executable, maps_partial);
    if (!xiso_maps_ready(maps_partial, error, error_size)) goto done;

    /* the movies are a bonus: a failure to copy them leaves them out, but a cancel stops the import */
    if (movie_count && lstat(movies, &info) && !mkdir(movies_partial, 0755)) {
        for (i = 0; i < movie_count; i++) {
            if (!copy_file(&run, kept_movies[i], movies_partial)) {
                if (run.cancelled) goto done;
                remove_folder_files(movies_partial);
                movie_count = 0;
                break;
            }
        }
    } else movie_count = 0;
    if (rename(maps_partial, maps)) {
        fail(error, error_size, "Could not finish the import. Check available storage.");
        goto done;
    }
    if (movie_count && rename(movies_partial, movies)) remove_folder_files(movies_partial);
    result = 1;
done:
    if (analyzed) analysis_free(&analysis);
    free(copies); free(kept_movies); free(run.buffer);
    if (run.image.fd >= 0) close(run.image.fd);
    return result;
}

int xiso_extract_maps(const char *image_path, const char *destination,
    xiso_progress_proc progress, void *context, char *error, size_t error_size) {
    return extract(image_path, destination, progress, context, error, error_size, 0);
}

int xiso_extract_maps_consuming(const char *image_path, const char *destination,
    xiso_progress_proc progress, void *context, char *error, size_t error_size) {
    return extract(image_path, destination, progress, context, error, error_size, 1);
}

/* ---- the listing ---- */

struct listing {
    struct image image;
    xiso_list_proc list;
    void *context;
    char *error;
    size_t error_size;
    size_t files;
    uint64_t visited[DEPTH_MAXIMUM + 1][2];
};

static int list_directory(struct listing *listing, uint32_t sector, uint32_t size, char *path, size_t path_length, int depth) {
    struct directory directory;
    size_t i;
    int status, ok = 1;

    if (depth > DEPTH_MAXIMUM) return directory_failure(DIR_DAMAGED, listing->error, listing->error_size);
    for (i = 0; i < (size_t)depth; i++)
        if (listing->visited[i][0] == sector && listing->visited[i][1] == size) return directory_failure(DIR_DAMAGED, listing->error, listing->error_size);
    listing->visited[depth][0] = sector; listing->visited[depth][1] = size;
    status = directory_load(&listing->image, listing->image.length, sector, size, &directory);
    if (status != DIR_OK) return directory_failure(status, listing->error, listing->error_size);
    if (!names_usable(&directory, listing->error, listing->error_size)) {directory_free(&directory); return 0;}
    for (i = 0; ok && i < directory.count; i++) {
        const struct entry *entry = &directory.entries[i];
        size_t length = path_length;
        if (length + entry->length + 2 >= 4096) {ok = directory_failure(DIR_DAMAGED, listing->error, listing->error_size); break;}
        memcpy(path + length, entry->name, entry->length);
        length += entry->length;
        path[length] = 0;
        if (entry->attributes & DIRECTORY_BIT) {
            if (entry->size) {
                path[length++] = '/'; path[length] = 0;
                ok = list_directory(listing, entry->sector, entry->size, path, length, depth + 1);
            }
        } else {
            uint64_t at = entry->size ? extent(&listing->image, entry->sector, entry->size) : 0;
            if (at == UINT64_MAX) ok = directory_failure(DIR_TRUNCATED, listing->error, listing->error_size);
            else if (++listing->files > LIST_FILES_MAXIMUM) ok = fail(listing->error, listing->error_size, "The disc image has too many files.");
            else if (!listing->list(listing->context, path, at, entry->size)) ok = fail(listing->error, listing->error_size, "Listing stopped.");
        }
    }
    directory_free(&directory);
    return ok;
}

int xiso_list(const char *image_path, xiso_list_proc list, void *context, char *error, size_t error_size) {
    struct listing *listing = calloc(1, sizeof(*listing));
    struct volume volume;
    char path[4096] = {0};
    int ok = 0;

    if (!listing) return fail(error, error_size, "Not enough memory to read the disc image.");
    listing->list = list; listing->context = context; listing->error = error; listing->error_size = error_size;
    if (!image_open(&listing->image, image_path, 0)) {free(listing); return fail(error, error_size, "Could not open the disc image.");}
    if (!find_partition(&listing->image, listing->image.length) || !volume_read(&listing->image, listing->image.length, &volume))
        fail(error, error_size, "This is not an Xbox XISO. Choose an uncompressed Halo: Combat Evolved .iso or .xiso file.");
    else ok = list_directory(listing, volume.root_sector, volume.root_size, path, 0, 0);
    close(listing->image.fd);
    free(listing);
    return ok;
}
