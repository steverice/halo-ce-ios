/* Apache-2.0. The loading screen's picture made larger by the on-device upscaler (port/ios/host/host_texture_upscale.m).

The loading screen (source/interface/progress_bar.c) lights the 320 by 240 gray picture of loading_picture.h. On a
device that makes upscaled textures the host also makes that picture 4 times larger, once, with the same model and
the path the graphics take (S4g, no back-projection fallback), and keeps it in the texture cache's folder as
loading-picture.bin (a ".bin", so the cache's size, its deletion and HALO_TEXTURE_UPSCALE_CLEAN all cover it). The
loading screen draws that file when it is there, was made from the picture it is about to draw, and the upscaled
textures are on; otherwise the original, exactly as before.

The file, little-endian: "VLPU", the version, the width and the height of the larger picture (1280 by 960), the
FNV-1a hash of the original picture's 76800 gray levels, rows from the top (so a re-imported picture is never drawn
beside a stale file), the recipe key (the model and this format's version: the host remakes a file made by another
recipe; the game does not look at it), and the FNV-1a hash of the payload; then the payload, the larger picture's gray
levels, rows from the top. Header-only, so a host probe can check it without the app. */
#ifndef HALO_LOADING_PICTURE_UPSCALED_H
#define HALO_LOADING_PICTURE_UPSCALED_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum
{
	LOADING_PICTURE_UPSCALED_SCALE = 4,
	LOADING_PICTURE_UPSCALED_SOURCE_WIDTH = 320,
	LOADING_PICTURE_UPSCALED_SOURCE_HEIGHT = 240,
	LOADING_PICTURE_UPSCALED_WIDTH = LOADING_PICTURE_UPSCALED_SCALE * LOADING_PICTURE_UPSCALED_SOURCE_WIDTH,
	LOADING_PICTURE_UPSCALED_HEIGHT = LOADING_PICTURE_UPSCALED_SCALE * LOADING_PICTURE_UPSCALED_SOURCE_HEIGHT,
	LOADING_PICTURE_UPSCALED_SOURCE_PIXELS = LOADING_PICTURE_UPSCALED_SOURCE_WIDTH * LOADING_PICTURE_UPSCALED_SOURCE_HEIGHT,
	LOADING_PICTURE_UPSCALED_PIXELS = LOADING_PICTURE_UPSCALED_WIDTH * LOADING_PICTURE_UPSCALED_HEIGHT,
	LOADING_PICTURE_UPSCALED_VERSION = 1,
	LOADING_PICTURE_UPSCALED_HEADER_BYTES = 40,
	LOADING_PICTURE_UPSCALED_FILE_BYTES = LOADING_PICTURE_UPSCALED_HEADER_BYTES + LOADING_PICTURE_UPSCALED_PIXELS,
};
#define LOADING_PICTURE_UPSCALED_FILE "loading-picture.bin"

/* (FNV-1a, 64 bits; seed it with 0 for a first call: it starts from the offset basis) */
static inline uint64_t loading_picture_upscaled_hash(const unsigned char *bytes, size_t size)
{
	uint64_t hash = 14695981039346656037ull;
	size_t at;

	for (at = 0; at < size; at++)
		hash = (hash ^ bytes[at]) * 1099511628211ull;
	return hash;
}

static inline void loading_picture_upscaled_put32(unsigned char *at, uint32_t value)
{
	at[0] = (unsigned char)value;
	at[1] = (unsigned char)(value >> 8);
	at[2] = (unsigned char)(value >> 16);
	at[3] = (unsigned char)(value >> 24);
}

