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
#include "host_external_session.h"
#include "host_external_splash.h"
#include "renderer_build.h"
#include "host_audio_route.h"
#include "host_audio_stall.h"
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
static int output_failures;

/* The Mac app's window on a second monitor is a window, not an external output. */
static int screen_is_external(UIScreen *screen) {
#if TARGET_OS_MACCATALYST
    (void)screen;return 0;
#else
    return screen && screen!=UIScreen.mainScreen;
#endif
}
/* A switch the renderer cannot take yet (before it starts) stays pending and is logged once. */
static int output_switch(void *layer,UIScreen *screen,int width,int height) {
    /* gpu_metal_output restarts pacing through host_external_screen before
       returning, so publish the destination for that call and roll back on refusal. */
    UIScreen *previous_screen=output_screen;
    output_screen=screen;
    if(gpu_metal_output(layer,(__bridge void *)screen,width,height)) {output_failures=0;return 1;}
    output_screen=previous_screen;
    if(++output_failures==1)
        host_logf(HOST_LOG_WARN,"external display: the renderer cannot take the %dx%d output yet",width,height);
    return 0;
}
#if TARGET_OS_MACCATALYST
/* Runner-only synthetic output shapes; the actual app window stays put. */
static unsigned long test_output_frame;
#endif

#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
static UIWindow *external_window;
static UIWindowScene *waiting_scene;
static struct host_external_splash splash_state;
static UIScreen *candidate_screen;
static NSArray<UIScreenMode *> *offered;
static struct host_external_mode candidates[64];
static int candidate_count, preferred_index, requested_index = -1;
static struct host_external_session session_state;
static int drawable_cap;
static CFTimeInterval deadline, accessory_available_since;
static int mode_refusals;
static CGSize mode_accepted;
static int mode_reissued;
static int first_low=-1, accepting_low;
static id accessory_registration;
static int accessory_available_logged=-1;
static struct host_external_refresh accessory_refresh;
static struct host_audio_route audio_route;
static _Atomic int audio_recover;
/* a system interruption (a call, Siri, an alarm) is under way: the system has stopped the sound on purpose, so the
   stall check and debug.audio_reopen_test leave it alone until it ends */
static _Atomic int audio_interrupted;
static UILabel *playing_label;
static IMP original_configuration[2];
static Class configuration_class[2];

@interface VISRExternalView : UIView
@end
@implementation VISRExternalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end
/* The scene's root view is plain black; the Metal view inside it is the largest aspect-correct rectangle of the
   screen the drawable fits, so a picture rendered below the screen's size is scaled up by the layer to fill it
   (bars only where the shapes differ), whatever size the system gives the scene. */
static VISRExternalView *external_metal_view;
static CGSize external_drawable;
static void external_fit(UIView *root);
static void external_log_geometry(const char *when) {
    UIView *root=external_window.rootViewController.view;
    UIScreen *screen=candidate_screen;
    host_logf(HOST_LOG_INFO,"external display: %s: scene %.0fx%.0f pt, screen scale %.2f (native %.2f, %.0fx%.0f px), picture %.0fx%.0f pt at %.0f,%.0f for a %.0fx%.0f drawable",
        when,root.bounds.size.width,root.bounds.size.height,screen.scale,screen.nativeScale,
        screen.nativeBounds.size.width,screen.nativeBounds.size.height,
        external_metal_view.frame.size.width,external_metal_view.frame.size.height,
        external_metal_view.frame.origin.x,external_metal_view.frame.origin.y,
        external_drawable.width,external_drawable.height);
}
/* What is actually on screen, read back from UIKit, not what VISR asked for: the window's visibility and scene, the
   scene's activation state, whether the system still mirrors the main screen onto this one, and whether the Metal
   view is in that window. */
