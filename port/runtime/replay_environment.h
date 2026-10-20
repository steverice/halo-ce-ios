#ifndef REPLAY_ENVIRONMENT_H
#define REPLAY_ENVIRONMENT_H

#include <stddef.h>
#include <string.h>

#define REPLAY_ACTION_HEADER "halo action recording 1"

/* Missing mode metadata identifies takes made with the original animation behavior.
   This describes simulation while replay owns it, never a persistent setting. */
struct replay_environment {
    int enhanced_animations, enhanced_recorded, original_lip_sync;
    const char *stereo;
    int action_version, controller_aim;
};

/* One strict grammar for the guest reader, native validator and report reader.
   Accept a whole header with an optional line ending, without modifying its bytes. */
static inline int replay_environment_parse(const char *line, struct replay_environment *environment)
{
    struct replay_environment parsed = {0, 0, 0, "off", 1, 0};
    const char *header = "halo action recording ";
    size_t prefix = strlen(header);
    int lip_seen = 0, stereo_seen = 0, aim_seen = 0;
    if (!line || strncmp(line, header, prefix)) return 0;
    line += prefix;
    if (*line != '1' && *line != '2') return 0;
    parsed.action_version = *line++ - '0';
    while (*line && *line != '\r' && *line != '\n') {
        const char *end;
        size_t length;
        if (*line++ != ' ' || !*line) return 0;
        end = line;
        while (*end && *end != ' ' && *end != '\r' && *end != '\n') end++;
        length = (size_t)(end - line);
#define REPLAY_TOKEN(text) (length == sizeof(text) - 1 && !memcmp(line, text, sizeof(text) - 1))
        if (REPLAY_TOKEN("enhanced_animations=0") || REPLAY_TOKEN("enhanced_animations=1")) {
            if (parsed.enhanced_recorded) return 0;
            parsed.enhanced_recorded = 1;
            parsed.enhanced_animations = line[length - 1] == '1';
        } else if (REPLAY_TOKEN("lip_sync=original") || REPLAY_TOKEN("lip_sync=simulated")) {
            if (lip_seen++) return 0;
            parsed.original_lip_sync = REPLAY_TOKEN("lip_sync=original");
        } else if (REPLAY_TOKEN("stereo=off") || REPLAY_TOKEN("stereo=head") ||
                   REPLAY_TOKEN("stereo=side_by_side") || REPLAY_TOKEN("stereo=screen")) {
            if (stereo_seen++) return 0;
            parsed.stereo = REPLAY_TOKEN("stereo=head") ? "head" :
                REPLAY_TOKEN("stereo=side_by_side") ? "side_by_side" :
                REPLAY_TOKEN("stereo=screen") ? "screen" : "off";
        } else if (REPLAY_TOKEN("aim=controller")) {
            if (aim_seen++ || parsed.action_version != 2) return 0;
            parsed.controller_aim = 1;
        } else return 0;
#undef REPLAY_TOKEN
        line = end;
    }
    if (*line == '\r') line++;
    if (*line == '\n') line++;
    if (*line) return 0;
    if (parsed.action_version == 2 && (!parsed.controller_aim ||
        (strcmp(parsed.stereo, "head") && strcmp(parsed.stereo, "side_by_side")))) return 0;
    if (environment) *environment = parsed;
    return 1;
}

#endif
