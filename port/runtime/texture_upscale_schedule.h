/* When the on-device upscaler may run (spec 2026-10-24-halo-ce-ios-on-device-upscaling-design.md, "When it runs",
and Steve's rules of 2026-10-24): the profile is asked first, and gameplay always wins. Header-only, so a host
probe can check the rules without the app. */
#pragma once
#include <string.h>

/* debug.texture_upscale_generate */
enum texture_upscale_mode
{
	TEXTURE_UPSCALE_MODE_OFF,     /* "off" (default): never */
	TEXTURE_UPSCALE_MODE_AUTO,    /* "auto": where the profile and the player's switch both say so */
	TEXTURE_UPSCALE_MODE_FORCE    /* "force": whatever the profile says, for development */
};

static inline enum texture_upscale_mode texture_upscale_mode_parse(const char *text)
{
	if (!text)
		return TEXTURE_UPSCALE_MODE_OFF;
	/* (true and 1: what the development switch was before it had a mode) */
	if (!strcmp(text, "auto") || !strcmp(text, "true") || !strcmp(text, "1"))
		return TEXTURE_UPSCALE_MODE_AUTO;
	if (!strcmp(text, "force"))
		return TEXTURE_UPSCALE_MODE_FORCE;
	return TEXTURE_UPSCALE_MODE_OFF;
}

/* Rule 1: whether this device upscales at all. profile_on is display.upscaled_textures from the device's profile
   (the game's default where no profile sets it), player_on the player's switch (config.toml, the Settings app),
   model_available a compiled model and a Neural Engine Core ML can use (rule 8). When this is 0 nothing else
   happens: no import step, no background work, no cache directory. */
static inline int texture_upscale_gate(enum texture_upscale_mode mode, int profile_on, int player_on,
	int model_available)
{
	if (!model_available || mode == TEXTURE_UPSCALE_MODE_OFF)
		return 0;
	if (mode == TEXTURE_UPSCALE_MODE_FORCE)
		return 1;
	return profile_on && player_on;
}

/* Rules 4 and 6: the work runs in menus, while a level loads and while a game is paused, never while one is
   played. A pause counts once it has lasted PAUSE_SECONDS, so opening the pause menu for a moment does not start
   a texture the return to the game then has to wait for. */
#define TEXTURE_UPSCALE_PAUSE_SECONDS 2.0
struct texture_upscale_activity
{
	int in_game;          /* a level's game has started (host_astc_game_started) and no map load has begun since */
	int paused;           /* game_time_set_paused */
	double paused_since;  /* seconds, the caller's clock */
};

static inline void texture_upscale_activity_map_will_load(struct texture_upscale_activity *a)
{
	a->in_game = 0;
	a->paused = 0;
}

static inline void texture_upscale_activity_game_started(struct texture_upscale_activity *a, int is_level)
{
	a->in_game = is_level != 0;
	a->paused = 0;
}

static inline void texture_upscale_activity_paused(struct texture_upscale_activity *a, int paused, double now)
{
	if (paused && !a->paused)
		a->paused_since = now;
	a->paused = paused != 0;
}

/* 1: the worker may start or continue a texture now */
static inline int texture_upscale_activity_may_run(const struct texture_upscale_activity *a, double now)
{
	if (!a->in_game)
		return 1;
	return a->paused && now - a->paused_since >= TEXTURE_UPSCALE_PAUSE_SECONDS;
}

/* seconds until a pause lets the worker run, 0 if it already may, negative if only a state change will */
static inline double texture_upscale_activity_wait(const struct texture_upscale_activity *a, double now)
{
	if (texture_upscale_activity_may_run(a, now))
		return 0;
	if (a->paused)
		return TEXTURE_UPSCALE_PAUSE_SECONDS - (now - a->paused_since);
	return -1;
}

/* The pre-level wait (spec 2026-10-04's "Upscaling assets", kept by Steve on 2026-10-24): what the loading
   screen shows while a level's missing textures are made before its load. */
enum texture_upscale_level_wait
{
	TEXTURE_UPSCALE_LEVEL_NONE,       /* nothing to wait for: the generator is off, or the level is ready */
	TEXTURE_UPSCALE_LEVEL_CHECKING,   /* the worker has not yet listed the level's textures */
	TEXTURE_UPSCALE_LEVEL_WORKING,    /* making them: done of total units (input texels) */
	TEXTURE_UPSCALE_LEVEL_DONE        /* the worker finished the level, ready or not (an error leaves it original) */
};
