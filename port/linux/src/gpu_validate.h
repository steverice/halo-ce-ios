/*
GPU_VALIDATE.H

Render validation (builds configured with HALO_RENDER_VALIDATION only): a
layer at gpu.h's boundary that checks every call the Direct3D front end makes
before the backend sees it, and gives every place a backend or the front end
substitutes, ignores or skips something a name and a count.

The checks follow innerhat's OpenCE host (tag test-v0.3.0-net28-mac13,
port/macos/host/host_metal.mm's validate and metal_draw_encoder.mm's
validateDraw and validateBindings, after pfista's halo-og), which refuses a
malformed frame with a reason before any Metal call. His host validates a
copy of the frame; this one reads the game's vertex and index memory where it
lies (the mirror's pages and the streamed ranges are the game's own memory),
so validating adds no copy, and what the mirror holds is scanned once per
upload, not once per frame.

Three parts:
- the validator (port/ios/host/gpu_validate.c), which wraps a struct gpu_backend: both
  backends get it from the one table (port/ios/host/host_gpu_dispatch.c);
- gpu_validation_report, which a backend calls where it falls back or skips
  (GPU_VALIDATION_REPORT, which is nothing in a build without validation);
- the front end's side (d3d8_device.c, in the guest): each draw's identity
  and the front end's own fallbacks, sent through two imports that exist only
  in a validation build (gpu_validation_identity, gpu_validation_note).

It compiles in the 64-bit host and the 32-bit guest: fixed-width fields only.
*/

#ifndef __HALO_GPU_VALIDATE_H
#define __HALO_GPU_VALIDATE_H

#include <stdint.h>
#include "gpu.h"

/* debug.render_validation */
enum
{
	GPU_VALIDATION_OFF,
	/* each distinct cause is logged once, with the draw it first happened
	in, and counted */
	GPU_VALIDATION_LOG,
	/* as LOG, and the first failure or fallback stops the run */
	GPU_VALIDATION_STRICT,
	/* as LOG, and a call with a failure is not passed on to the backend
	(a replay of a capture nobody vouches for: port/ios/host/gpu_replay.c) */
	GPU_VALIDATION_REJECT,
};

/* what a reason means for the run */
enum
{
	/* the call is wrong: a backend would read outside its data, draw the
	wrong thing or drop the draw */
	GPU_VALIDATION_FAILURE,
	/* a backend or the front end substitutes, ignores or skips, silently in
	a build without validation */
	GPU_VALIDATION_FALLBACK,
	/* legal, and handled alike by both backends, but worth a look */
	GPU_VALIDATION_WARNING,
	/* intended (a draw skipped while its pipeline compiles on a worker) */
	GPU_VALIDATION_EXPECTED,
};