static CFTimeInterval verify_at;
static int verify_left;
static void external_log_verified(const char *when) {
    UIWindow *window=external_window;
    UIWindowScene *scene=window.windowScene;
    const char *state=!scene?"none":scene.activationState==UISceneActivationStateForegroundActive?"foreground-active":
        scene.activationState==UISceneActivationStateForegroundInactive?"foreground-inactive":
        scene.activationState==UISceneActivationStateBackground?"background":"unattached";
    id mirrored=[UIScreen.mainScreen valueForKey:@"mirroredScreen"];
    host_logf(HOST_LOG_INFO,"external display: verified %s: window %s (hidden %d, key %d, level %.0f), scene screen is the connected display %d, scene %s, metal view in the window %d (hidden %d, alpha %.2f), the main screen is mirrored to a screen %d, %lu screens",
        when,window?"present":"missing",window.hidden,window.keyWindow,window.windowLevel,
        scene.screen==candidate_screen,state,external_metal_view.window==window,external_metal_view.hidden,
        external_metal_view.alpha,mirrored!=nil,(unsigned long)UIScreen.screens.count);
}
@interface VISRExternalRoot : UIView
@end
@implementation VISRExternalRoot
- (void)layoutSubviews { [super layoutSubviews];external_fit(self); }
@end
static void external_fit(UIView *root) {
    if(!external_metal_view || external_metal_view.superview!=root)return;
    double rect[4];
    if(!host_external_fit(root.bounds.size.width,root.bounds.size.height,(int)external_drawable.width,
        (int)external_drawable.height,rect))return;
    CGRect frame=CGRectMake(rect[0],rect[1],rect[2],rect[3]);
    CAMetalLayer *layer=(CAMetalLayer *)external_metal_view.layer;
    layer.contentsGravity=kCAGravityResize;
    if(!CGRectEqualToRect(external_metal_view.frame,frame))external_metal_view.frame=frame;
    /* (a size UIKit changed with the frame goes back to the renderer's) */
    if(!CGSizeEqualToSize(layer.drawableSize,external_drawable))layer.drawableSize=external_drawable;
}
@interface VISRExternalSceneDelegate : NSObject <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow *window;
@end

static NSString *audio_route_text(void) {
    NSMutableArray<NSString *> *ports=[NSMutableArray array];
    for(AVAudioSessionPortDescription *port in AVAudioSession.sharedInstance.currentRoute.outputs)
        [ports addObject:[NSString stringWithFormat:@"%@:%@:%@",port.portType,port.UID,port.portName]];
    return [ports componentsJoinedByString:@", "];
}
/* an output that carries a display or a car as well as sound: a route change to or from one reopens the sound
   device (host_audio_route.h) */
