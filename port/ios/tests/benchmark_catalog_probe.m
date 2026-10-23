#import <Foundation/Foundation.h>
#include "host_benchmark_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3 || argc - 3 > BENCHMARK_SCENES_MAXIMUM) return 2;
    @autoreleasepool {
        struct host_benchmark_catalog catalog;
        NSError *error = nil;
        NSString *folder = [NSString stringWithUTF8String:argv[1]];
        if (getenv("VISR_PROBE_MENU_CHOICES"))
            printf("menu_choices=%lu\n", (unsigned long)host_benchmark_menu_choice_count(folder));
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
        if (getenv("VISR_PROBE_VARIANTS")) {
            int indexes[HOST_BENCHMARK_VARIANTS_MAXIMUM];
            int matched = host_benchmark_catalog_variants_for(&catalog, getenv("VISR_PROBE_VARIANTS"), indexes);
            printf("variants=%d matched=%d\n", catalog.variant_count, matched);
            for (int index = 0; index < matched; index++) {
                const struct host_benchmark_variant *variant = &catalog.variants[indexes[index]];
                printf("variant=%s:%d:%d:%d\n", variant->id, variant->render_height, variant->output_height, variant->upscaler);
            }
            return 0;
        }
        if (getenv("VISR_PROBE_SIM_SETTINGS")) {
            for (int index = 0; index < count; index++)
                printf("sim=%s:%u:%u\n", selected[index].id, selected[index].sim_experiments,
                    selected[index].sim_experiments_mask);
            return 0;
        }
        for (int index = 0; index < count; index++)
            printf("%s\t%d\t%s\t%d\t%d\t%s\t%d\t%d\t%d\t%.3f\t%.3f\t%.3f\n", selected[index].id, selected[index].kind,
                selected[index].simulation ?: "", selected[index].players, selected[index].views,
                selected[index].game_mode ?: "", selected[index].score_limit, selected[index].time_limit_minutes,
                selected[index].arena_columns, selected[index].arena_spacing, selected[index].arena_distance,
                selected[index].arena_movement_radius);
    }
    return 0;
}
