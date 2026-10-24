/* The tester build's first screen (tools/ios_build.py --tester): Play or Benchmark; and, when the last benchmark was cut off (a crash, or
   the system ending the app), the question whether to carry on. Buttons a Siri
   Remote's focus, a controller or a touch can press. Ordinary builds do not
   compile this in: there the benchmark is debug.benchmark_suite's alone. */
#import <UIKit/UIKit.h>
#import <CoreText/CoreText.h>
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
#if TARGET_OS_VISION
@property(nonatomic, strong) UILabel *disabledBenchmarkTitle;
#endif
@property(nonatomic, strong) UIActivityIndicatorView *benchmarkSpinner;
@property(nonatomic, strong) UIStackView *brand, *menuContent, *buttons;
@property(nonatomic, strong) UIControl *splashShield;
@property(nonatomic, strong) UIImageView *brandIcon;
@property(nonatomic, strong) UILabel *brandTitle;
@property(nonatomic, strong) UIStackView *layoutStack;
@property(nonatomic, strong) NSLayoutConstraint *iconWidth, *contentWidth;
@property(nonatomic) CGFloat layoutScale;
@property(nonatomic) CFAbsoluteTime splashStarted;
@property(nonatomic) BOOL splashVisible, skippingPress;
- (void)refreshBenchmark;
- (void)endSplash:(NSString *)reason;
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

static UIColor *brandColor(unsigned rgb) {
    return [UIColor colorWithRed:((rgb >> 16) & 255) / 255.0 green:((rgb >> 8) & 255) / 255.0
        blue:(rgb & 255) / 255.0 alpha:1];
}

static CGFloat menuScale(UITraitCollection *traits) {
#if TARGET_OS_MACCATALYST
    /* Catalyst can report the iPad idiom; only an actual iPad gets the larger layout. */
    return 1;
#else
    return traits.userInterfaceIdiom == UIUserInterfaceIdiomPad &&
        traits.horizontalSizeClass == UIUserInterfaceSizeClassRegular &&
        traits.verticalSizeClass == UIUserInterfaceSizeClassRegular ? 1.75 : 1;
#endif
}

static UIFont *menuFont(UIFontTextStyle style, UITraitCollection *traits, CGFloat scale) {
    UIFont *font = [UIFont preferredFontForTextStyle:style compatibleWithTraitCollection:traits];
    return [font fontWithSize:font.pointSize * scale];
}

@implementation HaloBenchmarkMenuController
- (void)viewDidLoad {
    UIStackView *stack = [UIStackView new];
    UILabel *title = [UILabel new], *text = [UILabel new];
    NSInteger index = 0;

    [super viewDidLoad];
    self.chosen = -1;
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    self.view.backgroundColor = brandColor(0x081216);
    self.splashStarted = CFAbsoluteTimeGetCurrent();
    self.splashVisible = self.launchMenu;
    self.layoutStack = stack;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 24;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor]]];
    [scroll addSubview:stack];
    if (self.launchMenu) {
        NSURL *fontURL = [NSBundle.mainBundle URLForResource:@"Orbitron" withExtension:@"ttf"];
        if (fontURL) CTFontManagerRegisterFontsForURL((__bridge CFURLRef)fontURL, kCTFontManagerScopeProcess, NULL);
        self.brand = [UIStackView new];
        self.brand.axis = UILayoutConstraintAxisVertical;
        self.brand.alignment = UIStackViewAlignmentCenter;
        self.brand.spacing = 16;
        UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"VISRIcon"]];
        icon.contentMode = UIViewContentModeScaleAspectFit;
        icon.isAccessibilityElement = NO;
        icon.clipsToBounds = YES;
#if TARGET_OS_VISION
        icon.layer.cornerCurve = kCACornerCurveCircular;
#else
        icon.layer.cornerCurve = kCACornerCurveContinuous;
