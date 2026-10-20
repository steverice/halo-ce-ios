/* A downloaded replay is data, never a console script or saved game. Keep this validator shared with
   tools/data_pack.py so publishing and staging use the same bounded grammar. */
#include "host_recording_pack.h"
#include "../../runtime/benchmark_plan.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TICKS_MAXIMUM 108000 /* one hour at 30 ticks per second */
#define SEGMENTS_MAXIMUM 32
static const char *extensions[] = { "input", "actions", "seeds", "route", "checks" };
struct route_files { char name[49], scene[48]; unsigned mask; };

static int name_valid(const char *name, size_t maximum)
{
    size_t n = strlen(name);
    return n && n <= maximum && name[0] != '-' && name[n - 1] != '-' &&
        strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789-") == n;
}

/* openat and O_NOFOLLOW also protect against links changed after the directory scan. */
static char *read_file(int folder, const char *name, size_t maximum)
{
    int fd = openat(folder, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    char *text = NULL;
    size_t at = 0;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > maximum) goto done;
    text = malloc((size_t)st.st_size + 1);
    if (!text) goto done;
    while (at < (size_t)st.st_size) {
        ssize_t count = read(fd, text + at, (size_t)st.st_size - at);
        if (count <= 0) { free(text); text = NULL; goto done; }
        at += (size_t)count;
    }
    {
        char extra;
        if (read(fd, &extra, 1) != 0) { free(text); text = NULL; goto done; }
    }
    text[at] = 0;
    /* Parsers consume C strings; embedded NULs must never hide a suffix. All replay formats are ASCII. */
    for (at = 0; at < (size_t)st.st_size; at++) {
        unsigned char c = (unsigned char)text[at];
        if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c > 126) {
            free(text); text = NULL; break;
        }
    }
done:
    close(fd);
    return text;
}

static char *line_next(char **cursor)
{
    char *line = *cursor, *end;
    size_t length;
    if (!line || !*line) return NULL;
    end = strchr(line, '\n');
    if (end) { *end = 0; *cursor = end + 1; } else *cursor = NULL;
    length = strlen(line);
    if (length && line[length - 1] == '\r') line[--length] = 0;
    return line;
}
static int tokens(char *line, char **parts, int count)
{
    char *save = NULL, *at = strtok_r(line, " \t", &save);
    int n = 0;
    while (at) {
        if (n >= count) return 0;
        parts[n++] = at;
        at = strtok_r(NULL, " \t", &save);
    }
    return n == count;
}
static int integer(const char *text, int base, long long low, long long high, long long *value)
{
    char *end;
    long long n;
    const char *digits = base == 16 ? "0123456789abcdefABCDEF" : "0123456789";
    const char *at = text;
    if (*at == '-' && low < 0) at++;
    if (!*at || strspn(at, digits) != strlen(at) || strlen(at) > (base == 16 ? 8u : 10u)) return 0;
    errno = 0; n = strtoll(text, &end, base);
    if (errno || *end || n < low || n > high) return 0;
    *value = n;
    return 1;
}
static int real_number(const char *text, double low, double high, double *value)
{
    char *end;
    double n;
    if (!*text || strspn(text, "+-0123456789.eE") != strlen(text)) return 0;
    errno = 0; n = strtod(text, &end);
    if (errno || *end || !isfinite(n) || n < low || n > high) return 0;
    *value = n; return 1;
}
static int header(char **cursor, const char *expected, int metadata)
{
    char *line = line_next(cursor);
    if (!line) return 0;
    return metadata ? replay_environment_parse(line, NULL) : !strcmp(line, expected);
}
static int input_valid(char *text)
{
    char *cursor = text, *line;
    double previous = -1;
    int count = 0;
    if (!header(&cursor, "halo input recording 1", 0)) return 0;
    while ((line = line_next(&cursor))) {
        char *p[14]; double tick; long long n; int i;
        if (strlen(line) > 254 || !tokens(line, p, 14) || !real_number(p[0], 0, TICKS_MAXIMUM, &tick) || tick < previous ||
            !integer(p[1], 16, 0, 65535, &n)) return 0;
        for (i = 2; i < 10; i++) if (!integer(p[i], 10, 0, 255, &n)) return 0;
        for (i = 10; i < 14; i++) if (!integer(p[i], 10, -32768, 32767, &n)) return 0;
        previous = tick; count++;
    }
    return count > 0;
}
static int action_seeds_valid(char *actions, char *seeds)
{
    char *a = actions, *s = seeds, *line, *seed_line;
    long long last_segment = -1, last_tick = -1;
    int count = 0;
    if (!header(&a, "halo action recording 1", 1) || !header(&s, "halo seed recording 1", 0)) return 0;
    while ((line = line_next(&a))) {
        char *p[10], *q[4]; long long segment, tick, seed_segment, seed_tick, word; int i;
        seed_line = line_next(&s);
        if (!seed_line || strlen(line) > 254 || strlen(seed_line) > 126 || !tokens(line, p, 10) || !tokens(seed_line, q, 4) ||
            !integer(p[0], 10, 0, SEGMENTS_MAXIMUM - 1, &segment) || !integer(p[1], 10, 0, TICKS_MAXIMUM, &tick) ||
            !integer(q[0], 10, 0, SEGMENTS_MAXIMUM - 1, &seed_segment) || !integer(q[1], 10, 0, TICKS_MAXIMUM, &seed_tick) ||
            segment != seed_segment || tick != seed_tick || segment < last_segment ||
            (segment == last_segment && tick <= last_tick)) return 0;
        for (i = 2; i < 10; i++) {
            if (!integer(p[i], 16, 0, UINT32_MAX, &word)) return 0;
            /* player_action's words 1..5 are floats: facing, throttle and primary trigger. */
            if (i >= 3 && i <= 7) {
                uint32_t bits = (uint32_t)word; float value;
                memcpy(&value, &bits, sizeof(value));
                if (!isfinite(value) || fabsf(value) > 1000000.0f) return 0;
            }
        }
        if (!integer(q[2], 16, 0, UINT32_MAX, &word) || !integer(q[3], 16, 0, UINT32_MAX, &word)) return 0;
        last_segment = segment; last_tick = tick; count++;
    }
    return count > 0 && !line_next(&s);
}

