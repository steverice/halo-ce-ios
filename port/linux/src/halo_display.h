/* Physical render size, independent of the Xbox's 480-line layout space. */
#ifndef HALO_DISPLAY_H
#define HALO_DISPLAY_H
#include <math.h>

struct halo_display_size { long width, height; };

/* The guest layout follows a changed output only between frames. */
static inline long halo_display_layout_width(long pixels_x, long pixels_y, long requested, long fallback)
{
    long width = requested > 0 ? requested : pixels_x > 0 && pixels_y > 0 ?
        (480 * pixels_x / pixels_y) : fallback;
    return (width < 640 ? 640 : width > 1600 ? 1600 : width) & ~1L;
}

static inline struct halo_display_size halo_display_render_size(
    long display_width, long display_height, long logical_width,
    long requested_width, long requested_height, long maximum_dimension)
{
    struct halo_display_size size;
    double ratio, height;
    if (display_width <= 0 || display_height <= 0) {
        display_width = logical_width;
        display_height = 480;
    }
    /* Auto uses the physical display's exact ratio, not the rounded number
       of logical columns. Explicit screen_width keeps its chosen aspect. */
    ratio = requested_width <= 0 ? (double)display_width / display_height :
        (double)logical_width / 480.0;
    if (ratio < 4.0 / 3.0) ratio = 4.0 / 3.0;
    if (ratio > 1600.0 / 480.0) ratio = 1600.0 / 480.0;
    height = fmin((double)display_height, (double)display_width / ratio);
    if (requested_height > 0) height = fmin(height, fmax(240.0, (double)requested_height));
    if (maximum_dimension <= 0) maximum_dimension = 8192;
    height = fmin(height, fmin((double)maximum_dimension, maximum_dimension / ratio));
    size.width = (long)floor(height * ratio + 0.5);
    size.height = (long)floor(height + 0.5);
    if (size.width < 1) size.width = 1;
    if (size.height < 1) size.height = 1;
    return size;
}

/* Where gpu_present puts a picture in the drawable: fitted to it, keeping the
   picture's shape, and centered. A picture within a pixel of its fitted size
   (the display-sized back buffer of display.native_hud where the display's
   shape was clamped and its width rounded) is shown at its own size, so it
   is copied and not resampled. */
static inline struct halo_display_size halo_display_present_size(
    long window_width, long window_height, long picture_width, long picture_height)
{
    struct halo_display_size size;
    size.width = window_width;
    size.height = window_width * picture_height / picture_width;
    if (size.height > window_height) {
        size.height = window_height;
        size.width = window_height * picture_width / picture_height;
    }
    if (picture_width <= window_width && picture_height <= window_height &&
        size.width - picture_width >= -1 && size.width - picture_width <= 1 &&
        size.height - picture_height >= -1 && size.height - picture_height <= 1) {
        size.width = picture_width;
        size.height = picture_height;
    }
    return size;
}
#endif
