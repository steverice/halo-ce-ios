/* The script extracts the real helpers, cache storage and lifecycle callbacks. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int boolean;
typedef float real;
typedef unsigned char byte;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAXIMUM_NODES_PER_MODEL 64
#define NUMBEROF(a) (sizeof(a) / sizeof((a)[0]))
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(i) ((i) & 65535)
#define PIN(x, a, b) ((x) < (a) ? (a) : ((x) > (b) ? (b) : (x)))
#define VALID_INDEX(i, n) ((i) >= 0 && (i) < (n))
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; } } while (0)

struct model_node { short sibling, child, parent_node_index; };
struct tag_block { long count; struct model_node *address; };
struct model { struct tag_block nodes; };
struct tag_validation { int unused; };
static boolean tag_validate_contains(struct tag_validation *v, void *p, unsigned long n)
{ (void)v; (void)p; (void)n; return TRUE; }
static void tag_validate_correct(struct tag_validation *v, const char *f, ...)
{ (void)v; (void)f; }
static void tag_validate_refuse(struct tag_validation *v, const char *f, ...)
{ (void)v; (void)f; }
static int node_reads;
static struct model_node *probe_node(struct tag_block *block, long index)
{
	if (++node_reads > 128 || !VALID_INDEX(index, block->count))
	{
		fprintf(stderr, "ancestry walk did not terminate inside its node block\n");
		exit(1);
	}
	return &block->address[index];
}
#define TAG_BLOCK_GET_ELEMENT(block, index, type) probe_node(block, index)

#include "under_test.inc"

int main(int argc, char **argv)
{
	const char *test = argc == 2 ? argv[1] : "";
	struct model_node nodes[3] = {{NONE, 1, NONE}, {2, NONE, 0}, {NONE, NONE, 0}};
	struct model model = {{3, nodes}};
	struct tag_validation validation = {0};

	if (!strcmp(test, "hierarchy"))
	{
		CHECK(unit_node_is_at_or_below(&model, 1, 1));
		CHECK(!unit_node_is_at_or_below(&model, 2, 1));
		CHECK(unit_node_is_at_or_below(&model, 2, 0));
		return 0;
	}
	if (!strcmp(test, "root"))
	{
		nodes[0].parent_node_index = 0;
		CHECK(node_tree_check(&validation, (byte *)nodes, 3, sizeof(*nodes),
			offsetof(struct model_node, sibling), offsetof(struct model_node, child), offsetof(struct model_node, parent_node_index)));
		CHECK(nodes[0].parent_node_index == 0);
		CHECK(!unit_node_is_at_or_below(&model, 0, 1));
		return 0;
	}
	if (!strcmp(test, "bounds"))
	{
		CHECK(!unit_node_is_at_or_below(&model, 3, 1));
		CHECK(!unit_node_is_at_or_below(&model, -2, 1));
		return 0;
	}
	if (!strcmp(test, "cycle"))
	{
		nodes[1].parent_node_index = 2;
		nodes[2].parent_node_index = 1;
		CHECK(!unit_node_is_at_or_below(&model, 1, 0));
		return 0;
	}

	CHECK(FALSE);
	return 1;
}
