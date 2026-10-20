"""The Linux build's write tracking and crash report (port/linux/src/memory_watch.c): memory_watch.c compiled as
it is, beside a platform.h of the test's (a watched window of the test's own memory), with a main that installs
other handlers as a library would, writes to watched pages and faults. Needs a C compiler (clang, or CC), not
the game.

    python -m pytest --quiet tools/test_memory_watch.py
"""

import os
import shutil
import signal
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(sys.platform != "linux", reason="Linux signal and ucontext behavior")

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port/linux/src/memory_watch.c"

PLATFORM_H = r"""
#ifndef TEST_PLATFORM_H
#define TEST_PLATFORM_H
typedef int BOOL;
#define TRUE 1
#define FALSE 0
extern unsigned long test_window;
#define PLATFORM_CONTIGUOUS_BASE test_window
#define PLATFORM_CONTIGUOUS_SIZE 0x100000UL
BOOL platform_is_contiguous(const void *address);
#endif
"""

MAIN = r"""
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "platform.h"

void memory_watch_initialize(void);
void memory_watch_protect(unsigned long address, unsigned long size);
unsigned long memory_watch_generation(unsigned long address, unsigned long size);
void memory_watch_begin_frame(void);

unsigned long test_window;
static volatile int debug_lines, foreign_calls;
static volatile sig_atomic_t mask_seen, self_blocked, once_calls, bus_calls, fpe_calls, on_alt_stack;
static volatile sig_atomic_t fault_in_write, faulted_in_write;
static char alternate_stack[65536];
static sigjmp_buf foreign_jump;
static struct sigaction foreign_previous;
static pthread_barrier_t worker_barrier;

ssize_t write(int descriptor, const void *buffer, size_t count)
{
	if (fault_in_write && !faulted_in_write++)
		*(volatile int *)0x10 = 1;
	return syscall(SYS_write, descriptor, buffer, count);
}

BOOL platform_is_contiguous(const void *address)
{
	return (unsigned long)address - test_window < PLATFORM_CONTIGUOUS_SIZE;
}

void write_to_error_file(char *string, unsigned char date)
{
	(void)string;
	(void)date;
	debug_lines++;
}

static void mask_handler(int signal_number)
{
	sigset_t current;
	sigprocmask(SIG_SETMASK, NULL, &current);
	mask_seen = sigismember(&current, SIGUSR1);
	self_blocked = sigismember(&current, signal_number);
}

static void once_handler(int signal_number)
{
	(void)signal_number;
	once_calls++;
}

static void bus_handler(int signal_number)
{
	(void)signal_number;
	bus_calls++;
}

static void fpe_handler_raising_bus(int signal_number)
{
	(void)signal_number;
	fpe_calls++;
	raise(SIGBUS);
}

static void alt_stack_handler(int signal_number)
{
	char marker;
	(void)signal_number;
	on_alt_stack = (uintptr_t)&marker >= (uintptr_t)alternate_stack &&
		(uintptr_t)&marker < (uintptr_t)(alternate_stack + sizeof(alternate_stack));
}

static void install_handler(int signal_number, void (*handler)(int), int flags, int mask_signal)
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = handler;
	action.sa_flags = flags;
	sigemptyset(&action.sa_mask);
	if (mask_signal)
		sigaddset(&action.sa_mask, mask_signal);
	if (sigaction(signal_number, &action, NULL) != 0)
		exit(101);
	if (flags & SA_SIGINFO)
	{
		struct sigaction installed;
		if (sigaction(signal_number, NULL, &installed) != 0 || !(installed.sa_flags & SA_SIGINFO))
			exit(106);
	}
}

/* a library's handler that handles the faults itself (and jumps out) */
static void foreign_jumping(int signal_number, siginfo_t *information, void *context)
{
	(void)signal_number;
	(void)information;
	(void)context;
	foreign_calls++;
	siglongjmp(foreign_jump, 1);
}

/* ... one that hands the fault to the handler it found (this file's) */
static void foreign_chaining(int signal_number, siginfo_t *information, void *context)
{
	foreign_calls++;
	if (foreign_previous.sa_flags & SA_SIGINFO)
		foreign_previous.sa_sigaction(signal_number, information, context);
}

static void foreign_install(void (*handler)(int, siginfo_t *, void *))
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = handler;
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, &foreign_previous);
}

static int foreign_installed(void)
{
	struct sigaction current;

	sigaction(SIGSEGV, NULL, &current);
	return (current.sa_flags & SA_SIGINFO) &&
		(current.sa_sigaction == foreign_jumping || current.sa_sigaction == foreign_chaining);
}

static int watched_write(unsigned long offset)
{
	unsigned long before;

	memory_watch_protect(test_window + offset, 0x1000);
	before = memory_watch_generation(test_window + offset, 0x1000);
	*(volatile int *)(test_window + offset) = 7;
	return memory_watch_generation(test_window + offset, 0x1000) > before &&
		*(volatile int *)(test_window + offset) == 7;
}

static void *worker_recovery(void *unused)
{
	(void)unused;
	if (!sigsetjmp(foreign_jump, 1))
		*(volatile int *)0x10 = 1;
	pthread_barrier_wait(&worker_barrier);
	pthread_barrier_wait(&worker_barrier);
	if (!sigsetjmp(foreign_jump, 1))
		*(volatile int *)0x10 = 1;
	return NULL;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	void *window = mmap(NULL, PLATFORM_CONTIGUOUS_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (window == MAP_FAILED)
		return 100;
	test_window = (unsigned long)window;
	setvbuf(stdout, NULL, _IONBF, 0);
	if (!strcmp(mode, "reinstall"))
	{
		/* the constructor's handler, then a library's over it, then the
		device's initialize */
		foreign_install(foreign_jumping);
		memory_watch_initialize();
		printf("foreign_installed=%d\n", foreign_installed());
		printf("watched_write=%d foreign_calls=%d\n", watched_write(0), foreign_calls);
		/* a fault of nobody's watch: reported, and handed to the library's */
		if (!sigsetjmp(foreign_jump, 1))
			*(volatile int *)0x10 = 1;
		printf("unrelated foreign_calls=%d debug_lines=%d\n", foreign_calls, debug_lines > 0);
		/* (the library jumped out: the next initialize, or frame, puts
		this file's back) */
		memory_watch_begin_frame();
		printf("after_frame foreign_installed=%d watched_write=%d foreign_calls=%d\n", foreign_installed(),
			watched_write(0x2000), foreign_calls);
		return 0;
	}
	if (!strcmp(mode, "without"))
	{
		/* the library's over this file's and no initialize after: the
		watch's fault goes to the library (what the device's call fixes) */
		memory_watch_protect(test_window, 0x1000);
		foreign_install(foreign_jumping);
		if (!sigsetjmp(foreign_jump, 1))
			*(volatile int *)test_window = 1;
		printf("foreign_calls=%d\n", foreign_calls);
		return 0;
	}
	if (!strcmp(mode, "loop"))
	{
		/* a library's handler that chains back to this file's, which chains
		to it: the default action ends it, once reported */
		foreign_install(foreign_chaining);
		memory_watch_initialize();
		*(volatile int *)0x10 = 1;
		return 0;
	}
	if (!strcmp(mode, "segv"))
		*(volatile int *)0x10 = 1;
	else if (!strcmp(mode, "abort"))
		abort();
	else if (!strcmp(mode, "bus"))
		raise(SIGBUS);
	else if (!strcmp(mode, "fpe"))
		raise(SIGFPE);
	else if (!strcmp(mode, "ill"))
		raise(SIGILL);
	else if (!strcmp(mode, "ignored"))
	{
		/* a handler of the program's own before this file's (here: none
		but SIGFPE ignored), kept as what it chains to */
		signal(SIGFPE, SIG_IGN);
		memory_watch_initialize();
		raise(SIGFPE);
		printf("survived\n");
		return 0;
	}
	else if (!strcmp(mode, "ignored_siginfo"))
	{
		install_handler(SIGFPE, SIG_IGN, SA_SIGINFO, 0);
		memory_watch_initialize();
		raise(SIGFPE);
		printf("survived\n");
		return 0;
	}
	else if (!strcmp(mode, "ignored_siginfo_segv"))
	{
		install_handler(SIGSEGV, SIG_IGN, SA_SIGINFO, 0);
		memory_watch_initialize();
		raise(SIGSEGV);
		printf("survived\n");
		return 0;
	}
	else if (!strcmp(mode, "default_siginfo"))
	{
		install_handler(SIGBUS, SIG_DFL, SA_SIGINFO, 0);
		memory_watch_initialize();
		raise(SIGBUS);
		return 102;
	}
	else if (!strcmp(mode, "default_siginfo_segv"))
	{
		install_handler(SIGSEGV, SIG_DFL, SA_SIGINFO, 0);
		memory_watch_initialize();
		raise(SIGSEGV);
		return 102;
	}
	else if (!strcmp(mode, "mask") || !strcmp(mode, "mask_segv"))
	{
		int number = !strcmp(mode, "mask_segv") ? SIGSEGV : SIGFPE;
		install_handler(number, mask_handler, 0, SIGUSR1);
		memory_watch_initialize();
		raise(number);
		printf("mask=%d self=%d\n", mask_seen, self_blocked);
		return 0;
	}
	else if (!strcmp(mode, "one_shot") || !strcmp(mode, "one_shot_segv"))
	{
		int number = !strcmp(mode, "one_shot_segv") ? SIGSEGV : SIGFPE;
		install_handler(number, once_handler, SA_RESETHAND, 0);
		memory_watch_initialize();
		raise(number);
		printf("first=%d\n", once_calls);
		raise(number);
		return 103;
	}
	else if (!strcmp(mode, "nested_signals"))
	{
		install_handler(SIGBUS, bus_handler, 0, 0);
		install_handler(SIGFPE, fpe_handler_raising_bus, 0, 0);
		memory_watch_initialize();
		raise(SIGFPE);
		printf("fpe=%d bus=%d\n", fpe_calls, bus_calls);
		return 0;
	}
	else if (!strcmp(mode, "alternate_stack") || !strcmp(mode, "alternate_stack_segv"))
	{
		int number = !strcmp(mode, "alternate_stack_segv") ? SIGSEGV : SIGFPE;
		stack_t stack = {.ss_sp = alternate_stack, .ss_size = sizeof(alternate_stack)};
		if (sigaltstack(&stack, NULL) != 0)
			return 104;
		install_handler(number, alt_stack_handler, SA_ONSTACK, 0);
		memory_watch_initialize();
		raise(number);
		printf("alternate=%d\n", on_alt_stack);
		return 0;
	}
	else if (!strcmp(mode, "worker_recovery"))
	{
		pthread_t worker;
		foreign_install(foreign_jumping);
		memory_watch_initialize();
		if (pthread_barrier_init(&worker_barrier, NULL, 2) != 0 ||
			pthread_create(&worker, NULL, worker_recovery, NULL) != 0)
			return 107;
		pthread_barrier_wait(&worker_barrier);
		memory_watch_begin_frame();
		pthread_barrier_wait(&worker_barrier);
		pthread_join(worker, NULL);
		printf("worker_foreign_calls=%d\n", foreign_calls);
		return 0;
	}
	else if (!strcmp(mode, "report_fault"))
	{
		fault_in_write = 1;
		raise(SIGFPE);
		return 105;
	}
	return 0;
}
"""


