/*
HUD_HIRES.C

The high-res HUD's textures (hud_hires.h): which one stands for a bitmap being
uploaded, and each one's GL texture.

Which bitmap is at an address the game knows (from the loaded map's tags:
port/linux/game/hud_hires_tags.c). Each texture is decoded from its PNG when
needed and kept: common startup sheets decode ahead on a bounded worker;
the rendering thread uploads them on first draw. Up to 69 of the HUD's
textures take about 225 MB with their mip
levels, though a game draws only some (the scopes' only when zoomed), and
the titles of the menus shown, about 3 MB each (11 MB for the carnage
report's, a whole panel).
They are drawn with linear filtering and their mip levels (d3d8_device.c,
configure_sampler), as they are larger than they appear.

The PNGs are the ones tools/hud_assets.py and title_assets.py write, so only
what they write is read: 8-bit RGBA, not interlaced, its data inflated with
the port's zlib (port/third_party/zlib: a menus folder's PNGs are anyone's).
*/

#include "hud_hires.h"
#include "platform.h"
#include "port_config.h"
#include "xgpu.h"
#include "gpu.h"

#include "zlib_prefixed.h"

#include <stdlib.h>
#include <string.h>
#ifndef HALO_IOS
#include <sys/resource.h>
#endif

/* the game's (port/linux/game/hud_hires_tags.c) */
long hud_hires_asset_at(unsigned long address, long width, long height);

#define MAXIMUM_TEXTURES 128
/* Preserve the existing size admission limit for custom menu PNGs. Rows
 * now reconstruct and compact in one allocation, halving full-image scratch. */
#define MAXIMUM_DECODED_SIZE (192UL << 20)

static struct
{
	unsigned int texture;
	unsigned long levels;
	int failed;
	int other_pixels_logged;
} textures[MAXIMUM_TEXTURES];

long hud_hires_asset_count(void)
{
	return hud_hires_embedded_count < MAXIMUM_TEXTURES ? (long)hud_hires_embedded_count : MAXIMUM_TEXTURES;
}

char const *hud_hires_asset_tag(long asset)
{
	return hud_hires_embedded[asset].tag;
}

long hud_hires_asset_bitmap(long asset)
{
	return hud_hires_embedded[asset].bitmap;
}

/* whether the texture can stand for a bitmap of this size: a whole multiple
of it, the same both ways */
long hud_hires_asset_fits(long asset, long width, long height)
{
	const struct hud_hires_embedded *embedded = &hud_hires_embedded[asset];

	return width > 0 && height > 0 && embedded->width % width == 0 && embedded->height % height == 0 &&
		embedded->width / width == embedded->height / height && embedded->width / width > 1;
}

/* Changed settings since the previous rendered frame: HUD bit 0, titles bit 1.
 * Pending menu edits do not change config_changes(). */
unsigned long hud_hires_settings_changed(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static unsigned long previous;
	unsigned long current, changed;

	if (read_at == config_changes()) return 0;
	current = (config_boolean("display.high_res_hud") ? 1UL : 0UL) |
		(config_boolean("display.high_res_text") ? 2UL : 0UL);
	changed = read_at == (unsigned long)-1 ? 0 : previous ^ current;
	read_at = config_changes();
	previous = current;
	return changed;
}

int hud_hires_bitmap_affected(unsigned long address, long width, long height, unsigned long changed)
{
	long asset = hud_hires_asset_at(address, width, height);

	return asset >= 0 && asset < hud_hires_asset_count() &&
		(changed & (hud_hires_embedded[asset].title ? 2UL : 1UL)) != 0;
}

