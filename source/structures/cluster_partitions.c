/*
CLUSTER_PARTITIONS.C

symbols in this file:
00180C10 0080:
	_reference_list_remove (0000)
00180C90 00c0:
	_reference_list_copy (0000)
00180D50 00d0:
	_cluster_partition_new (0000)
00180E20 0030:
	_cluster_partition_make_valid (0000)
00180E50 0030:
	_cluster_partition_make_invalid (0000)
00180E80 0030:
	_cluster_partition_delete (0000)
00180EB0 0030:
	_cluster_partition_get_next_datum (0000)
00180EE0 0040:
	_cluster_partition_get_first_cluster (0000)
00180F20 0030:
	_cluster_partition_get_next_cluster (0000)
00180F50 0050:
	_cluster_partition_copy (0000)
00180FA0 0050:
	_cluster_partition_get_first_reference (0000)
00180FF0 0200:
	_cluster_partition_reconnect (0000)
001811F0 00b0:
	_cluster_partition_disconnect (0000)
001812A0 0080:
	_cluster_partition_get_first_datum (0000)
002A0A94 003a:
	??_C@_0DK@NMKBPPFN@attempt?5to?5remove?5invalid?5elemen@ (0000)
002A0AD0 001d:
	??_C@_0BN@NFEHOLIC@?4?4?2objects?2reference_lists?4h?$AA@ (0000)
002A0AF0 002d:
	??_C@_0CN@HMHOKAKO@result?9?$DOmaximum_count?$DN?$DNsource?9?$DOm@ (0000)
002A0B20 001b:
	??_C@_0BL@LCCDAOLG@result?9?$DOsize?$DN?$DNsource?9?$DOsize?$AA@ (0000)
002A0B3C 002f:
	??_C@_0CP@NPCAPMLK@couldn?8t?5allocate?5?$CFs?5cluster?5par@ (0000)
002A0B6C 000b:
	??_C@_0L@LMJLHDGO@?$CFs?5cluster?$AA@ (0000)
002A0B78 000b:
	??_C@_0L@FHNEJCED@cluster?5?$CFs?$AA@ (0000)
002A0B84 0013:
	??_C@_0BD@HGMPGNNE@cluster?5references?$AA@ (0000)
002A0B98 004d:
	??_C@_0EN@JKAMLONC@cluster_index?$DO?$DN0?5?$CG?$CG?5cluster_inde@ (0000)
002A0BE8 002f:
	??_C@_0CP@DHFJNEOJ@c?3?2halo?2SOURCE?2structures?2cluste@ (0000)
002A0C18 0028:
	??_C@_0CI@IEBPGENP@an?5object?5or?5light?5spanned?5?$CFd?5cl@ (0000)
002A0C40 001f:
	??_C@_0BP@ELGEMBFB@?$CKfirst_cluster_reference?$DN?$DNNONE?$AA@ (0000)
002A0C60 0018:
	??_C@_0BI@INKNBGDF@first_cluster_reference?$AA@ (0000)
002A0C78 000a:
	??_C@_09IKAEIPAD@partition?$AA@ (0000)
*/

/* ---------- headers */

#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "memory/data.h"
#include "objects/objects.h"
#include "objects/reference_lists.h"
#include "saved games/game_state.h"
#include "scenario/scenario.h"
#include "cluster_partitions.h"
#include "structure_bsp_definitions.h"
#include "structures/structures.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes */

void reference_list_remove(
	struct data_array *array,
	long *first_reference_index,
	long datum_index);
void reference_list_copy(
	struct data_array *result,
	struct data_array *source);

static long *cluster_partition_get_first_reference(
	struct cluster_partition *partition,
	short cluster_index);
static void cluster_partition_port_forget(
	struct cluster_partition *partition);
static long cluster_partition_port_add(
	struct data_array *array,
	long *first_reference_index,
	long datum_index,
	long *previous_references);
static void cluster_partition_port_remove(
	struct cluster_partition *partition,
	short cluster_index,
	long datum_index,
	long found_reference_index);
static void cluster_partition_port_candidate_add(
	struct cluster_partition *partition,
	short cluster_index,
	long reference_index,
	long datum_index);
static void cluster_partition_port_candidate_remove(
	struct cluster_partition *partition,
	short cluster_index,
	long reference_index);
static void cluster_partition_port_candidates_rebuild(
	struct cluster_partition *partition);

/* ---------- globals */

