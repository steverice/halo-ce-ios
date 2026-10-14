/* The app in the background: iOS ends an app that submits GPU work there, and
on visionOS closing the window only backgrounds the app, with the game still
playing. So while the scene is in the background the game's audio is paused
and the game holds at its next Present (host_gpu_dispatch.c), pumping the run
loop until the scene returns; the game resumes where it was. Not on a Mac. */
#import <UIKit/UIKit.h>
#include "ios_host.h"
#include "host_socket_revival.h"
#include "host_config.h"
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>

static _Atomic int backgrounded;
/* the time spent in the background: finished stays (ms), and when the one
under way began (CLOCK_MONOTONIC ms, 0 for none), for the game's clock, which
stands still there (posix_background_milliseconds, xbox_kernel.c) */
static unsigned long long background_total, background_began;
static pthread_mutex_t background_lock=PTHREAD_MUTEX_INITIALIZER;

static unsigned long long monotonic_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (unsigned long long)now.tv_sec*1000ULL+(unsigned long long)now.tv_nsec/1000000ULL;
}

unsigned long long host_lifecycle_background_milliseconds(void) {
    unsigned long long total;
    pthread_mutex_lock(&background_lock);
    total=background_total+(background_began?monotonic_milliseconds()-background_began:0);
    pthread_mutex_unlock(&background_lock);
    return total;
}

/* host_main.m: whether a scene is the game's window's (any of SDL's window
scenes before the window exists). Another scene going to the background (an
extra window scene being closed) leaves the game playing */
int host_scene_is_games(void *scene);

void host_lifecycle_install(void) {
    /* an iPad app on a Mac (tools/mac_run.py) may draw in the background, and
       a test run must not stop because its window went behind another */
    if(NSProcessInfo.processInfo.isiOSAppOnMac || NSProcessInfo.processInfo.isMacCatalystApp)return;
    [NSNotificationCenter.defaultCenter addObserverForName:UISceneDidEnterBackgroundNotification object:nil
        queue:nil usingBlock:^(NSNotification *notification){
            if(!host_scene_is_games((__bridge void *)notification.object))return;
            atomic_store(&backgrounded,1);
            pthread_mutex_lock(&background_lock);
            if(!background_began)background_began=monotonic_milliseconds();
            pthread_mutex_unlock(&background_lock);
            host_sdl_audio_pause(1);
            host_logf(HOST_LOG_INFO,"in the background: audio paused, the game holds at its next frame");
        }];
    [NSNotificationCenter.defaultCenter addObserverForName:UISceneWillEnterForegroundNotification object:nil
        queue:nil usingBlock:^(NSNotification *notification){
            if(!host_scene_is_games((__bridge void *)notification.object))return;
            pthread_mutex_lock(&background_lock);
            if(background_began){
                background_total+=monotonic_milliseconds()-background_began;
                background_began=0;
            }
            pthread_mutex_unlock(&background_lock);
            atomic_store(&backgrounded,0);
            host_sdl_audio_pause(0);
            host_logf(HOST_LOG_INFO,"in the foreground: the game resumes");
            /* (the sockets the system took back while the app was suspended,
               made again: host_socket_revival.c; with debug.test_reclaim_sockets,
               all of them, as if it had, since a simulator's app keeps its sockets) */
            char test[8];
            host_config_string("debug.test_reclaim_sockets","false",test,sizeof(test));
            host_socket_revive(strcmp(test,"true") ? 0 : 2);
        }];
}

int host_lifecycle_in_background(void) {
    return atomic_load(&backgrounded);
}

void host_lifecycle_hold(void) {
    while(atomic_load(&backgrounded)) {
        @autoreleasepool {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.1,true);
        }
    }
}