@pytest.fixture(scope="module")
def program(tmp_path_factory):
    compiler = os.environ.get("CC", "clang")
    if not shutil.which(compiler):
        pytest.skip(f"no {compiler}")
    work = tmp_path_factory.mktemp("memory_watch")
    # (memory_watch.c's "platform.h" is found beside it first: the source is
    # compiled from a folder that has the test's)
    (work / "platform.h").write_text(PLATFORM_H)
    (work / "memory_watch.c").write_text(SOURCE.read_text())
    (work / "main.c").write_text(MAIN)
    executable = work / "watch"
    subprocess.run([compiler, "-std=gnu11", "-O1", "-g", "-Wall", "-Werror", "-Wno-unused-result", "-D_GNU_SOURCE", "-pthread", "-I", str(work),
                    str(work / "main.c"), str(work / "memory_watch.c"), "-o", str(executable)], check=True)
    return executable


def run(program, mode):
    return subprocess.run([str(program), mode], capture_output=True, text=True, timeout=20)


def test_initialize_puts_the_handler_back_over_a_library(program):
    result = run(program, "reinstall")
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    # this file's handler again, the watch's write noted and let through,
    # and the library's handler not called for it
    assert lines[0] == "foreign_installed=0"
    assert lines[1] == "watched_write=1 foreign_calls=0"
    # a fault outside the watch: reported, then the library's
    assert lines[2] == "unrelated foreign_calls=1 debug_lines=0"
    assert "halo-linux: segmentation fault" in result.stderr
    assert lines[3] == "after_frame foreign_installed=0 watched_write=1 foreign_calls=1"