long hud_hires_override_find(unsigned long address, unsigned long width, unsigned long height,
	unsigned long level0_size)
{
	static int hud_enabled, titles_enabled;
	static unsigned long read_at = (unsigned long)-1;
	long asset;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		hud_enabled = config_boolean("display.high_res_hud");
		titles_enabled = config_boolean("display.high_res_text");
	}
	if (!hud_enabled && !titles_enabled)
		return -1;
	asset = hud_hires_asset_at(address, (long)width, (long)height);
	if (asset < 0 || asset >= hud_hires_asset_count())
		return -1;
	if (!(hud_hires_embedded[asset].title ? titles_enabled : hud_enabled))
		return -1;
	if (crc32(0L, (const Bytef *)address, (uInt)level0_size) != hud_hires_embedded[asset].crc)
	{
		if (!textures[asset].other_pixels_logged)
		{
			platform_log("high-res hud: %s bitmap %d is not the one its texture was drawn for here "
				"(another language's or a modified map): drawn as it is",
				hud_hires_embedded[asset].tag, hud_hires_embedded[asset].bitmap);
			textures[asset].other_pixels_logged = 1;
		}
		return -1;
	}
	return asset;
}

int hud_hires_override_coverage(long asset)
{
	return asset >= 0 && asset < hud_hires_asset_count() && hud_hires_embedded[asset].coverage;
}

int hud_hires_override_point_threshold(long asset)
{
	return asset >= 0 && asset < hud_hires_asset_count() && hud_hires_embedded[asset].point_threshold;
}

/* ---------- decoding */

static unsigned long big_endian_long(const unsigned char *bytes)
{
	return ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) |
		((unsigned long)bytes[2] << 8) | bytes[3];
}

static unsigned char paeth(unsigned char left, unsigned char up, unsigned char up_left)
{
	int estimate = (int)left + up - up_left;
	int to_left = abs(estimate - left), to_up = abs(estimate - up), to_up_left = abs(estimate - up_left);

	if (to_left <= to_up && to_left <= to_up_left)
		return left;
	return to_up <= to_up_left ? up : up_left;
}

