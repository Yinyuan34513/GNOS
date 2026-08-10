/*
 * sigtest.c — a non-interactive proof that user-installed signal handlers
 * work. (GPLv2)
 *
 * A signal is normally demonstrated by pressing ^C and watching a program
 * die, which is useless in a headless boot.  So this program arms handlers
 * with every flag worth having and then asserts the kernel did the right
 * thing: the right signal reached the right handler, sa_mask and SA_NODEFER
 * and SA_RESETHAND behaved, a blocked signal stayed pending until it was
 * unblocked, the whole register and FPU context survived the trip through
 * the stack frame, siglongjmp got out of a handler, and SA_RESTART made a
 * cut-off read() run to completion instead of coming back with EINTR.
 *
 * It is linked against musl on purpose.  musl's sigaction() always sets
 * SA_RESTORER and points it at its own nine-byte __restore_rt trampoline,
 * and its handlers read the ucontext at the offsets Linux defines -- so if
 * our rt_sigframe were not byte-for-byte Linux's, this program would crash
 * in user space with no clue.  Passing it means the ABI is right.
 */
#include <errno.h>
#include <stdint.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int fails;

static void ok(const char *what, int cond)
{
    printf("sigtest: %-46s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond)
        fails++;
}

/* Back to the default disposition; the kernel discards a pending signal when
 * its handler drops to SIG_DFL, which is exactly what we want between tests. */
static void reset(int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);
}

/* Every test starts with a clean mask, or a block left behind by the previous
 * one would change who gets delivered. */
static void unblock_all(void)
{
    sigset_t m;
    sigemptyset(&m);
    sigprocmask(SIG_SETMASK, &m, NULL);
}

/* clock_gettime() exists; nanosleep() does not, so a delay is a busy wait on
 * the monotonic clock the kernel does implement. */
static void msleep(int ms)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    do {
        clock_gettime(CLOCK_MONOTONIC, &b);
        long long e = (b.tv_sec - a.tv_sec) * 1000 +
                      (b.tv_nsec - a.tv_nsec) / 1000000;
        if (e >= ms)
            break;
    } while (1);
}

/* ---- 1. SA_SIGINFO: the right signal, with the right siginfo ----------- */
static volatile sig_atomic_t si_signo;
static volatile sig_atomic_t si_code;

static void siginfo_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)sig;
    (void)uctx;
    si_signo = (sig_atomic_t)info->si_signo;
    si_code  = (sig_atomic_t)info->si_code;
}

static void test_siginfo(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = siginfo_handler;
    sa.sa_flags     = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);

    si_signo = si_code = 0;
    raise(SIGUSR1);

    ok("SA_SIGINFO handler runs with correct signo", si_signo == SIGUSR1);
    ok("...and si_code reports SI_USER",             si_code  == SI_USER);
    reset(SIGUSR1);
}

/* ---- 2. sa_mask: a masked signal defers until the handler returns ------ */
static volatile sig_atomic_t usr1_depth;
static volatile sig_atomic_t got_usr2;
static volatile sig_atomic_t usr2_deferred;

static void usr1_mask_handler(int s)
{
    (void)s;
    usr1_depth++;
    got_usr2   = 0;            /* arm the watched flag */
    raise(SIGUSR2);            /* blocked by sa_mask until we return */
    usr2_deferred = (got_usr2 == 0);
}

static void usr2_handler(int s)
{
    (void)s;
    got_usr2 = 1;
}

static void test_samask(void)
{
    struct sigaction a1, a2;
    memset(&a1, 0, sizeof a1);
    memset(&a2, 0, sizeof a2);
    a1.sa_handler = usr1_mask_handler;
    sigaddset(&a1.sa_mask, SIGUSR2);          /* block SIGUSR2 while running */
    a2.sa_handler = usr2_handler;
    sigaction(SIGUSR1, &a1, NULL);
    sigaction(SIGUSR2, &a2, NULL);

    usr1_depth = usr2_deferred = 0;
    got_usr2 = 1;                             /* so the first check is honest */
    raise(SIGUSR1);

    ok("sa_mask: SIGUSR2 delivered after handler", got_usr2 == 1);
    ok("sa_mask: SIGUSR2 was blocked during handler",
       usr1_depth == 1 && usr2_deferred == 1);
    reset(SIGUSR1);
    reset(SIGUSR2);
}