static int profile_metadata_key(const char *key)
{
#define PROFILE_BOOLEAN(name) if (!strcmp(key, name)) return 1;
#define PROFILE_INTEGER(name, minimum, maximum) PROFILE_BOOLEAN(name)
#define PROFILE_REAL(name, minimum, maximum) PROFILE_BOOLEAN(name)
#define PROFILE_STRING(name, choices) PROFILE_BOOLEAN(name)
#define PROFILE_SCENE(name)
#define PROFILE_EXTERNAL(name)
#include "../../linux/src/port_profile_keys.h"
#undef PROFILE_BOOLEAN
#undef PROFILE_INTEGER
#undef PROFILE_REAL
#undef PROFILE_STRING
#undef PROFILE_SCENE
#undef PROFILE_EXTERNAL
    return 0;
}
static char *trim(char *value)
{
    char *end;
    while (*value == ' ' || *value == '\t') value++;
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    if (end > value + 1 && *value == '"' && end[-1] == '"') { value++; end[-1] = 0; }
    return value;
}
static int route_valid(char *text, const char *name, char scene[48])
{
    struct benchmark_route route;
    char *copy, *cursor, *line;
    char seen[64][64]; int seen_count = 0, ok = 1;
    copy = strdup(text);
    if (!copy) return 0;
    cursor = copy;
    while (ok && (line = line_next(&cursor))) {
        char *equal = strchr(line, '='), *key, *value;
        double n; long long v; int i;
        if (!*line) continue;
        if (strlen(line) > 254 || !equal) { ok = 0; break; }
        *equal = 0; key = trim(line); value = trim(equal + 1);
        if (!*key || strlen(key) >= 64 || !*value) { ok = 0; break; }
        if (strcmp(key, "segment_seed")) {
            for (i = 0; i < seen_count; i++) if (!strcmp(seen[i], key)) ok = 0;
            if (seen_count == 64) { ok = 0; break; }
            snprintf(seen[seen_count++], 64, "%s", key);
        }
        if (!strcmp(key, "first_tick")) ok &= integer(value, 10, 0, 1, &v);
        else if (!strcmp(key, "players")) ok &= integer(value, 10, 1, 1, &v);
        else if (!strcmp(key, "segments")) ok &= integer(value, 10, 1, SEGMENTS_MAXIMUM, &v);
        else if (!strcmp(key, "pauses") || !strcmp(key, "last_tick")) ok &= integer(value, 10, 0, TICKS_MAXIMUM, &v);
        else if (!strcmp(key, "lead") || !strcmp(key, "window") || !strcmp(key, "seconds")) ok &= real_number(value, 0, 3600, &n);
        else if (!strcmp(key, "enemy_multiplier")) ok &= real_number(value, 1, 8, &n);
        else if (!strcmp(key, "random_seed")) ok &= integer(value, 16, 0, UINT32_MAX, &v);
        else if (!strcmp(key, "segment_seed")) {
            char *p[3];
            ok &= tokens(value, p, 3) && integer(p[0], 10, 0, SEGMENTS_MAXIMUM - 1, &v) &&
                integer(p[1], 10, 0, TICKS_MAXIMUM, &v) && integer(p[2], 16, 0, UINT32_MAX, &v);
        }
        else if (!strcmp(key, "map")) ok &= name_valid(value, 31);
        else if (!strcmp(key, "scene")) ok &= name_valid(value, 47);
        else if (!strcmp(key, "difficulty")) ok &= !strcmp(value, "easy") || !strcmp(value, "normal") || !strcmp(value, "hard") || !strcmp(value, "impossible");
        else if (!strcmp(key, "entered")) ok &= !strcmp(value, "start") || !strcmp(value, "console");
        else if (!strcmp(key, "kind")) ok &= !strcmp(value, "gameplay") || !strcmp(value, "cutscene");
        else if (!strcmp(key, "heavy")) ok &= !strcmp(value, "true") || !strcmp(value, "false");
        else if (!strcmp(key, "stereo")) ok &= !strcmp(value, "off") || !strcmp(value, "head") || !strcmp(value, "side_by_side") || !strcmp(value, "screen");
        else if (strcmp(key, "version") && strcmp(key, "recorded") && strcmp(key, "closed_by") && !profile_metadata_key(key)) ok = 0;
    }
    free(copy);
    if (!ok || !benchmark_scene_from_route(text, name, &route) || !name_valid(route.map, 31) ||
        !name_valid(route.id, 47) || !isfinite(route.scene.lead) || !isfinite(route.scene.seconds) ||
        route.scene.lead < 0 || route.scene.lead > 3600 || route.scene.seconds < 0 || route.scene.seconds > 3600) return 0;
    snprintf(scene, 48, "%s", route.id);
    return 1;
}
static int checks_valid(char *text)
{
    char *at = text;
    double last = -1;
    int count = 0;
    while (*at) {
        double values[4]; int i;
        for (i = 0; i < 4; i++) {
            char *end = strpbrk(at, i == 0 ? ":" : i < 3 ? "," : ";\r\n");
            char saved = end ? *end : 0;
            if (end) *end = 0;
            if (!real_number(at, i == 0 ? 0 : -1000000, i == 0 ? TICKS_MAXIMUM : 1000000, &values[i])) return 0;
            if (end) *end = saved;
            if (i < 3 && !end) return 0;
            at = end ? end + 1 : at + strlen(at);
            if (i == 3) {
                if (values[0] <= last) return 0;
                last = values[0]; count++;
                if (saved == '\r' && *at == '\n') at++;
            }
        }
    }
    return count > 0;
}