/* port: the partitions (cluster_partition_new), whose references' places
cluster_partitions_port_forget forgets */
enum
{
	MAXIMUM_PORT_CLUSTER_PARTITIONS = 8,
};
static struct cluster_partition *cluster_partitions_port[MAXIMUM_PORT_CLUSTER_PARTITIONS];
static short cluster_partitions_port_count;
/* port: the list of a cluster index that fits no cluster's
(cluster_partition_get_first_reference) */
static long cluster_partition_port_no_cluster_first_reference;

/* ---------- public code */

void reference_list_remove(
	struct data_array *array,
	long *first_reference_index,
	long datum_index)
{
	long *reference_index = first_reference_index;
	struct data_reference *reference;

	while (*reference_index != NONE)
	{
		reference = (struct data_reference *)datum_get(array, *reference_index);
		if (reference->datum_index == datum_index)
		{
			datum_delete(array, *reference_index);
			*reference_index = reference->next_reference_index;

			return;
		}

		reference_index = &reference->next_reference_index;
	}

	match_vassert(
		"..\\objects\\reference_lists.h",
		0x6d,
		FALSE,
		csprintf(temporary, "attempt to remove invalid element %ld from reference list", datum_index));

	return;
}

/* The initialization and cursor-advance order preserve January's coalescing
 * of the source cursor into EBX and the loop index into ESI. */
void reference_list_copy(
	struct data_array *result,
	struct data_array *source)
{
	short absolute_index;
	struct data_reference *source_reference;
	struct data_reference *result_reference;

	match_assert("..\\objects\\reference_lists.h", 0x88, result->size==source->size);
	match_assert("..\\objects\\reference_lists.h", 0x89, result->maximum_count==source->maximum_count);
	absolute_index = 0;
	result_reference = result->data;
	source_reference = source->data;
	while (absolute_index < result->maximum_count)
	{
		if (source_reference->identifier)
		{
			*result_reference = *source_reference;
		}
		else if (result_reference->identifier)
		{
			datum_delete(result, absolute_index);
		}

		absolute_index++;
		source_reference++;
		result_reference++;
	}

	return;
}

void cluster_partition_new(
	struct cluster_partition *partition,
	char const *name)
{
	char cluster_name[256];

	partition->cluster_first_data_references = game_state_malloc(
		name,
		"cluster references",
		MAXIMUM_CLUSTERS_PER_STRUCTURE * sizeof(*partition->cluster_first_data_references));

	sprintf(cluster_name, "cluster %s", name);
	/* the native builds' longer reference lists (halo_port_capacity.h): an
	object or light that cannot be referenced drops out of its clusters */
	partition->data_reference_data = reference_list_new(cluster_name, HALO_PORT_MAXIMUM_CLUSTER_REFERENCES);

	sprintf(cluster_name, "%s cluster", name);
	partition->cluster_reference_data = reference_list_new(cluster_name, HALO_PORT_MAXIMUM_CLUSTER_REFERENCES);

	if (!partition->cluster_first_data_references ||
		!partition->cluster_reference_data ||
		!partition->data_reference_data)
	{
		error(_error_immediate, "couldn't allocate %s cluster partition globals", name);
	}

	/* port: where its references are (cluster_partition_port_remove); none
	known yet */
	partition->port_previous_references = debug_malloc(HALO_PORT_MAXIMUM_CLUSTER_REFERENCES * sizeof(long), FALSE,
		__FILE__, __LINE__);
	partition->port_cluster_data_references = debug_malloc(HALO_PORT_MAXIMUM_CLUSTER_REFERENCES * sizeof(long), FALSE,
		__FILE__, __LINE__);
	cluster_partition_port_forget(partition);
	if (cluster_partitions_port_count < MAXIMUM_PORT_CLUSTER_PARTITIONS)
		cluster_partitions_port[cluster_partitions_port_count++] = partition;

	return;
}

void cluster_partition_make_valid(
	struct cluster_partition *partition)
{
	csmemset(
		partition->cluster_first_data_references,
		NONE,
		MAXIMUM_CLUSTERS_PER_STRUCTURE * sizeof(*partition->cluster_first_data_references));
	data_make_valid(partition->cluster_reference_data);
	data_make_valid(partition->data_reference_data);
	cluster_partition_port_forget(partition);

	return;
}

void cluster_partition_make_invalid(
	struct cluster_partition *partition)
{
	if (partition->cluster_reference_data->valid)
		data_make_invalid(partition->cluster_reference_data);

