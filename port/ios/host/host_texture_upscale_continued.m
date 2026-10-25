/* Apache-2.0. The import's "Upscaling textures" phase carried on in the background with iOS 26's continued processing
   (BGContinuedProcessingTask; the rules are texture_upscale_continued.h's). iPhone and iPad only: the API is
   unavailable on tvOS, visionOS and Mac Catalyst, where these calls do nothing.

   What the task allows, from the SDK headers: background CPU and network by default; background GPU only with the
   com.apple.developer.background-tasks.continued-processing.gpu entitlement on hardware that reports it in
   BGTaskScheduler.supportedResources. The generator does not use the GPU (Core ML on the CPU and Neural Engine,
   refinement and compression on the CPU), so the request asks for no extra resource. The headers say nothing about
   the Neural Engine: if a prediction fails while the app is in the background, the queue stops as on any error and
   the task is completed unsuccessfully; the work resumes when the app returns. */
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#include <unistd.h>
#include "host_texture_upscale.h"
#include "ios_host.h"
#include "texture_upscale_continued.h"

#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST && !TARGET_OS_VISION
#import <UIKit/UIKit.h>
#import <BackgroundTasks/BackgroundTasks.h>
#define CONTINUED_API 1
#endif

static enum texture_upscale_continued_state continued_state;
static NSObject *continued_lock;

#ifdef CONTINUED_API
static id running_task;   /* BGContinuedProcessingTask (iOS 26) */
static dispatch_source_t progress_timer;
static BOOL stopped_by_task, in_background;
static unsigned serial, idle_reports;

API_AVAILABLE(ios(26.0)) static void finish(BGContinuedProcessingTask *task, BOOL success, const char *why) {
    if (progress_timer) { dispatch_source_cancel(progress_timer); progress_timer = nil; }
    [task setTaskCompletedWithSuccess:success];
    host_logf(HOST_LOG_INFO, "Upscale continued processing: completed (%s, %s)", success ? "success" : "unsuccessful", why);
}

API_AVAILABLE(ios(26.0)) static void report(BGContinuedProcessingTask *task) {
    unsigned long long done = 0, total = 0;
    host_texture_upscale_progress_units(&done, &total);
    /* (work units: input texels; the system shows the fraction) */
    task.progress.totalUnitCount = (int64_t)(total ? total : 1);
    task.progress.completedUnitCount = (int64_t)MIN(done, total ? total : 1);
    /* a queue that has stopped while the app is away (an error, the Neural Engine refused in the background) makes no
       more progress: end the task rather than let the system expire it as stalled */
    /* (two readings a second apart: a pump that ends and the next it schedules leave a moment with neither) */
    idle_reports = in_background && !host_texture_upscale_busy() ? idle_reports + 1 : 0;
    if (idle_reports >= 3) {
        enum texture_upscale_continued_action action;
        @synchronized (continued_lock) { action = texture_upscale_continued_step(&continued_state, TEXTURE_UPSCALE_CONTINUED_EXPIRED); }
        if (action == TEXTURE_UPSCALE_CONTINUED_STOP_AND_FAIL) { running_task = nil; finish(task, NO, "the queue stopped in the background"); }
    }
}

API_AVAILABLE(ios(26.0)) static void launched(BGContinuedProcessingTask *task) {
    enum texture_upscale_continued_action action;
    @synchronized (continued_lock) { action = texture_upscale_continued_step(&continued_state, TEXTURE_UPSCALE_CONTINUED_LAUNCHED); }
    if (action == TEXTURE_UPSCALE_CONTINUED_COMPLETE) { finish(task, YES, "the import phase had ended"); return; }
    host_logf(HOST_LOG_INFO, "Upscale continued processing: running");
    __weak BGContinuedProcessingTask *weak = task;
    task.expirationHandler = ^{
        BGContinuedProcessingTask *expiring = weak;
        if (!expiring) return;
        enum texture_upscale_continued_action expired;
        @synchronized (continued_lock) { expired = texture_upscale_continued_step(&continued_state, TEXTURE_UPSCALE_CONTINUED_EXPIRED); }
        if (expired != TEXTURE_UPSCALE_CONTINUED_STOP_AND_FAIL) return;
        /* stop at the texture in hand (finished ones are in the manifest already); resumed in the foreground */
        if (in_background) { stopped_by_task = YES; host_texture_upscale_background_stop(1); }
        running_task = nil;
        finish(expiring, NO, "expired");
    };
    running_task = task;
    report(task);
    progress_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(progress_timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 4);
    dispatch_source_set_event_handler(progress_timer, ^{ if (running_task == task) report(task); });
    dispatch_resume(progress_timer);
}

