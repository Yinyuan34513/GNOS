/*
 * thrtest.c — pthread bring-up self-test. (GPLv2, musl)
 *
 * The kernel grew real threads (CLONE_VM with a refcounted address space,
 * a thread-group id, group exit semantics) and a blocking futex.  This
 * program is the boot-time proof that musl's pthreads work on top of them:
 *
 *   1. mutexes + a shared counter across four threads (futex fast path);
 *   2. a condvar broadcast that must wake every waiter (futex requeue);
 *   3. a condvar timed wait that must time out (futex timeout);
 *   4. pthread_exit leaving the rest of the group running;
 *   5. one getpid() across threads, distinct gettid()s (tgid semantics);
 *   6. exit() killing a whole group, including threads parked in a syscall.
 *
 * Every check prints THRTEST: PASS n / FAIL n, and the exit status is the
 * first failing check or 0.  /etc/rc asserts on it, like the rest of the
 * boot-time tests.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

static int g_failed;

/* dbgputs(441): mirror a line to the debug console so the headless test
 * harness can see it in build/dbg.log -- the framebuffer console is not
 * captured there.  A GNOS extension. */
static void report(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    syscall(441, buf);                  /* SYS_dbgputs */
}

static void check(int ok, int n, const char *what)
{
    report("THRTEST: %s %d (%s)", ok ? "PASS" : "FAIL", n, what);
    if (!ok && !g_failed)
        g_failed = n;
}

/* ---- 1. mutex + shared counter ---------------------------------------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_count;

static void *counter_fn(void *arg)
{
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        pthread_mutex_lock(&g_lock);
        g_count++;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void test_mutex(void)
{
    pthread_t t[4];
    g_count = 0;
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, counter_fn, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    check(g_count == 40000, 1, "mutex counter");
}

/* ---- 2/3. condvar broadcast + timed wait ------------------------------- */
static pthread_mutex_t g_cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int g_cv_ready, g_cv_woke;

static void *cv_waiter(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_cv_lock);
    g_cv_ready++;
    while (!g_cv_woke)
        pthread_cond_wait(&g_cv, &g_cv_lock);
    pthread_mutex_unlock(&g_cv_lock);
    return NULL;
}

static void test_cond(void)
{
    pthread_t t[8];
    g_cv_ready = g_cv_woke = 0;

    for (int i = 0; i < 8; i++)
        pthread_create(&t[i], NULL, cv_waiter, NULL);

    /* Wait for every waiter to be parked on the condvar. */
    for (;;) {
        pthread_mutex_lock(&g_cv_lock);
        int r = g_cv_ready;
        pthread_mutex_unlock(&g_cv_lock);
        if (r == 8)
            break;
        sched_yield();
    }

    pthread_mutex_lock(&g_cv_lock);
    g_cv_woke = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_cv_lock);

    for (int i = 0; i < 8; i++)
        pthread_join(t[i], NULL);
    check(1, 2, "condvar broadcast");

    /* A timed wait with a deadline in the past must return ETIMEDOUT. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 0;                     /* deadline: now */
    pthread_mutex_lock(&g_cv_lock);
    int rc = pthread_cond_timedwait(&g_cv, &g_cv_lock, &ts);
    pthread_mutex_unlock(&g_cv_lock);
    check(rc == ETIMEDOUT, 3, "condvar timeout");
}

/* ---- 4. pthread_exit leaves the group alive ---------------------------- */
static void *just_exit_fn(void *arg)
{
    (void)arg;
    pthread_exit((void *)0x2a);
    return NULL;                        /* not reached */
}

static void test_pexit(void)
{
    pthread_t t;
    void *ret = NULL;
    pthread_create(&t, NULL, just_exit_fn, NULL);
    pthread_join(t, &ret);
    check(ret == (void *)0x2a, 4, "pthread_exit/join");
}

/* ---- 5. tgid vs tid ---------------------------------------------------- */
static pid_t g_main_pid;
static int   g_tids[4];

static void *tid_fn(void *arg)
{
    long i = (long)arg;
    g_tids[i] = (int)syscall(SYS_gettid);
    check(getpid() == g_main_pid, 5, "thread getpid == tgid");
    return NULL;
}

static void test_tgid(void)
{
    pthread_t t[4];
    g_main_pid = getpid();
    for (long i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, tid_fn, (void *)i);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    int distinct = 1;
    for (int i = 0; i < 4 && distinct; i++)
        for (int j = i + 1; j < 4; j++)
            if (g_tids[i] == g_tids[j])
                distinct = 0;
    check(distinct, 5, "threads have distinct tids");
}

/* ---- 6. exit() kills the whole group ------------------------------------ */
static void *sleeper_fn(void *arg)
{
    (void)arg;
    for (;;)
        sleep(1000);                    /* parked in nanosleep when exit lands */
    return NULL;
}

static void test_exit_group(void)
{
    pid_t c = fork();
    if (c == 0) {
        pthread_t t[2];
        pthread_create(&t[0], NULL, sleeper_fn, NULL);
        pthread_create(&t[1], NULL, sleeper_fn, NULL);
        exit(7);                        /* must take both sleepers down too */
    }
    int st = 0;
    pid_t w = waitpid(c, &st, 0);
    check(w == c && WIFEXITED(st) && WEXITSTATUS(st) == 7, 6,
          "exit() kills the whole group");
}

int main(void)
{
    report("THRTEST: starting");
    test_mutex();
    test_cond();
    test_pexit();
    test_tgid();
    test_exit_group();
    report("THRTEST: done (%s)", g_failed ? "FAILED" : "ALL PASS");
    return g_failed;
}
