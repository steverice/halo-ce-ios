/* Upscaled textures, per level (spec 2026-10-04 "Cache and versioning", "Settings"): a level loads upscaled textures
only if the switch was on and nothing asked for originals when it loaded; a skipped or low-storage level loads none,
not even cached ones, so it never shows upscaled and original textures side by side. Header-only for a host probe.
debug.upscaled_textures_toggle shows a level's originals in place of all its upscaled textures, and back, while it
plays: both stay resident, so only the texture drawn changes. */
#pragma once

struct texture_upscale_state
{
	int switch_on, original_requested, level_enabled;
	/* debug.upscaled_textures_toggle: draw the originals this level; textures replaced since the map loaded */
	int show_original;
	unsigned long replaced;
	/* debug.texture_compare_directory: a comparison of two sets replaces the original/upscaled cycle. higher: the
	   second set is drawn; fallback: the first set stood in for a texture the second has no file for */
	int compare, higher, fallback;
};

/* at every map load: the switch's value now, and whether piece 3 asked for originals (a skip, low storage, a failed
   write) before this load; the level keeps what this decides until the next map load */
static inline void texture_upscale_state_map_loaded(struct texture_upscale_state *s, int switch_on)
{
	s->switch_on = switch_on;
	s->level_enabled = switch_on && !s->original_requested;
	s->original_requested = 0;
	s->show_original = 0;   /* each level starts upscaled */
	s->replaced = 0;
	s->compare = s->higher = s->fallback = 0;
}

static inline void texture_upscale_state_request_original(struct texture_upscale_state *s)
{
	s->original_requested = 1;
}

static inline int texture_upscale_state_enabled(const struct texture_upscale_state *s)
{
	return s->level_enabled;
}

/* a texture of this level drawn upscaled where it may be: it is kept, and the toggle picks between it and the
   original at every draw, so one that arrives mid-level follows the toggle */
static inline void texture_upscale_state_replaced(struct texture_upscale_state *s)
{
	s->replaced++;
}

/* the toggle: 0 if this level has no upscaled textures (it loaded with them off, so there is nothing to switch to),
   else 1 with show_original flipped for every texture of the level at once */
static inline int texture_upscale_state_toggle(struct texture_upscale_state *s)
{
	if (!s->level_enabled)
		return 0;
	if (s->compare)
	{
		s->higher = !s->higher;
		s->fallback = 0;
	}
	else
		s->show_original = !s->show_original;
	return 1;
}

/* at every draw of a texture that has an upscaled one: draw the original instead */
static inline int texture_upscale_state_show_original(const struct texture_upscale_state *s)
{
	return s->show_original;
}

/* A second set, if configured: each map starts on the first, and the D-pad cycle then switches between the two sets,
never to the originals. */
static inline void texture_upscale_state_compare(struct texture_upscale_state *s, int configured)
{
	s->compare = configured && s->level_enabled;
	s->higher = s->fallback = s->show_original = 0;
}
