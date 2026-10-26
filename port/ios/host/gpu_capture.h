/*
GPU_CAPTURE.H

Capture and replay at gpu.h's boundary (builds configured with
HALO_RENDER_VALIDATION only): a window of frames, as the calls the Direct3D
front end made into the GPU backend, written to a file that a backend can be
driven from again without the game.

A capture file holds one window and everything its frames need, and nothing
else: each texture, shader and page of mirrored vertex memory is written the
first time a call in the window uses it, with the contents it has then. What
a file does not hold is what earlier frames drew into render targets: a
replay's targets start empty, so the first frames of a window can differ
from the game's where a draw reads a target an earlier frame filled (the
water's ripples, a zoom's blur). Compare a window's last frames.

A capture belongs to the backend it was made on: its shaders are in that
backend's language, and its draws were built for that backend's
capabilities (BGRA vertex colors, a base vertex, fans). A replay refuses a
capture whose capabilities differ from the backend's.

Captures contain the game's own data (its textures and models). They are
never kept in the repository.

Host only, 64-bit.
*/

#ifndef __HALO_GPU_CAPTURE_H
#define __HALO_GPU_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include "gpu.h"

#define GPU_CAPTURE_MAGIC "VISRCAP"
enum { GPU_CAPTURE_FORMAT = 1 };

/* a file: this header, then records until GPU_CAPTURE_END; the whole of it
gzip-compressed (zlib reads either) */
struct gpu_capture_header
{
	char magic[8];
	uint32_t format;
	/* tools/gpu_interface_hash.py: gpu.h and the headers it includes, and
	the front end's sources, as they were when the capture was made */
	uint32_t interface_hash, front_end_hash;
	/* the sizes of the structs the records hold, a second check on the same */
	uint32_t draw_size, uniforms_size, stereo_size, description_size, clear_size, capabilities_size;
	uint32_t initialize_flags;
	struct gpu_capabilities capabilities;
	/* the window: its first frame (presents before it) and how many */
	uint32_t first_frame, frame_count;
	char guest_sha256[68];
	char label[64];
};

/* a record: its operation, the bytes of payload after it (a multiple of 4) */
struct gpu_capture_record
{
	uint32_t operation, size;
};

/* each operation's payload: words (uint32_t), then a struct or bytes where named */
enum
{
	GPU_CAPTURE_END = 1,
	GPU_CAPTURE_TEXTURE_CREATE,      /* handle; struct gpu_texture_description */
	GPU_CAPTURE_TEXTURE_UPLOAD,      /* handle, face, level, size; bytes */
	GPU_CAPTURE_TEXTURE_COPY_LEVEL,  /* source, destination, level */
	GPU_CAPTURE_TEXTURE_MIPMAPS,     /* handle, base level */
	GPU_CAPTURE_TEXTURE_DESTROY,     /* handle */
	GPU_CAPTURE_BUFFER_CREATE,       /* handle, size */
	GPU_CAPTURE_BUFFER_WRITE,        /* handle, offset, size, flags; bytes */
	GPU_CAPTURE_STREAM_RESERVE,      /* vertex bytes, index bytes */
	GPU_CAPTURE_STREAM,              /* kind, size, the buffer and offset the backend gave; bytes */
	GPU_CAPTURE_SHADER_CREATE,       /* handle, stage, size; text without its terminator */
	GPU_CAPTURE_CONSTANTS,           /* count; count times: register, four floats */
	GPU_CAPTURE_UNIFORMS,            /* struct gpu_uniforms */
	GPU_CAPTURE_CLEAR,               /* count; struct gpu_clear; count struct gpu_rect */
	GPU_CAPTURE_DRAW,                /* the backend's result; struct gpu_draw */
	GPU_CAPTURE_VISIBILITY_BEGIN,
	GPU_CAPTURE_VISIBILITY_END,      /* slot */
	GPU_CAPTURE_VISIBILITY_RESULT,   /* slot */
	GPU_CAPTURE_FLUSH,
	GPU_CAPTURE_PRESENT,             /* back buffer */
	GPU_CAPTURE_PRESENT_STEREO,      /* struct gpu_stereo_present */
	GPU_CAPTURE_TEXTURE_COPY_RECTS,  /* source, count; count struct gpu_texture_copy */
	GPU_CAPTURE_OPERATION_COUNT
};

/* a record's payload is at most this (a 2048x2048 level is 16 MB) */
enum { GPU_CAPTURE_RECORD_LIMIT = 1 << 28 };

/* ---------- capturing */

struct gpu_capture_host
{
	void (*log)(const char *line);
	/* where the files go: FOLDER/frames-NNNNN.capture, NNNNN the window's first frame */
	const char *folder;
	/* the windows: "600+30,1200+30" is 30 frames from frame 600 and 30 from 1200 */
	const char *frames;
	uint32_t interface_hash, front_end_hash;
	const char *guest_sha256, *label;
};

/* replaces every function of table with one that records the call while a
window is open and then calls the function it replaced; does nothing without
a folder and a window */
void gpu_capture_install(struct gpu_backend *table, const struct gpu_capture_host *host);
/* closes an open window's file (the process is exiting) */
void gpu_capture_finish(void);

/* ---------- replaying */

/* what a replay is asked to change in the stream (the fuzzer): called with
each record before it is played; it may change the payload in place (never
its size). Returns nonzero if it changed anything */
typedef int gpu_replay_mutate(void *context, uint32_t operation, uint32_t *payload, uint32_t size);

struct gpu_replay_host
{
	void (*log)(const char *line);
	void *context;
	/* the flags the backend is initialized with, from the capture's */
	uint32_t (*initialize_flags)(void *context, uint32_t captured);
	/* before each present: the frame's number and its pictures (one, or a
	stereo frame's two eyes), with their sizes */
	void (*frame)(void *context, uint32_t frame, const gpu_texture *pictures, const uint32_t (*sizes)[2], uint32_t count);
	gpu_replay_mutate *mutate;
	/* after each mutated record was played, or refused by the reader
	(refused nonzero): the fuzzer asks the validator whether it objected */
	void (*mutated)(void *context, uint32_t operation, int refused);
	/* this build's hashes; a capture with another interface hash is refused
	unless force, one with another front end hash is played with a warning */
	uint32_t interface_hash, front_end_hash;
	int force;
};

struct gpu_replay_result
{
	uint32_t frames, records, draws, draws_skipped, mutated;
	/* 1: the front end's sources have changed since the capture was made */
	int stale_front_end;
	char error[256];
};

/* reads a file's header: 0, or -1 with why in error */
int gpu_replay_header(const char *path, struct gpu_capture_header *header, char *error, size_t size);
/* initializes backend and plays the file into it: 0 when the whole file
played, -1 (with result->error) when it was refused or is malformed; a
malformed record stops the replay without being played */
int gpu_replay_run(const char *path, const struct gpu_backend *backend, const struct gpu_replay_host *host,
	struct gpu_replay_result *result);

#endif
