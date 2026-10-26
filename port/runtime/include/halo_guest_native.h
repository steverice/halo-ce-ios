/*
HALO_GUEST_NATIVE.H

A saved frame pointer or a return address in the iOS guest is a native 64-bit
address, while the guest's pointers are offsets from the arena's start, which
the host keeps in x27 and which need not be a multiple of 4 GB
(tools/ios_asm_convert.py). halo_guest_address gives the guest pointer for such
an address, and 0 for 0. The low 32 bits of the native address are enough, so
an ILP32 cast that truncated it (__builtin_return_address) still converts.
*/

#ifndef __HALO_GUEST_NATIVE_H
#define __HALO_GUEST_NATIVE_H

#if defined(HALO_IOS) && defined(__aarch64__)
static __inline__ unsigned int halo_guest_address(unsigned long long native)
{
	unsigned long long arena;

	__asm__("mov %0, x27" : "=r"(arena));
	return native ? (unsigned int)native - (unsigned int)arena : 0;
}
#else
static __inline__ unsigned int halo_guest_address(unsigned long long native)
{
	return (unsigned int)native;
}
#endif

#endif
