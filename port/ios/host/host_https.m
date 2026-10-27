// posix_https_request (port/linux/src/posix.h) for Apple devices: a small HTTPS GET or form POST through
// NSURLSession, which checks the certificate against the system's authorities and the host name. The Delta game
// list and signed master list use synchronous requests (the guest's thread waits), keeping the
// answer in memory up to the caller's size. A redirect is not followed. The guest calls it from a thread of its own
// (p2p_delta.c), never from the game's.
#import <Foundation/Foundation.h>
#include "posix.h"
#include <string.h>

@interface HostHTTPSRequest : NSObject <NSURLSessionDataDelegate> {
@public
    struct posix_https_headers capturedHeaders;
}
@property(nonatomic) NSMutableData *body;
@property(nonatomic) NSUInteger limit;
@property(nonatomic) NSInteger status;
@property(nonatomic) BOOL tooLarge;
@property(nonatomic) BOOL invalidHeaders;
@property(nonatomic) BOOL captureHeaders;
@property(nonatomic) NSError *failure;
@property(nonatomic) dispatch_semaphore_t done;
@end

@implementation HostHTTPSRequest
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest * _Nullable))completionHandler {
    completionHandler(nil);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    if ([response isKindOfClass:NSHTTPURLResponse.class]) {
        NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
        _status = http.statusCode;
        if (_captureHeaders) {
            struct posix_https_headers *headers = &capturedHeaders;
            NSString *names[] = {@"Signature", @"Signature-Key", @"ETag", @"Retry-After"};
            char *values[] = {headers->signature, headers->signature_key, headers->etag, headers->retry_after};
            size_t sizes[] = {sizeof(headers->signature), sizeof(headers->signature_key),
                sizeof(headers->etag), sizeof(headers->retry_after)};
            for (int index = 0; index < 4; index++) {
                const char *value = [http valueForHTTPHeaderField:names[index]].UTF8String;
                if (!value) continue;
                if (strlen(value) >= sizes[index]) _invalidHeaders = YES;
                else memcpy(values[index], value, strlen(value) + 1);
            }
        }
    }
    // only a 200's body is wanted
    completionHandler(_status == 200 && !_invalidHeaders ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    if (_body.length + data.length > _limit) {
        _tooLarge = YES;
        [task cancel];
        return;
    }
    [_body appendData:data];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    _failure = error;
    dispatch_semaphore_signal(_done);
}
@end

static void host_https_error(char *error, int error_size, NSString *text) {
    if (error_size > 0) snprintf(error, (size_t)error_size, "%s", text.UTF8String ?: "");
}

static int host_https_request(const char *url, const char *form, int form_size, const char *etag,
    char *response, int response_size, int timeout_milliseconds, struct posix_https_headers *headers,
    char *error, int error_size) {
    if (headers) memset(headers, 0, sizeof(*headers));
    if (response && response_size > 0) response[0] = 0;
    if (error && error_size > 0) error[0] = 0;
    if (!url || !response || !error || response_size < 2 || error_size < 2 || (form && form_size < 0) ||
        (etag && (strlen(etag) >= 128 || strpbrk(etag, "\r\n")))) return 0;
    @autoreleasepool {
        NSURL *address = [NSURL URLWithString:@(url)];
        if (!address || ![address.scheme.lowercaseString isEqualToString:@"https"] || !address.host.length) {
            host_https_error(error, error_size, [NSString stringWithFormat:@"not an https:// address: %s", url]);
            return 0;
        }
        NSTimeInterval timeout = (timeout_milliseconds > 0 ? timeout_milliseconds : 20000) / 1000.0;
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:address
            cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:timeout];
        [request setValue:@"VISR" forHTTPHeaderField:@"User-Agent"];
        // NSURLSession adds the device's languages as Accept-Language; an empty one sends none (checked on the wire)
        [request setValue:@"" forHTTPHeaderField:@"Accept-Language"];
        if (etag && *etag) [request setValue:@(etag) forHTTPHeaderField:@"If-None-Match"];
        if (form) {
            request.HTTPMethod = @"POST";
            request.HTTPBody = [NSData dataWithBytes:form length:(NSUInteger)form_size];
            [request setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
        }
        HostHTTPSRequest *state = [HostHTTPSRequest new];
        state.body = [NSMutableData data];
        state.captureHeaders = headers != NULL;
        state.limit = (NSUInteger)response_size - 1;
        state.done = dispatch_semaphore_create(0);
        NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        configuration.timeoutIntervalForRequest = timeout;
        configuration.timeoutIntervalForResource = timeout * 2;
        configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration delegate:state delegateQueue:nil];
        [[session dataTaskWithRequest:request] resume];
        if (dispatch_semaphore_wait(state.done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * 3 * NSEC_PER_SEC)))) {
            [session invalidateAndCancel];
            host_https_error(error, error_size, @"no answer");
            return 0;
        }
        [session finishTasksAndInvalidate];
        if (headers) *headers = state->capturedHeaders;
        if (state.invalidHeaders) {
            host_https_error(error, error_size, @"oversized response header");
            return 0;
        }
        if (state.tooLarge) {
            host_https_error(error, error_size, @"the answer is too large");
            return 0;
        }
        // a cancelled non-200 is a status, not a failure
        if (state.status && state.status != 200) return (int)state.status;
        if (state.failure || state.status != 200) {
            host_https_error(error, error_size, state.failure.localizedDescription ?: @"no answer");
            return 0;
        }
        // NSURLSession delivers decoded bytes, which are the bytes covered by the signature.
        if (headers) headers->body_size = (int)state.body.length;
        memcpy(response, state.body.bytes, state.body.length);
        response[state.body.length] = 0;
        return 200;
    }
}

int posix_https_request(const char *url, const char *form, int form_size, char *response, int response_size,
    int timeout_milliseconds, char *error, int error_size) {
    return host_https_request(url, form, form_size, NULL, response, response_size, timeout_milliseconds,
        NULL, error, error_size);
}

int posix_https_get_signed(const char *url, const char *etag, char *response, int response_size,
    int timeout_milliseconds, struct posix_https_headers *headers, char *error, int error_size) {
    if (!headers) return 0;
    return host_https_request(url, NULL, 0, etag, response, response_size, timeout_milliseconds,
        headers, error, error_size);
}
