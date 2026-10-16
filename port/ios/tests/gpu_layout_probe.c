/* prints _Static_asserts pinning every gpu.h struct's size and field offsets
(and halo_stereo.h's, which host_stereo_frame passes)
as this (64-bit host) compiler lays them out; tools/ios_test.py compiles the
output with the guest's arm64_32 flags, where a mismatch fails the build */
#include "gpu.h"
#include "halo_stereo.h"

#include <stddef.h>
#include <stdio.h>

#define SIZE(type) printf("_Static_assert(sizeof(struct " #type ") == %zu, \"struct " #type "\");\n", sizeof(struct type))
#define FIELD(type, field) printf("_Static_assert(offsetof(struct " #type ", " #field ") == %zu, \"" #type "." #field "\");\n", offsetof(struct type, field))

int main(void)
{
	printf("#include <stddef.h>\n#include \"gpu.h\"\n#include \"halo_stereo.h\"\n");
	SIZE(gpu_compile_progress);
	FIELD(gpu_compile_progress, total);
	FIELD(gpu_compile_progress, completed);
	FIELD(gpu_compile_progress, failed);
	FIELD(gpu_compile_progress, sealed);
	SIZE(gpu_capabilities);
	FIELD(gpu_capabilities, shader_language);
	FIELD(gpu_capabilities, max_texture_size);
	SIZE(gpu_texture_description);
	FIELD(gpu_texture_description, foveated_eye);
	FIELD(gpu_texture_description, width);
	FIELD(gpu_texture_description, levels);
	SIZE(gpu_constant_store);
	FIELD(gpu_constant_store, serials);
	FIELD(gpu_constant_store, serial);
	FIELD(gpu_constant_store, log);
	SIZE(gpu_uniforms);
	FIELD(gpu_uniforms, serial);
	SIZE(gpu_viewport);
	SIZE(gpu_rect);
	SIZE(gpu_depth_stencil_state);
	SIZE(gpu_blend_state);
	SIZE(gpu_raster_state);
	SIZE(gpu_sampler_state);
	FIELD(gpu_sampler_state, max_mip_level);
	FIELD(gpu_sampler_state, lod_bias);
	FIELD(gpu_sampler_state, border_color);
	SIZE(gpu_stage);
	FIELD(gpu_stage, sampler);
	SIZE(gpu_vertex_stream);
	SIZE(gpu_vertex_attribute);
	FIELD(gpu_vertex_attribute, offset);
	SIZE(gpu_draw);
	FIELD(gpu_draw, viewport);
	FIELD(gpu_draw, scissor);
	FIELD(gpu_draw, depth_stencil);
	FIELD(gpu_draw, blend);
	FIELD(gpu_draw, raster);
	FIELD(gpu_draw, stages);
	FIELD(gpu_draw, streams);
	FIELD(gpu_draw, attributes);
	FIELD(gpu_draw, constant_values);
	FIELD(gpu_draw, index_buffer);
	FIELD(gpu_draw, base_vertex);
	SIZE(gpu_pipeline_description);
	FIELD(gpu_pipeline_description, blend);
	FIELD(gpu_pipeline_description, write_mask);
	FIELD(gpu_pipeline_description, alpha_source);
	FIELD(gpu_pipeline_description, alpha_destination);
	FIELD(gpu_pipeline_description, attribute_kinds);
	SIZE(gpu_clear);
	FIELD(gpu_clear, depth);
	FIELD(gpu_clear, stencil);
	SIZE(gpu_stereo_present);
	FIELD(gpu_stereo_present, eye_depth);
	FIELD(gpu_stereo_present, hud);
	FIELD(gpu_stereo_present, near_meters);
	FIELD(gpu_stereo_present, far_meters);
	FIELD(gpu_stereo_present, mode);
	FIELD(gpu_stereo_present, cinematic);
	FIELD(gpu_stereo_present, fade);
	FIELD(gpu_stereo_present, hud_aspect);
	FIELD(gpu_stereo_present, vignette);
	FIELD(gpu_stereo_present, cut_covered);
	FIELD(gpu_stereo_present, hud_ui);
	FIELD(gpu_stereo_present, reticle);
	FIELD(gpu_stereo_present, hud_tangents);
	FIELD(gpu_stereo_present, zoom);
	FIELD(gpu_stereo_present, zoom_tangents);
	FIELD(gpu_stereo_present, reticle_layer);
	FIELD(gpu_stereo_present, hud_group);
	FIELD(gpu_stereo_present, hud_group_extent);
	FIELD(gpu_stereo_present, ui);
	FIELD(gpu_stereo_present, ui_dim);
	FIELD(gpu_stereo_present, expanding);
	FIELD(gpu_stereo_present, expansion);
	FIELD(gpu_stereo_present, expansion_bars);
	FIELD(gpu_stereo_present, cutscene);
	FIELD(gpu_stereo_present, cutscene_forward);
	FIELD(gpu_stereo_present, cutscene_up);
	FIELD(gpu_stereo_present, cutscene_tangents);
	FIELD(gpu_stereo_present, cutscene_dim);
	FIELD(gpu_stereo_present, cut_requested);
	/* host_stereo_frame's (guest_host.h) */
	SIZE(halo_stereo_eye);
	FIELD(halo_stereo_eye, left);
	FIELD(halo_stereo_eye, down);
	SIZE(halo_stereo_frame);
	FIELD(halo_stereo_frame, mode);
	FIELD(halo_stereo_frame, head_yaw);
	FIELD(halo_stereo_frame, head_roll);
	FIELD(halo_stereo_frame, eyes);
	FIELD(halo_stereo_frame, eye_width);
	FIELD(halo_stereo_frame, eye_height);
	FIELD(halo_stereo_frame, foveated);
	FIELD(halo_stereo_frame, foveated_width);
	FIELD(halo_stereo_frame, foveated_height);
	return 0;
}
