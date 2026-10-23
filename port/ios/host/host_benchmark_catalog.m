#import "host_benchmark_catalog.h"
#import <CommonCrypto/CommonDigest.h>
#import "host_recording_pack.h"
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *const catalog_domain = @"VISRBenchmarkCatalog";
static const NSUInteger catalog_maximum_bytes = 256 * 1024;

static BOOL catalog_error(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:catalog_domain code:1
        userInfo:@{ NSLocalizedDescriptionKey: message }];
    return NO;
}

static NSString *digest(NSData *data) {
    uint8_t bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, bytes);
    NSMutableString *hex = [NSMutableString stringWithCapacity:64];
    for (NSUInteger index = 0; index < sizeof(bytes); index++) [hex appendFormat:@"%02x", bytes[index]];
    return hex;
}

static NSData *read_catalog_file(NSString *folder, const char *name, NSUInteger maximum) {
    int directory = open(folder.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) return nil;
    int descriptor = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    close(directory);
    if (descriptor < 0) return nil;
    struct stat info;
    if (fstat(descriptor, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 || (uint64_t)info.st_size > maximum) {
        close(descriptor);
        return nil;
    }
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)info.st_size];
    NSUInteger offset = 0;
    while (offset < data.length) {
        ssize_t count = read(descriptor, (uint8_t *)data.mutableBytes + offset, data.length - offset);
        if (count <= 0) { close(descriptor); return nil; }
        offset += (NSUInteger)count;
    }
    uint8_t extra;
    BOOL complete = read(descriptor, &extra, 1) == 0;
    close(descriptor);
    return complete ? data : nil;
}

static BOOL integer_number(id value, NSInteger minimum, NSInteger maximum, NSInteger *result) {
    if (![value isKindOfClass:NSNumber.class] ||
        CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() ||
        CFNumberIsFloatType((__bridge CFNumberRef)value)) return NO;
    NSInteger number = [value integerValue];
    if (number < minimum || number > maximum) return NO;
    if (result) *result = number;
    return YES;
}

static BOOL real_number(id value, double minimum, double maximum, double *result) {
    if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) return NO;
    double number = [value doubleValue];
    if (!isfinite(number) || number < minimum || number > maximum) return NO;
    if (result) *result = number;
    return YES;
}

static BOOL safe_name(NSString *value, NSUInteger maximum, BOOL empty) {
    if (![value isKindOfClass:NSString.class] || value.length > maximum || (!empty && !value.length)) return NO;
    const char *text = value.UTF8String;
    if (!text || strlen(text) != [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding]) return NO;
    if (!*text) return empty;
    if (*text == '-' || text[strlen(text) - 1] == '-') return NO;
    for (const char *at = text; *at; at++)
        if (!( (*at >= 'a' && *at <= 'z') || (*at >= '0' && *at <= '9') || *at == '-' )) return NO;
    return YES;
}

static BOOL string_field(NSDictionary *value, NSString *key, NSUInteger maximum, BOOL empty, NSString **result) {
    id one = value[key];
    if (!safe_name(one, maximum, empty)) return NO;
    if (result) *result = one;
    return YES;
}

static BOOL gameplay_active_in_window(NSString *folder, NSString *recording, double lead, double window) {
    NSString *name = [recording stringByAppendingPathExtension:@"actions"];
    NSData *data = read_catalog_file(folder, name.fileSystemRepresentation, HOST_RECORDING_PACK_MAX_FILE_BYTES);
    NSString *text = data ? [[NSString alloc] initWithData:data encoding:NSASCIIStringEncoding] : nil;
    if (!text) return NO;
    const char *contents = text.UTF8String;
    const char *line = strchr(contents, '\n');
    if (!line || strncmp(contents, "halo action recording 1", strlen("halo action recording 1"))) return NO;
    line++;
    double first = lead * 30.0, last = (lead + window) * 30.0;
    NSUInteger ordinal = 0;
    char *copy = strdup(line);
    if (!copy) return NO;
    char *save = NULL, *row = strtok_r(copy, "\n", &save);
    BOOL active = NO, valid = YES;
    while (row) {
        if ((double)ordinal >= first && (double)ordinal <= last) {
            long segment, tick;
            unsigned int word[8];
            if (sscanf(row, "%ld %ld %x %x %x %x %x %x %x %x", &segment, &tick,
                &word[0], &word[1], &word[2], &word[3], &word[4], &word[5], &word[6], &word[7]) != 10) {
                valid = NO;
                break;
            }
            (void)segment;
            (void)tick;
            float throttle_x, throttle_y, trigger;
            memcpy(&throttle_x, &word[3], sizeof(throttle_x));
            memcpy(&throttle_y, &word[4], sizeof(throttle_y));
            memcpy(&trigger, &word[5], sizeof(trigger));
            if (isfinite(throttle_x) && isfinite(throttle_y) && isfinite(trigger) &&
                (hypotf(throttle_x, throttle_y) > 0.1f || trigger > 0.05f)) active = YES;
        }
        ordinal++;
        row = strtok_r(NULL, "\n", &save);
    }
    free(copy);
    return valid && active;
}

