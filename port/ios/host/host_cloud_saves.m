/* Cloud saves in the host (host_cloud_saves.h): when they run, the queue the sync rules run on,
   the wait before the game starts, what the player is told, and the Settings page's rows.

   Nothing here runs on the game's thread except host_cloud_saves_directory_removed, which appends
   one line to a journal when the game deletes a save. The rules (port/runtime/cloud_saves.c) are
   single-threaded: every call into the engine is made on cloud_queue. Local saves are replaced
   only between the engine's local_write_begin and local_write_end, which hold cloud_gate; once
   the game starts, begin answers no for the rest of the run. */
#import <Foundation/Foundation.h>
#if HALO_NATIVE_MAC
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "host.h"
#include "host_cloud_saves.h"
#include "cloud_saves.h"
#include "guest_image.h"
#include "../../linux/include/halo_port_limits.h"

#define CLOUD_SCAN_SECONDS 15
#define CLOUD_RETRY_SECONDS 60         /* between tries while something waits and no save has changed */
#define CLOUD_WAIT_SECONDS 8           /* at launch, on a device that has saves */
#define CLOUD_WAIT_EMPTY_SECONDS 20    /* on one that has none: a new device, or a purged Apple TV */
#define CLOUD_ANSWER_SECONDS 20        /* for what a notice's button asks for */

static struct cloud_engine *cloud_engine;      /* cloud_queue only */
static dispatch_queue_t cloud_queue;
static dispatch_source_t cloud_timer;
static pthread_mutex_t cloud_gate = PTHREAD_MUTEX_INITIALIZER;
static int cloud_game_started;                 /* under cloud_gate */
static _Atomic int cloud_active, cloud_launch_done;
static char cloud_save_root[1024], cloud_state_path[1100];

static long long cloud_now(void *context) { (void)context; return (long long)time(NULL); }
static void cloud_log(void *context, const char *line) { (void)context; host_logf(HOST_LOG_INFO, "cloud saves: %s", line); }
static void cloud_random(void *context, uint8_t *bytes, size_t size) { (void)context; arc4random_buf(bytes, size); }

static int cloud_write_begin(void *context) {
    (void)context;
    pthread_mutex_lock(&cloud_gate);
    if (!cloud_game_started && atomic_load(&cloud_active) && host_cloud_saves_enabled()) return 1;
    pthread_mutex_unlock(&cloud_gate);
    return 0;
}

static void cloud_write_end(void *context) {
    (void)context;
    pthread_mutex_unlock(&cloud_gate);
}

/* the Settings page's status line */
static void cloud_publish(NSString *status) {
    [NSUserDefaults.standardUserDefaults setObject:host_cloud_saves_enabled() ? status : @"Off"
        forKey:@"cloud_saves_status"];
}

static void cloud_publish_state(enum cloud_state state) {
    if (!atomic_load(&cloud_active)) return;
    NSString *time = [NSDateFormatter localizedStringFromDate:NSDate.date dateStyle:NSDateFormatterNoStyle
        timeStyle:NSDateFormatterShortStyle];
    int slots = cloud_engine_synced_slots(cloud_engine);
    switch (state) {
    case CLOUD_STATE_IDLE:
        cloud_publish([NSString stringWithFormat:@"Up to date · %d save%s · %@", slots, slots == 1 ? "" : "s", time]); break;
    case CLOUD_STATE_PENDING: cloud_publish(@"Waiting for iCloud"); break;
    case CLOUD_STATE_NO_ACCOUNT: cloud_publish(@"Not signed in to iCloud"); break;
    case CLOUD_STATE_QUOTA: cloud_publish(@"iCloud storage is full"); break;
    case CLOUD_STATE_ACCOUNT_CHANGED: cloud_publish(@"Paused: another iCloud account"); break;
    case CLOUD_STATE_CLOUD_DELETED: cloud_publish(@"Paused: saves were deleted from iCloud"); break;
    case CLOUD_STATE_NEWER_FORMAT: cloud_publish(@"Some saves need a newer VISR"); break;
    case CLOUD_STATE_FAILED: cloud_publish(@"Not available in this build"); break;
    }
}

