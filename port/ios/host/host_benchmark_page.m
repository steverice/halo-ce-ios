/* The device benchmark's report, handed over with no account: the device serves
   a small page on the local network, and a screen shows the page's address as a
   QR code. A phone or computer on the same network opens it, reads the summary
   and downloads the report file, which its owner then sends however they like.
   An Apple TV has no Files app and no share sheet, so this is its way out; the
   import screen (host_tv_import.m) works the same way in the other direction.

   Only GET is answered, only from private and link-local addresses, and only two
   things are served: the page and the report. */
#import <CoreImage/CoreImage.h>
#import <UIKit/UIKit.h>
#include <Network/Network.h>
#include "ios_host.h"
#include "host_benchmark.h"
#include "host_benchmark_live.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdatomic.h>
#include <unistd.h>
#include <zlib.h>

#if !__has_feature(objc_arc)
#error The benchmark page requires Objective-C ARC.
#endif

#define PAGE_PORT "8081"
#define HEADER_LIMIT 8192

static nw_listener_t listener;
static dispatch_queue_t page_queue;
static _Atomic uint16_t page_port;
static NSData *page_html, *report_gzip;
static NSString *report_name;

/* ---------- serving */

static NSData *gzip(NSData *data) {
    NSMutableData *out = [NSMutableData dataWithLength:compressBound((uLong)data.length) + 32];
    z_stream stream = { 0 };

    /* 15 + 16: a gzip wrapper, which every browser and unarchiver opens */
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return nil;
    stream.next_in = (Bytef *)data.bytes; stream.avail_in = (uInt)data.length;
    stream.next_out = out.mutableBytes; stream.avail_out = (uInt)out.length;
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) { deflateEnd(&stream); return nil; }
    out.length = stream.total_out;
    deflateEnd(&stream);
    return out;
}

static NSString *escaped(NSString *text) {
    text = [text stringByReplacingOccurrencesOfString:@"&" withString:@"&amp;"];
    text = [text stringByReplacingOccurrencesOfString:@"<" withString:@"&lt;"];
    return [text stringByReplacingOccurrencesOfString:@">" withString:@"&gt;"];
}

static NSData *html(NSString *summary, NSString *code, NSUInteger size) {
    NSString *text = [NSString stringWithFormat:@"<!doctype html><html lang=en><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'><title>VISR benchmark report</title><style>"
        ":root{color-scheme:light dark;--bg:#f4f5f2;--fg:#15191b;--muted:#5c6468;--card:#fff;--accent:#4f8f3a;--line:#d6dad6}"
        "@media (prefers-color-scheme:dark){:root{--bg:#0b1114;--fg:#e8ece9;--muted:#9aa4a8;--card:#141d21;--line:#27343a}}"
        "body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,system-ui,sans-serif}"
        "main{max-width:560px;margin:0 auto;padding:32px 16px}h1{margin:0 0 4px;font-size:28px}p{color:var(--muted);margin:0 0 20px}"
        "pre{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;overflow-x:auto;font-size:14px}"
        "a.get{display:block;text-align:center;background:var(--accent);color:#fff;font-weight:600;text-decoration:none;"
        "padding:14px 16px;border-radius:12px;margin:20px 0}</style></head><body><main>"
        "<h1>VISR benchmark</h1><p>Report %@. Download the file and send it to the VISR developers. "
        "It holds the device's model, its system version and the measurements below in full; "
        "it does not hold the device's name, an account, an address or game assets. It includes shader compilation descriptors for improving load times.</p>"
        "<pre>%@</pre><a class=get href='/%@' download>Download the report (%lu KB)</a>"
        "<p>Keep the report screen open on the device until the download finishes.</p></main></body></html>",
        escaped(code), escaped(summary), report_name, (unsigned long)((size + 1023) / 1024)];

    return [text dataUsingEncoding:NSUTF8StringEncoding];
}

