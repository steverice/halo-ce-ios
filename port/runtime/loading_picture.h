/*
LOADING_PICTURE.H

The loading screen's picture, from the player's own disc.

The loading screen (source/interface/progress_bar.c) lights a 320 by 240
gray picture: the ring, with LOADING... across it. The PAL disc carries it
as maps\loading.tga, which the game reads. The NTSC disc has no such file:
its executable (default.xbe) holds the picture itself, as its width and
height (two 32-bit integers, 320 and 240) followed by runs, one byte each:
the high four bits a count of pixels (1 to 15), the low four bits their
gray level (times 16), rows from the top, 76800 pixels in all.

The importers (xiso.c) use this to write maps/loading.tga from an NTSC
disc's executable, in the form the game reads: an uncompressed 24-bit
picture, 320 by 240, rows from the bottom. Nothing of the picture is in this
repository; it exists only in the player's game data.
tools/loading_picture.py does the same for game data imported before this.
*/
#ifndef HALO_LOADING_PICTURE_H
#define HALO_LOADING_PICTURE_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum
{
	LOADING_PICTURE_WIDTH = 320,
	LOADING_PICTURE_HEIGHT = 240,
	LOADING_PICTURE_PIXELS = LOADING_PICTURE_WIDTH * LOADING_PICTURE_HEIGHT,
	LOADING_PICTURE_TGA_BYTES = 18 + 3 * LOADING_PICTURE_PIXELS,
	/* the least runs a picture can have, and the most this looks through */
	LOADING_PICTURE_MINIMUM_RUNS = LOADING_PICTURE_PIXELS / 15,
	LOADING_PICTURE_MAXIMUM_RUNS = LOADING_PICTURE_PIXELS,
};

/* the picture in an executable's bytes: its gray levels, rows from the top,
into pixels (LOADING_PICTURE_PIXELS bytes); 0 if the executable has none */
static inline int loading_picture_find(const unsigned char *executable, size_t size, unsigned char *pixels)
{
	static const unsigned char dimensions[8] = { 0x40, 0x01, 0, 0, 0xf0, 0, 0, 0 };
	size_t offset;

	for (offset = 0; offset + sizeof(dimensions) + LOADING_PICTURE_MINIMUM_RUNS <= size; offset++)
	{
		const unsigned char *runs = executable + offset + sizeof(dimensions);
		size_t left = size - offset - sizeof(dimensions), run, filled = 0, changes = 0;

		if (executable[offset] != dimensions[0] || memcmp(executable + offset, dimensions, sizeof(dimensions)))
			continue;
		for (run = 0; run < left && run < LOADING_PICTURE_MAXIMUM_RUNS && filled < LOADING_PICTURE_PIXELS; run++)
		{
			size_t count = runs[run] >> 4;

			if (!count || filled + count > LOADING_PICTURE_PIXELS)
				break;
			memset(pixels + filled, (runs[run] & 15) << 4, count);
			changes += run && (runs[run] & 15) != (runs[run - 1] & 15);
			filled += count;
		}
		/* (a picture, not a stretch of one value that happens to add up) */
		if (filled == LOADING_PICTURE_PIXELS && changes >= 256)
			return 1;
	}
	return 0;
}

/* writes the picture as the file the game reads; 0 if it can't be written */
static inline int loading_picture_write(const char *path, const unsigned char *pixels)
{
	unsigned char header[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		LOADING_PICTURE_WIDTH & 255, LOADING_PICTURE_WIDTH >> 8, LOADING_PICTURE_HEIGHT & 255,
		LOADING_PICTURE_HEIGHT >> 8, 24, 0 };
	unsigned char row[3 * LOADING_PICTURE_WIDTH];
	FILE *file = fopen(path, "wb");
	int ok, x, y;

	if (!file)
		return 0;
	ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
	for (y = LOADING_PICTURE_HEIGHT - 1; ok && y >= 0; y--)
	{
		for (x = 0; x < LOADING_PICTURE_WIDTH; x++)
			row[3 * x] = row[3 * x + 1] = row[3 * x + 2] = pixels[y * LOADING_PICTURE_WIDTH + x];
		ok = fwrite(row, 1, sizeof(row), file) == sizeof(row);
	}
	if (fclose(file))
		ok = 0;
	if (!ok)
		remove(path);
	return ok;
}

#endif