static int audio_route_external(void) {
    for (AVAudioSessionPortDescription *port in AVAudioSession.sharedInstance.currentRoute.outputs)
        if ([port.portType isEqualToString:AVAudioSessionPortHDMI] ||
            [port.portType isEqualToString:AVAudioSessionPortAirPlay] ||
            [port.portType isEqualToString:AVAudioSessionPortUSBAudio] ||
            [port.portType isEqualToString:AVAudioSessionPortDisplayPort] ||
            [port.portType isEqualToString:AVAudioSessionPortCarAudio]) return 1;
    return 0;
}
/* an output that carries video as well as sound (HDMI, AirPlay): a display the system may hand a scene */
static int audio_route_has_video(void) {
    for (AVAudioSessionPortDescription *port in AVAudioSession.sharedInstance.currentRoute.outputs)
        if ([port.portType isEqualToString:AVAudioSessionPortHDMI] ||
            [port.portType isEqualToString:AVAudioSessionPortAirPlay]) return 1;
    return 0;
}
static NSString *port_name(void) {
    for (AVAudioSessionPortDescription *port in AVAudioSession.sharedInstance.currentRoute.outputs)
        if ([port.portType isEqualToString:AVAudioSessionPortHDMI] ||
            [port.portType isEqualToString:AVAudioSessionPortAirPlay]) return port.portName;
    return @"External display";
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
        /* a class that inherits the method from the other would hand back our own replacement as SDL's */
        if(method && method_getImplementation(method)!=(IMP)configuration)
            original_configuration[i]=method_setImplementation(method,(IMP)configuration);
    }
}
static void begin_modes(void) {
    if (!candidate_screen) return;
    offered=candidate_screen.availableModes;
    candidate_count=(int)MIN(offered.count,64);
    preferred_index=-1;
    for(int i=0;i<candidate_count;i++) {
        UIScreenMode *mode=offered[i];
        candidates[i]=(struct host_external_mode){(int)mode.size.width,(int)mode.size.height,mode.pixelAspectRatio,0};
        if(mode==candidate_screen.preferredMode)preferred_index=i;
        host_logf(HOST_LOG_INFO,"external display: offered %dx%d, pixel aspect %.4f",
            candidates[i].width,candidates[i].height,candidates[i].pixel_aspect);
    }
    first_low=-1;accepting_low=0;mode_accepted=CGSizeZero;mode_refusals=0;
    requested_index=-1;
    host_external_session_connect(&session_state,(uintptr_t)(__bridge void *)external_window.windowScene);
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
/* The launch menu's artwork (icon over the VISR wordmark on the menu background), scaled to the display. */
static UIWindow *splash_window(UIWindowScene *scene) {
    UIWindow *window=[[UIWindow alloc]initWithWindowScene:scene];
    UIViewController *controller=[UIViewController new];
    UIView *view=[[UIView alloc]initWithFrame:scene.coordinateSpace.bounds];
    CGFloat unit=MAX(1.0,MIN(view.bounds.size.width,view.bounds.size.height)/400.0);
    view.backgroundColor=[UIColor colorWithRed:0x08/255.0 green:0x12/255.0 blue:0x16/255.0 alpha:1];
    UIImageView *icon=[[UIImageView alloc]initWithImage:[UIImage imageNamed:@"VISRIcon"]];
    icon.contentMode=UIViewContentModeScaleAspectFit;icon.clipsToBounds=YES;
    icon.layer.cornerCurve=kCACornerCurveContinuous;icon.layer.cornerRadius=104*unit*0.2237;
    UILabel *title=[UILabel new];
    UIFont *font=[UIFont fontWithName:@"Orbitron-Regular" size:36*unit]?:[UIFont systemFontOfSize:36*unit];
    title.attributedText=[[NSAttributedString alloc]initWithString:@"VISR" attributes:@{
        NSFontAttributeName:font,NSKernAttributeName:@(2.16*unit),
        NSForegroundColorAttributeName:[UIColor colorWithRed:0xe8/255.0 green:0xf6/255.0 blue:0xfa/255.0 alpha:1]}];
    UIStackView *stack=[[UIStackView alloc]initWithArrangedSubviews:@[icon,title]];
    stack.axis=UILayoutConstraintAxisVertical;stack.alignment=UIStackViewAlignmentCenter;stack.spacing=16*unit;
    stack.translatesAutoresizingMaskIntoConstraints=NO;
    [view addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:view.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:104*unit],[icon.heightAnchor constraintEqualToConstant:104*unit]]];
    controller.view=view;window.rootViewController=controller;window.hidden=NO;
    return window;
}
static int renderer_is_metal(void) {
    if(HALO_METAL_ONLY)return 1;
    char renderer[16]="";
    const char *launch=getenv("HALO_RENDERER");
    if(launch&&*launch)snprintf(renderer,sizeof(renderer),"%s",launch);
    else host_config_string("display.renderer","metal",renderer,sizeof(renderer));
    return !strcmp(renderer,"metal");
}
@implementation VISRExternalSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session;(void)options;
    if(![scene isKindOfClass:UIWindowScene.class])return;
    if(!phone_window) {
        waiting_scene=(UIWindowScene *)scene;
        if(host_external_splash_connect(&splash_state,(uintptr_t)(__bridge void *)scene)) {
            self.window=splash_window(waiting_scene);
            host_logf(HOST_LOG_INFO,"external display: showing the splash until the game starts");
        }
        return;
    }
    /* the splash window gives way to the game's */
    if(self.window && external_window!=self.window) { self.window.hidden=YES;self.window=nil; }
    if(!metal_enabled) {
        host_logf(HOST_LOG_INFO,"external display: renderer is OpenGL ES; the system mirrors this device");return;
    }
    UIWindowScene *window_scene=(UIWindowScene *)scene;
    if(self.window && external_window==self.window)return;
    self.window=[[UIWindow alloc]initWithWindowScene:window_scene];
    UIViewController *controller=[UIViewController new];
    controller.view=[[VISRExternalRoot alloc]initWithFrame:window_scene.coordinateSpace.bounds];
    controller.view.backgroundColor=UIColor.blackColor;
    external_metal_view=[[VISRExternalView alloc]initWithFrame:controller.view.bounds];
    external_metal_view.backgroundColor=UIColor.blackColor;
    [controller.view addSubview:external_metal_view];
    self.window.rootViewController=controller;self.window.hidden=NO;
    external_window=self.window;candidate_screen=window_scene.screen;
    candidate_screen.overscanCompensation=UIScreenOverscanCompensationNone;
    begin_modes();
}
- (void)sceneDidDisconnect:(UIScene *)scene {
    if(waiting_scene==scene)waiting_scene=nil;
    host_external_splash_disconnect(&splash_state,(uintptr_t)(__bridge void *)scene);
    if(host_external_session_disconnect(&session_state,(uintptr_t)(__bridge void *)scene)) {
        candidate_screen=nil;requested_index=-1;
        pthread_mutex_lock(&snapshot_lock);snapshot.generation++;pthread_mutex_unlock(&snapshot_lock);
    }
    if(external_window==self.window)external_metal_view=nil;
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
    /* a mode change (the display's own, or the one VISR asked for) resizes the scene: place the picture again */
    [NSNotificationCenter.defaultCenter addObserverForName:UIScreenModeDidChangeNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *note){
            if(note.object!=candidate_screen || !external_window)return;
            UIView *root=external_window.rootViewController.view;
            [root setNeedsLayout];[root layoutIfNeeded];
            if(external_drawable.width>0)external_log_geometry("the display changed its mode");
        }];
    [NSNotificationCenter.defaultCenter addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:nil
        usingBlock:^(NSNotification *note){
            NSUInteger type=[note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
            if(type==AVAudioSessionInterruptionTypeBegan)atomic_store(&audio_interrupted,1);
            else if(type==AVAudioSessionInterruptionTypeEnded) {
                atomic_store(&audio_interrupted,0);
                atomic_store(&audio_recover,1);atomic_store(&audio_changed,1);
            }
        }];
    /* an interruption's end is not always sent (the app was suspended during it): becoming active ends it too */
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil
        usingBlock:^(NSNotification *note){(void)note;atomic_store(&audio_interrupted,0);}];
    [NSNotificationCenter.defaultCenter addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:nil
        usingBlock:^(NSNotification *note){
            (void)note;atomic_store(&audio_recover,1);atomic_store(&audio_changed,1);
        }];
