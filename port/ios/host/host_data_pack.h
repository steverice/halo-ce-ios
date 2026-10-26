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
// Menu candidates remain pending until the user starts Play or Benchmark.
// These methods validate/read disk and must run off the presentation thread.
- (NSDictionary *)menuSnapshot;
- (void)selectMenuFolder:(NSString *)folder;
// Release candidates after the manager has atomically published its selection.
- (void)retainMenuFolder:(NSString *)folder;
- (void)markHealthy;
- (BOOL)stageEnvelope:(NSData *)data error:(NSError * _Nullable * _Nullable)error;
- (void)checkForUpdateAtURL:(NSURL *)url;
- (void)checkForUpdateAtURL:(NSURL *)url completion:(void (^ _Nullable)(BOOL success))completion;
- (NSDictionary *)metadata;
// The HTTP status the last feed check's first request was answered with, read in its completion: 0 when no answer
// came (offline, timed out, refused), so a caller can tell a feed that has nothing for it (404) from one it cannot reach.
@property(atomic, readonly) NSInteger lastFeedStatus;
// Optional, set before the first check: given an authenticated envelope's decoded files, whether they are older than
// what the store's bundled folder already holds. Such an envelope is refused before its sequence is compared, so a
// stale shard is recognized even when its sequence was used up by an earlier refusal.
@property(nonatomic, copy, nullable) BOOL (^supersededCheck)(NSDictionary<NSString *, NSData *> *files);
// Whether the last feed check's envelope was refused by supersededCheck: the feed answered, and the bundled data is
// newer than what it serves.
@property(atomic, readonly) BOOL lastFeedSuperseded;
// The visitor runs under the store lock so pruning cannot remove files during resolution.
// It sees the authenticated, fully validated pending generation, or the launch selection.
- (void)visitPendingSnapshot:(void (^)(NSString *folder, NSDictionary *identity))visitor;
@end

NS_ASSUME_NONNULL_END
