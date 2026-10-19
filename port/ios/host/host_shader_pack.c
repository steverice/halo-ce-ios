/* Strict admission for online shader descriptors, shared by the host and publisher. */
#include "host_shader_pack.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct record { int kind; char key[64], vertex[64], pixel[64], line[640]; };
static int hex_digit(char c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : -1; }
static int hex(const char *s, size_t count, unsigned char *bytes)
{
    if (strlen(s) != count * 2) return 0;
    for (size_t i = 0; i < count; i++) {
        int a = hex_digit(s[2*i]), b = hex_digit(s[2*i+1]);
        if (a < 0 || b < 0) return 0;
        bytes[i] = (unsigned char)(a*16+b);
    }
    return 1;
}
static int number(const char *s, unsigned maximum, unsigned *out, int radix)
{
    unsigned long n = 0;
    if (!*s || strlen(s) > 10) return 0;
    for (; *s; s++) {
        int v = radix == 16 ? hex_digit(*s) : *s >= '0' && *s <= '9' ? *s-'0' : -1;
        if (v < 0 || (unsigned)v > maximum || n > (maximum - (unsigned)v) / (unsigned)radix) return 0;
        n = n * (unsigned)radix + (unsigned)v;
    }
    *out = (unsigned)n; return 1;
}
static int hash_valid(const char *s) { unsigned char b[8]; return hex(s, 8, b); }
static int vertex_key(const char *s, char *key)
{
    char hash[17], variant[12], mask[12], tail;
    unsigned v, m;
    if (sscanf(s, "%16[^.].%11[^.].%11s%c", hash, variant, mask, &tail) != 3 ||
        !hash_valid(hash) || !number(variant, 1, &v, 10) || !number(mask, 65535, &m, 16) || (v && m)) return 0;
    snprintf(key, 64, "vs %s.%u.%x", hash, v, m); return 1;
}
static int parse(char *line, struct record *r)
{
    char *fields[14], *save = NULL, *token = strtok_r(line, " \t\r", &save);
    int n = 0;
    while (token && n < 14) { fields[n++] = token; token = strtok_r(NULL, " \t\r", &save); }
    if (token || !n) return 0;
    memset(r, 0, sizeof(*r));
    if (!strcmp(fields[0], "vs")) {
        unsigned variant, mask;
        if (n != 4 || !hash_valid(fields[1]) || !number(fields[2], 1, &variant, 10) ||
            !number(fields[3], 65535, &mask, 16) || (variant && mask)) return 0;
        r->kind = 0;
        snprintf(r->key, sizeof(r->key), "vs %s.%u.%x", fields[1], variant, mask);
        snprintf(r->line, sizeof(r->line), "vs %s %u %x", fields[1], variant, mask);
    } else if (!strcmp(fields[0], "ps")) {
        unsigned char bytes[252]; uint64_t h = UINT64_C(14695981039346656037);
        char hashed[17];
        if (n != 3 || !hash_valid(fields[1]) || !hex(fields[2], sizeof(bytes), bytes)) return 0;
        for (size_t i = 0; i < sizeof(bytes); i++) h = (h ^ bytes[i]) * UINT64_C(1099511628211);
        snprintf(hashed, sizeof(hashed), "%016llx", (unsigned long long)h);
        if (strcmp(hashed, fields[1])) return 0;
        /* Fixed guest ABI: 57 combiner words, texture modes, then bounded byte fields. */
        for (int i = 232; i < 236; i++) if (bytes[i] > 4) return 0;
        for (int i = 236; i < 240; i++) if (bytes[i] > 1) return 0;
        for (int i = 240; i < 244; i++) if (bytes[i] > 15) return 0;
        unsigned comparison = bytes[244] | ((unsigned)bytes[245] << 8) | ((unsigned)bytes[246] << 16) | ((unsigned)bytes[247] << 24);
        /* Xbox D3DCMP_NEVER..ALWAYS are 0x200..0x207 (xdk_pdb.h), not desktop D3D's 1..8. */
        if ((comparison && (comparison < 512 || comparison > 519)) || bytes[248] > 1 ||
            bytes[249] > 3 || bytes[250] > 1 || bytes[251] > 3) return 0;
        r->kind = 1;
        snprintf(r->key, sizeof(r->key), "ps %s", fields[1]);
        snprintf(r->line, sizeof(r->line), "ps %s %s", fields[1], fields[2]);
    } else if (!strcmp(fields[0], "pipeline")) {
        unsigned values[9]; unsigned char kinds[16];
        const unsigned maximum[] = {1,14,14,4,15,15,1,14,14};
        if ((n != 11 && n != 13) || !vertex_key(fields[1], r->vertex) || !hash_valid(fields[2]) ||
            !hex(fields[10], 16, kinds)) return 0;
        for (int i = 0; i < 16; i++) if (kinds[i] > 19) return 0;
        for (int i = 0; i < 7; i++) if (!number(fields[i+3], maximum[i], &values[i], 10)) return 0;
        values[7] = values[1]; values[8] = values[2];
        if (n == 13 && (!number(fields[11],14,&values[7],10) || !number(fields[12],14,&values[8],10))) return 0;
        r->kind = 2;
        snprintf(r->pixel, sizeof(r->pixel), "ps %s", fields[2]);
        snprintf(r->line, sizeof(r->line), "pipeline %s %s %u %u %u %u %u %u %u %s %u %u",
            r->vertex + 3, fields[2], values[0],values[1],values[2],values[3],values[4],values[5],values[6],fields[10],values[7],values[8]);
    } else return 0;
    return 1;
}
static int compare_key(const void *a, const void *b) { return strcmp(((const struct record *)a)->key, ((const struct record *)b)->key); }
static int compare_line(const void *a, const void *b)
{
    const struct record *x = a, *y = b;
    return x->kind != y->kind ? x->kind - y->kind : strcmp(x->line, y->line);
}
static int load(const char *text, size_t size, struct record *records, size_t *count)
{
    char *copy, *cursor; size_t first = *count;
    if (!text || size > HOST_SHADER_PACK_MAX_FILE_BYTES || memchr(text, 0, size)) return 0;
    copy = malloc(size + 1); if (!copy) return 0;
    memcpy(copy, text, size); copy[size] = 0; cursor = copy;
    while (*cursor) {
        char *line = cursor, *next = strchr(cursor, '\n');
        if (next) { *next = 0; cursor = next+1; } else cursor += strlen(cursor);
        if (strlen(line) > 1023) { free(copy); return 0; }
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
        if (!*line || *line == '#') continue;
        if (*count-first == HOST_SHADER_LIST_MAX_RECORDS || !parse(line, &records[*count])) { free(copy); return 0; }
        (*count)++;
    }
    free(copy);
    size_t n = *count-first;
    qsort(records+first,n,sizeof(*records),compare_key);
    for (size_t i = first; i < *count; i++) {
        struct record *r = records+i;
        if (r->kind < 2 && i > first && !strcmp(r->key,records[i-1].key) && strcmp(r->line,records[i-1].line)) return 0;
        if (r->kind == 2) {
            struct record key = {0};
            snprintf(key.key,sizeof(key.key),"%s",r->vertex);
            if (!bsearch(&key,records+first,n,sizeof(*records),compare_key)) return 0;
            snprintf(key.key,sizeof(key.key),"%s",r->pixel);
            if (!bsearch(&key,records+first,n,sizeof(*records),compare_key)) return 0;
        }
    }
    return n > 0;
}
int host_shader_list_merge(const char *base, size_t base_size, const char *additions, size_t additions_size,
    char **merged, size_t *merged_size)
{
    struct record *r = calloc(2 * HOST_SHADER_LIST_MAX_RECORDS,sizeof(*r));
    size_t n = 0, length = 0; char *output = NULL; int valid = 0;
    if (merged) *merged = NULL;
    if (merged_size) *merged_size = 0;
    if (!r || (base && !load(base,base_size,r,&n)) || (additions && !load(additions,additions_size,r,&n)) || !n) goto done;
    qsort(r,n,sizeof(*r),compare_key);
    for (size_t i = 1; i < n; i++) if (r[i].kind < 2 && !strcmp(r[i].key,r[i-1].key) && strcmp(r[i].line,r[i-1].line)) goto done;
    qsort(r,n,sizeof(*r),compare_line);
    for (size_t i = 0; i < n; i++) if (!i || strcmp(r[i].line,r[i-1].line)) length += strlen(r[i].line)+1;
    if (merged) {
        output = malloc(length+1); if (!output) goto done;
        size_t at = 0;
        for (size_t i = 0; i < n; i++) if (!i || strcmp(r[i].line,r[i-1].line)) {
            size_t bytes = strlen(r[i].line); memcpy(output+at,r[i].line,bytes); at += bytes; output[at++] = '\n';
        }
        output[at] = 0; *merged = output;
    }
    if (merged_size) *merged_size = length;
    valid = 1;
done:
    free(r); return valid;
}
int host_shader_map_name_valid(const char *name)
{
    size_t n = strlen(name);
    return n > 0 && n <= 63 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9')) &&
        strspn(name,"abcdefghijklmnopqrstuvwxyz0123456789_-") == n;
}
char *host_shader_list_read_file(const char *path, size_t *size)
{
    int fd = open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC); struct stat st; char *bytes = NULL;
    if (fd < 0) return NULL;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size > HOST_SHADER_PACK_MAX_FILE_BYTES) goto done;
    bytes = malloc((size_t)st.st_size+1); if (!bytes) goto done;
    *size = 0;
    while (*size < (size_t)st.st_size) {
        ssize_t n = read(fd,bytes+*size,(size_t)st.st_size-*size);
        if (n <= 0) { free(bytes); bytes = NULL; goto done; }
        *size += (size_t)n;
    }
    char extra;
    if (read(fd,&extra,1) != 0) { free(bytes); bytes = NULL; }