/* ---- 3. SA_NODEFER: the same signal re-enters the handler ------------- */
static volatile sig_atomic_t noder_enter;
static volatile sig_atomic_t noder_reenter;

static void noder_handler(int s)
{
    (void)s;
    noder_enter++;
    if (noder_enter == 1) {
        int before = noder_enter;            /* still 1 */
        raise(SIGUSR2);                      /* not blocked -> re-enters now */
        noder_reenter = (noder_enter > before);
    }
}

static void test_nodedefer(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = noder_handler;
    sa.sa_flags   = SA_NODEFER;
    sigaction(SIGUSR2, &sa, NULL);

    noder_enter = noder_reenter = 0;
    raise(SIGUSR2);

    ok("SA_NODEFER: handler re-entered on same signal",
       noder_enter == 2 && noder_reenter == 1);
    reset(SIGUSR2);
}

/* ---- 4. the same signal delivered twice in a row ---------------------- */
static volatile sig_atomic_t twice;

static void twice_handler(int s)
{
    (void)s;
    twice++;
}

static void test_twice(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = twice_handler;
    sigaction(SIGUSR1, &sa, NULL);

    twice = 0;
    raise(SIGUSR1);
    raise(SIGUSR1);

    ok("same signal delivered twice in a row", twice == 2);
    reset(SIGUSR1);
}

/* ---- 5. block, observe pending, then SIG_SETMASK delivers it ----------- */
static volatile sig_atomic_t pmask_ran;

static void pmask_handler(int s)
{
    (void)s;
    pmask_ran = 1;
}

static void test_pending(void)
{
    struct sigaction sa;
    sigset_t block, pend;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = pmask_handler;
    sigaction(SIGUSR1, &sa, NULL);

    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, NULL);

    pmask_ran = 0;
    raise(SIGUSR1);                          /* now pending, not delivered */

    sigemptyset(&pend);
    sigpending(&pend);
    ok("blocked signal is pending", sigismember(&pend, SIGUSR1) == 1);

    sigemptyset(&block);                     /* SIG_SETMASK to empty -> fires */
    sigprocmask(SIG_SETMASK, &block, NULL);
    ok("SIG_SETMASK delivers the pending signal", pmask_ran == 1);
    reset(SIGUSR1);
}

/* ---- 6. SIGKILL and SIGSTOP cannot be caught (EINVAL) ----------------- */
static void test_uncatchable(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;

    errno = 0;
    int rk = sigaction(SIGKILL, &sa, NULL);
    ok("sigaction(SIGKILL) rejected with EINVAL",
       rk == -1 && errno == EINVAL);

    errno = 0;
    int rs = sigaction(SIGSTOP, &sa, NULL);
    ok("sigaction(SIGSTOP) rejected with EINVAL",
       rs == -1 && errno == EINVAL);
}

/* ---- 7. register + FPU context preserved across the frame ------------- */
static double g_fp;

static void ctx_handler(int s)
{
    (void)s;
    /* Do real FPU work so the handler clobbers the live xmm registers:
     * the kernel has to fxsave them on the way in and fxrstor them on the
     * way out, or main's computation below comes back wrong. */
    volatile double z = 3.0 * 3.0 + g_fp;
    (void)z;
}

static void test_context(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ctx_handler;
    sigaction(SIGUSR1, &sa, NULL);

    g_fp = 1.25;
    uint64_t iacc = 0;          /* likely kept in rbx (callee-saved) */
    double   facc = 0.0;         /* accumulates in xmm */
    for (int i = 0; i < 100; i++) {
        iacc += (uint64_t)i;
        facc += 0.01;
        if (i == 50)
            raise(SIGUSR1);      /* interrupts the loop mid-flight */
    }
    ok("integer registers preserved across signal", iacc == 4950);
    /* 0.01 added a hundred times is not exactly 1.0 in binary, so the check
     * allows a little rounding -- the point is that the FPU context (which
     * the handler clobbers) is restored whole, not that the sum is exact. */
    ok("fp registers preserved across signal",      facc > 0.99 && facc < 1.01);
    reset(SIGUSR1);
}

/* ---- 8. siglongjmp out of a handler (no rt_sigreturn) ----------------- */
static sigjmp_buf lj_env;

static void lj_handler(int s)
{
    (void)s;
    siglongjmp(lj_env, 1);
}

