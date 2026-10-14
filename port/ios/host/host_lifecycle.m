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

static _Atomic int backgrounded;

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
            host_sdl_audio_pause(1);
            host_logf(HOST_LOG_INFO,"in the background: audio paused, the game holds at its next frame");
        }];
    [NSNotificationCenter.defaultCenter addObserverForName:UISceneWillEnterForegroundNotification object:nil
        queue:nil usingBlock:^(NSNotification *notification){
            if(!host_scene_is_games((__bridge void *)notification.object))return;
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

void host_lifecycle_hold(void) {
    while(atomic_load(&backgrounded)) {
        @autoreleasepool {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.1,true);
        }
    }
}
