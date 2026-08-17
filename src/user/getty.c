/*
 * getty.c — open a terminal, greet whoever is at it, and hand it to login.
 * (GPLv2)
 *
 * One of these runs on each of the six virtual terminals.  Its job is to turn
 * a bare character device into a *session*:
 *
 *   setsid()      leave init's session, become a session leader with no
 *                 controlling terminal -- the precondition for the next step;
 *   TIOCSCTTY     claim /dev/ttyN as this session's controlling terminal, so
 *                 that Ctrl-C on it reaches the right process group and
 *                 /dev/tty resolves to it for everything downstream;
 *   tcsetpgrp()   make ourselves (and therefore login, and the shell it
 *                 execs) the foreground job.
 *
 * Skip any of that and the shell starts without job control: Ctrl-C kills
 * nothing, and every background process that touches the terminal gets a
 * SIGTTOU it never asked for.
 *
 * Usage: getty ttyN [login-program]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define ISSUE  "/etc/issue"
#define LOGIN  "/bin/login"

/* Print /etc/issue, expanding the handful of escapes agetty defines that
 * mean anything here.  \l is the terminal name, \n the hostname. */
static void print_issue(const char *tty)
{
    FILE *f = fopen(ISSUE, "r");
    if (!f)
        return;

    char host[64] = "gnos";
    FILE *h = fopen("/etc/hostname", "r");
    if (h) {
        if (fgets(host, sizeof host, h)) {
            char *nl = strchr(host, '\n');
            if (nl)
                *nl = 0;
        }
        fclose(h);
    }

    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c != '\\') {
            fputc(c, stdout);
            continue;
        }
        int e = fgetc(f);
        switch (e) {
        case 'l': fputs(tty, stdout);  break;
        case 'n': fputs(host, stdout); break;
        case EOF: fputc('\\', stdout); break;
        default:  fputc('\\', stdout); fputc(e, stdout); break;
        }
    }
    fclose(f);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *name  = (argc > 1) ? argv[1] : "tty1";
    const char *lgin  = (argc > 2) ? argv[2] : LOGIN;

    char path[64];
    if (name[0] == '/')
        snprintf(path, sizeof path, "%s", name);
    else
        snprintf(path, sizeof path, "/dev/%s", name);

    /*
     * Leave init's session first.  setsid() fails if we are already a process
     * group leader, which is exactly what init made us -- so a failure here
     * is not fatal, it just means somebody already arranged the session.
     */
    setsid();

    /* O_NOCTTY: acquiring the terminal is TIOCSCTTY's job below, done
     * deliberately and with a return value we can check, rather than as a
     * side effect of open() that silently does nothing if it fails. */
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "getty: %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (ioctl(fd, TIOCSCTTY, 0) != 0) {
        fprintf(stderr, "getty: %s: cannot become controlling terminal: %s\n",
                path, strerror(errno));
        return 1;
    }

    if (dup2(fd, 0) < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
        return 1;
    if (fd > 2)
        close(fd);

    /* Foreground job = us, and after the exec, login and its shell. */
    tcsetpgrp(0, getpgrp());

    /* A terminal that a previous session left in raw mode with echo off would
     * make the login prompt unusable, so put it back to the defaults every
     * time round rather than trusting whoever had it last. */
    struct termios t;
    if (tcgetattr(0, &t) == 0) {
        t.c_iflag |= ICRNL | IXON | BRKINT;
        t.c_oflag |= OPOST | ONLCR;
        t.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG;
        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSAFLUSH, &t);
    }

    print_issue(name);

    char *av[2] = { (char *)lgin, NULL };
    execv(lgin, av);

    fprintf(stderr, "getty: cannot exec %s: %s\n", lgin, strerror(errno));
    return 127;
}
