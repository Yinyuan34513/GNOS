/*
 * anonfd.h — memfd/eventfd syscall layer. (GPLv2)
 */
#ifndef GNUCOS_ANONFD_H
#define GNUCOS_ANONFD_H

#include <stdint.h>
#include "vfs.h"

int64_t sys_memfd_create(uint64_t name, uint64_t flags);
int64_t sys_eventfd(uint64_t count);
int64_t sys_eventfd2(uint64_t count, uint64_t flags);

/* fcntl(F_SETFL) mirror: 1 when the node is an eventfd, and the setter
 * itself.  Keeps a client that flips O_NONBLOCK through fcntl instead of
 * EFD_NONBLOCK behaving. */
int  anonfd_is_eventfd(const vfs_node_t *n);
void anonfd_set_nonblock(vfs_node_t *n, int nb);

#endif