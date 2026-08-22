/*
 * timerfd.h — timerfd syscall layer. (GPLv2)
 */
#ifndef GNUCOS_TIMERFD_H
#define GNUCOS_TIMERFD_H

#include <stdint.h>
#include "vfs.h"

int64_t sys_timerfd_create(uint64_t clockid, uint64_t flags);
int64_t sys_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t uin,
                            uint64_t uout);
int64_t sys_timerfd_gettime(uint64_t fd, uint64_t uout);

/* Called from the PIT interrupt: advance expirations and wake waiters. */
void timerfd_tick(void);

/* fcntl(F_SETFL) mirror. */
int  timerfd_is_timerfd(const vfs_node_t *n);
void timerfd_set_nonblock(vfs_node_t *n, int nb);

#endif