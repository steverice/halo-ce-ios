/* Start the statically compiled guest on the iOS UI thread and persist its log. */
#import <Foundation/Foundation.h>
#if HALO_NATIVE_MAC
#import <AppKit/AppKit.h>
#include "host_macos.h"
#else
#import <UIKit/UIKit.h>
#endif
#import <Metal/Metal.h>
#include "ios_host.h"
#include "guest_image.h"
#include "host_display_pin.h"
#include "host_output_cap.h"
#include "host_config.h"
#include "host_profile.h"
#include "host_join_link.h"
#include "host_benchmark.h"
#include "host_external.h"
#include "host_cloud_saves.h"
#include "host_shader_upload.h"
#include "host_perf_reports.h"
#include "host_perf_hooks.h"
#include "renderer_build.h"
#if !TARGET_OS_TV && !HALO_NATIVE_MAC
#include "host_texture_settings.h"
#endif
#include "host_astc_store.h"
#include "../../linux/include/halo_port_limits.h" /* HALO_PORT_NETWORK_VERSION, for the log */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#if TARGET_OS_MACCATALYST
#include <objc/message.h>
#endif
#include <mach/mach.h>
#include <os/proc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static char data_root[1024],save_root[1024];
static FILE *log_file;
void host_logf(int priority,const char *format,...) {
    (void)priority;va_list ap;va_start(ap,format);
    char text[4096];vsnprintf(text,sizeof(text),format,ap);va_end(ap);
    fprintf(stderr,"halo-ios: %s\n",text);
    if(log_file){flockfile(log_file);fprintf(log_file,"%s\n",text);fflush(log_file);funlockfile(log_file);}
}
void host_log(int priority,const char *text) { host_logf(priority,"%s",text); }
#if HALO_RENDER_VALIDATION
/* host_render_replay.c */
int host_render_replay_requested(void);
void host_render_replay(void) __attribute__((noreturn));
#endif
/* host_memory_pressure.m */
void host_memory_pressure_start(void);
void host_fatal(const char *format,...) {
    va_list ap;va_start(ap,format);char text[1024];vsnprintf(text,sizeof(text),format,ap);va_end(ap);
    host_logf(HOST_LOG_ERROR,"FATAL: %s",text);
    if(!getenv("HALO_RUNNER"))SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"VISR",text,NULL);exit(1);
}
void host_abort(const char *reason) { host_logf(HOST_LOG_ERROR,"guest abort: %s",reason);abort(); }
void host_exit(int code) {
    host_logf(HOST_LOG_INFO,"game exit %d",code);
    host_spatial_rumble_hold(1);
    /* a save made just before the app is left still goes up (nothing without HALO_CLOUD_SAVES) */
    host_cloud_saves_leaving(3);
    host_shader_upload_suspend();
    host_perf_reports_finish(code != 0);
    exit(code);
}

/* What the system counts against the app, every ten seconds: tvOS ends an app
   well under the device's memory (2 GB on an Apple TV HD), and the log is the
   only place to see how close a level comes. Always on tvOS; elsewhere when
   HALO_MEMORY_LOG is set in the app's environment or the data folder has a
   file named memory-log (as tools make stderr.log). */
static void memory_log_start(void) {
    static dispatch_source_t timer;
    host_logf(HOST_LOG_INFO,"memory: the device has %llu MB",NSProcessInfo.processInfo.physicalMemory>>20);
    timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_global_queue(QOS_CLASS_UTILITY,0));
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,0),10*NSEC_PER_SEC,NSEC_PER_SEC);
    dispatch_source_set_event_handler(timer,^{
        task_vm_info_data_t info;mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
        if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&info,&count)!=KERN_SUCCESS)return;
#if TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
        host_logf(HOST_LOG_INFO,"memory: footprint %llu MB, peak %llu MB",
            (unsigned long long)info.phys_footprint>>20,(unsigned long long)info.ledger_phys_footprint_peak>>20);
#else
        host_logf(HOST_LOG_INFO,"memory: footprint %llu MB, peak %llu MB, %llu MB left before the system's limit",
            (unsigned long long)info.phys_footprint>>20,(unsigned long long)info.ledger_phys_footprint_peak>>20,
            (unsigned long long)os_proc_available_memory()>>20);