static void cloud_stop(void) {
    atomic_store(&cloud_active, 0);
    if (cloud_timer) dispatch_source_cancel(cloud_timer);
    cloud_timer = nil;
}

int host_cloud_saves_enabled(void) {
    return [NSUserDefaults.standardUserDefaults boolForKey:@"cloud_saves"];
}

int host_cloud_saves_sync_allowed(void) {
    if (!host_cloud_saves_enabled()) {
        /* A Settings-app change must also latch this run off before a later reenable. */
        if (atomic_exchange(&cloud_active, 0)) {
            cloud_publish(@"Off");
            if (cloud_queue) dispatch_async(cloud_queue, ^{ cloud_stop(); });
        }
        return 0;
    }
    return atomic_load(&cloud_active);
}

void host_cloud_saves_set_enabled(int enabled) {
    [NSUserDefaults.standardUserDefaults setBool:enabled != 0 forKey:@"cloud_saves"];
    if (!enabled) {
        /* Close the gate before returning to the guest; queued passes must see the change. */
        atomic_store(&cloud_active, 0);
        cloud_publish(@"Off");
        if (cloud_queue) dispatch_async(cloud_queue, ^{ cloud_stop(); });
    } else if (!atomic_load(&cloud_active)) {
        cloud_publish(@"Starts next launch");
    }
}

const char *host_cloud_saves_status(void) {
    static _Thread_local char status[512];
    NSString *value = host_cloud_saves_enabled() ?
        [NSUserDefaults.standardUserDefaults stringForKey:@"cloud_saves_status"] : @"Off";
    if (host_cloud_saves_enabled() && !atomic_load(&cloud_active) &&
            (!value.length || [value isEqualToString:@"Off"])) value = @"Starts next launch";
    snprintf(status, sizeof(status), "%s", (value ?: @"Starts next launch").UTF8String);
    return status;
}

/* cloud_queue only: the local saves as of the last pass that left nothing to do, as of the last
   pass whatever it left, and when that was */
static char cloud_idle_signature[CLOUD_HASH_SIZE], cloud_tried_signature[CLOUD_HASH_SIZE];
static CFAbsoluteTime cloud_last_pass;

/* one pass, on cloud_queue */
static void cloud_pass(enum cloud_pass pass) {
    if (!host_cloud_saves_sync_allowed()) return;
    char signature[CLOUD_HASH_SIZE];
    cloud_local_signature(cloud_save_root, cloud_state_path, signature);
    /* While the game runs a pass only uploads, and each pass begins by asking iCloud what it holds:
       when no save has changed since a pass that left nothing to do, there is nothing to ask for.
       When something still waits (no network, files being written), it is tried once a minute. */
    if (pass == CLOUD_PASS_RUNNING && (!strcmp(signature, cloud_idle_signature) ||
            (cloud_idle_signature[0] == 0 && cloud_last_pass && CFAbsoluteTimeGetCurrent() - cloud_last_pass < CLOUD_RETRY_SECONDS &&
             !strcmp(signature, cloud_tried_signature))))
        return;
    enum cloud_state state = cloud_engine_sync(cloud_engine, pass);
    cloud_last_pass = CFAbsoluteTimeGetCurrent();
    snprintf(cloud_tried_signature, sizeof(cloud_tried_signature), "%s", signature);
    snprintf(cloud_idle_signature, sizeof(cloud_idle_signature), "%s", state == CLOUD_STATE_IDLE ? signature : "");
    cloud_publish_state(state);
    if (state == CLOUD_STATE_FAILED) {
        host_logf(HOST_LOG_WARN, "cloud saves: stopped for this run");
        cloud_stop();
    }
}

/* Whether this signature carries the container. An app that asks CloudKit for a container its
   signature lacks is ended by an exception, and a sideloading tool that re-signs an unsigned build
   drops the entitlement, so the profile is read first. 1 yes, 0 no, -1 no profile to read. */
