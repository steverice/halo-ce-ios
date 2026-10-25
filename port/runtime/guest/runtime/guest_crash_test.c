/*
GUEST_CRASH_TEST.C

debug.crash_test: a fatal error made on purpose in guest code, to check what
a crash report shows (port/ios/host/host_crash.c, tools/guest_symbolicate.py).
The game's main loop calls guest_crash_test_update every frame
(source/main/main.c); at the frame CRASH_TEST_FRAME it fails in the way the
setting names:

- "halt": a failed assertion, through the game's own display_assert and
  system_exit, as a real one goes (the halt screen, or a crash).
- "fault": a write to guest address 0x10, a memory fault in guest code.
- "trap": a trap instruction in guest code, as musl's abort ends.
- "abort": the guest runtime's host_abort, a fatal error the host is told of.
- "map": the first level load fails as a damaged map file's does
  (main_new_map), which shows the player a message instead of crashing.

Each goes through two functions of its own first, kept out of line, so that
a report's guest frames can be checked against known names.
*/

#include "guest_host.h"
#include "port_config.h"

#include <string.h>

/* (the main menu has drawn for a few seconds by then) */
#define CRASH_TEST_FRAME 150

/* source/cseries/cseries.h */
void display_assert(char *information, char *file, long line, int fatal);
void system_exit(long code);

__attribute__((noinline)) static void guest_crash_test_fail(const char *mode)
{
	if (!strcmp(mode, "halt"))
	{
		display_assert("debug.crash_test", __FILE__, __LINE__, 1);
		system_exit(-1);
	}
	else if (!strcmp(mode, "fault"))
	{
		*(volatile int *)(unsigned long)0x10 = 1;
	}
	else if (!strcmp(mode, "trap"))
	{
		__builtin_trap();
	}
	else if (!strcmp(mode, "abort"))
	{
		host_abort("debug.crash_test");
	}
	__asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) static void guest_crash_test_frame(const char *mode)
{
	guest_crash_test_fail(mode);
	__asm__ volatile("" ::: "memory");
}

/* nonzero once, for debug.crash_test "map": main_new_map then takes its game_load failure path */
int guest_crash_test_map_load(void)
{
	static int done;

	if (done || strcmp(config_string("debug.crash_test"), "map"))
		return 0;
	done = 1;
	host_log(6, "debug.crash_test: failing the level load");
	return 1;
}

void guest_crash_test_update(void)
{
	static int frames = -1;
	static const char *mode;

	if (frames < 0)
	{
		mode = config_string("debug.crash_test");
		frames = 0;
	}
	if (!*mode || !strcmp(mode, "map") || ++frames != CRASH_TEST_FRAME)
		return;
	host_log(6, "debug.crash_test: failing now");
	guest_crash_test_frame(mode);
}
