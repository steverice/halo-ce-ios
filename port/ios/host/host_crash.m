/*
HOST_CRASH.M

Fatal errors as native crashes (host_crash.h).

Guest code runs from an alias of the app's __TEXT,__halocode in the guest
arena (host_loader.c), so Apple's crash reports show its frames as bare
addresses outside every image. The arena is 4 GB aligned, so an address's
low 32 bits are its guest address: the report's reason here names the guest
frames from the symbol table embedded beside the code (__TEXT,__halosyms,
tools/ios_embed_guest.py), and tools/guest_symbolicate.py names them in a
whole report from the build's guest-symbols.txt.

Two ways out, both native crashes:
- host_crash and host_guest_halt raise an NSException with the message and
  the top guest frames as its reason. It is never caught (the unwinder stops
  at the first guest frame, which has no unwind information, and the Mac
  apps crash on exceptions: NSApplicationCrashOnExceptions), so the system
  ends the app with SIGABRT and the report carries the reason in its
  Application Specific Information and the stack as its Last Exception
  Backtrace.
- A fault (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT) the memory
  watch does not own reaches crash_signal, which logs the registers and the
  frames with only async-signal-safe calls, puts a one-line summary in this
  image's crash annotation (__DATA,__crash_info, which the crash reporter
  copies into Application Specific Information as it does the Swift
  runtime's and the C library's messages), and lets the fault happen again
  under the default action, so the report shows the original exception.
*/

#import <Foundation/Foundation.h>
#include "ios_host.h"
#include "host_config.h"
#include "host_crash.h"
#include "guest_image.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#define MAXIMUM_FRAMES 64
/* guest frames named in an exception's reason: TestFlight and the Organizer show the reason in full, but
   keep it to what reads at a glance; the log and the report's backtrace have every frame */
#define REASON_FRAMES 8

/* ---------- the crash annotation (the layout's version 5, as the C library's own) */

struct crash_annotation
{
	uint64_t version;
	uint64_t message;
	uint64_t signature;
	uint64_t backtrace;
	uint64_t message2;
	uint64_t thread;
	uint64_t dialog_mode;
	uint64_t abort_cause;
};

__attribute__((used, section("__DATA,__crash_info")))
static struct crash_annotation crash_annotation = { 5, 0, 0, 0, 0, 0, 0, 0 };

/* ---------- the guest's symbols */

struct guest_symbol
{
	uint32_t address;
	uint32_t name;
};

static const struct guest_symbol *symbols;
static uint32_t symbol_count;
static const char *symbol_names;

static void symbols_load(void)
{
	/* tools/ios_embed_guest.py's symbol_table: the magic, the count, the names' size, the code's range and
	   the image's sha256, then the pairs and the names */
	const unsigned char *table = halo_guest_symbols;
	uint32_t count;

	if (memcmp(table, "VGSYM1\0\0", 8))
		return;
	memcpy(&count, table + 8, 4);
	symbols = (const struct guest_symbol *)(table + 88);
	symbol_names = (const char *)(symbols + count);
	symbol_count = count;
}

/* the guest function holding address, or NULL; *offset is address's offset into it */
static const char *guest_symbol(uint32_t address, uint32_t *offset)
{
	uint32_t low = 0, high = symbol_count;

	while (low < high)
	{
		uint32_t middle = low + (high - low) / 2;

		if (symbols[middle].address <= address)
			low = middle + 1;
		else
			high = middle;
	}
	if (!low)
		return NULL;
	*offset = address - symbols[low - 1].address;
	return symbol_names + symbols[low - 1].name;
}

static int guest_code_contains(uintptr_t address)
{
	uintptr_t start = host_arena + HALO_GUEST_IMAGE_BASE;

	return host_arena && address >= start && address < start + IOS_GUEST_CODE_SIZE;
}

/* ---------- stacks */

/* reads a frame record (the caller's frame pointer and the return address) without faulting */
static int read_frame(uintptr_t frame, uintptr_t record[2])
{
	vm_size_t got = 0;

	return frame && !(frame & 7) &&
		vm_read_overwrite(mach_task_self(), frame, sizeof(uintptr_t) * 2, (vm_address_t)record, &got) == KERN_SUCCESS &&
		got == sizeof(uintptr_t) * 2;
}

/* pc (when not 0), then the return addresses of the frame records from frame on. Guest frames keep
   standard 64-bit records on their arena stack, and the first guest frame's record leads back to the
   host frame that entered the guest (guest_call.S), so one walk crosses both */
