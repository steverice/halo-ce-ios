/*
HOST_CRASH.C

Fatal errors as native crashes (host_crash.h).

Guest code runs from an alias of the app's __TEXT,__halocode in the guest
arena (host_loader.c), so Apple's crash reports show its frames as bare
addresses outside every image. The arena is 4 GB aligned, so an address's
low 32 bits are its guest address: the log here names the guest frames from
the symbol table embedded beside the code (__TEXT,__halosyms,
tools/ios_embed_guest.py), and tools/guest_symbolicate.py names them in a
whole report from the build's guest-symbols.txt.

A report keeps no text of the app's own: on macOS 27 neither an uncaught
NSException's reason, nor the Swift runtime's fatalError message, nor an
app's crash annotation (__DATA,__crash_info) reaches a report's Application
Specific Information; only the C library's fixed "abort() called" does. What
a report always keeps is the crashed thread's frames and registers. So:

- host_crash and host_guest_halt end in a function named for what happened
  (visr_guest_halt, visr_guest_abort, visr_host_error), the report's top
  frame, which Xcode's Organizer groups crashes by. Its trap instruction
  runs with the reason packed into the registers as text: x0 holds "VISRCRSH",
  then x1 to x15 and x19 to x28 hold up to 200 bytes of the guest image's
  short hash, a "|" and the reason. tools/guest_symbolicate.py prints it.
- A fault (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT) the memory
  watch does not own reaches crash_signal, which logs a summary, the
  registers and the frames with only async-signal-safe calls, then lets the
  fault happen again under the default action, so the report shows the
  original exception, registers and frames.
*/

#include <TargetConditionals.h>
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
/* guest frames named in the log's one-line summary; the lines after it, and the report's backtrace, have
   every frame */
#define REASON_FRAMES 8

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

/* the guest function holding address, or NULL; *offset is address's offset into it. A return address
   (returning, after a call that is a function's last instruction) can be the first byte of the next
   function: it is looked up one byte back, in the calling function */
