/* Import only explicitly selected disc data into this app's own local folder. */
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "ios_host.h"
#include "xiso.h"
#include <stdatomic.h>

@interface VISRMacImport : NSObject {
@public
    atomic_int canceled;
    atomic_int completed;
}
- (void)cancel:(id)sender;
@end
@implementation VISRMacImport
- (void)cancel:(id)sender { (void)sender; atomic_store(&canceled, 1); }
@end
static int progress(void *context, const char *name, uint64_t done, uint64_t total) {
    (void)name; (void)done; (void)total;
    VISRMacImport *operation = (__bridge VISRMacImport *)context;
    return !atomic_load(&operation->canceled);
}
/* Publishing is separate from extraction. A failed move must retain the previous maps. */
static BOOL publish(NSString *staging, NSString *root, NSError **error) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *maps = [root stringByAppendingPathComponent:@"maps"];
    NSString *incoming = [staging stringByAppendingPathComponent:@"maps"];
    NSString *backup = [staging stringByAppendingPathComponent:@"previous-maps"];
    if (!xiso_maps_ready(incoming.fileSystemRepresentation, NULL, 0)) return NO;
    BOOL existing = [files fileExistsAtPath:maps];
    if (existing && ![files moveItemAtPath:maps toPath:backup error:error]) return NO;
    if (![files moveItemAtPath:incoming toPath:maps error:error]) {
        if (existing && ![files moveItemAtPath:backup toPath:maps error:nil])
            host_logf(HOST_LOG_ERROR, "Import rollback failed; original maps preserved in %s", backup.fileSystemRepresentation);
        return NO;
    }
    NSString *movies = [root stringByAppendingPathComponent:@"bink"];
    NSString *incoming_movies = [staging stringByAppendingPathComponent:@"bink"];
    if (![files fileExistsAtPath:movies] && [files fileExistsAtPath:incoming_movies])
        [files moveItemAtPath:incoming_movies toPath:movies error:nil];
    return YES;
}
static void recover(NSString *root) {
    NSFileManager *files = NSFileManager.defaultManager;
    for (NSString *name in [files contentsOfDirectoryAtPath:root error:nil]) {
        if (![name hasPrefix:@".visr-import-"] || ![[NSUUID alloc] initWithUUIDString:[name substringFromIndex:13]]) continue;
        NSString *staging = [root stringByAppendingPathComponent:name];
        NSString *marker = [staging stringByAppendingPathComponent:@"owner.txt"];
        if (![[NSString stringWithContentsOfFile:marker encoding:NSUTF8StringEncoding error:nil] isEqualToString:@"VISR native import v1"]) continue;
        NSString *backup = [staging stringByAppendingPathComponent:@"previous-maps"];
        NSString *maps = [root stringByAppendingPathComponent:@"maps"];
        if ([files fileExistsAtPath:backup]) {
            if (![files fileExistsAtPath:maps]) [files moveItemAtPath:backup toPath:maps error:nil];
            /* Preserve an old set whenever recovery cannot establish a valid current one. */
            if ([files fileExistsAtPath:backup] && !xiso_maps_ready(maps.fileSystemRepresentation, NULL, 0)) continue;
        }
        [files removeItemAtPath:staging error:nil];
    }
}
void host_ios_prepare_assets(const char *directory) {
    NSString *root = @(directory);
    NSFileManager *files = NSFileManager.defaultManager;
    recover(root);
    NSString *maps = [root stringByAppendingPathComponent:@"maps"];
    if (xiso_maps_ready(maps.fileSystemRepresentation, NULL, 0)) return;
    while (1) {
        NSOpenPanel *picker = [NSOpenPanel openPanel];
        picker.title = @"Choose your Halo disc image";
        picker.message = @"Select an original Xbox PAL or NTSC-US ISO or XISO. No game files are included.";
        picker.canChooseDirectories = NO; picker.allowsMultipleSelection = NO;
        /* XISO providers may report a generic data type. The extractor validates content. */
        picker.allowedContentTypes = @[UTTypeData];
        if ([picker runModal] != NSModalResponseOK) host_exit(0);
        NSURL *url = picker.URL;
        BOOL scoped = [url startAccessingSecurityScopedResource];
        NSString *staging = [root stringByAppendingPathComponent:[@".visr-import-" stringByAppendingString:NSUUID.UUID.UUIDString]];
        NSError *error = nil;
        if (![files createDirectoryAtPath:staging withIntermediateDirectories:NO attributes:nil error:&error] ||
            ![@"VISR native import v1" writeToFile:[staging stringByAppendingPathComponent:@"owner.txt"] atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
            if (scoped) [url stopAccessingSecurityScopedResource];
            host_alert("Could not start import", error.localizedDescription.UTF8String, "OK", NULL);
            continue;
        }
        NSAlert *dialog = [NSAlert new]; dialog.messageText = @"Importing Halo";
        dialog.informativeText = @"Your original disc image stays unchanged.";
        [dialog addButtonWithTitle:@"Cancel"];
        NSProgressIndicator *spinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, 240, 20)];
        spinner.indeterminate = YES; spinner.style = NSProgressIndicatorStyleBar;
        dialog.accessoryView = spinner; [spinner startAnimation:nil];
        VISRMacImport *operation = [VISRMacImport new];
        NSButton *cancel = dialog.buttons.firstObject; cancel.target = operation; cancel.action = @selector(cancel:);
        __block BOOL modal_running = YES;
        __block BOOL imported = NO;
        __block NSString *failure;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                char message[1024] = {0};
                imported = xiso_extract_maps(url.fileSystemRepresentation, staging.fileSystemRepresentation,
                    progress, (__bridge void *)operation, message, sizeof(message));
                if (!imported) failure = @(message);
                if (atomic_load(&operation->canceled)) { imported = NO; failure = @"Import canceled."; }
                atomic_store(&operation->completed, 1);
                dispatch_async(dispatch_get_main_queue(), ^{ if (modal_running) [NSApp stopModal]; });
            }
        });
        [dialog runModal];
        modal_running = NO;
        if (!atomic_load(&operation->completed)) atomic_store(&operation->canceled, 1);
        /* A keyboard dismissal must not release security scope or staging while the worker still reads them. */
        while (!atomic_load(&operation->completed))
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        [dialog.window orderOut:nil];
        if (scoped) [url stopAccessingSecurityScopedResource];
        if (imported && !publish(staging, root, &error)) {
            imported = NO; failure = error.localizedDescription ?: @"Could not finish the import. Existing data was preserved.";
        }
        /* A failed rollback leaves its unique backup intact for the next startup. */
        if (imported || ![files fileExistsAtPath:[staging stringByAppendingPathComponent:@"previous-maps"]])
            [files removeItemAtPath:staging error:nil];
        if (imported) { host_logf(HOST_LOG_INFO, "XISO import complete; original map bytes preserved"); return; }
        if (host_alert("Import did not complete", failure.UTF8String, "Try Again", "Quit") != 0) host_exit(0);
    }
}
