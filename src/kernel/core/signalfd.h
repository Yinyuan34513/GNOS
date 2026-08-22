/*
 * signalfd.h — signalfd4 syscall layer. (GPLv2)
 */
#ifndef GNUCOS_SIGNALFD_H
#define GNUCOS_SIGNALFD_H

#include <stdint.h>
#include "vfs.h"

/* signalfd4(fd, mask, sigsetsize, flags): create a signalfd when fd is -1,
 * or replace the mask of an existing one.  The Linux x86-64 number. */
int64_t sys_signalfd4(uint64_t fd, uint64_t umask, uint64_t sigsetsize,
                      uint64_t flags);

/* fcntl(F_SETFL) mirror, called from syscall.c. */
int  signalfd_is_signalfd(const vfs_node_t *n);
void signalfd_set_nonblock(vfs_node_t *n, int nb);

#endif
