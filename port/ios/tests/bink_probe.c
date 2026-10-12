/*
BINK_PROBE.C

Decodes a whole Bink movie with port/runtime/bink_decoder.c and prints what
it found, for tools/bink_test.py: the header's numbers, how many frames and
audio sample frames came out, and a hash of every picture and of the audio.
With a frame number and a path, also writes that frame as a PPM picture.

usage: bink-probe MOVIE [FRAME OUTPUT.ppm]
*/

#include "bink_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
	const unsigned char *bytes = data;

	/* FNV-1a */
	while (size--)
		hash = (hash ^ *bytes++) * 0x100000001b3ull;
	return hash;
}

int main(int argc, char **argv)
{
	struct bink_decoder_information information;
	uint64_t picture_hash = 0xcbf29ce484222325ull, audio_hash = 0xcbf29ce484222325ull;
	unsigned long frames = 0, audio_frames = 0, opaque_errors = 0;
	long wanted = argc == 4 ? atol(argv[2]) : -1;
	static int16_t audio[4096 * 2];
	uint8_t *pixels;
	int pitch, count;

	if (argc != 2 && argc != 4)
	{
		fprintf(stderr, "usage: %s MOVIE [FRAME OUTPUT.ppm]\n", argv[0]);
		return 2;
	}
	if (!bink_decoder_open(argv[1], &information))
		return 1;
	pitch = (int)information.width * 4;
	pixels = malloc((size_t)pitch * information.height);
	if (!pixels)
		return 1;

	/* nothing is queued, and nothing reads, before the first frame */
	if (bink_decoder_audio_read(audio, 4096, 0xffffffffu) != 0)
	{
		fprintf(stderr, "audio before the first frame\n");
		return 1;
	}
	while (bink_decoder_next_frame())
	{
		bink_decoder_copy_frame(pixels, pitch, (int)information.height);
		picture_hash = hash_bytes(picture_hash, pixels, (size_t)pitch * information.height);
		for (size_t index = 3; index < (size_t)pitch * information.height; index += 4)
			opaque_errors += pixels[index] != 255;
		if ((long)frames == wanted)
		{
			FILE *output = fopen(argv[3], "wb");

			if (!output)
				return 1;
			fprintf(output, "P6\n%u %u\n255\n", information.width, information.height);
			for (size_t index = 0; index < (size_t)information.width * information.height; index++)
			{
				const uint8_t *pixel = pixels + index * 4;
				uint8_t rgb[3] = { pixel[2], pixel[1], pixel[0] };

				fwrite(rgb, 1, 3, output);
			}
			fclose(output);
		}
		frames++;
		/* a read stops at `until`: the queue's first frame is numbered audio_frames */
		if (information.audio_channels && bink_decoder_audio_read(audio, 4096, (uint32_t)audio_frames) != 0)
		{
			fprintf(stderr, "a read passed its limit at frame %lu\n", frames);
			return 1;
		}
		while ((count = bink_decoder_audio_read(audio, 4096, 0xffffffffu)) > 0)
		{
			audio_hash = hash_bytes(audio_hash, audio, (size_t)count * information.audio_channels * sizeof(int16_t));
			audio_frames += (unsigned long)count;
		}
	}
	/* past the end: no more frames, and the last picture stays */
	if (bink_decoder_next_frame())
	{
		fprintf(stderr, "a frame after the last\n");
		return 1;
	}
	bink_decoder_close();
	free(pixels);
	printf("width=%u height=%u frames=%u rate=%u/%u audio_rate=%u audio_channels=%u "
		"decoded_frames=%lu audio_frames=%lu opaque_errors=%lu picture_hash=%016llx audio_hash=%016llx\n",
		information.width, information.height, information.frame_count, information.frame_rate,
		information.frame_rate_divisor, information.audio_rate, information.audio_channels,
		frames, audio_frames, opaque_errors, (unsigned long long)picture_hash, (unsigned long long)audio_hash);
	return 0;
}
