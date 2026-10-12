#ifndef HALO_XISO_H
#define HALO_XISO_H
#include <stddef.h>
#include <stdint.h>

/* Return zero to cancel. Called on the extraction thread. */
typedef int (*xiso_progress_proc)(void *context, const char *file, uint64_t done, uint64_t total);

/* Validate original Xbox cache headers and the complete campaign map set. */
int xiso_maps_ready(const char *maps, char *error, size_t error_size);

/* Extract into a new <destination>/maps, staging in maps.partial first.
   destination must be a private, existing directory. Existing files are never
   replaced. On failure the caller removes its private staging directory.
   Built with HALO_BINK, the disc's intro and credits movies (bink/intro*.bik,
   bink/credits*.bik), when it has them, also go into a new <destination>/bink,
   and xiso_plan below includes their bytes. */
int xiso_extract_maps(const char *image_path, const char *destination,
    xiso_progress_proc progress, void *context, char *error, size_t error_size);

/* An import that receives the image itself (the Apple TV's upload, host_tv_import.m) need not keep
   the whole disc: only the parts xiso_extract_maps reads.

   xiso_plan: once the first `received` bytes of an image of `total` bytes are in the file at
   image_path, the byte ranges an extraction will read (the volume descriptor, the root and maps
   directories, each .map file), sorted and merged, into ranges; returns their count, or 0 while
   the directories are not all there yet (or the file is no disc image, which the extraction then
   says). The receiver frees what it already wrote outside them and leaves the rest of the
   incoming image unwritten there, a hole in a file of the full length.

   xiso_extract_maps_consuming: as xiso_extract_maps, on an image the caller owns: each map's
   bytes in the image are freed (a hole) once the map is copied out and saved, so the import needs
   room for the maps once, not twice. The image is unusable afterwards, whether or not it succeeds. */
struct xiso_range { uint64_t start, end; };
enum { XISO_PLAN_RANGES = 300 };
int xiso_plan(const char *image_path, uint64_t received, uint64_t total, struct xiso_range *ranges, int maximum);
int xiso_extract_maps_consuming(const char *image_path, const char *destination,
    xiso_progress_proc progress, void *context, char *error, size_t error_size);
/* frees the file's storage from start to end (whole file-system blocks inside it); 0 if it could not */
int xiso_free_range(int fd, uint64_t start, uint64_t end);
#endif
