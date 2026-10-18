#import <Foundation/Foundation.h>
#include "../../runtime/benchmark_plan.h"

#define HOST_BENCHMARK_CATALOG_NAME "benchmark-catalog.json"

struct host_benchmark_catalog {
    int count;
    struct benchmark_scene scenes[BENCHMARK_SCENES_MAXIMUM];
    char ids[BENCHMARK_SCENES_MAXIMUM][48];
    char maps[BENCHMARK_SCENES_MAXIMUM][32];
    char difficulties[BENCHMARK_SCENES_MAXIMUM][16];
    char recordings[BENCHMARK_SCENES_MAXIMUM][64];
    char simulations[BENCHMARK_SCENES_MAXIMUM][32];
    char game_modes[BENCHMARK_SCENES_MAXIMUM][32];
    char sha256[65];
};

BOOL host_benchmark_catalog_load(NSString *folder, struct host_benchmark_catalog *catalog, NSError **error);
BOOL host_benchmark_catalog_validate_folder(NSString *folder, NSError **error);
NSString *host_benchmark_catalog_file_sha256(NSString *folder);