static int cloud_profile_has_container(NSString *container) {
    NSString *path = [NSBundle.mainBundle pathForResource:@"embedded" ofType:@"mobileprovision"];
#if TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
    if (!path) path = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Contents/embedded.provisionprofile"];
#endif
    NSData *profile = path ? [NSData dataWithContentsOfFile:path] : nil;
    if (!profile) return -1;
    /* a signed envelope around a property list */
    NSRange start = [profile rangeOfData:[@"<?xml" dataUsingEncoding:NSASCIIStringEncoding] options:0
        range:NSMakeRange(0, profile.length)];
    if (start.location == NSNotFound) return 0;
    NSRange end = [profile rangeOfData:[@"</plist>" dataUsingEncoding:NSASCIIStringEncoding] options:0
        range:NSMakeRange(start.location, profile.length - start.location)];
    if (end.location == NSNotFound) return 0;
    NSDictionary *list = [NSPropertyListSerialization propertyListWithData:
        [profile subdataWithRange:NSMakeRange(start.location, NSMaxRange(end) - start.location)] options:0 format:NULL error:NULL];
    if (![list isKindOfClass:NSDictionary.class]) return 0;
    NSDictionary *entitlements = list[@"Entitlements"];
    NSArray *containers = [entitlements isKindOfClass:NSDictionary.class] ?
        entitlements[@"com.apple.developer.icloud-container-identifiers"] : nil;
    NSArray *services = [entitlements isKindOfClass:NSDictionary.class] ? entitlements[@"com.apple.developer.icloud-services"] : nil;
    if (![containers isKindOfClass:NSArray.class] || ![containers containsObject:container]) return 0;
    /* (a profile may say "*" for the services) */
    return [services isKindOfClass:NSArray.class] ? [services containsObject:@"CloudKit"] :
        [(id)services isKindOfClass:NSString.class];
}

