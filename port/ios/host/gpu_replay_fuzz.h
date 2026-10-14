/*
GPU_REPLAY_FUZZ.H

The fuzzer's mutations of a capture's records (gpu_capture.h's
gpu_replay_mutate): each makes a call that is wrong whatever the shaders and
data are, so the validator must refuse it (gpu_validate.c, REJECT mode). A
mutated call that reaches the backend is a hole in the validator.

Included by the app's replay (host_render_replay.c) and by the probe
(port/ios/tests/gpu_replay_probe.c).
*/

#ifndef __HALO_GPU_REPLAY_FUZZ_H
#define __HALO_GPU_REPLAY_FUZZ_H

#include <math.h>
#include <stddef.h>
#include <string.h>
#include "gpu_capture.h"

struct gpu_replay_fuzz
{
	uint64_t state;
	/* one record in this many is mutated */
	uint32_t every;
	/* the last mutation, for the log */
	const char *last;
	uint32_t mutations;
};

static uint32_t gpu_replay_fuzz_random(struct gpu_replay_fuzz *fuzz)
{
	fuzz->state = fuzz->state * 6364136223846793005ull + 1442695040888963407ull;
	return (uint32_t)(fuzz->state >> 33);
}

/* nonzero if it changed the record */
static int gpu_replay_fuzz_mutate(void *context, uint32_t operation, uint32_t *payload, uint32_t size)
{
	struct gpu_replay_fuzz *fuzz = context;
	float not_a_number = nanf("");

	fuzz->last = NULL;
	if (operation != GPU_CAPTURE_DRAW && operation != GPU_CAPTURE_CLEAR && operation != GPU_CAPTURE_TEXTURE_UPLOAD &&
		operation != GPU_CAPTURE_BUFFER_WRITE && operation != GPU_CAPTURE_VISIBILITY_END)
		return 0;
	if (fuzz->every > 1 && gpu_replay_fuzz_random(fuzz) % fuzz->every)
		return 0;
	if (operation == GPU_CAPTURE_DRAW && size >= 4 + sizeof(struct gpu_draw))
	{
		struct gpu_draw draw;
		uint32_t pick = gpu_replay_fuzz_random(fuzz) % 16, attribute = gpu_replay_fuzz_random(fuzz) % GPU_ATTRIBUTE_COUNT;

		memcpy(&draw, (const char *)payload + 4, sizeof(draw));
		switch (pick)
		{
		case 0: draw.attributes[attribute].format = 200; fuzz->last = "an attribute format outside the enum"; break;
		case 1: draw.attributes[attribute].stream = 77; fuzz->last = "an attribute stream past the last"; break;
		case 2: draw.primitive = 99; fuzz->last = "a primitive outside the enum"; break;
		case 3: draw.blend.source = 250; fuzz->last = "a blend factor outside the enum"; break;
		case 4: draw.depth_stencil.depth_function = 99; fuzz->last = "a depth function outside the enum"; break;
		case 5: draw.raster.cull_mode = 9; fuzz->last = "a cull mode outside the enum"; break;
		case 6: draw.viewport.min_z = not_a_number; fuzz->last = "a NaN depth range"; break;
		case 7: draw.viewport.width = 0; fuzz->last = "an empty viewport"; break;
		case 8: draw.scissor.width = -5; fuzz->last = "a negative scissor"; break;
		case 9: draw.color_target = draw.depth_target = 0; fuzz->last = "no targets"; break;
		case 10: draw.count = 0x02000000; fuzz->last = "33 million vertices"; break;
		case 11: draw.pixel_shader = draw.vertex_shader; fuzz->last = "a vertex shader as the pixel shader"; break;
		case 12: draw.raster.depth_bias_enable = 1; draw.raster.depth_bias_slope = not_a_number; fuzz->last = "a NaN depth bias"; break;
		case 13:
			if (draw.index_buffer)
			{
				draw.base_vertex = -70000;
				fuzz->last = "a base vertex below every index";
			}
			else
			{
				draw.base_vertex = 5;
				fuzz->last = "a base vertex without indices";
			}
			break;
		case 14:
			if (draw.index_buffer)
			{
				draw.index_offset |= 1;
				fuzz->last = "an odd index offset";
			}
			else
			{
				draw.count = 0x7fffffff;
				fuzz->last = "two billion vertices";
			}
			break;
		default:
			if (draw.color_target && draw.depth_target)
			{
				gpu_texture color = draw.color_target;

				draw.color_target = draw.depth_target;
				draw.depth_target = color;
				fuzz->last = "the color and depth targets exchanged";
			}
			else
			{
				draw.depth_target = 0;
				draw.depth_stencil.depth_test = 1;
				if (!draw.color_target)
					draw.viewport.height = 0;
				fuzz->last = "a depth test without a depth target";
			}
			break;
		}
		memcpy((char *)payload + 4, &draw, sizeof(draw));
	}
	else if (operation == GPU_CAPTURE_CLEAR && size >= 4 + sizeof(struct gpu_clear))
	{
		struct gpu_clear clear;

		memcpy(&clear, (const char *)payload + 4, sizeof(clear));
		if (gpu_replay_fuzz_random(fuzz) & 1)
		{
			clear.flags |= 0x80;
			fuzz->last = "clear flags outside the three";
		}
		else
		{
			clear.color_target = clear.depth_target = 0;
			fuzz->last = "a clear without targets";
		}
		memcpy((char *)payload + 4, &clear, sizeof(clear));
	}
	else if (operation == GPU_CAPTURE_TEXTURE_UPLOAD && size >= 16)
	{
		payload[2] = 40;
		fuzz->last = "an upload to level 40";
	}
	else if (operation == GPU_CAPTURE_BUFFER_WRITE && size >= 16)
	{
		payload[1] = 0xfffffff0u;
		fuzz->last = "a buffer write past the end";
	}
	else if (operation == GPU_CAPTURE_VISIBILITY_END && size >= 4)
	{
		payload[0] = 5000;
		fuzz->last = "a visibility slot past the last";
	}
	if (fuzz->last)
		fuzz->mutations++;
	return fuzz->last != NULL;
}

#endif