	if (partition->data_reference_data->valid)
		data_make_invalid(partition->data_reference_data);

	return;
}

void cluster_partition_delete(
	struct cluster_partition *partition)
{
	if (partition->cluster_first_data_references)
		partition->cluster_first_data_references = NULL;

	if (partition->cluster_reference_data)
		partition->cluster_reference_data = NULL;

	if (partition->data_reference_data)
		partition->data_reference_data = NULL;

	return;
}

void cluster_partition_copy(
	struct cluster_partition *result,
	struct cluster_partition const *source)
{
	/* port: no more clusters than the partitions hold (a map's bsp may say
	it has more) */
	csmemcpy(
		result->cluster_first_data_references,
		source->cluster_first_data_references,
		PIN(global_structure_bsp_get()->clusters.count, 0, MAXIMUM_CLUSTERS_PER_STRUCTURE) *
			sizeof(*result->cluster_first_data_references));
	reference_list_copy(
		result->cluster_reference_data,
		source->cluster_reference_data);
	reference_list_copy(
		result->data_reference_data,
		source->data_reference_data);
	cluster_partition_port_forget(result);

	return;
}

void cluster_partitions_port_forget(
	void)
{
	short index;

	for (index = 0; index < cluster_partitions_port_count; index++)
		cluster_partition_port_forget(cluster_partitions_port[index]);
}

long cluster_partition_get_first_cluster(
	struct cluster_partition const *partition,
	long *reference_index,
	long first_cluster_reference)
{
	*reference_index = first_cluster_reference;

	return reference_list_get_next_datum_index(partition->cluster_reference_data, reference_index);
}

long cluster_partition_get_next_cluster(
	struct cluster_partition const *partition,
	long *reference_index)
{
	return reference_list_get_next_datum_index(partition->cluster_reference_data, reference_index);
}

void cluster_partition_reconnect(
	struct cluster_partition *partition,
	long datum_index,
	long *first_cluster_reference,
	real_point3d const *position,
	float radius,
	struct location const *location)
{
	short cluster_indices[64];
	short cluster_count;

	cluster_partition_port_reconnect_recording(
		partition,
		datum_index,
		first_cluster_reference,
		position,
		radius,
		location,
		cluster_indices,
		&cluster_count);

	return;
}

/* port: cluster_partition_reconnect, noting the clusters it joined (in the order it joined them) */
void cluster_partition_port_reconnect_recording(
	struct cluster_partition *partition,
	long datum_index,
	long *first_cluster_reference,
	real_point3d const *position,
	float radius,
	struct location const *location,
	short *cluster_indices,
	short *cluster_count_result)
{
	short cluster_count;

	match_assert("c:\\halo\\SOURCE\\structures\\cluster_partitions.c", 0x6f, partition);
	match_assert("c:\\halo\\SOURCE\\structures\\cluster_partitions.c", 0x70, first_cluster_reference);
	match_assert("c:\\halo\\SOURCE\\structures\\cluster_partitions.c", 0x71, *first_cluster_reference==NONE);
	match_assert("c:\\halo\\SOURCE\\structures\\cluster_partitions.c", 0x72, position);
	match_assert("c:\\halo\\SOURCE\\structures\\cluster_partitions.c", 0x73, location);

	cluster_count = structure_clusters_in_sphere(
		location->cluster_index,
		position,
		radius,
		64,
		cluster_indices);

	if (cluster_count > 64)
	{
		error(_error_silent, "an object or light spanned %d clusters.", cluster_count);
		cluster_count = 64;
	}

	*cluster_count_result = cluster_count;
	cluster_partition_port_reconnect_clusters(
		partition,
		datum_index,
		first_cluster_reference,
		cluster_indices,
		cluster_count);

	return;
}

