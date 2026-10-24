/* Pure mode policy; UIKit tracks negotiation and marks refused candidates. */
#pragma once
#include <stddef.h>
struct host_external_mode { int width, height; double pixel_aspect; int refused; };
int host_external_mode_choose(const struct host_external_mode *modes, size_t count,
    int preferred, int cap, int forced_width, int forced_height);
/* Parse and validate the entire comma-separated list before selecting index.
   Empty and "preferred" select policy (0x0); dimensions are bounded at 32768. */
int host_external_mode_parse(const char *text, int index, int *width, int *height, int *count);
/* Physical modes and renderer allocation are separate: never upscale. */
int host_external_mode_drawable(int width,int height,int cap,int *out_width,int *out_height);
