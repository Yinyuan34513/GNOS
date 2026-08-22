/*
 * unix.h — AF_UNIX stream sockets. (GPLv2)
 *
 * All functions take the unix-table index u (>= 0), never the VFS priv.
 * The syscall layer converts: vfs priv = -2 - u.
 */
#ifndef GNUCOS_UNIX_H
#define GNUCOS_UNIX_H

#include <stdint.h>
#include "vfs.h"

int  unix_create(int type, int protocol);
void unix_close(int u);

int  unix_bind_sys(int u, const char *path, uint32_t len);
int  unix_connect_sys(int u, const char *path, uint32_t len);
int  unix_listen_sys(int u, int backlog);
int  unix_accept_sys(int u);

/* socketpair(53): link two fresh sockets as peers. */
void unix_link(int a, int b);

/* read()/write() on the vfs node (dispatch targets for sock.c). */
int32_t unix_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len);
int32_t unix_node_write(vfs_node_t *n, uint64_t off, const void *buf,
                        uint32_t len);

/* sendmsg/recvmsg with SCM_RIGHTS; `fds` are vfs handles. */
int unix_sendmsg(int u, const void *buf, uint32_t len, const int *fds,
                 int nfds);
int unix_recvmsg(int u, void *buf, uint32_t len, int *fds, int *nfds,
                 int flags);

int unix_readable(int u);
int unix_writable(int u);
int unix_set_nonblock(int u, int on);
int unix_is_nonblock(int u);
int unix_shutdown(int u, int how);

/* getname(sockaddr_un path out); `len` is in/out. */
int unix_getname(int u, char *path, uint32_t *len, int peer);

#endif