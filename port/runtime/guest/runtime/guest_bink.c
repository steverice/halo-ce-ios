/*
GUEST_BINK.C

The Bink video SDK entry points bink_playback.c uses, for the Apple builds
(the other ports link port/linux/src/bink_null.c). The decoder is FFmpeg's,
in the host (host_bink_*, port/ios/host/host_bink.c); a host built without
it (HALO_BINK off) opens nothing, and the game skips the movie as it does
for a missing file.

What this side does:
	- which movies play: the intro and the credits, not the attract movies,
	  and none in a run that must draw the same frames every time
	  (display.movies);
	- the clock. The game draws a frame, then calls BinkWait until the next
	  one is due. Frame n is due n frame times after the first was drawn, by
	  the real clock, so a late frame is followed by early ones and the movie
	  keeps its length. A stall longer than MOVIE_STALL (the app in the
	  background, a debugger) moves the clock instead: the movie goes on
	  from where it was;
	- the audio, through the game's mixer (dsound_sdl.c) as one more
	  DirectSound stream of 16-bit PCM. The decoder queues each frame's
	  audio, which the file stores ahead of its picture; the mixer is given
	  it up to AUDIO_LEAD past the movie's time, so it never runs far ahead
	  of the pictures. After a stall the stream is flushed and the queue
	  skips to the movie's time.

The prototypes match the declarations in bink_playback.c; the RAD SDK's
RADEXPLINK is __stdcall.
*/

#include "platform.h"
#include "port_config.h"
#include "guest_host.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#define MOVIE_STALL_NANOSECONDS 250000000LL
#define AUDIO_LEAD_NANOSECONDS 250000000LL
#define WAIT_SLEEP_NANOSECONDS 1000000L
#define AUDIO_CHUNK_FRAMES 4096

typedef void *(__stdcall *rad_memory_allocate_proc)(unsigned long size);
typedef void (__stdcall *rad_memory_free_proc)(void *memory);
typedef void *(__stdcall *bink_sound_system_open_proc)(unsigned long param);

/* the members bink_playback.c reads */
typedef struct BINK
{
	unsigned long Width;
	unsigned long Height;
	unsigned long Frames;
	unsigned long FrameNum;
	unsigned long LastFrameNum;
} *HBINK;

static struct
{
	struct BINK bink;
	BOOL open;
	struct host_bink_information information;
	/* when frame 0 was drawn (0 until then), by the real clock */
	long long start;
	LPDIRECTSOUND sound;
	LPDIRECTSOUNDSTREAM stream;
} movie;

static short audio_chunk[AUDIO_CHUNK_FRAMES * 2];

/* ---------- which movies play */

/* display.movies: "auto" (the default) plays them except in a run that must
draw the same frames every time, as display.async_shaders (d3d8_device.c) */
static BOOL movies_enabled(void)
{
	const char *mode = config_string("display.movies");
	const char *reason = platform_fixed_timestep() ? "debug.fixed_timestep" :
		*config_string("debug.input_replay") ? "debug.input_replay" :
		config_boolean("debug.benchmark") ? "debug.benchmark" :
		config_real("debug.exit_after") > 0.0 ? "debug.exit_after" :
		config_integer("debug.screenshot_every") > 0 ? "debug.screenshot_every" : NULL;

	if (!strcmp(mode, "on"))
		return TRUE;
	if (!strcmp(mode, "off"))
		return FALSE;
	if (strcmp(mode, "auto"))
		platform_log("display.movies: unknown value \"%s\"; using \"auto\"", mode);
	if (reason)
	{
		platform_log("display.movies: a deterministic run (%s): movies are skipped", reason);
		return FALSE;
	}
	return TRUE;
}

static const char *base_name(const char *name)
{
	const char *base = name;

	for (; *name; name++)
	{
		if (*name == '\\' || *name == '/')
			base = name + 1;
	}
	return base;
}

/* ---------- the clock */

