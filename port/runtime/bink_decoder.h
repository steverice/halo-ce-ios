/*
BINK_DECODER.H

A Bink 1 movie decoded with FFmpeg's decoders (port/third_party/ffmpeg-bink):
one movie at a time, read from a file, its frames in order, as 32-bit
pixels, with its first audio track as interleaved 16-bit PCM.

Only compiled into builds with movies (HALO_BINK, port/ios/CMakeLists.txt).
FFmpeg is LGPL 2.1 or later; see port/third_party/ffmpeg-bink/README.md.
*/

#ifndef HALO_BINK_DECODER_H
#define HALO_BINK_DECODER_H

#include <stdint.h>

/* the same layout in the guest and the host (guest_host.h): 32-bit members only */
struct bink_decoder_information
{
	uint32_t width, height;
	uint32_t frame_count;
	/* frames per second, as a fraction */
	uint32_t frame_rate, frame_rate_divisor;
	/* 0 channels: no audio */
	uint32_t audio_rate, audio_channels;
};

/* where the decoder's and FFmpeg's messages go (default: stderr) */
void bink_decoder_set_log(void (*log)(const char *text));

/* opens the movie at path (a host path), closing any movie open; 1 if it opened */
int bink_decoder_open(const char *path, struct bink_decoder_information *information);
void bink_decoder_close(void);

/* decodes the next frame, and the audio stored with it into the audio queue;
0 once the movie has no more frames (the last frame decoded stays) */
int bink_decoder_next_frame(void);

/* the last frame decoded, as X8R8G8B8 (bytes B, G, R, 255), into the top
left of a surface `height` rows of `pitch` bytes */
void bink_decoder_copy_frame(void *pixels, int pitch, int height);

/* The audio queue is interleaved 16-bit PCM in sample frames, each numbered
from the movie's start. A read takes up to capacity frames, none numbered
`until` or later, and returns how many it took; a skip drops the queued
frames numbered below `position`. */
int bink_decoder_audio_read(int16_t *samples, int capacity, uint32_t until);
void bink_decoder_audio_skip(uint32_t position);

#endif
