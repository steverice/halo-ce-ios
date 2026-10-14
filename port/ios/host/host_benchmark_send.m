/* Sending the device benchmark's report to the VISR developers, in builds made
   for it (tools/ios_build.py --benchmark-upload, which signs with the iCloud
   entitlement): first a screen that shows what will be sent and asks, then one
   record in the public database of the app's iCloud container.

   The record holds the report and a few of its fields again as columns to
   search by: the report's code, the model identifier, system and app versions,
   the suite and mode, the start time to the minute, the run count and flags.
   CloudKit adds, and nothing here can remove, the record's times, the opaque
   ID of the iCloud user who created it and an ID it assigns to the device for
   this container; the screen says so. Nothing is
   sent before Send is pressed. */
#import <CloudKit/CloudKit.h>
#import <UIKit/UIKit.h>
#include "ios_host.h"
#include "host_benchmark.h"
#include <zlib.h>

#if !__has_feature(objc_arc)
#error The benchmark's sender requires Objective-C ARC.
#endif

#define RECORD_TYPE @"BenchmarkReport"
#define SEND_SECONDS 90
#define PAGE_LINES 28

/* ---------- what will be sent, as text a person can page through */

/* the report as indented text, with each run's per-frame arrays replaced by their counts: they are thousands of
   numbers a run, sent in full and of no use on a television screen */
static NSString *readable(NSData *report, NSUInteger *frame_values) {
    NSMutableDictionary *whole = [NSJSONSerialization JSONObjectWithData:report options:NSJSONReadingMutableContainers error:nil];
    NSData *text;

    *frame_values = 0;
    if (![whole isKindOfClass:NSDictionary.class]) return nil;
    for (NSMutableDictionary *run in whole[@"runs"])
        for (NSString *name in @[ @"frame_interval", @"frame_cpu", @"frame_gpu" ]) {
            NSString *encoded = run[name];

            if (![encoded isKindOfClass:NSString.class]) continue;
            /* base64 of 16-bit values: three bytes for every four characters, two bytes a value */
            *frame_values += encoded.length * 3 / 8;
            run[name] = [NSString stringWithFormat:@"(%lu frame times, sent in full)", (unsigned long)(encoded.length * 3 / 8)];
        }
    text = [NSJSONSerialization dataWithJSONObject:whole options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil];
    return text ? [[NSString alloc] initWithData:text encoding:NSUTF8StringEncoding] : nil;
}

@interface HaloBenchmarkConsentController : UIViewController
@property(nonatomic, copy) NSString *summary;
@property(nonatomic, copy) NSArray<NSString *> *lines;
@property(nonatomic, strong) UILabel *body, *position;
@property(nonatomic) NSInteger page, answer;     /* page -1: the summary; answer 0 none, 1 send, 2 don't */
@end

@implementation HaloBenchmarkConsentController
- (UIButton *)button:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];

    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:action forControlEvents:UIControlEventPrimaryActionTriggered];
    return button;
}
- (void)viewDidLoad {
    UIStackView *stack = [UIStackView new], *buttons = [UIStackView new];
    UILabel *title = [UILabel new];

    [super viewDidLoad];
    self.page = -1;
    self.view.backgroundColor = UIColor.blackColor;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 16;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    title.text = @"Send this report to the VISR developers?";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    title.textColor = UIColor.whiteColor;
    [stack addArrangedSubview:title];
    self.body = [UILabel new];
    self.body.numberOfLines = 0;
    self.body.textColor = UIColor.lightGrayColor;
    [stack addArrangedSubview:self.body];
    self.position = [UILabel new];
    self.position.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    self.position.textColor = UIColor.grayColor;
    [stack addArrangedSubview:self.position];
    buttons.axis = UILayoutConstraintAxisHorizontal;
    buttons.spacing = 40;
    [buttons addArrangedSubview:[self button:@"Send" action:@selector(send)]];
    [buttons addArrangedSubview:[self button:@"Don't send" action:@selector(decline)]];
    [buttons addArrangedSubview:[self button:@"Show everything: next page" action:@selector(next)]];
    [buttons addArrangedSubview:[self button:@"Back to the summary" action:@selector(first)]];
    [stack addArrangedSubview:buttons];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor multiplier:0.9]]];
    [self show];
}
- (void)show {
    NSInteger pages = ((NSInteger)self.lines.count + PAGE_LINES - 1) / PAGE_LINES;

    if (self.page < 0) {
        self.body.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
        self.body.textAlignment = NSTextAlignmentCenter;
        self.body.text = self.summary;
        self.position.text = [NSString stringWithFormat:@"The whole report is %ld pages; \"Show everything\" goes through them.", (long)pages];
    } else {
        NSRange range = NSMakeRange((NSUInteger)self.page * PAGE_LINES, PAGE_LINES);

        if (NSMaxRange(range) > self.lines.count) range.length = self.lines.count - range.location;
        self.body.font = [UIFont monospacedSystemFontOfSize:20 weight:UIFontWeightRegular];
        self.body.textAlignment = NSTextAlignmentLeft;
        self.body.text = [[self.lines subarrayWithRange:range] componentsJoinedByString:@"\n"];
        self.position.text = [NSString stringWithFormat:@"Page %ld of %ld", (long)self.page + 1, (long)pages];
    }
}
- (void)send { self.answer = 1; }
- (void)decline { self.answer = 2; }
- (void)first { self.page = -1; [self show]; }
- (void)next {
    NSInteger pages = ((NSInteger)self.lines.count + PAGE_LINES - 1) / PAGE_LINES;

    self.page = self.page + 1 >= pages ? -1 : self.page + 1;
    [self show];
}
@end

