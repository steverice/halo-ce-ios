/* Apache-2.0. Render-only ranking, independent of light storage and random state. */
#ifndef LIGHT_RENDER_SELECTION_H
#define LIGHT_RENDER_SELECTION_H
#include <float.h>

struct light_render_candidate
{
	unsigned long id;
	double score;
	int weapon, specular;
};

static double light_render_score(float radius, float x, float y, float z,
	float camera_x, float camera_y, float camera_z)
{
	double dx = (double)x - camera_x, dy = (double)y - camera_y, dz = (double)z - camera_z;
	double distance_squared = dx * dx + dy * dy + dz * dz;
	if (!(radius > 0 && radius <= FLT_MAX && distance_squared >= 0 && distance_squared <= DBL_MAX))
		return 0;
	/* Squaring radius/distance preserves the order without a square root. */
	return (double)radius * radius / (distance_squared > 1e-12 ? distance_squared : 1e-12);
}

static void light_render_select(const struct light_render_candidate *lights, int count,
	int dynamic_limit, int specular_limit, unsigned char *diffuse, unsigned char *specular)
{
	int order[128], i, j, dynamic_count = 0, specular_count = 0;
	for (i = 0; i < count; i++)
	{
		for (j = i; j > 0; j--)
		{
			const struct light_render_candidate *a = &lights[i], *b = &lights[order[j - 1]];
			if (!(a->weapon > b->weapon || (a->weapon == b->weapon &&
				(a->score > b->score || (a->score == b->score && a->id < b->id)))))
				break;
			order[j] = order[j - 1];
		}
		order[j] = i;
		diffuse[i] = specular[i] = 0;
	}
	for (j = 0; j < count; j++)
	{
		i = order[j];
		if (lights[i].weapon || !dynamic_limit || dynamic_count < dynamic_limit)
		{
			diffuse[i] = 1;
			dynamic_count++;
			if (lights[i].specular && (lights[i].weapon || !specular_limit || specular_count < specular_limit))
			{
				specular[i] = 1;
				specular_count++;
			}
		}
	}
}
#endif
