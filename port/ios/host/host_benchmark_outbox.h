/* Immutable reports survive replacement of benchmark/report.json and failed delivery. */
#import <Foundation/Foundation.h>
NSString *host_benchmark_outbox_store(NSString *folder, NSData *report, NSError **error);
NSArray<NSString *> *host_benchmark_outbox_pending(NSString *folder);
BOOL host_benchmark_outbox_acknowledge(NSString *path, NSError **error);

/* Copy and verify the old raw suite before an incompatible snapshot can replace it. */
BOOL host_benchmark_archive_interrupted(NSString *folder, NSError **error);

/* Each report family validates its own immutable payload and owns its record prefix. */
typedef BOOL (^HostOutboxValidator)(NSDictionary *payload);
NSString *host_typed_outbox_store(NSString *folder, NSData *payload, NSString *prefix, HostOutboxValidator validate, NSError **error);
NSArray<NSString *> *host_typed_outbox_pending(NSString *folder, NSString *prefix);
