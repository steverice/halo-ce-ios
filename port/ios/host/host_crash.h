/*
HOST_CRASH.H

Fatal errors as native crashes (host_crash.c). A fatal error, the game's own
(an assertion or a halt, which used to stop on the game's blue halt screen),
a host bug or a fault in guest code, is written to ios-runtime.log with the
guest's frames named, then ends the app as a native crash, so that Apple's
crash reporting (TestFlight feedback, Xcode's Organizer, the device's own
analytics) keeps a report of it, its reason packed into the crashed thread's
registers. tools/guest_symbolicate.py prints that reason and names the
report's guest frames from the build's guest-symbols.txt.
*/

#ifndef HALO_HOST_CRASH_H
#define HALO_HOST_CRASH_H

/* at launch, before host_benchmark_install and host_install_signal_handlers,
so that the memory watch's handler sees its own faults first and only passes
on the rest; descriptor is ios-runtime.log's, written in a signal handler */
void host_crash_install(int descriptor);

/* host_crash (a host bug, or a guest failure the host is told of) is
declared in host.h, beside host_fatal */

/* the guest's halt_and_catch_fire (source/main/main.c): messages is the
game's recent errors (error_get), the last naming the assertion or error.
Crashes, unless debug.halt_screen asks for the game's blue halt screen:
then returns 0 and the game draws it */
int host_guest_halt(const char *messages);

#endif