/* A case anchored on an event of its recording: the route carries the same anchor and the event's recorded time,
   and the case's lead (seconds after the event) becomes the window's start in the recording's time. */
static BOOL route_matches(NSString *folder, struct benchmark_scene *scene, NSError **error) {
    NSString *recording = [NSString stringWithUTF8String:scene->recording];
    NSString *route_name = [recording stringByAppendingPathExtension:@"route"];
    NSData *data = read_catalog_file(folder, route_name.fileSystemRepresentation, 4095);
    NSString *text = data ? [[NSString alloc] initWithData:data encoding:NSASCIIStringEncoding] : nil;
    struct benchmark_route route;
    if (!text || !benchmark_scene_from_route(text.UTF8String, scene->recording, &route) ||
        strcmp(route.id, scene->workload ? scene->workload : scene->id) || strcmp(route.map, scene->map) || strcmp(route.difficulty, scene->difficulty) ||
        route.scene.kind != scene->kind || fabs(route.scene.lead - scene->lead) > 1e-6 ||
        fabs(route.scene.seconds - scene->seconds) > 1e-6 ||
        strcmp(route.anchor, scene->anchor ? scene->anchor : "") ||
        fabs((route.scene.enemy_multiplier > 1.0 ? route.scene.enemy_multiplier : 1.0) -
             (scene->enemy_multiplier > 1.0 ? scene->enemy_multiplier : 1.0)) > 1e-6)
        return catalog_error(error, [NSString stringWithFormat:@"%@ does not match its signed catalog entry", recording]);
    if (scene->anchor) scene->lead += route.scene.anchor_seconds;
    return YES;
}

/* A case's "settings" (tools/benchmark_catalog.py CASE_SETTINGS has the same names and ranges): the experimental
   simulation values and explicit overrides as bits, the rest as the scene's knobs. NO for anything unsupported. */