static void test_longjmp(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = lj_handler;
    sigaction(SIGUSR1, &sa, NULL);

    if (sigsetjmp(lj_env, 1) == 0) {
        raise(SIGUSR1);          /* should never return here */
        ok("siglongjmp out of handler", 0);
    } else {
        ok("siglongjmp out of handler", 1);
    }
    reset(SIGUSR1);
}

/* ---- 9. SA_RESETHAND: handler fires once, then default kills ---------- */
static volatile sig_atomic_t reset_ran;

static void reset_handler(int s, siginfo_t *info, void *uctx)
{
    (void)s; (void)info; (void)uctx;
    reset_ran = 1;
}

static void test_resethand(void)
{
    pid_t kid = fork();
    if (kid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = reset_handler;
        sa.sa_flags     = SA_SIGINFO | SA_RESETHAND;
        sigaction(SIGUSR1, &sa, NULL);

        reset_ran = 0;
        raise(SIGUSR1);                 /* handler runs, then resets to DFL */
        raise(SIGUSR1);                 /* default action: terminate child  */
        _exit(42);                      /* reached only if reset failed     */
    }

    int st = 0;
    waitpid(kid, &st, 0);
    /* reset_ran lives in the child; here we only see the child's fate.  If
     * SA_RESETHAND worked the second raise found SIG_DFL and killed the
     * child with SIGUSR1; if it did not, the handler would have run again
     * and the child would have _exit(42) instead. */
    ok("SA_RESETHAND: first delivery runs, then resets to DFL",
       WIFSIGNALED(st) && WTERMSIG(st) == SIGUSR1);
    reset(SIGUSR1);
}

/* ---- 10. SA_RESTART: a cut-off read() runs to completion --------------- */
static volatile sig_atomic_t restart_hit;

/* musl caches getpid()/getppid() across fork(), so a forked child reports
 * its parent's pid, not its own -- which would send our signal to init.
 * gettid() is filled in fresh by the kernel (tid == pid here), so it gives
 * the restart child's real pid for the signaler grandchild to target. */
static volatile pid_t g_restart_pid;

static void restart_handler(int s)
{
    (void)s;
    restart_hit = 1;
}

/* Run one restart scenario in a child.  With use_restart set the read() must
 * come back with the byte after the handler; without it, read() must return
 * EINTR.  Returns 1 if the child reported success. */
static int do_restart_test(int use_restart)
{
    int fds[2];
    if (pipe(fds) != 0)
        return 0;

    pid_t kid = fork();
    if (kid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = restart_handler;
        sa.sa_flags   = use_restart ? SA_RESTART : 0;
        sigaction(SIGUSR1, &sa, NULL);

        g_restart_pid = (pid_t)syscall(SYS_gettid);   /* our real pid */

        /* A grandchild signals us after a delay, then writes the byte the
         * restarted read is waiting for.  It must inherit the open write end,
         * so the restart child keeps fds[1] until after the fork below. */
        pid_t sig = fork();
        if (sig == 0) {
            msleep(300);                 /* let the restart child sit in read() */
            kill(g_restart_pid, SIGUSR1);
            /* Yield for real (not a busy wait) so the restart child is
             * actually scheduled and its read() is interrupted by the signal
             * before the byte is written -- otherwise the read just returns
             * the data and the signal is delivered afterwards. */
            for (int i = 0; i < 200; i++)
                syscall(SYS_sched_yield);
            char c = 'x';
            write(fds[1], &c, 1);
            _exit(0);
        }
        close(fds[1]);                   /* keep the write end for the grandchild */

        restart_hit = 0;
        char c = 0;
        errno = 0;
        ssize_t n = read(fds[0], &c, 1);

        int fail = 0;
        if (!restart_hit)
            fail = 1;
        if (use_restart) {
            if (n != 1 || c != 'x')
                fail = 1;
        } else {
            if (n != -1 || errno != EINTR)
                fail = 1;
        }
        _exit(fail ? 1 : 0);
    }

    close(fds[0]);
    close(fds[1]);
    int st = 0;
    waitpid(kid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void test_restart(void)
{
    ok("SA_RESTART: interrupted read completes",   do_restart_test(1));
    ok("no SA_RESTART: read returns EINTR",        do_restart_test(0));
}

int main(void)
{
    unblock_all();

    test_siginfo();
    test_samask();
    test_nodedefer();
    test_twice();
    test_pending();
    test_uncatchable();
    test_context();
    test_longjmp();
    test_resethand();
    test_restart();

    printf("sigtest: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
