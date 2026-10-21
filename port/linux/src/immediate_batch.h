/*
IMMEDIATE_BATCH.H

The arithmetic of debug.immediate_merge (d3d8_device.c): which immediate-mode primitives are held, how many triangles
each makes, and the triangle list's indices, so the held draws of one state become one indexed draw. Plain C, for
port/ios/tests/immediate_batch_probe.c.

A fan of n vertices is the triangles (0, i + 1, i + 2) for i from 0 to n - 3, as the Metal backend converts one
(convert_primitive); a quad list's quads are (0, 1, 2) and (0, 2, 3); a triangle list's are taken in turn.
*/
#ifndef IMMEDIATE_BATCH_H
#define IMMEDIATE_BATCH_H

/* the most vertices one held draw takes: its indices are 16 bits, and each vertex is 256 bytes */
#define IMMEDIATE_BATCH_VERTICES 16000

enum
{
	IMMEDIATE_BATCH_NONE,
	IMMEDIATE_BATCH_FAN,
	IMMEDIATE_BATCH_QUADS,
	IMMEDIATE_BATCH_TRIANGLES
};

/* how many triangles count vertices of a kind make (0: not held) */
static inline unsigned long immediate_batch_triangles(int kind, unsigned long count)
{
	switch (kind)
	{
	case IMMEDIATE_BATCH_FAN: return count >= 3 ? count - 2 : 0;
	case IMMEDIATE_BATCH_QUADS: return count / 4 * 2;
	case IMMEDIATE_BATCH_TRIANGLES: return count / 3;
	default: return 0;
	}
}

/* whether count more vertices still fit in a draw that holds held already */
static inline int immediate_batch_fits(unsigned long held, unsigned long count)
{
	return count <= IMMEDIATE_BATCH_VERTICES && held + count <= IMMEDIATE_BATCH_VERTICES;
}

/* the indices of one triangle of a primitive whose first vertex is base */
static inline void immediate_batch_triangle(int kind, unsigned long base, unsigned long triangle, unsigned short out[3])
{
	if (kind == IMMEDIATE_BATCH_QUADS)
	{
		unsigned long quad = triangle / 2 * 4;

		out[0] = (unsigned short)(base + quad);
		out[1] = (unsigned short)(base + quad + (triangle % 2 == 0 ? 1 : 2));
		out[2] = (unsigned short)(base + quad + (triangle % 2 == 0 ? 2 : 3));
	}
	else if (kind == IMMEDIATE_BATCH_TRIANGLES)
	{
		out[0] = (unsigned short)(base + triangle * 3);
		out[1] = (unsigned short)(base + triangle * 3 + 1);
		out[2] = (unsigned short)(base + triangle * 3 + 2);
	}
	else
	{
		out[0] = (unsigned short)base;
		out[1] = (unsigned short)(base + triangle + 1);
		out[2] = (unsigned short)(base + triangle + 2);
	}
}

#endif