int host_recording_pack_validate(const char *folder, void (*log)(const char *))
{
    int fd = -1, count = 0, file_count = 0, catalog_count = 0, ok = 0, i;
    uint64_t total = 0;
    DIR *directory = NULL;
    struct dirent *entry;
    struct route_files routes[HOST_RECORDING_PACK_MAX_ROUTES] = {0};
    struct stat st;
    if (!folder || (fd = open(folder, O_RDONLY | O_DIRECTORY | O_NOFOLLOW)) < 0) goto done;
    directory = fdopendir(dup(fd));
    if (!directory) goto done;
    errno = 0;
    while ((entry = readdir(directory))) {
        char base[49], *dot; size_t length; int extension = -1, index;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++file_count > HOST_RECORDING_PACK_MAX_FILES || fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > HOST_RECORDING_PACK_MAX_FILE_BYTES) goto done;
        total += (uint64_t)st.st_size;
        if (total > HOST_RECORDING_PACK_MAX_TOTAL_BYTES) goto done;
        if (!strcmp(entry->d_name, "benchmark-catalog.json")) {
            if (++catalog_count > 1) goto done;
            errno = 0;
            continue;
        }
        dot = strrchr(entry->d_name, '.');
        if (!dot || !(length = (size_t)(dot - entry->d_name)) || length > 48) goto done;
        memcpy(base, entry->d_name, length); base[length] = 0;
        if (!name_valid(base, 48)) goto done;
        for (i = 0; i < 5; i++) if (!strcmp(dot + 1, extensions[i])) extension = i;
        if (extension < 0) goto done;
        for (index = 0; index < count && strcmp(routes[index].name, base); index++);
        if (index == count) {
            if (count == HOST_RECORDING_PACK_MAX_ROUTES) goto done;
            snprintf(routes[count++].name, 49, "%s", base);
        }
        routes[index].mask |= 1u << extension;
        errno = 0;
    }
    if (errno || (!count && !catalog_count) || count > BENCHMARK_SCENES_MAXIMUM) goto done;
    for (i = 0; i < count; i++) {
        char path[64], *contents[5] = {0}; int extension, valid = 1;
        if ((routes[i].mask & 15) != 15) goto done;
        for (extension = 0; extension < 5; extension++) if (routes[i].mask & (1u << extension)) {
            snprintf(path, sizeof(path), "%s.%s", routes[i].name, extensions[extension]);
            contents[extension] = read_file(fd, path, extension >= 3 ? 4095 : HOST_RECORDING_PACK_MAX_FILE_BYTES);
            if (!contents[extension]) valid = 0;
        }
        if (valid) valid = input_valid(contents[0]) && action_seeds_valid(contents[1], contents[2]) &&
            route_valid(contents[3], routes[i].name, routes[i].scene) && (!contents[4] || checks_valid(contents[4]));
        for (extension = 0; extension < 5; extension++) free(contents[extension]);
        if (!valid) goto done;
        for (extension = 0; extension < i; extension++)
            if (!strcmp(routes[i].scene, routes[extension].scene)) goto done;
    }
    ok = 1;
done:
    if (directory) closedir(directory);
    if (fd >= 0) close(fd);
    if (!ok && log) log("recording pack rejected: incomplete, unsafe, incompatible or malformed replay snapshot");
    return ok;
}
