/*
 * ptracetest.c — ptrace(2) self-test. (GPLv2, musl)
 *
 * Boot-time proof that the kernel's ptrace speaks the Linux ABI:
 * TRACEME, a SIGSTOP stop that waitpid() reports without WUNTRACED,
 * SIGCONT failing to resume a ptrace stop, PEEKDATA/POKEDATA across the
 * tracee's page tables, GETREGS, a TRACESYSGOOD syscall-entry stop under
 * PTRACE_SYSCALL, and the exit status the tracer lets through.  Every
 * verdict goes to the debug console via dbgputs(441) so a headless run
 * can read them, the same way drmtest does.
 */
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failed;
static long g_child_word;   /* the word PEEKDATA/POKEDATA reach for */

static void report(const char *fmt, ...)
{
    char buf[128];
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
    report("PTRACETEST: %s %d (%s)", ok ? "PASS" : "FAIL", n, what);
    if (!ok && !g_failed)
        g_failed = n;
}

int main(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        report("PTRACETEST: fork failed");
        return 1;
    }

    if (pid == 0) {
        /* The child traces itself: from now on the parent owns every stop
         * and, on exit, the wait status. */
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(2);

        raise(SIGSTOP);                 /* the first ptrace stop */

        /* Resumed by PTRACE_SYSCALL: the tracer has poked g_child_word. */
        if (g_child_word != 43)
            _exit(3);

        _exit(43);                      /* its exit syscall-entry stops first */
    }

    int st;
    if (waitpid(pid, &st, 0) < 0) {
        report("PTRACETEST: waitpid: %s", strerror(2)); /* ENOSYS-ish */
        return 1;
    }
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, 1,
          "SIGSTOP stop reported, no WUNTRACED");

    /* SIGCONT must not resume a ptrace stop: only PTRACE_CONT/SYSCALL can. */
    kill(pid, SIGCONT);
    check(waitpid(pid, &st, WNOHANG) == 0, 2,
          "SIGCONT does not resume a ptrace stop");

    long w = ptrace(PTRACE_PEEKDATA, pid, &g_child_word, 0);
    check(w == 0, 3, "PEEKDATA reads the tracee's word");
    check(ptrace(PTRACE_POKEDATA, pid, &g_child_word, 43) == 0, 4,
          "POKEDATA writes through the tracee's page tables");

    struct user_regs_struct regs;
    check(ptrace(PTRACE_GETREGS, pid, 0, &regs) == 0, 5, "GETREGS");
    check(regs.rip != 0 && regs.rsp != 0 && regs.orig_rax != 0, 6,
          "register image sane (rip/rsp/orig_rax)");

    /* TRACESYSGOOD: the syscall stop now reports SIGTRAP|0x80, the way
     * strace tells syscall stops from stray SIGTRAPs. */
    check(ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD) == 0, 7,
          "SETOPTIONS(TRACESYSGOOD)");

    /* Resume in PTRACE_SYSCALL mode: the child's _exit() stops at entry. */
    check(ptrace(PTRACE_SYSCALL, pid, 0, 0) == 0, 8, "PTRACE_SYSCALL");
    if (waitpid(pid, &st, 0) < 0)
        return 1;
    check(WIFSTOPPED(st) && WSTOPSIG(st) == (SIGTRAP | 0x80), 9,
          "syscall-entry stop reports SIGTRAP|0x80");

    check(ptrace(PTRACE_CONT, pid, 0, 0) == 0, 10, "PTRACE_CONT lets it die");
    if (waitpid(pid, &st, 0) < 0)
        return 1;
    check(WIFEXITED(st) && WEXITSTATUS(st) == 43, 11,
          "tracer-injected exit status 43");

    report("PTRACETEST: done (%s)", g_failed ? "FAILED" : "ALL PASS");
    return g_failed;
}