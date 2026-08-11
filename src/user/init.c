/*
 * init.c — GNOS init, PID 1. (GPLv2)
 *
 * init is a supervisor, not a terminal.  It does exactly three things:
 *
 *   - makes itself immune to the terminal's signals, so that a stray Ctrl-C
 *     can never take the whole system down,
 *   - starts /shell.elf in a process group of its own and hands that group
 *     the terminal, which is what makes the shell the foreground job,
 *   - reaps children forever, including the orphans the kernel re-parents
 *     onto it, and restarts the shell whenever it dies.
 *
 * Keeping the shell in a separate process is not decoration: job control is
 * defined in terms of process groups and a controlling terminal, and neither
 * concept means anything if the only process on the system is also the one
 * reading the keyboard.
 */
#include "ulib.h"

#define SHELL_PATH  "/bin/bash"   /* the interactive login shell */
#define RC_SHELL    "/bin/sh"     /* the one-shot startup script */
#define RC_PATH     "/etc/rc"

/* If the shell cannot even start, stop trying: an exec that fails instantly
 * in a restart loop is a fork bomb with extra steps. */
#define MAX_FAST_RESTARTS 5

static void ignore_terminal_signals(void)
{
    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
}

static void default_terminal_signals(void)
{
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
}

/*
 * Run a single startup script through the shell and wait for it to finish.
 * It is its own process group but is not given the terminal: a script reads
 * its commands from a file, not the keyboard, so there is nothing to
 * foreground.  A missing script is harmless -- the shell reports it and the
 * boot goes on.
 */
static void run_rc(void)
{
    int pid = fork();
    if (pid < 0)
        return;

    if (pid == 0) {
        char *av[3];
        av[0] = (char *)RC_SHELL;
        av[1] = (char *)RC_PATH;
        av[2] = 0;

        /* A startup script must not read from or block on the keyboard, so
         * point its stdin at /dev/null; stdout/stderr stay on the console. */
        int nullin = sys_open("/dev/null", O_RDONLY);
        if (nullin >= 0 && nullin != 0) {
            sys_dup2(nullin, 0);
            sys_close(nullin);
        }

        setpgid(0, 0);
        default_terminal_signals();
        execv(RC_SHELL, av);
        exit(127);
    }

    /* The startup script is the boot's one foreground job: it owns the
     * terminal while it runs, so that the commands it spawns -- e.g. ttytest
     * reconfiguring the line discipline -- are not a *background* group and
     * thus do not get stopped with SIGTTOU for touching the tty.  init itself
     * ignores SIGTTOU, so this tcsetpgrp from a non-foreground process is safe. */
    setpgid(pid, pid);
    tcsetpgrp(0, pid);

    int status = 0;
    waitpid(pid, &status, 0);
}

static int spawn_shell(void)
{
    int pid = fork();
    if (pid < 0)
        return pid;

    if (pid == 0) {
        char *av[2];
        av[0] = (char *)SHELL_PATH;
        av[1] = 0;

        /* A job control session starts here: our own group, and the
         * terminal handed to it. */
        setpgid(0, 0);
        tcsetpgrp(0, getpid());
        default_terminal_signals();

        execv(SHELL_PATH, av);

        print("init: cannot exec " SHELL_PATH "\n");
        exit(127);
    }

    /* Do it on this side too -- whoever gets scheduled first, the shell is
     * in its own group before it can matter. */
    setpgid(pid, pid);
    tcsetpgrp(0, pid);
    return pid;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ignore_terminal_signals();

    print("\nGNOS init: pid ");
    printn(getpid());
    print(" - starting the session\n");

    run_rc();                       /* one-shot /etc/rc before the prompt */

    int failed_execs = 0;

    for (;;) {
        int shell = spawn_shell();
        if (shell < 0) {
            print("init: fork failed, giving up\n");
            return 1;
        }

        int shell_status = 0;
        int shell_alive  = 1;
        while (shell_alive) {
            int status = 0;
            int who = waitpid(-1, &status, 0);

            if (who < 0)                /* no children at all: shouldn't be */
                break;
            if (who == shell) {
                shell_alive  = 0;
                shell_status = status;
            } else if (who > 0) {
                /* An orphan the kernel re-parented onto us. */
                print("init: reaped orphan pid ");
                printn(who);
                print("\n");
            }
        }

        /* Take the terminal back before saying anything on it. */
        tcsetpgrp(0, getpid());
        print("\ninit: shell exited, restarting\n");

        /* Exit code 127 is the one our own child uses when execv() fails, so
         * it is the only case where restarting cannot possibly help. */
        if (WIFEXITED(shell_status) && WEXITSTATUS(shell_status) == 127)
            failed_execs++;
        else
            failed_execs = 0;

        if (failed_execs > MAX_FAST_RESTARTS) {
            print("init: " SHELL_PATH " will not start, stopping here\n");
            return 1;
        }
    }
}
