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
- (void)markHealthy;
- (BOOL)stageEnvelope:(NSData *)data error:(NSError * _Nullable * _Nullable)error;
- (void)checkForUpdateAtURL:(NSURL *)url;
- (NSDictionary *)metadata;
@end

NS_ASSUME_NONNULL_END
