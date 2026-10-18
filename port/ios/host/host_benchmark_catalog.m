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

static BOOL route_matches(NSString *folder, struct benchmark_scene *scene, NSError **error) {
    NSString *recording = [NSString stringWithUTF8String:scene->recording];
    NSString *route_name = [recording stringByAppendingPathExtension:@"route"];
    NSData *data = read_catalog_file(folder, route_name.fileSystemRepresentation, 4095);
    NSString *text = data ? [[NSString alloc] initWithData:data encoding:NSASCIIStringEncoding] : nil;
    struct benchmark_route route;
    if (!text || !benchmark_scene_from_route(text.UTF8String, scene->recording, &route) ||
        strcmp(route.id, scene->id) || strcmp(route.map, scene->map) || strcmp(route.difficulty, scene->difficulty) ||
        route.scene.kind != scene->kind || fabs(route.scene.lead - scene->lead) > 1e-6 ||
        fabs(route.scene.seconds - scene->seconds) > 1e-6)
        return catalog_error(error, [NSString stringWithFormat:@"%@ does not match its signed catalog entry", recording]);
    return YES;
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
    if (document.count != 2 || !integer_number(document[@"schema"], 1, 1, NULL) ||
        ![cases isKindOfClass:NSDictionary.class] || !cases.count || cases.count > BENCHMARK_SCENES_MAXIMUM)
        return catalog_error(error, @"The signed benchmark catalog has an unsupported schema or case count.");

    struct host_benchmark_catalog built = {0};
    BOOL orders[BENCHMARK_SCENES_MAXIMUM] = {0};
    char seen_maps[BENCHMARK_SCENES_MAXIMUM][32] = {{0}};
    int seen_map_count = 0;
    BOOL workload = NO;
    built.count = (int)cases.count;
    for (NSString *identifier in cases) {
        id raw = cases[identifier];
        NSDictionary *entry = [raw isKindOfClass:NSDictionary.class] ? raw : nil;
        NSString *kind = entry[@"kind"], *map = nil, *difficulty = nil, *recording = nil;
        NSInteger order = -1, players = 0, views = 0, score = 0, time_limit = 0;
        double lead, window;
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
        if (![[NSSet setWithArray:entry.allKeys] isEqualToSet:expected])
            return catalog_error(error, [NSString stringWithFormat:@"Catalog case %@ has missing or unsupported fields.", identifier]);
        BOOL menu = [kind isEqual:@"menu"], cutscene = [kind isEqual:@"cutscene"];
        BOOL gameplay = [kind isEqual:@"gameplay"], multiplayer = [kind isEqual:@"multiplayer"];
        if ((!menu && !cutscene && !gameplay && !multiplayer) || (menu && (map.length || has_recording)) ||
            (!menu && !map.length) || ![@[@"easy", @"normal", @"hard", @"impossible"] containsObject:difficulty])
            return catalog_error(error, [NSString stringWithFormat:@"Catalog case %@ has an invalid kind, map, or difficulty.", identifier]);
        if (gameplay && !has_recording)
            return catalog_error(error, [NSString stringWithFormat:@"Gameplay case %@ needs a recording.", identifier]);
        if (gameplay) workload = YES;
        if (multiplayer && has_recording)
            return catalog_error(error, [NSString stringWithFormat:@"Multiplayer case %@ cannot replay a recording.", identifier]);
        if (has_recording) {
            if (!string_field(entry, @"recording", 48, NO, &recording))
                return catalog_error(error, [NSString stringWithFormat:@"Catalog case %@ has an invalid recording reference.", identifier]);
            for (int previous = 0; previous < seen_map_count; previous++)
                if (!strcmp(seen_maps[previous], map.UTF8String))
                    return catalog_error(error, [NSString stringWithFormat:@"Catalog selects more than one recording for %@.", map]);
            snprintf(seen_maps[seen_map_count++], sizeof(seen_maps[0]), "%s", map.UTF8String);
        }
        NSString *simulation = nil, *game_mode = nil;
        if (multiplayer) {
            id raw_simulation = entry[@"simulation"], raw_mode = entry[@"game_mode"];
            simulation = [raw_simulation isKindOfClass:NSString.class] ? raw_simulation : nil;
            game_mode = [raw_mode isKindOfClass:NSString.class] ? raw_mode : nil;
            if (!integer_number(entry[@"players"], 2, 32, &players) ||
                !simulation || !game_mode || ![game_mode isEqual:@"slayer"] ||
                !integer_number(entry[@"views"], 1, 1, &views) ||
                !integer_number(entry[@"score_limit"], 1, 1000, &score) ||
                !integer_number(entry[@"time_limit_minutes"], 0, 120, &time_limit) ||
                ([simulation isEqual:@"local_controllers"] && players != 4) ||
                (![simulation isEqual:@"local_controllers"] && ![simulation isEqual:@"network_players"]))
                return catalog_error(error, [NSString stringWithFormat:@"Multiplayer case %@ requests an unsupported workload.", identifier]);
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
        scene->simulation = multiplayer ? built.simulations[index] : NULL;
        scene->game_mode = multiplayer ? built.game_modes[index] : NULL;
        scene->players = (int)players; scene->views = (int)views;
        scene->score_limit = (int)score; scene->time_limit_minutes = (int)time_limit;
        if (scene->recording && !route_matches(folder, scene, error)) return NO;
        if (gameplay && !gameplay_active_in_window(folder, recording, lead, window))
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
        scene->difficulty = catalog->difficulties[index];
        scene->recording = catalog->recordings[index][0] ? catalog->recordings[index] : NULL;
        scene->simulation = catalog->simulations[index][0] ? catalog->simulations[index] : NULL;
        scene->game_mode = catalog->game_modes[index][0] ? catalog->game_modes[index] : NULL;
    }
    return YES;
}

BOOL host_benchmark_catalog_validate_folder(NSString *folder, NSError **error) {
    struct host_benchmark_catalog catalog;
    return host_benchmark_catalog_load(folder, &catalog, error);
}

NSString *host_benchmark_catalog_file_sha256(NSString *folder) {
    NSData *data = read_catalog_file(folder, HOST_BENCHMARK_CATALOG_NAME, catalog_maximum_bytes);
    return data ? digest(data) : nil;
}
