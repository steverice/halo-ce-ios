/* Portable collector contract: bounded append, immutable generations, dependencies. */
#include "shader_observations.h"
#include "../../linux/src/shader_observation_path.h"
#include "../host/host_shader_official_maps.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct shader_observation_collector collector;
int main(void)
{
    struct shader_observation_generation generation = {.token=1, .fingerprint="abc", .map_key="m_abc"};
    struct shader_observation descriptor = {.kind=SHADER_OBSERVATION_VERTEX, .metal=1, .vertex_hash=42};
    struct shader_observation_item item;
    char line[SHADER_OBSERVATION_LINE_MAX], vertex[SHADER_OBSERVATION_LINE_MAX], pixel[SHADER_OBSERVATION_LINE_MAX];
    unsigned i;
    char path[1024];
    assert(shader_observation_map_path("levels\\ui\\ui", "d:\\maps\\", NULL, path, sizeof(path)));
    assert(!strcmp(path, "d:\\maps\\ui.map"));
    assert(shader_observation_map_path("levels\\a10\\a10", "d:\\maps_fr\\", NULL, path, sizeof(path)));
    assert(!strcmp(path, "d:\\maps_fr\\a10.map"));
    assert(shader_observation_map_path("custom_maps\\same", "d:\\maps\\", "d:\\custom_maps\\pack-one\\same.map", path, sizeof(path)));
    assert(!strcmp(path, "d:\\custom_maps\\pack-one\\same.map"));
    assert(shader_observation_map_path("custom_maps\\same", "d:\\maps\\", "h:\\maps\\pack-two\\same.map", path, sizeof(path)));
    assert(!strcmp(path, "h:\\maps\\pack-two\\same.map"));
    assert(!shader_observation_map_path("levels\\a10\\a10", "d:\\maps\\", NULL, path, 4));
    assert(!path[0]);
    assert(!strcmp(host_shader_official_alias("f7461e5b6eaf0314c4f03a1c6e43752b06ae3e4782b70b240a8c15e5f634b3ce"), "a10"));
    assert(host_shader_official_alias("0000000000000000000000000000000000000000000000000000000000000000") == NULL);
    assert(!strcmp(host_shader_official_alias("35e3e560478d85178749be310ad13d6d6ecde618d32675261a3554592333a833"), "ui"));
    assert(sizeof(collector) < 2 * 1024 * 1024);
    shader_observations_init(&collector);
    descriptor.metal=0; assert(!shader_observations_append(&collector,&generation,&descriptor));
    descriptor.metal=1; descriptor.suppressed=1; assert(!shader_observations_append(&collector,&generation,&descriptor));
    descriptor.suppressed=0; descriptor.warming=1;
    for(i=0;i<SHADER_OBSERVATION_CAPACITY+1;i++) assert(!shader_observations_append(&collector,&generation,&descriptor));
    descriptor.warming=0;
    assert(shader_observations_append(&collector,&generation,&descriptor));
    generation.token=2; strcpy(generation.fingerprint,"def");
    assert(shader_observations_append(&collector,&generation,&descriptor));
    assert(shader_observations_take(&collector,&item));
    assert(item.generation.token==1 && !strcmp(item.generation.fingerprint,"abc"));
    assert(!item.descriptor.warming); /* Warmup cannot consume the space needed by first gameplay. */
    assert(shader_observations_lines(&item.descriptor,line,vertex,pixel));
    assert(!strcmp(line,"vs 000000000000002a 0 0") && !pixel[0]);
    assert(shader_observations_take(&collector,&item) && item.generation.token==2);
    assert(!shader_observations_take(&collector,&item));
    descriptor.warming=0;
    for(i=0;i<SHADER_OBSERVATION_CAPACITY;i++) assert(shader_observations_append(&collector,&generation,&descriptor));
    assert(!shader_observations_append(&collector,&generation,&descriptor));
    assert(atomic_load(&collector.dropped)==1);
    for(i=0;i<SHADER_OBSERVATION_CAPACITY;i++) assert(shader_observations_take(&collector,&item));
    descriptor.kind=SHADER_OBSERVATION_PIPELINE; descriptor.pixel_size=1;
    descriptor.pixel_hash=UINT64_C(12638153115695167455); /* FNV-1a of a zero byte. */
    assert(shader_observations_lines(&descriptor,line,vertex,pixel));
    assert(!strcmp(pixel,"ps af63bd4c8601b7df 00") && vertex[0] && !strncmp(line,"pipeline ",9));
    descriptor.pixel_hash++;
    assert(!shader_observations_lines(&descriptor,line,vertex,pixel));
    puts("shader observation collector passed");
    return 0;
}
