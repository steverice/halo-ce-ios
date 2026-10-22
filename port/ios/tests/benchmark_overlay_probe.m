/* Asset-free UIKit stand-ins for the production overlay's window lifecycle. */
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#include <assert.h>
#include <stdarg.h>

static NSString *const UISceneDidActivateNotification=@"scene active";
static NSString *const UIWindowSceneSessionRoleApplication=@"application";
enum { UISceneActivationStateUnattached=-1, UISceneActivationStateForegroundActive,
    UISceneActivationStateForegroundInactive, UISceneActivationStateBackground };
static const double UIWindowLevelNormal=0;
#define HOST_LOG_INFO 0
static void host_logf(int level, const char *format, ...) { (void)level; (void)format; }

@interface UIScene : NSObject
@property NSInteger activationState;
@end
@implementation UIScene
@end
@interface UIWindowScene : UIScene
@property(strong) UIWindowScene *session, *coordinateSpace;
@property(copy) NSString *role;
@property NSRect bounds;
@end
@implementation UIWindowScene
@end
@interface UIApplication : NSObject
@property(copy) NSSet *connectedScenes;
+ (instancetype)sharedApplication;
@end
@implementation UIApplication
+ (instancetype)sharedApplication { static UIApplication *app; if(!app)app=[self new]; return app; }
@end
@interface ProbeView : NSObject
@property BOOL hidden;
@end
@implementation ProbeView
@end
@interface HaloBenchmarkOverlayController : NSObject
@property(strong) NSTimer *timer;
@property(strong) ProbeView *view;
- (void)refreshProgress;
@end
@implementation HaloBenchmarkOverlayController
- (instancetype)init { if((self=[super init]))self.view=[ProbeView new]; return self; }
- (void)refreshProgress {}
@end
static int creations;
@interface HaloBenchmarkOverlayWindow : NSObject
@property NSRect frame;
@property double windowLevel;
@property BOOL hidden;
@property(strong) HaloBenchmarkOverlayController *rootViewController;
- (instancetype)initWithWindowScene:(UIWindowScene *)scene;
- (void)makeKeyAndVisible;
@end
@implementation HaloBenchmarkOverlayWindow
- (instancetype)initWithWindowScene:(UIWindowScene *)scene { creations++; return [super init]; }
- (void)makeKeyAndVisible { self.hidden=NO; }
@end

/* The Python harness inserts the production functions here. */
// PRODUCTION_OVERLAY

static UIWindowScene *scene(NSInteger state, NSString *role) {
    UIWindowScene *value=[UIWindowScene new];
    value.activationState=state;
    value.session=value;
    value.coordinateSpace=value;
    value.role=role;
    return value;
}
static void activate(UIWindowScene *value) {
    value.activationState=UISceneActivationStateForegroundActive;
    [NSNotificationCenter.defaultCenter postNotificationName:UISceneDidActivateNotification object:value];
}
int main(void) {
    @autoreleasepool {
        /* SDL's startup scene is connected but can still be foreground-inactive. */
        UIWindowScene *game=scene(UISceneActivationStateForegroundInactive,UIWindowSceneSessionRoleApplication);
        UIApplication.sharedApplication.connectedScenes=[NSSet setWithObject:game];
        host_benchmark_overlay_show(1);
        assert(benchmark_overlay && !benchmark_overlay.hidden && creations==1);
        activate(game);
        assert(creations==1);
        host_benchmark_overlay_presented(1);
        assert(benchmark_overlay.rootViewController.view.hidden);
        host_benchmark_overlay_presented(0);
        assert(!benchmark_overlay.rootViewController.view.hidden);
        host_benchmark_overlay_show(0);
        assert(!benchmark_overlay && !benchmark_overlay_activation);

        /* A request made before a usable scene exists survives until activation. */
        game.activationState=UISceneActivationStateBackground;
        host_benchmark_overlay_show(1);
        assert(!benchmark_overlay && benchmark_overlay_activation);
        activate(game);
        assert(benchmark_overlay && creations==2);
        host_benchmark_overlay_show(0);

        UIApplication.sharedApplication.connectedScenes=[NSSet set];
        host_benchmark_overlay_show(1);
        assert(!benchmark_overlay && benchmark_overlay_activation);
        /* Finishing before activation must cancel the pending request. */
        host_benchmark_overlay_show(0);
        UIApplication.sharedApplication.connectedScenes=[NSSet setWithObject:game];
        activate(game);
        assert(!benchmark_overlay && creations==2);
        UIApplication.sharedApplication.connectedScenes=[NSSet setWithObject:scene(
            UISceneActivationStateForegroundActive,@"external display")];
        host_benchmark_overlay_show(1);
        assert(!benchmark_overlay);
        host_benchmark_overlay_show(0);
    }
    return 0;
}