/* every reason: X(name as the code spells it, name as the log spells it, class) */
#define GPU_VALIDATION_REASONS(X) \
	/* ---- draws: geometry */ \
	X(INDEX_OUT_OF_RANGE, "index-out-of-range", GPU_VALIDATION_FAILURE) \
	X(INDEX_BELOW_ZERO, "index-below-zero", GPU_VALIDATION_FAILURE) \
	X(INDEX_RANGE_OUT_OF_RANGE, "index-range-out-of-range", GPU_VALIDATION_FAILURE) \
	X(INDEX_OFFSET_MISALIGNED, "index-offset-misaligned", GPU_VALIDATION_FAILURE) \
	X(VERTEX_RANGE_OUT_OF_RANGE, "vertex-range-out-of-range", GPU_VALIDATION_FAILURE) \
	X(STREAM_UNBOUND, "stream-unbound", GPU_VALIDATION_FAILURE) \
	X(STREAM_STALE, "stream-stale", GPU_VALIDATION_FAILURE) \
	X(BUFFER_UNKNOWN, "buffer-unknown", GPU_VALIDATION_FAILURE) \
	X(ATTRIBUTE_INVALID, "attribute-invalid", GPU_VALIDATION_FAILURE) \
	X(ATTRIBUTE_OUTSIDE_STRIDE, "attribute-outside-stride", GPU_VALIDATION_FAILURE) \
	X(BASE_VERTEX_UNSUPPORTED, "base-vertex-unsupported", GPU_VALIDATION_FAILURE) \
	X(PRIMITIVE_INVALID, "primitive-invalid", GPU_VALIDATION_FAILURE) \
	X(PRIMITIVE_UNSUPPORTED, "primitive-unsupported", GPU_VALIDATION_FAILURE) \
	X(PRIMITIVE_INCOMPLETE, "primitive-incomplete", GPU_VALIDATION_WARNING) \
	X(DRAW_EMPTY, "draw-empty", GPU_VALIDATION_WARNING) \
	X(DRAW_TOO_LARGE, "draw-too-large", GPU_VALIDATION_FAILURE) \
	/* ---- draws: numbers */ \
	X(VERTEX_NONFINITE, "vertex-nonfinite", GPU_VALIDATION_FAILURE) \
	X(VERTEX_HUGE, "vertex-huge", GPU_VALIDATION_WARNING) \
	X(VERTEX_DENORMAL, "vertex-denormal", GPU_VALIDATION_WARNING) \
	X(ATTRIBUTE_CONSTANT_NONFINITE, "attribute-constant-nonfinite", GPU_VALIDATION_FAILURE) \
	X(CONSTANT_NONFINITE, "constant-nonfinite", GPU_VALIDATION_FAILURE) \
	X(UNIFORM_NONFINITE, "uniform-nonfinite", GPU_VALIDATION_FAILURE) \
	X(DEPTH_BIAS_NONFINITE, "depth-bias-nonfinite", GPU_VALIDATION_FAILURE) \
	/* ---- draws: shaders, textures and samplers */ \
	X(SHADER_MISSING, "shader-missing", GPU_VALIDATION_FALLBACK) \
	X(SHADER_UNKNOWN, "shader-unknown", GPU_VALIDATION_FAILURE) \
	X(SHADER_STAGE_MISMATCH, "shader-stage-mismatch", GPU_VALIDATION_FAILURE) \
	X(SAMPLER_UNBOUND, "sampler-unbound", GPU_VALIDATION_FALLBACK) \
	X(TEXTURE_UNKNOWN, "texture-unknown", GPU_VALIDATION_FAILURE) \
	X(TEXTURE_TYPE_MISMATCH, "texture-type-mismatch", GPU_VALIDATION_FAILURE) \
	X(TEXTURE_NO_STORAGE, "texture-no-storage", GPU_VALIDATION_FALLBACK) \
	X(TEXTURE_INCOMPLETE_MIPS, "texture-incomplete-mips", GPU_VALIDATION_FAILURE) \
	X(TEXTURE_FORMAT_UNSUPPORTED, "texture-format-unsupported", GPU_VALIDATION_FALLBACK) \
	X(TEXTURE_DEPTH_SAMPLED, "texture-depth-sampled", GPU_VALIDATION_WARNING) \
	X(TEXTURE_IS_TARGET, "texture-is-target", GPU_VALIDATION_FAILURE) \
	X(SAMPLER_STATE_INVALID, "sampler-state-invalid", GPU_VALIDATION_FAILURE) \
	X(FILTER_UNSUPPORTED, "filter-unsupported", GPU_VALIDATION_FALLBACK) \
	X(ANISOTROPY_CLAMPED, "anisotropy-clamped", GPU_VALIDATION_FALLBACK) \
	X(BORDER_UNSUPPORTED, "border-unsupported", GPU_VALIDATION_FALLBACK) \
	X(SAMPLER_MIP_OUT_OF_RANGE, "sampler-mip-out-of-range", GPU_VALIDATION_WARNING) \
	X(SAMPLER_LOD_BIAS_NONFINITE, "sampler-lod-bias-nonfinite", GPU_VALIDATION_FAILURE) \
	/* ---- draws and clears: targets, viewport, scissor, state */ \
	X(TARGET_NONE, "target-none", GPU_VALIDATION_FAILURE) \
	X(TARGET_UNKNOWN, "target-unknown", GPU_VALIDATION_FAILURE) \
	X(TARGET_NOT_RENDER_TARGET, "target-not-render-target", GPU_VALIDATION_FAILURE) \
	X(TARGET_FORMAT, "target-format", GPU_VALIDATION_FAILURE) \
	X(DEPTH_TARGET_SMALLER, "depth-target-smaller", GPU_VALIDATION_FAILURE) \
	X(DEPTH_TARGET_LARGER, "depth-target-larger", GPU_VALIDATION_WARNING) \
	X(TARGET_FOVEATION_MISMATCH, "target-foveation-mismatch", GPU_VALIDATION_FAILURE) \
	X(DEPTH_STATE_WITHOUT_TARGET, "depth-state-without-target", GPU_VALIDATION_FAILURE) \
	X(VIEWPORT_EMPTY, "viewport-empty", GPU_VALIDATION_FAILURE) \
	X(VIEWPORT_DEPTH_RANGE, "viewport-depth-range", GPU_VALIDATION_FAILURE) \
	X(VIEWPORT_OUTSIDE_TARGET, "viewport-outside-target", GPU_VALIDATION_WARNING) \
	X(SCISSOR_INVALID, "scissor-invalid", GPU_VALIDATION_FAILURE) \
	X(SCISSOR_OUTSIDE_TARGET, "scissor-outside-target", GPU_VALIDATION_WARNING) \
	X(SCISSOR_EMPTY, "scissor-empty", GPU_VALIDATION_WARNING) \
	X(STATE_INVALID, "state-invalid", GPU_VALIDATION_FAILURE) \
	X(FILL_MODE_IGNORED, "fill-mode-ignored", GPU_VALIDATION_FALLBACK) \
	X(CLEAR_INVALID, "clear-invalid", GPU_VALIDATION_FAILURE) \
	X(CLEAR_DEPTH_WITHOUT_TARGET, "clear-depth-without-target", GPU_VALIDATION_FALLBACK) \
	X(CLEAR_DEPTH_RANGE, "clear-depth-range", GPU_VALIDATION_FAILURE) \
	X(CLEAR_RECT_OUTSIDE_TARGET, "clear-rect-outside-target", GPU_VALIDATION_WARNING) \
	X(CLEAR_EMPTY, "clear-empty", GPU_VALIDATION_WARNING) \
	/* ---- resources */ \
	X(TEXTURE_DESCRIPTION_INVALID, "texture-description-invalid", GPU_VALIDATION_FAILURE) \
	X(TEXTURE_LEVELS_CLAMPED, "texture-levels-clamped", GPU_VALIDATION_FALLBACK) \
	X(TEXTURE_TOO_LARGE, "texture-too-large", GPU_VALIDATION_FAILURE) \
	X(UPLOAD_INVALID, "upload-invalid", GPU_VALIDATION_FAILURE) \
	X(UPLOAD_SHORT, "upload-short", GPU_VALIDATION_FAILURE) \
	X(COPY_INVALID, "copy-invalid", GPU_VALIDATION_FAILURE) \
	X(MIPMAPS_INVALID, "mipmaps-invalid", GPU_VALIDATION_FAILURE) \
	X(DESTROY_UNKNOWN, "destroy-unknown", GPU_VALIDATION_FAILURE) \
	X(BUFFER_WRITE_OUT_OF_RANGE, "buffer-write-out-of-range", GPU_VALIDATION_FAILURE) \
	X(BUFFER_SIZE_INVALID, "buffer-size-invalid", GPU_VALIDATION_FAILURE) \
	X(STREAM_INVALID, "stream-invalid", GPU_VALIDATION_FAILURE) \
	X(SHADER_COMPILE_FAILED, "shader-compile-failed", GPU_VALIDATION_FAILURE) \
	X(VISIBILITY_INVALID, "visibility-invalid", GPU_VALIDATION_FAILURE) \
	X(PRESENT_INVALID, "present-invalid", GPU_VALIDATION_FAILURE) \
	/* ---- what a backend reports (GPU_VALIDATION_REPORT) */ \
	X(DRAW_SKIPPED_UNEXPLAINED, "draw-skipped-unexplained", GPU_VALIDATION_FAILURE) \
	X(PIPELINE_COMPILING, "pipeline-compiling", GPU_VALIDATION_EXPECTED) \
	X(PIPELINE_FAILED, "pipeline-failed", GPU_VALIDATION_FAILURE) \
	X(PROGRAM_LINK_FAILED, "program-link-failed", GPU_VALIDATION_FAILURE) \
	X(PASS_NOT_BEGUN, "pass-not-begun", GPU_VALIDATION_FAILURE) \
	X(DRAW_DROPPED, "draw-dropped", GPU_VALIDATION_FALLBACK) \
	X(SPECIALIZATION_FAILED, "specialization-failed", GPU_VALIDATION_FALLBACK) \
	X(EXACT_BORDERS_FAILED, "exact-borders-failed", GPU_VALIDATION_FALLBACK) \
	X(UPLOAD_IGNORED, "upload-ignored", GPU_VALIDATION_FALLBACK) \
	X(COPY_IGNORED, "copy-ignored", GPU_VALIDATION_FALLBACK) \
	X(MIPMAPS_IGNORED, "mipmaps-ignored", GPU_VALIDATION_FALLBACK) \
	X(READ_UNSUPPORTED, "read-unsupported", GPU_VALIDATION_FALLBACK) \
	X(DEPTH_CLAMP_UNSUPPORTED, "depth-clamp-unsupported", GPU_VALIDATION_FALLBACK) \
	X(FRAMEBUFFER_INCOMPLETE, "framebuffer-incomplete", GPU_VALIDATION_FAILURE) \
	X(VISIBILITY_LAPPED, "visibility-lapped", GPU_VALIDATION_FALLBACK) \
	X(UPSCALER_UNAVAILABLE, "upscaler-unavailable", GPU_VALIDATION_FALLBACK) \
	X(COMMAND_BUFFER_FAILED, "command-buffer-failed", GPU_VALIDATION_FAILURE) \
	X(GL_ERROR, "gl-error", GPU_VALIDATION_FAILURE) \
	/* ---- what the front end reports (gpu_validation_note) */ \
	X(FRONT_NO_PROGRAM, "front-no-program", GPU_VALIDATION_FALLBACK) \
	X(FRONT_NO_TARGET, "front-no-target", GPU_VALIDATION_FALLBACK) \
	X(FRONT_SKIP_LIST, "front-skip-list", GPU_VALIDATION_EXPECTED) \
	X(FRONT_STATE_UNKNOWN, "front-state-unknown", GPU_VALIDATION_FALLBACK) \
	X(FRONT_BLEND_SIGNED, "front-blend-signed", GPU_VALIDATION_FALLBACK) \
	X(FRONT_ATTRIBUTE_TYPE_UNKNOWN, "front-attribute-type-unknown", GPU_VALIDATION_FALLBACK) \
	X(FRONT_TEXTURE_FORMAT_UNKNOWN, "front-texture-format-unknown", GPU_VALIDATION_FALLBACK) \
	X(FRONT_TEXTURE_NOT_DRAWN, "front-texture-not-drawn", GPU_VALIDATION_FALLBACK) \
	X(FRONT_PREVIOUS_BACK_BUFFER, "front-previous-back-buffer", GPU_VALIDATION_FALLBACK)

