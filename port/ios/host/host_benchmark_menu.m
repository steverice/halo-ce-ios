/* The tester build's first screen (tools/ios_build.py --tester): Play or Benchmark; and, when the last benchmark was cut off (a crash, or
   the system ending the app), the question whether to carry on. Buttons a Siri
   Remote's focus, a controller or a touch can press. Ordinary builds do not
   compile this in: there the benchmark is debug.benchmark_suite's alone. */
#import <UIKit/UIKit.h>
#include "ios_host.h"
#include "host_benchmark.h"
#include "benchmark_plan.h"
#import "host_data_updates.h"

#if !__has_feature(objc_arc)
#error The benchmark menu requires Objective-C ARC.
#endif

@interface HaloBenchmarkMenuController : UIViewController
@property(nonatomic, copy) NSString *heading, *detail;
@property(nonatomic, copy) NSArray<NSString *> *choices;
@property(nonatomic) NSInteger chosen;
@property(nonatomic) BOOL launchMenu;
@property(nonatomic, strong) UIButton *playButton, *benchmarkButton;
@property(nonatomic, strong) UILabel *detailLabel, *benchmarkStatus;
@property(nonatomic, strong) UIActivityIndicatorView *benchmarkSpinner;
- (void)refreshBenchmark;
@end

static NSString *delivery_status;
/* The background sender and the benchmark can both publish a delivery result. The
   controller samples this string only on the main thread. */
void host_benchmark_menu_status(const char *message) {
    NSString *status = message ? @(message) : nil;
    if (NSThread.isMainThread) delivery_status = status;
    else dispatch_async(dispatch_get_main_queue(), ^{ delivery_status = status; });
}

static void retry_pending_reports(void) {
#ifdef HALO_BENCHMARK_CLOUD
    static BOOL attempted_pending;
    if (attempted_pending) return;
    attempted_pending = YES;
    /* CloudKit delivery can wait twelve seconds. Start only after presentation,
       on a worker, so even a pending outbox cannot delay Play. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            char message[256] = {0};
            const char *saved = [NSFileManager.defaultManager fileExistsAtPath:@"benchmark/report.json"] ?
                "benchmark/report.json" : NULL;
            host_benchmark_send(saved, NULL, message, sizeof(message));
            if (message[0]) host_benchmark_menu_status(message);
        }
    });
#endif
}

@implementation HaloBenchmarkMenuController
- (void)viewDidLoad {
    UIStackView *stack = [UIStackView new];
    UILabel *title = [UILabel new], *text = [UILabel new];
    NSInteger index = 0;

    [super viewDidLoad];
    self.chosen = -1;
    self.view.backgroundColor = UIColor.blackColor;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 24;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    title.text = self.heading;
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    title.textColor = UIColor.whiteColor;
    [stack addArrangedSubview:title];
    self.detailLabel = text;
    text.text = self.detail;
    text.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    text.textColor = UIColor.lightGrayColor;
    text.numberOfLines = 0;
    text.textAlignment = NSTextAlignmentCenter;
    [stack addArrangedSubview:text];
    UIStackView *buttons=[UIStackView new];
    buttons.axis=UILayoutConstraintAxisHorizontal; buttons.spacing=36;
    [stack addArrangedSubview:buttons];
    for (NSString *choice in self.choices) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];

        [button setTitle:choice forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
        button.tag = index++;
        if (button.tag == 0) self.playButton = button;
        if (self.launchMenu && button.tag == 1) self.benchmarkButton = button;
        [button addTarget:self action:@selector(pressed:) forControlEvents:UIControlEventPrimaryActionTriggered];
        [buttons addArrangedSubview:button];
    }
    if (self.launchMenu) {
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(menuBecameActive:)
            name:UIApplicationDidBecomeActiveNotification object:nil];
        UIStackView *status = [UIStackView new];
        status.axis = UILayoutConstraintAxisHorizontal;
        status.alignment = UIStackViewAlignmentCenter;
        status.spacing = 10;
        self.benchmarkSpinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        self.benchmarkSpinner.color = UIColor.whiteColor;
        self.benchmarkSpinner.hidesWhenStopped = YES;
        self.benchmarkStatus = [UILabel new];
        self.benchmarkStatus.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
        self.benchmarkStatus.textColor = UIColor.lightGrayColor;
        self.benchmarkStatus.numberOfLines = 0;
        self.benchmarkStatus.textAlignment = NSTextAlignmentCenter;
        [status addArrangedSubview:self.benchmarkSpinner];
        [status addArrangedSubview:self.benchmarkStatus];
        [stack addArrangedSubview:status];
        [self refreshBenchmark];
    }
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor multiplier:0.7]]];
}
- (NSArray<id<UIFocusEnvironment>> *)preferredFocusEnvironments {
    return self.playButton ? @[ self.playButton ] : [super preferredFocusEnvironments];
}
- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}
- (void)menuBecameActive:(NSNotification *)notification {
    (void)notification;
    UIWindow *window = self.viewIfLoaded.window;
    if (!self.launchMenu || self.chosen >= 0 || !window || window.hidden) return;
    host_data_updates_menu_start();
    [self refreshBenchmark];
}
- (void)refreshBenchmark {
    if (!self.launchMenu) return;
    NSDictionary *state = host_data_updates_menu_state();
    BOOL ready = [state[@"ready"] boolValue], loading = [state[@"loading"] boolValue];
    NSString *reason = state[@"reason"];
    BOOL wasEnabled = self.benchmarkButton.enabled;
    self.benchmarkButton.enabled = ready;
    NSString *status = ready ? @"" : loading ? @"Downloading benchmark data…" :
        reason.length ? reason : @"Benchmark data is unavailable";
    self.benchmarkStatus.text = status;
    self.benchmarkButton.accessibilityLabel = ready ? @"Benchmark" : [@"Benchmark. " stringByAppendingString:status];
    self.benchmarkButton.accessibilityTraits = UIAccessibilityTraitButton | (ready ? 0 : UIAccessibilityTraitNotEnabled);
    if (!ready && loading) [self.benchmarkSpinner startAnimating];
    else [self.benchmarkSpinner stopAnimating];
    self.detailLabel.text = delivery_status.length ? [self.detail stringByAppendingFormat:@"\n\n%@", delivery_status] : self.detail;
    if (wasEnabled && !ready) [self setNeedsFocusUpdate];
}
- (void)pressed:(UIButton *)button {
    /* Controller primary actions must honor the same gate as touch and focus. */
    if (self.launchMenu && button.tag == 1) [self refreshBenchmark];
    if (button.enabled) self.chosen = button.tag;
}
@end