/* The PNG's texels, BGRA in rows top first, and its size; NULL if it is not
one that tools/hud_assets.py writes */
static unsigned char *png_decode(const unsigned char *data, unsigned long size, unsigned long *png_width,
	unsigned long *png_height)
{
	unsigned long position = 8;
	unsigned long width = size >= 33 ? big_endian_long(data + 16) : 0;
	unsigned long height = size >= 33 ? big_endian_long(data + 20) : 0;
	unsigned long stride = width * 4, filtered_size = height * (stride + 1);
	unsigned char *compressed = NULL, *filtered = NULL, *pixels = NULL;
	unsigned long compressed_size = 0, row, column;
	uLongf inflated_size = filtered_size;
	int result;

	if (size < 33 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) || memcmp(data + 12, "IHDR", 4) ||
		!width || !height || width > 8192 || height > 8192 ||
		data[24] != 8 || data[25] != 6 || data[28] != 0)
		return NULL;
	if (filtered_size + stride * height > MAXIMUM_DECODED_SIZE)
	{
		platform_log("png: %lux%lu is too large to decode (more than %lu MB)", width, height,
			MAXIMUM_DECODED_SIZE >> 20);
		return NULL;
	}
	*png_width = width;
	*png_height = height;
	compressed = malloc(size);
	while (compressed && position + 12 <= size)
	{
		unsigned long length = big_endian_long(data + position);

		if (length > size - position - 12)
			break;
		if (!memcmp(data + position + 4, "IDAT", 4))
		{
			memcpy(compressed + compressed_size, data + position + 8, length);
			compressed_size += length;
		}
		else if (!memcmp(data + position + 4, "IEND", 4))
		{
			break;
		}
		position += 12 + length;
	}
	filtered = compressed ? malloc(filtered_size) : NULL;
	pixels = filtered;
	if (!pixels)
		goto failed;
	result = uncompress(filtered, &inflated_size, compressed, compressed_size);
	/* (the game's zlib is 1.1, which can stop short of saying the stream has
	ended when the output is exactly full: all of it is enough) */
	if ((result != Z_OK && result != Z_BUF_ERROR) || inflated_size != filtered_size)
		goto failed;
	/* (each row by its filter, the first pixel's 4 bytes, which have none to
	their left, apart; the first row has none above: zeroes) */
	for (row = 0; row < height; row++)
	{
		const unsigned char *line = filtered + row * (stride + 1) + 1;
		unsigned char filter = line[-1];
		unsigned char *out = filtered + row * (stride + 1) + 1;
		const unsigned char *above = row ? out - stride - 1 : NULL;

		if (filter > 4)
			goto failed;
		if (!above && filter == 2)
			filter = 0; /* (up: zero) */
		else if (!above && filter == 4)
			filter = 1; /* (Paeth of left, zero and zero: left) */
		switch (filter)
		{
		case 0:
			break;
		case 1:
			for (column = 4; column < stride; column++)
				out[column] = (unsigned char)(line[column] + out[column - 4]);
			break;
		case 2:
			for (column = 0; column < stride; column++)
				out[column] = (unsigned char)(line[column] + above[column]);
			break;
		case 3:
			for (column = 0; column < 4; column++)
				out[column] = (unsigned char)(line[column] + (above ? above[column] : 0) / 2);
			for (column = 4; column < stride; column++)
				out[column] = (unsigned char)(line[column] +
					((unsigned)out[column - 4] + (above ? above[column] : 0)) / 2);
			break;
		default:
			/* (Paeth of zero, up and zero: up) */
			for (column = 0; column < 4; column++)
				out[column] = (unsigned char)(line[column] + above[column]);
			for (column = 4; column < stride; column++)
				out[column] = (unsigned char)(line[column] + paeth(out[column - 4], above[column], above[column - 4]));
			break;
		}
	}
	/* Previous rows must stay RGBA until all filters have been reconstructed.
	 * Compact forward only afterward; source and destination can overlap. */
	for (row = 0; row < height; row++)
	{
		const unsigned char *line = filtered + row * (stride + 1) + 1;
		unsigned char *out = pixels + row * stride;

		for (column = 0; column < stride; column += 4)
		{
			unsigned char red = line[column], green = line[column + 1];
			unsigned char blue = line[column + 2], alpha = line[column + 3];

			out[column] = blue;
			out[column + 1] = green;
			out[column + 2] = red;
			out[column + 3] = alpha;
		}
	}
	free(compressed);
	return pixels;

failed:
	free(compressed);
	free(filtered);
	return NULL;
}

static unsigned int png_upload(unsigned char *pixels, unsigned long width, unsigned long height,
	unsigned long *levels)
{
	unsigned long largest;
	struct gpu_texture_description description = { 0 };
	gpu_texture texture;

	*levels = 1;
	for (largest = width > height ? width : height; largest > 1; largest >>= 1)
		(*levels)++;
	description.type = GPU_TEXTURE_2D;
	description.format = GPU_FORMAT_BGRA8;
	description.usage = GPU_USAGE_UPLOAD;
	description.width = (uint32_t)width;
	description.height = (uint32_t)height;
	description.depth = 1;
	description.levels = (uint32_t)*levels;
	texture = gpu_texture_create(&description);
	if (!texture) return 0;
	gpu_texture_upload(texture, 0, 0, pixels, (uint32_t)(width * height * 4));
	gpu_texture_generate_mipmaps(texture, 0);
	return texture;
}

unsigned int hud_hires_png_texture(const void *png, unsigned long size, unsigned long *levels)
{
	unsigned long width = 0, height = 0;
	unsigned char *pixels = png_decode(png, size, &width, &height);
	unsigned int texture = pixels ? png_upload(pixels, width, height, levels) : 0;

	free(pixels);
	return texture;
}

/* ---------- bounded startup decoding */

#define PRELOAD_LIMIT (32UL << 20)
/* Allow for one guest stack and a conservative zlib inflate workspace.
 * Native stacks are reaped asynchronously after guest join; that runtime
 * overhead can briefly overlap on map changes, outside the decoded buffers. */
