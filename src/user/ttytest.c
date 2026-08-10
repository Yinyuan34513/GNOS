/*
 * ttytest.c — a non-interactive check that the termios layer is real. (GPLv2)
 *
 * Everything a terminal does is normally proved by typing at it, which is no
 * use in a headless boot.  So this program asserts the parts that do not need
 * a human: that /dev/tty answers the ioctls, that the defaults are the ones a
 * Linux program expects to find, that cfmakeraw()'s settings survive a round
 * trip through the kernel, that VMIN/VTIME actually bound a read instead of
 * blocking for ever, and that a background process is stopped with SIGTTOU
 * when it tries to reconfigure the terminal.
 *
 * It is built against musl on purpose: the point is not that *our* headers
 * agree with the kernel, it is that a real libc's <termios.h> does.
 *
 * Two details are deliberate and easy to get wrong:
 *
 *   - it opens its own descriptor instead of using fd 0, because /etc/rc runs
 *     with the script spliced onto stdin and a file is not a terminal;
 *   - the raw-mode assertions are computed first and printed afterwards.
 *     With OPOST off a '\n' is a bare line feed, so printing mid-test would
 *     staircase the log down the right-hand side of the screen.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int fails;

static void ok(const char *what, int cond)
{
    printf("ttytest: %-34s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond)
        fails++;
}

int main(void)
{
    int tty = open("/dev/tty", O_RDWR);
    ok("open /dev/tty", tty >= 0);
    if (tty < 0)
        return 1;

    /* ---- what is and is not a terminal ---------------------------------- */
    ok("isatty(stdout)", isatty(1) == 1);
    ok("isatty(script stdin) is false", isatty(0) == 0);

    struct termios probe;
    int file = open("/etc/rc", O_RDONLY);
    errno = 0;
    ok("tcgetattr on a file -> ENOTTY",
       file >= 0 && tcgetattr(file, &probe) < 0 && errno == ENOTTY);
    if (file >= 0)
        close(file);

    /* ---- window size ---------------------------------------------------- */
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ok("TIOCGWINSZ reports a size",
       ioctl(tty, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0);

    /* ---- the defaults a program expects to inherit ---------------------- */
    struct termios saved;
    ok("tcgetattr", tcgetattr(tty, &saved) == 0);
    ok("ICANON|ECHO|ISIG are set",
       (saved.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG));
    ok("OPOST|ONLCR are set",
       (saved.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR));
    ok("ICRNL is set", (saved.c_iflag & ICRNL) != 0);
    ok("CS8 is set", (saved.c_cflag & CSIZE) == CS8);
    ok("cfgetospeed is B38400", cfgetospeed(&saved) == B38400);
    ok("c_cc[VINTR] is ^C",  saved.c_cc[VINTR]  == 003);
    ok("c_cc[VEOF] is ^D",   saved.c_cc[VEOF]   == 004);
    ok("c_cc[VERASE] is DEL", saved.c_cc[VERASE] == 0177);
    ok("c_cc[VSUSP] is ^Z",  saved.c_cc[VSUSP]  == 032);
    ok("c_cc[VMIN]/[VTIME] are 1/0",
       saved.c_cc[VMIN] == 1 && saved.c_cc[VTIME] == 0);

    /* ---- raw mode (results printed after the terminal is put back) ------ */
    struct termios raw = saved, back;
    cfmakeraw(&raw);

    int r_set = tcsetattr(tty, TCSANOW, &raw) == 0;
    int r_get = tcgetattr(tty, &back) == 0;
    int r_lflag = !(back.c_lflag & (ICANON | ECHO | ISIG));
    int r_oflag = !(back.c_oflag & OPOST);
    int r_iflag = !(back.c_iflag & ICRNL);
    int r_cc    = back.c_cc[VMIN] == 1 && back.c_cc[VTIME] == 0;

    /* MIN 0 / TIME 0 is a poll: nobody has typed anything, so it has to come
     * straight back with nothing rather than wait for a keystroke. */
    char ch = 0;
    back.c_cc[VMIN]  = 0;
    back.c_cc[VTIME] = 0;
    int r_poll = tcsetattr(tty, TCSANOW, &back) == 0 && read(tty, &ch, 1) == 0;

    /* MIN 0 / TIME 3 is a bounded wait: 0.3 s of nothing, then give up.  If
     * the kernel spun instead of sleeping this is where it would show. */
    back.c_cc[VTIME] = 3;
    int r_timed = tcsetattr(tty, TCSANOW, &back) == 0 && read(tty, &ch, 1) == 0;

    int r_restore = tcsetattr(tty, TCSAFLUSH, &saved) == 0;

    ok("cfmakeraw settings accepted", r_set && r_get);
    ok("raw clears ICANON|ECHO|ISIG", r_lflag);
    ok("raw clears OPOST", r_oflag);
    ok("raw clears ICRNL", r_iflag);
    ok("cfmakeraw leaves VMIN=1 VTIME=0", r_cc);
    ok("VMIN=0 VTIME=0 read returns 0", r_poll);
    ok("VMIN=0 VTIME=3 read times out", r_timed);
    ok("terminal restored", r_restore);

    /* ---- job control: SIGTTOU on a background reconfigure --------------- */
    int status = 0, stopped = 0, stopsig = 0;
    pid_t kid = fork();
    if (kid == 0) {
        setpgid(0, 0);                  /* leave the foreground group */
        tcsetattr(tty, TCSANOW, &saved);/* must be stopped before this lands */
        _exit(0);
    }
    if (kid > 0) {
        setpgid(kid, kid);              /* whoever runs first, same result */
        if (waitpid(kid, &status, WUNTRACED) == kid && WIFSTOPPED(status)) {
            stopped = 1;
            stopsig = WSTOPSIG(status);
        }
        kill(kid, SIGCONT);
        waitpid(kid, &status, 0);
    }
    ok("background tcsetattr is stopped", kid > 0 && stopped);
    ok("...and the signal is SIGTTOU", stopsig == SIGTTOU);

    close(tty);

    printf("ttytest: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
