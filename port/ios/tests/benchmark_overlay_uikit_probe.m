/* Asset-free simulator app: real UIKit runs the production overlay below.
   Only the benchmark's progress source and finish latch are supplied here. */
#import <UIKit/UIKit.h>
#include <assert.h>
#include <stdarg.h>
#include "host_benchmark.h"
#include "host_benchmark_live.h"

#define HOST_LOG_INFO 4
static struct host_benchmark_progress progress = {
    .title = "cost:d40-maw-fight-base", .mode = "Settings cost",
    .number = 1, .total = 3, .offscreen = 1, .measured_fraction = -1,
    .phase = HOST_BENCHMARK_LOADING
};
static int finished;
void host_benchmark_progress(struct host_benchmark_progress *out) { *out = progress; }
void host_benchmark_live(struct benchmark_live_snapshot *out) { memset(out, 0, sizeof(*out)); }
int host_benchmark_finish_requested(void) { return finished; }
void host_benchmark_request_finish(void) { finished = 1; }
static void host_logf(int priority, const char *format, ...) {
    (void)priority;
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    fputc('\n', stderr); fflush(stderr);
}

// PRODUCTION_OVERLAY

static void receipt(NSString *name) {
    NSString *folder = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    assert([name writeToFile:[folder stringByAppendingPathComponent:@"state.txt"]
        atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    fprintf(stderr, "PASS %s\n", name.UTF8String); fflush(stderr);
}

static void checkProgress(NSString *phase) {
    HaloBenchmarkOverlayController *controller = (id)benchmark_overlay.rootViewController;
    assert(controller && !benchmark_overlay.hidden);
    [controller refreshProgress];
    [controller.view layoutIfNeeded];
    assert(!controller.view.hidden && !controller.progressScreen.hidden && controller.panel.hidden);
    assert([controller.progressTitle.text isEqualToString:@"cost:d40-maw-fight-base"]);
    assert([controller.progressDetail.text containsString:phase]);
    for (UIView *label in @[controller.progressTitle, controller.progressDetail, controller.progressFinish]) {
        CGRect rect = [label convertRect:label.bounds toView:controller.view];
        assert(rect.size.width > 0 && rect.size.height > 0);
        assert(CGRectContainsRect(CGRectInset(controller.view.bounds, -1, -1), rect));
    }
    fprintf(stderr, "layout %.0fx%.0f: %s | %s\n", controller.view.bounds.size.width,
        controller.view.bounds.size.height, controller.progressTitle.text.UTF8String,
        controller.progressDetail.text.UTF8String);
}

@interface ProbeScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic) BOOL checkedStartup;
@end
@implementation ProbeScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *base = [UIViewController new];
    base.view.backgroundColor = UIColor.systemBlueColor;
    UILabel *label = [UILabel new];
    label.text = @"Presented check — underlying window";
    label.textColor = UIColor.whiteColor;
    label.numberOfLines = 0; label.frame = CGRectMake(20, 100, 300, 100);
    [base.view addSubview:label];
    self.window.rootViewController = base;
    [self.window makeKeyAndVisible];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    if (self.checkedStartup) return;
    self.checkedStartup = YES;
    /* Allow UIKit's real activation notification to reach the pending request. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        checkProgress(@"Loading");
        HaloBenchmarkOverlayWindow *first = benchmark_overlay;
        host_benchmark_overlay_show(1);
        assert(first == benchmark_overlay);
        receipt(@"loading");
    });
}
- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)contexts {
    (void)scene;
    NSString *command = contexts.anyObject.URL.host;
    HaloBenchmarkOverlayController *controller = (id)benchmark_overlay.rootViewController;
    if ([command isEqualToString:@"measuring"]) {
        progress.phase = HOST_BENCHMARK_MEASURING;
        progress.fraction = 0.5; progress.measured_fraction = 0.5; progress.elapsed = 42;
        checkProgress(@"Measuring");
        assert(!controller.progressBar.hidden && controller.progressBar.progress == 0.5f);
    } else if ([command isEqualToString:@"presented"]) {
        progress.presented = 1; host_benchmark_overlay_presented(1);
        assert(controller.view.hidden && controller.progressScreen.hidden && controller.panel.hidden);
    } else if ([command isEqualToString:@"restored"]) {
        progress.presented = 0; host_benchmark_overlay_presented(0);
        checkProgress(@"Measuring");
    } else if ([command isEqualToString:@"finish"]) {
        CGPoint point = [controller.progressFinish convertPoint:CGPointMake(
            CGRectGetMidX(controller.progressFinish.bounds), CGRectGetMidY(controller.progressFinish.bounds))
            toView:benchmark_overlay];
        UIView *hit = [benchmark_overlay hitTest:point withEvent:nil];
        assert(hit == controller.progressFinish || [hit isDescendantOfView:controller.progressFinish]);
        [controller.progressFinish sendActionsForControlEvents:UIControlEventPrimaryActionTriggered];
        checkProgress(@"Finishing");
        assert(finished && !controller.progressFinish.enabled);
    } else if ([command isEqualToString:@"hidden"]) {
        NSTimer *timer = controller.timer;
        host_benchmark_overlay_show(0);
        assert(!benchmark_overlay && !benchmark_overlay_activation && !timer.valid);
    } else { assert(0 && "unknown probe command"); }
    receipt(command);
}
@end

@interface ProbeApp : UIResponder <UIApplicationDelegate>
@end
@implementation ProbeApp
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)app; (void)options;
    assert(!UIApplication.sharedApplication.connectedScenes.count);
    host_benchmark_overlay_show(1);
    assert(!benchmark_overlay && benchmark_overlay_activation);
    fprintf(stderr, "PASS pending request before scene connection\n");
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)app; (void)options;
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Probe" sessionRole:session.role];
    config.delegateClass = ProbeScene.class;
    return config;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(ProbeApp.class)); }
}