#endif
}
void host_external_menu_splash(void) {
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    uintptr_t scene=host_external_splash_menu(&splash_state,renderer_is_metal());
    if(scene && waiting_scene) {
        VISRExternalSceneDelegate *delegate=(VISRExternalSceneDelegate *)waiting_scene.delegate;
        if([delegate isKindOfClass:VISRExternalSceneDelegate.class] && !delegate.window) {
            delegate.window=splash_window(waiting_scene);
            host_external_splash_shown(&splash_state,scene);
        }
    }
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
    snapshot_output(screen_is_external(output_screen),output_width,output_height,"preferred");
    last_touch=CACurrentMediaTime();
    host_external_install();
    atomic_store(&audio_changed,1);
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    host_audio_route_update(&audio_route,audio_route_text().UTF8String,AVAudioSession.sharedInstance.sampleRate,
        (int)AVAudioSession.sharedInstance.outputNumberOfChannels,audio_route_external(),0);
#endif
    const char *forced=getenv("HALO_EXTERNAL_MODE");
    char config[512];
    host_config_string("debug.external_mode","",config,sizeof(config));
    host_external_force(forced&&*forced?forced:config);
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    host_external_splash_attach(&splash_state);
    if(waiting_scene) {
        UIWindowScene *scene=waiting_scene;waiting_scene=nil;
        [(VISRExternalSceneDelegate *)scene.delegate scene:scene willConnectToSession:scene.session options:nil];
    }
    if(!metal)host_logf(HOST_LOG_INFO,"external display: renderer is OpenGL ES; the system mirrors this device");
    /* UISceneAccessory is in the iOS 27 SDK; an app built with an earlier SDK (the CI's) mirrors on iOS 27 */
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 270000
    if (@available(iOS 27.0,*)) {
        if(metal && !accessory_registration)
            accessory_registration=[phone_window.rootViewController registerSceneAccessory:
                [UISceneAccessory externalNonInteractiveSceneAccessoryWithConfiguration:external_configuration()]];
        accessory_available_logged=-1;
    }
#endif
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
/* The window scene the renderer presents in: the external display's while the game is on it, else the game
   window's (which Stage Manager may have moved to another screen). Its display link runs on that display's clock;
   UIScreen's own display link is deprecated in iOS 27 and is not tied to the scene's display. nil before the game
   window exists. */
void *host_external_scene(void) {
    UIWindowScene *scene=nil;
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    if(external_window && candidate_screen && output_screen==candidate_screen)scene=external_window.windowScene;
#endif
    if(!scene && phone_window && output_screen==phone_window.windowScene.screen)scene=phone_window.windowScene;
    return (__bridge void *)scene;
}
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
    if(candidate_screen){session_state.negotiating=1;requested_index=-1;}
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
    if(candidate_screen){session_state.negotiating=1;requested_index=-1;}
#endif
    return 1;
}
void host_external_mode_label(char *text,size_t size) {
    if(forced_width)snprintf(text,size,"%dx%d",forced_width,forced_height);
    else snprintf(text,size,"preferred");
}
int host_external_ready(void) {
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    return snapshot.external && !session_state.negotiating && !session_state.pending &&
        (!forced_width || (snapshot.width==forced_width && snapshot.height==forced_height));
#else
    return 0;
#endif
}