/* port: the joining half of cluster_partition_reconnect, for clusters already known: the datum goes to the head of
each cluster's list in the order given, as it does when the clusters are found again */
void cluster_partition_port_reconnect_clusters(
	struct cluster_partition *partition,
	long datum_index,
	long *first_cluster_reference,
	short const *cluster_indices,
	short cluster_count)
{
	short cluster_index_index;

	for (cluster_index_index = 0; cluster_index_index < cluster_count; cluster_index_index++)
	{
		short const cluster_index = cluster_indices[cluster_index_index];
		/* port: reference_list_add's, noting where the references are */
		long cluster_reference_index = cluster_partition_port_add(
			partition->cluster_reference_data,
			first_cluster_reference,
			cluster_index,
			NULL);
		long data_reference_index = cluster_partition_port_add(
			partition->data_reference_data,
			cluster_partition_get_first_reference(partition, cluster_index),
			datum_index,
			partition->port_previous_references);

		partition->port_modification_count++;
		if (data_reference_index != NONE && partition->port_candidates_valid)
			cluster_partition_port_candidate_add(partition, cluster_index, data_reference_index, datum_index);
		if (cluster_reference_index != NONE && partition->port_cluster_data_references)
		{
			partition->port_cluster_data_references[DATUM_INDEX_TO_ABSOLUTE_INDEX(cluster_reference_index)] =
				data_reference_index;
		}
	}

	return;
}

void cluster_partition_disconnect(
	struct cluster_partition *partition,
	long datum_index,
	long *first_cluster_reference)
{
	long cluster_reference_index = *first_cluster_reference;

	while (cluster_reference_index != NONE)
	{
		struct data_reference *cluster_reference = (struct data_reference *)datum_get(
			partition->cluster_reference_data,
			cluster_reference_index);
		short const cluster_index = (short)cluster_reference->datum_index;
		long data_reference_index = partition->port_cluster_data_references ?
			partition->port_cluster_data_references[DATUM_INDEX_TO_ABSOLUTE_INDEX(cluster_reference_index)] : NONE;

		datum_delete(partition->cluster_reference_data, cluster_reference_index);

		/* port: reference_list_remove's, starting where the reference was
		put */
		cluster_partition_port_remove(
			partition,
			cluster_index,
			datum_index,
			data_reference_index);

		cluster_reference_index = cluster_reference->next_reference_index;
	}

	*first_cluster_reference = NONE;

	return;
}

long cluster_partition_get_first_datum(
	struct cluster_partition const *partition,
	long *reference_index,
	short cluster_index)
{
	*reference_index = *cluster_partition_get_first_reference((struct cluster_partition *)partition, cluster_index);

	return reference_list_get_next_datum_index(partition->data_reference_data, reference_index);
}

/* port: Candidate lists. A collision ray that tests only some object types
stepped through every object in each cluster it crossed and passed over most
of them on their type. A partition with candidates keeps a second list per
cluster holding only the references of the data its candidate function
accepts. A reference joins it at the head when it joins the head of its
cluster's list (cluster_partition_reconnect) and leaves it when it leaves
that list (cluster_partition_port_remove), and a datum's answer does not
change while it is listed, so each candidate list is its cluster's list with
the others taken out, in the same order. The lists are not in the game
state: they are dropped with the references' places
(cluster_partition_port_forget) and built again from the clusters' lists
when next walked. */

void cluster_partition_port_enable_candidates(
	struct cluster_partition *partition,
	boolean (*candidate)(long datum_index))
{
	partition->port_candidates = debug_malloc(
		HALO_PORT_MAXIMUM_CLUSTER_REFERENCES * sizeof(*partition->port_candidates), FALSE, __FILE__, __LINE__);
	partition->port_cluster_first_candidates = debug_malloc(
		MAXIMUM_CLUSTERS_PER_STRUCTURE * sizeof(*partition->port_cluster_first_candidates), FALSE, __FILE__, __LINE__);
	partition->port_candidate = partition->port_candidates && partition->port_cluster_first_candidates ? candidate : NULL;
	partition->port_candidates_valid = FALSE;
}

boolean cluster_partition_port_candidates_ready(
	struct cluster_partition *partition)
{
	if (!partition->port_candidate)
		return FALSE;
	if (!partition->port_candidates_valid)
		cluster_partition_port_candidates_rebuild(partition);

	return TRUE;
}

long cluster_partition_port_get_first_candidate(
	struct cluster_partition const *partition,
	long *reference_index,
	short cluster_index)
{
	/* (a cluster index that fits no cluster's list has an empty list, as in
	cluster_partition_get_first_reference) */
	*reference_index = cluster_index >= 0 && cluster_index < MAXIMUM_CLUSTERS_PER_STRUCTURE ?
		partition->port_cluster_first_candidates[cluster_index] : NONE;

	return cluster_partition_port_get_next_candidate(partition, reference_index);
}

