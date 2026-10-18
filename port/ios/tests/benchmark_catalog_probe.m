#import <Foundation/Foundation.h>
#include "host_benchmark_catalog.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3 || argc - 3 > BENCHMARK_SCENES_MAXIMUM) return 2;
    @autoreleasepool {
        struct host_benchmark_catalog catalog;
        NSError *error = nil;
        NSString *folder = [NSString stringWithUTF8String:argv[1]];
        enum benchmark_mode mode = !strcmp(argv[2], "full") ? BENCHMARK_FULL : BENCHMARK_QUICK;
        const char *filters[BENCHMARK_SCENES_MAXIMUM];
        for (int index = 3; index < argc; index++) filters[index - 3] = argv[index];
        if (!host_benchmark_catalog_load(folder, &catalog, &error)) {
            fprintf(stderr, "%s\n", error.localizedDescription.UTF8String ?: "catalog unavailable");
            return 1;
        }
        struct benchmark_scene selected[BENCHMARK_SCENES_MAXIMUM];
        int count = benchmark_scenes_select(mode, catalog.scenes, catalog.count, argc > 3 ? filters : NULL,
            argc - 3, selected, BENCHMARK_SCENES_MAXIMUM);
        if (count < 0) return 1;
        printf("sha256=%s\n", catalog.sha256);
        for (int index = 0; index < count; index++)
            printf("%s\t%d\t%s\t%d\t%d\t%s\t%d\t%d\n", selected[index].id, selected[index].kind,
                selected[index].simulation ?: "", selected[index].players, selected[index].views,
                selected[index].game_mode ?: "", selected[index].score_limit, selected[index].time_limit_minutes);
    }
    return 0;
}
