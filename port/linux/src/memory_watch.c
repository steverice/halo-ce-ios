/*
MEMORY_WATCH.C

Write tracking for guest memory that the renderer caches.

Textures live in the Xbox contiguous window, where the game (or its
streaming threads) can rewrite them at any time. Instead of hashing their
contents every frame, the pages behind a cached texture are made read-only;
the first write faults, the handler records a new generation for the page,
makes it writable again and lets the write proceed. A cache entry is stale
when any of its pages has a generation newer than the entry.

Writes that the kernel performs on the game's behalf (read() into a
buffer) would fail with EFAULT instead of faulting, so the file layer reads
guest memory through a bounce buffer (xbox_files.c). Unprotecting ahead of
such a write (memory_watch_prepare_write) is not enough on its own: the
renderer can protect the pages again before the kernel writes them.

The handler writes a bounded signal, address and register report to stderr.
It reports SIGSEGV and the default dispositions of SIGBUS, SIGILL, SIGFPE
and SIGABRT, leaving custom and ignored dispositions for those four alone.
It starts before main; a library that installs its own SIGSEGV handler
later would take watched-page faults, so the renderer reinstalls the
watcher above it before the next frame.
*/

#include "platform.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>

#define WATCH_PAGE_SIZE 0x1000UL
#define WATCH_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / WATCH_PAGE_SIZE)

static unsigned char page_protected[WATCH_PAGE_COUNT];
static unsigned long page_generation[WATCH_PAGE_COUNT];
static volatile unsigned long current_generation = 1;
static BOOL watch_active = FALSE;

/* the signals reported as a crash, SIGSEGV first (the write tracking's), and
what handled each before this file's handler */
static const int crash_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
#define CRASH_SIGNAL_COUNT (int)(sizeof(crash_signals) / sizeof(crash_signals[0]))
/* Old SIGSEGV dispositions stay allocated: another thread may still read a
snapshot after this thread reinstalls the watcher. The pointer is published
with a lock-free atomic store before the new handler is installed. */
static struct sigaction *previous_segv_action;
static pthread_mutex_t handler_install_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread volatile sig_atomic_t chaining_segv __attribute__((tls_model("initial-exec")));
static __thread volatile sig_atomic_t reporting_crash __attribute__((tls_model("initial-exec")));
_Static_assert(__atomic_always_lock_free(sizeof(previous_segv_action), 0), "signal action pointer must be lock-free");

static unsigned long page_index(unsigned long address)
{
	return (address - PLATFORM_CONTIGUOUS_BASE) / WATCH_PAGE_SIZE;
}

static void mark_written(unsigned long page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
	mprotect((void *)(PLATFORM_CONTIGUOUS_BASE + page * WATCH_PAGE_SIZE), WATCH_PAGE_SIZE, PROT_READ | PROT_WRITE);
}

/* The crash path only formats into its stack buffer and writes to stderr.
The game logger and backtrace can deadlock in a signal from their own code. */
static void report_character(char *buffer, size_t capacity, size_t *length, char value)
{
	if (*length < capacity)
		buffer[(*length)++] = value;
}

static void report_text(char *buffer, size_t capacity, size_t *length, const char *value)
{
	while (*value)
		report_character(buffer, capacity, length, *value++);
}

static void report_number(char *buffer, size_t capacity, size_t *length, long value)
{
	char digits[24];
	unsigned long number;
	int count = 0;

	if (value < 0)
	{
		report_character(buffer, capacity, length, '-');
		number = (unsigned long)(-(value + 1)) + 1;
	}
	else
		number = (unsigned long)value;
	do {
		digits[count++] = (char)('0' + number % 10);
		number /= 10;
	} while (number && count < (int)sizeof(digits));
	while (count)
		report_character(buffer, capacity, length, digits[--count]);
}

static void report_hex(char *buffer, size_t capacity, size_t *length, unsigned long value)
{
	static const char digits[] = "0123456789abcdef";
	char reversed[2 * sizeof(value)];
	int count = 0;

	report_text(buffer, capacity, length, "0x");
	do {
		reversed[count++] = digits[value & 15];
		value >>= 4;
	} while (value && count < (int)sizeof(reversed));
	while (count)
		report_character(buffer, capacity, length, reversed[--count]);
}

static const char *crash_signal_name(int signal_number)
{
	switch (signal_number)
	{
	case SIGSEGV:
		return "segmentation fault";
	case SIGBUS:
		return "bus error";
	case SIGILL:
		return "illegal instruction";
	case SIGFPE:
		return "arithmetic exception";
	case SIGABRT:
		return "abort";
	}
	return "signal";
}

static int crash_signal_index(int signal_number)
{
	int index;

	for (index = 0; index < CRASH_SIGNAL_COUNT; index++)
	{
		if (crash_signals[index] == signal_number)
			return index;
	}
	return -1;
}