long cluster_partition_port_get_next_candidate(
	struct cluster_partition const *partition,
	long *reference_index)
{
	struct cluster_partition_port_candidate const *candidate;

	if (*reference_index == NONE)
		return NONE;
	candidate = &partition->port_candidates[*reference_index];
	*reference_index = candidate->next;

	return candidate->datum_index;
}

/* ---------- private code */

/* port: Where each reference is in its cluster's list, so a datum leaving
a cluster is found there at once. Every moving object and light leaves its
clusters and joins them again each tick (reference_list_add puts it first),
and reference_list_remove walked the list from its start: with hundreds of
lights in a few clusters (network co-op's extra enemies) those walks took
much of the tick. Each reference's place is checked before it is used, and
the list walked as before when it is wrong: the lists come out as they did,
every reference in the same place. The places are not in the game state, so
they are forgotten when it is loaded (cluster_partitions_port_forget) or a
new map makes the lists anew. */

static void cluster_partition_port_forget(
	struct cluster_partition *partition)
{
	partition->port_modification_count++;
	/* (the candidate lists are rebuilt from the clusters' lists when next
	walked) */
	partition->port_candidates_valid = FALSE;
	if (partition->port_previous_references)
		csmemset(partition->port_previous_references, 0xFF, HALO_PORT_MAXIMUM_CLUSTER_REFERENCES * sizeof(long));
	if (partition->port_cluster_data_references)
		csmemset(partition->port_cluster_data_references, 0xFF, HALO_PORT_MAXIMUM_CLUSTER_REFERENCES * sizeof(long));
}

/* reference_list_add's, and the new reference's place: first, before the
one that was (previous_references, if given) */
static long cluster_partition_port_add(
	struct data_array *array,
	long *first_reference_index,
	long datum_index,
	long *previous_references)
{
	long reference_index = datum_new(array);

	if (reference_index != NONE)
	{
		struct data_reference *reference = (struct data_reference *)datum_get(array, reference_index);

		reference->datum_index = datum_index;
		reference->next_reference_index = *first_reference_index;
		if (previous_references)
		{
			previous_references[DATUM_INDEX_TO_ABSOLUTE_INDEX(reference_index)] = NONE;
			if (*first_reference_index != NONE)
				previous_references[DATUM_INDEX_TO_ABSOLUTE_INDEX(*first_reference_index)] = reference_index;
		}
		*first_reference_index = reference_index;
	}
	else
	{
		error(_error_silent, "WARNING: maximum %ss per map (%d) exceeded.", array->name, array->maximum_count);
	}

	return reference_index;
}

/* reference_list_remove's on a cluster's list, at found_reference_index
(where the datum's reference was put) if it is there */
static void cluster_partition_port_remove(
	struct cluster_partition *partition,
	short cluster_index,
	long datum_index,
	long found_reference_index)
{
	long *first_reference_index = cluster_partition_get_first_reference(partition, cluster_index);
	struct data_array *array = partition->data_reference_data;
	long *previous_references = partition->port_previous_references;
	long reference_index = NONE;
	long previous_index = NONE;
	struct data_reference *reference;
	long next_index;

	/* (the reference put there, still this datum's, and first in this
	cluster's list or after the one noted before it) */
	if (previous_references && found_reference_index != NONE)
	{
		reference = (struct data_reference *)datum_try_and_get(array, found_reference_index);
		if (reference && reference->datum_index == datum_index)
		{
			long noted_previous = previous_references[DATUM_INDEX_TO_ABSOLUTE_INDEX(found_reference_index)];
			struct data_reference *previous = noted_previous != NONE ?
				(struct data_reference *)datum_try_and_get(array, noted_previous) : NULL;

			if (noted_previous == NONE ? *first_reference_index == found_reference_index :
				previous && previous->next_reference_index == found_reference_index)
			{
				reference_index = found_reference_index;
				previous_index = noted_previous;
			}
		}
	}
	/* (else the list walked, as reference_list_remove does) */
	if (reference_index == NONE)
	{
		long index = *first_reference_index;

		while (index != NONE)
		{
			reference = (struct data_reference *)datum_get(array, index);
			if (reference->datum_index == datum_index)
			{
				reference_index = index;
				break;
			}
			previous_index = index;
			index = reference->next_reference_index;
		}
	}
	if (reference_index == NONE)
	{
		match_vassert(
			"..\\objects\\reference_lists.h",
			0x6d,
			FALSE,
			csprintf(temporary, "attempt to remove invalid element %ld from reference list", datum_index));
		return;
	}
	partition->port_modification_count++;
	if (partition->port_candidates_valid)
		cluster_partition_port_candidate_remove(partition, cluster_index, reference_index);
	reference = (struct data_reference *)datum_get(array, reference_index);
	next_index = reference->next_reference_index;
	datum_delete(array, reference_index);
	if (previous_index == NONE)
		*first_reference_index = next_index;
	else
		((struct data_reference *)datum_get(array, previous_index))->next_reference_index = next_index;
	if (previous_references && next_index != NONE)
		previous_references[DATUM_INDEX_TO_ABSOLUTE_INDEX(next_index)] = previous_index;
}

