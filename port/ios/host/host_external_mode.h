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
/* Where a drawable of width x height sits in a screen of bounds_width x bounds_height points: the largest
   aspect-correct rectangle, centered (out is x, y, width, height). The layer is scaled up to it, so a picture
   rendered below the screen's size fills the screen, with bars only where the shapes differ (within half a
   percent counts as the same shape and takes the whole screen). Returns 0 for a size it cannot place. */
int host_external_fit(double bounds_width,double bounds_height,int width,int height,double out[4]);