static UIWindow *window_for(UIViewController *controller) {
    UIWindowScene *scene = nil;
    UIWindow *window;

    for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes)
        if ([candidate isKindOfClass:UIWindowScene.class]) { scene = (UIWindowScene *)candidate; break; }
    if (!scene) return nil;
    window = [[UIWindow alloc] initWithWindowScene:scene];
    window.frame = scene.coordinateSpace.bounds;
    window.windowLevel = UIWindowLevelNormal + 2;
    window.rootViewController = controller;
    [window makeKeyAndVisible];
    return window;
}

/* ---------- the record */

static NSData *gzipped(NSData *data) {
    NSMutableData *out = [NSMutableData dataWithLength:compressBound((uLong)data.length) + 32];
    z_stream stream = { 0 };

    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return nil;
    stream.next_in = (Bytef *)data.bytes; stream.avail_in = (uInt)data.length;
    stream.next_out = out.mutableBytes; stream.avail_out = (uInt)out.length;
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) { deflateEnd(&stream); return nil; }
    out.length = stream.total_out;
    deflateEnd(&stream);
    return out;
}

/* the report without its per-frame arrays, for the record's summary column */
static NSString *summary_json(NSData *report) {
    NSMutableDictionary *whole = [NSJSONSerialization JSONObjectWithData:report options:NSJSONReadingMutableContainers error:nil];
    NSData *text;

    for (NSMutableDictionary *run in whole[@"runs"]) [run removeObjectsForKeys:@[ @"frame_interval", @"frame_cpu", @"frame_gpu" ]];
    text = whole ? [NSJSONSerialization dataWithJSONObject:whole options:NSJSONWritingSortedKeys error:nil] : nil;
    return text ? [[NSString alloc] initWithData:text encoding:NSUTF8StringEncoding] : @"";
}