static BOOL case_settings(id raw, unsigned *sim_experiments, unsigned *sim_experiments_mask, struct benchmark_knobs *knobs) {
    NSArray *simulation = @[@"debug.collision_type_filter", @"debug.sphere_query_type_filter", @"debug.static_light_relink",
        @"debug.collision_candidates"];
    NSDictionary *settings = raw;
    if (![settings isKindOfClass:NSDictionary.class] || !settings.count) return NO;
    for (NSString *name in settings) {
        id value = settings[name];
        NSInteger number;
        double real;
        BOOL boolean = [value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID();
        NSUInteger bit = [simulation indexOfObject:name];
        if (bit != NSNotFound) {
            if (!boolean) return NO;
            *sim_experiments_mask |= 1u << bit;
            if ([value boolValue]) *sim_experiments |= 1u << bit;
        } else if ([name isEqual:@"display.render_height"]) {
            if (!integer_number(value, 0, 4320, &number)) return NO;
            knobs->mask |= BENCHMARK_KNOB_LINES; knobs->lines = number;
        } else if ([name isEqual:@"display.anisotropic_filtering"]) {
            if (!integer_number(value, 1, 16, &number) || ![@[@1, @2, @4, @8, @16] containsObject:@(number)]) return NO;
            knobs->mask |= BENCHMARK_KNOB_ANISOTROPY; knobs->anisotropy = number;
        } else if ([name isEqual:@"display.shadow_map_size"]) {
            if (!integer_number(value, 128, 2048, &number) || ![@[@128, @256, @512, @1024, @2048] containsObject:@(number)]) return NO;
            knobs->mask |= BENCHMARK_KNOB_SHADOW; knobs->shadow_size = number;
        } else if ([name isEqual:@"display.model_lod"]) {
            if (![@[@"max", @"auto"] containsObject:value]) return NO;
            knobs->mask |= BENCHMARK_KNOB_LOD; knobs->model_lod_max = [value isEqual:@"max"];
        } else if ([name isEqual:@"display.anti_aliasing"]) {
            NSUInteger method = [@[@"off", @"fxaa", @"smaa"] indexOfObject:value];
            if (method == NSNotFound) return NO;
            knobs->mask |= BENCHMARK_KNOB_ANTI_ALIASING; knobs->anti_aliasing = (int)method;
        } else if ([name isEqual:@"display.effect_resolution"]) {
            NSUInteger level = [@[@"xbox", @"half", @"full"] indexOfObject:value];
            if (level == NSNotFound) return NO;
            knobs->mask |= BENCHMARK_KNOB_EFFECT; knobs->effect_level = (int)level;
        } else if ([name isEqual:@"display.render_scale"]) {
            if (!real_number(value, 0.25, 1.0, &real)) return NO;
            knobs->mask |= BENCHMARK_KNOB_RENDER_SCALE; knobs->render_scale = real;
        } else if ([name isEqual:@"display.clutter_distance"]) {
            if (!real_number(value, 0.0, 200.0, &real)) return NO;
            knobs->mask |= BENCHMARK_KNOB_CLUTTER_DISTANCE; knobs->clutter_distance = real;
        } else if ([name isEqual:@"display.frame_rate"]) {
            /* (as a profile's: a whole number from 30; host_frame_rate_override applies it for the case's runs) */
            if (!integer_number(value, 30, 1000, &number)) return NO;
            knobs->mask |= BENCHMARK_KNOB_FRAME_RATE; knobs->frame_rate = number;
        } else if ([name isEqual:@"debug.clutter_lowest_lod"]) {
            if (!boolean) return NO;
            knobs->mask |= BENCHMARK_KNOB_CLUTTER_LOD; knobs->clutter_lowest_lod = [value boolValue];
        } else if ([name isEqual:@"debug.offscreen_window_wall_seconds"]) {
            if (!real_number(value, 30.0, 1800.0, &real)) return NO;
            knobs->mask |= BENCHMARK_KNOB_WALL_SECONDS; knobs->wall_seconds = real;
        } else return NO;
    }
    return YES;
}

/* The catalog's "variants": each {"id", "models", "settings"}, a unique id, one to eight distinct model identifiers
   (AppleTV, iPad or iPhone) and at least one of display.render_height, display.output_height (0 to 4320) and
   display.upscaler (bilinear or metalfx). NO for anything else, which refuses the whole catalog. */
static BOOL catalog_variants(id raw, struct host_benchmark_catalog *built) {
    NSArray *variants = raw;
    if (![variants isKindOfClass:NSArray.class] || !variants.count || variants.count > HOST_BENCHMARK_VARIANTS_MAXIMUM) return NO;
    NSMutableSet *seen = [NSMutableSet set];
    for (id item in variants) {
        NSDictionary *entry = item;
        struct host_benchmark_variant *variant = &built->variants[built->variant_count];
        if (![entry isKindOfClass:NSDictionary.class] ||
            ![[NSSet setWithArray:entry.allKeys] isEqualToSet:[NSSet setWithArray:@[@"id", @"models", @"settings"]]]) return NO;
        NSString *identifier = entry[@"id"];
        if (!safe_name(identifier, 47, NO) || [seen containsObject:identifier]) return NO;
        [seen addObject:identifier];
        snprintf(variant->id, sizeof(variant->id), "%s", identifier.UTF8String);
        NSArray *models = entry[@"models"];
        if (![models isKindOfClass:NSArray.class] || !models.count || models.count > HOST_BENCHMARK_VARIANT_MODELS_MAXIMUM ||
            [NSSet setWithArray:models].count != models.count) return NO;
        variant->model_count = 0;
        for (NSString *model in models) {
            if (![model isKindOfClass:NSString.class] || [model rangeOfString:@"^(AppleTV|iPad|iPhone)[1-9][0-9]?,[1-9][0-9]?$"
                options:NSRegularExpressionSearch].location == NSNotFound) return NO;
            snprintf(variant->models[variant->model_count++], sizeof(variant->models[0]), "%s", model.UTF8String);
        }
        NSDictionary *settings = entry[@"settings"];
        if (![settings isKindOfClass:NSDictionary.class] || !settings.count) return NO;
        variant->render_height = variant->output_height = variant->upscaler = -1;
        for (NSString *name in settings) {
            id value = settings[name];
            NSInteger number;
            if ([name isEqual:@"display.render_height"]) {
                if (!integer_number(value, 0, 4320, &number)) return NO;
                variant->render_height = (int)number;
            } else if ([name isEqual:@"display.output_height"]) {
                if (!integer_number(value, 0, 4320, &number)) return NO;
                variant->output_height = (int)number;
            } else if ([name isEqual:@"display.upscaler"]) {
                NSUInteger method = [@[@"bilinear", @"metalfx"] indexOfObject:value];
                if (![value isKindOfClass:NSString.class] || method == NSNotFound) return NO;
                variant->upscaler = (int)method;
            } else return NO;
        }
        built->variant_count++;
    }
    return YES;
}

int host_benchmark_catalog_variants_for(const struct host_benchmark_catalog *catalog, const char *model,
    int indexes[HOST_BENCHMARK_VARIANTS_MAXIMUM]) {
    int count = 0;
    for (int index = 0; model && index < catalog->variant_count; index++)
        for (int name = 0; name < catalog->variants[index].model_count; name++)
            if (!strcmp(catalog->variants[index].models[name], model)) { indexes[count++] = index; break; }
    return count;
}

/* The cases with each variant ({"variant_of", "order", "settings"}) replaced by the case it varies, under its own
   order and settings; *bases names the case each came from. nil when a variant is not valid. */
static NSDictionary *resolve_variants(NSDictionary *cases, NSMutableDictionary<NSString *, NSString *> *bases) {
    NSMutableDictionary *resolved = [NSMutableDictionary dictionary];
    for (NSString *identifier in cases) {
        NSDictionary *entry = cases[identifier];
        if (![entry isKindOfClass:NSDictionary.class] || !entry[@"variant_of"]) {
            resolved[identifier] = entry;
            continue;
        }
        NSString *name = entry[@"variant_of"];
        NSDictionary *base = [name isKindOfClass:NSString.class] ? cases[name] : nil;
        if (![[NSSet setWithArray:entry.allKeys] isEqualToSet:[NSSet setWithArray:@[@"variant_of", @"order", @"settings"]]] ||
            ![base isKindOfClass:NSDictionary.class] || base[@"variant_of"] || [base[@"kind"] isEqual:@"menu"]) return nil;
        NSMutableDictionary *merged = [base mutableCopy];
        merged[@"order"] = entry[@"order"];
        merged[@"settings"] = entry[@"settings"];
        resolved[identifier] = merged;
        bases[identifier] = name;
    }
    return resolved;
}

BOOL host_benchmark_catalog_load(NSString *folder, struct host_benchmark_catalog *catalog, NSError **error) {
    NSData *data = read_catalog_file(folder, HOST_BENCHMARK_CATALOG_NAME, catalog_maximum_bytes);
    id parsed = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    NSData *canonical = [parsed isKindOfClass:NSDictionary.class] ?
        [NSJSONSerialization dataWithJSONObject:parsed options:NSJSONWritingSortedKeys error:NULL] : nil;
    if (!data || !canonical || ![canonical isEqualToData:data])
        return catalog_error(error, @"The signed benchmark catalog is missing, malformed, or noncanonical.");
    NSDictionary *document = parsed;
    NSDictionary *cases = document[@"cases"];
    if (document.count != (document[@"variants"] ? 3u : 2u) || !integer_number(document[@"schema"], 1, 1, NULL) ||
        ![cases isKindOfClass:NSDictionary.class] || !cases.count || cases.count > BENCHMARK_SCENES_MAXIMUM)
        return catalog_error(error, @"The signed benchmark catalog has an unsupported schema or case count.");

    struct host_benchmark_catalog built = {0};
    if (document[@"variants"] && !catalog_variants(document[@"variants"], &built))
        return catalog_error(error, @"The signed benchmark catalog has an unsupported display variant.");
    BOOL orders[BENCHMARK_SCENES_MAXIMUM] = {0};
    char seen_recordings[BENCHMARK_SCENES_MAXIMUM][49] = {{0}};
    int seen_recording_count = 0;
    BOOL workload = NO;
    built.count = (int)cases.count;
    NSMutableDictionary<NSString *, NSString *> *bases = [NSMutableDictionary dictionary];
    NSDictionary *resolved = resolve_variants(cases, bases);
    if (!resolved) return catalog_error(error, @"The signed benchmark catalog has an invalid variant case.");
    for (NSString *identifier in resolved) {
        id raw = resolved[identifier];
        NSString *workload_id = bases[identifier] ?: identifier;
        NSDictionary *entry = [raw isKindOfClass:NSDictionary.class] ? raw : nil;
        NSString *kind = entry[@"kind"], *map = nil, *difficulty = nil, *recording = nil;
        NSInteger order = -1, players = 0, views = 0, score = 0, time_limit = 0;
        double lead, window, arena_spacing = 0, arena_distance = 0, arena_radius = 0, enemy_multiplier = 0;
        NSInteger arena_columns = 0;
        BOOL has_recording = entry[@"recording"] && entry[@"recording"] != NSNull.null;
        if (!safe_name(identifier, 47, NO) || !entry || ![kind isKindOfClass:NSString.class] ||
            !integer_number(entry[@"order"], 0, BENCHMARK_SCENES_MAXIMUM - 1, &order) || orders[order] ||
            !string_field(entry, @"map", 31, [kind isEqual:@"menu"], &map) ||
            !string_field(entry, @"difficulty", 15, NO, &difficulty) ||
            !real_number(entry[@"lead"], 0, 3600, &lead) || !real_number(entry[@"window"], 0.000001, 3600, &window))
            return catalog_error(error, [NSString stringWithFormat:@"Catalog case %@ has invalid fields.", identifier]);
        orders[order] = YES;
        NSSet *common = [NSSet setWithArray:@[@"kind", @"order", @"map", @"difficulty", @"recording", @"lead", @"window"]];
        NSMutableSet *expected = [common mutableCopy];
        if ([kind isEqual:@"multiplayer"])
            [expected unionSet:[NSSet setWithArray:@[@"simulation", @"players", @"views", @"game_mode", @"score_limit", @"time_limit_minutes"]]];
        if ([kind isEqual:@"multiplayer"] && entry[@"arena"]) [expected addObject:@"arena"];
        if (entry[@"enemy_multiplier"]) [expected addObject:@"enemy_multiplier"];
        if (entry[@"anchor"]) [expected addObject:@"anchor"];
        if (entry[@"settings"]) [expected addObject:@"settings"];
        if (![[NSSet setWithArray:entry.allKeys] isEqualToSet:expected])
            return catalog_error(error, [NSString stringWithFormat:@"Catalog case %@ has missing or unsupported fields.", identifier]);
        BOOL menu = [kind isEqual:@"menu"], cutscene = [kind isEqual:@"cutscene"];
        BOOL gameplay = [kind isEqual:@"gameplay"], multiplayer = [kind isEqual:@"multiplayer"];
        if ((!menu && !cutscene && !gameplay && !multiplayer) || (menu && (map.length || has_recording)) ||
            (!menu && !map.length) || ![@[@"easy", @"normal", @"hard", @"impossible"] containsObject:difficulty])
            return catalog_error(error, [NSString stringWithFormat:@"Catalog case %@ has an invalid kind, map, or difficulty.", identifier]);
        unsigned sim_experiments = 0, sim_experiments_mask = 0;
        struct benchmark_knobs knobs = {0};
        if (entry[@"settings"] && !case_settings(entry[@"settings"], &sim_experiments, &sim_experiments_mask, &knobs))
            return catalog_error(error, [NSString stringWithFormat:@"Catalog case %@ has unsupported settings.", identifier]);
        if (entry[@"enemy_multiplier"] && (multiplayer || menu || !real_number(entry[@"enemy_multiplier"], 1, 8, &enemy_multiplier)))
            return catalog_error(error, [NSString stringWithFormat:@"Catalog case %@ has an unsupported enemy multiplier.", identifier]);
        NSString *anchor = nil;
        if (entry[@"anchor"] && (!has_recording || multiplayer || ![entry[@"anchor"] isKindOfClass:NSString.class] ||
            [(NSString *)entry[@"anchor"] rangeOfString:@"^(cinematic_start|cinematic_end|cinematic_done|checkpoint|bsp_switch):([1-9][0-9]{0,2}|[a-z][a-z0-9_]{0,30})$"
                options:NSRegularExpressionSearch].location == NSNotFound))
            return catalog_error(error, [NSString stringWithFormat:@"Catalog case %@ has an unsupported anchor.", identifier]);
        if (entry[@"anchor"]) anchor = entry[@"anchor"];
        if (gameplay && !has_recording)
            return catalog_error(error, [NSString stringWithFormat:@"Gameplay case %@ needs a recording.", identifier]);
        if (gameplay) workload = YES;
        if (multiplayer && has_recording)
            return catalog_error(error, [NSString stringWithFormat:@"Multiplayer case %@ cannot replay a recording.", identifier]);
        if (has_recording) {
            if (!string_field(entry, @"recording", 48, NO, &recording))
                return catalog_error(error, [NSString stringWithFormat:@"Catalog case %@ has an invalid recording reference.", identifier]);
            if (!bases[identifier]) {
                for (int previous = 0; previous < seen_recording_count; previous++)
                    if (!strcmp(seen_recordings[previous], recording.UTF8String))
                        return catalog_error(error, [NSString stringWithFormat:@"Catalog selects recording %@ for more than one case.", recording]);
                snprintf(seen_recordings[seen_recording_count++], sizeof(seen_recordings[0]), "%s", recording.UTF8String);
            }
        }
        NSString *simulation = nil, *game_mode = nil;
        if (multiplayer) {
            id raw_simulation = entry[@"simulation"], raw_mode = entry[@"game_mode"];
            simulation = [raw_simulation isKindOfClass:NSString.class] ? raw_simulation : nil;
            game_mode = [raw_mode isKindOfClass:NSString.class] ? raw_mode : nil;
            if (!integer_number(entry[@"players"], 2, 128, &players) ||
                !simulation || !game_mode || ![game_mode isEqual:@"slayer"] ||
                !integer_number(entry[@"views"], 1, 1, &views) ||
                !integer_number(entry[@"score_limit"], 1, 1000, &score) ||
                !integer_number(entry[@"time_limit_minutes"], 0, 120, &time_limit) ||
                ([simulation isEqual:@"local_controllers"] && players != 4) ||
                (![simulation isEqual:@"local_controllers"] && ![simulation isEqual:@"network_players"]))
                return catalog_error(error, [NSString stringWithFormat:@"Multiplayer case %@ requests an unsupported workload.", identifier]);
            id arena = entry[@"arena"];
            if (players > 4 && !arena)
                return catalog_error(error, @"Scaled multiplayer requires a signed arena.");
            if (arena) {
                NSInteger spacing, distance, radius;
                NSSet *keys = [NSSet setWithArray:@[@"columns", @"spacing_milliunits", @"distance_milliunits", @"movement_radius_milliunits"]];
                if (![simulation isEqual:@"network_players"] || ![arena isKindOfClass:NSDictionary.class] ||
                    ![[NSSet setWithArray:[arena allKeys]] isEqualToSet:keys] ||
                    !integer_number(arena[@"columns"], 1, 8, &arena_columns) ||
                    !integer_number(arena[@"spacing_milliunits"], 800, 3000, &spacing) ||
                    !integer_number(arena[@"distance_milliunits"], 2000, 12000, &distance) ||
                    !integer_number(arena[@"movement_radius_milliunits"], 150, 1000, &radius) || radius * 3 > spacing)
                    return catalog_error(error, @"Multiplayer arena parameters are unsupported.");
                arena_spacing = spacing / 1000.0; arena_distance = distance / 1000.0; arena_radius = radius / 1000.0;
            }
            workload = YES;
        }
        int index = (int)order;
        snprintf(built.ids[index], sizeof(built.ids[index]), "%s", identifier.UTF8String);
        snprintf(built.maps[index], sizeof(built.maps[index]), "%s", map.UTF8String);
        snprintf(built.difficulties[index], sizeof(built.difficulties[index]), "%s", difficulty.UTF8String);
        if (recording) snprintf(built.recordings[index], sizeof(built.recordings[index]), "%s", recording.UTF8String);
        snprintf(built.simulations[index], sizeof(built.simulations[index]), "%s", simulation ? simulation.UTF8String : "");
        snprintf(built.game_modes[index], sizeof(built.game_modes[index]), "%s", game_mode ? game_mode.UTF8String : "");
        struct benchmark_scene *scene = &built.scenes[index];
        scene->id = built.ids[index]; scene->map = built.maps[index]; scene->difficulty = built.difficulties[index];
        scene->recording = recording ? built.recordings[index] : NULL; scene->lead = lead; scene->seconds = window;
        scene->kind = menu ? BENCHMARK_SCENE_MENU : cutscene ? BENCHMARK_SCENE_CUTSCENE :
            multiplayer ? BENCHMARK_SCENE_MULTIPLAYER : BENCHMARK_SCENE_GAMEPLAY;
        scene->heavy = gameplay;
        scene->enemy_multiplier = enemy_multiplier;
        if (anchor) {
            snprintf(built.anchors[index], sizeof(built.anchors[index]), "%s", anchor.UTF8String);
            scene->anchor = built.anchors[index];
        }
        scene->sim_experiments = sim_experiments;
        scene->sim_experiments_mask = sim_experiments_mask;
        scene->knobs = knobs;
        snprintf(built.workloads[index], sizeof(built.workloads[index]), "%s", workload_id.UTF8String);
        scene->workload = built.workloads[index];
        scene->simulation = multiplayer ? built.simulations[index] : NULL;
        scene->game_mode = multiplayer ? built.game_modes[index] : NULL;
        scene->players = (int)players; scene->views = (int)views;
        scene->score_limit = (int)score; scene->time_limit_minutes = (int)time_limit;
        scene->arena_columns = (int)arena_columns; scene->arena_spacing = arena_spacing;
        scene->arena_distance = arena_distance; scene->arena_movement_radius = arena_radius;
        if (scene->recording && !route_matches(folder, scene, error)) return NO;
        if (gameplay && !gameplay_active_in_window(folder, recording, scene->lead, window))
            return catalog_error(error, [NSString stringWithFormat:@"Gameplay case %@ has no movement or combat input in its measured window.", identifier]);
    }
    for (int order = 0; order < built.count; order++)
        if (!orders[order]) return catalog_error(error, @"Catalog case order must be contiguous from zero.");
    if (!workload) return catalog_error(error, @"Catalog requires a gameplay recording or a supported multiplayer workload.");
    NSString *sha = digest(data);
    if (!sha) return catalog_error(error, @"Catalog digest could not be computed.");
    snprintf(built.sha256, sizeof(built.sha256), "%s", sha.UTF8String);
    *catalog = built;
    for (int index = 0; index < catalog->count; index++) {
        struct benchmark_scene *scene = &catalog->scenes[index];
        scene->id = catalog->ids[index]; scene->map = catalog->maps[index];
        scene->workload = catalog->workloads[index];
        scene->difficulty = catalog->difficulties[index];
        scene->recording = catalog->recordings[index][0] ? catalog->recordings[index] : NULL;
        scene->anchor = catalog->anchors[index][0] ? catalog->anchors[index] : NULL;
        scene->simulation = catalog->simulations[index][0] ? catalog->simulations[index] : NULL;
        scene->game_mode = catalog->game_modes[index][0] ? catalog->game_modes[index] : NULL;
    }
    return YES;
}

BOOL host_benchmark_catalog_validate_folder(NSString *folder, NSError **error) {
    struct host_benchmark_catalog catalog;
    return host_benchmark_catalog_load(folder, &catalog, error);
}

NSUInteger host_benchmark_menu_choice_count(NSString *selected_folder) {
    return host_benchmark_catalog_validate_folder(selected_folder, NULL) ? 2 : 1;
}

NSString *host_benchmark_catalog_file_sha256(NSString *folder) {
    NSData *data = read_catalog_file(folder, HOST_BENCHMARK_CATALOG_NAME, catalog_maximum_bytes);
    return data ? digest(data) : nil;
}
