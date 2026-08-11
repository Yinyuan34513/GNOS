/*
 * syscall.h — POSIX system-call layer reached through int 0x80. (GPLv2)
 *
 * The numbers live in the shared contract header so that user programs and
 * the kernel are compiled against the same list.
 */
#ifndef GNUCOS_SYSCALL_H
#define GNUCOS_SYSCALL_H

#include <stdint.h>
#include "sysnum.h"

/* Register the int 0x80 handler.  Call after idt_init(). */
void syscall_init(void);

/* Fill a *kernel* buffer with pseudo-random bytes from the same stream
 * getrandom(2) uses.  /dev/urandom has to be indistinguishable from
 * getrandom(2); a second generator would be a second thing to seed and a
 * second thing to get wrong. */
void krandom_bytes(void *buf, uint32_t n);

#endif