static int stack_frames(uintptr_t pc, uintptr_t frame, uintptr_t *frames, int capacity)
{
	int count = 0;
	uintptr_t record[2];

	if (pc && count < capacity)
		frames[count++] = pc;
	while (count < capacity && read_frame(frame, record) && record[1])
	{
		frames[count++] = record[1];
		if (record[0] == frame)
			break;
		frame = record[0];
	}
	return count;
}

/* ---------- async-signal-safe text */

struct text
{
	char *buffer;
	size_t size, length;
};

static void text_add(struct text *text, const char *string)
{
	while (*string && text->length + 1 < text->size)
		text->buffer[text->length++] = *string++;
	text->buffer[text->length] = 0;
}

static void text_hex(struct text *text, uint64_t value, int digits)
{
	char digit[17];
	int count = 0;

	do
	{
		digit[count++] = "0123456789abcdef"[value & 15];
		value >>= 4;
	} while ((value || count < digits) && count < 16);
	text_add(text, "0x");
	while (count--)
	{
		char one[2] = { digit[count], 0 };

		text_add(text, one);
	}
}

static void text_decimal(struct text *text, long value)
{
	char digit[24];
	int count = 0;
	unsigned long magnitude = value < 0 ? (unsigned long)-value : (unsigned long)value;

	do
	{
		digit[count++] = (char)('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude);
	if (value < 0)
		text_add(text, "-");
	while (count--)
	{
		char one[2] = { digit[count], 0 };

		text_add(text, one);
	}
}

/* "guest 0x88123456 name+0x1c", or nothing for an address outside guest code */
static void text_guest(struct text *text, uintptr_t address)
{
	uint32_t offset = 0;
	const char *name;

	if (!guest_code_contains(address))
		return;
	text_add(text, " guest ");
	text_hex(text, (uint32_t)address, 8);
	name = guest_symbol((uint32_t)address, &offset);
	if (name)
	{
		text_add(text, " ");
		text_add(text, name);
		text_add(text, "+");
		text_hex(text, offset, 1);
	}
}

/* "name+0x1c < name+0x40 < ..." for the guest frames among frames */
static void text_guest_frames(struct text *text, const uintptr_t *frames, int count, int limit)
{
	int index, shown = 0;

	for (index = 0; index < count && shown < limit; index++)
	{
		uint32_t offset = 0;
		const char *name;

		if (!guest_code_contains(frames[index]))
			continue;
		if (shown++)
			text_add(text, " < ");
		name = guest_symbol((uint32_t)frames[index], &offset);
		if (name)
		{
			text_add(text, name);
			text_add(text, "+");
			text_hex(text, offset, 1);
		}
		else
			text_hex(text, (uint32_t)frames[index], 8);
	}
}

/* ---------- the log */

static int log_descriptor = -1;

static void log_write(const char *string, size_t length)
{
	if (log_descriptor >= 0)
		write(log_descriptor, string, length);
	write(2, "halo-ios: ", 10);
	write(2, string, length);
}

/* ---------- faults */

static const int crash_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT };
#define CRASH_SIGNAL_COUNT ((int)(sizeof(crash_signals) / sizeof(crash_signals[0])))
static atomic_int crashing;
/* set once host_crash has logged and raised: the SIGABRT that follows is its */
static atomic_int raised;

/* the guest image's sha256, shortened as in a reason: which build's guest-symbols.txt names its frames */
static char guest_image_short[13];

/* a signal sent (abort's, raise's, kill's) rather than raised by an instruction */
static int signal_sent(int number, const siginfo_t *info)
{
	return number == SIGABRT || info->si_code == 0 || info->si_code >= SI_USER;
}

static const char *signal_name(int number)
{
	switch (number)
	{
	case SIGSEGV: return "SIGSEGV";
	case SIGBUS: return "SIGBUS";
	case SIGILL: return "SIGILL";
	case SIGFPE: return "SIGFPE";
	case SIGTRAP: return "SIGTRAP";
	case SIGABRT: return "SIGABRT";
	default: return "signal";
	}
}

static void crash_signal(int number, siginfo_t *info, void *context)
{
	static char summary[1024], line[512];
	struct text text = { summary, sizeof(summary), 0 };
	ucontext_t *ucontext = context;
	uintptr_t pc = 0, lr = 0, fp = 0, sp = 0, frames[MAXIMUM_FRAMES];
	struct sigaction action = { 0 };
	int count, index;

	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	sigaction(number, &action, NULL);
	/* a second fault while reporting, or the abort that ends host_crash's exception: already reported */
	if (atomic_exchange(&crashing, 1) || (number == SIGABRT && atomic_load(&raised)))
	{
		if (signal_sent(number, info))
			raise(number);
		return;
	}
	if (ucontext && ucontext->uc_mcontext)
	{
		pc = (uintptr_t)arm_thread_state64_get_pc(ucontext->uc_mcontext->__ss);
		lr = (uintptr_t)arm_thread_state64_get_lr(ucontext->uc_mcontext->__ss);
		fp = (uintptr_t)arm_thread_state64_get_fp(ucontext->uc_mcontext->__ss);
		sp = (uintptr_t)arm_thread_state64_get_sp(ucontext->uc_mcontext->__ss);
	}
	count = stack_frames(pc, fp, frames, MAXIMUM_FRAMES);

	text_add(&text, guest_code_contains(pc) ? "guest fault " : "fault ");
	text_add(&text, signal_name(number));
	text_add(&text, " (code ");
	text_decimal(&text, info->si_code);
	text_add(&text, ") at ");
	text_hex(&text, (uintptr_t)info->si_addr, 1);
	if (host_low_owns((uintptr_t)info->si_addr, 1))
	{
		text_add(&text, " (guest ");
		text_hex(&text, (uint32_t)(uintptr_t)info->si_addr, 8);
		text_add(&text, ")");
	}
	text_add(&text, ", pc");
	if (!guest_code_contains(pc))
	{
		text_add(&text, " ");
		text_hex(&text, pc, 1);
	}
	text_guest(&text, pc);
	if (guest_code_contains(pc) || guest_code_contains(lr))
	{
		text_add(&text, " [guest: ");
		text_guest_frames(&text, frames, count, REASON_FRAMES);
		text_add(&text, "]");
	}
	text_add(&text, " (guest image ");
	text_add(&text, guest_image_short);
	text_add(&text, ")");
	crash_annotation.message = (uint64_t)(uintptr_t)summary;

	log_write("CRASH: ", 7);
	log_write(summary, text.length);
	log_write("\n", 1);
	{
		struct text registers = { line, sizeof(line), 0 };

		text_add(&registers, "CRASH: arena ");
		text_hex(&registers, host_arena, 16);
		text_add(&registers, ", lr ");
		text_hex(&registers, lr, 16);
		text_guest(&registers, lr);
		text_add(&registers, ", fp ");
		text_hex(&registers, fp, 16);
		text_add(&registers, ", sp ");
		text_hex(&registers, sp, 16);
		text_add(&registers, "\n");
		log_write(line, registers.length);
	}
	if (ucontext && ucontext->uc_mcontext)
	{
		for (index = 0; index < 29; index += 4)
		{
			struct text registers = { line, sizeof(line), 0 };
			int each;

			text_add(&registers, "CRASH:");
			for (each = index; each < index + 4 && each < 29; each++)
			{
				text_add(&registers, " x");
				text_decimal(&registers, each);
				text_add(&registers, " ");
				text_hex(&registers, ucontext->uc_mcontext->__ss.__x[each], 16);
			}
			text_add(&registers, "\n");
			log_write(line, registers.length);
		}
	}
	for (index = 0; index < count; index++)
	{
		struct text frame = { line, sizeof(line), 0 };

		text_add(&frame, "CRASH: frame ");
		text_decimal(&frame, index);
		text_add(&frame, ": ");
		text_hex(&frame, frames[index], 16);
		text_guest(&frame, frames[index]);
		text_add(&frame, "\n");
		log_write(line, frame.length);
	}
	/* a hardware fault happens again when this returns, now under the default action, and the report
	   shows it as it was; a sent signal (abort's, raise's) is sent again */
	if (signal_sent(number, info))
		raise(number);
}

void host_crash_install(int descriptor)
{
	static char alternate[64 * 1024];
	stack_t stack = { .ss_sp = alternate, .ss_size = sizeof(alternate), .ss_flags = 0 };
	struct sigaction action = { 0 };
	int index;

	log_descriptor = descriptor;
	snprintf(guest_image_short, sizeof(guest_image_short), "%s", HALO_GUEST_SHA256);
	symbols_load();
	/* the game's thread (the main thread) keeps a stack to report on when its own runs out */
	sigaltstack(&stack, NULL);
	action.sa_sigaction = crash_signal;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	for (index = 0; index < CRASH_SIGNAL_COUNT; index++)
		sigaction(crash_signals[index], &action, NULL);
#if TARGET_OS_OSX || TARGET_OS_MACCATALYST
	/* AppKit otherwise logs an exception that reaches its event handling and carries on */
	[NSUserDefaults.standardUserDefaults registerDefaults:@{ @"NSApplicationCrashOnExceptions": @YES }];
#endif
	host_logf(HOST_LOG_INFO, "crash reports: %u guest symbols", symbol_count);
}

/* ---------- fatal errors */

static void log_frames(const uintptr_t *frames, int count)
{
	int index;

	for (index = 0; index < count; index++)
	{
		char guest[256];
		struct text text = { guest, sizeof(guest), 0 };
		Dl_info found;

		text_guest(&text, frames[index]);
		if (!text.length && dladdr((const void *)frames[index], &found) && found.dli_sname)
			snprintf(guest, sizeof(guest), " %s+0x%lx", found.dli_sname,
				(unsigned long)(frames[index] - (uintptr_t)found.dli_saddr));
		host_logf(HOST_LOG_ERROR, "CRASH: frame %d: 0x%016lx%s", index, (unsigned long)frames[index], guest);
	}
}

static void crash_now(NSString *name, const char *message) __attribute__((noreturn));
static void crash_now(NSString *name, const char *message)
{
	uintptr_t frames[MAXIMUM_FRAMES];
	char reason[1536];
	struct text text = { reason, sizeof(reason), 0 };
	int count;

	if (atomic_exchange(&crashing, 1))
		abort();
	/* from this frame up: through the bridge into the guest, back out to the host that entered it */
	count = stack_frames(0, (uintptr_t)__builtin_frame_address(0), frames, MAXIMUM_FRAMES);
	text_add(&text, message);
	{
		int index, guest = 0;

		for (index = 0; index < count; index++)
			guest |= guest_code_contains(frames[index]);
		if (guest)
		{
			text_add(&text, " [guest: ");
			text_guest_frames(&text, frames, count, REASON_FRAMES);
			text_add(&text, "]");
		}
	}
	host_logf(HOST_LOG_ERROR, "CRASH: %s", reason);
	host_logf(HOST_LOG_ERROR, "CRASH: guest image sha256 %s, arena 0x%016lx", HALO_GUEST_SHA256,
		(unsigned long)host_arena);
	log_frames(frames, count);
	text_add(&text, " (guest image ");
	text_add(&text, guest_image_short);
	text_add(&text, ")");
	atomic_store(&raised, 1);
	atomic_store(&crashing, 0);
	@throw [NSException exceptionWithName:name reason:@(reason) userInfo:nil];
}

void host_crash(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	crash_now(@"VISRFatalError", message);
}

/* the last line of the game's messages that names an assertion or an error ("EXCEPTION halt in ..."),
   else its last line */
static void halt_reason(const char *messages, char *reason, size_t size)
{
	const char *line = messages, *chosen = NULL, *last = NULL;

	while (line && *line)
	{
		const char *end = strpbrk(line, "\r\n");
		size_t length = end ? (size_t)(end - line) : strlen(line);

		if (length)
		{
			last = line;
			if (!strncmp(line, "EXCEPTION", 9))
				chosen = line;
		}
		line = end ? end + strspn(end, "\r\n") : NULL;
	}
	if (!chosen)
		chosen = last;
	if (!chosen)
	{
		snprintf(reason, size, "the game halted with no message");
		return;
	}
	snprintf(reason, size, "%.*s", (int)strcspn(chosen, "\r\n"), chosen);
}

static int halt_screen_wanted(void)
{
	const char *variable = getenv("HALO_HALT_SCREEN");
	char value[16];

	if (variable && *variable)
		snprintf(value, sizeof(value), "%s", variable);
	else
		host_config_string("debug.halt_screen", "false", value, sizeof(value));
	return !strcmp(value, "1") || !strcasecmp(value, "true") || !strcasecmp(value, "yes") || !strcasecmp(value, "on");
}

int host_guest_halt(const char *messages)
{
	char reason[1024];

	halt_reason(messages ? messages : "", reason, sizeof(reason));
	if (halt_screen_wanted())
	{
		host_logf(HOST_LOG_ERROR, "the game halted (debug.halt_screen: its halt screen instead of a crash): %s", reason);
		return 0;
	}
	host_logf(HOST_LOG_ERROR, "the game halted; its recent messages:\n%s", messages ? messages : "");
	crash_now(@"VISRGuestHalt", reason);
}