#define PRELOAD_STACK (1UL << 20)
#define PRELOAD_INFLATE (256UL << 10)

enum { PRELOAD_NONE, PRELOAD_PENDING, PRELOAD_DECODING, PRELOAD_READY };
static struct
{
	int state;
	unsigned char *pixels;
	unsigned long width, height, bytes;
} preload[MAXIMUM_TEXTURES];
static pthread_mutex_t preload_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t preload_condition = PTHREAD_COND_INITIALIZER;
static pthread_t preload_thread;
static int preload_started, preload_stop;
static unsigned long preload_bytes;

#ifdef HALO_IOS
void host_thread_background(void);
int host_memory_available_megabytes(void);
#endif

/* These sheets were drawn at campaign startup. Scope/weapon variants and
 * titles stay on demand rather than filling the cache with unused images. */
static int preload_startup_asset(long asset)
{
	const struct hud_hires_embedded *image = &hud_hires_embedded[asset];
	const char *name = strrchr(image->tag, '\\');

	if (image->title || !name) return 0;
	name++;
	return ((!strcmp(name, "hud_reticles") || !strcmp(name, "hud_unit_meters")) && image->bitmap == 0) ||
		(!strcmp(name, "hud_unit_backgrounds") &&
			(image->bitmap == 0 || image->bitmap == 2 || image->bitmap == 6)) ||
		(!strcmp(name, "hud_weapon_backgrounds") && image->bitmap == 3);
}

static void *preload_worker(void *unused)
{
	long asset;
	(void)unused;
#ifdef HALO_IOS
	host_thread_background();
#else
	/* On Linux, priority zero names the calling thread. */
	setpriority(PRIO_PROCESS, 0, 10);
#endif
	for (asset = 0; asset < hud_hires_asset_count(); asset++)
	{
		const struct hud_hires_embedded *image = &hud_hires_embedded[asset];
		unsigned long width, height, bytes, scratch;
		unsigned char *pixels;

		pthread_mutex_lock(&preload_lock);
		if (preload_stop) { pthread_mutex_unlock(&preload_lock); break; }
		if (preload[asset].state != PRELOAD_PENDING)
		{ pthread_mutex_unlock(&preload_lock); continue; }
		/* Only embedded, dimension-checked sheets are speculative work. Charge
		 * the filtered allocation even after its pixels have been compacted. */
		if (image->png_size < 33 || image->png_size > PRELOAD_LIMIT ||
			big_endian_long((const unsigned char *)image->png + 16) != image->width ||
			big_endian_long((const unsigned char *)image->png + 20) != image->height)
		{
			preload[asset].state = PRELOAD_NONE;
			pthread_mutex_unlock(&preload_lock);
			continue;
		}
		width = image->width;
		height = image->height;
		bytes = width && width <= 8192 && height && height <= 8192 ? height * (width * 4 + 1) : PRELOAD_LIMIT;
		scratch = image->png_size + PRELOAD_INFLATE;
		if (bytes > PRELOAD_LIMIT || scratch > PRELOAD_LIMIT - bytes ||
			preload_bytes > PRELOAD_LIMIT - bytes - scratch)
		{
			preload[asset].state = PRELOAD_NONE;
			pthread_mutex_unlock(&preload_lock);
			continue;
		}
		preload_bytes += bytes + scratch;
		preload[asset].state = PRELOAD_DECODING;
		pthread_mutex_unlock(&preload_lock);
		pixels = png_decode((const unsigned char *)image->png, image->png_size, &width, &height);
		pthread_mutex_lock(&preload_lock);
		preload_bytes -= scratch;
		if (preload_stop || !pixels)
		{
			free(pixels);
			preload_bytes -= bytes;
			preload[asset].state = PRELOAD_NONE;
		}
		else
		{
			preload[asset].pixels = pixels;
			preload[asset].width = width;
			preload[asset].height = height;
			preload[asset].bytes = bytes;
			preload[asset].state = PRELOAD_READY;
		}
		pthread_cond_broadcast(&preload_condition);
		pthread_mutex_unlock(&preload_lock);
	}
	return NULL;
}