#endif
    });
    dispatch_resume(timer);
}
int host_errno(void) {return host_linux_errno(errno);}
void host_debug_thread_started(void) {}
void host_debug_thread_exited(void) {}
void host_debug_start_sampler(const char *setting) {(void)setting;}

#if TARGET_OS_MACCATALYST
/* AppKit hands a Mac app its Apple Events (among them GetURL: a halo:// link
   opened while the app runs) only when NSApplication looks at its event queue,
   and nothing here ever does: the game, SDL and the import screen pump the run
   loop with CFRunLoopRunInMode, so such a link was never opened. Looking with an empty mask dispatches the waiting Apple Events and
   takes nothing else off the queue. (A link the app is launched with comes
   another way, through the scene's connection options.) */
static void apple_events_timer(CFRunLoopTimerRef timer,void *info) {
    (void)timer;(void)info;
    static id application,distant_past;
    if(!application){
        application=((id(*)(id,SEL))objc_msgSend)((id)NSClassFromString(@"NSApplication"),sel_registerName("sharedApplication"));
        distant_past=NSDate.distantPast;
    }
    if(application)((id(*)(id,SEL,unsigned long long,id,id,BOOL))objc_msgSend)(application,
        sel_registerName("nextEventMatchingMask:untilDate:inMode:dequeue:"),0,distant_past,NSDefaultRunLoopMode,NO);
}
static void install_apple_events_timer(void) {
    CFRunLoopTimerRef timer=CFRunLoopTimerCreate(NULL,CFAbsoluteTimeGetCurrent()+0.25,0.25,0,0,apple_events_timer,NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(),timer,kCFRunLoopCommonModes);
    CFRelease(timer);
}
#endif

static uint32_t copy_string(const char *text) {
    char *p=host_low_map(strlen(text)+1,PROT_READ|PROT_WRITE);
    if(!p)host_fatal("out of guest memory");strcpy(p,text);return guest_pointer(p);
}
/* SDL's scene delegate (SDL_uikitappdelegate.m) calls main for every window
scene that connects: a second one (visionOS can connect or restore another
window scene of the app) fires its start timer whenever the running guest
pumps the run loop (SDL_PollEvent), and would start a second guest inside the
first, nested on the main thread, until the system kills the hung app. Only
the first call runs the game. A later one returns at once, and its
postFinishLaunch then turns SDL's iOS event pump off (SDL_SetiOSEventPump),
which is global: the running game would never run the main run loop again,
so no UIKit event, lifecycle notification or main-queue block would reach it.
host_extra_scene_poll, from the game's event poll, turns the pump back on and
closes the extra scene. */
#if !HALO_NATIVE_MAC
static int extra_scene_pending;

/* the scene holding the game's window, or nil before it exists */
static UIWindowScene *game_window_scene(void) {
    SDL_Window *game_window=host_sdl_window();
    UIWindow *window=game_window ? (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(game_window),
        SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,NULL) : nil;
    return window.windowScene;
}

/* whether a disconnecting scene is the game's: yes before the game's window
exists, when any of SDL's window scenes could be it (as before) */
static BOOL scene_is_the_games(UIScene *scene) {
    UIWindowScene *game_scene=game_window_scene();
    if(game_scene)return scene==game_scene;
    return [scene isKindOfClass:UIWindowScene.class] &&
        [NSStringFromClass([(NSObject *)scene.delegate class]) isEqualToString:@"SDLUIKitSceneDelegate"];
}

int host_scene_is_games(void *scene) {
    return scene_is_the_games((__bridge UIScene *)scene);
}

/* closes SDL's window scenes other than the one holding the game's window
(SDL's plain UIWindow, from the SDL window's properties); NO until that
window exists, since before then the game's scene could be any of them */
static BOOL close_extra_window_scenes(void) {
#if !TARGET_OS_TV
    UIWindowScene *game_scene=game_window_scene();
    if(!game_scene)return NO;
    for(UIScene *scene in UIApplication.sharedApplication.connectedScenes){
        /* SDL's window scenes only, never theater mode's immersive space */
        if(scene==game_scene || ![scene isKindOfClass:UIWindowScene.class] ||
            ![NSStringFromClass([(NSObject *)scene.delegate class]) isEqualToString:@"SDLUIKitSceneDelegate"])continue;
        host_logf(HOST_LOG_INFO,"closing an extra window scene (%s)",scene.session.role.UTF8String);
        [UIApplication.sharedApplication requestSceneSessionDestruction:scene.session options:nil
            errorHandler:^(NSError *error){host_logf(HOST_LOG_WARN,"the extra window scene didn't close: %s",
                error.description.UTF8String);}];
    }
#endif
    return YES;
}