static void respond(nw_connection_t connection, int status, NSString *type, NSString *extra, NSData *body) {
    NSString *head = [NSString stringWithFormat:@"HTTP/1.1 %d %@\r\nContent-Type: %@\r\nContent-Length: %lu\r\n%@"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
        status, status == 200 ? @"OK" : status == 404 ? @"Not Found" : @"Method Not Allowed", type, (unsigned long)body.length,
        extra ?: @""];
    NSMutableData *all = [[head dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];

    [all appendData:body];
    dispatch_data_t data = dispatch_data_create(all.bytes, all.length, page_queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    nw_connection_send(connection, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true, ^(nw_error_t error) {
        (void)error;
        nw_connection_cancel(connection);
    });
}

static void answer(nw_connection_t connection, NSString *head) {
    NSArray<NSString *> *parts = [[head componentsSeparatedByString:@"\r\n"].firstObject componentsSeparatedByString:@" "];
    NSString *path = parts.count >= 2 ? [parts[1] componentsSeparatedByString:@"?"].firstObject : @"";
    NSData *nothing = [@"Not here." dataUsingEncoding:NSUTF8StringEncoding];

    if (parts.count < 2 || ![parts[0] isEqualToString:@"GET"]) respond(connection, 405, @"text/plain; charset=utf-8", @"Allow: GET\r\n", nothing);
    else if ([path isEqualToString:@"/"]) respond(connection, 200, @"text/html; charset=utf-8", nil, page_html);
    else if ([path isEqualToString:[@"/" stringByAppendingString:report_name]])
        respond(connection, 200, @"application/gzip",
            [NSString stringWithFormat:@"Content-Disposition: attachment; filename=\"%@\"\r\n", report_name], report_gzip);
    else respond(connection, 404, @"text/plain; charset=utf-8", nil, nothing);
}

static void receive_head(nw_connection_t connection, NSMutableData *head) {
    nw_connection_receive(connection, 1, 4096, ^(dispatch_data_t content, nw_content_context_t context, bool complete, nw_error_t error) {
        (void)context;
        if (content) dispatch_data_apply(content, ^bool(dispatch_data_t region, size_t offset, const void *buffer, size_t size) {
            (void)region; (void)offset;
            [head appendBytes:buffer length:size];
            return true;
        });
        NSRange end = [head rangeOfData:[NSData dataWithBytes:"\r\n\r\n" length:4] options:0 range:NSMakeRange(0, head.length)];
        if (end.location != NSNotFound) {
            NSString *text = [[NSString alloc] initWithData:[head subdataWithRange:NSMakeRange(0, end.location)] encoding:NSUTF8StringEncoding];

            if (text) answer(connection, text);
            else nw_connection_cancel(connection);
        } else if (head.length >= HEADER_LIMIT || error || complete) nw_connection_cancel(connection);
        else receive_head(connection, head);
    });
}

/* a peer on this network: private IPv4, loopback, or link-local and unique-local IPv6 */
static BOOL local_peer(nw_connection_t connection) {
    nw_endpoint_t endpoint = nw_connection_copy_endpoint(connection);
    const struct sockaddr *address = endpoint ? nw_endpoint_get_address(endpoint) : NULL;

    if (!address) return NO;
    if (address->sa_family == AF_INET) {
        uint32_t ip = ntohl(((const struct sockaddr_in *)address)->sin_addr.s_addr);

        return ip >> 24 == 10 || ip >> 24 == 127 || ip >> 20 == 0xac1 || ip >> 16 == 0xc0a8 || ip >> 16 == 0xa9fe;
    }
    if (address->sa_family == AF_INET6) {
        const uint8_t *bytes = ((const struct sockaddr_in6 *)address)->sin6_addr.s6_addr;
        static const uint8_t loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

        if (!memcmp(bytes, mapped, 12))
            return bytes[12] == 10 || bytes[12] == 127 || (bytes[12] == 172 && bytes[13] >> 4 == 1) ||
                (bytes[12] == 192 && bytes[13] == 168) || (bytes[12] == 169 && bytes[13] == 254);
        return !memcmp(bytes, loopback, 16) || (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) || (bytes[0] & 0xfe) == 0xfc;
    }
    return NO;
}

static void start_listener(const char *port) {
    nw_parameters_t parameters = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_listener_t created;

    nw_parameters_set_reuse_local_address(parameters, true);
    created = nw_listener_create_with_port(port, parameters);
    if (!created) return;
    listener = created;
    nw_listener_set_queue(created, page_queue);
    nw_listener_set_new_connection_handler(created, ^(nw_connection_t connection) {
        nw_connection_set_queue(connection, page_queue);
        nw_connection_start(connection);
        if (!local_peer(connection)) { nw_connection_cancel(connection); return; }
        receive_head(connection, [NSMutableData new]);
    });
    nw_listener_set_state_changed_handler(created, ^(nw_listener_state_t state, nw_error_t error) {
        (void)error;
        if (state == nw_listener_state_ready) {
            atomic_store(&page_port, nw_listener_get_port(created));
            host_logf(HOST_LOG_INFO, "benchmark: the report page is on port %u", nw_listener_get_port(created));
        } else if (state == nw_listener_state_failed && created == listener) {
            nw_listener_cancel(created);
            /* the usual port is taken: let the system pick one */
            if (strcmp(port, "0")) start_listener("0");
        }
    });
    nw_listener_start(created);
}

/* ---------- the screen */

static NSString *local_address(void) {
    struct ifaddrs *list = NULL, *entry;
    NSString *found = nil, *fallback = nil;

    if (getifaddrs(&list)) return nil;
    for (entry = list; entry; entry = entry->ifa_next) {
        char text[INET_ADDRSTRLEN];

        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) continue;
        if (!(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK)) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)entry->ifa_addr)->sin_addr, text, sizeof(text));
        if (!strncmp(entry->ifa_name, "en", 2)) { if (!found) found = @(text); }
        else if (!fallback) fallback = @(text);
    }
    freeifaddrs(list);
    return found ?: fallback;
}

