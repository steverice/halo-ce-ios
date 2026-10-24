/* Exercise the real UIKit controller with a feed that completes after presentation. */
#import <UIKit/UIKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#import "../host/host_benchmark_menu.m"

static NSDictionary *feed_state;
static int menu_starts;
static atomic_bool sender_started;
static dispatch_semaphore_t sender_gate;
static void check(BOOL condition, const char *message);
static HaloBenchmarkMenuController *visible_menu(void) {
    for (UIWindowScene *scene in UIApplication.sharedApplication.connectedScenes)
        if ([scene isKindOfClass:UIWindowScene.class])
            for (UIWindow *window in scene.windows)
                if (!window.hidden && window.windowLevel > UIWindowLevelNormal &&
                    [window.rootViewController isKindOfClass:HaloBenchmarkMenuController.class])
                    return (HaloBenchmarkMenuController *)window.rootViewController;
    return nil;
}
int host_benchmark_send(const char *report_path, const char *summary, char *message, unsigned long size) {
    check(!NSThread.isMainThread, "Pending iCloud delivery runs off the presentation thread");
    dispatch_sync(dispatch_get_main_queue(), ^{
        check(visible_menu() != nil, "Launch menu is visible before pending delivery begins");
    });
    atomic_store(&sender_started, true);
    check(dispatch_semaphore_wait(sender_gate, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) == 0,
        "Play returns while pending iCloud delivery is still waiting");
    snprintf(message, size, "Pending upload retried");
    return 1;
}
NSDictionary *host_data_updates_menu_state(void) { return feed_state; }
void host_data_updates_menu_start(void) {
    menu_starts++;
    if (![feed_state[@"ready"] boolValue]) feed_state = @{ @"ready": @NO, @"loading": @YES, @"reason": @"" };
}
void host_data_updates_menu_finish(void) {}
void host_logf(int priority, const char *format, ...) {}
void benchmark_journal_load(const char *path, struct benchmark_journal *journal) { memset(journal, 0, sizeof(*journal)); }
const char *benchmark_mode_name(enum benchmark_mode mode) { return "full-unified"; }
NSString *host_recording_update_folder(NSString *bundled) { return bundled; }
NSUInteger host_benchmark_menu_choice_count(NSString *folder) { return 2; }
static void check(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static UIButton *button(UIView *view, NSInteger tag) {
    if ([view isKindOfClass:UIButton.class] && view.tag == tag) return (UIButton *)view;
    for (UIView *child in view.subviews) { UIButton *found = button(child, tag); if (found) return found; }
    return nil;
}
@interface HaloBenchmarkMenuController (ProbeRefresh)
- (void)refreshBenchmark;
@end
static void refresh(HaloBenchmarkMenuController *controller) {
    SEL selector = NSSelectorFromString(@"refreshBenchmark");
    if ([controller respondsToSelector:selector]) [controller refreshBenchmark];
}
@interface ProbeDelegate : UIResponder <UIApplicationDelegate, UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation ProbeDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options { return YES; }
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    feed_state = @{ @"ready": @NO, @"loading": @YES, @"reason": @"" };
    HaloBenchmarkMenuController *controller = [HaloBenchmarkMenuController new];
    controller.heading = @"VISR"; controller.detail = @"Play while benchmark data downloads.";
    controller.choices = @[ @"Play", @"Benchmark" ];
    if ([controller respondsToSelector:NSSelectorFromString(@"setLaunchMenu:")]) [controller setValue:@YES forKey:@"launchMenu"];
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    dispatch_async(dispatch_get_main_queue(), ^{
        UIButton *play = button(controller.view, 0), *benchmark = button(controller.view, 1);
        check(play.enabled, "Play is usable during download");
        check(benchmark && !benchmark.enabled, "Benchmark is visible and disabled during download");
        UIActivityIndicatorView *spinner = [controller valueForKey:@"benchmarkSpinner"];
        check(spinner.isAnimating, "Pending download shows a spinner");
        check((benchmark.accessibilityTraits & UIAccessibilityTraitNotEnabled) != 0, "VoiceOver announces unavailable Benchmark");
        check([controller.preferredFocusEnvironments.firstObject isEqual:play], "Initial focus lands on Play");
        [controller pressed:benchmark];
        check(controller.chosen == -1, "Controller activation cannot select pending Benchmark");
        feed_state = @{ @"ready": @YES, @"loading": @NO, @"reason": @"" };
        refresh(controller);
        check(benchmark.enabled, "Benchmark becomes usable without reopening the menu");
        check(!spinner.isAnimating && spinner.hidden, "Readiness removes the spinner");
        check((benchmark.accessibilityTraits & UIAccessibilityTraitNotEnabled) == 0, "VoiceOver availability updates on readiness");
        [controller pressed:benchmark];
        check(controller.chosen == 1, "Ready Benchmark can be selected");
        controller.chosen = -1;
        feed_state = @{ @"ready": @NO, @"loading": @NO, @"reason": @"Can't reach the profile server" };
        refresh(controller);
        check(!benchmark.enabled, "Failed download keeps Benchmark disabled");
        UILabel *status = [controller valueForKey:@"benchmarkStatus"];
        check([status.text containsString:@"Can't reach"], "Failed download shows a reason");
        check(!spinner.isAnimating, "Failed download stops the spinner");
        [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
        check(spinner.isAnimating && !benchmark.enabled, "Returning to the visible menu retries a failed download");
        [controller pressed:play];
        check(controller.chosen == 0, "Play works while retry is pending");
        int starts = menu_starts;
        [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
        check(menu_starts == starts, "A completed menu does not restart downloads");
        controller.chosen = -1;
        self.window.hidden = YES;
        [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
        check(menu_starts == starts, "A hidden menu does not restart downloads");
        self.window.hidden = NO;
        controller.chosen = 0;
        HaloBenchmarkMenuController *question = [HaloBenchmarkMenuController new];
        question.heading = @"Continue the benchmark?";
        question.choices = @[ @"Continue", @"Not now" ];
        [question loadViewIfNeeded];
        [question pressed:button(question.view, 1)];
        check(question.chosen == 1, "Generic continue question remains usable while feeds load");
        [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
        check(menu_starts == starts, "Generic questions do not restart downloads");
        sender_gate = dispatch_semaphore_create(0);
        [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:NO block:^(NSTimer *timer) {
            __block BOOL pressedWhileSending = NO;
            NSTimer *press = [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:YES block:^(NSTimer *timer) {
                HaloBenchmarkMenuController *menu = visible_menu();
                if (!menu || !atomic_load(&sender_started)) return;
                check(menu.playButton.enabled, "Play remains usable while iCloud delivery waits");
                pressedWhileSending = YES;
                [menu pressed:menu.playButton];
                [timer invalidate];
            }];
            const char *mode = host_benchmark_menu();
            [press invalidate];
            check(pressedWhileSending && mode == NULL, "Play finishes before pending iCloud delivery");
            dispatch_semaphore_signal(sender_gate);
            CFAbsoluteTime until = CFAbsoluteTimeGetCurrent() + 2;
            do {
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
                refresh(controller);
            } while (![controller.detailLabel.text containsString:@"Pending upload retried"] && CFAbsoluteTimeGetCurrent() < until);
            check([controller.detailLabel.text containsString:@"Pending upload retried"], "Delivery status repaints after background completion");
            puts("benchmark menu probe: passed");
            exit(0);
        }];
    });
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(ProbeDelegate.class)); }
}
