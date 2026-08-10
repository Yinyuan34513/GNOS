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

#endif