static UIImage *qr_image(NSString *text) {
    CIFilter *filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
    CIImage *output;
    CGImageRef image;
    UIImage *result;

    [filter setValue:[text dataUsingEncoding:NSUTF8StringEncoding] forKey:@"inputMessage"];
    [filter setValue:@"M" forKey:@"inputCorrectionLevel"];
    output = [filter.outputImage imageByApplyingTransform:CGAffineTransformMakeScale(10, 10)];
    if (!output) return nil;
    image = [[CIContext context] createCGImage:output fromRect:output.extent];
    if (!image) return nil;
    result = [UIImage imageWithCGImage:image];
    CGImageRelease(image);
    return result;
}

@interface HaloBenchmarkPageController : UIViewController
@property(nonatomic, copy) NSString *summary, *code;
@property(nonatomic, strong) UIImageView *qr;
@property(nonatomic, strong) UILabel *address;
@property(nonatomic, copy) NSString *shownURL;
@property(nonatomic) BOOL dismissed;
@end

@implementation HaloBenchmarkPageController
- (UILabel *)label:(UIFontTextStyle)style color:(UIColor *)color text:(NSString *)text {
    UILabel *label = [UILabel new];

    label.font = [UIFont preferredFontForTextStyle:style];
    label.textColor = color;
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.text = text;
    return label;
}
- (void)viewDidLoad {
    UIStackView *stack = [UIStackView new];

    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 18;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
#if TARGET_OS_TV
    [self.view addSubview:stack];
#else
    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    [scroll addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:20],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-20],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:20],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-20],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-40]]];
#endif
    [stack addArrangedSubview:[self label:UIFontTextStyleTitle1 color:UIColor.whiteColor
        text:[NSString stringWithFormat:@"Benchmark finished · report %@", self.code]]];
    [stack addArrangedSubview:[self label:UIFontTextStyleBody color:UIColor.lightGrayColor text:self.summary]];
    self.qr = [UIImageView new];
    self.qr.layer.magnificationFilter = kCAFilterNearest;
    self.qr.backgroundColor = UIColor.whiteColor;
    self.qr.contentMode = UIViewContentModeScaleAspectFit;
    [self.qr.widthAnchor constraintEqualToConstant:280].active = YES;
    [self.qr.heightAnchor constraintEqualToConstant:280].active = YES;
    [stack addArrangedSubview:self.qr];
    self.address = [self label:UIFontTextStyleHeadline color:UIColor.whiteColor text:@""];
    [stack addArrangedSubview:self.address];
    [stack addArrangedSubview:[self label:UIFontTextStyleCallout color:UIColor.lightGrayColor
        text:@"With a phone or computer on this network, scan the code or open the address, then download the report.\n"
#if TARGET_OS_TV
            "Press Back when you are done."
#else
            "Keep this screen open until the download finishes."
#endif
        ]];
#if !TARGET_OS_TV
    UIButton *done = [UIButton buttonWithType:UIButtonTypeSystem];
    [done setTitle:@"Done" forState:UIControlStateNormal];
    [done addTarget:self action:@selector(done:) forControlEvents:UIControlEventPrimaryActionTriggered];
    [done.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [stack addArrangedSubview:done];
#else
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor multiplier:0.85]]];
#endif
}
- (void)done:(UIButton *)button { (void)button; self.dismissed = YES; }
- (void)refresh {
    NSString *address = local_address();
    unsigned port = atomic_load(&page_port);
    NSString *url = address && port ? [NSString stringWithFormat:@"http://%@:%u", address, port] : nil;

    if (url && ![url isEqualToString:self.shownURL]) {
        self.shownURL = url;
        self.address.text = url;
        host_logf(HOST_LOG_INFO, "benchmark: the report page is at %s", url.UTF8String);
        self.qr.image = qr_image(url);
    } else if (!url && !self.shownURL) self.address.text = @"Connect this device to your network to get the report.";
}
/* Back or Menu on a remote, B on a controller, Escape on a keyboard */
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    for (UIPress *press in presses)
        if (press.type == UIPressTypeMenu) { self.dismissed = YES; return; }
    [super pressesBegan:presses withEvent:event];
}
@end