static void observe_application(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil
            queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) { (void)note; in_background = YES; }];
        [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillEnterForegroundNotification object:nil
            queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
                (void)note; in_background = NO;
                if (stopped_by_task) { stopped_by_task = NO; host_texture_upscale_background_stop(0); }
            }];
    });
}

static void submit(void) API_AVAILABLE(ios(26.0)) {
    /* registrations of continued processing tasks may happen after launch; each request gets its own identifier
       under the Info.plist's wildcard (BGTaskSchedulerPermittedIdentifiers), so none is registered twice */
    NSString *identifier = [NSString stringWithFormat:@"%@.texture-upscale.import-%u-%u", NSBundle.mainBundle.bundleIdentifier,
        (unsigned)getpid(), ++serial];
    BGTaskScheduler *scheduler = BGTaskScheduler.sharedScheduler;
    if (![scheduler registerForTaskWithIdentifier:identifier usingQueue:dispatch_get_main_queue() launchHandler:^(__kindof BGTask *task) {
            launched((BGContinuedProcessingTask *)task); }]) {
        host_logf(HOST_LOG_WARN, "Upscale continued processing: %s is not a permitted identifier", identifier.UTF8String);
        @synchronized (continued_lock) { texture_upscale_continued_step(&continued_state, TEXTURE_UPSCALE_CONTINUED_REFUSED); }
        return;
    }
    BGContinuedProcessingTaskRequest *request = [[BGContinuedProcessingTaskRequest alloc] initWithIdentifier:identifier
        title:@"Upscaling textures" subtitle:@"Halo: Combat Evolved"];
    /* run now or not at all: a request queued behind others would start after the player came back */
    request.strategy = BGContinuedProcessingTaskRequestSubmissionStrategyFail;
    request.requiredResources = BGContinuedProcessingTaskRequestResourcesDefault;
    void (^submitted)(NSError *) = ^(NSError *error) {
        if (!error) { host_logf(HOST_LOG_INFO, "Upscale continued processing: submitted %s", identifier.UTF8String); return; }
        host_logf(HOST_LOG_WARN, "Upscale continued processing: not submitted (%s %ld: %s)", error.domain.UTF8String,
            (long)error.code, error.localizedDescription.UTF8String);
        @synchronized (continued_lock) { texture_upscale_continued_step(&continued_state, TEXTURE_UPSCALE_CONTINUED_REFUSED); }
    };
    if (@available(iOS 27.0, *)) {
        [scheduler submitTaskRequest:request completionHandler:submitted];
    } else {
        NSError *error = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        BOOL ok = [scheduler submitTaskRequest:request error:&error];
#pragma clang diagnostic pop
        submitted(ok ? nil : (error ?: [NSError errorWithDomain:BGTaskSchedulerErrorDomain code:0 userInfo:nil]));
    }
}
#endif

void host_texture_upscale_continued_phase(int began) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ continued_lock = [NSObject new]; });
#ifdef CONTINUED_API
    if (@available(iOS 26.0, *)) {
        enum texture_upscale_continued_action action;
        @synchronized (continued_lock) {
            action = texture_upscale_continued_step(&continued_state,
                began ? TEXTURE_UPSCALE_CONTINUED_PHASE_BEGAN : TEXTURE_UPSCALE_CONTINUED_PHASE_ENDED);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            observe_application();
            if (action == TEXTURE_UPSCALE_CONTINUED_SUBMIT) submit();
            else if (action == TEXTURE_UPSCALE_CONTINUED_COMPLETE && running_task) {
                BGContinuedProcessingTask *task = running_task;
                running_task = nil;
                report(task);
                finish(task, YES, "the import phase ended");
            }
        });
        return;
    }
#endif
    (void)began;
}