done:
    close(fd); return bytes;
}
int host_shader_pack_validate(const char *folder, const char *bundled, void (*log)(const char *))
{
    struct stat st; DIR *dir = NULL; struct dirent *e; int valid = 0; size_t total = 0, files = 0;
    if (lstat(folder,&st) || !S_ISDIR(st.st_mode) || !(dir = opendir(folder))) goto done;
    while ((e = readdir(dir))) {
        const char *name = e->d_name; size_t length = strlen(name), size = 0, base_size = 0;
        char map[64], path[1200];
        if (!strcmp(name,".") || !strcmp(name,"..")) continue;
        if (length < 5 || length >= sizeof(map)+4 || strcmp(name+length-4,".txt")) goto done;
        memcpy(map,name,length-4); map[length-4] = 0;
        if (!host_shader_map_name_valid(map) || ++files > HOST_SHADER_PACK_MAX_MAPS) goto done;
        snprintf(path,sizeof(path),"%s/%s",folder,name);
        char *bytes = host_shader_list_read_file(path,&size), *base = NULL;
        if (bundled) { snprintf(path,sizeof(path),"%s/%s",bundled,name); base = host_shader_list_read_file(path,&base_size); }
        int accepted = bytes && size <= HOST_SHADER_PACK_MAX_TOTAL_BYTES-total && host_shader_list_merge(base,base_size,bytes,size,NULL,NULL);
        free(bytes); free(base);
        if (!accepted) goto done;
        total += size;
    }
    valid = files > 0;
done:
    if (dir) closedir(dir);
    if (!valid && log) log("shader update: invalid map descriptors or dependencies");
    return valid;
}