#endif
        self.brandIcon = icon;
        self.brandTitle = title;
        [self.brand addArrangedSubview:icon];
        self.iconWidth = [icon.widthAnchor constraintEqualToConstant:104];
        self.iconWidth.active = YES;
        [icon.heightAnchor constraintEqualToAnchor:icon.widthAnchor].active = YES;
        [self.brand addArrangedSubview:title];
        [stack addArrangedSubview:self.brand];
    }
    title.text = self.heading;
    title.numberOfLines = 0;
    title.textAlignment = NSTextAlignmentCenter;
    title.adjustsFontForContentSizeCategory = YES;
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    title.textColor = brandColor(0xe8f6fa);
    if (self.launchMenu) {
        UIFont *font = [UIFont fontWithName:@"Orbitron-Regular" size:36] ?: [UIFont systemFontOfSize:36];
        title.attributedText = [[NSAttributedString alloc] initWithString:@"VISR"
            attributes:@{NSFontAttributeName:font, NSKernAttributeName:@2.16}];
        self.menuContent = [UIStackView new];
        self.menuContent.axis = UILayoutConstraintAxisVertical;
        self.menuContent.alignment = UIStackViewAlignmentCenter;
        self.menuContent.spacing = 20;
        self.menuContent.alpha = 0;
        self.menuContent.accessibilityElementsHidden = YES;
        [stack addArrangedSubview:self.menuContent];
    } else [stack addArrangedSubview:title];
    UIStackView *content = self.menuContent ?: stack;
    self.detailLabel = text;
    text.text = self.detail;
    text.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    text.textColor = brandColor(0x93adb6);
    text.adjustsFontForContentSizeCategory = YES;
    text.numberOfLines = 0;
    text.textAlignment = NSTextAlignmentCenter;
    UIStackView *buttons=[UIStackView new];
    self.buttons = buttons;
    buttons.axis=UILayoutConstraintAxisVertical; buttons.spacing=16;
    buttons.alignment = UIStackViewAlignmentFill;
    [content addArrangedSubview:buttons];
    for (NSString *choice in self.choices) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];

        [button setTitle:choice forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
        button.titleLabel.adjustsFontForContentSizeCategory = YES;
        UIButtonConfiguration *configuration = index == 0 ? UIButtonConfiguration.filledButtonConfiguration : UIButtonConfiguration.borderedButtonConfiguration;
        configuration.baseBackgroundColor = index == 0 ? brandColor(0x3fd6ff) : brandColor(0x1d3640);
        configuration.baseForegroundColor = index == 0 ? brandColor(0x081216) : brandColor(0xe8f6fa);
        configuration.contentInsets = NSDirectionalEdgeInsetsMake(14, 32, 14, 32);
        button.configuration = configuration;
        BOOL branded = self.launchMenu;
        button.configurationUpdateHandler = ^(UIButton *updated) {
            UIButtonConfiguration *style = updated.configuration;
            style.baseForegroundColor = !updated.enabled ? brandColor(0x93adb6) :
                updated.focused ? brandColor(0x081216) : updated.tag == 0 ? brandColor(0x081216) : brandColor(0xe8f6fa);
            style.baseBackgroundColor = updated.focused || updated.tag == 0 ? brandColor(0x3fd6ff) : brandColor(0x1d3640);
            UIColor *ink = style.baseForegroundColor;
#if TARGET_OS_VISION
            if (branded && updated.tag == 1 && !updated.enabled) ink = UIColor.clearColor;
#endif
            CGFloat scale = branded ? menuScale(updated.traitCollection) : 1;
            UIFont *font = menuFont(UIFontTextStyleHeadline, updated.traitCollection, scale);
            style.contentInsets = NSDirectionalEdgeInsetsMake(14 * scale, 32 * scale, 14 * scale, 32 * scale);
#if TARGET_OS_VISION
            /* visionOS dims disabled configuration colors again unless their transformers are explicit. */
            style.background.backgroundColor = style.baseBackgroundColor;
            style.background.backgroundColorTransformer = ^UIColor *(UIColor *color) { return color; };
            style.background.strokeColor = brandColor(0x1d3640);
            style.background.strokeWidth = 1;
#endif
            style.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey,id> *(NSDictionary<NSAttributedStringKey,id> *attributes) {
                NSMutableDictionary *result = attributes.mutableCopy;
                result[NSForegroundColorAttributeName] = ink;
                result[NSFontAttributeName] = font;
                return result;
            };
            updated.configuration = style;
        };
        button.tag = index++;
        if (button.tag == 0) self.playButton = button;
        if (self.launchMenu && button.tag == 1) {
            self.benchmarkButton = button;
#if TARGET_OS_VISION
            /* The system's disabled vibrancy multiplies title opacity by 0.25.
               Keep the disabled label outside that effect so muted ink stays legible. */
            self.disabledBenchmarkTitle = [UILabel new];
            self.disabledBenchmarkTitle.text = choice;
            self.disabledBenchmarkTitle.textColor = brandColor(0x93adb6);
            self.disabledBenchmarkTitle.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
            self.disabledBenchmarkTitle.adjustsFontForContentSizeCategory = YES;
            self.disabledBenchmarkTitle.isAccessibilityElement = NO;
            self.disabledBenchmarkTitle.translatesAutoresizingMaskIntoConstraints = NO;
            [button addSubview:self.disabledBenchmarkTitle];
            [NSLayoutConstraint activateConstraints:@[
                [self.disabledBenchmarkTitle.centerXAnchor constraintEqualToAnchor:button.centerXAnchor],
                [self.disabledBenchmarkTitle.centerYAnchor constraintEqualToAnchor:button.centerYAnchor]]];
#endif
        }
        [button addTarget:self action:@selector(pressed:) forControlEvents:UIControlEventPrimaryActionTriggered];
        [buttons addArrangedSubview:button];
    }
    [content addArrangedSubview:text];
    if (self.launchMenu) {
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(menuBecameActive:)
            name:UIApplicationDidBecomeActiveNotification object:nil];
        UIStackView *status = [UIStackView new];
        status.axis = UILayoutConstraintAxisHorizontal;
        status.alignment = UIStackViewAlignmentCenter;
        status.spacing = 10;
        self.benchmarkSpinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        self.benchmarkSpinner.color = brandColor(0x93adb6);
        self.benchmarkSpinner.hidesWhenStopped = YES;
        self.benchmarkStatus = [UILabel new];
        self.benchmarkStatus.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
        self.benchmarkStatus.textColor = brandColor(0x93adb6);
        self.benchmarkStatus.adjustsFontForContentSizeCategory = YES;
        self.benchmarkStatus.numberOfLines = 0;
        self.benchmarkStatus.textAlignment = NSTextAlignmentCenter;
        [status addArrangedSubview:self.benchmarkSpinner];
        [status addArrangedSubview:self.benchmarkStatus];
        [content addArrangedSubview:status];
        [self refreshBenchmark];
    }
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.centerYAnchor],
        [stack.topAnchor constraintGreaterThanOrEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:24],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-24],
        [scroll.contentLayoutGuide.heightAnchor constraintGreaterThanOrEqualToAnchor:scroll.frameLayoutGuide.heightAnchor],
        [scroll.contentLayoutGuide.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
        (self.contentWidth = [stack.widthAnchor constraintLessThanOrEqualToConstant:480]),
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:24],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-24]]];
    NSLayoutConstraint *preferredHeight = [scroll.contentLayoutGuide.heightAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.heightAnchor];
    preferredHeight.priority = UILayoutPriorityDefaultLow;
    preferredHeight.active = YES;
    if (self.splashVisible) {
        self.splashShield = [UIControl new];
        self.splashShield.accessibilityLabel = @"VISR. Show menu";
        self.splashShield.isAccessibilityElement = YES;
        self.splashShield.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        self.splashShield.frame = self.view.bounds;
        [self.splashShield addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(skipSplash)]];
        [self.splashShield addTarget:self action:@selector(skipSplash) forControlEvents:UIControlEventPrimaryActionTriggered];
        [self.view addSubview:self.splashShield];
        __weak HaloBenchmarkMenuController *weak = self;
        [NSTimer scheduledTimerWithTimeInterval:2 repeats:NO block:^(NSTimer *timer) { [weak endSplash:@"timeout"]; }];
    }
}
#if !TARGET_OS_TV && !TARGET_OS_VISION
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskAllButUpsideDown; }
#endif
- (void)viewWillLayoutSubviews {
    [super viewWillLayoutSubviews];
    if (self.launchMenu) {
        CGFloat scale = menuScale(self.traitCollection);
        CGFloat brandScale = scale;
#if TARGET_OS_TV
        brandScale = 2;
#endif
        self.iconWidth.constant = 104 * brandScale;
#if TARGET_OS_VISION
        self.brandIcon.layer.cornerRadius = self.iconWidth.constant / 2;
#else
        self.brandIcon.layer.cornerRadius = self.iconWidth.constant * 0.225;
#endif
        self.brand.spacing = 16 * brandScale;
        UIFont *font = [UIFont fontWithName:@"Orbitron-Regular" size:36 * brandScale] ?: [UIFont systemFontOfSize:36 * brandScale];
        self.brandTitle.attributedText = [[NSAttributedString alloc] initWithString:@"VISR"
            attributes:@{NSFontAttributeName:font, NSKernAttributeName:@(2.16 * brandScale)}];
        self.layoutStack.spacing = 24 * scale;
        self.menuContent.spacing = 20 * scale;
        self.buttons.spacing = 16 * scale;
        self.contentWidth.constant = 480 * scale;
#if TARGET_OS_VISION
        self.disabledBenchmarkTitle.font = menuFont(UIFontTextStyleHeadline, self.traitCollection, scale);
#endif
        self.detailLabel.font = menuFont(UIFontTextStyleBody, self.traitCollection, scale);
        self.benchmarkStatus.font = menuFont(UIFontTextStyleFootnote, self.traitCollection, scale);
        if (self.layoutScale != scale) {
            self.layoutScale = scale;
            for (UIButton *button in self.buttons.arrangedSubviews) [button setNeedsUpdateConfiguration];
        }
    }
    BOOL compact = self.view.bounds.size.height < 500 && self.view.bounds.size.width > self.view.bounds.size.height;
    self.buttons.axis = compact ? UILayoutConstraintAxisHorizontal : UILayoutConstraintAxisVertical;
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (self.splashVisible) {
        self.brand.transform = CGAffineTransformIdentity;
        CGRect frame = [self.brand.superview convertRect:self.brand.frame toView:self.view];
        self.brand.transform = CGAffineTransformMakeTranslation(0, CGRectGetMidY(self.view.bounds) - CGRectGetMidY(frame));
    }
}
- (void)skipSplash { [self endSplash:@"input"]; }
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (self.splashVisible) { self.skippingPress = YES; [self skipSplash]; return; }
    [super pressesBegan:presses withEvent:event];
}
- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (self.skippingPress) { self.skippingPress = NO; return; }
    [super pressesEnded:presses withEvent:event];
}
- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (self.skippingPress) { self.skippingPress = NO; return; }
    [super pressesCancelled:presses withEvent:event];
}
- (void)endSplash:(NSString *)reason {
    if (!self.splashVisible) return;
    self.splashVisible = NO;
    host_logf(HOST_LOG_INFO, "splash ended: %s at %.1f s", reason.UTF8String, CFAbsoluteTimeGetCurrent() - self.splashStarted);
    [self.splashShield removeFromSuperview];
    self.menuContent.accessibilityElementsHidden = NO;
    if (UIAccessibilityIsReduceMotionEnabled()) {
        [UIView transitionWithView:self.view duration:0.35 options:UIViewAnimationOptionTransitionCrossDissolve animations:^{
            self.brand.transform = CGAffineTransformIdentity;
            self.menuContent.alpha = 1;
        } completion:nil];
    } else
    [UIView animateWithDuration:0.35 delay:0 options:UIViewAnimationOptionCurveEaseOut animations:^{
        self.brand.transform = CGAffineTransformIdentity;
        self.menuContent.alpha = 1;
    } completion:nil];
    [self setNeedsFocusUpdate];
    UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, self.playButton);
}
- (NSArray<id<UIFocusEnvironment>> *)preferredFocusEnvironments {
    return self.splashVisible && self.splashShield ? @[ self.splashShield ] : self.playButton ? @[ self.playButton ] : [super preferredFocusEnvironments];
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
    if ([state[@"profileFinished"] boolValue]) [self endSplash:@"download finished"];
    BOOL ready = [state[@"ready"] boolValue], loading = [state[@"loading"] boolValue];
    NSString *reason = state[@"reason"];
    BOOL wasEnabled = self.benchmarkButton.enabled;
    self.benchmarkButton.enabled = ready;
#if TARGET_OS_VISION
    self.disabledBenchmarkTitle.hidden = ready;
#endif
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
    if (self.splashVisible) { [self skipSplash]; return; }
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
#if !TARGET_OS_TV && !TARGET_OS_VISION
    if (@available(iOS 16.0, macCatalyst 16.0, *))
        for (UIWindow *candidate in scene.windows)
            if (!candidate.hidden) [candidate.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
#endif
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
