/*
 * epoll.h — the epoll syscall layer. (GPLv2)
 */
#ifndef GNUCOS_EPOLL_H
#define GNUCOS_EPOLL_H

#include <stdint.h>

int64_t sys_epoll_create(uint64_t flags);
int64_t sys_epoll_create1(uint64_t flags);
int64_t sys_epoll_ctl(int epfd, int op, int fd, uint64_t up_event);
int64_t sys_epoll_wait(int epfd, uint64_t uevents, int maxevents, int ms);
int64_t sys_epoll_pwait(int epfd, uint64_t uevents, int maxevents, int ms,
                        uint64_t usigmask);

#endif