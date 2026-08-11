/*
 * readlinetest.c — a non-interactive check that the keyboard input path and
 * the line discipline actually move bytes. (GPLv2)
 *
 * Typing at a terminal is the normal way to prove this works, which is no
 * good in a headless boot.  So instead of a human we use the ttyinject(404)
 * syscall, which pushes a buffer into the line discipline exactly as the
 * keyboard IRQ would have.  We then read() those bytes back and check they
 * came through the way a real terminal would deliver them:
 *
 *   - a canonical line is returned whole, newline included;
 *   - VERASE (Backspace, 0x7F) edits the line before it is returned;
 *   - VEOF (Ctrl-D, 0x04) on an empty line ends the read with 0 bytes;
 *   - in raw mode the bytes -- including an arrow-key escape sequence --
 *     pass straight through untouched, which is what readline relies on.
 *
 * Built against musl so a real libc's <termios.h> and the ttyinject number
 * from sysnum.h agree with the kernel.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "sysnum.h"        /* SYS_ttyinject */

static int fails;

static void ok(const char *what, int cond)
{
    printf("readlinetest: %-34s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond)
        fails++;
}

/* Read exactly `want` bytes, blocking (but our input is already injected). */
static int read_exact(int fd, char *buf, int want)
{
    int got = 0;
    while (got < want) {
        int n = (int)read(fd, buf + got, (size_t)(want - got));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return got;                 /* EOF */
        got += n;
    }
    return got;
}

/* Push a string of keystrokes into the kernel as if typed. */
static void type(int fd, const char *s, size_t n)
{
    (void)fd;                          /* ttyinject is terminal-global */
    syscall(SYS_ttyinject, s, n);
}

int main(void)
{
    int tty = open("/dev/tty", O_RDWR);
    ok("open /dev/tty", tty >= 0);
    if (tty < 0) {
        printf("readlinetest: %d failure(s)\n", fails);
        return 1;
    }

    struct termios saved;
    ok("tcgetattr", tcgetattr(tty, &saved) == 0);

    /* ---- canonical: a whole line comes back, newline included ---------- */
    type(tty, "hello\n", 6);
    char buf[64];
    int n = read_exact(tty, buf, 6);
    ok("canonical line read returns 6", n == 6);
    buf[6] = '\0';
    ok("canonical line is 'hello\\n'", n == 6 && memcmp(buf, "hello\n", 6) == 0);

    /* ---- VERASE edits the line before it is delivered ------------------
     * VERASE is DEL (0x7F), not Ctrl-H (0x08), so feed the erase char the way
     * the keyboard would: scancode 0x0E decodes to 0x7F. */
    type(tty, "abc\177\177d\n", 7);    /* "abc", two erases, "d" -> "ad" */
    n = read_exact(tty, buf, 3);
    ok("backspace edits line (read 3)", n == 3);
    buf[3] = '\0';
    ok("backspace result is 'ad\\n'", n == 3 && memcmp(buf, "ad\n", 3) == 0);

    /* ---- VEOF on an empty line ends the read with 0 bytes -------------- */
    type(tty, "\x04", 1);              /* Ctrl-D */
    n = (int)read(tty, buf, sizeof buf);
    ok("Ctrl-D at empty line -> EOF (0)", n == 0);

    /* ---- raw mode: escape sequences pass through verbatim ------------- */
    struct termios raw = saved;
    cfmakeraw(&raw);
    ok("enter raw mode", tcsetattr(tty, TCSANOW, &raw) == 0);

    const char *seq = "\033[Axyz";     /* up-arrow escape + text */
    type(tty, seq, strlen(seq));
    char rbuf[16];
    n = read_exact(tty, rbuf, (int)strlen(seq));
    ok("raw mode read returns all bytes", n == (int)strlen(seq));
    rbuf[n] = '\0';
    ok("raw mode passes escape sequence through",
       n == (int)strlen(seq) && memcmp(rbuf, seq, strlen(seq)) == 0);

    /* ---- put the terminal back the way we found it --------------------- */
    ok("restore termios", tcsetattr(tty, TCSAFLUSH, &saved) == 0);

    close(tty);

    printf("readlinetest: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