/* ---------- the screen between runs */

@interface HaloBenchmarkNoticeController : UIViewController
@property(nonatomic, copy) NSString *heading, *detail;
@property(nonatomic) BOOL stopped;
@end

@implementation HaloBenchmarkNoticeController
- (void)viewDidLoad {
    UIStackView *stack = [UIStackView new];
    UILabel *title = [UILabel new], *text = [UILabel new], *stop = [UILabel new];

    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 20;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    title.text = self.heading;
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    title.textColor = UIColor.whiteColor;
    text.text = self.detail;
    text.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    text.textColor = UIColor.lightGrayColor;
    text.numberOfLines = 0;
    text.textAlignment = NSTextAlignmentCenter;
#if TARGET_OS_TV
    stop.text = @"Choose Finish or press Back to save available data and return to the menu.";
#else
    stop.text = @"Leave the device alone until the benchmark says it has finished.";
#endif
    stop.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCallout];
    stop.textColor = UIColor.grayColor;
    stop.numberOfLines = 0;
    stop.textAlignment = NSTextAlignmentCenter;
    for (UILabel *label in @[ title, text, stop ]) [stack addArrangedSubview:label];
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:@"Finish" forState:UIControlStateNormal];
    [button addTarget:self action:@selector(stop:) forControlEvents:UIControlEventPrimaryActionTriggered];
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [stack addArrangedSubview:button];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor multiplier:0.8]]];
}
- (void)stop:(UIButton *)button { (void)button; host_benchmark_request_finish(); self.stopped = YES; }
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    for (UIPress *press in presses)
        if (press.type == UIPressTypeMenu) { self.stopped = YES; return; }
    [super pressesBegan:presses withEvent:event];
}
@end

int host_benchmark_notice(const char *heading, const char *detail, double seconds, int (*until)(void)) {
    UIWindowScene *scene = nil;
    HaloBenchmarkNoticeController *controller = [HaloBenchmarkNoticeController new];
    UIWindow *window = nil;
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + seconds;
    int stopped;

    for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes)
        if ([candidate isKindOfClass:UIWindowScene.class]) { scene = (UIWindowScene *)candidate; break; }
    controller.heading = @(heading);
    controller.detail = @(detail);
    if (scene) {
        window = [[UIWindow alloc] initWithWindowScene:scene];
        window.frame = scene.coordinateSpace.bounds;
        window.windowLevel = UIWindowLevelNormal + 2;
        window.rootViewController = controller;
        [window makeKeyAndVisible];
    }
    while (!controller.stopped && !host_benchmark_finish_requested() && CFAbsoluteTimeGetCurrent() < end && !(until && until())) {
        @autoreleasepool {
            if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true) == kCFRunLoopRunFinished) usleep(100000);
#if !TARGET_OS_TV
            CFRunLoopRunInMode((__bridge CFStringRef)UITrackingRunLoopMode, 0.01, true);
#endif
        }
    }
    stopped = controller.stopped || host_benchmark_finish_requested();
    window.hidden = YES;
    window.rootViewController = nil;
    return stopped;
}

void host_benchmark_page(const char *report_path, const char *summary, const char *code, int minutes) {
    NSData *report = [NSData dataWithContentsOfFile:@(report_path)];
    UIWindowScene *scene = nil;
    HaloBenchmarkPageController *controller;
    UIWindow *window;
    CFAbsoluteTime until = CFAbsoluteTimeGetCurrent() + 60.0 * minutes;

    report_gzip = report ? gzip(report) : nil;
    if (!report_gzip) { host_logf(HOST_LOG_INFO, "benchmark: no report to serve at %s", report_path); return; }
    report_name = [NSString stringWithFormat:@"visr-benchmark-%s.json.gz", code];
    page_html = html(@(summary), @(code), report_gzip.length);
    page_queue = dispatch_queue_create("halo.benchmark.page", DISPATCH_QUEUE_SERIAL);
    atomic_store(&page_port, 0);
    start_listener(PAGE_PORT);

    for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes)
        if ([candidate isKindOfClass:UIWindowScene.class]) { scene = (UIWindowScene *)candidate; break; }
    if (!scene) { host_logf(HOST_LOG_INFO, "benchmark: no window scene for the report screen"); return; }
    controller = [HaloBenchmarkPageController new];
    controller.summary = @(summary);
    controller.code = @(code);
    window = [[UIWindow alloc] initWithWindowScene:scene];
    window.frame = scene.coordinateSpace.bounds;
    window.windowLevel = UIWindowLevelNormal + 2;
    window.rootViewController = controller;
    [window makeKeyAndVisible];
    host_logf(HOST_LOG_INFO, "benchmark: showing the report screen for up to %d minutes", minutes);
    while (!controller.dismissed && CFAbsoluteTimeGetCurrent() < until) {
        @autoreleasepool {
            [controller refresh];
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
#if !TARGET_OS_TV
            CFRunLoopRunInMode((__bridge CFStringRef)UITrackingRunLoopMode, 0.01, true);
#endif
        }
    }
    dispatch_sync(page_queue, ^{ if (listener) nw_listener_cancel(listener); listener = nil; });
    window.hidden = YES;
    window.rootViewController = nil;
    host_logf(HOST_LOG_INFO, "benchmark: the report screen closed");
}

