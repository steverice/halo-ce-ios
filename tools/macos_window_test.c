/* Exercise actual host window creation without creating an OS window. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../port/ios/host/host_sdl.c"

static SDL_WindowFlags captured;
static char captured_title[32];
static char fake_window;
SDL_Window *SDL_CreateWindow(const char *title,int width,int height,SDL_WindowFlags flags) {
    assert(title && width==1000 && height==700);
    snprintf(captured_title,sizeof(captured_title),"%s",title);
    captured=flags;return (SDL_Window *)&fake_window;
}
SDL_MetalView SDL_Metal_CreateView(SDL_Window *window) { assert(window==(SDL_Window *)&fake_window);return &fake_window; }
const char *SDL_GetError(void) { return "test"; }
void host_ios_touch_attach(SDL_Window *window) { assert(window==(SDL_Window *)&fake_window); }
void host_external_attach(void *window,int metal) { assert(window==&fake_window && metal); }
void host_logf(int priority,const char *format,...) { (void)priority;(void)format; }
static void config(const char *text) {
    FILE *file=fopen("config.toml","w");assert(file);assert(fputs(text,file)>=0);assert(fclose(file)==0);
}
static void create(int fullscreen) {
    SDL_WindowFlags request=SDL_WINDOW_METAL|SDL_WINDOW_HIGH_PIXEL_DENSITY|SDL_WINDOW_HIDDEN|SDL_WINDOW_FULLSCREEN;
    assert(host_sdl_create_window("Halo",1000,700,(int64_t)request));
#if HALO_NATIVE_MAC
    assert(!strcmp(captured_title,"VISR"));
    assert(!!(captured&SDL_WINDOW_FULLSCREEN)==fullscreen);
    assert(captured&SDL_WINDOW_RESIZABLE);
#else
    assert(!strcmp(captured_title,"Halo"));
    (void)fullscreen;assert(captured==request);
#endif
    assert(captured&SDL_WINDOW_METAL);assert(captured&SDL_WINDOW_HIGH_PIXEL_DENSITY);assert(captured&SDL_WINDOW_HIDDEN);
}
int main(void) {
    char cwd[4096];assert(getcwd(cwd,sizeof(cwd)));
    char directory[]="/tmp/visr-native-window-XXXXXX";assert(mkdtemp(directory));assert(chdir(directory)==0);
    unsetenv("HALO_FULLSCREEN");unsetenv("HALO_DISPLAY_MODE");
    config("[display]\n");create(0); /* Shared ILP32 guest always requests fullscreen; native default must override it. */
    config("[display]\nfullscreen = true\n");create(1);
    config("[display]\nfullscreen = false\n");create(0);
    config("[display]\nmode = \"windowed\"\nfullscreen = true\n");create(0);
    config("[display]\nmode = \"fullscreen\"\n");create(1);
    config("[display]\nmode = \"borderless\"\n");create(1);
    config("[display]\nmode = \"invalid\"\n");create(0);
    config("[display]\n");assert(setenv("HALO_FULLSCREEN","true",1)==0);create(1);
    assert(setenv("HALO_DISPLAY_MODE","windowed",1)==0);create(0);
    unsetenv("HALO_FULLSCREEN");unsetenv("HALO_DISPLAY_MODE");
    assert(unlink("config.toml")==0);assert(chdir(cwd)==0);assert(rmdir(directory)==0);
#if HALO_NATIVE_MAC
    puts("Actual native host creation: windowed default and explicit fullscreen paths passed");
#else
    puts("Actual mobile host creation preserved every incoming window flag");
#endif
    return 0;
}
