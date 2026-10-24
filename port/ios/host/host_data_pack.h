#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

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
@end

NS_ASSUME_NONNULL_END
