/*
 * socktest.c — AF_UNIX + timerfd self-test. (GPLv2, musl)
 *
 * Boot-time proof of the wayland transport the kernel now speaks: stream
 * sockets over a pathname, socketpair, blocking and non-blocking I/O,
 * SCM_RIGHTS fd passing, and timerfd on the 100 Hz tick.  Every verdict
 * goes to the debug console via dbgputs(441) so `make test` can read it
 * headlessly.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <stdint.h>

#ifndef AF_UNIX
#define AF_UNIX 1
#endif

static int g_failed;

static void report(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    syscall(441, buf);
}

static void check(int ok, int n, const char *what)
{
    report("SOCKTEST: %s %d (%s) errno=%d", ok ? "PASS" : "FAIL", n, what,
           errno);
    if (!ok && !g_failed)
        g_failed = n;
}

static int memfd_create(const char *name, unsigned flags)
{
    return (int)syscall(319, name, flags);
}

int main(void)
{
    /* 1. socketpair: two ends of one stream */
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, 1, "socketpair");

    /* 2. blocking echo across the pair */
    {
        const char msg[] = "ping-pong!";
        check(write(sv[0], msg, sizeof msg) == (ssize_t)sizeof msg, 2,
              "write to pair end A");
        char buf[64] = { 0 };
        check(read(sv[1], buf, sizeof buf) == (ssize_t)sizeof msg &&
              memcmp(buf, msg, sizeof msg) == 0, 3, "blocking read at B");
    }

    /* 3. SCM_RIGHTS: a memfd crosses the pair and still points at the same
     * memory (write through the received fd is visible to the sender). */
    {
        int mfd = memfd_create("scm", 0);
        check(mfd >= 0, 4, "memfd_create");
        char *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        check(mem != MAP_FAILED, 5, "mmap memfd");

        struct iovec iov = { (void *)"fd", 2 };
        char cmsgbuf[CMSG_SPACE(sizeof(int))];
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cmsgbuf;
        mh.msg_controllen = sizeof cmsgbuf;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        *(int *)CMSG_DATA(cm) = mfd;

        check(sendmsg(sv[0], &mh, 0) == 2, 6, "sendmsg with SCM_RIGHTS");

        char rbuf[8] = { 0 };
        int rfd = -1;
        char rcmsg[CMSG_SPACE(sizeof(int))];
        struct iovec riov = { rbuf, sizeof rbuf };
        struct msghdr rmh;
        memset(&rmh, 0, sizeof rmh);
        rmh.msg_iov = &riov;
        rmh.msg_iovlen = 1;
        rmh.msg_control = rcmsg;
        rmh.msg_controllen = sizeof rcmsg;
        int rn = recvmsg(sv[1], &rmh, 0);
        check(rn == 2 && memcmp(rbuf, "fd", 2) == 0, 7, "recvmsg data");
        cm = CMSG_FIRSTHDR(&rmh);
        check(cm && cm->cmsg_level == SOL_SOCKET &&
              cm->cmsg_type == SCM_RIGHTS, 8, "recvmsg SCM_RIGHTS cmsg");
        if (cm)
            rfd = *(int *)CMSG_DATA(cm);
        check(rfd >= 0, 9, "received fd installed");
        if (rfd >= 0) {
            char *m2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                            rfd, 0);
            check(m2 != MAP_FAILED && m2 != mem, 10, "mmap received fd");
            if (m2 != MAP_FAILED) {
                strcpy(m2, "hello");
                check(memcmp(mem, "hello", 6) == 0, 11,
                      "shared memory is the same file");
                munmap(m2, 4096);
            }
            close(rfd);
        }
        if (mem != MAP_FAILED)
            munmap(mem, 4096);
        close(mfd);
    }

    /* 4. non-blocking pair end: EAGAIN on empty read */
    {
        int nb[2];
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, nb) == 0,
              12, "socketpair SOCK_NONBLOCK");
        char c;
        errno = 0;
        check(read(nb[0], &c, 1) == -1 && errno == EAGAIN, 13,
              "EAGAIN on empty read");
        check(write(nb[1], "x", 1) == 1 && read(nb[0], &c, 1) == 1, 14,
              "nonblock write/read");
        close(nb[0]);
        close(nb[1]);
    }

    /* 5. pathname server: bind/listen/connect/accept + poll readiness */
    {
        const char *path = "/tmp/wayland-0";
        unlink(path);
        int srv = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, path);
        check(bind(srv, (struct sockaddr *)&sa, sizeof sa) == 0, 15,
              "bind /tmp/wayland-0");
        check(listen(srv, 4) == 0, 16, "listen");

        int cli = socket(AF_UNIX, SOCK_STREAM, 0);
        check(connect(cli, (struct sockaddr *)&sa, sizeof sa) == 0, 17,
              "connect");

        struct pollfd pf = { srv, POLLIN, 0 };
        check(poll(&pf, 1, 100) == 1 && (pf.revents & POLLIN), 18,
              "poll: listener readable with pending connect");

        int acc = accept(srv, NULL, NULL);
        check(acc >= 0, 19, "accept");

        check(write(cli, "hi", 2) == 2, 20, "client write");
        char buf[8];
        check(read(acc, buf, sizeof buf) == 2 && memcmp(buf, "hi", 2) == 0,
              21, "server read");

        check(shutdown(cli, SHUT_WR) == 0, 22, "shutdown(SHUT_WR)");
        check(read(acc, buf, sizeof buf) == 0, 23, "EOF at peer after shutdown");

        close(acc);
        close(cli);
        close(srv);
        unlink(path);
    }

    /* 6. connect to a path nobody listens on */
    {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, "/tmp/nowhere-0");
        errno = 0;
        check(connect(fd, (struct sockaddr *)&sa, sizeof sa) == -1 &&
              errno == ECONNREFUSED, 24, "connect to unlistened path");
        close(fd);
    }

    /* 7. timerfd: one-shot 200 ms, then a 100 ms period, driven by poll */
    {
        int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
        check(tfd >= 0, 25, "timerfd_create");

        struct itimerspec its;
        memset(&its, 0, sizeof its);
        its.it_value.tv_nsec = 200 * 1000 * 1000;
        check(timerfd_settime(tfd, 0, &its, NULL) == 0, 26,
              "timerfd_settime 200ms");

        struct pollfd pf = { tfd, POLLIN, 0 };
        check(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN), 27,
              "poll: timerfd readable after expiry");

        uint64_t ex = 0;
        check(read(tfd, &ex, sizeof ex) == 8 && ex == 1, 28,
              "timerfd read: 1 expiration");

        its.it_value.tv_nsec = 100 * 1000 * 1000;
        its.it_interval.tv_nsec = 100 * 1000 * 1000;
        check(timerfd_settime(tfd, 0, &its, NULL) == 0, 29,
              "timerfd_settime periodic 100ms");
        struct pollfd pf2[1] = { { tfd, POLLIN, 0 } };
        check(poll(pf2, 1, 2000) == 1, 30, "poll periodic #1");
        check(read(tfd, &ex, sizeof ex) == 8 && ex >= 1, 31,
              "read periodic #1");
        check(poll(pf2, 1, 2000) == 1, 32, "poll periodic #2");
        check(read(tfd, &ex, sizeof ex) == 8 && ex >= 1, 33,
              "read periodic #2");

        struct itimerspec now;
        check(timerfd_gettime(tfd, &now) == 0, 34, "timerfd_gettime");
        check(now.it_interval.tv_nsec == 100 * 1000 * 1000, 35,
              "gettime: interval preserved");
        close(tfd);
    }

    close(sv[0]);
    close(sv[1]);

    report("SOCKTEST: done (%s)", g_failed ? "FAILED" : "ALL PASS");
    return g_failed;
}