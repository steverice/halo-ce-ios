// glft: present-interval recorder for OpenGL Mac apps, inserted with DYLD_INSERT_LIBRARIES.
// Hooks -[NSOpenGLContext flushBuffer] and CGLFlushDrawable. Each present appends two uint64 values to a
// memory-mapped file (GLFT_OUT): the mach_absolute_time at entry, and the ticks spent in an optional glFinish
// (GLFT_FINISH=1, diagnostic only: it serializes CPU and GPU). The file survives SIGTERM because the pages are shared.
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <objc/runtime.h>
#import <mach/mach_time.h>
#import <sys/mman.h>
#import <fcntl.h>
#import <unistd.h>
#import <stdatomic.h>
#import <stdio.h>
#import <string.h>

#define CAP (1u << 22)
typedef struct { char magic[8]; _Atomic uint64_t count; uint32_t numer, denom; uint64_t pid, finish, cap; } Header;
static Header *header; static uint64_t *rows; static int finishMode; static char outPath[1024];
static __thread int inFlush; static atomic_flag described = ATOMIC_FLAG_INIT;

static void describe(void) {
    if (atomic_flag_test_and_set(&described)) return;
    char path[1100]; snprintf(path, sizeof path, "%s.txt", outPath);
    FILE *f = fopen(path, "w"); if (!f) return;
    GLint vp[4] = {0}; glGetIntegerv(GL_VIEWPORT, vp);
    fprintf(f, "viewport %d %d %d %d\nrenderer %s\nversion %s\n", vp[0], vp[1], vp[2], vp[3],
            (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    glGetError();
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (glGetError() == GL_NO_ERROR && ext) fprintf(f, "timer_query %d\n", strstr(ext, "timer_query") != NULL);
    else fprintf(f, "timer_query unknown (core profile)\n");
    GLint swap = 0; CGLGetParameter(CGLGetCurrentContext(), kCGLCPSwapInterval, &swap);
    fprintf(f, "swap_interval %d\n", swap);
    fclose(f);
}

static void record(uint64_t t, uint64_t finish) {
    uint64_t i = atomic_fetch_add(&header->count, 1);
    if (i < CAP) { rows[2 * i] = t; rows[2 * i + 1] = finish; } else atomic_store(&header->count, CAP);
}

static void before(void) {
    describe();
}
static uint64_t doFinish(void) {
    if (!finishMode) return 0;
    uint64_t a = mach_absolute_time(); glFinish(); return mach_absolute_time() - a;
}

static id (*origNext)(id, SEL);
static id hookedNext(id self, SEL cmd) { record(mach_absolute_time(), 0); return origNext(self, cmd); }  // GLFT_METAL=1: validation on Metal ports
static void (*origFlush)(id, SEL);
static void hookedFlush(id self, SEL cmd) {
    before(); uint64_t t = mach_absolute_time(); uint64_t fin = doFinish(); record(t, fin);
    inFlush++; origFlush(self, cmd); inFlush--;
}
static CGLError (*realCGLFlush)(CGLContextObj);
static CGLError hookedCGLFlush(CGLContextObj ctx) {
    if (inFlush) return CGLFlushDrawable(ctx);
    before(); uint64_t t = mach_absolute_time(); uint64_t fin = doFinish(); record(t, fin);
    return CGLFlushDrawable(ctx);
}
#define DYLD_INTERPOSE(r, o) __attribute__((used)) static struct { const void *r; const void *o; } _i_##o \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)(unsigned long)&r, (const void *)(unsigned long)&o }
DYLD_INTERPOSE(hookedCGLFlush, CGLFlushDrawable);

__attribute__((constructor)) static void glftInit(void) {
    const char *out = getenv("GLFT_OUT"); if (!out) return;
    strlcpy(outPath, out, sizeof outPath);
    finishMode = getenv("GLFT_FINISH") && getenv("GLFT_FINISH")[0] == '1';
    int fd = open(out, O_RDWR | O_CREAT | O_TRUNC, 0644); if (fd < 0) return;
    size_t size = sizeof(Header) + (size_t)CAP * 16; ftruncate(fd, size);
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
    if (map == MAP_FAILED) return;
    header = map; rows = (uint64_t *)((char *)map + sizeof(Header));
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    memcpy(header->magic, "GLFT0001", 8); header->numer = tb.numer; header->denom = tb.denom;
    header->pid = getpid(); header->finish = finishMode; header->cap = CAP;
    if (getenv("GLFT_METAL") && getenv("GLFT_METAL")[0] == '1') {
        Method n = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
        origNext = (void *)method_getImplementation(n); method_setImplementation(n, (IMP)hookedNext);
        fprintf(stderr, "glft: recording CAMetalLayer nextDrawable calls to %s\n", out); return;
    }
    Method m = class_getInstanceMethod([NSOpenGLContext class], @selector(flushBuffer));
    origFlush = (void *)method_getImplementation(m); method_setImplementation(m, (IMP)hookedFlush);
    fprintf(stderr, "glft: recording presents to %s (finish=%d)\n", out, finishMode);
}
