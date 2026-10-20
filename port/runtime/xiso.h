/*
XISO.H

Reads the maps folder (and the intro and credits movies) out of an original
Xbox disc image, for the Apple importers (xiso.c).
*/
#ifndef HALO_XISO_H
#define HALO_XISO_H
#include <stddef.h>
#include <stdint.h>

/* Called as the copy goes with the file being written and the bytes copied of
   the total. Return zero to cancel. Called on the extraction thread. */
typedef int (*xiso_progress_proc)(void *context, const char *file, uint64_t done, uint64_t total);

/* Checks a folder of extracted maps: every campaign map is there, carries an
   original Xbox cache header, and all of them are from one release. error and
   error_size may be NULL and 0. Returns nonzero when the folder is complete. */
int xiso_maps_ready(const char *maps, char *error, size_t error_size);

/* Copies the image's maps folder to a new <destination>/maps, staging it in
   maps.partial first. destination must be a private, existing directory.
   Existing files are never replaced. On failure the caller removes its
   private staging directory. Built with HALO_BINK, the disc's intro and
   credits movies (bink/intro*.bik, bink/credits*.bik), when it has them, go
   into a new <destination>/bink, and xiso_plan includes their bytes. Returns
   nonzero on success, or zero with the reason for the player in error. */
int xiso_extract_maps(const char *image_path, const char *destination,
    xiso_progress_proc progress, void *context, char *error, size_t error_size);

/* An import that receives the image itself (the Apple TV's upload) need not
   keep the whole disc, only the parts xiso_extract_maps reads.

   xiso_plan: once the first `received` bytes of an image of `total` bytes are
   in the file at image_path, the byte ranges an extraction reads (the volume
   descriptor, the directories it walks, each file it copies), sorted and merged,
   into ranges; returns their count, or 0 while the directories have not all
   arrived (or the file is no disc image, which the extraction then says). The
   receiver frees what it already wrote outside them and leaves the rest of the
   incoming image unwritten, a hole in a file of the full length.

   xiso_extract_maps_consuming: as xiso_extract_maps, on an image the caller
   owns: each file's bytes in the image are freed (a hole) once the file is
   copied out and saved, so the import needs room for the maps once, not twice.
   The image is unusable afterwards, whether or not it succeeds. */
struct xiso_range { uint64_t start, end; };
enum { XISO_PLAN_RANGES = 300 };
int xiso_plan(const char *image_path, uint64_t received, uint64_t total, struct xiso_range *ranges, int maximum);
int xiso_extract_maps_consuming(const char *image_path, const char *destination,
    xiso_progress_proc progress, void *context, char *error, size_t error_size);

/* Frees the file's storage from start to end (the whole file-system blocks
   inside it); returns 0 if it could not. */
int xiso_free_range(int fd, uint64_t start, uint64_t end);

/* Lists every file in the image: path (folders joined with "/"), the file's
   byte offset in the image and its size. Return zero from the callback to stop.
   Returns nonzero when the whole listing was made. Used by tests. */
typedef int (*xiso_list_proc)(void *context, const char *path, uint64_t offset, uint64_t size);
int xiso_list(const char *image_path, xiso_list_proc list, void *context, char *error, size_t error_size);
#endif
