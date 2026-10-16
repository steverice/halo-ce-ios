#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AVFAudio/AVFAudio.h>
#import <objc/runtime.h>
#include <SDL3/SDL.h>
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <pthread.h>
#include <math.h>
#include "ios_host.h"
#include "host_external.h"
#include "host_external_mode.h"
#include "host_config.h"
#include "host_profile.h"

#if !TARGET_OS_VISION
static struct host_external_snapshot snapshot;
static pthread_mutex_t snapshot_lock = PTHREAD_MUTEX_INITIALIZER;
static UIWindow *phone_window;
static int metal_enabled, companion_on;
static _Atomic int audio_changed;
static char mode_list[512];
static int forced_width, forced_height, forced_index;

static CFTimeInterval last_touch;
static UIScreen *output_screen;
static int output_width, output_height;
static void *phone_layer;
#if TARGET_OS_MACCATALYST
/* Runner-only synthetic output shapes; the actual app window stays put. */
static unsigned long test_output_frame;
#endif

#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
static UIWindow *external_window;
static UIWindowScene *waiting_scene;
static UIScreen *candidate_screen;
static NSArray<UIScreenMode *> *offered;
static struct host_external_mode candidates[64];
static int candidate_count, preferred_index, requested_index = -1, negotiating, pending;
static CFTimeInterval deadline, accessory_available_since;
static int first_low=-1, accepting_low;
static NSString *chosen_key;
static NSMutableDictionary<NSString *, NSValue *> *chosen_modes;
static NSMutableDictionary<NSString *, NSMutableIndexSet *> *refused_modes;
static id accessory_registration;
static double audio_route_rate;
static UILabel *playing_label;
static IMP original_configuration[2];
static Class configuration_class[2];

@interface VISRExternalView : UIView
@end
@implementation VISRExternalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end
@interface VISRExternalSceneDelegate : NSObject <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow *window;
@end

