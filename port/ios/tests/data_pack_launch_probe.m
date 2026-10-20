/* Real presentation wrappers and signed store transitions, without a GPU or app cache. */
#import "../host/host_data_pack.h"
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include "../../third_party/monocypher/monocypher-ed25519.h"
#include "../host/host_gpu_dispatch.c"
#include <assert.h>

static HostDataPackStore *launchStore;
static int healthSignals;
void host_logf(int priority, const char *format, ...) { (void)priority; (void)format; }
const struct gpu_backend gpu_backend_metal = {0}, gpu_backend_gl = {0};
void host_lifecycle_hold(void) {}
void host_frame_counter_show(unsigned long frame) { (void)frame; }
void host_benchmark_frame(double interval, double cpu, double gpu, double refresh, double simulation, double render, int ticks) {
    (void)interval; (void)cpu; (void)gpu; (void)refresh; (void)simulation; (void)render; (void)ticks;
}
void host_frame_cpu_split(double *simulation, double *render, int *ticks) { *simulation = *render = -1.0; *ticks = 0; }
void host_data_updates_healthy(void) { healthSignals++; [launchStore markHealthy]; }
static uint32_t Present(gpu_texture picture) { (void)picture; assert(healthSignals == 0); return 0; }
static uint32_t PresentStereo(const struct gpu_stereo_present *picture) { (void)picture; assert(healthSignals == 0); return 0; }
static void Context(uint32_t scene, uint32_t rate, const char *level) { (void)scene; (void)rate; (void)level; }
static const struct gpu_backend renderer = {.present = Present, .present_stereo = PresentStereo, .frame_context = Context};

static HostDataPackStore *Store(NSString *root, NSData *key) {
    return [[HostDataPackStore alloc] initWithRoot:root bundled:@"/bundled" kind:@"profiles" channel:@"stable"
        publicKey:key compatibility:@"profiles-2" validator:^BOOL(NSString *folder, NSError **error) {
            (void)error;
            return [NSFileManager.defaultManager fileExistsAtPath:[folder stringByAppendingPathComponent:@"test.toml"]];
        }];
}
static NSData *Envelope(uint8_t *secret) {
    NSData *payload = [NSJSONSerialization dataWithJSONObject:@{@"files": @{@"test.toml": @"eA=="}} options:0 error:NULL];
    uint8_t hash[CC_SHA256_DIGEST_LENGTH], signature[64];
    CC_SHA256(payload.bytes, (CC_LONG)payload.length, hash);
    NSMutableString *digest = [NSMutableString string];
    for (NSUInteger i = 0; i < sizeof(hash); i++) [digest appendFormat:@"%02x", hash[i]];
    NSData *manifest = [NSJSONSerialization dataWithJSONObject:@{@"schema": @1, @"kind": @"profiles", @"channel": @"stable",
        @"sequence": @1, @"minimum_engine": @1, @"maximum_engine": @1, @"compatibility": @"profiles-2", @"sha256": digest, @"notes": @"test"} options:0 error:NULL];
    crypto_ed25519_sign(signature, secret, manifest.bytes, manifest.length);
    return [NSJSONSerialization dataWithJSONObject:@{@"manifest": [manifest base64EncodedStringWithOptions:0],
        @"signature": [[NSData dataWithBytes:signature length:64] base64EncodedStringWithOptions:0],
        @"payload": [payload base64EncodedStringWithOptions:0]} options:0 error:NULL];
}
int main(void) { @autoreleasepool {
    uint8_t seed[32] = {2}, secret[64], public[32]; // Public fixture, never a release key.
    crypto_ed25519_key_pair(secret, public, seed);
    NSData *key = [NSData dataWithBytes:public length:32], *envelope = Envelope(secret);
    for (int mode = 0; mode < 7; mode++) {
        /* Menu, direct gameplay, opening cutscene/stereo, loading-only,
           failure before rendering, failure before presentation, and a loading frame after an abandoned world frame. */
        const char *levels[] = {"ui", "b30", "a10", "b30", "", "b30", "b30"};
        NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        launchStore = Store(root, key);
        assert([launchStore stageEnvelope:envelope error:NULL]);
        NSString *trial = [launchStore beginLaunch];
        updates_healthy = 0;
        ready_frame = 0;
        healthSignals = 0;
        wrapped_backend = &renderer;
        backend = &renderer;
        metal_backend = 1;
        gpu_frame_context(mode == 0 ? 0 : mode == 2 ? 1 : 2, 0, levels[mode]);
        /* Startup and loading frames do not qualify, even with a named level. */
        present_wrapped(1);
        present_stereo_wrapped(NULL);
        assert(healthSignals == 0);
        if (mode < 3 || mode >= 5) host_data_updates_frame_ready(1);
        assert(healthSignals == 0); // Rendering alone does not accept the pack.
        if (mode == 6) {
            host_data_updates_frame_ready(0);
            present_wrapped(1);
            present_stereo_wrapped(NULL);
            assert(healthSignals == 0);
        }
        if (mode < 3) {
            if (mode == 2) present_stereo_wrapped(NULL); else present_wrapped(1);
            assert(healthSignals == 1);
            assert([[Store(root, key) beginLaunch] isEqual:trial]);
        } else {
            assert([[Store(root, key) beginLaunch] isEqual:@"/bundled"]);
            assert(![launchStore stageEnvelope:envelope error:NULL]);
        }
        [NSFileManager.defaultManager removeItemAtPath:root error:NULL];
    }
    puts("PASS: menu, gameplay and stereo retain signed packs; loading and failed launches roll back");
} return 0; }