/* The map loader owns this join, before it hands control to gameplay.
 * Keep prepared pixels and their reservations until render-thread upload. */
void hud_hires_preload_finish(void)
{
	if (preload_started)
	{
		pthread_join(preload_thread, NULL);
		preload_started = 0;
	}
}

void hud_hires_preload_reset(void)
{
	long asset;

	pthread_mutex_lock(&preload_lock);
	preload_stop = 1;
	pthread_cond_broadcast(&preload_condition);
	pthread_mutex_unlock(&preload_lock);
	/* Never hold the publication lock while joining. */
	if (preload_started) pthread_join(preload_thread, NULL);
	pthread_mutex_lock(&preload_lock);
	for (asset = 0; asset < MAXIMUM_TEXTURES; asset++) free(preload[asset].pixels);
	memset(preload, 0, sizeof(preload));
	preload_started = preload_stop = 0;
	preload_bytes = 0;
	pthread_mutex_unlock(&preload_lock);
}

void hud_hires_preload(const long *assets, long count)
{
	long index, pending = 0;

	hud_hires_preload_reset();
	if (!config_boolean("display.high_res_hud")) return;
#ifdef HALO_IOS
	{
		int available = host_memory_available_megabytes();
		if (available >= 0 && available < 128) return;
	}
#endif
	for (index = 0; index < count; index++)
	{
		long asset = assets[index];
		if (asset >= 0 && asset < hud_hires_asset_count() && !textures[asset].texture &&
			!textures[asset].failed && preload_startup_asset(asset))
		{
			preload[asset].state = PRELOAD_PENDING;
			pending++;
		}
	}
	if (pending)
	{
		preload_bytes = PRELOAD_STACK;
		if (pthread_create(&preload_thread, NULL, preload_worker, NULL) == 0)
			preload_started = 1;
		else
			hud_hires_preload_reset();
	}
}

unsigned int hud_hires_override_texture(long asset, unsigned long *levels)
{
	const struct hud_hires_embedded *embedded;
	unsigned char *pixels;
	unsigned long bytes, width, height;

	if (asset < 0 || asset >= hud_hires_asset_count() || textures[asset].failed)
		return 0;
	if (textures[asset].texture)
	{
		*levels = textures[asset].levels;
		return textures[asset].texture;
	}
	embedded = &hud_hires_embedded[asset];
	pthread_mutex_lock(&preload_lock);
	/* Demand never waits behind other images. Claim queued work immediately;
	 * only an image already being decoded needs a wait. */
	while (preload[asset].state == PRELOAD_DECODING)
		pthread_cond_wait(&preload_condition, &preload_lock);
	pixels = preload[asset].pixels;
	bytes = preload[asset].bytes;
	width = preload[asset].width;
	height = preload[asset].height;
	preload[asset].pixels = NULL;
	preload[asset].state = PRELOAD_NONE;
	pthread_mutex_unlock(&preload_lock);
	if (pixels)
	{
		textures[asset].texture = png_upload(pixels, width, height, &textures[asset].levels);
		free(pixels);
		pthread_mutex_lock(&preload_lock);
		preload_bytes -= bytes;
		pthread_mutex_unlock(&preload_lock);
	}
	else
		textures[asset].texture = hud_hires_png_texture(embedded->png, embedded->png_size, &textures[asset].levels);
	if (!textures[asset].texture)
	{
		platform_log("high-res hud: could not decode the texture for %s bitmap %d", embedded->tag, embedded->bitmap);
		textures[asset].failed = 1;
		return 0;
	}
	*levels = textures[asset].levels;
	return textures[asset].texture;
}