static BOOL airplay(void) {
    for (AVAudioSessionPortDescription *port in AVAudioSession.sharedInstance.currentRoute.outputs)
        if ([port.portType isEqualToString:AVAudioSessionPortAirPlay]) return YES;
    return NO;
}
static NSString *audio_route_text(void) {
    NSMutableArray<NSString *> *ports=[NSMutableArray array];
    for(AVAudioSessionPortDescription *port in AVAudioSession.sharedInstance.currentRoute.outputs)
        [ports addObject:[NSString stringWithFormat:@"%@:%@",port.portType,port.portName]];
    return [ports componentsJoinedByString:@", "];
}
static NSString *port_name(void) {
    for (AVAudioSessionPortDescription *port in AVAudioSession.sharedInstance.currentRoute.outputs)
        if ([port.portType isEqualToString:AVAudioSessionPortHDMI]) return port.portName;
    return @"";
}
static UISceneConfiguration *external_configuration(void) {
    UISceneConfiguration *config = [UISceneConfiguration configurationWithName:@"VISRExternalDisplay"
        sessionRole:UIWindowSceneSessionRoleExternalDisplayNonInteractive];
    config.delegateClass = VISRExternalSceneDelegate.class;
    return config;
}
static UISceneConfiguration *configuration(id self, SEL selector, UIApplication *app,
    UISceneSession *session, UISceneConnectionOptions *options) {
    if ([session.role isEqualToString:UIWindowSceneSessionRoleExternalDisplayNonInteractive])
        return external_configuration();
    for (int i=0;i<2;i++)
        if (original_configuration[i] && [self isKindOfClass:configuration_class[i]])
            return ((UISceneConfiguration *(*)(id,SEL,UIApplication *,UISceneSession *,UISceneConnectionOptions *))
                original_configuration[i])(self,selector,app,session,options);
    return nil;
}
/* Before UIApplicationMain can offer the legacy external role to SDL. */
__attribute__((constructor)) static void install_configuration(void) {
    if(@available(iOS 27.0,*))return;
    SEL selector=@selector(application:configurationForConnectingSceneSession:options:);
    const char *names[]={"SDLUIKitSceneDelegate","SDLUIKitDelegate"};
    for(int i=0;i<2;i++) {
        configuration_class[i]=objc_getClass(names[i]);
        Method method=class_getInstanceMethod(configuration_class[i],selector);
        if(method)original_configuration[i]=method_setImplementation(method,(IMP)configuration);
    }
}
static void begin_modes(void) {
    if (!candidate_screen) return;
    offered=candidate_screen.availableModes;
    candidate_count=(int)MIN(offered.count,64);
    preferred_index=-1;
    NSMutableString *key=[NSMutableString stringWithString:port_name()];
    for(int i=0;i<candidate_count;i++) {
        UIScreenMode *mode=offered[i];
        candidates[i]=(struct host_external_mode){(int)mode.size.width,(int)mode.size.height,mode.pixelAspectRatio,0};
        if(mode==candidate_screen.preferredMode)preferred_index=i;
        [key appendFormat:@"/%dx%d:%.4f",candidates[i].width,candidates[i].height,candidates[i].pixel_aspect];
    }
    chosen_key=key;first_low=-1;accepting_low=0;
    if(!chosen_modes)chosen_modes=[NSMutableDictionary dictionary];
    if(!refused_modes)refused_modes=[NSMutableDictionary dictionary];
    NSMutableIndexSet *refused=refused_modes[chosen_key];
    if(!refused)refused_modes[chosen_key]=refused=[NSMutableIndexSet indexSet];
    for(int i=0;i<candidate_count;i++)candidates[i].refused=[refused containsIndex:(NSUInteger)i];
    NSValue *cached=chosen_modes[chosen_key];
    if(cached && !forced_width && CGSizeEqualToSize(candidate_screen.currentMode.size,cached.CGSizeValue)) {
        negotiating=0;pending=1;
    } else { negotiating=1;requested_index=-1; }
    pthread_mutex_lock(&snapshot_lock);
    snapshot.generation++;
    snapshot.mode_count=candidate_count;
    snprintf(snapshot.name,sizeof(snapshot.name),"%s",port_name().UTF8String);
    for(int i=0;i<candidate_count;i++) {
        snapshot.modes[i].width=candidates[i].width;snapshot.modes[i].height=candidates[i].height;
    }
    pthread_mutex_unlock(&snapshot_lock);
    host_logf(HOST_LOG_INFO,"external display: connected, \"%s\", %d modes, preferred %.0fx%.0f, current %.0fx%.0f",
        port_name().UTF8String,candidate_count,candidate_screen.preferredMode.size.width,
        candidate_screen.preferredMode.size.height,candidate_screen.currentMode.size.width,candidate_screen.currentMode.size.height);
}
@implementation VISRExternalSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session;(void)options;
    if(!phone_window) { waiting_scene=(UIWindowScene *)scene;return; }
    if(!metal_enabled) {
        host_logf(HOST_LOG_INFO,"external display: renderer is OpenGL ES; the system mirrors this device");return;
    }
    if(airplay()) { host_logf(HOST_LOG_INFO,"external display: AirPlay route; the system mirrors this device");return; }
    if(![scene isKindOfClass:UIWindowScene.class])return;
    UIWindowScene *window_scene=(UIWindowScene *)scene;
    self.window=[[UIWindow alloc]initWithWindowScene:window_scene];
    UIViewController *controller=[UIViewController new];
    controller.view=[[VISRExternalView alloc]initWithFrame:window_scene.coordinateSpace.bounds];
    controller.view.backgroundColor=UIColor.blackColor;
    self.window.rootViewController=controller;self.window.hidden=NO;
    external_window=self.window;candidate_screen=window_scene.screen;
    candidate_screen.overscanCompensation=UIScreenOverscanCompensationNone;
    begin_modes();
}
- (void)sceneDidDisconnect:(UIScene *)scene {
    if(waiting_scene==scene)waiting_scene=nil;
    if(self.window && external_window==self.window) {
        candidate_screen=nil;negotiating=0;pending=1;requested_index=-1;
        pthread_mutex_lock(&snapshot_lock);snapshot.generation++;pthread_mutex_unlock(&snapshot_lock);
    }
    self.window=nil;(void)scene;
}
@end
#endif

