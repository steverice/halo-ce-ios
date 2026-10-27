/*
IMMEDIATE_BATCH_PROBE.C

debug.immediate_merge's arithmetic (port/linux/src/immediate_batch.h): each primitive's triangles are the ones the
unmerged draw makes (a fan as the Metal backend converts it, a quad list as quad_indices does), and a stream of many
large fans, which fills a held draw again and again, splits at the vertex limit with every index in range and every
triangle kept, in order.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "immediate_batch.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

/* the unmerged draw's triangles, written out from the primitives' definitions (gpu_metal.m convert_primitive; d3d8_device.c
quad_indices), with every vertex shifted by base */
static unsigned long reference(int kind, unsigned long base, unsigned long count, unsigned short *out)
{
	unsigned long index, written = 0;

	if (kind == IMMEDIATE_BATCH_FAN)
	{
		for (index = 0; index + 2 < count; index++)
		{
			out[written++] = (unsigned short)(base + 0);
			out[written++] = (unsigned short)(base + index + 1);
			out[written++] = (unsigned short)(base + index + 2);
		}
	}
	else if (kind == IMMEDIATE_BATCH_QUADS)
	{
		for (index = 0; index + 3 < count; index += 4)
		{
			unsigned short v0 = (unsigned short)(base + index), v1 = v0 + 1, v2 = v0 + 2, v3 = v0 + 3;

			out[written++] = v0; out[written++] = v1; out[written++] = v2;
			out[written++] = v0; out[written++] = v2; out[written++] = v3;
		}
	}
	else
	{
		for (index = 0; index + 2 < count; index += 3)
		{
			out[written++] = (unsigned short)(base + index);
			out[written++] = (unsigned short)(base + index + 1);
			out[written++] = (unsigned short)(base + index + 2);
		}
	}
	return written;
}

static unsigned long generated(int kind, unsigned long base, unsigned long count, unsigned short *out)
{
	unsigned long triangles = immediate_batch_triangles(kind, count), triangle;

	for (triangle = 0; triangle < triangles; triangle++)
		immediate_batch_triangle(kind, base, triangle, out + triangle * 3);
	return triangles * 3;
}

static void primitives_match_their_unmerged_draws(void)
{
	static unsigned short expected[3 * 20000], actual[3 * 20000];
	int kind;
	unsigned long count;

	for (kind = IMMEDIATE_BATCH_FAN; kind <= IMMEDIATE_BATCH_TRIANGLES; kind++)
		for (count = 0; count < 70; count++)
		{
			unsigned long base = count * 37 % 1000;
			unsigned long want = reference(kind, base, count, expected), got = generated(kind, base, count, actual);

			CHECK(want == got);
			CHECK(!memcmp(expected, actual, want * sizeof(unsigned short)));
			CHECK(immediate_batch_triangles(kind, count) * 3 == want);
		}
	CHECK(immediate_batch_triangles(IMMEDIATE_BATCH_NONE, 12) == 0);
	CHECK(immediate_batch_triangles(IMMEDIATE_BATCH_FAN, 2) == 0);
	CHECK(immediate_batch_triangles(IMMEDIATE_BATCH_QUADS, 7) == 2);
}

static void the_limit_is_exact(void)
{
	CHECK(immediate_batch_fits(0, IMMEDIATE_BATCH_VERTICES));
	CHECK(!immediate_batch_fits(0, IMMEDIATE_BATCH_VERTICES + 1));
	CHECK(immediate_batch_fits(IMMEDIATE_BATCH_VERTICES - 3, 3));
	CHECK(!immediate_batch_fits(IMMEDIATE_BATCH_VERTICES - 3, 4));
	CHECK(IMMEDIATE_BATCH_VERTICES * 2 < 65536);
}

/* many large fans, near the limit: a held draw is flushed when the next primitive does not fit, as immediate_batch_add does */
static void many_large_fans_split_at_the_limit_and_keep_every_triangle(void)
{
	static const unsigned long sizes[] = { 120, 4, 15999, 3, 7000, 7001, 2000, 16000, 513, 9000, 8000 };
	static unsigned short batch[3 * 16000];
	unsigned long held = 0, indices = 0, batches = 0, triangles_in = 0, triangles_out = 0, round;

	for (round = 0; round < 3000; round++)
	{
		unsigned long count = round % 3 == 0 ? sizes[round / 3 % (sizeof(sizes) / sizeof(sizes[0]))] : 100 + round * 7 % 700;

		if (!immediate_batch_fits(held, count))
		{
			/* the held draw is made: in range, and no triangle lost */
			unsigned long index;

			for (index = 0; index < indices; index++)
				CHECK(batch[index] < held);
			triangles_out += indices / 3;
			batches++;
			held = indices = 0;
		}
		triangles_in += immediate_batch_triangles(IMMEDIATE_BATCH_FAN, count);
		indices += generated(IMMEDIATE_BATCH_FAN, held, count, batch + indices);
		held += count;
		CHECK(held <= IMMEDIATE_BATCH_VERTICES);
		CHECK(indices <= 3 * (IMMEDIATE_BATCH_VERTICES - 2));
	}
	triangles_out += indices / 3;
	CHECK(triangles_in == triangles_out);
	CHECK(batches > 100);
}

/* the triangles of consecutive fans, in one held draw, are each fan's triangles in order, shifted by what came before */
static void a_held_draw_is_its_fans_in_order(void)
{
	static unsigned short batch[3 * 4000], one[3 * 4000], expected[3 * 4000];
	unsigned long held = 0, indices = 0, expected_count = 0, round;

	for (round = 0; round < 40; round++)
	{
		unsigned long count = 3 + round * 11 % 90, written;

		indices += generated(IMMEDIATE_BATCH_FAN, held, count, batch + indices);
		written = reference(IMMEDIATE_BATCH_FAN, held, count, one);
		memcpy(expected + expected_count, one, written * sizeof(unsigned short));
		expected_count += written;
		held += count;
	}
	CHECK(indices == expected_count);
	CHECK(!memcmp(batch, expected, indices * sizeof(unsigned short)));
}

/* light volume batching (rasterizer_widget_sprite3d_batch_begin): a quad list of n sprites makes the triangles that n
four-vertex fans, held one after another, make, in the same order, for any n a held draw takes */
static void a_quad_list_of_sprites_is_their_fans(void)
{
	static unsigned short fans[3 * 20000], quads[3 * 20000];
	unsigned long sprites;

	for (sprites = 0; sprites <= IMMEDIATE_BATCH_VERTICES / 4; sprites += sprites < 64 ? 1 : 997)
	{
		unsigned long sprite, fan_count = 0, quad_count;

		for (sprite = 0; sprite < sprites; sprite++)
			fan_count += generated(IMMEDIATE_BATCH_FAN, sprite * 4, 4, fans + fan_count);
		quad_count = generated(IMMEDIATE_BATCH_QUADS, 0, sprites * 4, quads);
		CHECK(fan_count == sprites * 6);
		CHECK(quad_count == fan_count);
		CHECK(!memcmp(fans, quads, fan_count * sizeof(unsigned short)));
		CHECK(immediate_batch_fits(0, sprites * 4));
	}
}

int main(void)
{
	primitives_match_their_unmerged_draws();
	the_limit_is_exact();
	many_large_fans_split_at_the_limit_and_keep_every_triangle();
	a_held_draw_is_its_fans_in_order();
	a_quad_list_of_sprites_is_their_fans();
	if (failures)
	{
		fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	puts("immediate batch probe: ok");
	return 0;
}