def test_without_initialize_a_library_takes_the_watch(program):
    # what the device's initialize prevents
    result = run(program, "without")
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "foreign_calls=1"


def test_a_handler_chaining_back_does_not_loop(program):
    result = run(program, "loop")
    assert result.returncode == -signal.SIGSEGV
    assert result.stderr.count("halo-linux: segmentation fault") == 1


@pytest.mark.parametrize("mode, number, name", [
    ("segv", signal.SIGSEGV, "segmentation fault"),
    ("abort", signal.SIGABRT, "abort"),
    ("bus", signal.SIGBUS, "bus error"),
    ("fpe", signal.SIGFPE, "arithmetic exception"),
    ("ill", signal.SIGILL, "illegal instruction"),
])
def test_crash_reported_then_default(program, mode, number, name):
    result = run(program, mode)
    assert result.returncode == -number
    assert result.stderr.count(f"halo-linux: {name} (signal {int(number)},") == 1
    assert "halo-linux: called from" not in result.stderr  # (debug.txt's lines, not the terminal's)


def test_an_ignored_signal_stays_ignored(program):
    result = run(program, "ignored")
    assert result.returncode == 0
    assert result.stdout.strip() == "survived"


def test_siginfo_ignore_and_default_dispositions(program):
    for mode in ("ignored_siginfo", "ignored_siginfo_segv"):
        ignored = run(program, mode)
        assert ignored.returncode == 0, ignored.stderr
        assert ignored.stdout.strip() == "survived"
        assert "halo-linux:" not in ignored.stderr
    for mode, number, name in (("default_siginfo", signal.SIGBUS, "bus error"),
                               ("default_siginfo_segv", signal.SIGSEGV, "segmentation fault")):
        default = run(program, mode)
        assert default.returncode == -number, default.stderr
        assert default.stderr.count(f"halo-linux: {name}") == 1