void host_external_install(void) {
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    static int installed;
    if(installed++)return;
    [NSNotificationCenter.defaultCenter addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:nil
        usingBlock:^(NSNotification *note){(void)note;atomic_store(&audio_changed,1);}];
#endif
}
static void snapshot_output(int external,int width,int height,const char *reason);
void host_external_attach(void *window_pointer,int metal) {
    SDL_Window *window=window_pointer;
    metal_enabled=metal;
    phone_window=(__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,NULL);
    phone_layer=host_sdl_metal_layer();
    output_screen=phone_window.windowScene.screen;
    SDL_GetWindowSizeInPixels(window,&output_width,&output_height);
    snapshot_output(output_screen && output_screen!=UIScreen.mainScreen,output_width,output_height,"preferred");
    last_touch=CACurrentMediaTime();
    host_external_install();
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    audio_route_rate=AVAudioSession.sharedInstance.sampleRate;
    host_logf(HOST_LOG_INFO,"audio route: %s, %.0f Hz",audio_route_text().UTF8String,
        audio_route_rate);
#endif
    const char *forced=getenv("HALO_EXTERNAL_MODE");
    char config[512];
    host_config_string("debug.external_mode","",config,sizeof(config));
    host_external_force(forced&&*forced?forced:config);
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    if(waiting_scene) {
        UIWindowScene *scene=waiting_scene;waiting_scene=nil;
        [(VISRExternalSceneDelegate *)scene.delegate scene:scene willConnectToSession:scene.session options:nil];
    }
    if(!metal)host_logf(HOST_LOG_INFO,"external display: renderer is OpenGL ES; the system mirrors this device");
    if (@available(iOS 27.0,*)) {
        if(metal && !accessory_registration)
            accessory_registration=[phone_window.rootViewController registerSceneAccessory:
                [UISceneAccessory externalNonInteractiveSceneAccessoryWithConfiguration:external_configuration()]];
    }
#endif
}
static void snapshot_output(int external,int width,int height,const char *reason) {
    pthread_mutex_lock(&snapshot_lock);
    snapshot.external=external;snapshot.width=width;snapshot.height=height;
    snapshot.refresh_hertz=(int)output_screen.maximumFramesPerSecond;
    snapshot.generation++;
    snprintf(snapshot.reason,sizeof(snapshot.reason),"%s",reason);
    if(!external){snapshot.name[0]=0;snapshot.mode_count=0;}
    pthread_mutex_unlock(&snapshot_lock);
}
void host_external_snapshot(struct host_external_snapshot *out) {
    pthread_mutex_lock(&snapshot_lock);*out=snapshot;pthread_mutex_unlock(&snapshot_lock);
    if(!companion_on && phone_window) {
        SDL_GetWindowSizeInPixels(host_sdl_window(),&out->width,&out->height);
        out->refresh_hertz=(int)phone_window.windowScene.screen.maximumFramesPerSecond;
        snprintf(out->reason,sizeof(out->reason),"preferred");
    }
}
int host_external_active(void) { return snapshot.external; }
void *host_external_phone_layer(void) { return companion_on?phone_layer:NULL; }
void *host_external_screen(void) { return (__bridge void *)(output_screen?:UIScreen.mainScreen); }
int host_external_size(int *width,int *height) {
#if TARGET_OS_MACCATALYST
    const char *test=getenv("HALO_TEST_OUTPUT_SIZES");
    if(test && *test) {
        int count=0,w=0,h=0;
        if(!host_external_mode_parse(test,0,&w,&h,&count))host_fatal("Invalid HALO_TEST_OUTPUT_SIZES");
        int index=(int)(test_output_frame/90);
        if(index>=count)index=count-1;
        if(!host_external_mode_parse(test,index,&w,&h,&count) || w<=0 || h<=0)
            host_fatal("HALO_TEST_OUTPUT_SIZES needs explicit sizes");
        *width=w;*height=h;return 1;
    }
#endif
    if(!companion_on)return 0;
    *width=output_width;*height=output_height;return 1;
}

