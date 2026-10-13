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
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdatomic.h>
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
        "it does not hold the device's name, an account, an address or anything from the game.</p>"
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
    [self.view addSubview:stack];
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
            "Press Back when you are done."]];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor multiplier:0.85]]];
}
- (void)refresh {
    NSString *address = local_address();
    unsigned port = atomic_load(&page_port);
    NSString *url = address && port ? [NSString stringWithFormat:@"http://%@:%u", address, port] : nil;

    if (url && ![url isEqualToString:self.shownURL]) {
        self.shownURL = url;
        self.address.text = url;
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
        }
    }
    dispatch_sync(page_queue, ^{ if (listener) nw_listener_cancel(listener); listener = nil; });
    window.hidden = YES;
    window.rootViewController = nil;
    host_logf(HOST_LOG_INFO, "benchmark: the report screen closed");
}
