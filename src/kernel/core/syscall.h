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
#include "proc.h"

/* Register the int 0x80 handler.  Call after idt_init(). */
void syscall_init(void);

/* The scan/block core shared by poll(2), select(2) and epoll_wait(2).
 * `p` points at pollfd triples (fd, events, revents) -- user memory for the
 * poll family, kernel scratch for epoll.  Returns the number of descriptors
 * with a nonzero revents, or a negative errno. */
int64_t do_ppoll(uint8_t *p, uint64_t nfds, int64_t ticks);

/* Per-process descriptor table access, shared with the anonymous-fd
 * creators (memfd/eventfd/epoll).  fd_handle returns the open-file handle
 * behind a descriptor, or -1; fd_alloc binds a handle to the lowest free
 * descriptor. */
int fd_handle(int fd);
int fd_alloc(proc_t *p, int handle);

/* Fill a *kernel* buffer with pseudo-random bytes from the same stream
 * getrandom(2) uses.  /dev/urandom has to be indistinguishable from
 * getrandom(2); a second generator would be a second thing to seed and a
 * second thing to get wrong. */
void krandom_bytes(void *buf, uint32_t n);

#endif