/* A list is retained for live chords and suite iteration. Repeating it does
   not reset the player's current selection. An empty list restores policy. */
int host_external_force(const char *mode) {
    if(!mode)mode="";
    if(strlen(mode)>=sizeof(mode_list))return 0;
    int width=0,height=0,count=0;
    if(!host_external_mode_parse(mode,0,&width,&height,&count))return 0;
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    if(candidate_screen) {
        if(width && host_external_mode_choose(candidates,(size_t)candidate_count,preferred_index,0,
            width,height)<0)return 0;
    }
#endif
    if(!strcmp(mode,mode_list))return 1;
    snprintf(mode_list,sizeof(mode_list),"%s",mode);forced_index=0;
    forced_width=width;forced_height=height;
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    if(candidate_screen){negotiating=1;requested_index=-1;}
#endif
    return 1;
}
int host_external_cycle(int direction) {
    if(!mode_list[0])return 0;
    int width=0,height=0,count=0;
    if(!host_external_mode_parse(mode_list,0,&width,&height,&count))return 0;
    int next=(forced_index+(direction<0?-1:1)+count)%count;
    if(!host_external_mode_parse(mode_list,next,&width,&height,&count))return 0;
    forced_index=next;forced_width=width;forced_height=height;
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    if(candidate_screen){negotiating=1;requested_index=-1;}
#endif
    return 1;
}
void host_external_mode_label(char *text,size_t size) {
    if(forced_width)snprintf(text,size,"%dx%d",forced_width,forced_height);
    else snprintf(text,size,"preferred");
}
int host_external_ready(void) {
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    return snapshot.external && !negotiating && !pending &&
        (!forced_width || (snapshot.width==forced_width && snapshot.height==forced_height));
#else
    return 0;
#endif
}

