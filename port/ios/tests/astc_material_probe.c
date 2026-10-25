#include "../../linux/src/astc_encode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ASTC_MATERIAL_BASELINE
/* The previous worker has no material information. This mode demonstrates the
 * regression against its conservative policy before the collector is added. */
static struct astc_alpha_policy *astc_material_collect(FILE *map, uint64_t size,
    uint64_t instances, uint32_t count, int64_t translation)
{
    (void)map; (void)size; (void)instances; (void)translation;
    return calloc(count, sizeof(struct astc_alpha_policy));
}
#else
#include "../../linux/src/astc_material.h"
#endif

static unsigned char bytes[8192];
static void word(unsigned at, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) bytes[at + i] = value >> (i * 8);
}
static void reference(unsigned at, unsigned index)
{
    memcpy(bytes + at, "mtib", 4);
    word(at + 12, 0x10000 + index);
}
static void fixture(void)
{
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes + 128, "osos", 4);
    memcpy(bytes + 160, "mtib", 4);
    memcpy(bytes + 192, "vnes", 4);
    memcpy(bytes + 224, "mtib", 4);
    for (unsigned i = 0; i < 4; i++) word(128 + i * 32 + 12, 0x10000 + i);
    word(128 + 20, 0x40000000 + 1024);
    word(192 + 20, 0x40000000 + 2048);
    word(1024 + 0x24, 4); /* shader_base.type: model. */
    word(2048 + 0x24, 3); /* shader_base.type: environment. */
    reference(1024 + 0xA4, 1);
}
static struct astc_alpha_policy *collect(void)
{
    FILE *map = tmpfile();
    assert(map && fwrite(bytes, 1, sizeof(bytes), map) == sizeof(bytes));
    struct astc_alpha_policy *result = astc_material_collect(map, sizeof(bytes), 128, 4, -0x40000000LL);
    fclose(map);
    assert(result);
    return result;
}
static void expect_known(const struct astc_alpha_policy *policy)
{
    assert(policy->known);
    for (unsigned r = 0; r < 256; r++)
        assert(!!(policy->boundaries[r / 8] & (1u << (r % 8))) == (r == 128));
}
int main(void)
{
    const unsigned model_refs[] = {0xA4, 0xBC, 0xDC, 0x164};
    fixture();
    struct astc_alpha_policy *policy = collect();
    expect_known(policy + 1);
    assert(!policy[3].known);
    free(policy);

    fixture();
    reference(1024 + 0xA4, 3);
    policy = collect();
    expect_known(policy + 3);
    free(policy);

    for (unsigned i = 1; i < sizeof(model_refs) / sizeof(*model_refs); i++) {
        fixture();
        reference(1024 + model_refs[i], 3);
        policy = collect();
        expect_known(policy + 1);
        assert(!policy[3].known);
        free(policy);
    }

    /* A second, unknown consumer must dominate a recognized model use. */
    reference(6000, 1);
    policy = collect();
    assert(!policy[1].known);
    free(policy);

    fixture();
    for (unsigned i = 0; i < sizeof(model_refs) / sizeof(*model_refs); i++)
        reference(1024 + model_refs[i], 3);
    policy = collect();
    assert(!policy[3].known);
    free(policy);

    fixture();
    reference(6000, 1);
    word(6012, 0x20001); /* Matching low index with a stale datum salt. */
    policy = collect();
    expect_known(policy + 1);
    free(policy);

    fixture();
    memcpy(bytes + 128, "soso", 4);
    memcpy(bytes + 160, "bitm", 4);
    memcpy(bytes + 1024 + 0xA4, "bitm", 4);
    policy = collect();
    expect_known(policy + 1);
    free(policy);

    fixture();
    bytes[1024 + 0x28] = 8; /* Alpha-blended model decal. */
    policy = collect();
    assert(!policy[1].known);
    free(policy);

    fixture();
    word(1024 + 0x24, 3); /* A model FourCC with an environment dispatch type. */
    policy = collect();
    assert(!policy[1].known);
    free(policy);

    fixture();
    /* An unknown tag start inside a candidate's minimum layout disqualifies
     * that material even when it does not share its exact start. */
    memcpy(bytes + 224, "xxxx", 4);
    word(224 + 20, 0x40000000 + 1024 + 0x80);
    policy = collect();
    assert(!policy[1].known);
    free(policy);

    fixture();
    /* A preceding environment extent can cover the model start without an
     * intervening tag start inside the model's own extent. */
    memset(bytes + 1024 + 0xA4, 0, 16);
    word(128 + 20, 0x40000000 + 2048 + 0x300);
    reference(2048 + 0x300 + 0xA4, 1);
    policy = collect();
    assert(!policy[1].known);
    free(policy);

    fixture();
    /* A blended model can alias an environment reference from an earlier
     * start. The environment's allowlist must not hide the blended consumer. */
    memset(bytes + 1024 + 0xA4, 0, 16);
    word(128 + 20, 0x40000000 + 2048 - 0x1C);
    bytes[2048 + 0x0C] = 8;
    reference(2048 + 0x88, 1);
    policy = collect();
    assert(!policy[1].known);
    free(policy);

    fixture();
    /* Unknown tags sharing a recognized material start also disqualify it. */
    memcpy(bytes + 224, "xxxx", 4);
    word(224 + 20, 0x40000000 + 1024);
    policy = collect();
    assert(!policy[1].known);
    free(policy);
#ifndef ASTC_MATERIAL_BASELINE
    FILE *map = tmpfile();
    assert(map);
    assert(!astc_material_collect(map, 8, 16, 4, 0));
    assert(!astc_material_collect(map, sizeof(bytes), 128, 65537, 0));
    assert(!astc_material_collect(map, sizeof(bytes), 128, 4, 0)); /* Truncated physical file. */
    fclose(map);
#endif

    fixture();
    /* Every environment role remains conservative because these alpha
     * channels can participate in continuous blend masks. */
    const unsigned environment_refs[] = {0x88, 0xB8, 0xCC, 0xFC, 0x128, 0x254, 0x324};
    for (unsigned i = 0; i < sizeof(environment_refs) / sizeof(*environment_refs); i++)
        reference(2048 + environment_refs[i], 3);
    policy = collect();
    expect_known(policy + 1);
    assert(!policy[3].known);
    free(policy);

    for (unsigned i = 0; i < sizeof(environment_refs) / sizeof(*environment_refs); i++) {
        fixture();
        reference(2048 + environment_refs[i], 3);
        policy = collect();
        expect_known(policy + 1);
        assert(!policy[3].known);
        free(policy);
    }

    /* An unknown consumer may straddle an I/O chunk boundary. */
    reference(4092, 3);
    policy = collect();
    assert(!policy[3].known);
    free(policy);

    fixture();
    word(128 + 20, 0xFFFFFFFF); /* Malformed recognized material. */
    policy = collect();
    assert(!policy[1].known);
    free(policy);
    puts("ASTC material policy probe passed");
    return 0;
}