#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
static CGSize mode_before;
static void mode_accept(void) {
    host_external_session_accept(&session_state);
    mode_accepted=candidate_screen.currentMode.size;
    int width=0,height=0;
    host_external_mode_drawable((int)mode_accepted.width,(int)mode_accepted.height,drawable_cap,&width,&height);
    const char *reason=forced_width?"forced":
        width<(int)mode_accepted.width || height<(int)mode_accepted.height?"capped":
        requested_index==preferred_index?"preferred":"capped";
    pthread_mutex_lock(&snapshot_lock);
    snprintf(snapshot.reason,sizeof(snapshot.reason),"%s",reason);
    pthread_mutex_unlock(&snapshot_lock);
    host_logf(HOST_LOG_INFO,"external display: physical %.0fx%.0f at %ld Hz, drawable %dx%d [%s]%s",
        mode_accepted.width,mode_accepted.height,(long)candidate_screen.maximumFramesPerSecond,
        width,height,reason,forced_width?" OUTPUT FORCED BY debug.external_mode":"");
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
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
/* Sound back on the route the system has now (the player's own pick in the output menu is one), after a route
   change, an interruption's end, a media services reset or a stall: the session made active again (the system
   deactivates it in an interruption, and SDL activates it only when its first device opens), then every stream
   and the device under them closed and opened again on that route. */
static void audio_reopen(const char *why) {
    NSError *error=nil;
    if(![AVAudioSession.sharedInstance setActive:YES error:&error])
        host_logf(HOST_LOG_WARN,"audio route: cannot make the audio session active (%s): %s",why,
            error.localizedDescription.UTF8String);
    host_logf(HOST_LOG_INFO,"audio route: reopening the sound device (%s), output %s",why,audio_route_text().UTF8String);
    host_sdl_audio_route_changed();
}
#endif
void host_external_poll(void) {
    if(atomic_exchange(&audio_changed,0)) {
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
        AVAudioSession *audio=AVAudioSession.sharedInstance;
        int recover=atomic_exchange(&audio_recover,0);
        if(host_audio_route_update(&audio_route,audio_route_text().UTF8String,audio.sampleRate,
            (int)audio.outputNumberOfChannels,audio_route_external(),recover))
            audio_reopen(recover?"the system's audio came back":"the route changed to or from an external output, or its format changed");
        host_logf(HOST_LOG_INFO,"audio route: category=%s mode=%s output=%s rate=%.0f Hz latency=%.3f s",
            audio.category.UTF8String,audio.mode.UTF8String,audio_route_text().UTF8String,
            audio.sampleRate,audio.outputLatency);
        if(candidate_screen) {
            pthread_mutex_lock(&snapshot_lock);
            snprintf(snapshot.name,sizeof(snapshot.name),"%s",port_name().UTF8String);
            pthread_mutex_unlock(&snapshot_lock);
            playing_label.text=[NSString stringWithFormat:@"Playing on %@",port_name()];
        }
#endif
    }
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    /* A stream that should be playing (not paused by the game or the background) whose device has stopped asking
       for sound: no callback for 1.5 s, whatever the game's sound was. A silent game still gets its callbacks, so
       quiet moments never count, and a system interruption never does (host_audio_stall.h has the schedule). */
    {
        static unsigned long long stall_checked, test_reopened;
        static struct host_audio_stall stall;
        static int was_interrupted;
        static double test_every=-1.0;
        unsigned long long now=SDL_GetTicksNS();
        int interrupted=atomic_load(&audio_interrupted);
        /* (an interruption over: the 1.5 s starts now, not inside it) */
        if(was_interrupted && !interrupted)host_sdl_audio_touch();
        if(interrupted && !was_interrupted)host_logf(HOST_LOG_INFO,"audio route: a system interruption began; the sound waits for its end");
        was_interrupted=interrupted;
        /* (debug.audio_reopen_test: the reopen a route change makes, on a timer, for a device that cannot be replugged) */
        if(test_every<0.0)test_every=host_config_real("debug.audio_reopen_test",0.0);
        if(test_every>0.0 && !interrupted) {
            if(!test_reopened)test_reopened=now;
            else if(now-test_reopened>=(unsigned long long)(test_every*1e9)) {
                test_reopened=now;
                audio_reopen("debug.audio_reopen_test");
            }
        }
        if(now-stall_checked>=250*SDL_NS_PER_MS) {
            stall_checked=now;
            if(host_audio_stall_due(&stall,now,interrupted,!interrupted && host_sdl_audio_stalled(now)))
                audio_reopen("the sound device stopped asking for sound for 1.5 s");
        }
    }
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 270000
    if(@available(iOS 27.0,*)) {
        UISceneAccessoryRegistration *registration=accessory_registration;
        if(registration && accessory_available_logged!=(int)registration.available) {
            accessory_available_logged=registration.available;
            host_logf(HOST_LOG_INFO,"external display: scene accessory %s (%lu screens, route %s)",
                registration.available?"available":"not available",(unsigned long)UIScreen.screens.count,audio_route_text().UTF8String);
        }
        /* a display plugged in while the game runs can be left mirrored with the registration never available:
           register it again (at most twice) so the system offers the scene */
        if(registration && metal_enabled) {
            int seen=UIScreen.screens.count>1 || audio_route_has_video();
            if(host_external_refresh_due(&accessory_refresh,seen,registration.available,candidate_screen!=nil,
                CACurrentMediaTime())) {
                host_logf(HOST_LOG_INFO,"external display: a display is connected but the scene accessory is not available; registering it again");
                [phone_window.rootViewController unregisterSceneAccessory:registration];
                accessory_registration=[phone_window.rootViewController registerSceneAccessory:
                    [UISceneAccessory externalNonInteractiveSceneAccessoryWithConfiguration:external_configuration()]];
                accessory_available_logged=-1;accessory_available_since=0;
                registration=accessory_registration;
            }
        }
        if(registration.available && !candidate_screen) {
            if(!accessory_available_since)accessory_available_since=CACurrentMediaTime();
            else if(accessory_available_since>0 && CACurrentMediaTime()-accessory_available_since>3) {
                host_logf(HOST_LOG_WARN,"external display: scene did not connect within three seconds of available");
                accessory_available_since=-1;
            }
        } else accessory_available_since=0;
    }
#endif
    label_layout();
    if(verify_left>0 && companion_on && CACurrentMediaTime()>=verify_at) {
        external_log_verified(verify_left==2?"2 s after the move":"6 s after the move");
        verify_at=CACurrentMediaTime()+4;verify_left--;
    }
    /* The display can change its own mode (a slow monitor finishing a switch, a receiver re-syncing): follow it.
       VISR never asks for another mode in answer, so a late change cannot start a chain of requests. */
    if(candidate_screen && !session_state.negotiating && mode_accepted.width>0 &&
        !CGSizeEqualToSize(candidate_screen.currentMode.size,mode_accepted)) {
        host_logf(HOST_LOG_INFO,"external display: the display changed itself to %.0fx%.0f; keeping it",
            candidate_screen.currentMode.size.width,candidate_screen.currentMode.size.height);
        mode_accept();
    }
    if(!candidate_screen || !session_state.negotiating)return;
    if(requested_index<0) {
        char cap_text[80];
        struct host_profile profile;
        host_device_profile_copy(&profile);
        int cap=host_profile_external_value(&profile,"display.output_height",cap_text,sizeof(cap_text))?
            atoi(cap_text):1080;
        drawable_cap=forced_width?0:cap;
        /* The display stays in its own preferred (native) mode: the profile's output height bounds the drawable
           only (host_external_mode_drawable), and the layer scales the picture to the screen (host_external_fit). */
        int chosen=host_external_mode_choose(candidates,(size_t)candidate_count,preferred_index,0,forced_width,forced_height);
        if(chosen<0) {
            if(!forced_width && first_low>=0 && !accepting_low) {chosen=first_low;accepting_low=1;}
            else if(forced_width) {
                host_logf(HOST_LOG_WARN,"external display: forced mode %dx%d refused; choosing by the profile instead",
                    forced_width,forced_height);
                forced_width=forced_height=0;return;
            } else {
                host_logf(HOST_LOG_WARN,"external display: no usable mode");
                CGSize current=candidate_screen.currentMode.size;
                if(current.width>0 && current.height>0 &&
                    fabs(candidate_screen.currentMode.pixelAspectRatio-1.0)<=0.001) {
                    host_logf(HOST_LOG_WARN,"external display: using current physical mode with bounded drawable");
                    mode_accept();
                } else {
                    /* Return to local rendering if a previously active output becomes unusable. */
                    candidate_screen=nil;
                    host_external_session_disconnect(&session_state,session_state.owner);
                }
                return;
            }
        }
        requested_index=chosen;mode_reissued=0;
        mode_before=candidate_screen.currentMode.size;
        if(CGSizeEqualToSize(mode_before,offered[chosen].size))deadline=0;
        else {candidate_screen.currentMode=offered[chosen];deadline=CACurrentMediaTime()+1;return;}
    }
    CGSize current=candidate_screen.currentMode.size;
    if(CGSizeEqualToSize(current,offered[requested_index].size)) {
        if(candidate_screen.maximumFramesPerSecond<60 && !forced_width && !accepting_low && requested_index!=preferred_index) {
            host_logf(HOST_LOG_WARN,"external display: mode %.0fx%.0f below 60 Hz (%ld)",current.width,current.height,
                (long)candidate_screen.maximumFramesPerSecond);
            if(first_low<0)first_low=requested_index;
            candidates[requested_index].refused=1;requested_index=-1;return;
        }
        if(candidate_screen.maximumFramesPerSecond<60)
            host_logf(HOST_LOG_WARN,"external display: keeping mode below 60 Hz");
        mode_accept();return;
    }
    /* wait the whole second: the system may move the display to another mode of its own first */
    if(deadline && CACurrentMediaTime()<deadline)return;
    if(deadline && !mode_reissued && !CGSizeEqualToSize(current,mode_before)) {
        mode_reissued=1;mode_before=current;
        candidate_screen.currentMode=offered[requested_index];deadline=CACurrentMediaTime()+1;return;
    }
    host_logf(HOST_LOG_WARN,"external display: refused %dx%d; current %.0fx%.0f",candidates[requested_index].width,
        candidates[requested_index].height,current.width,current.height);
    candidates[requested_index].refused=1;requested_index=-1;
    /* A display that ignores mode changes (an EDID-fixed monitor) refuses every candidate, a second each: after two
       refusals with the display still in the mode it connected in, keep that mode with a bounded drawable. */
    if(++mode_refusals>=2 && !forced_width && !CGSizeEqualToSize(current,CGSizeZero) &&
        fabs(candidate_screen.currentMode.pixelAspectRatio-1.0)<=0.001) {
        host_logf(HOST_LOG_INFO,"external display: %d modes refused; keeping the display's own %.0fx%.0f",mode_refusals,
            current.width,current.height);
        mode_accept();
    }
#endif
}
void host_external_commit(int allow_pause) {
    (void)allow_pause;
    /* (a benchmark display variant's built-in output cap, before the frame's first draw) */
    gpu_metal_output_cap_commit();
#if TARGET_OS_MACCATALYST
    test_output_frame++;
#endif
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    if(session_state.pending) {
        host_external_session_complete(&session_state);
        int was_external=snapshot.external;
        if(candidate_screen && !session_state.negotiating && external_window) {
            int width=0,height=0;
            if(!host_external_mode_drawable((int)candidate_screen.currentMode.size.width,
                (int)candidate_screen.currentMode.size.height,drawable_cap,&width,&height)) {
                session_state.pending=1;return;
            }
            external_drawable=CGSizeMake(width,height);
            if(!output_switch((__bridge void *)external_metal_view.layer,
                candidate_screen,width,height)) {
                session_state.pending=1;return;
            }
            output_screen=candidate_screen;output_width=width;output_height=height;
            char reason[32];snprintf(reason,sizeof(reason),"%s",snapshot.reason[0]?snapshot.reason:"preferred");
            companion_on=1;
            external_window.hidden=NO;
            snapshot_output(1,output_width,output_height,reason);
            pthread_mutex_lock(&snapshot_lock);
            snapshot.mode_count=candidate_count;
            snprintf(snapshot.name,sizeof(snapshot.name),"%s",port_name().UTF8String);
            for(int i=0;i<candidate_count;i++) {
                snapshot.modes[i].width=candidates[i].width;snapshot.modes[i].height=candidates[i].height;
            }
            pthread_mutex_unlock(&snapshot_lock);
            if(!playing_label) {
                playing_label=[UILabel new];playing_label.text=@"Playing on display";
                playing_label.textColor=UIColor.lightGrayColor;playing_label.font=[UIFont systemFontOfSize:11];
                playing_label.numberOfLines=2;playing_label.textAlignment=NSTextAlignmentCenter;
                playing_label.userInteractionEnabled=NO;
                [phone_window.rootViewController.view addSubview:playing_label];
                VISRCompanionTouch *touch=[VISRCompanionTouch new];touch.cancelsTouchesInView=NO;
                [phone_window addGestureRecognizer:touch];
            }
            playing_label.text=[NSString stringWithFormat:@"Playing on %@",port_name()];
            playing_label.hidden=NO;last_touch=CACurrentMediaTime();label_layout();
            external_fit(external_window.rootViewController.view);
            external_log_geometry("the renderer was moved to the display");
            external_log_verified("at the move");
            verify_at=CACurrentMediaTime()+2;verify_left=2;
            host_logf(HOST_LOG_INFO,"external display: the renderer presents to the display's layer (drawable %dx%d); the verified lines say what is on screen",output_width,output_height);
        } else if(!companion_on) {
            /* the display went away before the game moved to it: the renderer never left this device */
            external_window.hidden=YES;
            if(!candidate_screen) {external_window=nil;offered=nil;first_low=-1;accepting_low=0;}
            snapshot_output(screen_is_external(output_screen),output_width,output_height,"preferred");
            if(!candidate_screen)host_logf(HOST_LOG_INFO,"external display: disconnected before the game moved to it");
        } else {
            int width=0,height=0;
            UIScreen *local_screen=phone_window.windowScene.screen;
            SDL_GetWindowSizeInPixels(host_sdl_window(),&width,&height);
            if(!output_switch(phone_layer,local_screen,width,height)) {
                session_state.pending=1;return;    /* retry before the next frame */
            }
            output_screen=local_screen;output_width=width;output_height=height;
            companion_on=0;
            snapshot_output(screen_is_external(output_screen),output_width,output_height,"preferred");
            external_window.hidden=YES;
            if(!candidate_screen) {external_window=nil;offered=nil;first_low=-1;accepting_low=0;}
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
        if(!output_switch(phone_layer,output_screen,output_width,output_height)) {
            output_screen=previous_screen;return;
        }
        snapshot_output(screen_is_external(output_screen),output_width,output_height,"preferred");
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
void host_external_menu_splash(void) {}
void host_external_attach(void *window,int metal) { (void)window;(void)metal; }
void host_external_poll(void) {}
void host_external_commit(int allow_pause) { (void)allow_pause; }
int host_external_active(void) { return 0; }
int host_external_size(int *width,int *height) { (void)width;(void)height;return 0; }
void *host_external_phone_layer(void) { return NULL; }
void *host_external_screen(void) { return NULL; }
void *host_external_scene(void) { return NULL; }
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