static long long real_nanoseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/* how long after frame 0 a frame (numbered from 0) is due */
static long long frame_nanoseconds(unsigned long frame)
{
	return (long long)frame * movie.information.frame_rate_divisor * 1000000000LL / movie.information.frame_rate;
}

/* ---------- audio */

static void audio_open(void)
{
	WAVEFORMATEX format;
	DSSTREAMDESC description;

	if (!movie.sound || !movie.information.audio_channels)
		return;
	memset(&format, 0, sizeof(format));
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = (WORD)movie.information.audio_channels;
	format.nSamplesPerSec = movie.information.audio_rate;
	format.wBitsPerSample = 16;
	format.nBlockAlign = (WORD)(2 * movie.information.audio_channels);
	format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
	memset(&description, 0, sizeof(description));
	description.dwMaxAttachedPackets = 64;
	description.lpwfxFormat = &format;
	if (IDirectSound_CreateSoundStream(movie.sound, &description, &movie.stream, NULL) != DS_OK)
		movie.stream = NULL;
}

/* gives the mixer the queued audio up to AUDIO_LEAD past the movie's time */
static void audio_pump(long long movie_time)
{
	unsigned long until;
	int pass;

	if (!movie.stream)
		return;
	until = (unsigned long)((movie_time + AUDIO_LEAD_NANOSECONDS) * movie.information.audio_rate / 1000000000LL);
	for (pass = 0; pass < 64; pass++)
	{
		XMEDIAPACKET packet;
		DWORD status = 0;
		int frames;

		IDirectSoundStream_GetStatus(movie.stream, &status);
		if (!(status & XMO_STATUSF_ACCEPT_INPUT_DATA))
		{
			/* the mixer's finished packets are completed here (dsound_sdl.c),
			as the game's sound update does each frame */
			DirectSoundDoWork();
			IDirectSoundStream_GetStatus(movie.stream, &status);
			if (!(status & XMO_STATUSF_ACCEPT_INPUT_DATA))
				return;
		}
		frames = host_bink_audio_read(audio_chunk, AUDIO_CHUNK_FRAMES, until);
		if (frames <= 0)
			return;
		memset(&packet, 0, sizeof(packet));
		packet.pvBuffer = audio_chunk;
		packet.dwMaxSize = (DWORD)frames * 2 * movie.information.audio_channels;
		/* (the mixer copies the samples as it takes the packet) */
		IDirectSoundStream_Process(movie.stream, &packet, NULL);
	}
}

/* ---------- the SDK's entry points */

void __stdcall RADSetMemory(rad_memory_allocate_proc allocate, rad_memory_free_proc release)
{
	/* the decoder allocates in the host, not from the game's Bink pool */
	(void)allocate;
	(void)release;
}

void *__stdcall BinkOpenDirectSound(unsigned long param)
{
	return (void *)param;
}

long __stdcall BinkSetSoundSystem(bink_sound_system_open_proc open, unsigned long param)
{
	(void)open;
	movie.sound = (LPDIRECTSOUND)param;
	return movie.sound != NULL;
}

void __stdcall BinkSetIOSize(unsigned long io_size)
{
	(void)io_size;
}

HBINK __stdcall BinkOpen(const char *name, unsigned long flags)
{
	char path[1024];

	(void)flags;
	if (movie.open || !name || !name[0])
		return NULL;
	if (!strncasecmp(base_name(name), "attract", 7))
	{
		platform_log("Bink: the attract movies are not played; skipping \"%s\"", name);
		return NULL;
	}
	if (!movies_enabled())
		return NULL;
	platform_translate_path(name, path, sizeof(path));
	if (!host_bink_open(path, &movie.information))
	{
		/* (a host without the decoder has said so) */
		return NULL;
	}
	if (!movie.information.frame_rate || !movie.information.frame_rate_divisor || !movie.information.frame_count)
	{
		host_bink_close();
		return NULL;
	}
	movie.bink.Width = movie.information.width;
	movie.bink.Height = movie.information.height;
	movie.bink.Frames = movie.information.frame_count;
	movie.bink.FrameNum = 1;
	movie.bink.LastFrameNum = 0;
	movie.start = 0;
	movie.open = TRUE;
	audio_open();
	platform_log("Bink: playing \"%s\": %lux%lu, %lu frames at %.2f a second%s", name, movie.bink.Width,
		movie.bink.Height, movie.bink.Frames,
		(double)movie.information.frame_rate / movie.information.frame_rate_divisor,
		movie.stream ? "" : ", without audio");
	return &movie.bink;
}