def test_prior_mask_and_self_blocking_apply(program):
    for mode in ("mask", "mask_segv"):
        result = run(program, mode)
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "mask=1 self=1"
        assert result.stderr.count("halo-linux: segmentation fault") == int(mode.endswith("segv"))


def test_prior_one_shot_handler_resets(program):
    for mode, number in (("one_shot", signal.SIGFPE), ("one_shot_segv", signal.SIGSEGV)):
        result = run(program, mode)
        assert result.returncode == -number, result.stderr
        assert result.stdout.strip() == "first=1"
        assert result.stderr.count("halo-linux: segmentation fault") == int(mode.endswith("segv"))


def test_nested_different_signals_keep_their_handlers(program):
    result = run(program, "nested_signals")
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "fpe=1 bus=1"
    assert "halo-linux:" not in result.stderr


def test_prior_alternate_stack_is_used(program):
    for mode in ("alternate_stack", "alternate_stack_segv"):
        result = run(program, mode)
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "alternate=1"
        assert result.stderr.count("halo-linux: segmentation fault") == int(mode.endswith("segv"))


def test_worker_recovery_after_foreign_longjmp(program):
    result = run(program, "worker_recovery")
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "worker_foreign_calls=2"
    assert result.stderr.count("halo-linux: segmentation fault") == 2


def test_a_fault_in_the_reporter_defaults_without_reporting_again(program):
    result = run(program, "report_fault")
    assert result.returncode == -signal.SIGSEGV, result.stderr
    assert "halo-linux: segmentation fault" not in result.stderr
