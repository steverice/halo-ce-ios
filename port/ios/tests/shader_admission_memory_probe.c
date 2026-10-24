/* Small observation batches must not reserve the full publication catalog. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static size_t largest;
static void *bounded_calloc(size_t count, size_t size)
{
    if (size && count > 65536 / size) return NULL;
    if (count * size > largest) largest = count * size;
    return calloc(count, size);
}
#define calloc bounded_calloc
#include "../host/host_shader_pack.c"
#undef calloc
int main(void)
{
    const char *one = "vs 0000000000000001 0 0\n";
    const char *two = "# supporting evidence\n\nvs 0000000000000002 1 0\n";
    char *merged = NULL;
    size_t size = 0;
    assert(host_shader_list_merge(one, strlen(one), two, strlen(two), &merged, &size));
    assert(strstr(merged, one) && strstr(merged, "vs 0000000000000002 1 0\n"));
    assert(largest == 2 * sizeof(struct record));
    free(merged);
    assert(!host_shader_list_merge("", 0, NULL, 0, NULL, NULL));
    puts("shader admission memory probe passed");
    return 0;
}