/* the game's event poll (host_sdl.c), on the main thread outside any SDL
call: after a later main call, the event pump comes back on, and the extra
scene closes once the game's window exists */
/* polls left to watch the game's window after an extra scene closed */
static int extra_scene_watch;

void host_extra_scene_poll(void) {
    /* SDL's delegate for the closed scene tells the whole app it went to the
       background (SDL_uikitappdelegate.m: SDL_OnApplicationDidEnterBackground),
       which minimizes the game's window though its own scene stays active, and
       no restore follows: restore it */
    if(extra_scene_watch>0){
        SDL_Window *window=host_sdl_window();
        extra_scene_watch--;
        if(window&&(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED)){
            /* SDL's UIKit driver has no RestoreWindow (SDL_RestoreWindow does
               nothing there); its foreground notifications give the window
               back its focus and send it RESTORED, which clears the flag and
               enters fullscreen again */
            SDL_OnApplicationWillEnterForeground();
            SDL_OnApplicationDidEnterForeground();
            if(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED)
                host_logf(HOST_LOG_WARN,"the game's window is still minimized after the extra scene's close");
            else
                host_logf(HOST_LOG_INFO,"the game's window is restored after the extra scene's close minimized it");
            extra_scene_watch=0;
        }
    }
    if(!extra_scene_pending)return;
    if(extra_scene_pending==1){
        char directory[1024];
        SDL_SetiOSEventPump(true);
        host_logf(HOST_LOG_INFO,"the event pump is back on after the extra window scene's start");
        /* SDL's scene delegate also changed the working directory to the app's
           resources for the new scene; the host's relative paths (config.toml,
           host_config.c) are the data folder's */
        if(getcwd(directory,sizeof(directory))&&strcmp(directory,data_root)){
            if(chdir(data_root)==0)
                host_logf(HOST_LOG_INFO,"the working directory is the data folder again (the extra scene set %s)",directory);
            else
                host_logf(HOST_LOG_WARN,"the working directory can't go back to the data folder (errno %d); it's %s",
                    errno,directory);
        }
        /* a block on the main queue runs only when the main run loop does */
        dispatch_async(dispatch_get_main_queue(),^{host_logf(HOST_LOG_INFO,"the main queue runs again");});
        extra_scene_pending=2;
    }
    if(close_extra_window_scenes()){
        extra_scene_pending=0;
        extra_scene_watch=600;
    }
}

/* debug.test_extra_scene (the simulator): asks for a second window scene of
the app once the game runs, to drive the path above */
void host_extra_scene_test(void) {
#if !TARGET_OS_TV
    host_logf(HOST_LOG_INFO,"debug.test_extra_scene: asking for a second window scene");
    [UIApplication.sharedApplication requestSceneSessionActivation:nil userActivity:nil options:nil
        errorHandler:^(NSError *error){host_logf(HOST_LOG_WARN,"debug.test_extra_scene: %s",error.description.UTF8String);}];
#endif
}

#endif /* UIKit scene recovery */

int main(int argc,char **argv) {
    (void)argc;(void)argv;
    #if !HALO_NATIVE_MAC
    static int entered;
    if(entered++){
        host_logf(HOST_LOG_WARN,"another window scene connected and asked to start the game while it runs (start %d); ignored",entered);
        extra_scene_pending=1;
        return 0;
    }
    #endif
    @autoreleasepool {
#if HALO_NATIVE_MAC
        host_macos_install();
#endif
        /* Shadow measurements need resume events even when report uploads are off. */
#if HALO_NATIVE_MAC
        NSString *resume_notification=NSApplicationDidBecomeActiveNotification;
#else
        NSString *resume_notification=UIApplicationDidBecomeActiveNotification;
#endif
        [NSNotificationCenter.defaultCenter addObserverForName:resume_notification object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;host_perf_hooks_dynamic_resume();}];
        host_external_install();
#if TARGET_OS_TV
        /* tvOS apps may only write to Caches (purgeable). */
        NSString *documents=[NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES).firstObject stringByAppendingPathComponent:@"Halo"];
        [NSFileManager.defaultManager createDirectoryAtPath:documents withIntermediateDirectories:YES attributes:nil error:nil];
