/* The tester build's first screen (tools/ios_build.py --tester): Play or Benchmark; and, when the last benchmark was cut off (a crash, or
   the system ending the app), the question whether to carry on. Buttons a Siri
   Remote's focus, a controller or a touch can press. Ordinary builds do not
   compile this in: there the benchmark is debug.benchmark_suite's alone. */
#import <UIKit/UIKit.h>
#include "ios_host.h"
#include "host_benchmark.h"
#include "benchmark_plan.h"
#import "host_benchmark_catalog.h"
#import "host_data_updates.h"

#if !__has_feature(objc_arc)
#error The benchmark menu requires Objective-C ARC.
#endif

@interface HaloBenchmarkMenuController : UIViewController
@property(nonatomic, copy) NSString *heading, *detail;
@property(nonatomic, copy) NSArray<NSString *> *choices;
@property(nonatomic) NSInteger chosen;
@end

static NSString *delivery_status;
void host_benchmark_menu_status(const char *message) { delivery_status=message ? @(message) : nil; }

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
    UIStackView *buttons=[UIStackView new];
    buttons.axis=UILayoutConstraintAxisHorizontal; buttons.spacing=36;
    [stack addArrangedSubview:buttons];
    for (NSString *choice in self.choices) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];

        [button setTitle:choice forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
        button.tag = index++;
        [button addTarget:self action:@selector(pressed:) forControlEvents:UIControlEventPrimaryActionTriggered];
        [buttons addArrangedSubview:button];
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
    NSString *bundled = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"benchmark-recordings"];
    NSString *selected = host_recording_update_folder(bundled);
    NSUInteger choice_count = host_benchmark_menu_choice_count(selected);
    BOOL ready = choice_count == 2;

    /* a benchmark that was cut off comes first: the app cannot reopen itself, so this is where it carries on */
    benchmark_journal_load("benchmark/journal.txt", &journal);
    /* (a journal written for other scenes, by another build, is started over by host_benchmark_start) */
    if (ready && journal.valid && !journal.finished && journal.count > 0) {
        NSString *detail = [NSString stringWithFormat:@"The %s benchmark stopped %@ run %d. Carrying on skips nothing "
            "that already finished.", benchmark_mode_name(journal.mode), journal.cut_off ? @"during" : @"after",
            journal.cut_off ? journal.count : journal.count];

        if (ask(@"Continue the benchmark?", detail, @[ @"Continue", @"Not now" ]) == 0) {
            host_logf(HOST_LOG_INFO, "benchmark menu: carrying on the %s suite", benchmark_mode_name(journal.mode));
            return benchmark_mode_name(journal.mode);
        }
    }
#ifdef HALO_BENCHMARK_CLOUD
    static BOOL attempted_pending;
    if(!attempted_pending) {
        char message[256];
        const char *saved=[NSFileManager.defaultManager fileExistsAtPath:@"benchmark/report.json"] ? "benchmark/report.json" : NULL;
        host_benchmark_send(saved,NULL,message,sizeof(message));
        if(message[0])delivery_status=@(message);
    }
    attempted_pending=YES;
#endif
    NSString *detail = ready ?
        @"The benchmark plays the game by itself and measures this device. Finish at any time. Benchmark data uploads automatically through iCloud." :
        @"Benchmark data is unavailable on this launch. VISR is checking for the signed download. Play now and try Benchmark after reopening VISR.";
    if(delivery_status.length)detail=[detail stringByAppendingFormat:@"\n\n%@",delivery_status];
    switch (ask(@"VISR", detail, choice_count == 2 ? @[ @"Play", @"Benchmark" ] : @[ @"Play" ])) {
    case 1: host_logf(HOST_LOG_INFO, "benchmark menu: unified"); return "full-unified";
    default: return NULL;
    }
}
