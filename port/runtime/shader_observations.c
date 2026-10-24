/* Bounded single-producer collection; canonicalization runs on the worker. */
#include "shader_observations.h"
#include <string.h>
#include <stdio.h>

void shader_observations_init(struct shader_observation_collector *c)
{
    memset(c, 0, sizeof(*c));
    atomic_init(&c->read, 0); atomic_init(&c->write, 0); atomic_init(&c->dropped, 0);
}
int shader_observations_append(struct shader_observation_collector *c,
    const struct shader_observation_generation *g, const struct shader_observation *d)
{
    uint32_t write, read;
    if (!g || !g->token || !g->fingerprint[0] || !d->metal || d->suppressed || d->warming) return 0;
    write = atomic_load_explicit(&c->write, memory_order_relaxed);
    read = atomic_load_explicit(&c->read, memory_order_acquire);
    if (write - read >= SHADER_OBSERVATION_CAPACITY) {
        atomic_fetch_add_explicit(&c->dropped, 1, memory_order_relaxed); return 0;
    }
    c->items[write % SHADER_OBSERVATION_CAPACITY].generation = *g;
    c->items[write % SHADER_OBSERVATION_CAPACITY].descriptor = *d;
    atomic_store_explicit(&c->write, write + 1, memory_order_release);
    return 1;
}
int shader_observations_take(struct shader_observation_collector *c, struct shader_observation_item *item)
{
    uint32_t read = atomic_load_explicit(&c->read, memory_order_relaxed);
    if (read == atomic_load_explicit(&c->write, memory_order_acquire)) return 0;
    *item = c->items[read % SHADER_OBSERVATION_CAPACITY];
    atomic_store_explicit(&c->read, read + 1, memory_order_release);
    return 1;
}
static uint64_t hash64(const uint8_t *bytes, uint32_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    while (size--) { hash ^= *bytes++; hash *= UINT64_C(1099511628211); }
    return hash;
}
int shader_observations_lines(const struct shader_observation *d, char *line, char *vertex, char *pixel)
{
    uint32_t i;
    char kinds[33];
    line[0] = vertex[0] = pixel[0] = 0;
    if (d->kind < SHADER_OBSERVATION_VERTEX || d->kind > SHADER_OBSERVATION_PIPELINE || d->variant > 1) return 0;
    if (d->kind != SHADER_OBSERVATION_VERTEX) {
        size_t length;
        if (!d->pixel_size || d->pixel_size > SHADER_OBSERVATION_PIXEL_MAX ||
            hash64(d->pixel, d->pixel_size) != d->pixel_hash) return 0;
        length = (size_t)snprintf(pixel, SHADER_OBSERVATION_LINE_MAX, "ps %016llx ", (unsigned long long)d->pixel_hash);
        for (i = 0; i < d->pixel_size; ++i) {
            static const char hex[] = "0123456789abcdef";
            pixel[length++] = hex[d->pixel[i] >> 4]; pixel[length++] = hex[d->pixel[i] & 15];
        }
        pixel[length] = 0;
    }
    if (d->kind != SHADER_OBSERVATION_PIXEL)
        snprintf(vertex, SHADER_OBSERVATION_LINE_MAX, "vs %016llx %u %x", (unsigned long long)d->vertex_hash, d->variant, d->packed_mask);
    if (d->kind == SHADER_OBSERVATION_VERTEX) strcpy(line, vertex);
    else if (d->kind == SHADER_OBSERVATION_PIXEL) strcpy(line, pixel);
    else {
        if (d->blend > 1 || d->exact_borders > 15 || d->depth > 1 || d->write_mask > 15 ||
            d->source > 14 || d->destination > 14 || d->operation > 4 ||
            d->alpha_source > 14 || d->alpha_destination > 14) return 0;
        for (i = 0; i < 16; ++i) {
            if (d->attribute_kinds[i] > 19) return 0;
            snprintf(kinds + 2 * i, 3, "%02x", d->attribute_kinds[i]);
        }
        snprintf(line, SHADER_OBSERVATION_LINE_MAX,
            "pipeline %016llx.%u.%x %016llx %u %u %u %u %u %u %u %s %u %u",
            (unsigned long long)d->vertex_hash, d->variant, d->packed_mask, (unsigned long long)d->pixel_hash,
            d->blend, d->source, d->destination, d->operation, d->write_mask, d->exact_borders, d->depth,
            kinds, d->alpha_source, d->alpha_destination);
    }
    return 1;
}