#elif HALO_NATIVE_MAC
        NSString *documents=@(host_macos_data_root());
#elif TARGET_OS_MACCATALYST
        /* Not sandboxed, so NSDocumentDirectory would be the user's own ~/Documents:
           HALO_DATA_ROOT (a runner's data folder), else Application Support/<bundle ID>. */
        NSString *documents=NSProcessInfo.processInfo.environment[@"HALO_DATA_ROOT"];
        if(!documents.length)documents=[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,NSUserDomainMask,YES).firstObject
            stringByAppendingPathComponent:NSBundle.mainBundle.bundleIdentifier];
        [NSFileManager.defaultManager createDirectoryAtPath:documents withIntermediateDirectories:YES attributes:nil error:nil];
#else
        NSString *documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
#endif
#if HALO_NATIVE_MAC
        if (strlen(documents.fileSystemRepresentation) + sizeof("/save") >= sizeof(data_root))
            host_fatal("The game data folder path is too long");
#endif
        snprintf(data_root,sizeof(data_root),"%s",documents.fileSystemRepresentation);
        snprintf(save_root,sizeof(save_root),"%s/save",data_root);mkdir(save_root,0755);
        /* a benchmark suite's saves and profile are scratch, so a run leaves the player's untouched */
        if(getenv("HALO_BENCHMARK_SUITE")&&*getenv("HALO_BENCHMARK_SUITE")){
            char run[1100];
            snprintf(run,sizeof(run),"%s/benchmark",data_root);mkdir(run,0755);
            snprintf(run,sizeof(run),"%s/benchmark/run",data_root);mkdir(run,0755);
            snprintf(save_root,sizeof(save_root),"%s/benchmark/run/save",data_root);mkdir(save_root,0755);
        }
        chdir(data_root);
        log_file=fopen("ios-runtime.log","w");setvbuf(stderr,NULL,_IONBF,0);
        /* internet play's MQTT brokers (network.brokers_file, p2p_signal.c): the app's
           list, written beside config.toml at each start, as upstream's Android app does */
        {
            NSString *brokers=[NSBundle.mainBundle pathForResource:@"brokers" ofType:@"txt"];
            NSData *list=brokers?[NSData dataWithContentsOfFile:brokers]:nil;
            if(!list||![list writeToFile:@"brokers.txt" atomically:YES])
                host_logf(HOST_LOG_ERROR,"cannot write brokers.txt from the app bundle");
        }
        /* Tools (tools/mac_run.py) create stderr.log to keep the guest's own log, platform_log
           and debug.gpu_stats among it, which otherwise only a debugger's console shows; so does
           opening a join link (host_join_link.c). Past 4 MB it starts anew. */
        if(access("stderr.log",F_OK)==0){struct stat st;int big=stat("stderr.log",&st)==0&&st.st_size>(4L<<20);
            int fd=open("stderr.log",O_WRONLY|O_APPEND|(big?O_TRUNC:0));if(fd>=0){dup2(fd,2);close(fd);}}
        host_logf(HOST_LOG_INFO,"Halo iOS native guest starting");
        /* names the exact game image a result folder came from (tools/mac_run.py compare-inputs) */
        host_logf(HOST_LOG_INFO,"guest image sha256 %s",HALO_GUEST_SHA256);
#if TARGET_OS_TV
        memory_log_start();
#else
        if(getenv("HALO_MEMORY_LOG")||access("memory-log",F_OK)==0)memory_log_start();
#endif
        host_memory_pressure_start();
        host_shader_upload_start(HALO_GUEST_SHA256);
        host_perf_reports_start(HALO_GUEST_SHA256);
        /* halo://join links (host_join_link.c): before the import screen can spin the run loop,
           which is when SDL's scene delegate passes on the link the app was launched with */
        host_join_link_install(data_root);
#if TARGET_OS_MACCATALYST
        install_apple_events_timer();