#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
static CGSize mode_before;
static void mode_accept(void) {
    negotiating=0;pending=1;
    chosen_modes[chosen_key]=[NSValue valueWithCGSize:candidate_screen.currentMode.size];
    const char *reason=forced_width?"forced":requested_index==preferred_index?"preferred":"capped";
    pthread_mutex_lock(&snapshot_lock);
    snprintf(snapshot.reason,sizeof(snapshot.reason),"%s",reason);
    pthread_mutex_unlock(&snapshot_lock);
    host_logf(HOST_LOG_INFO,"external display: output %.0fx%.0f at %ld Hz [%s]%s",
        candidate_screen.currentMode.size.width,candidate_screen.currentMode.size.height,
        (long)candidate_screen.maximumFramesPerSecond,reason,forced_width?" OUTPUT FORCED BY debug.external_mode":"");
}
@interface VISRCompanionTouch : UIGestureRecognizer
@end
@implementation VISRCompanionTouch
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)touches;(void)event;last_touch=CACurrentMediaTime();self.state=UIGestureRecognizerStateFailed;
}
@end
static void label_layout(void) {
    if(!playing_label || !snapshot.external)return;
    CGRect bounds=phone_window.rootViewController.view.bounds;
    double ratio=output_height>0?(double)output_width/output_height:16.0/9.0;
    double width=MIN(bounds.size.width,bounds.size.height*ratio);
    double margin=(bounds.size.width-width)/2;
    UIEdgeInsets safe=phone_window.safeAreaInsets;
    double left=MAX(0,margin-safe.left),right=MAX(0,margin-safe.right);
    double top=(bounds.size.height-width/ratio)/2;
    playing_label.hidden=NO;
    if(MAX(left,right)>=48) {
        BOOL on_right=right>=left;
        playing_label.frame=CGRectMake(on_right?bounds.size.width-margin+4:safe.left+4,safe.top+8,
            MAX(left,right)-8,56);
    } else if(top-safe.top>=24) {
        playing_label.frame=CGRectMake(safe.left+8,safe.top,bounds.size.width-safe.left-safe.right-16,24);
    } else playing_label.hidden=YES;
    playing_label.alpha=host_external_companion_dim();
}
#endif
void host_external_poll(void) {
    if(atomic_exchange(&audio_changed,0)) {
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
        double rate=AVAudioSession.sharedInstance.sampleRate;
        if(rate>0 && fabs(rate-audio_route_rate)>0.5) {
            host_sdl_audio_route_changed();audio_route_rate=rate;
        }
        host_logf(HOST_LOG_INFO,"audio route: %s, %.0f Hz",audio_route_text().UTF8String,
            AVAudioSession.sharedInstance.sampleRate);
#endif
    }
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    if(@available(iOS 27.0,*)) {
        UISceneAccessoryRegistration *registration=accessory_registration;
        if(registration.available && !candidate_screen) {
            if(!accessory_available_since)accessory_available_since=CACurrentMediaTime();
            else if(accessory_available_since>0 && CACurrentMediaTime()-accessory_available_since>3) {
                host_logf(HOST_LOG_WARN,"external display: scene did not connect within three seconds of available");
                accessory_available_since=-1;
            }
        } else accessory_available_since=0;
    }
    label_layout();
    if(!candidate_screen || !negotiating)return;
    if(requested_index<0) {
        char cap_text[80];
        int cap=host_profile_external_value(host_device_profile(),"display.output_height",cap_text,sizeof(cap_text))?
            atoi(cap_text):0;
        int chosen=host_external_mode_choose(candidates,(size_t)candidate_count,preferred_index,cap,forced_width,forced_height);
        if(chosen<0) {
            if(!forced_width && first_low>=0 && !accepting_low) {chosen=first_low;accepting_low=1;}
            else {
                host_logf(HOST_LOG_WARN,"external display: no usable mode%s",forced_width?"; forced mode refused":"");
                negotiating=0;pending=0;
                if(!companion_on)external_window.hidden=YES;
                return;
            }
        }
        requested_index=chosen;
        mode_before=candidate_screen.currentMode.size;
        if(CGSizeEqualToSize(mode_before,offered[chosen].size))deadline=0;
        else {candidate_screen.currentMode=offered[chosen];deadline=CACurrentMediaTime()+1;return;}
    }
    CGSize current=candidate_screen.currentMode.size;
    if(CGSizeEqualToSize(current,offered[requested_index].size)) {
        if(candidate_screen.maximumFramesPerSecond<60 && !forced_width && !accepting_low) {
            host_logf(HOST_LOG_WARN,"external display: mode %.0fx%.0f below 60 Hz (%ld)",current.width,current.height,
                (long)candidate_screen.maximumFramesPerSecond);
            if(first_low<0)first_low=requested_index;
            candidates[requested_index].refused=1;requested_index=-1;return;
        }
        if(candidate_screen.maximumFramesPerSecond<60)
            host_logf(HOST_LOG_WARN,"external display: keeping mode below 60 Hz");
        mode_accept();return;
    }
    if(deadline && CACurrentMediaTime()<deadline && CGSizeEqualToSize(current,mode_before))return;
    host_logf(HOST_LOG_WARN,"external display: refused %dx%d; current %.0fx%.0f",candidates[requested_index].width,
        candidates[requested_index].height,current.width,current.height);
    [refused_modes[chosen_key] addIndex:(NSUInteger)requested_index];
    candidates[requested_index].refused=1;requested_index=-1;
#endif
}
void host_external_commit(int allow_pause) {
    (void)allow_pause;
#if TARGET_OS_MACCATALYST
    test_output_frame++;
#endif
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    if(pending) {
        pending=0;
        int was_external=snapshot.external;
        UIScreen *previous_screen=output_screen;
        if(candidate_screen && external_window) {
            output_screen=candidate_screen;
            output_width=(int)candidate_screen.currentMode.size.width;
            output_height=(int)candidate_screen.currentMode.size.height;
            if(!gpu_metal_output((__bridge void *)external_window.rootViewController.view.layer,
                (__bridge void *)output_screen,output_width,output_height)) {
                output_screen=previous_screen;pending=1;return;
            }
            char reason[32];snprintf(reason,sizeof(reason),"%s",snapshot.reason[0]?snapshot.reason:"preferred");
            companion_on=1;
            external_window.hidden=NO;
            snapshot_output(1,output_width,output_height,reason);
            if(!playing_label) {
                playing_label=[UILabel new];playing_label.text=@"Playing on display";
                playing_label.textColor=UIColor.lightGrayColor;playing_label.font=[UIFont systemFontOfSize:11];
                playing_label.numberOfLines=2;playing_label.textAlignment=NSTextAlignmentCenter;
                playing_label.userInteractionEnabled=NO;
                [phone_window.rootViewController.view addSubview:playing_label];
                VISRCompanionTouch *touch=[VISRCompanionTouch new];touch.cancelsTouchesInView=NO;
                [phone_window addGestureRecognizer:touch];
            }
            playing_label.hidden=NO;last_touch=CACurrentMediaTime();label_layout();
            host_logf(HOST_LOG_INFO,"external display: the game moved to the display (drawable %dx%d)",output_width,output_height);
        } else {
            output_screen=phone_window.windowScene.screen;
            SDL_GetWindowSizeInPixels(host_sdl_window(),&output_width,&output_height);
            if(!gpu_metal_output(phone_layer,(__bridge void *)output_screen,output_width,output_height)) {
                output_screen=previous_screen;pending=1;return;
            }
            companion_on=0;
            snapshot_output(output_screen && output_screen!=UIScreen.mainScreen,output_width,output_height,"preferred");
            external_window.hidden=YES;external_window=nil;offered=nil;first_low=-1;accepting_low=0;
            playing_label.hidden=YES;
            if(was_external && allow_pause)host_ios_touch_pause();
            host_logf(HOST_LOG_INFO,"external display: disconnected; the game is back on this device%s",
                was_external&&allow_pause?", paused":"");
        }
    }
#endif
    /* Stage Manager moves the SDL window, rather than offering a companion scene. */
    if(metal_enabled && !companion_on && phone_window && output_screen!=phone_window.windowScene.screen) {
        UIScreen *previous_screen=output_screen;
        output_screen=phone_window.windowScene.screen;
        SDL_GetWindowSizeInPixels(host_sdl_window(),&output_width,&output_height);
        if(!gpu_metal_output(phone_layer,(__bridge void *)output_screen,output_width,output_height)) {
            output_screen=previous_screen;return;
        }
        snapshot_output(output_screen && output_screen!=UIScreen.mainScreen,output_width,output_height,"preferred");
        host_logf(HOST_LOG_INFO,"external display: window moved to %dx%d at %ld Hz",output_width,output_height,
            (long)output_screen.maximumFramesPerSecond);
    }
}
double host_external_companion_dim(void) {
    int count=0;SDL_JoystickID *ids=SDL_GetGamepads(&count);int hardware=0;
    for(int i=0;i<count;i++) {
        const char *name=SDL_GetGamepadNameForID(ids[i]);
        if(name && strcmp(name,"Halo Touch Controls")) {hardware=1;break;}
    }
    SDL_free(ids);
    return hardware && CACurrentMediaTime()-last_touch>=10 ? 0.3 : 1.0;
}

#else
void host_external_install(void) {}
void host_external_attach(void *window,int metal) { (void)window;(void)metal; }
void host_external_poll(void) {}
void host_external_commit(int allow_pause) { (void)allow_pause; }
int host_external_active(void) { return 0; }
int host_external_size(int *width,int *height) { (void)width;(void)height;return 0; }
void *host_external_phone_layer(void) { return NULL; }
void *host_external_screen(void) { return NULL; }
double host_external_companion_dim(void) { return 1.0; }
void host_external_snapshot(struct host_external_snapshot *out) {
    memset(out,0,sizeof(*out));
    SDL_Window *window=host_sdl_window();
    if(window)SDL_GetWindowSizeInPixels(window,&out->width,&out->height);
    out->refresh_hertz=90;
    snprintf(out->reason,sizeof(out->reason),"preferred");
}
int host_external_cycle(int direction) { (void)direction;return 0; }
int host_external_force(const char *mode) { (void)mode;return 0; }
int host_external_ready(void) { return 0; }
void host_external_mode_label(char *text,size_t size) { if(size)text[0]=0; }
#endif