static long *cluster_partition_get_first_reference(
	struct cluster_partition *partition,
	short cluster_index)
{
	match_assert(
		"c:\\halo\\SOURCE\\structures\\cluster_partitions.c",
		0xd5,
		cluster_index>=0 && cluster_index<global_structure_bsp_get()->clusters.count);

	/* port: a cluster index that fits no cluster's list has an empty list
	(what is put in it is not kept), not one past the lists */
	if (cluster_index < 0 || cluster_index >= MAXIMUM_CLUSTERS_PER_STRUCTURE)
	{
		cluster_partition_port_no_cluster_first_reference = NONE;
		return &cluster_partition_port_no_cluster_first_reference;
	}

	return &partition->cluster_first_data_references[cluster_index];
}

/* the reference just put first in its cluster's list put first in the
cluster's candidate list, if its datum is a candidate */
static void cluster_partition_port_candidate_add(
	struct cluster_partition *partition,
	short cluster_index,
	long reference_index,
	long datum_index)
{
	long const absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(reference_index);
	struct cluster_partition_port_candidate *candidate = &partition->port_candidates[absolute_index];

	candidate->member = cluster_index >= 0 && cluster_index < MAXIMUM_CLUSTERS_PER_STRUCTURE &&
		partition->port_candidate(datum_index);
	if (candidate->member)
	{
		long *first = &partition->port_cluster_first_candidates[cluster_index];

		candidate->datum_index = datum_index;
		candidate->previous = NONE;
		candidate->next = *first;
		if (*first != NONE)
			partition->port_candidates[*first].previous = absolute_index;
		*first = absolute_index;
	}
}

/* the reference leaving its cluster's list taken out of the cluster's
candidate list, if it is in it */
static void cluster_partition_port_candidate_remove(
	struct cluster_partition *partition,
	short cluster_index,
	long reference_index)
{
	long const absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(reference_index);
	struct cluster_partition_port_candidate *candidate = &partition->port_candidates[absolute_index];

	if (!candidate->member)
		return;
	if (candidate->previous == NONE)
		partition->port_cluster_first_candidates[cluster_index] = candidate->next;
	else
		partition->port_candidates[candidate->previous].next = candidate->next;
	if (candidate->next != NONE)
		partition->port_candidates[candidate->next].previous = candidate->previous;
	candidate->member = FALSE;
}

/* every cluster's candidate list made again from its list, in its order */
static void cluster_partition_port_candidates_rebuild(
	struct cluster_partition *partition)
{
	short cluster_index;

	csmemset(partition->port_candidates, 0, HALO_PORT_MAXIMUM_CLUSTER_REFERENCES * sizeof(*partition->port_candidates));
	for (cluster_index = 0; cluster_index < MAXIMUM_CLUSTERS_PER_STRUCTURE; cluster_index++)
	{
		long *first = &partition->port_cluster_first_candidates[cluster_index];
		long last = NONE;
		long reference_index = partition->cluster_first_data_references[cluster_index];

		*first = NONE;
		while (reference_index != NONE)
		{
			struct data_reference const *reference = (struct data_reference const *)datum_get(
				partition->data_reference_data,
				reference_index);
			long const absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(reference_index);
			struct cluster_partition_port_candidate *candidate = &partition->port_candidates[absolute_index];

			if (partition->port_candidate(reference->datum_index))
			{
				candidate->member = TRUE;
				candidate->datum_index = reference->datum_index;
				candidate->previous = last;
				candidate->next = NONE;
				if (last == NONE)
					*first = absolute_index;
				else
					partition->port_candidates[last].next = absolute_index;
				last = absolute_index;
			}
			reference_index = reference->next_reference_index;
		}
	}
	partition->port_candidates_valid = TRUE;
}