void __stdcall BinkClose(HBINK bink)
{
	(void)bink;
	if (!movie.open)
		return;
	if (movie.stream)
	{
		IDirectSoundStream_Flush(movie.stream);
		IDirectSoundStream_Release(movie.stream);
		movie.stream = NULL;
	}
	host_bink_close();
	movie.open = FALSE;
	/* (a movie closed before its first wait has no start) */
	platform_log("Bink: closed at frame %lu of %lu, %.1f s after its first frame", movie.bink.FrameNum,
		movie.bink.Frames, movie.start ? (double)(real_nanoseconds() - movie.start) / 1e9 : 0.0);
}

/* decodes frame FrameNum (numbered from 1) */
long __stdcall BinkDoFrame(HBINK bink)
{
	(void)bink;
	if (movie.open)
		host_bink_decode_frame();
	return 0;
}

void __stdcall BinkNextFrame(HBINK bink)
{
	(void)bink;
	if (!movie.open)
		return;
	movie.bink.LastFrameNum = movie.bink.FrameNum;
	if (movie.bink.FrameNum < movie.bink.Frames)
		movie.bink.FrameNum++;
}

/* nonzero while frame FrameNum is not yet due; the game calls this in a loop
after it draws a frame, so the wait sleeps */
long __stdcall BinkWait(HBINK bink)
{
	long long now, due;

	(void)bink;
	if (!movie.open)
		return 0;
	/* debug.fixed_timestep (display.movies = "on"): a movie frame for each frame drawn */
	if (platform_fixed_timestep())
	{
		audio_pump(frame_nanoseconds(movie.bink.FrameNum - 1));
		return 0;
	}
	now = real_nanoseconds();
	if (!movie.start)
		movie.start = now;
	due = movie.start + frame_nanoseconds(movie.bink.FrameNum - 1);
	if (now - due > MOVIE_STALL_NANOSECONDS)
	{
		/* a stall: the movie goes on from this frame, and its audio from this frame's */
		movie.start += now - due;
		if (movie.stream)
		{
			IDirectSoundStream_Flush(movie.stream);
			host_bink_audio_skip((unsigned int)(frame_nanoseconds(movie.bink.FrameNum - 1) *
				movie.information.audio_rate / 1000000000LL));
		}
		due = now;
	}
	audio_pump(now - movie.start);
	if (now < due)
	{
		struct timespec duration = { 0, due - now < WAIT_SLEEP_NANOSECONDS ? (long)(due - now) : WAIT_SLEEP_NANOSECONDS };

		nanosleep(&duration, NULL);
		return 1;
	}
	return 0;
}

long __stdcall BinkCopyToBuffer(HBINK bink, void *destination, long destination_pitch,
	unsigned long destination_height, unsigned long destination_x, unsigned long destination_y,
	unsigned long flags)
{
	(void)bink; (void)destination_x; (void)destination_y; (void)flags;
	if (movie.open && destination)
		host_bink_copy_frame(destination, (int)destination_pitch, (int)destination_height);
	return 0;
}

/* debug_bink's statistics: none are kept (Frames 1 keeps its averages finite) */
void __stdcall BinkGetSummary(HBINK bink, void *summary)
{
	(void)bink;
	memset(summary, 0, 0x7C);
}

void __stdcall BinkGetRealtime(HBINK bink, void *realtime, unsigned long frame_count)
{
	(void)bink; (void)frame_count;
	memset(realtime, 0, 0x38);
	((unsigned long *)realtime)[3] = 1;
}