/* saves the record; 1 sent, 0 not, with a sentence for the screen either way */
static int upload(NSData *report, NSString **message) {
    NSDictionary *whole = [NSJSONSerialization JSONObjectWithData:report options:0 error:nil];
    NSDictionary *device = whole[@"device"], *app = whole[@"app"];
    CKContainer *container = [CKContainer containerWithIdentifier:@HALO_BENCHMARK_CLOUD];
    NSURL *file = [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:
        [NSString stringWithFormat:@"visr-benchmark-%@.json.gz", whole[@"code"]]];
    NSDateFormatter *minutes = [NSDateFormatter new];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block CKAccountStatus account = CKAccountStatusCouldNotDetermine;
    __block NSError *failure = nil;
    CFAbsoluteTime until = CFAbsoluteTimeGetCurrent() + SEND_SECONDS;
    CKRecord *record;

    if (![whole isKindOfClass:NSDictionary.class] || ![gzipped(report) writeToURL:file atomically:YES]) {
        *message = @"The report could not be prepared for sending.";
        return 0;
    }
    [container accountStatusWithCompletionHandler:^(CKAccountStatus status, NSError *error) {
        (void)error;
        account = status;
        dispatch_semaphore_signal(done);
    }];
    while (dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) && CFAbsoluteTimeGetCurrent() < until)
        @autoreleasepool { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true); }
    if (account != CKAccountStatusAvailable) {
        *message = @"This device is not signed in to iCloud, so the report cannot be sent this way.";
        host_logf(HOST_LOG_INFO, "benchmark: not sent: iCloud account status %ld", (long)account);
        return 0;
    }
    minutes.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    minutes.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    minutes.dateFormat = @"yyyy-MM-dd'T'HH:mm'Z'";
    /* the record's name: the report's code and a random part, so a second send of one report is a second record
       and no name can be guessed */
    record = [[CKRecord alloc] initWithRecordType:RECORD_TYPE recordID:[[CKRecordID alloc] initWithRecordName:
        [NSString stringWithFormat:@"%@-%@", whole[@"code"], NSUUID.UUID.UUIDString]]];
    record[@"code"] = whole[@"code"];
    record[@"schema"] = whole[@"format"];
    record[@"suite"] = whole[@"suite"];
    record[@"mode"] = whole[@"mode"];
    record[@"model"] = device[@"model"];
    record[@"platform"] = device[@"platform"];
    record[@"osVersion"] = device[@"os"];
    record[@"appVersion"] = app[@"version"];
    record[@"appBuild"] = app[@"build"];
    record[@"started"] = [minutes dateFromString:whole[@"started"] ?: @""];
    record[@"runs"] = @([whole[@"runs"] count]);
    if ([whole[@"flags"] count]) record[@"flags"] = whole[@"flags"];
    record[@"summary"] = summary_json(report);
    record[@"report"] = [[CKAsset alloc] initWithFileURL:file];
    [container.publicCloudDatabase saveRecord:record completionHandler:^(CKRecord *saved, NSError *error) {
        (void)saved;
        failure = error;
        dispatch_semaphore_signal(done);
    }];
    while (dispatch_semaphore_wait(done, DISPATCH_TIME_NOW)) {
        if (CFAbsoluteTimeGetCurrent() >= until) {
            *message = @"Sending took too long. The report is still on this device.";
            host_logf(HOST_LOG_INFO, "benchmark: not sent: no answer in %d s", SEND_SECONDS);
            return 0;
        }
        @autoreleasepool { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true); }
    }
    [NSFileManager.defaultManager removeItemAtURL:file error:nil];
    if (failure) {
        *message = @"The report could not be sent. It is still on this device.";
        host_logf(HOST_LOG_INFO, "benchmark: not sent: %s (%s %ld)", failure.localizedDescription.UTF8String,
            failure.domain.UTF8String, (long)failure.code);
        return 0;
    }
    *message = [NSString stringWithFormat:@"Sent. Thank you. The report's code is %@.", whole[@"code"]];
    host_logf(HOST_LOG_INFO, "benchmark: report %s sent as record %s", [whole[@"code"] UTF8String], record.recordID.recordName.UTF8String);
    return 1;
}

int host_benchmark_send(const char *report_path, const char *summary, char *message, unsigned long size) {
    NSData *report = [NSData dataWithContentsOfFile:@(report_path)];
    NSUInteger frame_values = 0;
    NSString *text = report ? readable(report, &frame_values) : nil, *result = @"";
    HaloBenchmarkConsentController *controller;
    UIWindow *window;
    int sent = 0;

    *message = 0;
    if (!text) return 0;
#ifndef HALO_TESTER_BUILD
    /* a developer's own test of the upload, with nobody at the screen: HALO_BENCHMARK_SEND=send in the launch's
       environment sends without the question. A tester build has no such way around it */
    {
        const char *unattended = getenv("HALO_BENCHMARK_SEND");

        if (unattended && !strcmp(unattended, "send")) {
            host_logf(HOST_LOG_INFO, "benchmark: HALO_BENCHMARK_SEND=send: sending without the question (a developer's test)");
            sent = upload(report, &result);
            snprintf(message, size, "%s", result.UTF8String);
            return sent;
        }
    }
#endif
    controller = [HaloBenchmarkConsentController new];
    controller.summary = [NSString stringWithFormat:@"%s\nThe report holds this device's model, system version, graphics chip and "
        "display mode, the app's version, and the measurements of each run, with %lu frame times.\nIt does not hold the device's "
        "name, any account, any address or location, or anything from the game.\nIt goes to the VISR developers through iCloud, "
        "which records with it the time and two IDs it assigns in this app: one for your iCloud account and one for this device.",
        summary, (unsigned long)frame_values];
    controller.lines = [text componentsSeparatedByString:@"\n"];
    window = window_for(controller);
    if (!window) return 0;
    host_logf(HOST_LOG_INFO, "benchmark: asking whether to send the report");
    while (!controller.answer)
        @autoreleasepool { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true); }
    if (controller.answer == 1) sent = upload(report, &result);
    else {
        result = @"Not sent.";
        host_logf(HOST_LOG_INFO, "benchmark: the tester chose not to send the report");
    }
    window.hidden = YES;
    window.rootViewController = nil;
    snprintf(message, size, "%s", result.UTF8String);
    return sent;
}