static const char *guest_symbol(uint32_t address, int return_address, uint32_t *offset)
{
	uint32_t low = 0, high = symbol_count, looked_up = address - (return_address ? 1 : 0);

	while (low < high)
	{
		uint32_t middle = low + (high - low) / 2;

		if (symbols[middle].address <= looked_up)
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
static void text_guest(struct text *text, uintptr_t address, int return_address)
{
	uint32_t offset = 0;
	const char *name;

	if (!guest_code_contains(address))
		return;
	text_add(text, " guest ");
	text_hex(text, (uint32_t)address, 8);
	name = guest_symbol((uint32_t)address, return_address, &offset);
	if (name)
	{
		text_add(text, " ");
		text_add(text, name);
		text_add(text, "+");
		text_hex(text, offset, 1);
	}
}

/* "name+0x1c < name+0x40 < ..." for the guest frames among frames; the first is a pc when first_is_pc,
   and the rest are return addresses */
static void text_guest_frames(struct text *text, const uintptr_t *frames, int count, int limit, int first_is_pc)
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
		name = guest_symbol((uint32_t)frames[index], index > 0 || !first_is_pc, &offset);
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
/* set once host_crash has logged: the trap that follows is its */
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
	/* a second fault while reporting, or host_crash's own trap: already reported */
	if (atomic_exchange(&crashing, 1) || atomic_load(&raised))
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
	text_guest(&text, pc, 0);
	if (guest_code_contains(pc) || guest_code_contains(lr))
	{
		text_add(&text, " [guest: ");
		text_guest_frames(&text, frames, count, REASON_FRAMES, pc != 0);
		text_add(&text, "]");
	}
	text_add(&text, " (guest image ");
	text_add(&text, guest_image_short);
	text_add(&text, ")");

	log_write("CRASH: ", 7);
	log_write(summary, text.length);
	log_write("\n", 1);
	{
		struct text registers = { line, sizeof(line), 0 };

		text_add(&registers, "CRASH: arena ");
		text_hex(&registers, host_arena, 16);
		text_add(&registers, ", lr ");
		text_hex(&registers, lr, 16);
		text_guest(&registers, lr, 1);
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
		text_guest(&frame, frames[index], index > 0 || !pc);
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
#if !TARGET_OS_TV
	static char alternate[64 * 1024];
	stack_t stack = { .ss_sp = alternate, .ss_size = sizeof(alternate), .ss_flags = 0 };
#endif
	struct sigaction action = { 0 };
	int index;

	log_descriptor = descriptor;
	snprintf(guest_image_short, sizeof(guest_image_short), "%s", HALO_GUEST_SHA256);
	symbols_load();
#if !TARGET_OS_TV
	/* the game's thread (the main thread) keeps a stack to report on when its own runs out (tvOS has no
	   sigaltstack: there, overrunning the stack ends the app without the log's lines) */
	sigaltstack(&stack, NULL);
#endif
	action.sa_sigaction = crash_signal;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	for (index = 0; index < CRASH_SIGNAL_COUNT; index++)
		sigaction(crash_signals[index], &action, NULL);
	/* (tools/mac_run.py finds a run's crash report by its pid) */
	host_logf(HOST_LOG_INFO, "crash reports: %u guest symbols, pid %d", symbol_count, (int)getpid());
}

/* ---------- fatal errors */

/* frames are return addresses (crash_now's walk starts at its own frame record), so each is looked up
   one byte back: after a noreturn call that ends a function, the return address is the next function's
   first byte */
static void log_frames(const uintptr_t *frames, int count)
{
	int index;

	for (index = 0; index < count; index++)
	{
		char guest[256];
		struct text text = { guest, sizeof(guest), 0 };
		Dl_info found;

		text_guest(&text, frames[index], 1);
		if (!text.length && dladdr((const void *)(frames[index] - 1), &found) && found.dli_sname)
			snprintf(guest, sizeof(guest), " %s+0x%lx", found.dli_sname,
				(unsigned long)(frames[index] - (uintptr_t)found.dli_saddr));
		host_logf(HOST_LOG_ERROR, "CRASH: frame %d: 0x%016lx%s", index, (unsigned long)frames[index], guest);
	}
}

/* the registers a crash's reason is packed into (x16 and x17 are the trap's scratch, x18 is the
   platform's, x29 and x30 keep the frame chain the report's backtrace follows) */
#define REASON_REGISTERS 25
#define REASON_MAGIC 0x4853524352534956ull /* "VISRCRSH", little-endian */

struct reason_words
{
	uint64_t magic, words[REASON_REGISTERS];
};

/* loads x0 (the magic), x1-x15 and x19-x28 from words and traps. A macro so that the trap is in the
   named function, the report's top frame. The pointer is pinned to x16: with every other register
   clobbered the compiler used to pass it in x30, so the report's lr read as the pointer instead of the
   return address (the backtrace still followed the saved frame record, but a symbolicator that falls
   back to lr would have named a wrong frame) */
#define TRAP_WITH_REASON(reason) \
	do \
	{ \
		register const struct reason_words *table __asm__("x16") = (reason); \
		\
		__asm__ volatile( \
			"ldp x0, x1, [x16, #0]\n" "ldp x2, x3, [x16, #16]\n" "ldp x4, x5, [x16, #32]\n" \
			"ldp x6, x7, [x16, #48]\n" "ldp x8, x9, [x16, #64]\n" "ldp x10, x11, [x16, #80]\n" \
			"ldp x12, x13, [x16, #96]\n" "ldp x14, x15, [x16, #112]\n" "ldp x19, x20, [x16, #128]\n" \
			"ldp x21, x22, [x16, #144]\n" "ldp x23, x24, [x16, #160]\n" "ldp x25, x26, [x16, #176]\n" \
			"ldp x27, x28, [x16, #192]\n" \
			"brk #1\n" \
			:: "r"(table) \
			: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", \
			  "x15", "x17", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "memory"); \
		__builtin_unreachable(); \
	} while (0)

__attribute__((noinline, noreturn)) void visr_guest_halt(const struct reason_words *reason) { TRAP_WITH_REASON(reason); }
__attribute__((noinline, noreturn)) void visr_guest_abort(const struct reason_words *reason) { TRAP_WITH_REASON(reason); }
__attribute__((noinline, noreturn)) void visr_host_error(const struct reason_words *reason) { TRAP_WITH_REASON(reason); }

enum crash_kind { _crash_guest_halt, _crash_guest_abort, _crash_host_error };

static void crash_now(enum crash_kind kind, const char *message) __attribute__((noreturn));
static void crash_now(enum crash_kind kind, const char *message)
{
	static struct reason_words reason;
	uintptr_t frames[MAXIMUM_FRAMES];
	char summary[1536];
	struct text text = { summary, sizeof(summary), 0 };
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
			text_guest_frames(&text, frames, count, REASON_FRAMES, 0);
			text_add(&text, "]");
		}
	}
	host_logf(HOST_LOG_ERROR, "CRASH: %s", summary);
	host_logf(HOST_LOG_ERROR, "CRASH: guest image sha256 %s, arena 0x%016lx", HALO_GUEST_SHA256,
		(unsigned long)host_arena);
	log_frames(frames, count);
	reason.magic = REASON_MAGIC;
	snprintf((char *)reason.words, sizeof(reason.words), "%s|%s", guest_image_short, message);
	atomic_store(&raised, 1);
	atomic_store(&crashing, 0);
	switch (kind)
	{
	case _crash_guest_halt: visr_guest_halt(&reason);
	case _crash_guest_abort: visr_guest_abort(&reason);
	default: visr_host_error(&reason);
	}
}

void host_crash(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	crash_now(_crash_host_error, message);
}

/* the guest runtime's own fatal errors (guest_misc.c, guest_thread.c) */
void host_abort(const char *reason)
{
	char message[1024];

	snprintf(message, sizeof(message), "guest abort: %s", reason ? reason : "");
	crash_now(_crash_guest_abort, message);
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
	/* "EXCEPTION halt in c:\halo\SOURCE\units\units.c,#123: ..." keeps the file's name alone, so more of
	   the reason fits in a report's registers */
	if (!strncmp(reason, "EXCEPTION", 9))
	{
		char *in = strstr(reason, " in "), *comma = in ? strstr(in, ",#") : NULL;

		if (comma)
		{
			char *name = comma;

			while (name > in + 4 && name[-1] != '/' && name[-1] != '\\')
				name--;
			memmove(in + 4, name, strlen(name) + 1);
		}
	}
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
	crash_now(_crash_guest_halt, reason);
}

void host_map_load_failed(const char *map_name, const char *messages)
{
	host_logf(HOST_LOG_ERROR, "the level %s did not load; the game's recent messages:\n%s", map_name ? map_name : "?",
		messages ? messages : "");
	/* the map file's own name ("levels\\a10\\a10" is a10.map) */
	const char *name = map_name && *map_name ? map_name : "?", *slash = strrchr(name, '\\');

	host_fatal("The level %s could not be loaded. Its map file (%s.map) may be damaged or missing: import the game again.",
		slash ? slash + 1 : name, slash ? slash + 1 : name);
}
