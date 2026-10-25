/* The scale rule of port/assets/texture-policy.json ("scales", texture_policy_scale) against the shipping set:
every row of port/assets/texture-selection-lock.tsv (tools/texture_selection_lock.py, read from the set's files) must
get the scale the set has. texture-selection-probe LOCK_TSV */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "texture_policy.h"

static int named(const char *name, const char *const *names, int count)
{
	for (int i = 0; i < count; i++)
		if (!strcmp(name, names[i]))
			return i;
	fprintf(stderr, "texture-selection-probe: unknown name %s\n", name);
	exit(2);
}

int main(int argc, char **argv)
{
	static const char *const kinds[] = { "graphic", "surface", "companion", "bump", "original" };
	static const char *const treatments[] = { "skip", "color", "first-person", "emissive", "multipurpose", "bump" };
	char line[8192];
	unsigned long rows = 0, wrong = 0, by_scale[9] = { 0 };
	FILE *file = argc == 2 ? fopen(argv[1], "r") : NULL;

	if (!file)
		return fprintf(stderr, "usage: texture-selection-probe LOCK_TSV\n"), 2;
	while (fgets(line, sizeof(line), file))
	{
		char *fields[7], *cursor = line, *tags[64];
		int field = 0, tag_count = 0;
		uint32_t width, height, scale, got;
		uint64_t hash;

		line[strcspn(line, "\n")] = 0;
		if (line[0] == '#' || !strncmp(line, "hash\t", 5))
			continue;
		while (field < 7)
		{
			fields[field++] = cursor;
			cursor = strchr(cursor, '\t');
			if (!cursor)
				break;
			*cursor++ = 0;
		}
		assert(field == 7);
		hash = strtoull(fields[0], NULL, 16);
		width = (uint32_t)strtoul(fields[1], NULL, 10);
		height = (uint32_t)strtoul(fields[2], NULL, 10);
		scale = (uint32_t)strtoul(fields[5], NULL, 10);
		for (char *tag = strtok(fields[6], "|"); tag && tag_count < 64; tag = strtok(NULL, "|"))
			tags[tag_count++] = tag;
		got = texture_policy_scale(&texture_policy_embedded, (enum texture_policy_kind)named(fields[3], kinds, 5),
			TEXTURE_POLICY_S4G, (enum texture_policy_treatment)named(fields[4], treatments, 6), hash, width, height,
			(const char *const *)tags, (size_t)tag_count);
		if (got != scale)
		{
			fprintf(stderr, "%016" PRIx64 " %ux%u %s %s: the policy says %ux, the set has %ux\n", hash, width, height,
				fields[3], fields[4], got, scale);
			wrong++;
		}
		if (scale < 9)
			by_scale[scale]++;
		rows++;
	}
	fclose(file);
	/* an original is never upscaled */
	assert(texture_policy_scale(&texture_policy_embedded, TEXTURE_POLICY_GRAPHIC, TEXTURE_POLICY_ORIGINAL,
		TEXTURE_POLICY_COLOR, 0, 0, 0, NULL, 0) == 1);
	printf("%lu textures: 2x %lu, 4x %lu, 8x %lu; %lu differ from the set\n", rows, by_scale[2], by_scale[4],
		by_scale[8], wrong);
	return rows && !wrong ? 0 : 1;
}
