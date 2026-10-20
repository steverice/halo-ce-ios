// posix_https_request (port/linux/src/posix.h) for Apple devices: a small HTTPS GET or form POST through
// NSURLSession, which checks the certificate against the system's authorities and the host name. The Delta game
// list's requests (p2p_delta.c) are all it serves, so it is synchronous (the guest's thread waits) and keeps the
// answer in memory up to the caller's size. A redirect is not followed. The guest calls it from a thread of its own
// (p2p_delta.c), never from the game's.
#import <Foundation/Foundation.h>
#include "posix.h"
#include <string.h>

@interface HostHTTPSRequest : NSObject <NSURLSessionDataDelegate>
@property(nonatomic) NSMutableData *body;
@property(nonatomic) NSUInteger limit;
@property(nonatomic) NSInteger status;
@property(nonatomic) BOOL tooLarge;
@property(nonatomic) NSError *failure;
@property(nonatomic) dispatch_semaphore_t done;
@end

@implementation HostHTTPSRequest
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest * _Nullable))completionHandler {
    completionHandler(nil);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    if ([response isKindOfClass:NSHTTPURLResponse.class]) _status = ((NSHTTPURLResponse *)response).statusCode;
    // only a 200's body is wanted
    completionHandler(_status == 200 ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
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

int posix_https_request(const char *url, const char *form, int form_size, char *response, int response_size,
    int timeout_milliseconds, char *error, int error_size) {
    if (response_size > 0) response[0] = 0;
    if (error_size > 0) error[0] = 0;
    if (!url || response_size < 2 || error_size < 2 || (form && form_size < 0)) return 0;
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
        if (form) {
            request.HTTPMethod = @"POST";
            request.HTTPBody = [NSData dataWithBytes:form length:(NSUInteger)form_size];
            [request setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
        }
        HostHTTPSRequest *state = [HostHTTPSRequest new];
        state.body = [NSMutableData data];
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
        memcpy(response, state.body.bytes, state.body.length);
        response[state.body.length] = 0;
        return 200;
    }
}
