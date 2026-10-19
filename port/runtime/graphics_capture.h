/* Public performance configuration only. Do not add paths or user/account/network data. */
#pragma once
#define CONFIG_GRAPHICS_MAXIMUM_BYTES (32 * 1024)
#define CONFIG_GRAPHICS_MAXIMUM_STRING_BYTES 256
#define CONFIG_GRAPHICS_MAXIMUM_KEYS 128
#define GRAPHICS_CAPTURE_DEBUG_KEYS(X) \
    X("debug.foveation_eye_passes") \
    X("debug.anti_aliasing_folded") \
    X("debug.rate_map_test") \
    X("debug.fixed_timestep") \
    X("debug.null_renderer") \
    X("debug.fixed_timestep_paced") \
    X("debug.visibility_any_sample") \
    X("debug.frame_load_cpu") \
    X("debug.frame_load_every") \
    X("debug.frame_load_gpu") \
    X("debug.frame_counter") \
    X("debug.gl_debug") \
    X("debug.render_validation") \
    X("debug.metal_state_cache") \
    X("debug.shader_list_warm") \
    X("debug.metal_pipeline_archive") \
    X("debug.metal_specialize") \
    X("debug.metal_vertex_bgra") \
    X("debug.metal_base_vertex") \
    X("debug.gpu_flush_draws") \
    X("debug.gpu_stats") \
    X("debug.texture_no_cache")
