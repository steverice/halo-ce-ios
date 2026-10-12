/*
BINK_DECODER.C

A Bink 1 movie decoded with FFmpeg's demuxer and decoders (bink_decoder.h).

FFmpeg is used through its public API only (libavformat, libavcodec,
libavutil), on one thread: the vendored build has no threading
(port/third_party/ffmpeg-bink/README.md). The file is read through a custom
AVIOContext, since that build has no protocols either. The decoder's
YUV 4:2:0 picture is converted here, and its float audio becomes 16-bit PCM
here, so neither libswscale nor libswresample is needed.
*/

#include "bink_decoder.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_BUFFER_SIZE 65536

static struct
{
	FILE *file;
	AVIOContext *io;
	AVFormatContext *format;
	AVCodecContext *video, *audio;
	int video_stream, audio_stream;
	AVPacket *packet;
	/* the last picture decoded, and the frame the decoders receive into */
	AVFrame *picture, *scratch;
	int has_picture;

	/* the audio queue: `audio_count` sample frames from `audio_base` on,
	and the position below which decoded audio is dropped (bink_decoder_audio_skip) */
	int16_t *audio_samples;
	uint32_t audio_capacity, audio_count, audio_base, audio_skip;
	int audio_channels;
} movie = { .video_stream = -1, .audio_stream = -1 };

static void (*log_proc)(const char *text);

/* ---------- logging */