#endif
#if TARGET_OS_VISION
        /* Closing the window is how a visionOS app is left, and an app reopened
           into a new scene would have no game window: quit (the game saves at
           checkpoints). Registered before the import setup screen, whose wait
           loop is when a player is most likely to close the window; delivered
           while that loop or the guest pumps the run loop. */
        [NSNotificationCenter.defaultCenter addObserverForName:UISceneDidDisconnectNotification object:nil queue:nil
            usingBlock:^(NSNotification *notification){
                /* an extra window scene closing (close_extra_window_scenes), or
                theater mode's space, isn't the game's window closing */
                if(!scene_is_the_games(notification.object)){
                    host_logf(HOST_LOG_INFO,"a scene that isn't the game's window disconnected");
                    return;
                }
                host_exit(0);}];
#endif
#if !HALO_NATIVE_MAC
        UIApplication.sharedApplication.idleTimerDisabled=YES;
#endif
        /* iCloud saves (nothing without HALO_CLOUD_SAVES): begun before the import screen, whose
           time on a new or purged device is also the time the saves take to come back */
        host_cloud_saves_start(data_root,save_root);
#if TARGET_OS_MACCATALYST
        /* a runner's run (tools/mac_run.py sets HALO_RUNNER) must not slow down when its window is covered
           or minimized: App Nap would throttle its timers and drawable presentation */
        static id runner_activity;
        if(getenv("HALO_RUNNER")){
            runner_activity=[NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated|NSActivityLatencyCritical
                reason:@"a test run"];
            host_logf(HOST_LOG_INFO,"runner: App Nap held off");
        }
#endif
        /* the arena is the step a device can refuse (visionOS: see port/ios/README.md); reserved before the import
           screen so a refusal shows at once, not after a multi-GB import */
#if HALO_RENDER_VALIDATION
        if(!host_render_replay_requested())
#endif
#if TARGET_OS_TV
        /* (no Files app on an Apple TV; and a message box would hold a test run open) */
        if(host_load_image(NULL,0))host_fatal(host_arena?"Could not map the signed game image.":
            "This Apple TV cannot give the game the 2.2 GB of address space it needs.");
#elif HALO_NATIVE_MAC || TARGET_OS_MACCATALYST
        if(host_load_image(NULL,0))host_fatal("%s See ios-runtime.log in %s.",
            host_arena ? "Could not map the signed game image." : "Could not reserve the game's memory arena (about 2.2 GB of address space).", data_root);
#else
        if(host_load_image(NULL,0))host_fatal(host_arena?"Could not map the signed game image. See ios-runtime.log in Files.":
            "Could not reserve the game's memory arena (about 2.2 GB of address space). See ios-runtime.log in Files.");
#endif
#if HALO_RENDER_VALIDATION
        /* a capture replayed with no game (host_render_replay.c): no game data, no game image */
        if(!host_render_replay_requested())
#endif
        host_ios_prepare_assets(data_root);
        host_astc_store_init(data_root);
        /* the device benchmark: debug.benchmark_suite's value through the launch's environment, or in a tester
           build the choice made on its first screen */
        const char *benchmark_suite=getenv("HALO_BENCHMARK_SUITE");
#ifdef HALO_TESTER_BUILD
        if(!benchmark_suite&&!getenv("HALO_RUNNER"))benchmark_suite=host_benchmark_menu();
#endif
#if !TARGET_OS_TV && !HALO_NATIVE_MAC
        /* the Settings app's texture pane: acted on now and at every return to the foreground */
        host_texture_settings_observe(data_root);
#endif
        /* the device benchmark's crash marker goes under the memory watch's handlers, which pass on what is not theirs */
        host_benchmark_install();
        host_install_signal_handlers();
        SDL_SetHint(SDL_HINT_ORIENTATIONS,"LandscapeLeft LandscapeRight");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"0");
        /* Keep the Siri Remote out of the player ports; Halo needs a real controller. */
        SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK,"0");
#if !HALO_NATIVE_MAC
        if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMEPAD))host_fatal("SDL initialization: %s",SDL_GetError());
