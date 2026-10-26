/* Canonical public hardware keys and optional future capacity evidence. */
#include "host_profile.h"
#include "tomlc17.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int host_profile_device_key(const struct host_device *device, char *key, size_t size)
{
    const char *platform = device->platform, *model = device->model, *chip = device->chip;
    char encoded[64] = "_"; size_t n = strlen(model);
    if (strcmp(platform,"ios") && strcmp(platform,"tvos") && strcmp(platform,"visionos") && strcmp(platform,"mac")) return 0;
    if (!n || n >= sizeof(device->model) || !((model[0]>='A' && model[0]<='Z') || (model[0]>='a' && model[0]<='z')) ||
        strspn(model,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789,.-") != n || strstr(model,"..") ||
        model[n-1] == '.' || model[n-1] == '-') return 0;
    if (!strcmp(platform,"mac")) {
        if (strncmp(chip,"Apple M",7)) return 0;
        const char *p = chip+7, *start = p;
        if (*p < '1' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') p++;
        if (p-start > 6 || (*p && strcmp(p," Pro") && strcmp(p," Max") && strcmp(p," Ultra"))) return 0;
        if (strlen(chip) >= sizeof(encoded)) return 0;
        snprintf(encoded,sizeof(encoded),"%s",chip);
        for (char *c = encoded; *c; c++) if (*c == ' ') *c = '-';
    } else if (*chip) return 0;
    int written = snprintf(key,size,"%s/%s/%s",platform,model,encoded);
    return written > 0 && (size_t)written < size;
}
int host_profile_capacity_validate(const char *path)
{
    int descriptor = open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC), valid = 0;
    struct stat st; char *text = NULL; toml_result_t result = {0};
    if (descriptor < 0) return 0;
    if (fstat(descriptor,&st) || !S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size > 4096) goto done;
    text = malloc((size_t)st.st_size+1); if (!text) goto done;
    if (read(descriptor,text,(size_t)st.st_size) != st.st_size || memchr(text,0,(size_t)st.st_size)) goto done;
    text[st.st_size] = 0; result = toml_parse(text,(int)st.st_size);
    if (!result.ok) goto done;
    for (int i = 0; i < result.toptab.u.tab.size; i++) {
        const char *name = result.toptab.u.tab.key[i];
        if (strcmp(name,"schema") && strcmp(name,"validated_maximum_players") && strcmp(name,"evidence")) goto done;
    }
    toml_datum_t schema = toml_get(result.toptab,"schema"), maximum = toml_get(result.toptab,"validated_maximum_players"), evidence = toml_get(result.toptab,"evidence");
    if (schema.type != TOML_INT64 || schema.u.int64 != 1 ||
        (maximum.type != TOML_UNKNOWN && (maximum.type != TOML_INT64 || maximum.u.int64 < 2 || maximum.u.int64 > 128)) ||
        (evidence.type != TOML_UNKNOWN && (evidence.type != TOML_STRING || !*evidence.u.s || strlen(evidence.u.s) > 2048)) ||
        (maximum.type != TOML_UNKNOWN && evidence.type != TOML_STRING)) goto done;
    valid = 1;
done:
    toml_free(result); free(text); close(descriptor); return valid;
}