/* A bounded, async-signal-safe report. The caller's log may hold a lock or
be the reason for SIGABRT, so stderr is the only destination here. */
static void crash_report(int signal_number, siginfo_t *information, void *context)
{
	ucontext_t *ucontext = context;
	char line[256];
	unsigned long pc = 0, frame = 0, stack = 0;
	size_t length = 0;

#if defined(__i386__)
	if (ucontext)
	{
		pc = (unsigned long)ucontext->uc_mcontext.gregs[REG_EIP];
		frame = (unsigned long)ucontext->uc_mcontext.gregs[REG_EBP];
		stack = (unsigned long)ucontext->uc_mcontext.gregs[REG_ESP];
	}
#elif defined(__x86_64__)
	if (ucontext)
	{
		pc = (unsigned long)ucontext->uc_mcontext.gregs[REG_RIP];
		frame = (unsigned long)ucontext->uc_mcontext.gregs[REG_RBP];
		stack = (unsigned long)ucontext->uc_mcontext.gregs[REG_RSP];
	}
#else
	(void)ucontext;
#endif
	report_text(line, sizeof(line), &length, "halo-linux: ");
	report_text(line, sizeof(line), &length, crash_signal_name(signal_number));
	report_text(line, sizeof(line), &length, " (signal ");
	report_number(line, sizeof(line), &length, signal_number);
	report_text(line, sizeof(line), &length, ", code ");
	report_number(line, sizeof(line), &length, information ? information->si_code : 0);
	report_text(line, sizeof(line), &length, ") at ");
	report_hex(line, sizeof(line), &length,
		information && information->si_code > 0 ? (unsigned long)information->si_addr : 0);
	report_text(line, sizeof(line), &length, " pc ");
	report_hex(line, sizeof(line), &length, pc);
	report_text(line, sizeof(line), &length, " frame ");
	report_hex(line, sizeof(line), &length, frame);
	report_text(line, sizeof(line), &length, " stack ");
	report_hex(line, sizeof(line), &length, stack);
	report_character(line, sizeof(line), &length, '\n');
	(void)write(STDERR_FILENO, line, length);
}

static BOOL crash_handler_installed(int signal_number, struct sigaction *current);

/* The kernel invokes the restored disposition: its mask, one-shot and
alternate-stack behavior are not reproduced by calling a function pointer. */
static void crash_default(int signal_number, siginfo_t *information)
{
	static const struct sigaction action = { .sa_handler = SIG_DFL };

	(void)sigaction(signal_number, &action, NULL);
	if (!information || information->si_code <= 0)
		(void)raise(signal_number);
	/* A synchronous fault retries its instruction under SIG_DFL. */
}

static void crash_handler(int signal_number, siginfo_t *information, void *context)
{
	int index = crash_signal_index(signal_number);
	BOOL sent = !information || information->si_code <= 0;
	const struct sigaction *previous;

	if (signal_number == SIGSEGV && information && platform_is_contiguous(information->si_addr))
	{
		unsigned long page = page_index((unsigned long)information->si_addr);

		/* a write to a page the renderer watches: noted, and let through */
		if (page < WATCH_PAGE_COUNT && page_protected[page])
		{
			mark_written(page);
			return;
		}
	}
	if (index < 0)
		return;
	if (reporting_crash)
	{
		crash_default(signal_number, information);
		return;
	}
	if (signal_number == SIGSEGV)
	{
		struct sigaction current;

		previous = __atomic_load_n(&previous_segv_action, __ATOMIC_ACQUIRE);
		/* A foreign handler that jumped away left this thread's guard set.
		Once the next frame reinstalls us, this is a new signal. A foreign
		handler directly calling us while it remains installed is recursion. */
		if (chaining_segv)
		{
			if (crash_handler_installed(SIGSEGV, &current))
				chaining_segv = 0;
			else
			{
				crash_default(signal_number, information);
				return;
			}
		}
		if (sent && previous && previous->sa_handler == SIG_IGN)
			return;
		chaining_segv = 1;
		reporting_crash = 1;
		crash_report(signal_number, information, context);
		reporting_crash = 0;
		/* SIG_IGN on a synchronous fault cannot let the faulting instruction
		make progress. Use the default disposition instead of retrying forever. */
		if (!previous || (!sent && previous->sa_handler == SIG_IGN) ||
			sigaction(signal_number, previous, NULL) != 0)
			crash_default(signal_number, information);
		else if (sent)
		{
			(void)raise(signal_number);
			chaining_segv = 0;
		}
		return;
	}
	/* These four diagnostic-only signals are intercepted only while their
	prior disposition is default. Custom and ignored dispositions are left
	installed by crash_handlers_install. */
	reporting_crash = 1;
	crash_report(signal_number, information, context);
	reporting_crash = 0;
	crash_default(signal_number, information);
}

