#include "harness.h"

#include <stdarg.h>
#include <stdlib.h>

typedef unsigned long long Uint64;
typedef unsigned int GLuint;
struct texture_entry
{
	struct texture_entry *next;
	GLuint texture;
	struct { unsigned long width, height, depth, levels; int cube_map; } description;
	unsigned long address, generation, last_used_frame;
	GLuint hud_upscale;
};
#include "config.inc"
static struct texture_entry *texture_buckets[TEXTURE_BUCKET_COUNT];
static struct { struct texture_entry *entry; } recent_textures[4];
static Uint64 clock_ms;
static Uint64 SDL_GetTicks(void) { return clock_ms; }
static unsigned long destroyed;
static void gpu_texture_destroy(GLuint texture) { destroyed++; }
static void platform_log(const char *format, ...) { }
static long available_mb = 500;
static long available(void) { return available_mb; }
static void xgpu_gl_state_invalidate(void) { }
/* display.texture_idle_seconds and debug.texture_idle_seconds */
static double idle_setting = 30.0, debug_setting = -1.0;
static double config_real(const char *name) { return name[0] == 'd' && name[1] == 'e' ? debug_setting : idle_setting; }
/* the high-res HUD's settings never change here */
static unsigned long hud_hires_settings_changed(void) { return 0; }
static int hud_hires_bitmap_affected(unsigned long address, long width, long height, unsigned long changed)
{
	return 0;
}
#include "under_test.inc"

/* the texture drawn every frame, and the one drawn until a time */
enum { ALWAYS = 1, BUCKET_ALWAYS = 7, SOMETIMES = 2, BUCKET_SOMETIMES = 9 };

static void add(GLuint texture, int bucket)
{
	struct texture_entry *entry = calloc(1, sizeof(*entry));

	entry->texture = texture;
	entry->last_used_frame = texture_frame;
	entry->next = texture_buckets[bucket];
	texture_buckets[bucket] = entry;
}

static struct texture_entry *find(GLuint texture, int bucket)
{
	struct texture_entry *entry;

	for (entry = texture_buckets[bucket]; entry; entry = entry->next)
	{
		if (entry->texture == texture)
			return entry;
	}
	return NULL;
}

/* frames at a rate for a time (seconds), drawing one texture every frame and
the other until seen_until: the seconds it was dropped after it was last
drawn (-1: kept) */
static double play(int rate, double seen_until, double seconds)
{
	long frame;

	add(ALWAYS, BUCKET_ALWAYS);
	add(SOMETIMES, BUCKET_SOMETIMES);
	for (frame = 0; frame < (long)(seconds * rate); frame++)
	{
		struct texture_entry *sometimes;

		clock_ms = (Uint64)frame * 1000 / rate;
		xgpu_texture_cache_begin_frame();
		CHECK(find(ALWAYS, BUCKET_ALWAYS), "the texture drawn every frame was dropped at %.1f s", clock_ms / 1000.0);
		find(ALWAYS, BUCKET_ALWAYS)->last_used_frame = texture_frame;
		sometimes = find(SOMETIMES, BUCKET_SOMETIMES);
		if (!sometimes)
			return clock_ms / 1000.0 - seen_until;
		if (clock_ms < seen_until * 1000)
			sometimes->last_used_frame = texture_frame;
	}
	return -1.0;
}

static void check_idle(double idle, double lowest)
{
	/* (a sweep every 10 s drops what was not drawn since the sweep the idle
	time before it: the idle time to 10 s more after) */
	CHECK(idle >= lowest && idle <= lowest + 11.0, "dropped %.1f s after it was last drawn, not %.0f to %.0f", idle, lowest, lowest + 10.0);
}

/* a memory warning after seconds of frames, three textures: one drawn up to
the warning, one last drawn five seconds before it, one never drawn after
being added */
static void check_trim(int request)
{
	enum { RECENT = 11, STALE = 12 };
	long frame;

	xgpu_available_memory_hook = available;
	add(RECENT, 20);
	add(STALE, 21);
	for (frame = 0; frame < 10 * 60; frame++)
	{
		clock_ms = (Uint64)frame * 1000 / 60;
		xgpu_texture_cache_begin_frame();
		if (frame < 5 * 60)
			find(STALE, 21)->last_used_frame = texture_frame;
		find(RECENT, 20)->last_used_frame = texture_frame;
	}
	/* (a sweep is not due: the idle time is the profile's, a minute) */
	CHECK(find(STALE, 21) && find(RECENT, 20), "textures were dropped without a warning");
	if (request)
		xgpu_texture_cache_request_trim();
	clock_ms += 16;
	xgpu_texture_cache_begin_frame();
	CHECK(find(RECENT, 20), "the texture drawn just before the warning was dropped");
	if (request)
	{
		CHECK(!find(STALE, 21), "the texture not drawn for five seconds was kept through the warning");
		CHECK(destroyed == 1, "%lu textures destroyed, not 1", destroyed);
		/* the request is consumed: a later frame drops nothing more */
		add(STALE, 21);
		find(STALE, 21)->last_used_frame = 0;
		clock_ms += 16;
		xgpu_texture_cache_begin_frame();
		CHECK(find(STALE, 21), "a second trim ran without a second warning");
	}
	else
		CHECK(find(STALE, 21), "a texture was dropped without a warning");
}

int main(int argc, char **argv)
{
	const char *case_name = argv[1];

	CASE("fast")
	{
		/* at 360 frames a second, a texture out of sight for five seconds
		(1800 frames) is kept */
		check_idle(play(360, 1.0, 60.0), 30.0);
	}
	CASE("steady")
	{
		/* at 60 frames a second, as it was in frames: half a minute */
		check_idle(play(60, 1.0, 80.0), 30.0);
	}
	CASE("late")
	{
		/* round the sweeps more than once */
		check_idle(play(360, 25.0, 80.0), 30.0);
	}
	CASE("profile")
	{
		/* the default, a minute, at a frame rate that would have dropped it in five seconds */
		idle_setting = 60.0;
		check_idle(play(360, 1.0, 100.0), 60.0);
	}
	CASE("debug")
	{
		/* the debug value in place of the profile's, a shorter and a longer one */
		idle_setting = 60.0;
		debug_setting = 20.0;
		check_idle(play(120, 1.0, 60.0), 20.0);
	}
	CASE("long")
	{
		/* the longest, past the sweeps remembered once round */
		idle_setting = 600.0;
		check_idle(play(30, 1.0, 700.0), 600.0);
	}
	CASE("trim")
	{
		idle_setting = 60.0;
		check_trim(1);
	}
	CASE("no_trim")
	{
		idle_setting = 60.0;
		check_trim(0);
	}
	return 0;
}