/* shows a question and waits for its answer: the index of the choice pressed */
static NSInteger ask(NSString *heading, NSString *detail, NSArray<NSString *> *choices, BOOL launchMenu) {
    UIWindowScene *scene = nil;
    HaloBenchmarkMenuController *controller = [HaloBenchmarkMenuController new];
    UIWindow *window;
    NSInteger chosen;

    for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes)
        if ([candidate isKindOfClass:UIWindowScene.class]) { scene = (UIWindowScene *)candidate; break; }
    if (!scene) return 0;
    controller.heading = heading;
    controller.detail = detail;
    controller.choices = choices;
    controller.launchMenu = launchMenu;
    window = [[UIWindow alloc] initWithWindowScene:scene];
    window.frame = scene.coordinateSpace.bounds;
    window.windowLevel = UIWindowLevelNormal + 2;
    window.rootViewController = controller;
    [window makeKeyAndVisible];
    if (launchMenu) retry_pending_reports();
    while (controller.chosen < 0) {
        @autoreleasepool {
            [controller refreshBenchmark];
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
#if !TARGET_OS_TV
            CFRunLoopRunInMode((__bridge CFStringRef)UITrackingRunLoopMode, 0.01, true);
#endif
        }
    }
    chosen = controller.chosen;
    window.hidden = YES;
    window.rootViewController = nil;
    return chosen;
}

const char *host_benchmark_menu(void) {
    for (;;) {
        /* Menu setup is memory-only; validation and downloads run on workers. */
        host_data_updates_menu_start();
        NSString *detail = @"The benchmark plays the game by itself and measures this device. Finish at any time. "
            "Benchmark data uploads automatically through iCloud.";
        if (ask(@"VISR", detail, @[ @"Play", @"Benchmark" ], YES) == 0) {
            host_data_updates_menu_finish();
            return NULL;
        }

        /* A cut-off suite is offered only after choosing a ready Benchmark. No
           catalog or journal disk reads belong before the initial menu. */
        struct benchmark_journal journal;
        benchmark_journal_load("benchmark/journal.txt", &journal);
        if (journal.valid && !journal.finished && journal.count > 0) {
            NSString *detail = [NSString stringWithFormat:@"The %s benchmark stopped %@ run %d. Carrying on skips nothing "
                "that already finished.", benchmark_mode_name(journal.mode), journal.cut_off ? @"during" : @"after", journal.count];
            if (ask(@"Continue the benchmark?", detail, @[ @"Continue", @"Not now" ], NO) != 0) continue;
            host_data_updates_menu_finish();
            host_logf(HOST_LOG_INFO, "benchmark menu: carrying on the %s suite", benchmark_mode_name(journal.mode));
            return benchmark_mode_name(journal.mode);
        }
        host_data_updates_menu_finish();
        host_logf(HOST_LOG_INFO, "benchmark menu: unified");
        return "full-unified";
    }
}