#endif
        host_ios_touch_initialize();
        const SDL_DisplayMode *mode=SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        int width=640,pixel_width=640,pixel_height=480;
        if(mode && mode->w && mode->h){
            int longer=mode->w>mode->h?mode->w:mode->h,shorter=mode->w>mode->h?mode->h:mode->w;
            float density=mode->pixel_density>0?mode->pixel_density:1;
            width=(480*longer/shorter)&~1;
            pixel_width=(int)(longer*density+0.5f);pixel_height=(int)(shorter*density+0.5f);
        }
        /* The runner's pinned display (HALO_HOST_DISPLAY=1366x1024@2, what the iPad runner's SDL
           reports): the guest's display and drawable size, whatever the window's (host_sdl.c). */
        const char *pin_text=getenv("HALO_HOST_DISPLAY");
        struct host_display_pin pin;
        int pinned=host_display_pin_parse(pin_text,&pin);
        if(pinned<0)host_fatal("HALO_HOST_DISPLAY=%s is not WIDTHxHEIGHT@SCALE (the runner pins 1366x1024@2)",pin_text);
        if(pinned){
            width=pin.screen_width;pixel_width=pin.pixel_width;pixel_height=pin.pixel_height;
            host_sdl_pin_window_pixels(pixel_width,pixel_height);
            host_logf(HOST_LOG_INFO,"display pinned to %s: %dx%d pixels, width %d",pin_text,pixel_width,pixel_height,width);
        }
        /* The built-in output cap (host_output_cap.h): the game draws into a drawable at most that many lines tall
           and the system scales it to the panel, whose mode is left alone. On an Apple TV a tvOS profile's
           display.output_height sets it, so the profile resolves here, before the guest is told its display; on an
           iPhone or iPad only debug.output_height (HALO_OUTPUT_HEIGHT) does, for measuring. Either of those
           overrides the profile. Metal only: the OpenGL ES view keeps the screen's own size. Every other device
           resolves the profile below, as before. */
        const char *profile_values=NULL;
#if TARGET_OS_TV || (TARGET_OS_IOS && !TARGET_OS_MACCATALYST)
#if TARGET_OS_TV
        profile_values=host_device_profile_install(data_root);
#endif
        if(!pinned){
            char cap_text[32]="",override_text[32]="",renderer[16]="";
#if TARGET_OS_TV
            struct host_profile profile;
            host_device_profile_copy(&profile);
            host_profile_value(&profile,"display.output_height",cap_text,sizeof(cap_text));
#endif
            const char *launch_cap=getenv("HALO_OUTPUT_HEIGHT");
            if(launch_cap&&*launch_cap)snprintf(override_text,sizeof(override_text),"%s",launch_cap);
            else host_config_string("debug.output_height","",override_text,sizeof(override_text));
            int overridden=host_output_cap_height(override_text,NULL)>0;
            int cap=host_output_cap_height(override_text,cap_text);
            const char *launch_renderer=getenv("HALO_RENDERER");
            if(launch_renderer&&*launch_renderer)snprintf(renderer,sizeof(renderer),"%s",launch_renderer);
            else host_config_string("display.renderer","metal",renderer,sizeof(renderer));
            if(HALO_METAL_ONLY)snprintf(renderer,sizeof(renderer),"metal");
            int capped_width,capped_height;
            host_sdl_output_native(pixel_width,pixel_height,strcmp(renderer,"metal")?0:cap);
            if(cap&&strcmp(renderer,"metal"))
                host_logf(HOST_LOG_INFO,"output: the %d-line cap is for Metal; %s keeps %dx%d",cap,renderer,pixel_width,pixel_height);
            else if(host_output_cap_apply(pixel_width,pixel_height,cap,&capped_width,&capped_height)){
                host_logf(HOST_LOG_INFO,"output: %dx%d capped to %dx%d (%s)",pixel_width,pixel_height,capped_width,capped_height,
                    overridden?"debug.output_height":"display.output_height");
                pixel_width=capped_width;pixel_height=capped_height;
                host_sdl_cap_output(pixel_width,pixel_height);
            }
            else if(cap)
                host_logf(HOST_LOG_INFO,"output: the %d-line cap leaves %dx%d as it is",cap,pixel_width,pixel_height);
        }
#endif
#if HALO_RENDER_VALIDATION
        if(host_render_replay_requested())host_render_replay();