/* Pass through game input while keeping native progress scrolling and Finish usable. */
@interface HaloBenchmarkOverlayWindow : UIWindow
@end
@implementation HaloBenchmarkOverlayWindow
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event {
    UIView *hit=[super hitTest:point withEvent:event];
    for(UIView *view=hit;view;view=view.superview)if([view isKindOfClass:UIControl.class] || [view isKindOfClass:UIScrollView.class])return hit;
    return nil;
}
@end
/* Raw render/submission intervals, never callback arrival or physical pacing.
   UIKit reads and draws at four Hz; the render thread only fills a C ring. */
@interface HaloBenchmarkGraphView : UIView {
    struct benchmark_live_snapshot _snapshot;
}
- (void)updateSnapshot:(const struct benchmark_live_snapshot *)snapshot;
@end
@implementation HaloBenchmarkGraphView
- (instancetype)initWithFrame:(CGRect)frame {
    if((self=[super initWithFrame:frame])) {
        self.backgroundColor=UIColor.clearColor;
        self.userInteractionEnabled=NO;
        self.isAccessibilityElement=YES;
        self.accessibilityLabel=@"Render intervals in milliseconds";
    }
    return self;
}
- (void)updateSnapshot:(const struct benchmark_live_snapshot *)snapshot {
    _snapshot=*snapshot;
    self.accessibilityValue=snapshot->count ? [NSString stringWithFormat:@"%u most recent renders. Peak %.1f milliseconds. %u above the graph range.",snapshot->count,snapshot->peak_ms,snapshot->clipped] : @"Waiting for render timing";
    [self setNeedsDisplay];
}
- (void)drawRect:(CGRect)rect {
    (void)rect;
    CGContextRef context=UIGraphicsGetCurrentContext();
    if(!context)return;
#if TARGET_OS_TV
    CGFloat fontSize=13;
#else
    CGFloat fontSize=10;
#endif
    NSDictionary *text=@{NSFontAttributeName:[UIFont monospacedDigitSystemFontOfSize:fontSize weight:UIFontWeightRegular],NSForegroundColorAttributeName:UIColor.lightGrayColor};
    [@"Render interval (ms)" drawAtPoint:CGPointMake(0,0) withAttributes:text];
    CGRect plot=CGRectMake(1,14,MAX(1,self.bounds.size.width-33),MAX(1,self.bounds.size.height-28));
    CGContextSetStrokeColorWithColor(context,[UIColor.whiteColor colorWithAlphaComponent:0.25].CGColor);
    CGContextSetLineWidth(context,0.5);
    for(unsigned i=1;i<=2;i++) {
        double reference=i*1000.0/60.0;
        CGFloat y=CGRectGetMaxY(plot)-reference/BENCHMARK_LIVE_GRAPH_MAX_MS*plot.size.height;
        CGContextMoveToPoint(context,plot.origin.x,y); CGContextAddLineToPoint(context,CGRectGetMaxX(plot),y);
        CGContextStrokePath(context);
        [[NSString stringWithFormat:@"%.1f",reference] drawAtPoint:CGPointMake(CGRectGetMaxX(plot)+3,y-6) withAttributes:text];
    }
    CGContextSetStrokeColorWithColor(context,UIColor.systemGreenColor.CGColor);
    CGContextSetLineWidth(context,1);
    for(unsigned i=0;i<_snapshot.count;i++) {
        CGFloat x=CGRectGetMaxX(plot)-(_snapshot.count-1-i)*plot.size.width/(BENCHMARK_LIVE_HISTORY-1);
        CGFloat y=CGRectGetMaxY(plot)-fmin(_snapshot.interval_ms[i]/BENCHMARK_LIVE_GRAPH_MAX_MS,1)*plot.size.height;
        if(!i)CGContextMoveToPoint(context,x,y); else CGContextAddLineToPoint(context,x,y);
    }
    CGContextStrokePath(context);
    if(_snapshot.count==1) {
        CGContextSetFillColorWithColor(context,UIColor.systemGreenColor.CGColor);
        CGFloat y=CGRectGetMaxY(plot)-fmin(_snapshot.interval_ms[0]/BENCHMARK_LIVE_GRAPH_MAX_MS,1)*plot.size.height;
        CGContextFillEllipseInRect(context,CGRectMake(CGRectGetMaxX(plot)-1,y-1,2,2));
    }
    CGContextSetFillColorWithColor(context,UIColor.systemRedColor.CGColor);
    for(unsigned i=0;i<_snapshot.count;i++)if(_snapshot.interval_ms[i]>BENCHMARK_LIVE_GRAPH_MAX_MS) {
        CGFloat x=CGRectGetMaxX(plot)-(_snapshot.count-1-i)*plot.size.width/(BENCHMARK_LIVE_HISTORY-1);
        CGContextMoveToPoint(context,x,plot.origin.y); CGContextAddLineToPoint(context,x-2,plot.origin.y+4);
        CGContextAddLineToPoint(context,x+2,plot.origin.y+4); CGContextClosePath(context);
    }
    CGContextFillPath(context);
    NSString *peak=_snapshot.count ? [NSString stringWithFormat:@"Peak %.1f ms%@ · %u renders",_snapshot.peak_ms,_snapshot.clipped ? @" ↑" : @"",_snapshot.count] : @"Waiting for render timing";
    [peak drawAtPoint:CGPointMake(0,self.bounds.size.height-12) withAttributes:text];
}
@end
@interface HaloBenchmarkOverlayController : UIViewController
@property(nonatomic,strong) UILabel *status;
@property(nonatomic,strong) HaloBenchmarkGraphView *graph;
@property(nonatomic,strong) NSTimer *timer;
/* while the frames are drawn offscreen, a screen of its own in place of the game's: what is being measured, how
   far along, and Finish */