static inline uint32_t loading_picture_upscaled_get32(const unsigned char *at)
{
	return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static inline void loading_picture_upscaled_put64(unsigned char *at, uint64_t value)
{
	loading_picture_upscaled_put32(at, (uint32_t)value);
	loading_picture_upscaled_put32(at + 4, (uint32_t)(value >> 32));
}

static inline uint64_t loading_picture_upscaled_get64(const unsigned char *at)
{
	return (uint64_t)loading_picture_upscaled_get32(at) | (uint64_t)loading_picture_upscaled_get32(at + 4) << 32;
}

/* the original picture's gray levels, rows from the top, from a 24-bit uncompressed 320 by 240 TGA the way the game
reads one (progress_bar.c: the file's first row is the picture's last, the level is the first channel; the 18-byte
header's own fields are not looked at past the type, size and depth); 0 if the file is not that */
static inline int loading_picture_upscaled_read_tga(const char *path, unsigned char *gray)
{
	unsigned char header[18], row[3 * LOADING_PICTURE_UPSCALED_SOURCE_WIDTH];
	FILE *file = fopen(path, "rb");
	int ok, x, y;

	if (!file)
		return 0;
	ok = fread(header, 1, sizeof(header), file) == sizeof(header) && header[2] == 2 && header[16] == 24 &&
		(header[12] | header[13] << 8) == LOADING_PICTURE_UPSCALED_SOURCE_WIDTH &&
		(header[14] | header[15] << 8) == LOADING_PICTURE_UPSCALED_SOURCE_HEIGHT;
	for (y = 0; ok && y < LOADING_PICTURE_UPSCALED_SOURCE_HEIGHT; y++)
	{
		ok = fread(row, 1, sizeof(row), file) == sizeof(row);
		for (x = 0; ok && x < LOADING_PICTURE_UPSCALED_SOURCE_WIDTH; x++)
			gray[(LOADING_PICTURE_UPSCALED_SOURCE_HEIGHT - 1 - y) * LOADING_PICTURE_UPSCALED_SOURCE_WIDTH + x] = row[3 * x];
	}
	fclose(file);
	return ok;
}

/* the file's header, then the payload (LOADING_PICTURE_UPSCALED_PIXELS bytes) after it, into bytes
(LOADING_PICTURE_UPSCALED_FILE_BYTES); source_hash is of the original's gray levels */
static inline void loading_picture_upscaled_encode(unsigned char *bytes, uint64_t source_hash, uint64_t recipe,
	const unsigned char *gray)
{
	memset(bytes, 0, LOADING_PICTURE_UPSCALED_HEADER_BYTES);
	memcpy(bytes, "VLPU", 4);
	loading_picture_upscaled_put32(bytes + 4, LOADING_PICTURE_UPSCALED_VERSION);
	loading_picture_upscaled_put32(bytes + 8, LOADING_PICTURE_UPSCALED_WIDTH);
	loading_picture_upscaled_put32(bytes + 12, LOADING_PICTURE_UPSCALED_HEIGHT);
	loading_picture_upscaled_put64(bytes + 16, source_hash);
	loading_picture_upscaled_put64(bytes + 24, recipe);
	loading_picture_upscaled_put64(bytes + 32, loading_picture_upscaled_hash(gray, LOADING_PICTURE_UPSCALED_PIXELS));
	memcpy(bytes + LOADING_PICTURE_UPSCALED_HEADER_BYTES, gray, LOADING_PICTURE_UPSCALED_PIXELS);
}

/* whether a file's first LOADING_PICTURE_UPSCALED_HEADER_BYTES are those of a current file for this source and,
when recipe is not 0, this recipe */
static inline int loading_picture_upscaled_header_current(const unsigned char *header, uint64_t source_hash,
	uint64_t recipe)
{
	return !memcmp(header, "VLPU", 4) && loading_picture_upscaled_get32(header + 4) == LOADING_PICTURE_UPSCALED_VERSION &&
		loading_picture_upscaled_get32(header + 8) == LOADING_PICTURE_UPSCALED_WIDTH &&
		loading_picture_upscaled_get32(header + 12) == LOADING_PICTURE_UPSCALED_HEIGHT &&
		loading_picture_upscaled_get64(header + 16) == source_hash &&
		(!recipe || loading_picture_upscaled_get64(header + 24) == recipe);
}

/* the whole file, header and payload checked: 1 if it is a current file whose payload is intact */
static inline int loading_picture_upscaled_check(const unsigned char *bytes, size_t size, uint64_t source_hash,
	uint64_t recipe)
{
	return size == LOADING_PICTURE_UPSCALED_FILE_BYTES &&
		loading_picture_upscaled_header_current(bytes, source_hash, recipe) &&
		loading_picture_upscaled_get64(bytes + 32) ==
			loading_picture_upscaled_hash(bytes + LOADING_PICTURE_UPSCALED_HEADER_BYTES, LOADING_PICTURE_UPSCALED_PIXELS);
}

/* the file in `directory`, read whole into bytes (LOADING_PICTURE_UPSCALED_FILE_BYTES) and checked; 0 if there is
none, it is another size, or it is not current for this source */
static inline int loading_picture_upscaled_load(const char *directory, uint64_t source_hash, uint64_t recipe,
	unsigned char *bytes)
{
	char path[1100];
	FILE *file;
	int ok;

	if (snprintf(path, sizeof(path), "%s/%s", directory, LOADING_PICTURE_UPSCALED_FILE) >= (int)sizeof(path))
		return 0;
	file = fopen(path, "rb");
	if (!file)
		return 0;
	ok = fread(bytes, 1, LOADING_PICTURE_UPSCALED_FILE_BYTES, file) == LOADING_PICTURE_UPSCALED_FILE_BYTES &&
		fgetc(file) == EOF;
	fclose(file);
	return ok && loading_picture_upscaled_check(bytes, LOADING_PICTURE_UPSCALED_FILE_BYTES, source_hash, recipe);
}

/* the file in `directory`, written through a .bin.tmp and a rename; 0 if it could not be */
static inline int loading_picture_upscaled_store(const char *directory, const unsigned char *bytes)
{
	char path[1100], temporary[1112];
	FILE *file;
	int ok;

	if (snprintf(path, sizeof(path), "%s/%s", directory, LOADING_PICTURE_UPSCALED_FILE) >= (int)sizeof(path))
		return 0;
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	file = fopen(temporary, "wb");
	if (!file)
		return 0;
	ok = fwrite(bytes, 1, LOADING_PICTURE_UPSCALED_FILE_BYTES, file) == LOADING_PICTURE_UPSCALED_FILE_BYTES;
	ok = fclose(file) == 0 && ok;
	ok = ok && rename(temporary, path) == 0;
	if (!ok)
		remove(temporary);
	return ok;
}

/* the picture's gray level out of the pixel the game builds from it (progress_bar.c tgaLoadImageData: the level in
the red byte) */
static inline unsigned char loading_picture_upscaled_level(unsigned long pixel)
{
	return (unsigned char)(pixel >> 16);
}

#endif
