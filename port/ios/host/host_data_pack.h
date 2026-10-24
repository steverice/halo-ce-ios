#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSString * const HostDataPackSnapshotStagedNotification;

typedef BOOL (^HostPackValidator)(NSString *folder, NSError **error);

// Each instance serves one kind/channel and freezes its selected folder for a launch.
// Use separate roots for different kinds or channels. Pack files contain data only.
@interface HostDataPackStore : NSObject
- (instancetype)initWithRoot:(NSString *)root
                     bundled:(NSString *)bundled
                        kind:(NSString *)kind
                     channel:(NSString *)channel
                   publicKey:(NSData *)key
               compatibility:(NSString *)compatibility
                   validator:(HostPackValidator)validator;
- (NSString *)beginLaunch;
- (void)markHealthy;
- (BOOL)stageEnvelope:(NSData *)data error:(NSError * _Nullable * _Nullable)error;
- (void)checkForUpdateAtURL:(NSURL *)url;
- (NSDictionary *)metadata;
// The visitor runs under the store lock so pruning cannot remove files during resolution.
// It sees the authenticated, fully validated pending generation, or the launch selection.
- (void)visitPendingSnapshot:(void (^)(NSString *folder, NSDictionary *identity))visitor;
@end

NS_ASSUME_NONNULL_END
