/* The pose recording keeps game and draw samples in independent replay order. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../host/host_spatial_record.c"

void host_logf(int priority, const char *format, ...)
{
	(void)priority;
	(void)format;
}

int main(void)
{
	char name[] = "/tmp/visr-spatial-record-XXXXXX";
	char path[sizeof(name) + sizeof(".poses")];
	struct halo_spatial_sample game = {0}, draw = {0}, read = {0};
	int fd = mkstemp(name);

	assert(fd >= 0);
	close(fd);
	unlink(name);
	snprintf(path, sizeof(path), "%s.poses", name);
	game.reason = HALO_SPATIAL_READY;
	game.hands[1].held = 1;
	draw.head_yaw = 0.5f;
	host_spatial_record_start(name);
	assert(record_file);
	host_spatial_record_write(HALO_SPATIAL_SAMPLE_GAME, &game);
	host_spatial_record_write(HALO_SPATIAL_SAMPLE_DRAW, &draw);
	host_spatial_record_write(HALO_SPATIAL_SAMPLE_GAME, &game);
	fclose(record_file);
	record_file = NULL;
	assert(host_spatial_replay_start(name));
	assert(host_spatial_replay_next(HALO_SPATIAL_SAMPLE_DRAW, &read));
	assert(read.head_yaw == 0.5f && read.synthetic == 2);
	assert(host_spatial_replay_next(HALO_SPATIAL_SAMPLE_GAME, &read));
	assert(read.hands[1].held == 1 && read.synthetic == 2);
	assert(host_spatial_replay_next(HALO_SPATIAL_SAMPLE_GAME, &read));
	assert(host_spatial_replay_next(HALO_SPATIAL_SAMPLE_GAME, &read));
	assert(read.hands[1].held == 1);
	unlink(path);
	return 0;
}