@property(nonatomic,strong) UIView *progressScreen, *panel;
@property(nonatomic,strong) UILabel *progressTitle, *progressDetail, *progressPerf;
@property(nonatomic,strong) HaloBenchmarkGraphView *progressGraph;
@property(nonatomic,strong) UIProgressView *progressBar;
@property(nonatomic,strong) UIButton *progressFinish;
@end
@implementation HaloBenchmarkOverlayController
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor=UIColor.clearColor;
    UIStackView *panel=[UIStackView new]; panel.axis=UILayoutConstraintAxisVertical; panel.spacing=6;
    panel.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:0.78]; panel.layer.cornerRadius=10;
    panel.layoutMargins=UIEdgeInsetsMake(10,12,10,12); panel.layoutMarginsRelativeArrangement=YES;
    panel.translatesAutoresizingMaskIntoConstraints=NO;
    self.status=[UILabel new]; self.status.textColor=UIColor.whiteColor; self.status.numberOfLines=2;
#if TARGET_OS_TV
    CGFloat width=480, font=18, graphHeight=128;
#else
    CGFloat width=360, font=12, graphHeight=100;
#endif
    self.status.font=[UIFont monospacedDigitSystemFontOfSize:font weight:UIFontWeightMedium];
    self.status.text=@"Benchmarking…\nFPS: — CPU: —ms GPU: —ms";
    self.status.accessibilityHint=@"Work averages use up to 60 renders. GPU time is completed busy work per render, not latency. CPU and GPU overlap.";
    UIButton *finish=[UIButton buttonWithType:UIButtonTypeSystem];
    [finish setTitle:@"Finish" forState:UIControlStateNormal];
    [finish.heightAnchor constraintGreaterThanOrEqualToConstant:44].active=YES;
    [finish.widthAnchor constraintGreaterThanOrEqualToConstant:60].active=YES;
    [finish addTarget:self action:@selector(finish:) forControlEvents:UIControlEventPrimaryActionTriggered];
    UIStackView *row=[[UIStackView alloc] initWithArrangedSubviews:@[self.status,finish]];
    row.axis=UILayoutConstraintAxisHorizontal; row.alignment=UIStackViewAlignmentCenter; row.spacing=8;
    [finish setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [self.status setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    self.graph=[HaloBenchmarkGraphView new];
    [self.graph.heightAnchor constraintEqualToConstant:graphHeight].active=YES;
    [panel addArrangedSubview:row]; [panel addArrangedSubview:self.graph]; [self.view addSubview:panel];
    self.panel=panel;
    [self buildProgressScreen];
    NSLayoutConstraint *preferred=[panel.widthAnchor constraintEqualToConstant:width]; preferred.priority=UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[preferred,
        [panel.widthAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.widthAnchor constant:-24],
        [panel.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-12],
        [panel.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-12]]];
    [self refreshProgress];
    __weak HaloBenchmarkOverlayController *weak=self;
    self.timer=[NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *timer) {
        HaloBenchmarkOverlayController *controller=weak;
        if(!controller) { [timer invalidate]; return; }
        [controller refreshProgress];
        if(controller.view.hidden)return;
        struct benchmark_live_snapshot snapshot; host_benchmark_live(&snapshot);
        NSString *fps=snapshot.count ? [NSString stringWithFormat:@"%.0f",snapshot.fps] : @"—";
        NSString *cpu=snapshot.cpu_available ? [NSString stringWithFormat:@"%.1f",snapshot.cpu_ms] : @"—";
        NSString *gpu=snapshot.gpu_available ? [NSString stringWithFormat:@"%.1f",snapshot.gpu_ms] : @"—";
        NSString *perf=[NSString stringWithFormat:@"FPS: %@ CPU: %@ms GPU: %@ms",fps,cpu,gpu];
        if(!controller.progressScreen.hidden) {
            controller.progressPerf.text=perf;
            [controller.progressGraph updateSnapshot:&snapshot];
            return;
        }
        controller.status.text=host_benchmark_finish_requested() ? @"Finishing…" :
            [NSString stringWithFormat:@"Benchmarking…\n%@",perf];
        controller.graph.hidden=host_benchmark_finish_requested();
        [controller.graph updateSnapshot:&snapshot];
    }];
}
- (void)buildProgressScreen {
    UIView *screen=[UIView new];
    screen.backgroundColor=UIColor.blackColor;
    screen.translatesAutoresizingMaskIntoConstraints=NO;
    screen.hidden=YES;
    UIStackView *stack=[UIStackView new];
    stack.axis=UILayoutConstraintAxisVertical; stack.alignment=UIStackViewAlignmentCenter; stack.spacing=18;
    stack.translatesAutoresizingMaskIntoConstraints=NO;
    UILabel *heading=[UILabel new];
    heading.text=@"Benchmarking";
    heading.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    heading.textColor=UIColor.lightGrayColor;
    self.progressTitle=[UILabel new];
    self.progressTitle.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    self.progressTitle.textColor=UIColor.whiteColor;
    self.progressTitle.textAlignment=NSTextAlignmentCenter;
    self.progressTitle.numberOfLines=0;
    self.progressDetail=[UILabel new];
    self.progressDetail.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    self.progressDetail.textColor=UIColor.lightGrayColor;
    self.progressDetail.textAlignment=NSTextAlignmentCenter;
    self.progressDetail.numberOfLines=0;
    self.progressBar=[[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    self.progressBar.translatesAutoresizingMaskIntoConstraints=NO;
    self.progressPerf=[UILabel new];
    self.progressPerf.font=[UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightMedium];
    self.progressPerf.textColor=UIColor.whiteColor;
    self.progressPerf.textAlignment=NSTextAlignmentCenter;
    self.progressPerf.adjustsFontSizeToFitWidth=YES;
    self.progressPerf.text=@"FPS: — CPU: —ms GPU: —ms";
    self.progressGraph=[HaloBenchmarkGraphView new];
    self.progressGraph.translatesAutoresizingMaskIntoConstraints=NO;
#if TARGET_OS_TV
    CGFloat graphHeight=128;
#else
    CGFloat graphHeight=96;
#endif
    [self.progressGraph.heightAnchor constraintEqualToConstant:graphHeight].active=YES;
    UILabel *note=[UILabel new];
    note.text=@"Benchmark running — please don’t close the app";
    note.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    note.textColor=UIColor.grayColor;
    note.textAlignment=NSTextAlignmentCenter;
    note.numberOfLines=0;
    self.progressFinish=[UIButton buttonWithType:UIButtonTypeSystem];
    [self.progressFinish setTitle:@"Finish" forState:UIControlStateNormal];
    self.progressFinish.titleLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    [self.progressFinish.heightAnchor constraintGreaterThanOrEqualToConstant:44].active=YES;
    [self.progressFinish addTarget:self action:@selector(finish:) forControlEvents:UIControlEventPrimaryActionTriggered];
    UIView *topSpace=[UIView new], *bottomSpace=[UIView new];
    for(UIView *view in @[topSpace,heading,self.progressTitle,self.progressDetail,self.progressBar,self.progressPerf,self.progressGraph,note,self.progressFinish,bottomSpace])
        [stack addArrangedSubview:view];
    [topSpace.heightAnchor constraintEqualToAnchor:bottomSpace.heightAnchor].active=YES;
    UIScrollView *scroll=[UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints=NO;
    [screen addSubview:scroll];
    [scroll addSubview:stack];
    [self.view insertSubview:screen atIndex:0];
    self.progressScreen=screen;
    [NSLayoutConstraint activateConstraints:@[
        [screen.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [screen.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [screen.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [screen.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:screen.safeAreaLayoutGuide.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:screen.safeAreaLayoutGuide.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:screen.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:screen.safeAreaLayoutGuide.bottomAnchor],
        [scroll.contentLayoutGuide.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
        [stack.centerXAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.centerXAnchor],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor multiplier:0.8],
        [self.progressBar.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [self.progressGraph.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [self.progressPerf.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:12],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-12],
        [scroll.contentLayoutGuide.heightAnchor constraintGreaterThanOrEqualToAnchor:scroll.frameLayoutGuide.heightAnchor]]];
}
- (void)refreshProgress {
    struct host_benchmark_progress progress;
    host_benchmark_progress(&progress);
    self.view.hidden=progress.presented!=0;
    BOOL offscreen=host_benchmark_progress_visible(&progress);
    self.progressScreen.hidden=!offscreen;
    self.panel.hidden=offscreen || progress.presented;
    if(!offscreen)return;
    self.progressTitle.text=progress.title[0] ? @(progress.title) : @"Getting ready";
    char detail[512];
    host_benchmark_progress_detail(&progress,detail,sizeof(detail));
    self.progressDetail.text=host_benchmark_finish_requested() ? @"Finishing…" : @(detail);
    self.progressBar.hidden=progress.phase!=HOST_BENCHMARK_MEASURING || progress.measured_fraction<0;
    [self.progressBar setProgress:(float)progress.fraction animated:NO];
    self.progressFinish.enabled=!host_benchmark_finish_requested();
}
- (void)finish:(UIButton *)button { button.enabled=NO; host_benchmark_request_finish(); }
#if TARGET_OS_TV
- (NSArray<id<UIFocusEnvironment>> *)preferredFocusEnvironments {
    return self.progressScreen.hidden ? [super preferredFocusEnvironments] : @[self.progressFinish];
}
#endif
@end
static HaloBenchmarkOverlayWindow *benchmark_overlay;
static id benchmark_overlay_activation;
void host_benchmark_overlay_show(int visible) {
    if(!NSThread.isMainThread) { dispatch_async(dispatch_get_main_queue(),^{ host_benchmark_overlay_show(visible); }); return; }
    if(!visible) {
        if(benchmark_overlay_activation) [NSNotificationCenter.defaultCenter removeObserver:benchmark_overlay_activation];
        benchmark_overlay_activation=nil;
        [((HaloBenchmarkOverlayController *)benchmark_overlay.rootViewController).timer invalidate];
        benchmark_overlay.hidden=YES; benchmark_overlay.rootViewController=nil; benchmark_overlay=nil; return;
    }
    if(benchmark_overlay)return;
    /* SDL can enter the guest before the connected scene becomes active. Keep
       the request alive if no foreground window scene has connected yet. */
    if(!benchmark_overlay_activation)
        benchmark_overlay_activation=[NSNotificationCenter.defaultCenter addObserverForName:UISceneDidActivateNotification
            object:nil queue:nil usingBlock:^(NSNotification *notification) { host_benchmark_overlay_show(1); }];
    for(UIScene *candidate in UIApplication.sharedApplication.connectedScenes) {
        if(![candidate isKindOfClass:UIWindowScene.class] ||
            ![((UIWindowScene *)candidate).session.role isEqualToString:UIWindowSceneSessionRoleApplication])continue;
        host_logf(HOST_LOG_INFO,"benchmark overlay: window scene state %ld",(long)candidate.activationState);
        if(candidate.activationState!=UISceneActivationStateForegroundActive &&
            candidate.activationState!=UISceneActivationStateForegroundInactive)continue;
        benchmark_overlay=[[HaloBenchmarkOverlayWindow alloc] initWithWindowScene:(UIWindowScene *)candidate];
        benchmark_overlay.frame=((UIWindowScene *)candidate).coordinateSpace.bounds;
        benchmark_overlay.windowLevel=UIWindowLevelNormal+1;
        benchmark_overlay.rootViewController=[HaloBenchmarkOverlayController new];
        [benchmark_overlay makeKeyAndVisible];
        host_logf(HOST_LOG_INFO,"benchmark overlay: created native progress window");
        break;
    }
}

void host_benchmark_overlay_presented(int presented) {
    void (^update)(void)=^{
        benchmark_overlay.rootViewController.view.hidden=presented!=0;
        [(HaloBenchmarkOverlayController *)benchmark_overlay.rootViewController refreshProgress];
    };
    if(NSThread.isMainThread)update();
    else dispatch_sync(dispatch_get_main_queue(),update);
}