static BOOL crash_handler_installed(int signal_number, struct sigaction *current)
{
	return sigaction(signal_number, NULL, current) == 0 && (current->sa_flags & SA_SIGINFO) &&
		current->sa_sigaction == crash_handler;
}

/* The renderer calls this on one thread. The mutex serializes our own
installers; another library registering an action concurrently remains a
process-wide last-writer race, which sigaction does not make atomic with our
query. The next frame checks again. */
static BOOL crash_handlers_install(void)
{
	struct sigaction action, current;
	BOOL segv_ready = FALSE;
	int index;

	if (pthread_mutex_lock(&handler_install_lock) != 0)
		return FALSE;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = crash_handler;
	sigemptyset(&action.sa_mask);
	for (index = 0; index < CRASH_SIGNAL_COUNT; index++)
	{
		struct sigaction *snapshot;

		if (sigaction(crash_signals[index], NULL, &current) != 0)
			continue;
		if ((current.sa_flags & SA_SIGINFO) && current.sa_sigaction == crash_handler)
		{
			if (index == 0)
				segv_ready = TRUE;
			continue;
		}
		/* Only SIGSEGV needs to remain above a foreign handler for watched
		page faults. The other signals are diagnostic-only, so leave foreign
		and ignored dispositions alone. */
		if (index != 0 && current.sa_handler != SIG_DFL)
			continue;
		if (index == 0)
		{
			snapshot = malloc(sizeof(*snapshot));
			if (!snapshot)
				continue;
			*snapshot = current;
			/* Old snapshots are retained for handlers already running on
			another thread. The pointer store itself is lock-free. */
			__atomic_store_n(&previous_segv_action, snapshot, __ATOMIC_RELEASE);
		}
		action.sa_flags = SA_SIGINFO | (index == 0 ? SA_NODEFER : 0);
		if (sigaction(crash_signals[index], &action, NULL) == 0 && index == 0)
		{
			chaining_segv = 0;
			segv_ready = TRUE;
		}
	}
	if (sigaction(SIGSEGV, NULL, &current) != 0 ||
		!((current.sa_flags & SA_SIGINFO) && current.sa_sigaction == crash_handler))
		segv_ready = FALSE;
	pthread_mutex_unlock(&handler_install_lock);
	return segv_ready;
}

/* at the renderer's device (d3d8_device.c), before any page is protected; and
from the start (below). A library that installed a SIGSEGV handler in
between (the GL driver's, as the window opened) would get the protected
pages' faults, and crash or hang at the first write to a texture: the
handler goes in again, chained to that one */
void memory_watch_initialize(void)
{
	watch_active = crash_handlers_install();
}

/* the handler from the start, for its crash report: the renderer's device
(d3d8_device.c) installs it too, but a game with debug.null_renderer (the
network tests, net_compare) creates none, and a crash before the device
went unreported */
__attribute__((constructor))
static void memory_watch_install(void)
{
	memory_watch_initialize();
}

void memory_watch_protect(unsigned long address, unsigned long size)
{
	unsigned long first, last, page;

	if (!watch_active || !size || !platform_is_contiguous((void *)address))
		return;
	first = page_index(address);
	last = page_index(address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (!page_protected[page])
		{
			page_protected[page] = 1;
			mprotect((void *)(PLATFORM_CONTIGUOUS_BASE + page * WATCH_PAGE_SIZE), WATCH_PAGE_SIZE, PROT_READ);
		}
	}
}

unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
	unsigned long first, last, page, newest = 0;

	if (!size || !platform_is_contiguous((void *)address))
		return 0;
	first = page_index(address);
	last = page_index(address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

unsigned long memory_watch_serial(void)
{
	return current_generation;
}

void memory_watch_prepare_write(void *address, unsigned long size)
{
	unsigned long start = (unsigned long)address;
	unsigned long first, last, page;

	if (!watch_active || !size)
		return;
	if (start + size <= PLATFORM_CONTIGUOUS_BASE || start >= PLATFORM_CONTIGUOUS_BASE + PLATFORM_CONTIGUOUS_SIZE)
		return;
	if (start < PLATFORM_CONTIGUOUS_BASE)
		start = PLATFORM_CONTIGUOUS_BASE;
	first = page_index(start);
	last = page_index((unsigned long)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

void memory_watch_forget(void *address, unsigned long size)
{
	unsigned long start = (unsigned long)address;
	unsigned long first, last, page;

	if (!size || !platform_is_contiguous(address))
		return;
	first = page_index(start);
	last = page_index(start + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}

/* page protection sees each write at once; once a frame, the handler is
checked to be the one installed (a library may put its own in at any time:
a sound device opened, a gamepad's) */
void memory_watch_begin_frame(void)
{
	if (watch_active)
		crash_handlers_install();
}
