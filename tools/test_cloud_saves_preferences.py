"""Run the host's preference and launch gate functions without an iCloud account."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@pytest.mark.skipif(sys.platform != "darwin" or not shutil.which("xcrun"), reason="Foundation probe requires macOS")
def test_host_requires_opt_in_and_reenable_waits_for_launch(tmp_path):
    host = (ROOT / "port/ios/host/host_cloud_saves.m").read_text()
    functions = "\n".join(function(host, signature) for signature in [
        "static void cloud_publish(", "static void cloud_stop(", "int host_cloud_saves_enabled(",
        "int host_cloud_saves_sync_allowed(",
        "void host_cloud_saves_set_enabled(", "const char *host_cloud_saves_status(",
    ])
    # End at the first entitlement lookup. Reaching it represents cloud startup;
    # the rest of startup needs an account and is covered by the Apple builds.
    start = function(host, "void host_cloud_saves_start(")
    start = start[:start.index("    int in_profile =")] + "    startup_calls++;\n}"
    source = r'''
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HOST_LOG_INFO 0
#define host_logf(...) ((void)0)
static _Atomic int cloud_active;
static dispatch_queue_t cloud_queue;
static dispatch_source_t cloud_timer;
static int startup_calls;
int host_cloud_saves_enabled(void);
/* Use an isolated volatile preferences domain: no account or player preferences. */
@interface ProbeDefaults : NSObject
@property(class, readonly) ProbeDefaults *standardUserDefaults;
- (void)registerDefaults:(NSDictionary *)values;
- (BOOL)boolForKey:(NSString *)key;
- (NSString *)stringForKey:(NSString *)key;
- (void)setBool:(BOOL)value forKey:(NSString *)key;
- (void)setObject:(id)value forKey:(NSString *)key;
@end
@implementation ProbeDefaults {
    NSMutableDictionary *stored;
    NSMutableDictionary *registered;
}
+ (ProbeDefaults *)standardUserDefaults {
    static ProbeDefaults *value;
    if (!value) { value = [ProbeDefaults new]; value->stored = [NSMutableDictionary new]; value->registered = [NSMutableDictionary new]; }
    return value;
}
- (void)registerDefaults:(NSDictionary *)values { [registered addEntriesFromDictionary:values]; }
- (BOOL)boolForKey:(NSString *)key { return [(stored[key] ?: registered[key]) boolValue]; }
- (NSString *)stringForKey:(NSString *)key { return stored[key] ?: registered[key]; }
- (void)setBool:(BOOL)value forKey:(NSString *)key { stored[key] = @(value); }
- (void)setObject:(id)value forKey:(NSString *)key { stored[key] = value; }
@end
'''
    source += (functions + "\n" + start).replace("NSUserDefaults", "ProbeDefaults")
    source += r'''
int main(void) { @autoreleasepool {
    assert(!host_cloud_saves_enabled());
    host_cloud_saves_start("unused", "unused");
    assert(!startup_calls && !host_cloud_saves_enabled());
    assert(!strcmp(host_cloud_saves_status(), "Off"));
    [ProbeDefaults.standardUserDefaults setBool:YES forKey:@"cloud_saves"];
    assert(!strcmp(host_cloud_saves_status(), "Starts next launch"));
    host_cloud_saves_set_enabled(1);
    assert(host_cloud_saves_enabled() && !atomic_load(&cloud_active));
    assert(!strcmp(host_cloud_saves_status(), "Starts next launch"));
    host_cloud_saves_start("unused", "unused");
    assert(startup_calls == (getenv("PROBE_SUPPRESS") ? 0 : 1));
    atomic_store(&cloud_active, 1);
    host_cloud_saves_set_enabled(0);
    assert(!host_cloud_saves_enabled() && !atomic_load(&cloud_active));
    cloud_publish(@"Up to date");
    assert(!strcmp(host_cloud_saves_status(), "Off"));
    host_cloud_saves_start("unused", "unused");
    assert(startup_calls == (getenv("PROBE_SUPPRESS") ? 0 : 1));
    host_cloud_saves_set_enabled(1);
    assert(!atomic_load(&cloud_active) && !host_cloud_saves_sync_allowed());
    atomic_store(&cloud_active, 1);
    [ProbeDefaults.standardUserDefaults setBool:NO forKey:@"cloud_saves"];
    assert(!host_cloud_saves_sync_allowed());
    [ProbeDefaults.standardUserDefaults setBool:YES forKey:@"cloud_saves"];
    assert(!host_cloud_saves_sync_allowed());
} return 0; }
'''
    path = tmp_path / "preferences.m"
    path.write_text(source)
    binary = tmp_path / "preferences"
    subprocess.run(["xcrun", "clang", "-fobjc-arc", "-fblocks", "-framework", "Foundation", str(path), "-o", str(binary)], check=True)
    # Even this environment value must not opt the player in.
    environment = {key: value for key, value in os.environ.items()
                   if key not in {"HALO_RUNNER", "HALO_BENCHMARK_SUITE", "HALO_CLOUD_SAVES", "PROBE_SUPPRESS"}}
    for overrides in [{"HALO_CLOUD_SAVES": "1"},
                      {"HALO_CLOUD_SAVES": "0", "PROBE_SUPPRESS": "1"},
                      {"HALO_RUNNER": "1", "PROBE_SUPPRESS": "1"},
                      {"HALO_BENCHMARK_SUITE": "quick", "PROBE_SUPPRESS": "1"}]:
        subprocess.run([str(binary)], env={**environment, **overrides}, check=True)
