/*
 * chvt.c — switch the console the screen is showing. (GPLv2)
 *
 * `chvt N` is what Ctrl+Alt+F<N> does, minus the keyboard: both end up in the
 * kernel's tty_vt_switch().  It exists because a script (and a test) needs a
 * way to change terminals, and because the hotkey is useless on a machine
 * being driven over a serial line or by `make test`.
 *
 * Usage:  chvt N        switch to /dev/ttyN and wait until it is on screen
 *         chvt          print which terminal is on screen now
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* <linux/vt.h> is not in musl's include tree. */
#define VT_GETSTATE   0x5603
#define VT_ACTIVATE   0x5606
#define VT_WAITACTIVE 0x5607

struct vt_stat { unsigned short v_active, v_signal, v_state; };

int main(int argc, char **argv)
{
    /* /dev/tty is this process's own terminal, which is the one whose driver
     * owns the switch.  /dev/console is the fallback for a process that has
     * no controlling terminal -- a boot script, typically. */
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0)
        fd = open("/dev/console", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "chvt: no terminal: %s\n", strerror(errno));
        return 1;
    }

    if (argc < 2) {
        struct vt_stat st;
        memset(&st, 0, sizeof st);
        if (ioctl(fd, VT_GETSTATE, &st) != 0) {
            fprintf(stderr, "chvt: VT_GETSTATE: %s\n", strerror(errno));
            return 1;
        }
        printf("%u\n", st.v_active);
        return 0;
    }

    char *end;
    long n = strtol(argv[1], &end, 10);
    if (*end || n < 1 || n > 63) {
        fprintf(stderr, "usage: chvt N\n");
        return 1;
    }

    if (ioctl(fd, VT_ACTIVATE, (int)n) != 0) {
        fprintf(stderr, "chvt: cannot switch to %ld: %s\n", n, strerror(errno));
        return 1;
    }
    /* VT_ACTIVATE only asks; VT_WAITACTIVE is what makes `chvt 2 && echo hi`
     * put the "hi" on the terminal the user is now looking at. */
    if (ioctl(fd, VT_WAITACTIVE, (int)n) != 0) {
        fprintf(stderr, "chvt: wait for %ld: %s\n", n, strerror(errno));
        return 1;
    }
    return 0;
}