#endif
        /* what the guest is told, under either runner (tools/mac_run.py compare-inputs) */
        host_logf(HOST_LOG_INFO,"guest display: width %d, pixels %dx%d",width,pixel_width,pixel_height);
        char env_data[1200],env_save[1200],env_width[64],env_pixel_width[64],env_pixel_height[64];
        snprintf(env_data,sizeof(env_data),"HALO_DATA_ROOT=%s",data_root);
        snprintf(env_save,sizeof(env_save),"HALO_SAVE_ROOT=%s",save_root);
        snprintf(env_width,sizeof(env_width),"HALO_DISPLAY_WIDTH=%d",width);
        snprintf(env_pixel_width,sizeof(env_pixel_width),"HALO_DISPLAY_PIXEL_WIDTH=%d",pixel_width);
        snprintf(env_pixel_height,sizeof(env_pixel_height),"HALO_DISPLAY_PIXEL_HEIGHT=%d",pixel_height);
        const char *env[256];size_t env_count=0;
        env[env_count++]=env_data;env[env_count++]=env_save;env[env_count++]=env_width;
        env[env_count++]=env_pixel_width;env[env_count++]=env_pixel_height;
        /* This device's profile (profiles/, host_profile.h): its values go to the game as one variable, which
           port_config.c lays under config.toml. Not as the settings' own HALO_ variables, which win over the
           file: a profile never outranks what the player chose. */
        static char env_profile[sizeof("HALO_DEVICE_PROFILE_VALUES=")+HOST_PROFILE_MAXIMUM_VALUES*216];
        snprintf(env_profile,sizeof(env_profile),"HALO_DEVICE_PROFILE_VALUES=%s",profile_values?profile_values:host_device_profile_install(data_root));
        env[env_count++]=env_profile;
        /* the build, for what the game writes beside a recording (input_replay.c), and the folder its automatic
           takes go to */
        static char env_build[96];
        snprintf(env_build,sizeof(env_build),"HALO_APP_BUILD=%s",
            [[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] description].UTF8String?:"");
        env[env_count++]=env_build;
#if HALO_CLOUD_SAVES
        /* The shared guest only offers cloud controls when this host can sync saves. */
        env[env_count++]="HALO_HOST_CLOUD_SAVES=1";
#else
        env[env_count++]="HALO_HOST_CLOUD_SAVES=0";
#endif
        mkdir("recordings",0755);
#if TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
        /* the Mac app can end itself, so its menus keep Quit (menu_files.c's platform="quit"); on an iPhone, an
           iPad, an Apple TV and a Vision Pro the system closes the app and the menus have none */
        env[env_count++]="HALO_APP_CAN_QUIT=1";
        /* The Mac app alone honors display.frame_rate, so its menus alone show
           the Frame Rate row (platform="mac_app", port/linux/src/menu_platform.h),
           with the display's own highest rate among its choices
           (port/linux/game/menu_tags.c). */
        static char env_max_fps[48];
#if HALO_NATIVE_MAC
        snprintf(env_max_fps,sizeof(env_max_fps),"HALO_DISPLAY_MAX_FPS=%.0f",host_macos_refresh_rate());
#else
        snprintf(env_max_fps,sizeof(env_max_fps),"HALO_DISPLAY_MAX_FPS=%ld",(long)UIScreen.mainScreen.maximumFramesPerSecond);
#endif
        env[env_count++]="HALO_HOST_MAC=1";env[env_count++]=env_max_fps;
#endif
#if TARGET_OS_VISION
        /* The aspect is pinned to 16:9, since a visionOS window's size is only a request and the game takes its aspect
           from the first drawable it sees. */
        env[env_count++]="HALO_SCREEN_WIDTH=852";
#endif
        /* Choose the guest renderer before its environment snapshot and window creation. A Metal-only build overrides
           the player's config and any launch-time OpenGL request. */
        static char env_renderer[32];
        const char *launch_renderer=getenv("HALO_RENDERER");
        const char *guest_renderer=host_renderer_guest_value(TARGET_OS_VISION,TARGET_OS_TV,launch_renderer);
        if(guest_renderer){
            snprintf(env_renderer,sizeof(env_renderer),"HALO_RENDERER=%s",guest_renderer);
            env[env_count++]=env_renderer;
#if HALO_METAL_ONLY
            host_logf(HOST_LOG_INFO,"renderer: Metal-only build forces Metal for the game window and GPU backend");
#elif TARGET_OS_TV
            host_logf(HOST_LOG_INFO,"tvOS: %s from the launch's environment",env_renderer);
#endif
        }
