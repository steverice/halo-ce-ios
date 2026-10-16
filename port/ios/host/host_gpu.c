/*
HOST_GPU.C

The platform functions the GPU backend (port/linux/src/gpu_gl.c) calls, for
the host it runs in on iOS and tvOS.
*/

#include "ios_host.h"
#include "host_external.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>

/* the guest's platform_log (xbox_kernel.c), prefix and stream included, so
the backend's lines land in stderr.log among the guest's: stderr is
unbuffered (host_main.m) and both sides write it from the GL thread */
void platform_log(const char *format, ...)
{
	va_list arguments;

	fputs("halo-linux: ", stderr);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

void platform_video_drawable_size(int *width, int *height)
{
	*width = 0;
	*height = 0;
    if(!host_external_size(width,height))SDL_GetWindowSizeInPixels(host_sdl_window(), width, height);
}

void platform_video_swap(void)
{
	/* display.frame_rate's cap (host_gpu_dispatch.c) */
	host_frame_rate_limit();
	SDL_GL_SwapWindow(host_sdl_window());
}