void host_cloud_saves_start(const char *data_root, const char *save_root) {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSDictionary<NSString *, NSString *> *environment = NSProcessInfo.processInfo.environment;
    [defaults registerDefaults:@{ @"cloud_saves": @NO, @"cloud_saves_restore": @NO, @"cloud_saves_delete": @NO,
        @"cloud_saves_status": @"Off" }];
    /* a test run (tools/mac_run.py) plays with a data folder of its own and must not reach an account */
    if (environment[@"HALO_RUNNER"] || [environment[@"HALO_BENCHMARK_SUITE"] length] || [environment[@"HALO_CLOUD_SAVES"] isEqualToString:@"0"]) {
        host_logf(HOST_LOG_INFO, "cloud saves: off for this run");
        return;
    }
    if (!host_cloud_saves_enabled()) {
        cloud_publish(@"Off");
        host_logf(HOST_LOG_INFO, "cloud saves: off in Settings");
        return;
    }
    int in_profile = cloud_profile_has_container(@HALO_CLOUD_CONTAINER);
#if TARGET_OS_SIMULATOR
    /* a simulator build has no profile; it may use the Mac's account when asked to */
    if (in_profile < 0 && [environment[@"HALO_CLOUD_SAVES"] isEqualToString:@"1"]) in_profile = 1;
#endif
    if (in_profile <= 0) {
        cloud_publish(@"Not available in this build");
        host_logf(HOST_LOG_INFO, "cloud saves: off, this app's signature does not carry the iCloud container %s",
            HALO_CLOUD_CONTAINER);
        return;
    }
    struct cloud_store store;
    if (!host_cloudkit_store(HALO_CLOUD_CONTAINER, &store)) {
        cloud_publish(@"Not available in this build");
        return;
    }
    /* the bookkeeping, in a folder of its own that device backups leave out: a device restored
       from another's backup must not take over what that one knew of the cloud */
    NSString *folder = [@(data_root) stringByAppendingPathComponent:@"cloud-saves"];
    [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    [[NSURL fileURLWithPath:folder] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    snprintf(cloud_save_root, sizeof(cloud_save_root), "%s", save_root);
    snprintf(cloud_state_path, sizeof(cloud_state_path), "%s/state", folder.fileSystemRepresentation);
#if TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
    NSString *device = @"Mac";
    const char *platform = "macOS";
#else
    NSString *device = UIDevice.currentDevice.model;
    const char *platform = TARGET_OS_TV ? "tvOS" : TARGET_OS_VISION ? "visionOS" : "iOS";
#endif
    NSString *app = [NSString stringWithFormat:@"%@ (%@)",
        [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
        [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"]];
    struct cloud_config config = {
        .save_root = cloud_save_root, .state_path = cloud_state_path, .device = device.UTF8String,
        .platform = platform,
        .app = app.UTF8String, .image = HALO_GUEST_SHA256, .state_size = HALO_PORT_GAME_STATE_SIZE,
        .saved_game_version = HALO_PORT_SAVED_GAME_VERSION,
        .network_version = HALO_PORT_NETWORK_VERSION, .now = cloud_now, .log = cloud_log,
        .local_write_begin = cloud_write_begin, .local_write_end = cloud_write_end, .random = cloud_random,
    };
    cloud_engine = cloud_engine_create(&config, &store);
    if (!cloud_engine) return;
    cloud_queue = dispatch_queue_create("visr.cloud-saves",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    atomic_store(&cloud_active, 1);
    cloud_publish(@"Checking iCloud");
    host_logf(HOST_LOG_INFO, "cloud saves: on, container %s", HALO_CLOUD_CONTAINER);
    BOOL restore = [defaults boolForKey:@"cloud_saves_restore"], delete = [defaults boolForKey:@"cloud_saves_delete"];
    [defaults setBool:NO forKey:@"cloud_saves_restore"];
    [defaults setBool:NO forKey:@"cloud_saves_delete"];
    dispatch_async(cloud_queue, ^{
        if (!atomic_load(&cloud_active) || !host_cloud_saves_enabled()) {
            atomic_store(&cloud_launch_done, 1);
            return;
        }
        if (delete) {
            /* the Settings page's "Delete saves from iCloud": the cloud's copy goes, this device's saves stay,
               and the setting goes off so that nothing is uploaded again unasked */
            if (cloud_engine_delete_cloud(cloud_engine) == CLOUD_OK) {
                [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"cloud_saves"];
                cloud_publish(@"Off");
                cloud_stop();
            } else
                host_logf(HOST_LOG_WARN, "cloud saves: could not delete the saves from iCloud now");
        } else {
            if (restore && cloud_engine_restore_copy(cloud_engine, NULL) != CLOUD_OK)
                host_logf(HOST_LOG_WARN, "cloud saves: there was no set-aside save to restore, or it could not be reached");
            cloud_pass(CLOUD_PASS_LAUNCH);
        }
        atomic_store(&cloud_launch_done, 1);
    });
}

/* runs block on cloud_queue and waits for it up to seconds, the main run loop running meanwhile
   (a blocked main thread at launch is what the system's watchdog ends an app for); NO on a timeout */
static BOOL cloud_wait(double seconds, void (^block)(void)) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    if (block) dispatch_async(cloud_queue, ^{ block(); dispatch_semaphore_signal(done); });
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;
    while (block ? dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) != 0 : !atomic_load(&cloud_launch_done)) {
        if (!atomic_load(&cloud_active) || !host_cloud_saves_enabled() || CFAbsoluteTimeGetCurrent() >= deadline) return NO;
        @autoreleasepool { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true); }
    }
    return YES;
}

/* a message with one or two buttons; returns the button's index */
static int cloud_ask(NSString *message, const char *first, const char *second) {
    SDL_MessageBoxButtonData buttons[2] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, first },
        { 0, 1, second } };
    SDL_MessageBoxData box = { SDL_MESSAGEBOX_INFORMATION, NULL, "iCloud saves", message.UTF8String, second ? 2 : 1,
        buttons, NULL };
    int pressed = 0;
    if (!SDL_ShowMessageBox(&box, &pressed)) {
        host_logf(HOST_LOG_WARN, "cloud saves: could not show a message: %s", SDL_GetError());
        return 0;
    }
    return pressed;
}

static NSString *cloud_describe(const struct cloud_summary *summary, BOOL this_device, BOOL saved_game) {
    static NSString *const difficulties[] = { @"Easy", @"Normal", @"Heroic", @"Legendary" };
    NSString *who = this_device ? @"this device" : (summary->device[0] ? @(summary->device) : @"another device");
    if (!saved_game)
        return [NSString stringWithFormat:@"%@ — %d level%s finished", who, summary->levels, summary->levels == 1 ? "" : "s"];
    NSString *title = @(cloud_level_title(summary->checkpoint_map)) ?: @"a level";
    return summary->difficulty >= 0 && summary->difficulty < 4 ?
        [NSString stringWithFormat:@"%@ — checkpoint on %@, %@", who, title, difficulties[summary->difficulty]] :
        [NSString stringWithFormat:@"%@ — checkpoint on %@", who, title];
}

/* what the passes so far left for the player, before the game starts */
static void cloud_show_notices(void) {
    for (int shown = 0; shown < 8 && atomic_load(&cloud_active) && host_cloud_saves_enabled(); shown++) {
        __block struct cloud_notice notice;
        __block int have = 0;
        if (!cloud_wait(5, ^{ have = cloud_engine_notice(cloud_engine, &notice); }) || !have ||
            !atomic_load(&cloud_active) || !host_cloud_saves_enabled()) return;
        const struct cloud_notice current = notice;
        switch (current.kind) {
        case CLOUD_NOTICE_CONFLICT: {
            BOOL saved_game = !strncmp(current.unit, "save-", 5);
            NSString *name = current.kept.player[0] ? @(current.kept.player) : nil;
            NSString *message = [NSString stringWithFormat:@"%@ played on two devices.\n\nKept: %@.\nSet aside: %@.",
                saved_game ? [NSString stringWithFormat:@"The saved checkpoint of “%@” was", name ?: @"a profile"] :
                    [NSString stringWithFormat:@"“%@” was", name ?: @"A save"],
                cloud_describe(&current.kept, current.kept_local, saved_game),
                cloud_describe(&current.aside, !current.kept_local, saved_game)];
            if (cloud_ask(message, "OK", "Use the other one") == 1) {
                __block enum cloud_status status = CLOUD_RETRY;
                if (!cloud_wait(CLOUD_ANSWER_SECONDS, ^{ status = cloud_engine_restore_copy(cloud_engine, current.unit); }) ||
                    status != CLOUD_OK)
                    cloud_ask(@"The other one could not be fetched from iCloud now. It is still there: turn on "
                        "“Restore the last set-aside save” in VISR's page in Settings to try again.", "OK", NULL);
            }
            break;
        }
        case CLOUD_NOTICE_QUOTA:
            cloud_ask(@"Your iCloud storage is full. Saves on this device are not being copied to iCloud until there is space.",
                "OK", NULL);
            break;
        case CLOUD_NOTICE_NEWER_FORMAT:
            cloud_ask(@"Some saves in iCloud were made by a newer version of VISR. They are left alone on this device until "
                "VISR is updated here.", "OK", NULL);
            break;
        case CLOUD_NOTICE_ACCOUNT_CHANGED:
        case CLOUD_NOTICE_CLOUD_DELETED: {
            BOOL account = current.kind == CLOUD_NOTICE_ACCOUNT_CHANGED;
            int carry_on = account ?
                cloud_ask(@"This device is signed in to a different iCloud account than the one its saves were synced with.",
                    "Keep them on this device only", "Add them to this account") :
                cloud_ask(@"VISR's saves were deleted from iCloud. The saves on this device are untouched.",
                    "Turn iCloud saves off", "Upload them again");
            cloud_wait(CLOUD_ANSWER_SECONDS, ^{
                cloud_engine_answer(cloud_engine, carry_on);
                if (carry_on)
                    cloud_pass(CLOUD_PASS_LAUNCH);
                else {
                    [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"cloud_saves"];
                    cloud_publish(@"Off");
                    cloud_stop();
                }
            });
            /* (cloud_engine_answer dropped the notice) */
            continue;
        }
        case CLOUD_NOTICE_RESTORE_INCOMPLETE:
            break;
        }
        cloud_wait(5, ^{ cloud_engine_notice_done(cloud_engine); });
    }
}

static void cloud_scan(void (^then)(void)) {
    dispatch_async(cloud_queue, ^{
        cloud_pass(CLOUD_PASS_RUNNING);
        if (then) then();
    });
}

void host_cloud_saves_before_game(void) {
    if (!host_cloud_saves_sync_allowed()) return;
    int seconds = cloud_local_slot_count(cloud_save_root) ? CLOUD_WAIT_SECONDS : CLOUD_WAIT_EMPTY_SECONDS;
    if (cloud_wait(seconds, nil))
        cloud_show_notices();
    else
        host_logf(HOST_LOG_WARN, "cloud saves: iCloud did not answer within %d s; the game starts with the saves on this "
            "device, and what iCloud holds is looked at again at the next launch", seconds);
    /* from here on only the game changes the local saves; a replace in progress finishes first */
    pthread_mutex_lock(&cloud_gate);
    cloud_game_started = 1;
    pthread_mutex_unlock(&cloud_gate);
    if (!atomic_load(&cloud_active)) return;
    cloud_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, cloud_queue);
    dispatch_source_set_timer(cloud_timer, dispatch_time(DISPATCH_TIME_NOW, CLOUD_SCAN_SECONDS * NSEC_PER_SEC),
        CLOUD_SCAN_SECONDS * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(cloud_timer, ^{ cloud_pass(CLOUD_PASS_RUNNING); });
    dispatch_resume(cloud_timer);
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:NSUserDefaultsDidChangeNotification object:NSUserDefaults.standardUserDefaults queue:nil
        usingBlock:^(NSNotification *note) { (void)note; host_cloud_saves_sync_allowed(); }];
#if HALO_NATIVE_MAC
    [center addObserverForName:NSApplicationWillResignActiveNotification object:nil queue:nil
        usingBlock:^(NSNotification *note) { (void)note; if (atomic_load(&cloud_active)) cloud_scan(nil); }];
#else
    [center addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil
        usingBlock:^(NSNotification *note) { (void)note; if (atomic_load(&cloud_active)) cloud_scan(nil); }];
    [center addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil
        usingBlock:^(NSNotification *note) {
            (void)note;
            if (!atomic_load(&cloud_active)) return;
            /* the upload gets what time the system gives an app that has left the screen */
            __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
            void (^end)(void) = ^{
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (task != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:task];
                    task = UIBackgroundTaskInvalid;
                });
            };
            task = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"VISRCloudSaves" expirationHandler:end];
            cloud_scan(end);
        }];
