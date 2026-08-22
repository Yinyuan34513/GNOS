/*
 * eventest.c — memfd_create(319), eventfd(290) and epoll(291) self-test.
 * (GPLv2, musl)
 *
 * The three anonymous fds wlroots and libwayland build their event loop on:
 *
 *   - memfd: ftruncate, MAP_SHARED, write-through-fd landing in the same
 *     pages as the mapping, and a fork() proving the mapping is genuinely
 *     shared between processes (the wl_shm guarantee);
 *   - eventfd: EAGAIN on an empty nonblocking read, counter accumulation
 *     across writes, semaphore mode draining one at a time;
 *   - epoll: add/readiness/data round-trip and a timeout that returns
 *     zero, the exact behaviour libwayland's event loop depends on.
 *
 * Verdicts go to the debug console via dbgputs(441) for headless `make
 * test` runs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

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
    report("EVENTEST: %s %d (%s) errno=%d", ok ? "PASS" : "FAIL", n, what,
           errno);
    if (!ok && !g_failed)
        g_failed = n;
}

int main(void)
{
    /* ---- memfd: pool semantics ---------------------------------------- */
    int mfd = memfd_create("pool", 0);
    check(mfd >= 0, 1, "memfd_create");
    if (mfd < 0) {
        report("EVENTEST: done (FAILURES)");
        return 1;
    }
    check(ftruncate(mfd, 4096) == 0, 2, "ftruncate to 4096");

    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    check(p != MAP_FAILED, 3, "mmap MAP_SHARED");
    if (p != MAP_FAILED) {
        for (int i = 0; i < 4096; i++)
            p[i] = (char)(i & 0xFF);
        /* Write through the fd must land in the same pages. */
        static const char tag[] = "GNOSmemfd";
        check(write(mfd, tag, sizeof tag - 1) == (ssize_t)sizeof tag - 1 &&
              memcmp(p, tag, sizeof tag - 1) == 0, 4,
              "write() lands in the mapping");
        /* Fresh ftruncate extension reads as zero (shmem semantics):
         * grow the file, then re-map to the new size -- the order real
         * wl_shm pools use. */
        check(ftruncate(mfd, 8192) == 0, 5, "ftruncate to 8192");
        munmap(p, 4096);
        p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        check(p != MAP_FAILED && p[4096] == 0 && p[5000] == 0, 5,
              "extended pages are zeroed");
        if (p != MAP_FAILED)
            munmap(p, 8192);
    }

    /* fork() shares the mapping: child writes, parent reads. */
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (p != MAP_FAILED) {
        pid_t kid = fork();
        if (kid == 0) {
            strcpy(p, "hello from child");
            _exit(0);
        }
        int st = 0;
        waitpid(kid, &st, 0);
        check(strcmp(p, "hello from child") == 0, 6,
              "fork shares memfd mapping");
        munmap(p, 4096);
    }
    close(mfd);

    /* ---- eventfd: plain and semaphore --------------------------------- */
    int efd = eventfd(0, EFD_NONBLOCK);
    check(efd >= 0, 7, "eventfd2 nonblock");
    if (efd >= 0) {
        uint64_t u = 0;
        errno = 0;
        check(read(efd, &u, 8) == -1 && errno == EAGAIN, 8,
              "empty nonblocking read EAGAIN");
        u = 5;
        check(write(efd, &u, 8) == 8, 9, "write 5");
        u = 0;
        check(read(efd, &u, 8) == 8 && u == 5, 10, "read drains to 0");
        u = 1;
        write(efd, &u, 8); write(efd, &u, 8); u = 2; write(efd, &u, 8);
        u = 0;
        check(read(efd, &u, 8) == 8 && u == 4, 11, "counter accumulates");
        close(efd);
    }

    int sfd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    check(sfd >= 0, 12, "eventfd semaphore");
    if (sfd >= 0) {
        uint64_t u = 3;
        write(sfd, &u, 8);
        u = 0;
        check(read(sfd, &u, 8) == 8 && u == 1, 13, "semaphore drains 1");
        u = 0;
        check(read(sfd, &u, 8) == 8 && u == 1, 14, "semaphore drains 1 again");

        /* ---- epoll over the semaphore fd ------------------------------ */
        int ep = epoll_create1(0);
        check(ep >= 0, 15, "epoll_create1");
        if (ep >= 0) {
            struct epoll_event ev;
            memset(&ev, 0, sizeof ev);
            ev.events = EPOLLIN;
            ev.data.u64 = 0x1234;
            check(epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev) == 0, 16,
                  "epoll_ctl ADD");
            struct epoll_event out;
            memset(&out, 0, sizeof out);
            u = 0;
            read(sfd, &u, 8);                /* drain the one left over */
            read(sfd, &u, 8);                /* and the EAGAIN hit */
            errno = 0;
            check(epoll_wait(ep, &out, 1, 10) == 0, 17, "empty wait times out");
            errno = 0;
            check(epoll_wait(ep, &out, 1, 10) == 0, 18, "drained wait times out");
            u = 9;
            write(sfd, &u, 8);
            check(epoll_wait(ep, &out, 1, 100) == 1, 19, "epoll reports readiness");
            check((out.events & EPOLLIN) && out.data.u64 == 0x1234, 20,
                  "epoll event data round-trips");
            check(epoll_ctl(ep, EPOLL_CTL_DEL, sfd, NULL) == 0, 21,
                  "epoll_ctl DEL");
            close(ep);
        }
        close(sfd);
    }

    report(g_failed ? "EVENTEST: done (FAILURES)" : "EVENTEST: done (ALL PASS)");
    return g_failed ? 1 : 0;
}
