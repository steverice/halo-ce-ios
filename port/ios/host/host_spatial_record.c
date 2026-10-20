/* The pose stream's recorder and replayer (host_spatial_record.h). */
#include "host_spatial_record.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HEADER "halo pose recording 1\n"

struct record
{
	uint32_t frame, kind;
	struct halo_spatial_sample sample;
};

static FILE *record_file;
static uint32_t record_frame;
static double record_flushed;

/* the replay: each kind's samples in order, and the next one of each */
static struct halo_spatial_sample *replay_samples[2];
static long replay_kind_count[2], replay_kind_capacity[2], replay_next[2];

static double seconds_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

void host_spatial_record_start(const char *name)
{
	char path[256];

	if (!name[0])
		return;
	snprintf(path, sizeof(path), "%s.poses", name);
	record_file = fopen(path, "wb");
	if (!record_file)
	{
		host_logf(HOST_LOG_WARN, "spatial: cannot write %s; not recording poses", path);
		return;
	}
	fputs(HEADER, record_file);
	record_flushed = seconds_now();
	host_logf(HOST_LOG_INFO, "spatial: recording poses to %s", path);
}

void host_spatial_record_write(int kind, const struct halo_spatial_sample *sample)
{
	uint32_t header[2];
	double now;

	if (!record_file)
		return;
	if (kind == HALO_SPATIAL_SAMPLE_GAME)
		record_frame++;
	header[0] = record_frame;
	header[1] = (uint32_t)kind;
	fwrite(header, sizeof(header), 1, record_file);
	fwrite(sample, sizeof(*sample), 1, record_file);
	now = seconds_now();
	if (now - record_flushed >= 1.0)
	{
		record_flushed = now;
		fflush(record_file);
	}
}

int host_spatial_replay_start(const char *name)
{
	char path[256], line[64];
	FILE *file;
	struct record record;
	long count;

	if (!name[0])
		return 0;
	snprintf(path, sizeof(path), "%s.poses", name);
	file = fopen(path, "rb");
	if (!file)
	{
		host_logf(HOST_LOG_WARN, "spatial: cannot read %s; not replaying poses", path);
		return 0;
	}
	if (!fgets(line, sizeof(line), file) || strcmp(line, HEADER))
	{
		host_logf(HOST_LOG_WARN, "spatial: %s isn't a \"halo pose recording 1\" file; not replaying poses", path);
		fclose(file);
		return 0;
	}
	while (fread(&record, sizeof(record.frame) + sizeof(record.kind), 1, file) == 1 &&
		fread(&record.sample, sizeof(record.sample), 1, file) == 1)
	{
		if (record.kind > HALO_SPATIAL_SAMPLE_DRAW)
		{
			host_logf(HOST_LOG_WARN, "spatial: %s has a sample of kind %u; stopping there", path, record.kind);
			break;
		}
		if (replay_kind_count[record.kind] == replay_kind_capacity[record.kind])
		{
			long capacity = replay_kind_capacity[record.kind] ? replay_kind_capacity[record.kind] * 2 : 1024;
			struct halo_spatial_sample *grown = realloc(replay_samples[record.kind], (size_t)capacity * sizeof(*grown));

			if (!grown)
				break;
			replay_samples[record.kind] = grown;
			replay_kind_capacity[record.kind] = capacity;
		}
		replay_samples[record.kind][replay_kind_count[record.kind]++] = record.sample;
	}
	fclose(file);
	count = replay_kind_count[0] + replay_kind_count[1];
	if (!count)
	{
		host_logf(HOST_LOG_WARN, "spatial: %s has no samples; not replaying poses", path);
		return 0;
	}
	host_logf(HOST_LOG_INFO, "spatial: replaying %s (%ld samples)", name, count);
	return 1;
}

int host_spatial_replay_next(int kind, struct halo_spatial_sample *sample)
{
	long index;

	if (kind < 0 || kind > HALO_SPATIAL_SAMPLE_DRAW || !replay_kind_count[kind])
		return 0;
	/* past the last, the last again */
	index = replay_next[kind];
	if (index < replay_kind_count[kind])
		replay_next[kind]++;
	else
		index = replay_kind_count[kind] - 1;
	*sample = replay_samples[kind][index];
	sample->synthetic = 2;
	return 1;
}
