/* The tester build's first screen (tools/ios_build.py --tester): Play, or Run
   benchmark, quick or full; and, when the last benchmark was cut off (a crash, or
   the system ending the app), the question whether to carry on. Buttons a Siri
   Remote's focus, a controller or a touch can press. Ordinary builds do not
   compile this in: there the benchmark is debug.benchmark_suite's alone. */
#import <UIKit/UIKit.h>
#include "ios_host.h"
#include "host_benchmark.h"
#include "benchmark_plan.h"

#if !__has_feature(objc_arc)
#error The benchmark menu requires Objective-C ARC.
#endif

@interface HaloBenchmarkMenuController : UIViewController
@property(nonatomic, copy) NSString *heading, *detail;
@property(nonatomic, copy) NSArray<NSString *> *choices;
@property(nonatomic) NSInteger chosen;
@end

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
    text.text = self.detail;
    text.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    text.textColor = UIColor.lightGrayColor;
    text.numberOfLines = 0;
    text.textAlignment = NSTextAlignmentCenter;
    [stack addArrangedSubview:text];
    for (NSString *choice in self.choices) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];

        [button setTitle:choice forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
        button.tag = index++;
        [button addTarget:self action:@selector(pressed:) forControlEvents:UIControlEventPrimaryActionTriggered];
        [stack addArrangedSubview:button];
    }
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor multiplier:0.7]]];
}
- (void)pressed:(UIButton *)button {
    self.chosen = button.tag;
}
@end

/* shows a question and waits for its answer: the index of the choice pressed */
static NSInteger ask(NSString *heading, NSString *detail, NSArray<NSString *> *choices) {
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
    window = [[UIWindow alloc] initWithWindowScene:scene];
    window.frame = scene.coordinateSpace.bounds;
    window.windowLevel = UIWindowLevelNormal + 2;
    window.rootViewController = controller;
    [window makeKeyAndVisible];
    while (controller.chosen < 0) {
        @autoreleasepool {
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
    struct benchmark_journal journal;

    /* a benchmark that was cut off comes first: the app cannot reopen itself, so this is where it carries on */
    benchmark_journal_load("benchmark/journal.txt", &journal);
    /* (a journal written for other scenes, by another build, is started over by host_benchmark_start) */
    if (journal.valid && !journal.finished && journal.count > 0) {
        NSString *detail = [NSString stringWithFormat:@"The %s benchmark stopped %@ run %d. Carrying on skips nothing "
            "that already finished.", benchmark_mode_name(journal.mode), journal.cut_off ? @"during" : @"after",
            journal.cut_off ? journal.count : journal.count];

        if (ask(@"Continue the benchmark?", detail, @[ @"Continue", @"Not now" ]) == 0) {
            host_logf(HOST_LOG_INFO, "benchmark menu: carrying on the %s suite", benchmark_mode_name(journal.mode));
            return benchmark_mode_name(journal.mode);
        }
    }
    switch (ask(@"VISR", @"The benchmark plays parts of the game by itself and measures how well this device runs them. "
        "Start it, then leave the device alone until it says it has finished.",
        @[ @"Play", @"Run benchmark: quick (about 10 minutes)", @"Run benchmark: full (about 45 minutes)" ])) {
    case 1: host_logf(HOST_LOG_INFO, "benchmark menu: quick"); return "quick";
    case 2: host_logf(HOST_LOG_INFO, "benchmark menu: full"); return "full";
    default: return NULL;
    }
}
