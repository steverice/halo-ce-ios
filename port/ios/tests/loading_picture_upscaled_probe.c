/* loading_picture_upscaled.h: the larger loading picture's file, its checks and the TGA read */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "loading_picture_upscaled.h"

int main(void)
{
	unsigned char *gray = malloc(LOADING_PICTURE_UPSCALED_SOURCE_PIXELS), *larger = malloc(LOADING_PICTURE_UPSCALED_PIXELS);
	unsigned char *file = malloc(LOADING_PICTURE_UPSCALED_FILE_BYTES), *loaded = malloc(LOADING_PICTURE_UPSCALED_FILE_BYTES);
	char directory[] = "/tmp/loading-picture-probe-XXXXXX", path[256], tga[256];
	unsigned char header[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 1, 240, 0, 24, 0 }, row[3 * 320];
	uint64_t hash;
	FILE *handle;
	size_t i;
	int x, y;

	assert(gray && larger && file && loaded && mkdtemp(directory));
	assert(LOADING_PICTURE_UPSCALED_WIDTH == 1280 && LOADING_PICTURE_UPSCALED_HEIGHT == 960);
	assert(LOADING_PICTURE_UPSCALED_FILE_BYTES == 40 + 1228800);
	for (i = 0; i < LOADING_PICTURE_UPSCALED_SOURCE_PIXELS; i++)
		gray[i] = (unsigned char)(i * 7 + i / 320);
	for (i = 0; i < LOADING_PICTURE_UPSCALED_PIXELS; i++)
		larger[i] = (unsigned char)(i * 13 + i / 1280);
	hash = loading_picture_upscaled_hash(gray, LOADING_PICTURE_UPSCALED_SOURCE_PIXELS);
	assert(hash != loading_picture_upscaled_hash(gray, LOADING_PICTURE_UPSCALED_SOURCE_PIXELS - 1));
	assert(loading_picture_upscaled_hash((const unsigned char *)"", 0) == 14695981039346656037ull);

	/* nothing there, then a stored file, round trip */
	assert(!loading_picture_upscaled_load(directory, hash, 0, loaded));
	loading_picture_upscaled_encode(file, hash, 99, larger);
	assert(loading_picture_upscaled_store(directory, file));
	assert(loading_picture_upscaled_load(directory, hash, 0, loaded));
	assert(!memcmp(loaded + LOADING_PICTURE_UPSCALED_HEADER_BYTES, larger, LOADING_PICTURE_UPSCALED_PIXELS));
	/* the game does not know the recipe; the host does, and remakes a file of another recipe */
	assert(loading_picture_upscaled_load(directory, hash, 99, loaded));
	assert(!loading_picture_upscaled_load(directory, hash, 100, loaded));
	/* a re-imported picture is never drawn beside a stale file */
	gray[5] ^= 1;
	assert(!loading_picture_upscaled_load(directory, loading_picture_upscaled_hash(gray, LOADING_PICTURE_UPSCALED_SOURCE_PIXELS), 0, loaded));
	gray[5] ^= 1;
	/* damage to the payload, a short file, a long file, a wrong version or size */
	file[LOADING_PICTURE_UPSCALED_HEADER_BYTES + 1000] ^= 0x40;
	assert(!loading_picture_upscaled_check(file, LOADING_PICTURE_UPSCALED_FILE_BYTES, hash, 0));
	file[LOADING_PICTURE_UPSCALED_HEADER_BYTES + 1000] ^= 0x40;
	assert(loading_picture_upscaled_check(file, LOADING_PICTURE_UPSCALED_FILE_BYTES, hash, 0));
	assert(!loading_picture_upscaled_check(file, LOADING_PICTURE_UPSCALED_FILE_BYTES - 1, hash, 0));
	assert(!loading_picture_upscaled_check(file, LOADING_PICTURE_UPSCALED_FILE_BYTES + 1, hash, 0));
	file[4] = 2;
	assert(!loading_picture_upscaled_check(file, LOADING_PICTURE_UPSCALED_FILE_BYTES, hash, 0));
	file[4] = 1;
	file[9] ^= 1;
	assert(!loading_picture_upscaled_check(file, LOADING_PICTURE_UPSCALED_FILE_BYTES, hash, 0));
	file[9] ^= 1;
	memcpy(file, "VLPX", 4);
	assert(!loading_picture_upscaled_check(file, LOADING_PICTURE_UPSCALED_FILE_BYTES, hash, 0));
	/* a file on disk of the wrong size */
	snprintf(path, sizeof(path), "%s/%s", directory, LOADING_PICTURE_UPSCALED_FILE);
	handle = fopen(path, "ab");
	assert(handle && fputc(0, handle) == 0 && !fclose(handle));
	assert(!loading_picture_upscaled_load(directory, hash, 0, loaded));
	/* no temporary is left behind */
	snprintf(tga, sizeof(tga), "%s.tmp", path);
	assert(access(tga, F_OK) != 0);
	assert(!unlink(path));

	/* the game's reading of a TGA: first file row is the picture's last, the first channel is the level */
	snprintf(tga, sizeof(tga), "%s/loading.tga", directory);
	handle = fopen(tga, "wb");
	assert(handle && fwrite(header, 1, sizeof(header), handle) == sizeof(header));
	for (y = 239; y >= 0; y--)
	{
		for (x = 0; x < 320; x++)
		{
			row[3 * x] = gray[y * 320 + x];
			row[3 * x + 1] = row[3 * x + 2] = 0x55;
		}
		assert(fwrite(row, 1, sizeof(row), handle) == sizeof(row));
	}
	assert(!fclose(handle));
	memset(loaded, 0, LOADING_PICTURE_UPSCALED_SOURCE_PIXELS);
	assert(loading_picture_upscaled_read_tga(tga, loaded));
	assert(!memcmp(loaded, gray, LOADING_PICTURE_UPSCALED_SOURCE_PIXELS));
	/* not that file: compressed, 32-bit, another size, truncated, missing */
	header[2] = 10;
	handle = fopen(tga, "r+b");
	assert(handle && fwrite(header, 1, sizeof(header), handle) == sizeof(header) && !fclose(handle));
	assert(!loading_picture_upscaled_read_tga(tga, loaded));
	header[2] = 2;
	header[16] = 32;
	handle = fopen(tga, "r+b");
	assert(handle && fwrite(header, 1, sizeof(header), handle) == sizeof(header) && !fclose(handle));
	assert(!loading_picture_upscaled_read_tga(tga, loaded));
	header[16] = 24;
	header[12] = 0;
	header[13] = 1;
	handle = fopen(tga, "r+b");
	assert(handle && fwrite(header, 1, sizeof(header), handle) == sizeof(header) && !fclose(handle));
	assert(!loading_picture_upscaled_read_tga(tga, loaded));
	header[12] = 64;
	header[13] = 1;
	handle = fopen(tga, "r+b");
	assert(handle && fwrite(header, 1, sizeof(header), handle) == sizeof(header) && !fclose(handle));
	assert(loading_picture_upscaled_read_tga(tga, loaded));
	assert(!truncate(tga, 18 + 3 * 320 * 100));
	assert(!loading_picture_upscaled_read_tga(tga, loaded));
	assert(!unlink(tga));
	assert(!loading_picture_upscaled_read_tga(tga, loaded));

	/* the level the game takes from its pixel (tgaLoadImageData) */
	for (i = 0; i < 256; i++)
		assert(loading_picture_upscaled_level(((i << 9) | (i & ~(size_t)1)) << 7 | i >> 2) == i);
	assert(!rmdir(directory));
	free(gray);
	free(larger);
	free(file);
	free(loaded);
	puts("loading_picture_upscaled_probe: ok");
	return 0;
}
