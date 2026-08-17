/*
 * evtest.c — /dev/input/event0 self-test. (GPLv2, musl)
 *
 * Boot-time proof that the kernel's evdev driver speaks the Linux UAPI:
 * identity ioctls (version, id, name, capability bitmap), then a synthetic
 * key press/release driven through inputinject(405) -- the input twin of
 * ttyinject -- observed through poll() and read(), and finally the
 * O_NONBLOCK/EAGAIN behaviour libevdev relies on.  Every verdict goes to
 * the debug console via dbgputs(441) so `make test` can read it headlessly.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ---- the UAPI subset, byte-copies from the kernel's input.h ------------ */
struct input_event {
    long tv_sec;
    long tv_usec;
    unsigned short type, code;
    int value;
};

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02

#define SYN_REPORT 0
#define KEY_A 30
#define REL_X 0x00
#define REL_Y 0x01

#define BUS_I8042 0x11

#define EVIOCGVERSION   0x80044501
#define EVIOCGID        0x80084502
#define EVIOCGNAME(len) (0x80000000 | ((len) << 16) | 0x4506)
/* EVIOCGBIT(ev,len): _IOC(READ, 'E', 0x20+ev, len) -- ev rides in nr. */
#define EVIOCGBIT(ev, len) (0x80000000 | ((len) << 16) | (0x45 << 8) | 0x20 + (ev))
#define EV_VERSION 0x010100

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
    report("EVTEST: %s %d (%s)", ok ? "PASS" : "FAIL", n, what);
    if (!ok && !g_failed)
        g_failed = n;
}

int main(void)
{
    int fd = open("/dev/input/event0", O_RDONLY);
    check(fd >= 0, 1, "open /dev/input/event0");
    if (fd < 0) {
        report("EVTEST: done (FAILURES)");
        return 1;
    }

    int v = 0;
    check(ioctl(fd, EVIOCGVERSION, &v) == 0 && v == EV_VERSION, 2, "EVIOCGVERSION");

    unsigned short id[4] = {0, 0, 0, 0};
    check(ioctl(fd, EVIOCGID, id) == 0 && id[0] == BUS_I8042, 3, "EVIOCGID bus");

    char name[64] = {0};
    int n = ioctl(fd, EVIOCGNAME(64), name);
    check(n > 0 && strstr(name, "keyboard") != NULL, 4, "EVIOCGNAME");

    unsigned char bits[96] = {0};
    check(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) == 0 &&
          (bits[KEY_A >> 3] & (1u << (KEY_A & 7))), 5, "EVIOCGBIT has KEY_A");

    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, 20);
    check(r == 0, 6, "poll timeout with empty queue");

    /* A synthetic press + SYN, then a release + SYN. */
    syscall(405, EV_KEY, KEY_A, 1);
    syscall(405, EV_SYN, SYN_REPORT, 0);
    syscall(405, EV_KEY, KEY_A, 0);
    syscall(405, EV_SYN, SYN_REPORT, 0);

    r = poll(&p, 1, 20);
    check(r == 1 && (p.revents & POLLIN), 7, "poll sees injected events");

    struct input_event ev[4];
    int got = 0;
    while (got < 4) {
        ssize_t rd = read(fd, (char *)ev + got * sizeof ev[0],
                          sizeof ev[0]);
        if (rd != (ssize_t)sizeof ev[0])
            break;
        got++;
    }
    check(got == 4, 8, "read four events");
    check(ev[0].type == EV_KEY && ev[0].code == KEY_A && ev[0].value == 1, 9,
          "press event");
    check(ev[1].type == EV_SYN && ev[1].code == SYN_REPORT, 10, "sync after press");
    check(ev[2].type == EV_KEY && ev[2].code == KEY_A && ev[2].value == 0, 11,
          "release event");
    check(ev[3].type == EV_SYN, 12, "sync after release");

    check(fcntl(fd, F_SETFL, O_NONBLOCK) == 0, 13, "fcntl O_NONBLOCK");
    errno = 0;
    struct input_event e;
    check(read(fd, &e, sizeof e) == -1 && errno == EAGAIN, 14,
          "nonblocking read EAGAIN");

    int m = open("/dev/input/event1", O_RDONLY);
    check(m >= 0, 15, "open /dev/input/event1");
    if (m >= 0) {
        memset(bits, 0, sizeof bits);
        check(ioctl(m, EVIOCGBIT(EV_REL, sizeof bits), bits) == 0 &&
              (bits[REL_X >> 3] & (1u << (REL_X & 7))) &&
              (bits[REL_Y >> 3] & (1u << (REL_Y & 7))), 16,
              "mouse advertises REL_X/REL_Y");
        close(m);
    }
    close(fd);

    report(g_failed ? "EVTEST: done (FAILURES)" : "EVTEST: done (ALL PASS)");
    return g_failed ? 1 : 0;
}
