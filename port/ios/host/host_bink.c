/*
HOST_BINK.C

The guest's Bink movie imports (guest_host.h; the guest's side is
port/runtime/guest/runtime/guest_bink.c). With HALO_BINK they are
port/runtime/bink_decoder.c's functions, FFmpeg's decoders behind them
(port/third_party/ffmpeg-bink, LGPL). Without it this file is all there is:
no movie opens, the game skips it, and the app has no FFmpeg code.
*/

#include "ios_host.h"
#include "guest_host.h"

#include <stdio.h>
#include <string.h>

#if HALO_BINK

#include "bink_decoder.h"

_Static_assert(sizeof(struct host_bink_information) == sizeof(struct bink_decoder_information),
	"guest_host.h's host_bink_information must follow bink_decoder_information");

static void log_line(const char *text)
{
	host_logf(HOST_LOG_INFO, "%s", text);
}

int host_bink_open(const char *path, struct host_bink_information *information)
{
	struct bink_decoder_information found;
	int opened;

	bink_decoder_set_log(log_line);
	opened = bink_decoder_open(path, &found);
	memcpy(information, &found, sizeof(found));
	return opened;
}

void host_bink_close(void)
{
	bink_decoder_close();
}

int host_bink_decode_frame(void)
{
	return bink_decoder_next_frame();
}

void host_bink_copy_frame(void *pixels, int pitch, int height)
{
	if (pitch <= 0 || height <= 0)
		return;
	/* the frame is a texture's memory, whose pages the texture cache watches
	for writes (host_memory.c): a write from the host must say so itself */
	host_memory_watch_prepare_write(guest_pointer(pixels), (uint32_t)pitch * (uint32_t)height);
	bink_decoder_copy_frame(pixels, pitch, height);
}

int host_bink_audio_read(short *samples, int capacity, unsigned int until)
{
	return bink_decoder_audio_read(samples, capacity, until);
}

void host_bink_audio_skip(unsigned int position)
{
	bink_decoder_audio_skip(position);
}

#else

int host_bink_open(const char *path, struct host_bink_information *information)
{
	(void)information;
	/* what port/linux/src/bink_null.c says */
	fprintf(stderr, "halo-linux: Bink video is not supported; skipping \"%s\"\n", path ? path : "");
	return 0;
}

void host_bink_close(void) {}
int host_bink_decode_frame(void) { return 0; }
void host_bink_copy_frame(void *pixels, int pitch, int height) { (void)pixels; (void)pitch; (void)height; }
int host_bink_audio_read(short *samples, int capacity, unsigned int until) { (void)samples; (void)capacity; (void)until; return 0; }
void host_bink_audio_skip(unsigned int position) { (void)position; }

#endif
