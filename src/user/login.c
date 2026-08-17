/*
 * login.c — authenticate a user and become them. (GPLv2)
 *
 * This is the one program on the system whose whole job is to *lose*
 * privilege.  getty execs it as root on a terminal nobody owns yet; it asks
 * for a name and a password, checks the password against /etc/shadow, and
 * then hands the terminal to a shell running as somebody who can no longer
 * undo any of it.
 *
 * The order of the drop matters and is the classic place to get it wrong:
 *
 *   initgroups() -> setgid() -> setuid()
 *
 * Supplementary groups and the primary gid must be set *while still root*,
 * because setgroups(2) and setgid(2) are privileged calls.  Doing setuid()
 * first would leave the process running as the user but still in root's
 * groups, with no way to fix it.  Each call is checked: a setuid() that
 * silently failed would hand out a root shell.
 *
 * Password entry turns ECHO off through termios and restores it on every
 * exit path, including the signal one -- a login that dies with the terminal
 * still silent leaves the next person typing into the void.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define MAX_TRIES 3

static struct termios g_saved;
static int            g_saved_ok;

static void restore_tty(void)
{
    if (g_saved_ok)
        tcsetattr(0, TCSANOW, &g_saved);
}

static void on_signal(int sig)
{
    (void)sig;
    restore_tty();
    _exit(1);
}

/* Read one line, minus its newline.  Returns 0 on success, -1 on EOF. */
static int read_line(char *buf, size_t cap)
{
    size_t n = 0;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r <= 0)
            return (r == 0 && n) ? 0 : -1;
        if (c == '\n' || c == '\r')
            break;
        if (n + 1 < cap)
            buf[n++] = c;
    }
    buf[n] = 0;
    return 0;
}

/* Same, with the terminal's echo turned off for the duration. */
static int read_secret(char *buf, size_t cap)
{
    struct termios raw;
    int quiet = 0;

    if (tcgetattr(0, &g_saved) == 0) {
        g_saved_ok = 1;
        raw = g_saved;
        raw.c_lflag &= ~(tcflag_t)ECHO;
        quiet = tcsetattr(0, TCSAFLUSH, &raw) == 0;
    }

    int r = read_line(buf, cap);

    if (quiet)
        restore_tty();
    g_saved_ok = 0;
    /* The user's Enter was swallowed by the silent terminal, so put the
     * line break on the screen ourselves. */
    write(1, "\n", 1);
    return r;
}

/*
 * Compare a typed password against the stored hash.  An empty hash field
 * means "no password"; a hash that is not a valid crypt setting (the "!" and
 * "*" the system accounts carry) can never match anything, which is exactly
 * what locking an account means.
 */
static int password_ok(const char *stored, const char *typed)
{
    if (!stored)
        return 0;
    if (!*stored)
        return !*typed;
    if (stored[0] == '!' || stored[0] == '*')
        return 0;

    char *got = crypt(typed, stored);
    if (!got)
        return 0;
    /* Not a timing-safe compare, and deliberately not pretending to be: the
     * attacker here is sitting at the keyboard of a machine whose root
     * password is printed on /etc/issue. */
    return strcmp(got, stored) == 0;
}

static const char *shadow_hash(const char *user, const struct passwd *pw)
{
    struct spwd *sp = getspnam(user);
    if (sp && sp->sp_pwdp)
        return sp->sp_pwdp;
    /* No /etc/shadow entry: fall back to the passwd field, which is how a
     * system without shadow passwords has always worked. */
    return pw->pw_passwd;
}

int main(int argc, char **argv)
{
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);

    /* `login -f name` skips authentication; only root may ask for it, which
     * is the case that matters (getty --autologin, and the installer). */
    const char *forced = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc && geteuid() == 0)
            forced = argv[++i];
    }

    char user[64], pass[128];
    struct passwd *pw = NULL;

    for (int tries = 0; ; tries++) {
        if (forced) {
            snprintf(user, sizeof user, "%s", forced);
        } else {
            if (tries >= MAX_TRIES) {
                fprintf(stderr, "login: too many tries\n");
                return 1;
            }
            fputs("login: ", stdout);
            fflush(stdout);
            if (read_line(user, sizeof user) < 0)
                return 1;
            if (!user[0])
                continue;
        }

        pw = getpwnam(user);

        if (!forced) {
            fputs("password: ", stdout);
            fflush(stdout);
            if (read_secret(pass, sizeof pass) < 0)
                return 1;
        }

        /*
         * An unknown user and a wrong password produce the same message and,
         * as far as anyone watching can tell, the same delay.  Telling them
         * apart is how you enumerate accounts.
         */
        if (forced || (pw && password_ok(shadow_hash(user, pw), pass))) {
            memset(pass, 0, sizeof pass);
            break;
        }
        memset(pass, 0, sizeof pass);
        sleep(1);
        fputs("login incorrect\n", stdout);
    }

    if (!pw) {                       /* only reachable via -f with a bad name */
        fprintf(stderr, "login: no such user: %s\n", user);
        return 1;
    }

    const char *shell = (pw->pw_shell && *pw->pw_shell) ? pw->pw_shell
                                                        : "/bin/sh";
    const char *home  = (pw->pw_dir && *pw->pw_dir) ? pw->pw_dir : "/";

    /* ---- drop privilege, in the only order that is safe ----------------- */
    if (initgroups(pw->pw_name, pw->pw_gid) != 0 && errno != EPERM) {
        fprintf(stderr, "login: initgroups: %s\n", strerror(errno));
        return 1;
    }
    if (setgid(pw->pw_gid) != 0) {
        fprintf(stderr, "login: setgid: %s\n", strerror(errno));
        return 1;
    }
    if (setuid(pw->pw_uid) != 0) {
        fprintf(stderr, "login: setuid: %s\n", strerror(errno));
        return 1;
    }
    /* Belt and braces: if the drop somehow did not take, do not exec a shell. */
    if (getuid() != pw->pw_uid || geteuid() != pw->pw_uid) {
        fprintf(stderr, "login: privilege drop failed\n");
        return 1;
    }

    if (chdir(home) != 0)
        chdir("/");

    setenv("HOME", home, 1);
    setenv("SHELL", shell, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("PATH", pw->pw_uid == 0 ? "/sbin:/bin:/usr/sbin:/usr/bin"
                                   : "/bin:/usr/bin", 1);

    /* A leading '-' in argv[0] is how every shell since v7 is told it is a
     * login shell, and therefore to read the profile files. */
    char arg0[64];
    const char *base = strrchr(shell, '/');
    snprintf(arg0, sizeof arg0, "-%s", base ? base + 1 : shell);

    char *av[2] = { arg0, NULL };
    execv(shell, av);

    fprintf(stderr, "login: cannot exec %s: %s\n", shell, strerror(errno));
    return 127;
}
