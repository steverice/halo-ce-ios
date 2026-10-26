/* Canonical public hardware keys, optional future capacity evidence and the profile set's revision. */
#include "host_profile.h"
#include "tomlc17.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

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

/* hardware/revision.toml (tools/profile_revision.py): exactly schema = 1, a revision from 1 to 2^53 - 1 and the
   64-digit lowercase sha256 of the profile sources it was stamped from. The hash is the publisher's check; the app
   only orders shards by the revision. */
long long host_profile_revision_parse(const char *text, size_t size, char sha256[65])
{
    long long revision = 0;
    if (sha256) *sha256 = 0;
    if (!text || !size || size > HOST_PROFILE_REVISION_MAXIMUM_BYTES || memchr(text,0,size)) return 0;
    char *copy = malloc(size+1); if (!copy) return 0;
    memcpy(copy,text,size); copy[size] = 0;
    toml_result_t result = toml_parse(copy,(int)size);
    free(copy);
    if (!result.ok) { toml_free(result); return 0; }
    int fields = result.toptab.u.tab.size;
    toml_datum_t schema = toml_get(result.toptab,"schema"), value = toml_get(result.toptab,"revision"), sha = toml_get(result.toptab,"sha256");
    if (fields == 3 && schema.type == TOML_INT64 && schema.u.int64 == 1 && value.type == TOML_INT64 &&
        value.u.int64 >= 1 && value.u.int64 <= 9007199254740991LL && sha.type == TOML_STRING &&
        strlen(sha.u.s) == 64 && strspn(sha.u.s,"0123456789abcdef") == 64) {
        revision = value.u.int64;
        if (sha256) memcpy(sha256,sha.u.s,65);
    }
    toml_free(result);
    return revision;
}
long long host_profile_revision_read(const char *folder, char sha256[65])
{
    char path[1024], text[HOST_PROFILE_REVISION_MAXIMUM_BYTES]; struct stat st; long long revision = 0;
    if (sha256) *sha256 = 0;
    if (!folder || snprintf(path,sizeof(path),"%s/hardware/revision.toml",folder) >= (int)sizeof(path)) return -1;
    int descriptor = open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (descriptor < 0) return errno == ENOENT ? 0 : -1;
    if (!fstat(descriptor,&st) && S_ISREG(st.st_mode) && st.st_size >= 1 && st.st_size <= (off_t)sizeof(text) &&
        read(descriptor,text,(size_t)st.st_size) == st.st_size) revision = host_profile_revision_parse(text,(size_t)st.st_size,sha256);
    close(descriptor);
    return revision ? revision : -1;
}