#define GPU_VALIDATION_REASON_ENUM(name, text, class) GPU_VALIDATION_##name,
enum
{
	GPU_VALIDATION_REASONS(GPU_VALIDATION_REASON_ENUM)
	GPU_VALIDATION_REASON_COUNT
};
#undef GPU_VALIDATION_REASON_ENUM

/* how the front end made the next draw (gpu_validation_identity.kind) */
enum
{
	GPU_VALIDATION_DRAW_VERTICES = 1,
	GPU_VALIDATION_DRAW_INDEXED,
	GPU_VALIDATION_DRAW_IMMEDIATE,
	GPU_VALIDATION_DRAW_CLEAR,
};

/* the next gpu_draw's or gpu_clear's identity, as the front end knows it:
the vertex program's number and variant and the pixel shader key's hash (the
names debug.gpu_dump_shaders gives their files: vs059_0, ps_6c143490), and
the game code that asked for the draw (the D3D entry point's return address,
which halo_guest.elf.map resolves). Fixed-width: it crosses to the host. */
struct gpu_validation_identity
{
	uint32_t kind;
	uint32_t vertex_program, vertex_variant;
	uint32_t pixel_key;
	uint32_t caller;
	uint32_t frame;
};

#if HALO_RENDER_VALIDATION

/* the front end's: before the gpu_draw or gpu_clear it describes */
void gpu_validation_identity(const struct gpu_validation_identity *identity);
/* the front end's: one of the FRONT reasons, with two numbers that tell its
causes apart (a state and its value, a format) */
void gpu_validation_note(uint32_t reason, uint32_t detail, uint32_t value);