#endif
    /* the switch turned off in Settings while the app was away stops the uploads at once; turned on, it
       takes effect at the next launch, when the saves may be compared before the game runs */
#if HALO_NATIVE_MAC
    NSString *foreground = NSApplicationWillBecomeActiveNotification;
#else
    NSString *foreground = UIApplicationWillEnterForegroundNotification;
#endif
    [center addObserverForName:foreground object:nil queue:nil
        usingBlock:^(NSNotification *note) {
            (void)note;
            if (atomic_load(&cloud_active) && ![NSUserDefaults.standardUserDefaults boolForKey:@"cloud_saves"]) {
                host_logf(HOST_LOG_INFO, "cloud saves: turned off in Settings");
                cloud_publish(@"Off");
                atomic_store(&cloud_active, 0);
                dispatch_async(cloud_queue, ^{ cloud_stop(); });
            }
        }];
}

void host_cloud_saves_directory_removed(const char *path) {
    char slot[CLOUD_SLOT_ID_SIZE];
    if (!cloud_state_path[0] || !cloud_slot_of_path(cloud_save_root, path, slot)) return;
    /* written before the game goes on, so that a crash right after cannot turn the deletion into a
       missing save that is restored */
    cloud_note_local_delete(cloud_state_path, slot);
}

void host_cloud_saves_leaving(double seconds) {
    if (!host_cloud_saves_sync_allowed() || !cloud_queue) return;
    pthread_mutex_lock(&cloud_gate);
    int started = cloud_game_started;
    pthread_mutex_unlock(&cloud_gate);
    if (!started) return;
    /* the game has stopped (this is its thread, or the main thread ending the app), so its files are whole */
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    dispatch_async(cloud_queue, ^{
        cloud_pass(CLOUD_PASS_LEAVING);
        dispatch_semaphore_signal(finished);
    });
    if (dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC))) != 0)
        host_logf(HOST_LOG_INFO, "cloud saves: the last upload did not finish in %.0f s; it is made at the next launch", seconds);
}