#if TARGET_OS_TV || TARGET_OS_VISION
        char env_render[64];
        /* A build made with tools/ios_build.py --render-height N draws at N whatever the device's profile says
           (0, the default: the profile's display.render_height and display.render_scale). */
        int render_height=[[NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloRenderHeight"] intValue];
#if TARGET_OS_TV
        /* (the Apple TV HD's 540 is its profile's: profiles/apple-tv-hd.toml) */
        /* a launch's own HALO_RENDER_HEIGHT, to measure another height without a rebuild */
        if(getenv("HALO_RENDER_HEIGHT")&&atoi(getenv("HALO_RENDER_HEIGHT"))>0)render_height=atoi(getenv("HALO_RENDER_HEIGHT"));
#endif
        if(render_height>0){snprintf(env_render,sizeof(env_render),"HALO_RENDER_HEIGHT=%d",render_height);env[env_count++]=env_render;}
#endif
        char env_benchmark[320];
        if(benchmark_suite&&*benchmark_suite){
            snprintf(env_benchmark,sizeof(env_benchmark),"HALO_BENCHMARK_SUITE=%s",benchmark_suite);
            env[env_count++]=env_benchmark;
        }
        /* The game gets only the variables named here, not the app's own environment, so a launch's
           HALO_ANTI_ALIASING (devicectl's --environment-variables) would otherwise never reach
           display.anti_aliasing: it is passed on, for comparing FXAA with a launch that lacks it. */
        char env_anti_aliasing[48];
        const char *launch_anti_aliasing=getenv("HALO_ANTI_ALIASING");
        if(launch_anti_aliasing&&*launch_anti_aliasing){
            snprintf(env_anti_aliasing,sizeof(env_anti_aliasing),"HALO_ANTI_ALIASING=%s",launch_anti_aliasing);
            env[env_count++]=env_anti_aliasing;
        }
        static char env_external[sizeof("HALO_EXTERNAL_MODE=")+512];
        const char *external_mode=getenv("HALO_EXTERNAL_MODE");
        if(external_mode && *external_mode) {
            snprintf(env_external,sizeof(env_external),"HALO_EXTERNAL_MODE=%s",external_mode);
            env[env_count++]=env_external;
        }
        /* a benchmark suite takes every setting from its launch's environment: each HALO_ variable the host
           has not already set is passed on, so the run needs no change to config.toml */
        if(benchmark_suite&&*benchmark_suite){
            extern char **environ;
            for(char **launch=environ;launch&&*launch&&env_count<250;launch++){
                if(strncmp(*launch,"HALO_",5))continue;
                const char *equals=strchr(*launch,'=');
                if(!equals)continue;
                size_t name_length=(size_t)(equals-*launch);
                int known=0;
                for(size_t i=0;i<env_count;i++)
                    if(!strncmp(env[i],*launch,name_length)&&env[i][name_length]=='=')known=1;
                if(!known)env[env_count++]=*launch;
            }
        }
        env[env_count++]="TZ=UTC0";env[env_count++]=NULL;
        uint32_t *environment=host_low_map(sizeof(uint32_t)*env_count,PROT_READ|PROT_WRITE);
        for(size_t i=0;i<env_count-1;i++)environment[i]=copy_string(env[i]);environment[env_count-1]=0;
        uint32_t *arguments=host_low_map(8,PROT_READ|PROT_WRITE);arguments[0]=copy_string("halo");arguments[1]=0;
        struct halo_guest_boot *boot=host_low_map(sizeof(*boot),PROT_READ|PROT_WRITE);
        *boot=(struct halo_guest_boot){1,guest_pointer(arguments),guest_pointer(environment),0x4000};
        size_t stack_size=16*1024*1024;
        void *stack=host_low_map(stack_size+0x4000,PROT_READ|PROT_WRITE);
        if(!stack)host_fatal("could not allocate game stack");mprotect(stack,0x4000,PROT_NONE);
        /* (machines of different network versions cannot play together: the log of each says which it is) */
        host_logf(HOST_LOG_INFO,"network version %d",HALO_PORT_NETWORK_VERSION);
        /* the saves are compared with iCloud's before the game reads them, and not replaced after */
        host_cloud_saves_before_game();
        host_logf(HOST_LOG_INFO,"entering guest at %08x, data %s",host_image.header->start,data_root);
        host_shader_upload_prepare();
        host_guest_on_stack((uintptr_t)host_pointer(host_image.header->start),guest_pointer(boot),host_arena,(char *)stack+stack_size+0x4000);
    }
    return 0;
}
