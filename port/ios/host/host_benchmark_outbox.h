/* Immutable reports survive replacement of benchmark/report.json and failed delivery. */
#import <Foundation/Foundation.h>
NSString *host_benchmark_outbox_store(NSString *folder, NSData *report, NSError **error);
NSArray<NSString *> *host_benchmark_outbox_pending(NSString *folder);
BOOL host_benchmark_outbox_acknowledge(NSString *path, NSError **error);

/* Copy and verify the old raw suite before an incompatible snapshot can replace it. */
BOOL host_benchmark_archive_interrupted(NSString *folder, NSError **error);
