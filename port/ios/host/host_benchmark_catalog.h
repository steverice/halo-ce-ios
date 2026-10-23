#import <Foundation/Foundation.h>
#include "../../runtime/benchmark_plan.h"

#define HOST_BENCHMARK_CATALOG_NAME "benchmark-catalog.json"
/* logged when a build that carries routes runs benchmark/local-routes' unsigned catalog; release validation
   (tools/release_validation.py) refuses an app whose executable holds this text */
#define HOST_BENCHMARK_LOCAL_CATALOG_NOTE "unsigned local catalog: benchmark/local-routes (development builds only)"

#include "host_benchmark_variant.h"

struct host_benchmark_catalog {
    int count;
    int variant_count;
    struct host_benchmark_variant variants[HOST_BENCHMARK_VARIANTS_MAXIMUM];
    struct benchmark_scene scenes[BENCHMARK_SCENES_MAXIMUM];
    char ids[BENCHMARK_SCENES_MAXIMUM][48];
    char workloads[BENCHMARK_SCENES_MAXIMUM][48];
    char maps[BENCHMARK_SCENES_MAXIMUM][32];
    char difficulties[BENCHMARK_SCENES_MAXIMUM][16];
    char recordings[BENCHMARK_SCENES_MAXIMUM][64];
    char anchors[BENCHMARK_SCENES_MAXIMUM][48];
    char simulations[BENCHMARK_SCENES_MAXIMUM][32];
    char game_modes[BENCHMARK_SCENES_MAXIMUM][32];
    char sha256[65];
};

BOOL host_benchmark_catalog_load(NSString *folder, struct host_benchmark_catalog *catalog, NSError **error);
/* the indexes in catalog->variants of the variants a device of model runs, in order; their number */
int host_benchmark_catalog_variants_for(const struct host_benchmark_catalog *catalog, const char *model,
    int indexes[HOST_BENCHMARK_VARIANTS_MAXIMUM]);
BOOL host_benchmark_catalog_validate_folder(NSString *folder, NSError **error);
NSString *host_benchmark_catalog_file_sha256(NSString *folder);
NSUInteger host_benchmark_menu_choice_count(NSString *selected_folder);