#endif

/* ---------- the host's side (64-bit only from here) */

#if !HALO_ILP32

/* what the validator needs from its host: where lines go, and how a strict
run stops (it does not return) */
struct gpu_validation_host
{
	void (*log)(const char *line);
	void (*stop)(const char *line);
	/* GPU_VALIDATION_* */
	int mode;
	/* reasons a strict run does not stop for, separated by commas; NULL for none */
	const char *allow;
};

/* replaces every function of table with one that checks its arguments and
then calls the function it replaced; mode OFF leaves the table alone */
void gpu_validation_install(struct gpu_backend *table, const struct gpu_validation_host *host);
/* a backend's or the validator's own finding: counted under the reason and
cause (which tells apart the causes of one reason: a format, a shader), and
logged with the current draw the first time. Any thread. */
void gpu_validation_report(uint32_t reason, uint64_t cause, const char *format, ...)
	__attribute__((format(printf, 3, 4)));
/* every cause so far with its count; also runs when the process exits */
void gpu_validation_summary(void);
const char *gpu_validation_reason_name(uint32_t reason);
/* findings of each class so far (tests) */
unsigned long gpu_validation_count(uint32_t reason);
/* calls a REJECT run did not pass on, and failures so far */
unsigned long gpu_validation_rejected(void);
unsigned long gpu_validation_failures(void);
/* forgets everything (tests) */
void gpu_validation_reset(void);

#endif

/* a backend's report, which is nothing in a build without validation */
#if HALO_RENDER_VALIDATION
#define GPU_VALIDATION_REPORT(reason, cause, ...) gpu_validation_report(reason, cause, __VA_ARGS__)
#else
#define GPU_VALIDATION_REPORT(reason, cause, ...) ((void)0)
#endif

#endif