static void log_line(const char *format, ...)
{
	char text[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	if (log_proc)
		log_proc(text);
	else
		fprintf(stderr, "%s\n", text);
}

static void ffmpeg_log(void *context, int level, const char *format, va_list arguments)
{
	char text[512];
	size_t length;

	(void)context;
	if (level > AV_LOG_WARNING)
		return;
	vsnprintf(text, sizeof(text), format, arguments);
	length = strlen(text);
	while (length && (text[length - 1] == '\n' || text[length - 1] == '\r'))
		text[--length] = 0;
	if (length)
		log_line("bink: ffmpeg: %s", text);
}

void bink_decoder_set_log(void (*log)(const char *text))
{
	log_proc = log;
}

/* ---------- file access */

static int io_read(void *opaque, uint8_t *buffer, int size)
{
	size_t count = fread(buffer, 1, (size_t)size, (FILE *)opaque);

	if (count)
		return (int)count;
	return ferror((FILE *)opaque) ? AVERROR(EIO) : AVERROR_EOF;
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
	FILE *file = (FILE *)opaque;

	if (whence & AVSEEK_SIZE)
	{
		off_t position = ftello(file), size;

		if (position < 0 || fseeko(file, 0, SEEK_END))
			return AVERROR(EIO);
		size = ftello(file);
		fseeko(file, position, SEEK_SET);
		return size;
	}
	whence &= ~AVSEEK_FORCE;
	if (fseeko(file, (off_t)offset, whence))
		return AVERROR(EIO);
	return ftello(file);
}

/* ---------- open and close */

static AVCodecContext *decoder_open(const AVStream *stream)
{
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	AVCodecContext *context = codec ? avcodec_alloc_context3(codec) : NULL;

	if (!context)
		return NULL;
	context->pkt_timebase = stream->time_base;
	if (avcodec_parameters_to_context(context, stream->codecpar) < 0 || avcodec_open2(context, codec, NULL) < 0)
		avcodec_free_context(&context);
	return context;
}

void bink_decoder_close(void)
{
	avcodec_free_context(&movie.video);
	avcodec_free_context(&movie.audio);
	if (movie.format)
		avformat_close_input(&movie.format);
	if (movie.io)
	{
		av_freep(&movie.io->buffer);
		avio_context_free(&movie.io);
	}
	if (movie.file)
		fclose(movie.file);
	av_packet_free(&movie.packet);
	av_frame_free(&movie.picture);
	av_frame_free(&movie.scratch);
	free(movie.audio_samples);
	memset(&movie, 0, sizeof(movie));
	movie.video_stream = movie.audio_stream = -1;
}

int bink_decoder_open(const char *path, struct bink_decoder_information *information)
{
	unsigned char *buffer;
	const AVStream *stream;
	unsigned int index;
	int result;

	bink_decoder_close();
	av_log_set_callback(ffmpeg_log);
	memset(information, 0, sizeof(*information));

	movie.file = fopen(path, "rb");
	if (!movie.file)
	{
		log_line("bink: cannot open \"%s\"", path);
		return 0;
	}
	buffer = av_malloc(IO_BUFFER_SIZE);
	movie.io = buffer ? avio_alloc_context(buffer, IO_BUFFER_SIZE, 0, movie.file, io_read, NULL, io_seek) : NULL;
	movie.format = avformat_alloc_context();
	movie.packet = av_packet_alloc();
	movie.picture = av_frame_alloc();
	movie.scratch = av_frame_alloc();
	if (!movie.io || !movie.format || !movie.packet || !movie.picture || !movie.scratch)
	{
		if (!movie.io)
			av_free(buffer);
		avformat_free_context(movie.format);
		movie.format = NULL;
		bink_decoder_close();
		return 0;
	}
	movie.format->pb = movie.io;
	/* (on failure avformat_open_input frees the context) */
	result = avformat_open_input(&movie.format, NULL, av_find_input_format("bink"), NULL);
	if (result < 0)
	{
		log_line("bink: \"%s\" is not a Bink movie FFmpeg reads (error %d)", path, result);
		bink_decoder_close();
		return 0;
	}

	/* the Bink header says all the demuxer needs: no avformat_find_stream_info */
	for (index = 0; index < movie.format->nb_streams; index++)
	{
		enum AVMediaType type = movie.format->streams[index]->codecpar->codec_type;

		if (type == AVMEDIA_TYPE_VIDEO && movie.video_stream < 0)
			movie.video_stream = (int)index;
		else if (type == AVMEDIA_TYPE_AUDIO && movie.audio_stream < 0)
			movie.audio_stream = (int)index;
	}
	if (movie.video_stream < 0)
	{
		log_line("bink: \"%s\" has no video", path);
		bink_decoder_close();
		return 0;
	}
	stream = movie.format->streams[movie.video_stream];
	movie.video = decoder_open(stream);
	if (!movie.video || movie.video->width <= 0 || movie.video->height <= 0 ||
		stream->time_base.num <= 0 || stream->time_base.den <= 0 || stream->duration <= 0)
	{
		log_line("bink: cannot decode the video of \"%s\"", path);
		bink_decoder_close();
		return 0;
	}
	information->width = (uint32_t)movie.video->width;
	information->height = (uint32_t)movie.video->height;
	/* the demuxer's time base is one frame */
	information->frame_count = (uint32_t)stream->duration;
	information->frame_rate = (uint32_t)stream->time_base.den;
	information->frame_rate_divisor = (uint32_t)stream->time_base.num;

	if (movie.audio_stream >= 0)
	{
		movie.audio = decoder_open(movie.format->streams[movie.audio_stream]);
		if (movie.audio && movie.audio->sample_rate > 0 &&
			(movie.audio->ch_layout.nb_channels == 1 || movie.audio->ch_layout.nb_channels == 2))
		{
			movie.audio_channels = movie.audio->ch_layout.nb_channels;
			information->audio_rate = (uint32_t)movie.audio->sample_rate;
			information->audio_channels = (uint32_t)movie.audio_channels;
		}
		else
		{
			log_line("bink: playing \"%s\" without its audio", path);
			avcodec_free_context(&movie.audio);
			movie.audio_stream = -1;
		}
	}
	return 1;
}

/* ---------- decoding */

static void audio_append(const AVFrame *frame)
{
	uint32_t frames = (uint32_t)frame->nb_samples, first = 0, index;
	int planar = frame->format == AV_SAMPLE_FMT_FLTP, channel;
	int16_t *out;

	if (frame->format != AV_SAMPLE_FMT_FLT && !planar)
		return;
	/* the frames a skip dropped before they were decoded */
	if (!movie.audio_count && movie.audio_base < movie.audio_skip)
	{
		first = movie.audio_skip - movie.audio_base;
		if (first > frames)
			first = frames;
		movie.audio_base += first;
	}
	if (first == frames)
		return;
	if (movie.audio_count + (frames - first) > movie.audio_capacity)
	{
		uint32_t capacity = (movie.audio_count + (frames - first)) * 2;
		int16_t *samples = realloc(movie.audio_samples, (size_t)capacity * movie.audio_channels * sizeof(int16_t));

		if (!samples)
			return;
		movie.audio_samples = samples;
		movie.audio_capacity = capacity;
	}
	out = movie.audio_samples + (size_t)movie.audio_count * movie.audio_channels;
	for (index = first; index < frames; index++)
	{
		for (channel = 0; channel < movie.audio_channels; channel++)
		{
			float sample = planar ? ((const float *)frame->extended_data[channel])[index] :
				((const float *)frame->data[0])[index * movie.audio_channels + channel];
			float scaled = sample * 32768.0f;

			*out++ = scaled >= 32767.0f ? 32767 : scaled <= -32768.0f ? -32768 : (int16_t)scaled;
		}
	}
	movie.audio_count += frames - first;
}

int bink_decoder_next_frame(void)
{
	if (!movie.video)
		return 0;
	for (;;)
	{
		AVCodecContext *decoder;
		int result = av_read_frame(movie.format, movie.packet);

		if (result < 0)
			return 0;
		decoder = movie.packet->stream_index == movie.video_stream ? movie.video :
			movie.packet->stream_index == movie.audio_stream ? movie.audio : NULL;
		result = decoder ? avcodec_send_packet(decoder, movie.packet) : AVERROR(EINVAL);
		av_packet_unref(movie.packet);
		if (result < 0)
			continue;
		/* a Bink frame's audio comes before its picture, so the picture ends the frame */
		while (avcodec_receive_frame(decoder, movie.scratch) >= 0)
		{
			if (decoder == movie.audio)
			{
				audio_append(movie.scratch);
				av_frame_unref(movie.scratch);
			}
			else
			{
				av_frame_unref(movie.picture);
				av_frame_move_ref(movie.picture, movie.scratch);
				movie.has_picture = 1;
				return 1;
			}
		}
	}
}

static inline uint8_t clamp8(int value)
{
	return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

void bink_decoder_copy_frame(void *pixels, int pitch, int height)
{
	const AVFrame *picture = movie.picture;
	int width, rows, x, y;
	int full_range;

	if (!movie.has_picture || !pixels || pitch <= 0 ||
		(picture->format != AV_PIX_FMT_YUV420P && picture->format != AV_PIX_FMT_YUVA420P))
		return;
	width = picture->width < pitch / 4 ? picture->width : pitch / 4;
	rows = picture->height < height ? picture->height : height;
	full_range = picture->color_range == AVCOL_RANGE_JPEG;
	/* ITU-R BT.601, 8 fractional bits; each chroma sample covers 2x2 pixels */
	for (y = 0; y < rows; y++)
	{
		const uint8_t *luma = picture->data[0] + (ptrdiff_t)y * picture->linesize[0];
		const uint8_t *cb = picture->data[1] + (ptrdiff_t)(y / 2) * picture->linesize[1];
		const uint8_t *cr = picture->data[2] + (ptrdiff_t)(y / 2) * picture->linesize[2];
		uint8_t *out = (uint8_t *)pixels + (ptrdiff_t)y * pitch;

		for (x = 0; x < width; x++)
		{
			int d = cb[x / 2] - 128, e = cr[x / 2] - 128;
			int c, red, green, blue;

			if (full_range)
			{
				c = luma[x] * 256 + 128;
				red = c + 359 * e;
				green = c - 88 * d - 183 * e;
				blue = c + 454 * d;
			}
			else
			{
				c = (luma[x] - 16) * 298 + 128;
				red = c + 409 * e;
				green = c - 100 * d - 208 * e;
				blue = c + 516 * d;
			}
			out[0] = clamp8(blue >> 8);
			out[1] = clamp8(green >> 8);
			out[2] = clamp8(red >> 8);
			out[3] = 255;
			out += 4;
		}
	}
}

/* ---------- the audio queue */

static void audio_drop(uint32_t frames)
{
	movie.audio_count -= frames;
	movie.audio_base += frames;
	memmove(movie.audio_samples, movie.audio_samples + (size_t)frames * movie.audio_channels,
		(size_t)movie.audio_count * movie.audio_channels * sizeof(int16_t));
}

int bink_decoder_audio_read(int16_t *samples, int capacity, uint32_t until)
{
	uint32_t frames = movie.audio_count;

	if (!movie.audio || capacity <= 0 || until <= movie.audio_base)
		return 0;
	if (frames > until - movie.audio_base)
		frames = until - movie.audio_base;
	if (frames > (uint32_t)capacity)
		frames = (uint32_t)capacity;
	if (!frames)
		return 0;
	memcpy(samples, movie.audio_samples, (size_t)frames * movie.audio_channels * sizeof(int16_t));
	audio_drop(frames);
	return (int)frames;
}

void bink_decoder_audio_skip(uint32_t position)
{
	if (!movie.audio)
		return;
	if (position > movie.audio_skip)
		movie.audio_skip = position;
	if (position > movie.audio_base && movie.audio_count)
	{
		uint32_t frames = position - movie.audio_base;

		audio_drop(frames < movie.audio_count ? frames : movie.audio_count);
	}
}
